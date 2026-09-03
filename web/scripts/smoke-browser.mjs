import { spawn } from "node:child_process";
import { access, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

const root = path.resolve(import.meta.dirname, "..");
const customJarPath = process.argv[2];
const extraJarPaths = process.argv.slice(3).map((value) => path.resolve(value));
const reinstallJarPath = process.env.PHONEME_SMOKE_REINSTALL_JAR?.trim()
  ? path.resolve(process.env.PHONEME_SMOKE_REINSTALL_JAR.trim())
  : "";
const websocketProxyUrl = process.env.PHONEME_WEBSOCKET_PROXY_URL?.trim() || "";
const holdMilliseconds = Math.max(0, Number.parseInt(process.env.PHONEME_SMOKE_HOLD_MS ?? "0", 10) || 0);
const skipReload = process.env.PHONEME_SMOKE_SKIP_RELOAD === "1";
const skipPlayerMenu = process.env.PHONEME_SMOKE_SKIP_PLAYER_MENU === "1";
const showFps = process.env.PHONEME_SMOKE_SHOW_FPS === "1";
const testOffline = process.env.PHONEME_SMOKE_OFFLINE === "1";
const overrideMainClass = process.env.PHONEME_SMOKE_MAIN_CLASS?.trim() || "";
const keySequence = (process.env.PHONEME_SMOKE_KEYS ?? "")
  .split(",")
  .map((key) => key.trim())
  .filter(Boolean);
const keyDelayMilliseconds = Math.max(0, Number.parseInt(process.env.PHONEME_SMOKE_KEY_DELAY_MS ?? "0", 10) || 0);
const screenshotPath = process.env.PHONEME_SMOKE_SCREENSHOT?.trim() || "";
const forceWebGl = process.env.PHONEME_SMOKE_WEBGL === "1";
const enableGpu = process.env.PHONEME_SMOKE_GPU === "1" || forceWebGl;
const jarPath = path.resolve(
  customJarPath ??
    path.join(
      root,
      "../Core/Compatibility/fixtures/generated/compatibility-fixtures.jar"
    )
);
const chromePath = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const port = 4175;
const debuggingPort = Number.parseInt(process.env.PHONEME_SMOKE_DEBUG_PORT ?? "", 10) || (19000 + (process.pid % 10000));
const externalBaseUrl = process.env.PHONEME_SMOKE_BASE_URL?.trim() || "";
const baseUrl = externalBaseUrl || `http://127.0.0.1:${port}`;

await access(jarPath);
for (const extraJarPath of extraJarPaths) await access(extraJarPath);
if (reinstallJarPath) await access(reinstallJarPath);
await access(chromePath);

const preview = externalBaseUrl ? null : spawn(
  "npm",
  ["run", "preview", "--", "--host", "127.0.0.1", "--port", String(port)],
  { cwd: root, stdio: ["ignore", "pipe", "pipe"] }
);
const chrome = spawn(
  chromePath,
  [
    "--headless=new",
    ...(enableGpu ? [] : ["--disable-gpu"]),
    ...(forceWebGl ? ["--disable-features=WebGPU"] : []),
    "--no-first-run",
    "--no-default-browser-check",
    `--remote-debugging-port=${debuggingPort}`,
    `--user-data-dir=/tmp/phoneme-web-smoke-${process.pid}`,
    baseUrl
  ],
  { stdio: ["ignore", "pipe", "pipe"] }
);

let browserErrors = "";
chrome.stderr.on("data", (chunk) => {
  browserErrors += String(chunk);
});

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function waitForJson(url, timeoutMs = 15_000) {
  const deadline = Date.now() + timeoutMs;
  let lastError;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      if (response.ok) return await response.json();
    } catch (error) {
      lastError = error;
    }
    await sleep(100);
  }
  throw lastError ?? new Error(`Timed out waiting for ${url}`);
}

console.log("[smoke] waiting for Chrome");
const pages = await waitForJson(`http://127.0.0.1:${debuggingPort}/json`);
const page = pages.find((entry) => entry.type === "page");
if (!page?.webSocketDebuggerUrl) throw new Error("Chrome page target not found");

const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  socket.addEventListener("open", resolve, { once: true });
  socket.addEventListener("error", reject, { once: true });
});

let commandId = 0;
const pending = new Map();
const exceptions = [];
const networkEvents = [];
socket.addEventListener("message", (event) => {
  const message = JSON.parse(String(event.data));
  if (message.id) {
    const callback = pending.get(message.id);
    if (callback) {
      pending.delete(message.id);
      if (message.error) callback.reject(new Error(message.error.message));
      else callback.resolve(message.result);
    }
    return;
  }
  if (message.method === "Runtime.exceptionThrown") {
    exceptions.push(message.params.exceptionDetails);
  } else if (message.method === "Network.requestWillBeSent") {
    const request = message.params?.request;
    if (request?.url?.includes('/api/http')) {
      networkEvents.push({ kind: 'request', method: request.method, url: request.url });
    }
  } else if (message.method === "Network.responseReceived") {
    const response = message.params?.response;
    if (response?.url?.includes('/api/http')) {
      networkEvents.push({ kind: 'response', status: response.status, url: response.url });
    }
  }
});

function command(method, params = {}) {
  const id = ++commandId;
  socket.send(JSON.stringify({ id, method, params }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
}

async function evaluate(expression) {
  const result = await command("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true
  });
  if (result.exceptionDetails) {
    throw new Error(result.exceptionDetails.text || "Runtime.evaluate failed");
  }
  return result.result?.value;
}

async function waitForExpression(expression, timeoutMs = 20_000) {
  const deadline = Date.now() + timeoutMs;
  let lastValue;
  while (Date.now() < deadline) {
    lastValue = await evaluate(expression);
    if (lastValue) return lastValue;
    await sleep(100);
  }
  throw new Error(`Timed out waiting for expression: ${expression}\nLast value: ${String(lastValue)}`);
}

let exitCode = 0;
try {
  console.log("[smoke] connected to DevTools");
  await command("Runtime.enable");
  await command("DOM.enable");
  await command("Page.enable");
  await command("Network.enable");

  if (websocketProxyUrl) {
    console.log("[smoke] configuring WebSocket proxy", websocketProxyUrl);
    await evaluate(`(() => {
      const key = 'phoneme-web.settings.v2';
      let settings = {};
      try { settings = JSON.parse(localStorage.getItem(key) || '{}'); } catch {}
      settings.websocketProxyUrl = ${JSON.stringify(websocketProxyUrl)};
      localStorage.setItem(key, JSON.stringify(settings));
      return true;
    })()`);
    await command("Page.reload", { ignoreCache: false });
  }

  console.log("[smoke] waiting for runtime");
  await waitForExpression("document.querySelector('.app-root')?.getAttribute('data-runtime-phase') === 'ready'");
  const manifestResponse = await fetch(new URL("/wasm/manifest.json", baseUrl), { cache: "no-store" });
  if (!manifestResponse.ok) throw new Error(`WASM manifest HTTP ${manifestResponse.status}`);
  const wasmManifest = await manifestResponse.json();
  if (!String(wasmManifest.module ?? "").startsWith("build-") ||
      !String(wasmManifest.wasm ?? "").startsWith("build-") ||
      !String(wasmManifest.compatModule ?? "").startsWith("build-") ||
      !String(wasmManifest.compatWasm ?? "").startsWith("build-")) {
    throw new Error(`Invalid dual-build WASM manifest: ${JSON.stringify(wasmManifest)}`);
  }
  console.log("[smoke] runtime ready", wasmManifest.version);
  const documentNode = await command("DOM.getDocument", { depth: -1, pierce: true });
  const inputNode = await command("DOM.querySelector", {
    nodeId: documentNode.root.nodeId,
    selector: "input[type=file]"
  });
  if (!inputNode.nodeId) throw new Error("JAR file input not found");
  console.log("[smoke] selecting JAR");
  await command("DOM.setFileInputFiles", {
    files: [jarPath, ...extraJarPaths],
    nodeId: inputNode.nodeId
  });

  console.log("[smoke] waiting for installation");
  await waitForExpression(`document.querySelectorAll('.game-row').length >= ${1 + extraJarPaths.length}`, 60_000);
  console.log("[smoke] installed");
  const installedTitle = await evaluate(`(() => {
    const row = document.querySelector('.game-row');
    const titled = row?.querySelector('.game-row-title');
    return titled?.getAttribute('title') || titled?.textContent?.trim() || '';
  })()`);
  if (!installedTitle) throw new Error("Installed MIDlet title not found");
  if (reinstallJarPath) {
    console.log("[smoke] reinstalling changed same-version JAR");
    const beforeReplacement = await evaluate(`(() => {
      const games = JSON.parse(localStorage.getItem('phoneme-web.games.v1') || '[]');
      return games[0] || null;
    })()`);
    if (!beforeReplacement?.suiteId || !beforeReplacement?.id) {
      throw new Error(`Installed game record missing before replacement: ${JSON.stringify(beforeReplacement)}`);
    }
    await sleep(10);
    await command("DOM.setFileInputFiles", {
      files: [reinstallJarPath],
      nodeId: inputNode.nodeId
    });
    const beforeSuiteId = Number(beforeReplacement.suiteId);
    const beforeInstalledAt = Number(beforeReplacement.installedAt || 0);
    const beforeIdLiteral = JSON.stringify(String(beforeReplacement.id));
    const replacement = await waitForExpression(`(() => {
      const games = JSON.parse(localStorage.getItem('phoneme-web.games.v1') || '[]');
      if (games.length !== 1) return null;
      const game = games[0];
      if (game.suiteId !== ${beforeSuiteId} || game.id !== ${beforeIdLiteral} ||
          Number(game.installedAt || 0) <= ${beforeInstalledAt}) return null;
      return game;
    })()`, 60_000);
    console.log("[smoke] replacement kept suite/UI identity", replacement.suiteId, replacement.id);
  }
  if (overrideMainClass) {
    console.log("[smoke] overriding MIDlet main class", overrideMainClass);
    await evaluate(`(() => {
      const key = 'phoneme-web.games.v1';
      const games = JSON.parse(localStorage.getItem(key) || '[]');
      if (!games.length) return false;
      games[0].mainClass = ${JSON.stringify(overrideMainClass)};
      localStorage.setItem(key, JSON.stringify(games));
      return true;
    })()`);
    await command("Page.reload", { ignoreCache: false });
    await waitForExpression("document.querySelector('.app-root')?.getAttribute('data-runtime-phase') === 'ready' && Boolean(document.querySelector('.game-row-title'))", 30_000);
  }
  await evaluate(`(() => {
    const row = document.querySelector('.game-row');
    if (!row) return false;
    row.click();
    return true;
  })()`);
  await waitForExpression("[...document.querySelectorAll('button')].some((element) => element.textContent?.trim() === 'Bắt đầu')");
  if (showFps) {
    console.log("[smoke] enabling FPS overlay");
    const enabled = await evaluate(`(() => {
      const label = [...document.querySelectorAll('label')].find((element) =>
        (element.textContent || '').includes('Hiện FPS'));
      const input = label?.querySelector('input[type="checkbox"]');
      if (!input) return false;
      if (!input.checked) input.click();
      return input.checked;
    })()`);
    if (!enabled) throw new Error("FPS profile switch not found");
  }
  await evaluate(`(() => {
    const button = [...document.querySelectorAll('button')].find((element) => element.textContent?.trim() === 'Bắt đầu');
    if (!button) return false;
    button.click();
    return true;
  })()`);

  console.log("[smoke] launching MIDlet");
  const expectedTitleLiteral = JSON.stringify(installedTitle);
  const strictFixture = !customJarPath && !overrideMainClass;
  const result = await waitForExpression(`(() => {
    const body = document.body?.innerText || '';
    const error = body.includes('Không thể chạy ứng dụng');
    const canvas = document.querySelector('canvas.emulator-canvas');
    const lcdui = document.querySelector('.native-lcdui');
    const running = body.includes('Đang chạy ' + ${expectedTitleLiteral});
    if (error) return { status: 'error', body };
    if (canvas && running && canvas.width === 240 && canvas.height === 320) {
      const surface = document.querySelector('.emulator-surface');
      const keypad = document.querySelector('.virtual-keypad-overlay');
      const appbar = document.querySelector('.emulator-appbar');
      const canvasRect = canvas.getBoundingClientRect();
      const surfaceRect = surface?.getBoundingClientRect();
      return {
        status: 'canvas',
        width: canvas.width,
        height: canvas.height,
        topAligned: Boolean(surfaceRect) && Math.abs(canvasRect.top - surfaceRect.top) < 2,
        keypad: Boolean(keypad),
        appbar: Boolean(appbar),
        body
      };
    }
    if (${strictFixture ? "false" : "true"} && lcdui && running) {
      return { status: 'lcdui', body };
    }
    return null;
  })()`, 120_000);

  if (result.status === "error") {
    throw new Error(`MIDlet launch failed:\n${result.body}`);
  }
  if (result.status === "canvas" && (!result.topAligned || !result.keypad || !result.appbar)) {
    throw new Error(`Player chrome/layout mismatch: ${JSON.stringify(result, null, 2)}`);
  }
  if (exceptions.length) {
    throw new Error(`Browser exceptions: ${JSON.stringify(exceptions, null, 2)}`);
  }

  if (!skipPlayerMenu) {
    console.log("[smoke] checking Swift-style player menu");
    await evaluate(`(() => {
      const button = document.querySelector('button[aria-label="Thêm"]');
      if (!button) return false;
      button.click();
      return true;
    })()`);
    await waitForExpression(`(() => {
      const body = document.body?.innerText || '';
      return body.includes('Bàn phím ảo') &&
        body.includes('Khóa xoay màn hình') &&
        body.includes('Tự động dịch') &&
        body.includes('Thoát');
    })()`);
    const playerMenuText = await evaluate(`(() => {
      const menu = document.querySelector('.player-menu .MuiMenu-list');
      return menu?.innerText || '';
    })()`);
    if (/Tạm dừng|Khởi động lại|Toàn màn hình/.test(playerMenuText)) {
      throw new Error(`Player menu contains non-Swift actions: ${playerMenuText}`);
    }
    await evaluate(`(() => {
      const items = [...document.querySelectorAll('.player-menu .MuiMenuItem-root')];
      const translation = items.find((item) => item.innerText?.includes('Tự động dịch'));
      if (!translation) return false;
      translation.click();
      return true;
    })()`);
    await waitForExpression(`(() => {
      const menus = [...document.querySelectorAll('.player-submenu .MuiMenu-list')];
      const text = menus.map((menu) => menu.innerText || '').join(' ');
      return text.includes('Từ:') && text.includes('Sang:');
    })()`);
    await evaluate(`document.body.click()`);
  }

  const frameHashes = [];
  const captureFrameHash = async (label) => {
    const value = await evaluate(`(() => {
      const canvas = document.querySelector('canvas.emulator-canvas');
      if (!canvas) return null;
      try {
        const context = canvas.getContext('2d');
        if (!context) return null;
        const bytes = context.getImageData(0, 0, canvas.width, canvas.height).data;
        let hash = 2166136261 >>> 0;
        for (let index = 0; index < bytes.length; index += 97) {
          hash ^= bytes[index];
          hash = Math.imul(hash, 16777619) >>> 0;
        }
        return hash.toString(16).padStart(8, '0');
      } catch {
        // A worker-owned OffscreenCanvas intentionally has no main-thread 2D
        // context. Frame hashing is optional for those optimized render paths.
        return null;
      }
    })()`);
    frameHashes.push({ label, value });
  };

  if (keySequence.length) {
    if (keyDelayMilliseconds > 0) {
      console.log(`[smoke] waiting ${keyDelayMilliseconds} ms before keys`);
      await sleep(keyDelayMilliseconds);
    }
    await captureFrameHash("before-keys");
    console.log("[smoke] sending keys", keySequence.join(", "));
    for (const key of keySequence) {
      await command("Input.dispatchKeyEvent", { type: "keyDown", key });
      await sleep(80);
      await command("Input.dispatchKeyEvent", { type: "keyUp", key });
      await sleep(250);
      await captureFrameHash(key);
    }
  }

  if (holdMilliseconds > 0) {
    console.log(`[smoke] holding active MIDlet for ${holdMilliseconds} ms`);
    await sleep(holdMilliseconds);
  }

  const measuredFps = showFps
    ? await evaluate(`(() => {
        const text = document.querySelector('.fps-overlay')?.textContent || '';
        const match = text.match(/([0-9]+(?:\\.[0-9]+)?)/);
        return match ? Number(match[1]) : null;
      })()`)
    : null;
  if (showFps) console.log("[smoke] measured FPS", measuredFps);

  console.log("[smoke] exiting and relaunching MIDlet");
  await evaluate(`(() => {
    const button = document.querySelector('button[aria-label="Thêm"]');
    if (!button) return false;
    button.click();
    return true;
  })()`);
  await waitForExpression(`(() => {
    const menu = document.querySelector('.player-menu .MuiMenu-list');
    return Boolean(menu) && (menu.innerText || '').includes('Thoát');
  })()`);
  await evaluate(`(() => {
    const item = [...document.querySelectorAll('.player-menu .MuiMenuItem-root')]
      .find((element) => element.textContent?.trim() === 'Thoát');
    if (!item) return false;
    item.click();
    return true;
  })()`);
  await waitForExpression(`[...document.querySelectorAll('.MuiDialog-root button')].some((element) => element.textContent?.trim() === 'Thoát')`);
  await evaluate(`(() => {
    const button = [...document.querySelectorAll('.MuiDialog-root button')]
      .find((element) => element.textContent?.trim() === 'Thoát');
    if (!button) return false;
    button.click();
    return true;
  })()`);
  await waitForExpression(`Boolean(document.querySelector('.game-row')) && !document.querySelector('canvas.emulator-canvas')`);
  await evaluate(`(() => {
    const row = document.querySelector('.game-row');
    if (!row) return false;
    row.click();
    return true;
  })()`);
  await waitForExpression(`(() => {
    const body = document.body?.innerText || '';
    const canvas = document.querySelector('canvas.emulator-canvas');
    return Boolean(canvas) && body.includes('Đang chạy ' + ${expectedTitleLiteral});
  })()`, 30_000);
  if (exceptions.length) {
    throw new Error(`Browser exceptions after exit/relaunch: ${JSON.stringify(exceptions, null, 2)}`);
  }
  console.log("[smoke] runtime recovered after exit/relaunch");
  await sleep(1_250);

  if (screenshotPath) {
    const dataUrl = await evaluate(`document.querySelector('canvas.emulator-canvas')?.toDataURL('image/png') || ''`);
    if (dataUrl) {
      await writeFile(screenshotPath, Buffer.from(dataUrl.split(',', 2)[1], 'base64'));
      console.log('[smoke] wrote screenshot', screenshotPath);
    }
  }

  const liveState = await evaluate(`(() => ({
    phase: document.querySelector('.app-root')?.getAttribute('data-runtime-phase') || '',
    memory: Number(document.querySelector('.app-root')?.getAttribute('data-runtime-memory') || 0),
    live: document.querySelector('.sr-only')?.textContent || '',
    error: document.querySelector('.emulator-error')?.textContent || ''
  }))()`);
  let reload = "skipped";
  if (!skipReload) {
    console.log("[smoke] reloading while MIDlet is active");
    await command("Page.reload", { ignoreCache: false });
    await waitForExpression(`(() => {
      return document.querySelector('.app-root')?.getAttribute('data-runtime-phase') === 'ready' &&
        Boolean(document.querySelector('.game-row'));
    })()`, 30_000);
    if (exceptions.length) {
      throw new Error(`Browser exceptions after reload: ${JSON.stringify(exceptions, null, 2)}`);
    }
    console.log("[smoke] runtime recovered after reload");
    reload = "ready";
  }

  let offline = "skipped";
  if (testOffline) {
    console.log("[smoke] checking service worker before offline reload");
    const serviceWorkerState = await waitForExpression(`(() => {
      const controller = navigator.serviceWorker?.controller;
      return controller ? { state: controller.state, scriptURL: controller.scriptURL } : null;
    })()`, 30_000);
    console.log("[smoke] service worker", JSON.stringify(serviceWorkerState));
    await command("Network.emulateNetworkConditions", {
      offline: true,
      latency: 0,
      downloadThroughput: 0,
      uploadThroughput: 0,
      connectionType: "none"
    });
    console.log("[smoke] reloading fully offline");
    await command("Page.reload", { ignoreCache: false });
    await waitForExpression(`(() => {
      return document.querySelector('.app-root')?.getAttribute('data-runtime-phase') === 'ready' &&
        Boolean(document.querySelector('.game-row'));
    })()`, 30_000);
    if (exceptions.length) {
      throw new Error(`Browser exceptions after offline reload: ${JSON.stringify(exceptions, null, 2)}`);
    }
    offline = "ready";
    console.log("[smoke] runtime recovered fully offline");
    await command("Network.emulateNetworkConditions", {
      offline: false,
      latency: 0,
      downloadThroughput: -1,
      uploadThroughput: -1,
      connectionType: "wifi"
    });
  }

  console.log(JSON.stringify({ ok: true, jarPath, result, liveState, measuredFps, reload, offline, networkEvents, frameHashes }, null, 2));
} catch (error) {
  exitCode = 1;
  console.error(error instanceof Error ? error.stack : String(error));
  try {
    const diagnostic = await evaluate(`(() => ({
      body: document.body?.innerText || '',
      canvas: (() => {
        const element = document.querySelector('canvas.emulator-canvas');
        return element ? { width: element.width, height: element.height } : null;
      })(),
      live: document.querySelector('.sr-only')?.textContent || ''
    }))()`);
    console.error("[smoke] diagnostic", JSON.stringify(diagnostic, null, 2));
  } catch (diagnosticError) {
    console.error("[smoke] diagnostic failed", String(diagnosticError));
  }
} finally {
  socket.close();
  chrome.kill("SIGTERM");
  preview?.kill("SIGTERM");
  await sleep(200);
  if (/TypeError|ReferenceError|RuntimeError/.test(browserErrors)) {
    console.error(browserErrors);
  }
  process.exit(exitCode);
}

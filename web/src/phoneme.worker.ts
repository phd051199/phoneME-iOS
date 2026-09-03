/// <reference lib="webworker" />

import { PhoneMEWebRuntime } from "./phoneME";
import { createFramePresenter, type FramePresenter } from "./framePresenter";
import type { GameEntry, JarMetadata, ManagedStorageKind } from "./types";
import { installWorkerMediaBridge, type MediaStatusMessage } from "./workerMediaBridge";

const scope = self as unknown as DedicatedWorkerGlobalScope;
const mediaBridge = installWorkerMediaBridge(scope);
const runtime = new PhoneMEWebRuntime();
// The browser presentation path is already throttled by the GPU/browser when
// needed. 30 FPS here made fast games feel like they were running in slow
// motion even when the VM had enough headroom. Keep the old power saving path
// only for background mode.
const WORKER_MAX_FPS = 60;
const WORKER_FRAME_INTERVAL_MS = 1000 / WORKER_MAX_FPS;
const BACKGROUND_PUMP_INTERVAL_MS = 250;
let recyclableFramePixels: Uint8ClampedArray<ArrayBuffer> | null = null;
let framePresenter: FramePresenter | null = null;
let renderLoopTimer = 0;
let renderLoopRunning = false;
let renderLoopForeground = true;
let renderLoopGeneration = 0n;
let renderLoopFrameCount = 0;
let renderLoopLastReportAt = performance.now();
let renderLoopWidth = 0;
let renderLoopHeight = 0;

type RequestMessage = {
  id: number;
  type: string;
  payload?: any;
};

type ResponseMessage = {
  id: number;
  ok: boolean;
  result?: unknown;
  error?: string;
  fatal?: boolean;
};

function postResponse(message: ResponseMessage, transfer: Transferable[] = []) {
  scope.postMessage(message, transfer);
}

function formatError(error: unknown) {
  if (error instanceof Error) return error.stack || error.message;
  if (error && typeof error === "object") {
    const candidate = error as { name?: unknown; message?: unknown; stack?: unknown };
    if (typeof candidate.stack === "string" && candidate.stack) return candidate.stack;
    if (typeof candidate.message === "string" && candidate.message) {
      return typeof candidate.name === "string" && candidate.name
        ? `${candidate.name}: ${candidate.message}`
        : candidate.message;
    }
    try {
      return JSON.stringify(error);
    } catch {
      // Fall through to String for non-serializable foreign errors.
    }
  }
  return String(error);
}

function postLog(line: string, isError: boolean) {
  scope.postMessage({ event: "log", line, isError });
}

function stopRenderLoop() {
  renderLoopRunning = false;
  if (renderLoopTimer) clearTimeout(renderLoopTimer);
  renderLoopTimer = 0;
  renderLoopGeneration = 0n;
  renderLoopFrameCount = 0;
  renderLoopWidth = 0;
  renderLoopHeight = 0;
}

function scheduleRenderLoop(delayMilliseconds: number) {
  if (!renderLoopRunning) return;
  if (renderLoopTimer) clearTimeout(renderLoopTimer);
  renderLoopTimer = setTimeout(
    runRenderLoop,
    Math.max(0, delayMilliseconds)
  ) as unknown as number;
}

function runRenderLoop() {
  renderLoopTimer = 0;
  if (!renderLoopRunning || !framePresenter || !runtime.activeGame) return;
  const startedAt = performance.now();

  try {
    let presentedFrame: {
      width: number;
      height: number;
      generation: bigint;
      backend: FramePresenter["backend"];
    } | null = null;

    if (renderLoopForeground) {
      runtime.withFrameView(renderLoopGeneration, (frame) => {
        // The typed-array view points directly at the core's published RGBA
        // buffer. withFrameView holds the framebuffer read lease only across
        // this synchronous GPU upload and releases it immediately afterwards.
        framePresenter!.present(frame);
        renderLoopGeneration = frame.generation;
        renderLoopFrameCount += 1;
        if (frame.width !== renderLoopWidth || frame.height !== renderLoopHeight) {
          renderLoopWidth = frame.width;
          renderLoopHeight = frame.height;
          presentedFrame = {
            width: frame.width,
            height: frame.height,
            generation: frame.generation,
            backend: framePresenter!.backend
          };
        }
      });
    } else {
      // Keep low-frequency storage/LCDUI maintenance alive while presentation
      // is suspended. Runtime::suspend disables Canvas work but Java timers,
      // sockets and workers intentionally continue for online MIDlets.
      runtime.pump();
    }

    const events = runtime.pollLcduiEvents();
    const now = performance.now();
    const shouldReportFrames = now - renderLoopLastReportAt >= 1000;
    const renderedFrames = shouldReportFrames ? renderLoopFrameCount : 0;
    if (shouldReportFrames) {
      renderLoopFrameCount = 0;
      renderLoopLastReportAt = now;
    }
    if (events.length || presentedFrame || renderedFrames > 0) {
      scope.postMessage({
        event: "runtimeTick",
        events,
        presentedFrame,
        renderedFrames
      });
    }
  } catch (error) {
    const formatted = formatError(runtime.fatalError ?? error);
    postLog(formatted, true);
    if (runtime.fatalError) {
      scope.postMessage({ event: "fatal", error: formatted });
    } else {
      scope.postMessage({
        event: "runtimeTick",
        events: [],
        presentedFrame: null,
        renderedFrames: 0,
        error: formatted
      });
    }
    stopRenderLoop();
    return;
  }

  const elapsed = performance.now() - startedAt;
  const interval = renderLoopForeground
    ? WORKER_FRAME_INTERVAL_MS
    : BACKGROUND_PUMP_INTERVAL_MS;
  scheduleRenderLoop(Math.max(0, interval - elapsed));
}

function startRenderLoop() {
  stopRenderLoop();
  if (!framePresenter || !runtime.activeGame) return;
  renderLoopRunning = true;
  renderLoopLastReportAt = performance.now();
  scheduleRenderLoop(0);
}

function frameTransfer(frame: ReturnType<PhoneMEWebRuntime["copyFrame"]>) {
  return frame ? [frame.pixels.buffer as ArrayBuffer] : [];
}

function resultTransfer(result: unknown): Transferable[] {
  if (!result) return [];
  if (result instanceof Uint8Array || result instanceof Uint8ClampedArray) {
    return [result.buffer as ArrayBuffer];
  }
  if (typeof result !== "object") return [];
  if ("frame" in result) {
    return frameTransfer((result as { frame: ReturnType<PhoneMEWebRuntime["copyFrame"]> }).frame);
  }
  if ("pixels" in result) {
    return [(result as { pixels: Uint8ClampedArray }).pixels.buffer as ArrayBuffer];
  }
  if ("files" in result) {
    const files = (result as { files?: Array<{ data?: Uint8Array }> }).files ?? [];
    return files
      .map((file) => file.data?.buffer)
      .filter((buffer): buffer is ArrayBuffer => buffer instanceof ArrayBuffer);
  }
  return [];
}

async function handleRequest(message: RequestMessage) {
  const payload = message.payload ?? {};
  switch (message.type) {
  case "initialize":
    await runtime.initialize({
      websocketProxyUrl: payload.websocketProxyUrl,
      onLog: postLog
    });
    return undefined;
  case "installJar":
    return await runtime.installJar(
      payload.file as File,
      payload.metadata as JarMetadata
    );
  case "uninstall":
    await runtime.uninstall(payload.game as GameEntry, Boolean(payload.removeData));
    return undefined;
  case "launch":
    await runtime.launch(
      payload.game as GameEntry,
      Number(payload.width),
      Number(payload.height)
    );
    return undefined;
  case "resize":
    runtime.resize(Number(payload.width), Number(payload.height));
    return undefined;
  case "configureHeap":
    runtime.configureHeap(Number(payload.heapMegabytes));
    return undefined;
  case "configureFrameRate":
    runtime.configureFrameRate();
    return undefined;
  case "configureFrameRateOverride":
    runtime.configureFrameRateOverride(
      Boolean(payload.enabled),
      Number(payload.framesPerSecond)
    );
    return undefined;
  case "configureTranslation":
    runtime.configureTranslation(
      Boolean(payload.enabled),
      payload.provider as "google" | "bing" | "automatic",
      String(payload.sourceLanguage ?? "auto"),
      String(payload.targetLanguage ?? "vi")
    );
    return undefined;
  case "pause":
    runtime.pause();
    return undefined;
  case "resume":
    runtime.resume();
    return undefined;
  case "stopMidlet":
    runtime.stopMidlet();
    return undefined;
  case "attachCanvas": {
    const canvas = payload.canvas as OffscreenCanvas | undefined;
    if (!canvas) throw new Error("Không nhận được OffscreenCanvas cho renderer");
    stopRenderLoop();
    framePresenter?.dispose();
    framePresenter = await createFramePresenter(canvas, Boolean(payload.filtering));
    recyclableFramePixels = null;
    renderLoopForeground = Boolean(payload.foreground ?? true);
    runtime.setHostForeground(renderLoopForeground);
    startRenderLoop();
    return { backend: framePresenter.backend };
  }
  case "detachCanvas":
    stopRenderLoop();
    framePresenter?.dispose();
    framePresenter = null;
    return undefined;
  case "setPresenterFiltering":
    framePresenter?.setFiltering(Boolean(payload.enabled));
    return undefined;
  case "setPresenterForeground":
    renderLoopForeground = Boolean(payload.foreground);
    runtime.setHostForeground(renderLoopForeground);
    if (renderLoopRunning) scheduleRenderLoop(0);
    return undefined;
  case "capturePresenter": {
    if (!framePresenter) return null;
    try {
      const captured = await framePresenter.capture();
      if (captured) return captured;
    } catch {
      // Some GPU canvas implementations do not expose their swapchain through
      // convertToBlob(). Reconstruct only on explicit screenshot requests.
    }
    const frame = runtime.copyFrame(0n);
    if (!frame || typeof OffscreenCanvas === "undefined") return null;
    const screenshotCanvas = new OffscreenCanvas(frame.width, frame.height);
    const context = screenshotCanvas.getContext("2d", { alpha: false });
    if (!context) return null;
    context.putImageData(new ImageData(frame.pixels, frame.width, frame.height), 0, 0);
    return await screenshotCanvas.convertToBlob({ type: "image/png" });
  }
  case "tick": {
    const incomingPixels = payload.recyclablePixels;
    if (incomingPixels instanceof Uint8ClampedArray && incomingPixels.buffer.byteLength > 0) {
      recyclableFramePixels = incomingPixels as Uint8ClampedArray<ArrayBuffer>;
    }

    const includeFrame = Boolean(payload.includeFrame);
    const previousGeneration = BigInt(payload.previousGeneration ?? 0);

    if (!includeFrame) runtime.pump();
    const frame = includeFrame
      ? runtime.copyFrame(previousGeneration, recyclableFramePixels)
      : null;
    if (frame) recyclableFramePixels = null;
    return {
      events: runtime.pollLcduiEvents(),
      frame,
      presentedFrame: null,
      renderedFrames: 0
    };
  }
  case "copyLcduiImage":
    return runtime.copyLcduiImage(Number(payload.componentId));
  case "sendKey":
    runtime.sendKey(Number(payload.keyCode), Boolean(payload.pressed));
    return undefined;
  case "sendPointer":
    runtime.sendPointer(Number(payload.x), Number(payload.y), Number(payload.action));
    return undefined;
  case "selectCommand":
    runtime.selectCommand(Number(payload.commandId));
    return undefined;
  case "selectListItemCommand":
    runtime.selectListItemCommand(
      Number(payload.componentId),
      Number(payload.elementIndex),
      Number(payload.commandId)
    );
    return undefined;
  case "focusItem":
    runtime.focusItem(Number(payload.componentId));
    return undefined;
  case "activateItem":
    runtime.activateItem(Number(payload.componentId));
    return undefined;
  case "setText":
    runtime.setText(
      Number(payload.componentId),
      String(payload.value ?? ""),
      Number(payload.caret)
    );
    return undefined;
  case "setChoice":
    runtime.setChoice(
      Number(payload.componentId),
      Number(payload.index),
      Boolean(payload.selected)
    );
    return undefined;
  case "setGauge":
    runtime.setGauge(Number(payload.componentId), Number(payload.value));
    return undefined;
  case "setDate":
    runtime.setDate(Number(payload.componentId), Number(payload.unixSeconds));
    return undefined;
  case "setScrollPosition":
    runtime.setScrollPosition(Number(payload.position));
    return undefined;
  case "flushStorage":
    await runtime.flushStorage();
    return undefined;
  case "memoryStats":
    return runtime.memoryStats();
  case "listManagedStorage":
    return await runtime.listManagedStorage(
      payload.kind as ManagedStorageKind,
      String(payload.relativePath ?? "")
    );
  case "readManagedStorageFile":
    return await runtime.readManagedStorageFile(
      payload.kind as ManagedStorageKind,
      String(payload.relativePath ?? "")
    );
  case "exportManagedStorage":
    return await runtime.exportManagedStorage(
      payload.kind as ManagedStorageKind,
      String(payload.relativePath ?? "")
    );
  case "importManagedStorageFiles":
    await runtime.importManagedStorageFiles(
      payload.kind as ManagedStorageKind,
      String(payload.relativeDirectory ?? ""),
      payload.uploads as Array<{ relativePath: string; file: File }>
    );
    return undefined;
  case "createManagedStorageDirectory":
    await runtime.createManagedStorageDirectory(
      payload.kind as ManagedStorageKind,
      String(payload.relativePath ?? "")
    );
    return undefined;
  case "deleteManagedStorageEntry":
    await runtime.deleteManagedStorageEntry(
      payload.kind as ManagedStorageKind,
      String(payload.relativePath ?? "")
    );
    return undefined;
  case "shutdown":
    try {
      stopRenderLoop();
      runtime.stopMidlet();
      await runtime.flushStorage();
    } finally {
      framePresenter?.dispose();
      framePresenter = null;
      runtime.dispose();
      scope.close();
    }
    return undefined;
  case "dispose":
    stopRenderLoop();
    framePresenter?.dispose();
    framePresenter = null;
    runtime.dispose();
    scope.close();
    return undefined;
  default:
    throw new Error(`Lệnh Web Worker không hỗ trợ: ${message.type}`);
  }
}

scope.addEventListener("message", (event: MessageEvent<RequestMessage | MediaStatusMessage>) => {
  const incoming = event.data;
  if ("event" in incoming) {
    if (incoming.event === "mediaStatus") mediaBridge.applyStatus(incoming);
    return;
  }
  const message = incoming;
  void Promise.resolve()
    .then(() => handleRequest(message))
    .then((result) => {
      if (!message.id) return;
      postResponse({ id: message.id, ok: true, result }, resultTransfer(result));
    })
    .catch((error) => {
      const fatal = Boolean(runtime.fatalError);
      const formatted = formatError(runtime.fatalError ?? error);
      if (!message.id) {
        postLog(formatted, true);
        if (fatal) {
          scope.postMessage({ event: "fatal", error: formatted });
          scope.close();
        }
        return;
      }
      postResponse({
        id: message.id,
        ok: false,
        error: formatted,
        fatal
      });
      if (fatal) scope.close();
    });
});

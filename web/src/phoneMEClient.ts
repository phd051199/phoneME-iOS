import {
  PhoneMEWebRuntime as DirectPhoneMEWebRuntime,
  type FrameData,
  type FrameView,
  type PhoneMEOptions
} from "./phoneME";
import type { FramePresenterBackend } from "./framePresenter";
import type {
  GameEntry,
  JarMetadata,
  LcduiEvent,
  ManagedStorageEntry,
  ManagedStorageExport,
  ManagedStorageKind
} from "./types";
import { installWebMediaBridge } from "./webMediaBridge";

type WorkerRequest = {
  id: number;
  type: string;
  payload?: unknown;
};

type WorkerResponse = {
  id?: number;
  ok?: boolean;
  result?: unknown;
  error?: string;
  fatal?: boolean;
  event?: "log" | "media" | "fatal" | "runtimeTick";
  events?: LcduiEvent[];
  presentedFrame?: {
    width: number;
    height: number;
    generation: bigint;
    backend: FramePresenterBackend;
  } | null;
  renderedFrames?: number;
  line?: string;
  isError?: boolean;
  action?: string;
  handle?: number;
  data?: Uint8Array;
  locator?: string;
  contentType?: string;
  count?: number;
  level?: number;
  muted?: number | boolean;
  microseconds?: number;
  note?: number;
  durationMilliseconds?: number;
  volume?: number;
};

type PendingRequest = {
  resolve: (value: unknown) => void;
  reject: (reason?: unknown) => void;
  timeout: number;
};

export type RuntimeMemoryStats = {
  appEstimatedBytes: number;
  wasmLinearBytes: number;
  frameStagingBytes: number;
  mediaCompressedBytes: number;
  mediaDecodedBytes: number;
  mediaEntries: number;
  totalTrackedBytes: number;
};

export type RuntimeTick = {
  events: LcduiEvent[];
  frame: FrameData | FrameView | null;
  error?: string;
  presentedFrame?: {
    width: number;
    height: number;
    generation: bigint;
    backend: FramePresenterBackend;
  } | null;
  renderedFrames?: number;
};

function isIOS16WebKit() {
  const navigatorValue = globalThis.navigator;
  if (!navigatorValue) return false;

  const userAgent = navigatorValue.userAgent;
  const iOSDevice = /iP(?:hone|ad|od)/.test(userAgent) ||
    (navigatorValue.platform === "MacIntel" && navigatorValue.maxTouchPoints > 1);
  if (!iOSDevice) return false;

  const osVersion = userAgent.match(/(?:CPU (?:iPhone )?OS|OS) (\d+)[_.]/)?.[1];
  const safariVersion = userAgent.match(/Version\/(\d+)(?:\.|\s)/)?.[1];
  const majorVersion = Number(osVersion ?? safariVersion ?? 0);
  return majorVersion === 16;
}

export class PhoneMEWebRuntime {
  private worker: Worker | null = null;
  private directRuntime: DirectPhoneMEWebRuntime | null = null;
  private nextRequestId = 0;
  private pending = new Map<number, PendingRequest>();
  private initialized = false;
  private initializePromise: Promise<void> | null = null;
  private initializeOptions: PhoneMEOptions = {};
  private currentGameValue: GameEntry | null = null;
  private onLog?: PhoneMEOptions["onLog"];
  private mediaHandles = new Map<number, number>();
  private mediaStatusTimer: number | null = null;
  private recyclableFramePixels: Uint8ClampedArray<ArrayBuffer> | null = null;
  private workerPresenterAttached = false;
  private workerPresenterAttaching = false;
  private workerPresenterBackend: FramePresenterBackend | null = null;
  private workerTickEvents: LcduiEvent[] = [];
  private workerTickPresentedFrame: RuntimeTick["presentedFrame"] = null;
  private workerTickRenderedFrames = 0;
  private workerTickError = "";
  private runtimeTickListeners = new Set<(tick: RuntimeTick) => void>();

  get ready() {
    return this.initialized && (this.worker !== null || this.directRuntime?.ready === true);
  }

  get activeGame() {
    return this.directRuntime?.activeGame ?? this.currentGameValue;
  }

  get presentationRunsInWorker() {
    return this.workerPresenterAttached;
  }

  async initialize(options?: PhoneMEOptions) {
    if (options) this.initializeOptions = options;
    if (this.ready) return;
    if (this.initializePromise) return await this.initializePromise;

    this.initializePromise = this.initializeOnce(this.initializeOptions);
    try {
      await this.initializePromise;
    } finally {
      this.initializePromise = null;
    }
  }

  private async initializeOnce(options: PhoneMEOptions) {
    if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer === "undefined") {
      throw new Error("WebAssembly đa luồng cần COOP/COEP. Hãy chạy bằng Vite hoặc máy chủ có header cách ly chéo nguồn.");
    }

    if (this.worker || this.directRuntime) {
      this.invalidateRuntime(new Error("Đang khởi tạo lại phoneME Web runtime"));
    }
    this.onLog = options.onLog;

    // Safari iOS 16 cannot reliably host Emscripten's pthread pool inside an
    // additional module Worker. Keep the compatibility path there, but run the
    // core off the browser UI thread everywhere else so file/RMS/network or VM
    // work cannot freeze React and input handling.
    if (isIOS16WebKit()) {
      const runtime = new DirectPhoneMEWebRuntime();
      this.directRuntime = runtime;
      this.onLog?.("Safari iOS 16: bật chế độ WebAssembly tương thích.", false);
      try {
        await runtime.initialize(options);
        if (this.directRuntime !== runtime) {
          throw new Error("phoneME Web runtime đã bị thay thế khi đang khởi tạo");
        }
        this.initialized = true;
        return;
      } catch (error) {
        const reason = error instanceof Error ? error : new Error(String(error));
        if (this.directRuntime === runtime) this.invalidateRuntime(reason);
        throw reason;
      }
    }

    const worker = new Worker(new URL("./phoneme.worker.ts", import.meta.url), { type: "module" });
    this.worker = worker;
    worker.addEventListener("message", this.handleMessage);
    worker.addEventListener("error", this.handleWorkerError);
    worker.addEventListener("messageerror", this.handleWorkerMessageError);
    try {
      await this.request("initialize", { websocketProxyUrl: options.websocketProxyUrl });
      if (this.worker !== worker) {
        throw new Error("phoneME Web Worker đã bị thay thế khi đang khởi tạo");
      }
      this.initialized = true;
    } catch (error) {
      const reason = error instanceof Error ? error : new Error(String(error));
      if (this.worker === worker) this.invalidateRuntime(reason);
      throw reason;
    }
  }

  async installJar(file: File, metadata: JarMetadata): Promise<GameEntry> {
    await this.ensureReady();
    if (this.directRuntime) return await this.directRuntime.installJar(file, metadata);
    return await this.request<GameEntry>("installJar", { file, metadata });
  }

  async recycleIdleRuntime() {
    if (this.activeGame) return;
    if (!this.worker && !this.directRuntime) return;

    // Installing/parsing a JAR can permanently grow WebAssembly linear memory;
    // Wasm memory cannot shrink again inside the same instance. Persist first,
    // then discard the idle installer instance so the next MIDlet starts from
    // a clean runtime instead of inheriting installer memory/state.
    await this.flushStorage();
    this.invalidateRuntime(new Error("Khởi tạo lại phoneME sau khi cài ứng dụng"));
  }

  async uninstall(game: GameEntry, removeData: boolean) {
    await this.ensureReady();
    if (this.directRuntime) {
      await this.directRuntime.uninstall(game, removeData);
      return;
    }
    await this.request("uninstall", { game, removeData });
    if (this.currentGameValue?.id === game.id) this.currentGameValue = null;
  }

  async launch(game: GameEntry, width: number, height: number) {
    await this.ensureReady();
    if (this.directRuntime) {
      await this.directRuntime.launch(game, width, height);
      return;
    }
    await this.request("launch", { game, width, height });
    this.currentGameValue = game;
  }

  async resize(width: number, height: number) {
    if (!this.ready) return;
    if (this.directRuntime) {
      if (!this.directRuntime.activeGame) return;
      this.directRuntime.resize(width, height);
      return;
    }
    if (!this.currentGameValue) return;
    await this.request("resize", { width, height });
  }

  async configureHeap(heapMegabytes: number) {
    await this.ensureReady();
    if (this.directRuntime) {
      this.directRuntime.configureHeap(heapMegabytes);
      return;
    }
    await this.request("configureHeap", { heapMegabytes });
  }

  async configureFrameRate() {
    await this.ensureReady();
    if (this.directRuntime) {
      this.directRuntime.configureFrameRate();
      return;
    }
    await this.request("configureFrameRate");
  }

  async configureFrameRateOverride(_enabled: boolean, _framesPerSecond: number) {
    await this.configureFrameRate();
  }

  async configureTranslation(enabled: boolean, provider: "google" | "bing" | "automatic", sourceLanguage: string) {
    await this.ensureReady();
    if (this.directRuntime) {
      this.directRuntime.configureTranslation(enabled, provider, sourceLanguage);
      return;
    }
    if (!this.currentGameValue) return;
    await this.request("configureTranslation", { enabled, provider, sourceLanguage });
  }

  async pause() {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) {
      this.directRuntime.pause();
      return;
    }
    await this.request("pause");
  }

  async resume() {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) {
      this.directRuntime.resume();
      return;
    }
    await this.request("resume");
  }

  async stopMidlet() {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) {
      this.directRuntime.stopMidlet();
      return;
    }
    await this.request("stopMidlet");
    this.currentGameValue = null;
  }

  endAppSession() {
    if (this.directRuntime) {
      // iOS 16 runs the Emscripten pthread runtime on the browser UI thread.
      // Calling C++ destruction here can synchronously pthread_join a stuck
      // MIDlet worker and freeze Safari. Flush storage, abandon the module, and
      // reload the document so WebKit tears down the whole pthread agent safely.
      const directRuntime = this.directRuntime;
      this.directRuntime = null;
      this.clearMediaBridgeState();
      this.recyclableFramePixels = null;
      this.initialized = false;
      this.currentGameValue = null;
      try { directRuntime.setHostForeground(false); } catch { /* best effort */ }
      void directRuntime.flushStorage()
        .catch(() => undefined)
        .finally(() => globalThis.location?.reload());
      return;
    }
    this.invalidateRuntime(new Error("Phiên ứng dụng phoneME đã kết thúc"), true, true);
  }

  async tick(previousGeneration: bigint, includeFrame: boolean): Promise<RuntimeTick> {
    if (!this.ready || !this.activeGame) return { events: [], frame: null, presentedFrame: null };

    if (this.directRuntime) {
      if (!includeFrame) {
        this.directRuntime.releaseFrameView();
        this.directRuntime.pump();
      }
      const frame = includeFrame
        ? this.directRuntime.acquireFrameView(previousGeneration)
        : null;
      return {
        events: this.directRuntime.pollLcduiEvents(),
        frame,
        presentedFrame: null
      };
    }

    if (this.workerPresenterAttaching) {
      return { events: [], frame: null, presentedFrame: null, renderedFrames: 0 };
    }

    if (this.workerPresenterAttached) {
      return this.drainWorkerTick();
    }

    const recyclablePixels = this.recyclableFramePixels;
    this.recyclableFramePixels = null;
    const transferablePixels = recyclablePixels?.buffer.byteLength ? recyclablePixels : null;
    const result = await this.request<RuntimeTick>("tick", {
      previousGeneration,
      includeFrame,
      presentFrame: false,
      recyclablePixels: transferablePixels
    }, transferablePixels ? [transferablePixels.buffer] : []);
    if (result.frame?.pixels instanceof Uint8ClampedArray) {
      this.recyclableFramePixels = result.frame.pixels;
    } else if (recyclablePixels && !transferablePixels) {
      this.recyclableFramePixels = recyclablePixels;
    }
    return result;
  }

  async attachWorkerCanvas(
    canvas: HTMLCanvasElement,
    filtering: boolean
  ): Promise<FramePresenterBackend | null> {
    if (!this.worker || this.directRuntime || this.workerPresenterAttached) {
      return this.workerPresenterBackend;
    }
    const transferableCanvas = canvas as HTMLCanvasElement & {
      transferControlToOffscreen?: () => OffscreenCanvas;
    };
    if (typeof transferableCanvas.transferControlToOffscreen !== "function") return null;

    this.workerPresenterAttaching = true;
    try {
      const offscreen = transferableCanvas.transferControlToOffscreen();
      const result = await this.request<{ backend: FramePresenterBackend }>(
        "attachCanvas",
        { canvas: offscreen, filtering, foreground: document.visibilityState !== "hidden" },
        [offscreen]
      );
      this.workerPresenterAttached = true;
      this.workerPresenterBackend = result.backend;
      this.recyclableFramePixels = null;
      this.workerTickEvents = [];
      this.workerTickPresentedFrame = null;
      this.workerTickRenderedFrames = 0;
      this.workerTickError = "";
      this.onLog?.(`Renderer web: ${result.backend.toUpperCase()} trong Worker.`, false);
      return result.backend;
    } finally {
      this.workerPresenterAttaching = false;
    }
  }

  detachWorkerCanvas() {
    if (!this.workerPresenterAttached && !this.workerPresenterAttaching) return;
    if (this.worker) this.notify("detachCanvas");
    this.workerPresenterAttached = false;
    this.workerPresenterAttaching = false;
    this.workerPresenterBackend = null;
    this.workerTickEvents = [];
    this.workerTickPresentedFrame = null;
    this.workerTickRenderedFrames = 0;
    this.workerTickError = "";
  }

  setPresenterFiltering(enabled: boolean) {
    if (!this.workerPresenterAttached || !this.worker) return;
    this.notify("setPresenterFiltering", { enabled });
  }

  releaseFrameView() {
    this.directRuntime?.releaseFrameView();
  }

  setPresenterForeground(foreground: boolean) {
    if (this.directRuntime) {
      this.directRuntime.setHostForeground(foreground);
      return;
    }
    if (!this.worker) return;
    this.notify("setPresenterForeground", { foreground });
  }

  subscribeRuntimeTicks(listener: (tick: RuntimeTick) => void) {
    this.runtimeTickListeners.add(listener);
    if (this.workerTickEvents.length || this.workerTickPresentedFrame || this.workerTickRenderedFrames > 0 || this.workerTickError) {
      queueMicrotask(() => {
        if (this.runtimeTickListeners.has(listener)) listener(this.drainWorkerTick());
      });
    }
    return () => {
      this.runtimeTickListeners.delete(listener);
    };
  }

  async capturePresentedFrame(): Promise<Blob | null> {
    if (!this.workerPresenterAttached || !this.worker) return null;
    return await this.request<Blob | null>("capturePresenter");
  }

  async copyLcduiImage(componentId: number): Promise<FrameData | null> {
    if (!this.ready || !this.activeGame) return null;
    if (this.directRuntime) return this.directRuntime.copyLcduiImage(componentId);
    return await this.request<FrameData | null>("copyLcduiImage", { componentId });
  }

  async memoryStats(): Promise<RuntimeMemoryStats> {
    const core = this.directRuntime
      ? this.directRuntime.memoryStats()
      : this.worker
        ? await this.request<{
            appEstimatedBytes: number;
            wasmLinearBytes: number;
            frameStagingBytes: number;
          }>("memoryStats")
        : { appEstimatedBytes: 0, wasmLinearBytes: 0, frameStagingBytes: 0 };
    const media = installWebMediaBridge().memoryUsage();
    return {
      ...core,
      mediaCompressedBytes: media.compressedBytes,
      mediaDecodedBytes: media.decodedBytes,
      mediaEntries: media.entries,
      totalTrackedBytes: core.wasmLinearBytes + media.totalBytes
    };
  }

  sendKey(keyCode: number, pressed: boolean) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.sendKey(keyCode, pressed);
    else this.notify("sendKey", { keyCode, pressed });
  }

  sendPointer(x: number, y: number, action: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.sendPointer(x, y, action);
    else this.notify("sendPointer", { x, y, action });
  }

  selectCommand(commandId: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.selectCommand(commandId);
    else this.notify("selectCommand", { commandId });
  }

  selectListItemCommand(componentId: number, elementIndex: number, commandId: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) {
      this.directRuntime.selectListItemCommand(componentId, elementIndex, commandId);
    } else {
      this.notify("selectListItemCommand", { componentId, elementIndex, commandId });
    }
  }

  focusItem(componentId: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.focusItem(componentId);
    else this.notify("focusItem", { componentId });
  }

  activateItem(componentId: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.activateItem(componentId);
    else this.notify("activateItem", { componentId });
  }

  setText(componentId: number, value: string, caret: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.setText(componentId, value, caret);
    else this.notify("setText", { componentId, value, caret });
  }

  setChoice(componentId: number, index: number, selected: boolean) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.setChoice(componentId, index, selected);
    else this.notify("setChoice", { componentId, index, selected });
  }

  setGauge(componentId: number, value: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.setGauge(componentId, value);
    else this.notify("setGauge", { componentId, value });
  }

  setDate(componentId: number, unixSeconds: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.setDate(componentId, unixSeconds);
    else this.notify("setDate", { componentId, unixSeconds });
  }

  setScrollPosition(position: number) {
    if (!this.ready || !this.activeGame) return;
    if (this.directRuntime) this.directRuntime.setScrollPosition(position);
    else this.notify("setScrollPosition", { position });
  }

  async flushStorage() {
    if (!this.ready) return;
    if (this.directRuntime) {
      await this.directRuntime.flushStorage();
      return;
    }
    if (!this.worker) return;
    await this.request("flushStorage");
  }

  async listManagedStorage(kind: ManagedStorageKind, relativePath = ""): Promise<ManagedStorageEntry[]> {
    await this.ensureReady();
    if (this.directRuntime) return await this.directRuntime.listManagedStorage(kind, relativePath);
    return await this.request<ManagedStorageEntry[]>("listManagedStorage", { kind, relativePath });
  }

  async readManagedStorageFile(kind: ManagedStorageKind, relativePath: string) {
    await this.ensureReady();
    if (this.directRuntime) return await this.directRuntime.readManagedStorageFile(kind, relativePath);
    return await this.request<Uint8Array<ArrayBuffer>>("readManagedStorageFile", { kind, relativePath });
  }

  async exportManagedStorage(kind: ManagedStorageKind, relativePath: string): Promise<ManagedStorageExport> {
    await this.ensureReady();
    if (this.directRuntime) return await this.directRuntime.exportManagedStorage(kind, relativePath);
    return await this.request<ManagedStorageExport>("exportManagedStorage", { kind, relativePath });
  }

  async importManagedStorageFiles(
    kind: ManagedStorageKind,
    relativeDirectory: string,
    uploads: Array<{ relativePath: string; file: File }>
  ) {
    await this.ensureReady();
    if (this.directRuntime) {
      await this.directRuntime.importManagedStorageFiles(kind, relativeDirectory, uploads);
      return;
    }
    await this.request("importManagedStorageFiles", { kind, relativeDirectory, uploads });
  }

  async createManagedStorageDirectory(kind: ManagedStorageKind, relativePath: string) {
    await this.ensureReady();
    if (this.directRuntime) {
      await this.directRuntime.createManagedStorageDirectory(kind, relativePath);
      return;
    }
    await this.request("createManagedStorageDirectory", { kind, relativePath });
  }

  async deleteManagedStorageEntry(kind: ManagedStorageKind, relativePath: string) {
    await this.ensureReady();
    if (this.directRuntime) {
      await this.directRuntime.deleteManagedStorageEntry(kind, relativePath);
      return;
    }
    await this.request("deleteManagedStorageEntry", { kind, relativePath });
  }

  dispose() {
    if (this.directRuntime) {
      // Page teardown on the iOS 16 compatibility path must never synchronously
      // join Java pthreads on Safari's main thread. The document owner will
      // release the abandoned module when the page is destroyed/reloaded.
      try { this.directRuntime.setHostForeground(false); } catch { /* best effort */ }
      this.directRuntime = null;
      this.clearMediaBridgeState();
      this.initialized = false;
      this.currentGameValue = null;
      return;
    }
    this.invalidateRuntime(new Error("phoneME Web runtime đã đóng"), true);
  }

  private async ensureReady() {
    if (!this.ready) await this.initialize();
  }

  private request<T = void>(type: string, payload?: unknown, transfer: Transferable[] = []): Promise<T> {
    const worker = this.worker;
    if (!worker) return Promise.reject(new Error("phoneME Web chưa sẵn sàng"));
    const id = ++this.nextRequestId;
    const timeoutMilliseconds = this.requestTimeoutMilliseconds(type);
    return new Promise<T>((resolve, reject) => {
      const timeout = window.setTimeout(() => {
        const pending = this.pending.get(id);
        if (!pending) return;
        this.pending.delete(id);
        const error = new Error(`phoneME Web không phản hồi khi xử lý ${type}`);
        pending.reject(error);
        if (type !== "memoryStats") this.invalidateRuntime(error);
      }, timeoutMilliseconds);
      this.pending.set(id, {
        resolve: (value) => resolve(value as T),
        reject,
        timeout
      });
      try {
        worker.postMessage({ id, type, payload } satisfies WorkerRequest, transfer);
      } catch (error) {
        const pending = this.pending.get(id);
        if (pending) window.clearTimeout(pending.timeout);
        this.pending.delete(id);
        reject(error);
      }
    });
  }

  private requestTimeoutMilliseconds(type: string) {
    switch (type) {
    case "tick": return 2_000;
    case "stopMidlet": return 1_000;
    case "initialize": return 20_000;
    // Large MIDlets can spend tens of seconds in first-start verification,
    // class indexing and static initialization, especially on mobile Safari.
    // Treating that work as a hung Worker after 20 seconds killed a healthy
    // launch and made freshly installed larger games appear permanently broken.
    case "launch": return 30_000;
    case "installJar":
    case "uninstall": return 30_000;
    case "flushStorage":
    case "listManagedStorage":
    case "readManagedStorageFile":
    case "memoryStats": return 10_000;
    case "exportManagedStorage":
    case "importManagedStorageFiles":
    case "createManagedStorageDirectory":
    case "deleteManagedStorageEntry": return 30_000;
    default: return 5_000;
    }
  }

  private notify(type: string, payload?: unknown) {
    this.worker?.postMessage({ id: 0, type, payload } satisfies WorkerRequest);
  }

  private drainWorkerTick(): RuntimeTick {
    const result: RuntimeTick = {
      events: this.workerTickEvents,
      frame: null,
      presentedFrame: this.workerTickPresentedFrame,
      renderedFrames: this.workerTickRenderedFrames,
      error: this.workerTickError || undefined
    };
    this.workerTickEvents = [];
    this.workerTickPresentedFrame = null;
    this.workerTickRenderedFrames = 0;
    this.workerTickError = "";
    return result;
  }

  private handleMessage = (event: MessageEvent<WorkerResponse>) => {
    const message = event.data;
    if (message.event === "log") {
      this.onLog?.(message.line ?? "", Boolean(message.isError));
      return;
    }
    if (message.event === "media") {
      this.handleMediaCommand(message);
      return;
    }
    if (message.event === "runtimeTick") {
      if (message.events?.length) {
        this.workerTickEvents.push(...message.events);
        if (this.workerTickEvents.length > 2048) {
          this.workerTickEvents.splice(0, this.workerTickEvents.length - 2048);
        }
      }
      if (message.presentedFrame) this.workerTickPresentedFrame = message.presentedFrame;
      this.workerTickRenderedFrames += Math.max(0, Number(message.renderedFrames ?? 0));
      if (message.error) this.workerTickError = message.error;
      if (this.runtimeTickListeners.size > 0) {
        const tick = this.drainWorkerTick();
        for (const listener of this.runtimeTickListeners) listener(tick);
      }
      return;
    }
    if (message.event === "fatal") {
      const error = new Error(message.error || "phoneME Web Worker đã dừng do lỗi nghiêm trọng");
      this.onLog?.(error.message, true);
      if (this.runtimeTickListeners.size > 0) {
        const tick: RuntimeTick = { events: [], frame: null, error: error.message };
        for (const listener of this.runtimeTickListeners) listener(tick);
      }
      this.invalidateRuntime(error);
      return;
    }
    if (!message.id) return;
    const pending = this.pending.get(message.id);
    if (!pending) return;
    this.pending.delete(message.id);
    window.clearTimeout(pending.timeout);
    if (message.ok) {
      pending.resolve(message.result);
      return;
    }
    const error = new Error(message.error || "Lỗi phoneME Web Worker");
    pending.reject(error);
    if (message.fatal) this.invalidateRuntime(error);
  };

  private handleMediaCommand(message: WorkerResponse) {
    const worker = this.worker;
    if (!worker || !message.action) return;
    const bridge = installWebMediaBridge();
    const logicalHandle = Number(message.handle ?? 0);
    const nativeHandle = () => this.mediaHandles.get(logicalHandle) ?? 0;

    try {
      switch (message.action) {
      case "createData": {
        const data = message.data instanceof Uint8Array ? message.data : new Uint8Array();
        const handle = bridge.createData(data, String(message.contentType ?? ""));
        if (logicalHandle > 0 && handle > 0) this.mediaHandles.set(logicalHandle, handle);
        break;
      }
      case "createLocator": {
        const handle = bridge.createLocator(String(message.locator ?? ""), String(message.contentType ?? ""));
        if (logicalHandle > 0 && handle > 0) this.mediaHandles.set(logicalHandle, handle);
        break;
      }
      case "start": bridge.start(nativeHandle()); break;
      case "stop": bridge.stop(nativeHandle()); break;
      case "close": {
        const handle = nativeHandle();
        if (handle) bridge.close(handle);
        this.mediaHandles.delete(logicalHandle);
        break;
      }
      case "setLoopCount": bridge.setLoopCount(nativeHandle(), Number(message.count ?? 1)); break;
      case "setVolume": bridge.setVolume(nativeHandle(), Number(message.level ?? 100)); break;
      case "setMute": bridge.setMute(nativeHandle(), message.muted ?? false); break;
      case "setTime": bridge.setTime(nativeHandle(), Number(message.microseconds ?? 0)); break;
      case "playTone":
        bridge.playTone(
          Number(message.note ?? 0),
          Number(message.durationMilliseconds ?? 0),
          Number(message.volume ?? 100)
        );
        break;
      default:
        return;
      }
      if (logicalHandle > 0 && this.mediaHandles.has(logicalHandle)) this.postMediaStatus(logicalHandle);
      this.ensureMediaStatusTimer();
    } catch (error) {
      this.onLog?.(`Web Audio bridge: ${error instanceof Error ? error.message : String(error)}`, true);
      if (logicalHandle > 0) {
        worker.postMessage({ event: "mediaStatus", handle: logicalHandle, error: true });
      }
    }
  }

  private ensureMediaStatusTimer() {
    if (this.mediaStatusTimer !== null || this.mediaHandles.size === 0) return;
    this.mediaStatusTimer = window.setInterval(() => {
      if (!this.worker || this.mediaHandles.size === 0) {
        if (this.mediaStatusTimer !== null) window.clearInterval(this.mediaStatusTimer);
        this.mediaStatusTimer = null;
        return;
      }
      let needsPolling = false;
      for (const handle of this.mediaHandles.keys()) {
        needsPolling = this.postMediaStatus(handle) || needsPolling;
      }
      if (!needsPolling && this.mediaStatusTimer !== null) {
        window.clearInterval(this.mediaStatusTimer);
        this.mediaStatusTimer = null;
      }
    }, 500);
  }

  private postMediaStatus(logicalHandle: number) {
    const worker = this.worker;
    const nativeHandle = this.mediaHandles.get(logicalHandle);
    if (!worker || !nativeHandle) return false;
    const bridge = installWebMediaBridge();
    const duration = bridge.getDuration(nativeHandle);
    const playing = Boolean(bridge.isPlaying(nativeHandle));
    const ended = Boolean(bridge.hasEnded(nativeHandle));
    const error = Boolean(bridge.hasError(nativeHandle));
    worker.postMessage({
      event: "mediaStatus",
      handle: logicalHandle,
      duration,
      time: bridge.getTime(nativeHandle),
      playing,
      ended,
      error
    });
    return playing || (duration < 0 && !ended && !error);
  }

  private clearMediaBridgeState() {
    if (this.mediaStatusTimer !== null) {
      window.clearInterval(this.mediaStatusTimer);
      this.mediaStatusTimer = null;
    }
    installWebMediaBridge().reset();
    this.mediaHandles.clear();
  }

  private handleWorkerError = (event: ErrorEvent) => {
    this.invalidateRuntime(new Error(event.message || "phoneME Web Worker bị lỗi"));
  };

  private handleWorkerMessageError = () => {
    this.invalidateRuntime(new Error("Không đọc được phản hồi từ phoneME Web Worker"));
  };

  private invalidateRuntime(reason: Error, notifyWorker = false, gracefulWorkerShutdown = false) {
    this.clearMediaBridgeState();
    this.recyclableFramePixels = null;
    this.workerPresenterAttached = false;
    this.workerPresenterAttaching = false;
    this.workerPresenterBackend = null;
    this.workerTickEvents = [];
    this.workerTickPresentedFrame = null;
    this.workerTickRenderedFrames = 0;
    this.workerTickError = "";
    const directRuntime = this.directRuntime;
    this.directRuntime = null;
    if (directRuntime) {
      try {
        directRuntime.dispose();
      } catch {
        // A direct Emscripten runtime may already be aborted and not safely disposable.
      }
    }

    const worker = this.worker;
    this.worker = null;
    if (worker) {
      worker.removeEventListener("message", this.handleMessage);
      worker.removeEventListener("error", this.handleWorkerError);
      worker.removeEventListener("messageerror", this.handleWorkerMessageError);
      if (notifyWorker) {
        try {
          worker.postMessage({
            id: 0,
            type: gracefulWorkerShutdown ? "shutdown" : "dispose"
          } satisfies WorkerRequest);
        } catch {
          // The worker may already be terminated after a fatal WebAssembly abort.
        }
      }
      if (gracefulWorkerShutdown) {
        window.setTimeout(() => worker.terminate(), 750);
      } else {
        worker.terminate();
      }
    }

    this.initialized = false;
    this.currentGameValue = null;
    for (const { reject, timeout } of this.pending.values()) {
      window.clearTimeout(timeout);
      reject(reason);
    }
    this.pending.clear();
  }
}

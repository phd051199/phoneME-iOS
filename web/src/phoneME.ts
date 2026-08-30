import { Zip, ZipDeflate } from "fflate";
import type {
  GameEntry,
  JarMetadata,
  LcduiEvent,
  ManagedStorageEntry,
  ManagedStorageExport,
  ManagedStorageKind
} from "./types";

type EmscriptenFs = {
  filesystems: { IDBFS: unknown };
  mkdir(path: string): void;
  mkdirTree(path: string): void;
  mount(type: unknown, options: Record<string, unknown>, mountpoint: string): void;
  syncfs(populate: boolean, callback: (error?: unknown) => void): void;
  readdir(path: string): string[];
  stat(path: string): { mode: number; size: number; mtime?: Date | number };
  isDir(mode: number): boolean;
  readFile(path: string): Uint8Array;
  writeFile(path: string, data: Uint8Array): void;
  unlink(path: string): void;
  rmdir(path: string): void;
};

type PhoneMEModule = {
  FS: EmscriptenFs;
  HEAPU8: Uint8Array;
  HEAP32: Int32Array;
  _malloc(size: number): number;
  _free(pointer: number): void;
  UTF8ToString(pointer: number): string;
  stringToUTF8(value: string, pointer: number, capacity: number): void;
  lengthBytesUTF8(value: string): number;
  pthreadPoolReady?: Promise<unknown>;
  _phoneme_c_api_version(): number;
  _phoneme_create(): number;
  _phoneme_destroy(runtime: number): void;
  _phoneme_configure(runtime: number, runtimeHome: number, classArchive: number): number;
  _phoneme_configure_keymap(runtime: number, up: number, down: number, left: number, right: number, fire: number, soft1: number, soft2: number): number;
  _phoneme_configure_app_frame_pacing(runtime: number, appId: number, framesPerSecond: number, pacingMode: number): number;
  _phoneme_configure_app_heap(runtime: number, appId: number, heapMegabytes: number): number;
  _phoneme_configure_translation(runtime: number, enabled: number, source: number, target: number): number;
  _phoneme_configure_app_translation_v2(runtime: number, appId: number, enabled: number, provider: number, source: number, target: number): number;
  _phoneme_install_jar(runtime: number, jarPath: number, suiteIdOut: number): number;
  _phoneme_uninstall_suite(runtime: number, suiteId: number, removeData: number): number;
  _phoneme_set_suite_trust(runtime: number, suiteId: number, trust: number): number;
  _phoneme_start_system(runtime: number): number;
  _phoneme_start_midlet(runtime: number, suiteId: number, mainClass: number, appId: number, width: number, height: number): number;
  _phoneme_set_foreground(runtime: number, appId: number, width: number, height: number): number;
  _phoneme_pause_midlet(runtime: number, appId: number): number;
  _phoneme_resume_midlet(runtime: number, appId: number): number;
  _phoneme_destroy_midlet(runtime: number, appId: number): number;
  _phoneme_suspend(runtime: number): void;
  _phoneme_resume(runtime: number): void;
  _phoneme_midlet_state(runtime: number, appId: number): number;
  _phoneme_midlet_used_memory(runtime: number, appId: number, timeout: number): bigint;
  _phoneme_send_key(runtime: number, keyCode: number, pressed: number): void;
  _phoneme_send_pointer(runtime: number, x: number, y: number, action: number): void;
  _phoneme_pump_events(runtime: number): void;
  _phoneme_copy_frame_rgba(runtime: number, destination: number, capacity: number, width: number, height: number, generation: number): number;
  _phoneme_copy_frame_rgba_since(runtime: number, previousGeneration: bigint, destination: number, capacity: number, width: number, height: number, generation: number): number;
  _phoneme_acquire_frame_rgba_since(runtime: number, previousGeneration: bigint, width: number, height: number, generation: number): number;
  _phoneme_release_frame_rgba(runtime: number): void;
  _phoneme_storage_generation?(runtime: number): bigint;
  _phoneme_copy_lcdui_image_rgba(runtime: number, componentId: number, destination: number, capacity: number, width: number, height: number, generation: number): number;
  _phoneme_web_poll_lcdui_event_json(runtime: number): number;
  _phoneme_web_error_name(code: number): number;
  _phoneme_lcdui_select_command(runtime: number, commandId: number): void;
  _phoneme_lcdui_select_list_item_command(runtime: number, componentId: number, elementIndex: number, commandId: number): void;
  _phoneme_lcdui_focus_item(runtime: number, componentId: number): void;
  _phoneme_lcdui_activate_item(runtime: number, componentId: number): void;
  _phoneme_lcdui_set_text(runtime: number, componentId: number, text: number, caret: number): void;
  _phoneme_lcdui_set_choice(runtime: number, componentId: number, elementIndex: number, selected: number): void;
  _phoneme_lcdui_set_gauge(runtime: number, componentId: number, value: number): void;
  _phoneme_lcdui_set_date(runtime: number, componentId: number, value: bigint): void;
  _phoneme_lcdui_set_scroll_position(runtime: number, position: number): void;
};

type ModuleFactory = (options: Record<string, unknown>) => Promise<PhoneMEModule>;

type WasmBuildManifest = {
  version: string;
  module: string;
  wasm: string;
  compatModule?: string;
  compatWasm?: string;
};

function supportsWasmSimd() {
  try {
    // A type-only module containing v128 is enough to detect the parser support
    // Safari gained in 16.4, without executing any SIMD instruction.
    return WebAssembly.validate(new Uint8Array([
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x06, 0x01, 0x60, 0x01, 0x7b, 0x00,
    ]));
  } catch {
    return false;
  }
}

export type FrameData = {
  pixels: Uint8ClampedArray<ArrayBuffer>;
  width: number;
  height: number;
  generation: bigint;
};

export type FrameView = {
  pixels: Uint8Array<ArrayBufferLike>;
  width: number;
  height: number;
  generation: bigint;
};

type StagedFrame = {
  byteCount: number;
  width: number;
  height: number;
  generation: bigint;
};

export type PhoneMEOptions = {
  websocketProxyUrl?: string;
  onLog?: (line: string, error: boolean) => void;
};

const APP_ID = 1;
const STORAGE_ROOT = "/phoneme";
const STORAGE_FLUSH_DEBOUNCE_MS = 5_000;
const MAX_WEB_JAVA_HEAP_MIB = 192;
const MAX_JAR_BYTES = 64 * 1024 * 1024;
const MAX_FRAME_DIMENSION = 2_048;
const MAX_FRAME_BYTES = 32 * 1024 * 1024;
const MAX_MANAGED_EXPORT_BYTES = 512 * 1024 * 1024;
const MAX_MANAGED_ARCHIVE_BYTES = 256 * 1024 * 1024;
const MAX_MANAGED_EXPORT_FILES = 4_096;
const RUNTIME_HOME = `${STORAGE_ROOT}/runtime`;
const IMPORT_ROOT = `${STORAGE_ROOT}/imports`;
const MANAGED_STORAGE_ROOTS: Record<ManagedStorageKind, string> = {
  files: `${RUNTIME_HOME}/files`,
  rms: `${RUNTIME_HOME}/rms`
};

function sanitizeName(value: string) {
  const normalized = value.normalize("NFKD").replace(/[^a-zA-Z0-9._-]+/g, "-");
  return normalized.replace(/^-+|-+$/g, "").slice(0, 80) || "game.jar";
}

function normalizeManagedPath(value: string) {
  const normalized = value.replace(/\\/g, "/");
  if (normalized.startsWith("/")) throw new Error("Đường dẫn lưu trữ phải là đường dẫn tương đối");
  const parts = normalized.split("/").filter((part) => part.length > 0 && part !== ".");
  if (parts.some((part) => part === ".." || part.includes("\0"))) {
    throw new Error("Đường dẫn lưu trữ không hợp lệ");
  }
  return parts.join("/");
}

function parentPath(value: string) {
  const index = value.lastIndexOf("/");
  return index < 0 ? "" : value.slice(0, index);
}

function basename(value: string, fallback: string) {
  const normalized = normalizeManagedPath(value);
  return normalized ? normalized.slice(normalized.lastIndexOf("/") + 1) : fallback;
}

export class PhoneMEWebRuntime {
  private module: PhoneMEModule | null = null;
  private runtime = 0;
  private metadataPointer = 0;
  private framePointer = 0;
  private frameCapacity = 0;
  private currentGame: GameEntry | null = null;
  private initialized = false;
  private flushPromise: Promise<void> | null = null;
  private lastFlushedStorageGeneration = 0n;
  private storageDirtySince = 0;
  private fatalErrorValue: Error | null = null;
  private restoreWebSocketRouting: (() => void) | null = null;

  get ready() {
    return this.initialized && this.module !== null && this.runtime !== 0 && !this.fatalErrorValue;
  }

  get activeGame() {
    return this.currentGame;
  }

  get fatalError() {
    return this.fatalErrorValue;
  }

  async initialize(options: PhoneMEOptions = {}) {
    if (this.ready) return;
    if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer === "undefined") {
      throw new Error("WebAssembly đa luồng cần COOP/COEP. Hãy chạy bằng Vite hoặc máy chủ có header cách ly chéo nguồn.");
    }

    this.fatalErrorValue = null;
    this.configureWebsockifyRouting(options.websocketProxyUrl);
    const { moduleUrl, wasmUrl } = await this.resolveWasmAssets();
    const imported = await import(/* @vite-ignore */ moduleUrl) as { default: ModuleFactory };
    this.module = await imported.default({
      locateFile: (path: string) => path === "phoneme.wasm" ? wasmUrl : new URL(path, moduleUrl).href,
      print: (line: string) => options.onLog?.(String(line), false),
      printErr: (line: string) => options.onLog?.(String(line), true),
      websocket: options.websocketProxyUrl ? { subprotocol: "binary" } : undefined,
      onAbort: (reason: unknown) => {
        const message = reason instanceof Error ? reason.message : String(reason || "WebAssembly đã abort");
        this.fatalErrorValue = new Error(message);
      }
    });

    this.ensureDirectory(STORAGE_ROOT);
    try {
      this.module.FS.mount(
        this.module.FS.filesystems.IDBFS,
        { autoPersist: false },
        STORAGE_ROOT
      );
    } catch (error) {
      if (this.errnoOf(error) !== 10) throw error; // EBUSY: already mounted.
    }
    await this.syncFileSystem(true);
    this.ensureDirectory(RUNTIME_HOME);
    this.ensureDirectory(IMPORT_ROOT);
    this.ensureDirectory(MANAGED_STORAGE_ROOTS.files);
    this.ensureDirectory(MANAGED_STORAGE_ROOTS.rms);
    void globalThis.navigator?.storage?.persist?.().catch(() => false);

    this.runtime = this.module._phoneme_create();
    if (!this.runtime) throw new Error("Không tạo được phoneME runtime");
    this.metadataPointer = this.module._malloc(16);

    const configureCode = this.withCString(RUNTIME_HOME, (home) =>
      this.module!._phoneme_configure(this.runtime, home, 0)
    );
    this.assertOk(configureCode, "Cấu hình core");
    this.assertOk(
      this.module._phoneme_configure_keymap(this.runtime, -1, -2, -3, -4, -5, -6, -7),
      "Cấu hình phím"
    );
    this.assertOk(this.module._phoneme_start_system(this.runtime), "Khởi động hệ thống J2ME");
    this.lastFlushedStorageGeneration = this.storageGeneration() ?? 0n;
    this.storageDirtySince = 0;
    this.initialized = true;
  }

  async installJar(file: File, metadata: JarMetadata): Promise<GameEntry> {
    const module = this.requireModule();
    if (file.size <= 0 || file.size > MAX_JAR_BYTES) {
      throw new Error("JAR quá lớn để cài đặt an toàn trên bản web");
    }
    const id = crypto.randomUUID();
    const fileName = sanitizeName(file.name.toLowerCase().endsWith(".jar") ? file.name : `${file.name}.jar`);
    const path = `${IMPORT_ROOT}/${id}-${fileName}`;
    module.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));

    const suitePointer = module._malloc(4);
    try {
      const result = this.withCString(path, (jarPath) =>
        module._phoneme_install_jar(this.runtime, jarPath, suitePointer)
      );
      this.assertOk(result, "Cài đặt JAR");
      const suiteId = module.HEAP32[suitePointer >> 2];
      this.assertOk(module._phoneme_set_suite_trust(this.runtime, suiteId, 1), "Cấp quyền suite");
      return {
        id,
        suiteId,
        title: metadata.title,
        vendor: metadata.vendor,
        version: metadata.version,
        mainClass: metadata.mainClass,
        fileName,
        installedAt: Date.now(),
        iconDataUrl: metadata.iconDataUrl
      };
    } finally {
      module._free(suitePointer);
      try {
        module.FS.unlink(path);
      } catch {
        // The managed suite store already owns its private JAR copy.
      }
      // Installation is not complete until both the managed suite and removal
      // of the transient import file are durable in IDBFS. The old detached
      // flush allowed the UI to launch/reload while syncfs was still running,
      // which could leave a stale import or a partially persisted suite after
      // a quick navigation/reload on mobile browsers.
      await this.flushStorage();
    }
  }

  async uninstall(game: GameEntry, removeData: boolean) {
    const module = this.requireModule();
    if (this.currentGame?.id === game.id) this.stopMidlet();
    this.assertOk(
      module._phoneme_uninstall_suite(this.runtime, game.suiteId, removeData ? 1 : 0),
      "Gỡ ứng dụng"
    );
    await this.flushStorage();
  }

  async launch(game: GameEntry, width: number, height: number) {
    const module = this.requireModule();
    this.assertFrameDimensions(width, height);
    await module.pthreadPoolReady;
    if (this.currentGame) {
      module._phoneme_destroy_midlet(this.runtime, APP_ID);
      this.currentGame = null;
    }
    this.assertOk(
      module._phoneme_set_suite_trust(this.runtime, game.suiteId, 1),
      `Cấp quyền ${game.title}`
    );
    const result = this.withCString(game.mainClass, (mainClass) =>
      module._phoneme_start_midlet(this.runtime, game.suiteId, mainClass, APP_ID, width, height)
    );
    this.assertOk(result, `Mở ${game.title}`);
    this.currentGame = game;
  }

  resize(width: number, height: number) {
    if (!this.currentGame) return;
    this.assertFrameDimensions(width, height);
    this.assertOk(
      this.requireModule()._phoneme_set_foreground(this.runtime, APP_ID, width, height),
      "Đổi kích thước màn hình"
    );
  }

  configureHeap(heapMegabytes: number) {
    const safeHeapMegabytes = Math.min(
      MAX_WEB_JAVA_HEAP_MIB,
      Math.max(1, Math.round(heapMegabytes))
    );
    this.assertOk(
      this.requireModule()._phoneme_configure_app_heap(this.runtime, APP_ID, safeHeapMegabytes),
      "Cấu hình heap"
    );
  }

  configureFrameRate() {
    this.assertOk(
      this.requireModule()._phoneme_configure_app_frame_pacing(
        this.runtime,
        APP_ID,
        30,
        1
      ),
      "Giới hạn FPS"
    );
  }

  configureFrameRateOverride(_enabled: boolean, _framesPerSecond: number) {
    this.configureFrameRate();
  }

  configureTranslation(enabled: boolean, provider: "google" | "bing" | "automatic", sourceLanguage: string) {
    if (!this.currentGame) return;
    const module = this.requireModule();
    const providerValue = provider === "google" ? 0 : provider === "bing" ? 1 : 2;
    const invoke = (sourcePointer: number) => this.withCString("vi", (targetPointer) =>
      module._phoneme_configure_app_translation_v2(
        this.runtime,
        APP_ID,
        enabled ? 1 : 0,
        providerValue,
        sourcePointer,
        targetPointer
      )
    );
    const code = sourceLanguage === "auto"
      ? invoke(0)
      : this.withCString(sourceLanguage, invoke);
    this.assertOk(code, "Cấu hình tự động dịch");
  }

  pause() {
    if (!this.currentGame) return;
    this.assertOk(this.requireModule()._phoneme_pause_midlet(this.runtime, APP_ID), "Tạm dừng");
  }

  resume() {
    if (!this.currentGame) return;
    this.assertOk(this.requireModule()._phoneme_resume_midlet(this.runtime, APP_ID), "Tiếp tục");
  }

  setHostForeground(foreground: boolean) {
    if (!this.ready || !this.currentGame) return;
    if (foreground) this.requireModule()._phoneme_resume(this.runtime);
    else this.requireModule()._phoneme_suspend(this.runtime);
  }

  stopMidlet() {
    if (!this.currentGame) return;
    this.requireModule()._phoneme_destroy_midlet(this.runtime, APP_ID);
    this.currentGame = null;
    void this.flushStorage();
  }

  state() {
    return this.ready ? this.requireModule()._phoneme_midlet_state(this.runtime, APP_ID) : 0;
  }

  usedMemory() {
    if (!this.currentGame) return 0;
    const value = this.requireModule()._phoneme_midlet_used_memory(this.runtime, APP_ID, 0);
    return value < 0n ? 0 : Number(value);
  }

  memoryStats() {
    const module = this.module;
    return {
      appEstimatedBytes: this.currentGame && this.ready ? this.usedMemory() : 0,
      wasmLinearBytes: module?.HEAPU8.buffer.byteLength ?? 0,
      frameStagingBytes: this.frameCapacity
    };
  }

  sendKey(keyCode: number, pressed: boolean) {
    this.requireModule()._phoneme_send_key(this.runtime, keyCode, pressed ? 1 : 0);
  }

  sendPointer(x: number, y: number, action: number) {
    this.requireModule()._phoneme_send_pointer(this.runtime, x, y, action);
  }

  pump() {
    this.requireModule()._phoneme_pump_events(this.runtime);
    this.maybeFlushStorage();
  }

  copyFrame(previousGeneration: bigint, reusablePixels?: Uint8ClampedArray<ArrayBuffer> | null): FrameData | null {
    const module = this.requireModule();
    const staged = this.stageChangedFrame(previousGeneration);
    if (!staged) return null;
    const pixels = reusablePixels && reusablePixels.byteLength >= staged.byteCount
      ? new Uint8ClampedArray(reusablePixels.buffer, reusablePixels.byteOffset, staged.byteCount)
      : new Uint8ClampedArray(staged.byteCount);
    pixels.set(module.HEAPU8.subarray(this.framePointer, this.framePointer + staged.byteCount));
    return {
      pixels,
      width: staged.width,
      height: staged.height,
      generation: staged.generation
    };
  }

  acquireFrameView(previousGeneration: bigint): FrameView | null {
    const module = this.requireModule();
    this.maybeFlushStorage();
    const widthPointer = this.metadataPointer;
    const heightPointer = this.metadataPointer + 4;
    const generationPointer = this.metadataPointer + 8;
    const pixelsPointer = module._phoneme_acquire_frame_rgba_since(
      this.runtime,
      previousGeneration,
      widthPointer,
      heightPointer,
      generationPointer
    );
    if (!pixelsPointer) return null;

    try {
      const view = new DataView(module.HEAPU8.buffer);
      const width = view.getInt32(widthPointer, true);
      const height = view.getInt32(heightPointer, true);
      const generation = view.getBigUint64(generationPointer, true);
      this.assertFrameDimensions(width, height);
      return {
        pixels: new Uint8Array(module.HEAPU8.buffer, pixelsPointer, width * height * 4),
        width,
        height,
        generation
      };
    } catch (error) {
      module._phoneme_release_frame_rgba(this.runtime);
      throw error;
    }
  }

  releaseFrameView() {
    if (!this.module || !this.runtime) return;
    this.module._phoneme_release_frame_rgba(this.runtime);
  }

  withFrameView<T>(previousGeneration: bigint, body: (frame: FrameView) => T): T | null {
    const frame = this.acquireFrameView(previousGeneration);
    if (!frame) return null;
    try {
      return body(frame);
    } finally {
      this.releaseFrameView();
    }
  }

  copyLcduiImage(componentId: number): FrameData | null {
    const module = this.requireModule();
    const widthPointer = this.metadataPointer;
    const heightPointer = this.metadataPointer + 4;
    const generationPointer = this.metadataPointer + 8;
    const required = module._phoneme_copy_lcdui_image_rgba(
      this.runtime,
      componentId,
      0,
      0,
      widthPointer,
      heightPointer,
      generationPointer
    );
    if (required <= 0) return null;
    this.ensureFrameCapacity(required);
    const copied = module._phoneme_copy_lcdui_image_rgba(
      this.runtime,
      componentId,
      this.framePointer,
      this.frameCapacity,
      widthPointer,
      heightPointer,
      generationPointer
    );
    if (copied <= 0) return null;
    const view = new DataView(module.HEAPU8.buffer);
    return {
      pixels: (() => {
        const pixels = new Uint8ClampedArray(copied);
        pixels.set(module.HEAPU8.subarray(this.framePointer, this.framePointer + copied));
        return pixels;
      })(),
      width: view.getInt32(widthPointer, true),
      height: view.getInt32(heightPointer, true),
      generation: view.getBigUint64(generationPointer, true)
    };
  }

  pollLcduiEvents(limit = 512) {
    const module = this.requireModule();
    const events: LcduiEvent[] = [];
    while (events.length < limit) {
      const pointer = module._phoneme_web_poll_lcdui_event_json(this.runtime);
      if (!pointer) break;
      events.push(JSON.parse(module.UTF8ToString(pointer)) as LcduiEvent);
    }
    return events;
  }

  selectCommand(commandId: number) {
    this.requireModule()._phoneme_lcdui_select_command(this.runtime, commandId);
  }

  selectListItemCommand(componentId: number, elementIndex: number, commandId: number) {
    this.requireModule()._phoneme_lcdui_select_list_item_command(this.runtime, componentId, elementIndex, commandId);
  }

  focusItem(componentId: number) {
    this.requireModule()._phoneme_lcdui_focus_item(this.runtime, componentId);
  }

  activateItem(componentId: number) {
    this.requireModule()._phoneme_lcdui_activate_item(this.runtime, componentId);
  }

  setText(componentId: number, value: string, caret: number) {
    this.withCString(value, (text) => {
      this.requireModule()._phoneme_lcdui_set_text(this.runtime, componentId, text, caret);
      return 0;
    });
  }

  setChoice(componentId: number, index: number, selected: boolean) {
    this.requireModule()._phoneme_lcdui_set_choice(this.runtime, componentId, index, selected ? 1 : 0);
  }

  setGauge(componentId: number, value: number) {
    this.requireModule()._phoneme_lcdui_set_gauge(this.runtime, componentId, value);
  }

  setDate(componentId: number, unixSeconds: number) {
    this.requireModule()._phoneme_lcdui_set_date(this.runtime, componentId, BigInt(Math.trunc(unixSeconds)));
  }

  setScrollPosition(position: number) {
    this.requireModule()._phoneme_lcdui_set_scroll_position(this.runtime, position);
  }

  async flushStorage() {
    if (!this.module) return;
    if (!this.flushPromise) {
      const generationBeforeFlush = this.runtime
        ? (this.storageGeneration() ?? this.lastFlushedStorageGeneration)
        : this.lastFlushedStorageGeneration;
      this.flushPromise = this.syncFileSystem(false).then(() => {
        this.lastFlushedStorageGeneration = generationBeforeFlush;
        this.storageDirtySince = 0;
      }).finally(() => {
        this.flushPromise = null;
      });
    }
    await this.flushPromise;
  }

  async listManagedStorage(kind: ManagedStorageKind, relativePath = ""): Promise<ManagedStorageEntry[]> {
    const module = this.requireModule();
    const relative = normalizeManagedPath(relativePath);
    const absolute = this.managedStoragePath(kind, relative);
    this.ensureDirectory(MANAGED_STORAGE_ROOTS[kind]);
    const entries = module.FS.readdir(absolute)
      .filter((name) => name !== "." && name !== "..")
      .map((name) => {
        const path = relative ? `${relative}/${name}` : name;
        const stat = module.FS.stat(this.managedStoragePath(kind, path));
        const modifiedAt = stat.mtime instanceof Date ? stat.mtime.getTime() : Number(stat.mtime ?? 0);
        return {
          name,
          path,
          isDirectory: module.FS.isDir(stat.mode),
          size: stat.size,
          modifiedAt: Number.isFinite(modifiedAt) ? modifiedAt : 0
        } satisfies ManagedStorageEntry;
      });
    entries.sort((left, right) => {
      if (left.isDirectory !== right.isDirectory) return left.isDirectory ? -1 : 1;
      return left.name.localeCompare(right.name, undefined, { numeric: true, sensitivity: "base" });
    });
    return entries;
  }

  async readManagedStorageFile(kind: ManagedStorageKind, relativePath: string) {
    const module = this.requireModule();
    const relative = normalizeManagedPath(relativePath);
    if (!relative) throw new Error("Hãy chọn một tệp để tải xuống");
    const absolute = this.managedStoragePath(kind, relative);
    const stat = module.FS.stat(absolute);
    if (module.FS.isDir(stat.mode)) throw new Error("Đường dẫn đã chọn là thư mục");
    return new Uint8Array(module.FS.readFile(absolute));
  }

  async exportManagedStorage(kind: ManagedStorageKind, relativePath: string): Promise<ManagedStorageExport> {
    const module = this.requireModule();
    const relative = normalizeManagedPath(relativePath);
    const absolute = this.managedStoragePath(kind, relative);
    const stat = module.FS.stat(absolute);
    const name = basename(relative, kind === "rms" ? "rms" : "files");
    if (!module.FS.isDir(stat.mode)) {
      if (Number(stat.size) > MAX_MANAGED_ARCHIVE_BYTES) {
        throw new Error("Tệp quá lớn để tải xuống an toàn trong trình duyệt");
      }
      return {
        name,
        isDirectory: false,
        files: [{ path: name, data: new Uint8Array(module.FS.readFile(absolute)) }]
      };
    }

    const entries: Array<{ absolutePath: string; archivePath: string; size: number }> = [];
    let totalBytes = 0;
    const collect = (directory: string, archivePath: string) => {
      for (const child of module.FS.readdir(directory)) {
        if (child === "." || child === "..") continue;
        const childAbsolute = `${directory}/${child}`;
        const childArchivePath = archivePath ? `${archivePath}/${child}` : child;
        const childStat = module.FS.stat(childAbsolute);
        if (module.FS.isDir(childStat.mode)) {
          collect(childAbsolute, childArchivePath);
          continue;
        }
        if (entries.length >= MAX_MANAGED_EXPORT_FILES) {
          throw new Error(`Thư mục có quá nhiều tệp để xuất an toàn (tối đa ${MAX_MANAGED_EXPORT_FILES})`);
        }
        const size = Math.max(0, Number(childStat.size));
        if (size > MAX_MANAGED_ARCHIVE_BYTES) {
          throw new Error(`Tệp ${childArchivePath} quá lớn để nén an toàn trong trình duyệt`);
        }
        totalBytes += size;
        if (totalBytes > MAX_MANAGED_EXPORT_BYTES) {
          throw new Error("Thư mục quá lớn để xuất trong một lần");
        }
        entries.push({ absolutePath: childAbsolute, archivePath: childArchivePath, size });
      }
    };
    collect(absolute, name);

    const archive = await new Promise<Blob>((resolve, reject) => {
      const chunks: Uint8Array<ArrayBuffer>[] = [];
      let emittedBytes = 0;
      let settled = false;
      const archiveStream = new Zip((error, chunk, final) => {
        if (settled) return;
        if (error) {
          settled = true;
          reject(error);
          return;
        }
        if (chunk?.byteLength) {
          emittedBytes += chunk.byteLength;
          if (emittedBytes > MAX_MANAGED_ARCHIVE_BYTES) {
            settled = true;
            archiveStream.terminate();
            reject(new Error("File ZIP vượt giới hạn bộ nhớ an toàn của trình duyệt"));
            return;
          }
          chunks.push(chunk.slice());
        }
        if (final) {
          settled = true;
          resolve(new Blob(chunks, { type: "application/zip" }));
        }
      });

      try {
        for (const entry of entries) {
          const zipEntry = new ZipDeflate(entry.archivePath, { level: 1 });
          archiveStream.add(zipEntry);
          // Read and compress one file at a time. Raw directory contents are
          // never retained together, avoiding the previous folder-size RAM spike.
          zipEntry.push(module.FS.readFile(entry.absolutePath), true);
        }
        archiveStream.end();
      } catch (error) {
        if (!settled) {
          settled = true;
          try { archiveStream.terminate(); } catch { /* best effort */ }
          reject(error);
        }
      }
    });
    return { name, isDirectory: true, files: [], archive };
  }

  async importManagedStorageFiles(
    kind: ManagedStorageKind,
    relativeDirectory: string,
    uploads: Array<{ relativePath: string; file: File }>
  ) {
    this.assertStorageMutationAllowed();
    const module = this.requireModule();
    const base = normalizeManagedPath(relativeDirectory);
    let totalUploadBytes = 0;
    for (const upload of uploads) {
      if (upload.file.size > MAX_MANAGED_ARCHIVE_BYTES) {
        throw new Error(`Tệp ${upload.file.name} quá lớn để tải lên an toàn`);
      }
      totalUploadBytes += Math.max(0, upload.file.size);
      if (totalUploadBytes > MAX_MANAGED_EXPORT_BYTES) {
        throw new Error("Tổng dữ liệu tải lên quá lớn cho một lần thao tác");
      }
      const child = normalizeManagedPath(upload.relativePath);
      if (!child) continue;
      const target = base ? `${base}/${child}` : child;
      const parent = parentPath(target);
      if (parent) this.ensureDirectory(this.managedStoragePath(kind, parent));
      module.FS.writeFile(
        this.managedStoragePath(kind, target),
        new Uint8Array(await upload.file.arrayBuffer())
      );
    }
    await this.flushStorage();
  }

  async createManagedStorageDirectory(kind: ManagedStorageKind, relativePath: string) {
    this.assertStorageMutationAllowed();
    const relative = normalizeManagedPath(relativePath);
    if (!relative) throw new Error("Tên thư mục không hợp lệ");
    this.ensureDirectory(this.managedStoragePath(kind, relative));
    await this.flushStorage();
  }

  async deleteManagedStorageEntry(kind: ManagedStorageKind, relativePath: string) {
    this.assertStorageMutationAllowed();
    const module = this.requireModule();
    const relative = normalizeManagedPath(relativePath);
    if (!relative) throw new Error("Không thể xóa thư mục gốc");
    const remove = (absolute: string) => {
      const stat = module.FS.stat(absolute);
      if (!module.FS.isDir(stat.mode)) {
        module.FS.unlink(absolute);
        return;
      }
      for (const child of module.FS.readdir(absolute)) {
        if (child === "." || child === "..") continue;
        remove(`${absolute}/${child}`);
      }
      module.FS.rmdir(absolute);
    };
    remove(this.managedStoragePath(kind, relative));
    await this.flushStorage();
  }

  dispose() {
    const module = this.module;
    if (module && this.currentGame && this.runtime) module._phoneme_destroy_midlet(this.runtime, APP_ID);
    if (module && this.runtime) module._phoneme_destroy(this.runtime);
    if (module && this.framePointer) module._free(this.framePointer);
    if (module && this.metadataPointer) module._free(this.metadataPointer);
    this.runtime = 0;
    this.framePointer = 0;
    this.frameCapacity = 0;
    this.metadataPointer = 0;
    this.module = null;
    this.initialized = false;
    this.currentGame = null;
    this.flushPromise = null;
    this.lastFlushedStorageGeneration = 0n;
    this.storageDirtySince = 0;
    this.fatalErrorValue = null;
    this.restoreWebSocketRouting?.();
    this.restoreWebSocketRouting = null;
  }

  private configureWebsockifyRouting(proxyUrl?: string) {
    this.restoreWebSocketRouting?.();
    this.restoreWebSocketRouting = null;
    if (!proxyUrl) return;

    const proxyBase = new URL(proxyUrl, globalThis.location.href);
    const NativeWebSocket = globalThis.WebSocket;
    const RoutedWebSocket = new Proxy(NativeWebSocket, {
      construct(Target, args) {
        const [rawUrl, rawProtocols] = args as [string | URL, string | string[] | undefined];
        const protocols = Array.isArray(rawProtocols)
          ? rawProtocols
          : rawProtocols ? [rawProtocols] : [];
        const isSocketFs = protocols.includes("binary") || protocols.includes("base64");
        if (!isSocketFs) return Reflect.construct(Target, args);

        const destination = new URL(String(rawUrl));
        const port = destination.port || (destination.protocol === "wss:" ? "443" : "80");
        const routed = new URL(proxyBase.href);
        const access = routed.searchParams.get("access") ?? "";
        routed.searchParams.delete("access");
        const target = `${destination.hostname}:${port}`;
        routed.searchParams.set("token", access ? `${access}@${target}` : target);
        const nextArgs = rawProtocols === undefined
          ? [routed.href]
          : [routed.href, rawProtocols];
        return Reflect.construct(Target, nextArgs);
      }
    }) as typeof WebSocket;

    globalThis.WebSocket = RoutedWebSocket;
    this.restoreWebSocketRouting = () => {
      if (globalThis.WebSocket === RoutedWebSocket) globalThis.WebSocket = NativeWebSocket;
    };
  }

  private requireModule() {
    if (this.fatalErrorValue) throw this.fatalErrorValue;
    if (!this.module || !this.runtime) throw new Error("phoneME Web chưa sẵn sàng");
    return this.module;
  }

  private assertOk(code: number, action: string) {
    if (code === 0) return;
    const module = this.requireModule();
    const messagePointer = module._phoneme_web_error_name(code);
    const message = messagePointer ? module.UTF8ToString(messagePointer) : `mã ${code}`;
    throw new Error(`${action}: ${message}`);
  }

  private withCString<T>(value: string, body: (pointer: number) => T) {
    const module = this.module;
    if (!module) throw new Error("phoneME Web chưa nạp module");
    const capacity = module.lengthBytesUTF8(value) + 1;
    const pointer = module._malloc(capacity);
    if (!pointer) throw new Error("WebAssembly hết bộ nhớ");
    try {
      module.stringToUTF8(value, pointer, capacity);
      return body(pointer);
    } finally {
      module._free(pointer);
    }
  }

  private stageChangedFrame(previousGeneration: bigint): StagedFrame | null {
    const module = this.requireModule();
    this.maybeFlushStorage();
    const widthPointer = this.metadataPointer;
    const heightPointer = this.metadataPointer + 4;
    const generationPointer = this.metadataPointer + 8;

    for (let attempt = 0; attempt < 3; attempt += 1) {
      const required = module._phoneme_copy_frame_rgba_since(
        this.runtime,
        previousGeneration,
        this.framePointer,
        this.frameCapacity,
        widthPointer,
        heightPointer,
        generationPointer
      );
      if (required <= 0) return null;
      if (!this.framePointer || required > this.frameCapacity) {
        this.ensureFrameCapacity(required);
        continue;
      }

      const view = new DataView(module.HEAPU8.buffer);
      const width = view.getInt32(widthPointer, true);
      const height = view.getInt32(heightPointer, true);
      const generation = view.getBigUint64(generationPointer, true);
      if (width <= 0 || height <= 0 || generation === previousGeneration) return null;
      return { byteCount: required, width, height, generation };
    }
    throw new Error("Framebuffer thay đổi kích thước liên tục khi đang copy");
  }

  private ensureFrameCapacity(required: number) {
    const module = this.requireModule();
    if (!Number.isFinite(required) || required <= 0 || required > MAX_FRAME_BYTES) {
      throw new Error("Framebuffer vượt giới hạn bộ nhớ an toàn của bản web");
    }
    if (required <= this.frameCapacity) return;
    if (this.framePointer) module._free(this.framePointer);
    this.frameCapacity = Math.max(required, Math.ceil(required * 1.25));
    this.framePointer = module._malloc(this.frameCapacity);
    if (!this.framePointer) throw new Error("Không cấp phát được framebuffer WebAssembly");
  }

  private assertFrameDimensions(width: number, height: number) {
    if (!Number.isFinite(width) || !Number.isFinite(height) ||
        width <= 0 || height <= 0 ||
        width > MAX_FRAME_DIMENSION || height > MAX_FRAME_DIMENSION ||
        width * height * 4 > MAX_FRAME_BYTES) {
      throw new Error(`Kích thước màn hình web không hợp lệ: ${width}×${height}`);
    }
  }

  private maybeFlushStorage() {
    if (!this.module || !this.currentGame || this.flushPromise) return;
    const generation = this.storageGeneration();
    if (generation !== null && generation === this.lastFlushedStorageGeneration) {
      this.storageDirtySince = 0;
      return;
    }
    const now = performance.now();
    if (this.storageDirtySince <= 0) {
      this.storageDirtySince = now;
      return;
    }
    if (now - this.storageDirtySince < STORAGE_FLUSH_DEBOUNCE_MS) return;
    void this.flushStorage().catch(() => undefined);
  }

  private storageGeneration(): bigint | null {
    if (!this.module || !this.runtime) return null;
    const storageGeneration = this.module._phoneme_storage_generation;
    if (typeof storageGeneration !== "function") return null;
    return storageGeneration(this.runtime);
  }

  private ensureDirectory(path: string) {
    this.module?.FS.mkdirTree(path);
  }

  private managedStoragePath(kind: ManagedStorageKind, relativePath: string) {
    const relative = normalizeManagedPath(relativePath);
    return relative ? `${MANAGED_STORAGE_ROOTS[kind]}/${relative}` : MANAGED_STORAGE_ROOTS[kind];
  }

  private assertStorageMutationAllowed() {
    if (this.currentGame) {
      throw new Error("Hãy dừng game trước khi thay đổi File/RMS để tránh hỏng dữ liệu đang mở");
    }
  }

  private async resolveWasmAssets() {
    const fallbackModuleUrl = new URL("/wasm/phoneme.js", globalThis.location.href).href;
    const fallbackWasmUrl = new URL("/wasm/phoneme.wasm", globalThis.location.href).href;
    try {
      const manifestUrl = new URL("/wasm/manifest.json", globalThis.location.href);
      const response = await fetch(manifestUrl, { cache: "no-cache" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const manifest = await response.json() as Partial<WasmBuildManifest>;
      if (!manifest.version || !manifest.module || !manifest.wasm) {
        throw new Error("manifest WebAssembly không hợp lệ");
      }
      const useSimd = supportsWasmSimd();
      const selectedModule = useSimd
        ? manifest.module
        : (manifest.compatModule || "/wasm/phoneme.js");
      const selectedWasm = useSimd
        ? manifest.wasm
        : (manifest.compatWasm || "/wasm/phoneme.wasm");
      const moduleUrl = new URL(selectedModule, manifestUrl).href;
      const wasmUrl = new URL(selectedWasm, manifestUrl).href;
      if (new URL(moduleUrl).origin !== globalThis.location.origin || new URL(wasmUrl).origin !== globalThis.location.origin) {
        throw new Error("manifest WebAssembly trỏ ra ngoài origin");
      }
      return { moduleUrl, wasmUrl };
    } catch {
      // Backward-compatible fallback for deployments created before manifest.json.
      return { moduleUrl: fallbackModuleUrl, wasmUrl: fallbackWasmUrl };
    }
  }

  private errnoOf(error: unknown) {
    if (!error || typeof error !== "object") return undefined;
    const errno = (error as { errno?: unknown }).errno;
    return typeof errno === "number" ? errno : undefined;
  }

  private syncFileSystem(populate: boolean) {
    if (!this.module) return Promise.resolve();
    return new Promise<void>((resolve, reject) => {
      this.module!.FS.syncfs(populate, (error) => error ? reject(error) : resolve());
    });
  }
}

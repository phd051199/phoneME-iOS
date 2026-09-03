import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  AppBar,
  Box,
  Button,
  CircularProgress,
  Dialog,
  DialogActions,
  DialogContent,
  DialogContentText,
  DialogTitle,
  Divider,
  FormControlLabel,
  IconButton,
  Menu,
  MenuItem,
  Switch,
  Toolbar,
  Tooltip,
  Typography
} from "@mui/material";
import {
  ArrowForwardIosRounded,
  AspectRatioRounded,
  CameraAltRounded,
  CheckRounded,
  KeyboardRounded,
  MoreHorizRounded,
  OpenWithRounded,
  PowerSettingsNewRounded,
  RestartAltRounded,
  ScreenRotationRounded,
  TranslateRounded,
  VisibilityOffRounded,
  VisibilityRounded
} from "@mui/icons-material";
import { createFramePresenter, type FramePresenter } from "./framePresenter";
import { applyLcduiEvents, EMPTY_LCDUI_STATE, LCDUICommandBar, NativeLcduiView, type LcduiState } from "./lcdui";
import type { PhoneMEWebRuntime, RuntimeTick } from "./phoneMEClient";
import type { GameEntry, RuntimeSnapshot } from "./types";
import type { TranslationLanguage, TranslationSourceLanguage, VirtualKeyboardType, WebGameProfile } from "./webProfile";

const WEB_MAX_FPS = 60;
const HOST_FRAME_INTERVAL_MS = 1000 / WEB_MAX_FPS;
const BACKGROUND_HOST_POLL_INTERVAL_MS = 250;
const POINTER_MOVE_INTERVAL_MS = 1000 / 60;
const POINTER_PRESSED = 1;
const POINTER_RELEASED = 2;
const POINTER_DRAGGED = 3;

const TRANSLATION_LANGUAGES: Array<{ value: TranslationLanguage; label: string }> = [
  { value: "vi", label: "Tiếng Việt" },
  { value: "zh-CN", label: "Tiếng Trung (Giản thể)" },
  { value: "zh-TW", label: "Tiếng Trung (Phồn thể)" },
  { value: "ja", label: "Tiếng Nhật" },
  { value: "ko", label: "Tiếng Hàn" },
  { value: "en", label: "Tiếng Anh" },
  { value: "ru", label: "Tiếng Nga" },
  { value: "th", label: "Tiếng Thái" },
  { value: "id", label: "Tiếng Indonesia" },
  { value: "es", label: "Tiếng Tây Ban Nha" },
  { value: "pt", label: "Tiếng Bồ Đào Nha" },
  { value: "fr", label: "Tiếng Pháp" },
  { value: "de", label: "Tiếng Đức" }
];

const TRANSLATION_SOURCE_LANGUAGES: Array<{ value: TranslationSourceLanguage; label: string }> = [
  { value: "auto", label: "Tự động nhận diện" },
  ...TRANSLATION_LANGUAGES
];

const translationLanguageLabel = (language: TranslationSourceLanguage | TranslationLanguage) =>
  TRANSLATION_SOURCE_LANGUAGES.find((item) => item.value === language)?.label ?? language;

const KEYBOARD_MAP: Record<string, number> = {
  ArrowUp: -1,
  ArrowDown: -2,
  ArrowLeft: -3,
  ArrowRight: -4,
  Enter: -5,
  " ": -5,
  q: -6,
  Q: -6,
  F1: -6,
  e: -7,
  E: -7,
  F2: -7,
  "0": 48,
  "1": 49,
  "2": 50,
  "3": 51,
  "4": 52,
  "5": 53,
  "6": 54,
  "7": 55,
  "8": 56,
  "9": 57,
  "*": 42,
  "#": 35
};

type KeyControl = {
  id: string;
  label: string;
  keys: number[];
  groupId: "soft-keys" | "numbers" | "directions";
  column: number;
  row: number;
};

type KeyboardEditMode = "none" | "position" | "size";

type KeyboardDefinition = {
  metricColumns: number;
  contentColumns: number;
  rows: number;
  keyHeightFactor: number;
  controls: KeyControl[];
};

function keyGroup(id: string): KeyControl["groupId"] {
  if (id === "soft-left" || id === "soft-right" || id === "menu" || id === "fire-top") return "soft-keys";
  if (id.startsWith("n-") || ["one", "three", "seven", "nine", "star", "zero", "pound"].includes(id)) return "numbers";
  return "directions";
}

const key = (id: string, label: string, keyCode: number, column: number, row: number): KeyControl => ({
  id,
  label,
  keys: [keyCode],
  groupId: keyGroup(id),
  column,
  row
});

const diagonal = (id: string, label: string, keys: number[], column: number, row: number): KeyControl => ({
  id,
  label,
  keys,
  groupId: "directions",
  column,
  row
});

function numberControls(startColumn: number): KeyControl[] {
  const values = [
    ["1", 49], ["2", 50], ["3", 51],
    ["4", 52], ["5", 53], ["6", 54],
    ["7", 55], ["8", 56], ["9", 57],
    ["*", 42], ["0", 48], ["#", 35]
  ] as const;
  return values.map(([label, code], index) => key(`n-${label}`, label, code, startColumn + index % 3, Math.floor(index / 3)));
}

function directionControls(startColumn: number): KeyControl[] {
  return [
    key("soft-left", "L", -6, startColumn, 0),
    key("soft-right", "R", -7, startColumn + 2, 0),
    diagonal("up-left", "↖", [-1, -3], startColumn, 1),
    key("up", "↑", -1, startColumn + 1, 1),
    diagonal("up-right", "↗", [-1, -4], startColumn + 2, 1),
    key("left", "←", -3, startColumn, 2),
    key("fire", "F", -5, startColumn + 1, 2),
    key("right", "→", -4, startColumn + 2, 2),
    diagonal("down-left", "↙", [-2, -3], startColumn, 3),
    key("down", "↓", -2, startColumn + 1, 3),
    diagonal("down-right", "↘", [-2, -4], startColumn + 2, 3)
  ];
}

function keyboardDefinition(type: VirtualKeyboardType): KeyboardDefinition {
  if (type === "numbersArrows" || type === "arrowsNumbers") {
    const numbersFirst = type === "numbersArrows";
    return {
      metricColumns: 6,
      contentColumns: 6,
      rows: 4,
      keyHeightFactor: 0.75,
      controls: [
        ...numberControls(numbersFirst ? 0 : 3),
        ...directionControls(numbersFirst ? 3 : 0)
      ]
    };
  }

  if (type === "numbers") {
    return {
      metricColumns: 6,
      contentColumns: 5,
      rows: 4,
      keyHeightFactor: 0.75,
      controls: [
        key("soft-left", "L", -6, 0, 0),
        key("soft-right", "R", -7, 4, 0),
        ...numberControls(1)
      ]
    };
  }

  if (type === "arrows") {
    return {
      metricColumns: 6,
      contentColumns: 5,
      rows: 3,
      keyHeightFactor: 0.75,
      controls: [
        key("soft-left", "L", -6, 0, 0),
        diagonal("up-left", "↖", [-1, -3], 1, 0),
        key("up", "↑", -1, 2, 0),
        diagonal("up-right", "↗", [-1, -4], 3, 0),
        key("soft-right", "R", -7, 4, 0),
        key("left", "←", -3, 1, 1),
        key("fire", "F", -5, 2, 1),
        key("right", "→", -4, 3, 1),
        diagonal("down-left", "↙", [-2, -3], 1, 2),
        key("down", "↓", -2, 2, 2),
        diagonal("down-right", "↘", [-2, -4], 3, 2)
      ]
    };
  }

  const arrows = type === "phoneArrows";
  const controls: KeyControl[] = [
    key("soft-left", "L", -6, 0, 0),
    arrows ? { id: "menu", label: "M", keys: [], groupId: "soft-keys", column: 1, row: 0 } : key("fire-top", "F", -5, 1, 0),
    key("soft-right", "R", -7, 2, 0)
  ];
  if (arrows) {
    controls.push(
      key("one", "1", 49, 0, 1), key("up", "↑", -1, 1, 1), key("three", "3", 51, 2, 1),
      key("left", "←", -3, 0, 2), key("fire", "F", -5, 1, 2), key("right", "→", -4, 2, 2),
      key("seven", "7", 55, 0, 3), key("down", "↓", -2, 1, 3), key("nine", "9", 57, 2, 3),
      key("star", "*", 42, 0, 4), key("zero", "0", 48, 1, 4), key("pound", "#", 35, 2, 4)
    );
  } else {
    numberControls(0).forEach((control) => controls.push({ ...control, row: control.row + 1 }));
  }
  return { metricColumns: 3, contentColumns: 3, rows: 5, keyHeightFactor: 0.5625, controls };
}

class HostKeyInputCoordinator {
  private readonly heldOwners = new Map<string, number[]>();
  private readonly keyOwnerCounts = new Map<number, number>();
  private readonly pointerOwners = new Map<number, string>();
  private readonly ownerPointers = new Map<string, Set<number>>();

  constructor(private readonly runtime: PhoneMEWebRuntime) {}

  press(ownerId: string, keys: number[]) {
    if (this.heldOwners.has(ownerId)) return false;
    const uniqueKeys = [...new Set(keys)];
    if (!uniqueKeys.length) return false;
    this.heldOwners.set(ownerId, uniqueKeys);
    for (const code of uniqueKeys) {
      const owners = this.keyOwnerCounts.get(code) ?? 0;
      this.keyOwnerCounts.set(code, owners + 1);
      if (owners === 0) this.runtime.sendKey(code, true);
    }
    return true;
  }

  pressPointer(ownerId: string, pointerId: number, keys: number[]) {
    if (!keys.length) return false;
    const previousOwner = this.pointerOwners.get(pointerId);
    if (previousOwner && previousOwner !== ownerId) this.releasePointer(pointerId);

    let pointers = this.ownerPointers.get(ownerId);
    if (!pointers) {
      pointers = new Set<number>();
      this.ownerPointers.set(ownerId, pointers);
    }
    if (pointers.has(pointerId)) return false;
    const wasHeldByPointer = pointers.size > 0;
    pointers.add(pointerId);
    this.pointerOwners.set(pointerId, ownerId);
    return wasHeldByPointer ? false : this.press(ownerId, keys);
  }

  releasePointer(pointerId: number) {
    const ownerId = this.pointerOwners.get(pointerId);
    if (!ownerId) return false;
    this.pointerOwners.delete(pointerId);
    const pointers = this.ownerPointers.get(ownerId);
    pointers?.delete(pointerId);
    if (pointers?.size) return false;
    this.ownerPointers.delete(ownerId);
    return this.release(ownerId);
  }

  release(ownerId: string) {
    const keys = this.heldOwners.get(ownerId);
    this.detachPointers(ownerId);
    if (!keys) return false;
    this.heldOwners.delete(ownerId);
    for (const code of keys) {
      const owners = this.keyOwnerCounts.get(code) ?? 0;
      if (owners <= 1) {
        this.keyOwnerCounts.delete(code);
        this.runtime.sendKey(code, false);
      } else {
        this.keyOwnerCounts.set(code, owners - 1);
      }
    }
    return true;
  }

  releasePrefix(prefix: string) {
    let released = 0;
    const owners = [...this.heldOwners.keys()].filter((ownerId) => ownerId.startsWith(prefix));
    for (const ownerId of owners) {
      if (this.release(ownerId)) released += 1;
    }
    return released;
  }

  hasHeldPrefix(prefix: string) {
    for (const ownerId of this.heldOwners.keys()) {
      if (ownerId.startsWith(prefix)) return true;
    }
    return false;
  }

  releaseAll() {
    if (!this.heldOwners.size && !this.keyOwnerCounts.size && !this.pointerOwners.size) return;
    this.heldOwners.clear();
    this.pointerOwners.clear();
    this.ownerPointers.clear();
    const keys = [...this.keyOwnerCounts.keys()].sort((a, b) => a - b);
    this.keyOwnerCounts.clear();
    for (const code of keys) this.runtime.sendKey(code, false);
  }

  private detachPointers(ownerId: string) {
    const pointers = this.ownerPointers.get(ownerId);
    if (!pointers) return;
    for (const pointerId of pointers) this.pointerOwners.delete(pointerId);
    this.ownerPointers.delete(ownerId);
  }
}

function VirtualKeyButton({ control, coordinator, profile, onActivity }: {
  control: KeyControl;
  coordinator: HostKeyInputCoordinator;
  profile: WebGameProfile;
  onActivity: () => void;
}) {
  const ownerId = `virtual:${control.id}`;
  const finishPointer = useCallback((pointerId: number) => {
    if (coordinator.releasePointer(pointerId)) onActivity();
  }, [coordinator, onActivity]);

  useEffect(() => () => {
    coordinator.release(ownerId);
  }, [coordinator, ownerId]);

  return <Button
    className={`virtual-key shape-${profile.buttonShape} group-${control.groupId}`}
    variant="text"
    aria-label={control.label}
    onContextMenu={(event) => event.preventDefault()}
    onPointerDown={(event) => {
      if (event.pointerType === "mouse" && event.button !== 0) return;
      event.preventDefault();
      try {
        event.currentTarget.setPointerCapture(event.pointerId);
      } catch {
        // Global pointerup/pointercancel fallback below still guarantees release.
      }
      if (!coordinator.pressPointer(ownerId, event.pointerId, control.keys)) return;
      if (profile.hapticFeedback && "vibrate" in navigator) navigator.vibrate(8);
      onActivity();
    }}
    onPointerUp={(event) => finishPointer(event.pointerId)}
    onPointerCancel={(event) => finishPointer(event.pointerId)}
    onLostPointerCapture={(event) => finishPointer(event.pointerId)}
  >{control.label}</Button>;
}

function VirtualKeypad({ inputCoordinator, profile, surfaceWidth, surfaceHeight, editMode, onProfileChange, onActivity }: {
  inputCoordinator: HostKeyInputCoordinator;
  profile: WebGameProfile;
  surfaceWidth: number;
  surfaceHeight: number;
  editMode: KeyboardEditMode;
  onProfileChange: (profile: WebGameProfile) => void;
  onActivity: () => void;
}) {
  const definition = useMemo(() => keyboardDefinition(profile.virtualKeyboardType), [profile.virtualKeyboardType]);
  const dragRef = useRef<{
    controlId: string;
    groupId: KeyControl["groupId"];
    startX: number;
    startY: number;
    offsetX: number;
    offsetY: number;
    scaleWidth: number;
    scaleHeight: number;
  } | null>(null);
  // Keep web keypad geometry in lockstep with KeypadView.swift. The native app
  // lays controls out inside a bottom keyboard frame, sizes keys from the
  // layout's metric column count, and uses a real 4...8pt gap between keys.
  const layoutWidth = Math.max(surfaceWidth - 12, 0);
  const layoutHeight = surfaceWidth > surfaceHeight
    ? Math.min(surfaceHeight * 0.50, 280)
    : Math.min(surfaceHeight * 0.42, 320);
  const layoutLeft = (surfaceWidth - layoutWidth) / 2;
  const layoutTop = surfaceHeight - layoutHeight - 8;
  const gap = Math.min(8, Math.max(4, layoutWidth / 82));
  const keyWidth = Math.max(
    28,
    (layoutWidth - gap * (definition.metricColumns - 1)) / definition.metricColumns
  );
  const availableHeight = Math.max(
    28,
    (layoutHeight - gap * (definition.rows - 1)) / definition.rows
  );
  const keyHeight = Math.min(keyWidth * definition.keyHeightFactor, availableHeight);
  const contentWidth = keyWidth * definition.contentColumns
    + gap * (definition.contentColumns - 1);
  const contentHeight = keyHeight * definition.rows + gap * (definition.rows - 1);
  const startX = layoutLeft + (layoutWidth - contentWidth) / 2;
  const startY = layoutTop + layoutHeight - contentHeight;
  const hidden = new Set(profile.hiddenKeyboardControlIds);

  const beginEdit = (event: React.PointerEvent<HTMLDivElement>, control: KeyControl) => {
    if (editMode === "none") return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture(event.pointerId);
    const offset = profile.keyboardControlOffsets[control.id] ?? { x: 0, y: 0 };
    const scale = profile.keyboardGroupScales[control.groupId] ?? { width: 1, height: 1 };
    dragRef.current = {
      controlId: control.id,
      groupId: control.groupId,
      startX: event.clientX,
      startY: event.clientY,
      offsetX: offset.x,
      offsetY: offset.y,
      scaleWidth: scale.width,
      scaleHeight: scale.height
    };
  };

  const updateEdit = (event: React.PointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current;
    if (!drag || editMode === "none") return;
    event.preventDefault();
    const dx = event.clientX - drag.startX;
    const dy = event.clientY - drag.startY;
    if (editMode === "position") {
      onProfileChange({
        ...profile,
        keyboardControlOffsets: {
          ...profile.keyboardControlOffsets,
          [drag.controlId]: {
            x: Math.max(-1, Math.min(1, drag.offsetX + dx / Math.max(surfaceWidth, 1))),
            y: Math.max(-1, Math.min(1, drag.offsetY + dy / Math.max(surfaceHeight, 1)))
          }
        }
      });
    } else {
      onProfileChange({
        ...profile,
        keyboardGroupScales: {
          ...profile.keyboardGroupScales,
          [drag.groupId]: {
            width: Math.max(0.45, Math.min(2.5, drag.scaleWidth + dx / Math.max(keyWidth * 2, 1))),
            height: Math.max(0.45, Math.min(2.5, drag.scaleHeight + dy / Math.max(keyHeight * 2, 1)))
          }
        }
      });
    }
  };

  const endEdit = () => { dragRef.current = null; };

  useEffect(() => {
    if (editMode !== "none") inputCoordinator.releasePrefix("virtual:");
  }, [editMode, inputCoordinator]);

  useEffect(() => {
    inputCoordinator.releasePrefix("virtual:");
  }, [inputCoordinator, profile.virtualKeyboardType]);

  useEffect(() => () => {
    inputCoordinator.releasePrefix("virtual:");
  }, [inputCoordinator]);

  return <Box
    className={`virtual-keypad-overlay ${editMode !== "none" ? "keyboard-editing" : ""}`}
    data-layout={profile.virtualKeyboardType}
    style={{
      height: surfaceHeight,
      "--keyboard-opacity": editMode === "none" ? profile.keyboardOpacity : 1
    } as React.CSSProperties}
  >
    <Box className="virtual-key-grid" style={{ width: surfaceWidth, height: surfaceHeight }}>
      {definition.controls.filter((control) => !hidden.has(control.id)).map((control) => {
        const baseLeft = startX + control.column * (keyWidth + gap);
        const baseTop = startY + control.row * (keyHeight + gap);
        const offset = profile.keyboardControlOffsets[control.id] ?? { x: 0, y: 0 };
        const scale = profile.keyboardGroupScales[control.groupId] ?? { width: 1, height: 1 };
        const width = keyWidth * scale.width;
        const height = keyHeight * scale.height;
        const left = baseLeft + offset.x * surfaceWidth - (width - keyWidth) / 2;
        const top = baseTop + offset.y * surfaceHeight - (height - keyHeight) / 2;
        return <Box
          key={control.id}
          className={`virtual-key-cell ${editMode !== "none" ? "editable" : ""}`}
          style={{ left, top, width, height }}
          onPointerDown={(event) => beginEdit(event, control)}
          onPointerMove={updateEdit}
          onPointerUp={endEdit}
          onPointerCancel={endEdit}
        >
          <VirtualKeyButton control={control} coordinator={inputCoordinator} profile={profile} onActivity={onActivity} />
        </Box>;
      })}
    </Box>
  </Box>;
}

function renderedFrameRect(
  frameWidth: number,
  frameHeight: number,
  availableWidth: number,
  availableHeight: number,
  profile: WebGameProfile
) {
  const fw = Math.max(frameWidth, 1);
  const fh = Math.max(frameHeight, 1);
  const aw = Math.max(availableWidth, 1);
  const ah = Math.max(availableHeight, 1);
  let width = fw;
  let height = fh;

  if (profile.scaleType === "asIs") {
    const scale = profile.scalePercent / 100;
    width = fw * scale;
    height = fh * scale;
  } else if (profile.scaleType === "fill") {
    if (profile.preserveAspectRatio) {
      const scale = Math.max(aw / fw, ah / fh);
      width = fw * scale;
      height = fh * scale;
    } else {
      width = aw;
      height = ah;
    }
  } else {
    const requestedScale = Math.max(profile.scalePercent / 100, 0.01);
    if (profile.preserveAspectRatio) {
      const widthScale = aw / fw;
      const heightScale = ah / fh;
      const shouldFitWidth = profile.screenGravity === "top" && fh >= fw && ah >= aw;
      const fitScale = shouldFitWidth ? widthScale : Math.min(widthScale, heightScale);
      width = fw * fitScale * requestedScale;
      height = fh * fitScale * requestedScale;
    } else {
      width = aw * requestedScale;
      height = ah * requestedScale;
    }
  }

  let left = (aw - width) / 2;
  let top = (ah - height) / 2;
  if (profile.screenGravity === "left") left = 0;
  if (profile.screenGravity === "right") left = aw - width;
  if (profile.screenGravity === "top") top = 0;
  if (profile.screenGravity === "bottom") top = ah - height;
  if (profile.screenGravity === "left" || profile.screenGravity === "right") top = (ah - height) / 2;
  if (profile.screenGravity === "top" || profile.screenGravity === "bottom") left = (aw - width) / 2;
  return { left, top, width, height };
}

export type EmulatorScreenProps = {
  runtime: PhoneMEWebRuntime;
  game: GameEntry;
  profile: WebGameProfile;
  translationProvider: "google" | "bing" | "automatic";
  onProfileChange: (profile: WebGameProfile) => void;
  onHide: () => void;
  onStop: () => void;
  onSnapshot: (snapshot: RuntimeSnapshot) => void;
};

export function EmulatorScreen({ runtime, game, profile, translationProvider, onProfileChange, onStop, onSnapshot }: EmulatorScreenProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const surfaceRef = useRef<HTMLDivElement>(null);
  const generationRef = useRef(0n);
  const frameCounterRef = useRef({ count: 0, startedAt: performance.now() });
  const telemetryRef = useRef({ fps: 0, width: profile.screenWidth, height: profile.screenHeight });
  const mainPresenterRef = useRef<FramePresenter | null>(null);
  const inputCoordinator = useMemo(() => new HostKeyInputCoordinator(runtime), [runtime]);
  const activePointerRef = useRef<{ id: number; x: number; y: number } | null>(null);
  const lastPointerMoveAtRef = useRef(0);
  const hideTimerRef = useRef<number | null>(null);
  const appBarHideTimerRef = useRef<number | null>(null);
  const appBarSwipeRef = useRef<{ id: number; x: number; y: number } | null>(null);
  const [surfaceSize, setSurfaceSize] = useState({ width: 1, height: 1 });
  const [frameSize, setFrameSize] = useState({ width: profile.screenWidth, height: profile.screenHeight });
  const [lcdui, setLcdui] = useState<LcduiState>(EMPTY_LCDUI_STATE);
  const [fpsValue, setFpsValue] = useState(0);
  const [runtimeError, setRuntimeError] = useState("");
  const [sessionReady, setSessionReady] = useState(false);
  const [presentationReady, setPresentationReady] = useState(false);
  const [showKeypad, setShowKeypad] = useState(profile.showVirtualKeyboard);
  const [menuAnchor, setMenuAnchor] = useState<HTMLElement | null>(null);
  const [keyboardMenuAnchor, setKeyboardMenuAnchor] = useState<HTMLElement | null>(null);
  const [layoutMenuAnchor, setLayoutMenuAnchor] = useState<HTMLElement | null>(null);
  const [translationMenuAnchor, setTranslationMenuAnchor] = useState<HTMLElement | null>(null);
  const [translationSourceMenuAnchor, setTranslationSourceMenuAnchor] = useState<HTMLElement | null>(null);
  const [translationTargetMenuAnchor, setTranslationTargetMenuAnchor] = useState<HTMLElement | null>(null);
  const [keyboardEditMode, setKeyboardEditMode] = useState<KeyboardEditMode>("none");
  const [keyboardEditSnapshot, setKeyboardEditSnapshot] = useState<WebGameProfile | null>(null);
  const [showHiddenKeysEditor, setShowHiddenKeysEditor] = useState(false);
  const [hiddenKeyDraft, setHiddenKeyDraft] = useState<string[]>(profile.hiddenKeyboardControlIds);
  const [showExitConfirmation, setShowExitConfirmation] = useState(false);
  const [isAppBarTemporarilyVisible, setIsAppBarTemporarilyVisible] = useState(false);
  const activeScreen = lcdui.activeScreenId ? lcdui.screens[lcdui.activeScreenId] : undefined;
  const hasNativeScreen = Boolean(activeScreen?.visible && activeScreen.type !== 22);
  const hasCanvasScreen = Boolean(activeScreen?.visible && activeScreen.type === 22);
  const presentsFullscreenCanvas = Boolean(
    hasCanvasScreen && (profile.forceFullscreen || activeScreen?.fullScreen)
  );
  const showsCanvasCommandBar = Boolean(
    hasCanvasScreen && !presentsFullscreenCanvas && activeScreen?.commands.length
  );
  const navigationTitle = activeScreen?.visible && activeScreen.type !== 21 && activeScreen.title?.trim()
    ? activeScreen.title.trim()
    : game.title;

  const displayRect = useMemo(() => renderedFrameRect(
    frameSize.width,
    frameSize.height,
    surfaceSize.width,
    surfaceSize.height,
    profile
  ), [frameSize.height, frameSize.width, profile, surfaceSize.height, surfaceSize.width]);

  const scheduleKeyboardHide = useCallback(() => {
    if (hideTimerRef.current !== null) window.clearTimeout(hideTimerRef.current);
    hideTimerRef.current = null;
    if (
      keyboardEditMode !== "none" ||
      !profile.keyboardHideDelayMilliseconds ||
      !showKeypad ||
      inputCoordinator.hasHeldPrefix("virtual:")
    ) return;
    hideTimerRef.current = window.setTimeout(() => {
      hideTimerRef.current = null;
      if (inputCoordinator.hasHeldPrefix("virtual:")) return;
      setShowKeypad(false);
    }, profile.keyboardHideDelayMilliseconds);
  }, [inputCoordinator, keyboardEditMode, profile.keyboardHideDelayMilliseconds, showKeypad]);

  useEffect(() => {
    setShowKeypad(profile.showVirtualKeyboard);
  }, [profile.showVirtualKeyboard]);

  useEffect(() => {
    scheduleKeyboardHide();
    return () => {
      if (hideTimerRef.current !== null) window.clearTimeout(hideTimerRef.current);
    };
  }, [scheduleKeyboardHide]);

  useEffect(() => () => {
    if (appBarHideTimerRef.current !== null) window.clearTimeout(appBarHideTimerRef.current);
  }, []);

  useEffect(() => {
    const surface = surfaceRef.current;
    if (!surface) return;
    const update = () => setSurfaceSize({ width: surface.clientWidth, height: surface.clientHeight });
    update();
    const observer = new ResizeObserver(update);
    observer.observe(surface);
    return () => observer.disconnect();
  }, [keyboardEditMode, profile.showAppBar]);

  useEffect(() => {
    let active = true;
    generationRef.current = 0n;
    setLcdui(EMPTY_LCDUI_STATE);
    setRuntimeError("");
    setSessionReady(false);
    onSnapshot({
      phase: "loading",
      message: runtime.ready ? `Đang mở ${game.title}` : `Đang chờ lõi phoneME để mở ${game.title}`,
      fps: 0,
      usedMemory: 0,
      frameWidth: profile.screenWidth,
      frameHeight: profile.screenHeight
    });
    const alreadyRunning = runtime.activeGame?.id === game.id;
    const launchPromise = (async () => {
      if (!alreadyRunning) {
        await runtime.configureHeap(profile.heapSizeMegabytes);
        await runtime.configureFrameRateOverride(
          profile.frameRateOverride,
          profile.frameRateLimit
        );
        await runtime.launch(game, profile.screenWidth, profile.screenHeight);
      } else {
        await runtime.configureFrameRateOverride(
          profile.frameRateOverride,
          profile.frameRateLimit
        );
      }
      await runtime.configureTranslation(
        profile.autoTranslateEnabled,
        translationProvider,
        profile.translationSourceLanguage,
        profile.translationTargetLanguage
      );
    })();
    void launchPromise.then(() => {
      if (!active) {
        runtime.endAppSession();
        return;
      }
      setSessionReady(true);
      onSnapshot({
        phase: "running",
        message: `Đang chạy ${game.title}`,
        fps: 0,
        usedMemory: 0,
        frameWidth: profile.screenWidth,
        frameHeight: profile.screenHeight
      });
    }).catch((error) => {
      if (!active) return;
      const message = error instanceof Error ? error.message : String(error);
      setRuntimeError(message);
      onSnapshot({ phase: "error", message, fps: 0, usedMemory: 0, frameWidth: profile.screenWidth, frameHeight: profile.screenHeight });
    });
    return () => {
      active = false;
      inputCoordinator.releaseAll();
      runtime.endAppSession();
    };
    // Launch once for this game/profile screen size. Visual profile fields are handled without restarting.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [game.id, runtime]);

  useEffect(() => {
    void runtime.resize(profile.screenWidth, profile.screenHeight).catch((error) => {
      setRuntimeError(error instanceof Error ? error.message : String(error));
    });
  }, [profile.screenHeight, profile.screenWidth, runtime]);

  useEffect(() => {
    const releaseCanvasPointer = (pointerId?: number) => {
      const pointer = activePointerRef.current;
      if (!pointer || (pointerId !== undefined && pointer.id !== pointerId)) return false;
      runtime.sendPointer(pointer.x, pointer.y, POINTER_RELEASED);
      activePointerRef.current = null;
      lastPointerMoveAtRef.current = 0;
      return true;
    };
    const releaseHostInput = () => {
      inputCoordinator.releaseAll();
      releaseCanvasPointer();
    };
    const handleVisibility = () => {
      if (document.visibilityState === "hidden") releaseHostInput();
    };
    const handlePointerEnd = (event: PointerEvent) => {
      inputCoordinator.releasePointer(event.pointerId);
      releaseCanvasPointer(event.pointerId);
    };
    window.addEventListener("blur", releaseHostInput);
    window.addEventListener("pagehide", releaseHostInput);
    window.addEventListener("pointerup", handlePointerEnd);
    window.addEventListener("pointercancel", handlePointerEnd);
    document.addEventListener("visibilitychange", handleVisibility);
    return () => {
      window.removeEventListener("blur", releaseHostInput);
      window.removeEventListener("pagehide", releaseHostInput);
      window.removeEventListener("pointerup", handlePointerEnd);
      window.removeEventListener("pointercancel", handlePointerEnd);
      document.removeEventListener("visibilitychange", handleVisibility);
      releaseHostInput();
    };
  }, [inputCoordinator, runtime]);

  useEffect(() => {
    if (profile.touchInput) return;
    const pointer = activePointerRef.current;
    if (pointer) runtime.sendPointer(pointer.x, pointer.y, POINTER_RELEASED);
    activePointerRef.current = null;
    lastPointerMoveAtRef.current = 0;
  }, [profile.touchInput, runtime]);

  useEffect(() => {
    const handleDown = (event: KeyboardEvent) => {
      const keyCode = KEYBOARD_MAP[event.key];
      if (keyCode === undefined || event.repeat) return;
      if ((event.target as HTMLElement | null)?.matches("input, textarea, select, [contenteditable=true]")) return;
      event.preventDefault();
      inputCoordinator.press(`hardware:${event.code || `key:${event.key}`}`, [keyCode]);
    };
    const handleUp = (event: KeyboardEvent) => {
      const released = inputCoordinator.release(`hardware:${event.code || `key:${event.key}`}`);
      if (released || KEYBOARD_MAP[event.key] !== undefined) event.preventDefault();
    };
    window.addEventListener("keydown", handleDown);
    window.addEventListener("keyup", handleUp);
    return () => {
      window.removeEventListener("keydown", handleDown);
      window.removeEventListener("keyup", handleUp);
      inputCoordinator.releasePrefix("hardware:");
    };
  }, [inputCoordinator]);

  const recordRenderedFrames = useCallback((count: number) => {
    if (!profile.showFPS || count <= 0) return;
    const now = performance.now();
    frameCounterRef.current.count += count;
    const elapsed = now - frameCounterRef.current.startedAt;
    if (elapsed < 1000) return;
    setFpsValue(frameCounterRef.current.count * 1000 / Math.max(1, elapsed));
    frameCounterRef.current = { count: 0, startedAt: now };
  }, [profile.showFPS]);

  const applyRuntimeTick = useCallback((result: RuntimeTick) => {
    if (result.error) {
      setRuntimeError(result.error);
      onSnapshot({ phase: "error", message: result.error, fps: 0, usedMemory: 0, frameWidth: frameSize.width, frameHeight: frameSize.height });
      return;
    }
    if (result.events.length) {
      setLcdui((current) => applyLcduiEvents(current, result.events));
    }
    const presentedFrame = result.presentedFrame;
    if (presentedFrame) {
      generationRef.current = presentedFrame.generation;
      setFrameSize((current) =>
        current.width === presentedFrame.width && current.height === presentedFrame.height
          ? current
          : { width: presentedFrame.width, height: presentedFrame.height }
      );
    }
    if (result.renderedFrames) recordRenderedFrames(result.renderedFrames);

    const frame = result.frame;
    const presenter = mainPresenterRef.current;
    if (frame) {
      try {
        if (presenter) {
          generationRef.current = frame.generation;
          setFrameSize((current) =>
            current.width === frame.width && current.height === frame.height
              ? current
              : { width: frame.width, height: frame.height }
          );
          presenter.present(frame);
          recordRenderedFrames(1);
        }
      } finally {
        // No-op for Worker presentation. On the iOS 16 direct path this
        // releases the core framebuffer read lease immediately after upload.
        runtime.releaseFrameView();
      }
    }
  }, [frameSize.height, frameSize.width, onSnapshot, recordRenderedFrames, runtime]);

  useEffect(() => {
    frameCounterRef.current = { count: 0, startedAt: performance.now() };
    if (!profile.showFPS) setFpsValue(0);
  }, [profile.showFPS, sessionReady]);

  useEffect(() => {
    telemetryRef.current = {
      fps: fpsValue,
      width: frameSize.width,
      height: frameSize.height
    };
  }, [fpsValue, frameSize.height, frameSize.width]);

  useEffect(() => {
    if (!sessionReady || runtimeError) return;
    let active = true;
    let timer = 0;
    const sample = async () => {
      try {
        const stats = await runtime.memoryStats();
        if (!active) return;
        const current = telemetryRef.current;
        onSnapshot({
          phase: "running",
          message: `Đang chạy ${game.title}`,
          fps: current.fps,
          usedMemory: stats.totalTrackedBytes,
          frameWidth: current.width,
          frameHeight: current.height
        });
      } catch {
        // Memory telemetry is diagnostic only and must never stop a MIDlet.
      }
      if (active) timer = window.setTimeout(() => void sample(), 2_000);
    };
    timer = window.setTimeout(() => void sample(), 1_000);
    return () => {
      active = false;
      window.clearTimeout(timer);
    };
  }, [game.title, onSnapshot, runtime, runtimeError, sessionReady]);

  useEffect(() => {
    if (!sessionReady) return;
    return runtime.subscribeRuntimeTicks(applyRuntimeTick);
  }, [applyRuntimeTick, runtime, sessionReady]);

  useEffect(() => {
    if (!sessionReady || hasNativeScreen) {
      setPresentationReady(false);
      return;
    }
    const canvas = canvasRef.current;
    if (!canvas) return;
    let active = true;
    setPresentationReady(false);

    void (async () => {
      try {
        const workerBackend = await runtime.attachWorkerCanvas(canvas, profile.filtering);
        if (!active) {
          if (workerBackend) runtime.detachWorkerCanvas();
          return;
        }
        if (workerBackend) {
          setPresentationReady(true);
          return;
        }

        const presenter = await createFramePresenter(canvas, profile.filtering);
        if (!active) {
          presenter.dispose();
          return;
        }
        mainPresenterRef.current = presenter;
        setPresentationReady(true);
      } catch (error) {
        if (!active) return;
        setRuntimeError(error instanceof Error ? error.message : String(error));
      }
    })();

    return () => {
      active = false;
      mainPresenterRef.current?.dispose();
      mainPresenterRef.current = null;
      runtime.detachWorkerCanvas();
    };
  }, [hasNativeScreen, runtime, sessionReady]);

  useEffect(() => {
    mainPresenterRef.current?.setFiltering(profile.filtering);
    runtime.setPresenterFiltering(profile.filtering);
  }, [profile.filtering, runtime]);

  useEffect(() => {
    if (!sessionReady) return;
    const updateForeground = () => {
      runtime.setPresenterForeground(document.visibilityState !== "hidden");
    };
    updateForeground();
    document.addEventListener("visibilitychange", updateForeground);
    return () => document.removeEventListener("visibilitychange", updateForeground);
  }, [runtime, sessionReady]);

  useEffect(() => {
    if (!sessionReady || runtime.presentationRunsInWorker) return;
    let timer = 0;
    let animationFrame = 0;
    let active = true;
    let lastHostTickAt = 0;

    const scheduleNext = () => {
      if (!active) return;
      if (document.visibilityState === "hidden") {
        timer = window.setTimeout(() => void tick(performance.now()), BACKGROUND_HOST_POLL_INTERVAL_MS);
      } else {
        animationFrame = window.requestAnimationFrame((now) => void tick(now));
      }
    };

    const tick = async (now: number) => {
      if (!active) return;
      const visible = document.visibilityState !== "hidden";
      if (visible && lastHostTickAt > 0 && now - lastHostTickAt < HOST_FRAME_INTERVAL_MS - 1) {
        scheduleNext();
        return;
      }
      lastHostTickAt = now;

      if (!runtimeError) {
        try {
          const includeFrame = !hasNativeScreen && visible && presentationReady;
          const result = await runtime.tick(generationRef.current, includeFrame);
          if (!active) {
            runtime.releaseFrameView();
            return;
          }
          applyRuntimeTick(result);
        } catch (error) {
          if (active) {
            const message = error instanceof Error ? error.message : String(error);
            setRuntimeError(message);
            onSnapshot({
              phase: "error",
              message,
              fps: 0,
              usedMemory: 0,
              frameWidth: frameSize.width,
              frameHeight: frameSize.height
            });
          }
        }
      }

      scheduleNext();
    };

    scheduleNext();
    return () => {
      active = false;
      runtime.releaseFrameView();
      window.clearTimeout(timer);
      window.cancelAnimationFrame(animationFrame);
    };
  }, [applyRuntimeTick, frameSize.height, frameSize.width, hasNativeScreen, onSnapshot, presentationReady, runtime, runtimeError, sessionReady]);

  const stop = () => {
    setShowExitConfirmation(false);
    inputCoordinator.releaseAll();
    runtime.endAppSession();
    onStop();
  };

  const saveScreenshot = () => {
    setMenuAnchor(null);
    void (async () => {
      let blob = await runtime.capturePresentedFrame();
      if (!blob && mainPresenterRef.current) blob = await mainPresenterRef.current.capture();
      if (!blob) {
        const canvas = canvasRef.current;
        if (canvas) blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/png"));
      }
      if (!blob) return;
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = `${game.title.replace(/[^a-z0-9_-]+/gi, "-") || "phoneme"}.png`;
      anchor.click();
      URL.revokeObjectURL(url);
    })();
  };

  const toggleKeypad = () => {
    const next = !showKeypad;
    if (!next) inputCoordinator.releasePrefix("virtual:");
    setShowKeypad(next);
    onProfileChange({ ...profile, showVirtualKeyboard: next });
  };

  const revealAppBarTemporarily = () => {
    if (profile.showAppBar || keyboardEditMode !== "none") return;
    setIsAppBarTemporarilyVisible(true);
    if (appBarHideTimerRef.current !== null) window.clearTimeout(appBarHideTimerRef.current);
    appBarHideTimerRef.current = window.setTimeout(() => {
      appBarHideTimerRef.current = null;
      setIsAppBarTemporarilyVisible(false);
    }, 8000);
  };

  const toggleAppBar = () => {
    if (appBarHideTimerRef.current !== null) window.clearTimeout(appBarHideTimerRef.current);
    appBarHideTimerRef.current = null;
    setIsAppBarTemporarilyVisible(false);
    onProfileChange({ ...profile, showAppBar: !profile.showAppBar });
    closePlayerMenus();
  };

  const closePlayerMenus = () => {
    setMenuAnchor(null);
    setKeyboardMenuAnchor(null);
    setLayoutMenuAnchor(null);
    setTranslationMenuAnchor(null);
    setTranslationSourceMenuAnchor(null);
    setTranslationTargetMenuAnchor(null);
    if (isAppBarTemporarilyVisible) {
      window.setTimeout(() => setIsAppBarTemporarilyVisible(false), 120);
    }
  };

  const setKeyboardLayout = (layout: VirtualKeyboardType) => {
    inputCoordinator.releasePrefix("virtual:");
    onProfileChange({ ...profile, virtualKeyboardType: layout, showVirtualKeyboard: true });
    setShowKeypad(true);
    closePlayerMenus();
  };

  const toggleRotationLock = () => {
    const next = !profile.rotationLocked;
    onProfileChange({ ...profile, rotationLocked: next });
    const orientation = screen.orientation as ScreenOrientation & {
      lock?: (orientation: "portrait" | "landscape") => Promise<void>;
      unlock?: () => void;
    };
    if (next && orientation.lock) {
      const target = orientation.type.startsWith("landscape") ? "landscape" : "portrait";
      void orientation.lock(target).catch(() => undefined);
    } else {
      orientation.unlock?.();
    }
    closePlayerMenus();
  };

  const setTranslation = (
    enabled: boolean,
    source: TranslationSourceLanguage = profile.translationSourceLanguage,
    target: TranslationLanguage = profile.translationTargetLanguage
  ) => {
    onProfileChange({
      ...profile,
      autoTranslateEnabled: enabled,
      translationSourceLanguage: source,
      translationTargetLanguage: target
    });
    void runtime.configureTranslation(enabled, translationProvider, source, target).catch((error) => {
      setRuntimeError(error instanceof Error ? error.message : String(error));
    });
    closePlayerMenus();
  };

  const beginKeyboardEdit = (mode: Exclude<KeyboardEditMode, "none">) => {
    inputCoordinator.releasePrefix("virtual:");
    setKeyboardEditSnapshot(profile);
    setKeyboardEditMode(mode);
    setShowKeypad(true);
    onProfileChange({ ...profile, showVirtualKeyboard: true });
    closePlayerMenus();
  };

  const finishKeyboardEdit = () => {
    setKeyboardEditMode("none");
    setKeyboardEditSnapshot(null);
    scheduleKeyboardHide();
  };

  const discardKeyboardEdit = () => {
    if (keyboardEditSnapshot) onProfileChange(keyboardEditSnapshot);
    setShowKeypad(keyboardEditSnapshot?.showVirtualKeyboard ?? true);
    setKeyboardEditMode("none");
    setKeyboardEditSnapshot(null);
  };

  const resetKeyboardLayout = () => {
    inputCoordinator.releasePrefix("virtual:");
    onProfileChange({
      ...profile,
      virtualKeyboardType: "arrowsNumbers",
      showVirtualKeyboard: true,
      keyboardControlOffsets: {},
      keyboardGroupScales: {},
      hiddenKeyboardControlIds: []
    });
    setShowKeypad(true);
    setKeyboardEditMode("none");
    setKeyboardEditSnapshot(null);
    closePlayerMenus();
  };

  const toggleHiddenControl = (controlId: string, visible: boolean) => {
    setHiddenKeyDraft((current) => {
      const hidden = new Set(current);
      if (visible) hidden.delete(controlId);
      else hidden.add(controlId);
      return [...hidden];
    });
  };

  const applyHiddenControlChanges = () => {
    onProfileChange({ ...profile, hiddenKeyboardControlIds: hiddenKeyDraft, showVirtualKeyboard: true });
    setShowKeypad(true);
    setShowHiddenKeysEditor(false);
  };

  const pointerPosition = (event: React.PointerEvent<HTMLCanvasElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const width = Math.max(bounds.width, 1);
    const height = Math.max(bounds.height, 1);
    const frameWidth = Math.max(1, frameSize.width);
    const frameHeight = Math.max(1, frameSize.height);
    return {
      x: Math.max(0, Math.min(frameWidth - 1, Math.floor((event.clientX - bounds.left) * frameWidth / width))),
      y: Math.max(0, Math.min(frameHeight - 1, Math.floor((event.clientY - bounds.top) * frameHeight / height)))
    };
  };

  const appBarOccupiesLayout = profile.showAppBar || keyboardEditMode !== "none";
  const appBarVisible = appBarOccupiesLayout || isAppBarTemporarilyVisible;

  return <Box className={`emulator-root ${appBarOccupiesLayout ? "with-appbar" : "without-appbar"}`}>
    {appBarVisible ? <AppBar
      position={isAppBarTemporarilyVisible && !appBarOccupiesLayout ? "absolute" : "relative"}
      elevation={0}
      className={`emulator-appbar ${isAppBarTemporarilyVisible && !appBarOccupiesLayout ? "temporary-overlay" : ""}`}
    >
      <Toolbar>
        {keyboardEditMode !== "none" ? <>
          <Button color="inherit" className="bar-text-action" onClick={discardKeyboardEdit}>Hủy</Button>
          <Typography className="emulator-appbar-title editing-title" variant="subtitle1" noWrap>
            {keyboardEditMode === "position" ? "Di chuyển phím ảo" : "Đổi kích thước nhóm phím"}
          </Typography>
          <Button color="inherit" className="bar-text-action primary" onClick={finishKeyboardEdit}>Xong</Button>
        </> : <>
          <Typography className="emulator-appbar-title" variant="subtitle1" noWrap>{navigationTitle}</Typography>
          <Tooltip title="Bàn phím ảo">
            <span><IconButton color="inherit" disabled={hasNativeScreen} onClick={toggleKeypad}><KeyboardRounded /></IconButton></span>
          </Tooltip>
          <Tooltip title="Chụp màn hình">
            <IconButton color="inherit" onClick={saveScreenshot}><CameraAltRounded /></IconButton>
          </Tooltip>
          <IconButton className="ellipsis-circle-button" color="inherit" aria-label="Thêm" onClick={(event) => {
            if (appBarHideTimerRef.current !== null) window.clearTimeout(appBarHideTimerRef.current);
            appBarHideTimerRef.current = null;
            setMenuAnchor(event.currentTarget);
          }}><MoreHorizRounded /></IconButton>
        </>}
      </Toolbar>
    </AppBar> : null}

    {!appBarVisible ? <Box
      className="emulator-appbar-reveal-zone"
      aria-hidden="true"
      onPointerDown={(event) => {
        if (event.pointerType === "mouse" && event.button !== 0) return;
        appBarSwipeRef.current = { id: event.pointerId, x: event.clientX, y: event.clientY };
        try { event.currentTarget.setPointerCapture(event.pointerId); } catch { /* best effort */ }
      }}
      onPointerMove={(event) => {
        const start = appBarSwipeRef.current;
        if (!start || start.id !== event.pointerId) return;
        const dx = event.clientX - start.x;
        const dy = event.clientY - start.y;
        if (dy >= 32 && Math.abs(dy) > Math.abs(dx)) {
          appBarSwipeRef.current = null;
          revealAppBarTemporarily();
        }
      }}
      onPointerUp={() => { appBarSwipeRef.current = null; }}
      onPointerCancel={() => { appBarSwipeRef.current = null; }}
    /> : null}

    <Box ref={surfaceRef} className="emulator-surface">
      {runtimeError ? <Box className="emulator-error">
        <Typography variant="subtitle1">Không thể chạy ứng dụng</Typography>
        <Typography color="text.secondary">{runtimeError}</Typography>
      </Box> : !sessionReady ? <Box className="emulator-error">
        <CircularProgress size={28} />
        <Typography variant="subtitle1">Đang khởi động {game.title}</Typography>
        <Typography color="text.secondary">{runtime.ready ? "Đang tải ứng dụng…" : "Đang chờ lõi phoneME sẵn sàng…"}</Typography>
      </Box> : hasNativeScreen && activeScreen ? <NativeLcduiView runtime={runtime} screen={activeScreen} /> : <canvas
        ref={canvasRef}
        className="emulator-canvas"
        style={{
          left: displayRect.left,
          top: displayRect.top,
          width: displayRect.width,
          height: displayRect.height,
          imageRendering: profile.filtering ? "auto" : "pixelated"
        }}
        onContextMenu={(event) => event.preventDefault()}
        onPointerDown={(event) => {
          if (!profile.touchInput || activePointerRef.current) return;
          if (event.pointerType === "mouse" && event.button !== 0) return;
          event.preventDefault();
          const point = pointerPosition(event);
          activePointerRef.current = { id: event.pointerId, ...point };
          lastPointerMoveAtRef.current = performance.now();
          try {
            event.currentTarget.setPointerCapture(event.pointerId);
          } catch {
            // Pointer capture is best-effort on older Safari/iOS builds.
          }
          runtime.sendPointer(point.x, point.y, POINTER_PRESSED);
        }}
        onPointerMove={(event) => {
          const activePointer = activePointerRef.current;
          if (!profile.touchInput || !activePointer || activePointer.id !== event.pointerId) return;
          event.preventDefault();
          const point = pointerPosition(event);
          activePointer.x = point.x;
          activePointer.y = point.y;
          const now = performance.now();
          if (now - lastPointerMoveAtRef.current < POINTER_MOVE_INTERVAL_MS) return;
          lastPointerMoveAtRef.current = now;
          runtime.sendPointer(point.x, point.y, POINTER_DRAGGED);
        }}
        onPointerUp={(event) => {
          const activePointer = activePointerRef.current;
          if (!profile.touchInput || !activePointer || activePointer.id !== event.pointerId) return;
          event.preventDefault();
          const point = pointerPosition(event);
          activePointerRef.current = null;
          lastPointerMoveAtRef.current = 0;
          runtime.sendPointer(point.x, point.y, POINTER_RELEASED);
        }}
        onPointerCancel={(event) => {
          const activePointer = activePointerRef.current;
          if (!activePointer || activePointer.id !== event.pointerId) return;
          activePointerRef.current = null;
          lastPointerMoveAtRef.current = 0;
          runtime.sendPointer(activePointer.x, activePointer.y, POINTER_RELEASED);
        }}
        onLostPointerCapture={(event) => {
          const activePointer = activePointerRef.current;
          if (!activePointer || activePointer.id !== event.pointerId) return;
          activePointerRef.current = null;
          lastPointerMoveAtRef.current = 0;
          runtime.sendPointer(activePointer.x, activePointer.y, POINTER_RELEASED);
        }}
      />}

      {profile.showFPS && !hasNativeScreen ? <Box className="fps-overlay">{Math.round(fpsValue)} FPS</Box> : null}
      {showKeypad && profile.showVirtualKeyboard && !hasNativeScreen ? <VirtualKeypad
        inputCoordinator={inputCoordinator}
        profile={profile}
        surfaceWidth={surfaceSize.width}
        surfaceHeight={surfaceSize.height}
        editMode={keyboardEditMode}
        onProfileChange={onProfileChange}
        onActivity={() => {
          setShowKeypad(true);
          scheduleKeyboardHide();
        }}
      /> : null}
    </Box>

    {showsCanvasCommandBar && activeScreen ? <LCDUICommandBar screen={activeScreen} runtime={runtime} /> : null}

    <Menu className="player-menu" anchorEl={menuAnchor} open={Boolean(menuAnchor)} onClose={closePlayerMenus}>
      <MenuItem onClick={(event) => setKeyboardMenuAnchor(event.currentTarget)}>
        <KeyboardRounded fontSize="small" /><span>Bàn phím ảo</span><ArrowForwardIosRounded className="submenu-chevron" />
      </MenuItem>
      <Divider />
      <MenuItem onClick={toggleRotationLock}>
        <ScreenRotationRounded fontSize="small" />
        <span>{profile.rotationLocked ? "Mở khóa xoay màn hình" : "Khóa xoay màn hình"}</span>
      </MenuItem>
      <MenuItem onClick={(event) => setTranslationMenuAnchor(event.currentTarget)}>
        <TranslateRounded fontSize="small" /><span>Tự động dịch</span><ArrowForwardIosRounded className="submenu-chevron" />
      </MenuItem>
      <Divider />
      <MenuItem onClick={toggleAppBar}>
        {profile.showAppBar ? <VisibilityOffRounded fontSize="small" /> : <VisibilityRounded fontSize="small" />}
        <span>{profile.showAppBar ? "Ẩn App Bar" : "Hiện App Bar"}</span>
      </MenuItem>
      <MenuItem className="destructive-menu-item" onClick={() => { closePlayerMenus(); setShowExitConfirmation(true); }}>
        <PowerSettingsNewRounded fontSize="small" /><span>Thoát</span>
      </MenuItem>
    </Menu>

    <Menu className="player-submenu" anchorEl={keyboardMenuAnchor} open={Boolean(keyboardMenuAnchor)} onClose={() => setKeyboardMenuAnchor(null)} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
      <MenuItem onClick={() => beginKeyboardEdit("position")}><OpenWithRounded fontSize="small" /><span>Di chuyển phím</span></MenuItem>
      <MenuItem onClick={() => beginKeyboardEdit("size")}><AspectRatioRounded fontSize="small" /><span>Đổi kích thước nhóm phím</span></MenuItem>
      <Divider />
      <MenuItem onClick={(event) => setLayoutMenuAnchor(event.currentTarget)}>
        <KeyboardRounded fontSize="small" /><span>Chọn layout</span><ArrowForwardIosRounded className="submenu-chevron" />
      </MenuItem>
      <MenuItem onClick={() => { setHiddenKeyDraft(profile.hiddenKeyboardControlIds); closePlayerMenus(); setShowHiddenKeysEditor(true); }}><VisibilityRounded fontSize="small" /><span>Các nút hiển thị</span></MenuItem>
      <MenuItem className="destructive-menu-item" onClick={resetKeyboardLayout}>
        <RestartAltRounded fontSize="small" /><span>Đặt lại layout bàn phím</span>
      </MenuItem>
    </Menu>

    <Menu className="player-submenu" anchorEl={layoutMenuAnchor} open={Boolean(layoutMenuAnchor)} onClose={() => setLayoutMenuAnchor(null)} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
      {(["phone", "phoneArrows", "numbersArrows", "arrowsNumbers", "numbers", "arrows"] as VirtualKeyboardType[]).map((layout) => <MenuItem key={layout} onClick={() => setKeyboardLayout(layout)}>
        {profile.virtualKeyboardType === layout ? <CheckRounded fontSize="small" /> : <Box className="menu-icon-spacer" />}
        <span>{layout === "phone" ? "Phone" : layout === "phoneArrows" ? "Phone (arrows)" : layout === "numbersArrows" ? "Numbers & arrows" : layout === "arrowsNumbers" ? "Arrows & numbers" : layout === "numbers" ? "Numbers" : "Arrows"}</span>
      </MenuItem>)}
    </Menu>

    <Menu className="player-submenu" anchorEl={translationMenuAnchor} open={Boolean(translationMenuAnchor)} onClose={() => { setTranslationMenuAnchor(null); setTranslationSourceMenuAnchor(null); setTranslationTargetMenuAnchor(null); }} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
      <MenuItem onClick={() => setTranslation(false)}>
        {!profile.autoTranslateEnabled ? <CheckRounded fontSize="small" /> : <Box className="menu-icon-spacer" />}<span>Tắt</span>
      </MenuItem>
      <Divider />
      <MenuItem onClick={(event) => {
        setTranslationTargetMenuAnchor(null);
        setTranslationSourceMenuAnchor(event.currentTarget);
      }}>
        <TranslateRounded fontSize="small" />
        <span>Từ: {translationLanguageLabel(profile.translationSourceLanguage)}</span>
        <ArrowForwardIosRounded className="submenu-chevron" />
      </MenuItem>
      <MenuItem onClick={(event) => {
        setTranslationSourceMenuAnchor(null);
        setTranslationTargetMenuAnchor(event.currentTarget);
      }}>
        <TranslateRounded fontSize="small" />
        <span>Sang: {translationLanguageLabel(profile.translationTargetLanguage)}</span>
        <ArrowForwardIosRounded className="submenu-chevron" />
      </MenuItem>
    </Menu>

    <Menu className="player-submenu" anchorEl={translationSourceMenuAnchor} open={Boolean(translationSourceMenuAnchor)} onClose={() => setTranslationSourceMenuAnchor(null)} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
      {TRANSLATION_SOURCE_LANGUAGES.map((language) => <MenuItem key={language.value} onClick={() => setTranslation(true, language.value, profile.translationTargetLanguage)}>
        {profile.autoTranslateEnabled && profile.translationSourceLanguage === language.value ? <CheckRounded fontSize="small" /> : <Box className="menu-icon-spacer" />}
        <span>{language.label}</span>
      </MenuItem>)}
    </Menu>

    <Menu className="player-submenu" anchorEl={translationTargetMenuAnchor} open={Boolean(translationTargetMenuAnchor)} onClose={() => setTranslationTargetMenuAnchor(null)} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
      {TRANSLATION_LANGUAGES.map((language) => <MenuItem key={language.value} onClick={() => setTranslation(true, profile.translationSourceLanguage, language.value)}>
        {profile.autoTranslateEnabled && profile.translationTargetLanguage === language.value ? <CheckRounded fontSize="small" /> : <Box className="menu-icon-spacer" />}
        <span>{language.label}</span>
      </MenuItem>)}
    </Menu>

    <Dialog open={showHiddenKeysEditor} onClose={() => setShowHiddenKeysEditor(false)} fullWidth maxWidth="xs">
      <DialogTitle>Các nút ảo</DialogTitle>
      <DialogContent className="visible-buttons-dialog">
        {keyboardDefinition(profile.virtualKeyboardType).controls.map((control) => <FormControlLabel
          key={control.id}
          className="visible-button-row"
          labelPlacement="start"
          label={control.label === "L" ? "Phím mềm trái" : control.label === "R" ? "Phím mềm phải" : control.label === "F" ? "Fire / OK" : control.label}
          control={<Switch
            checked={!hiddenKeyDraft.includes(control.id)}
            onChange={(_, checked) => toggleHiddenControl(control.id, checked)}
          />}
        />)}
      </DialogContent>
      <DialogActions>
        <Button onClick={() => setShowHiddenKeysEditor(false)}>Hủy</Button>
        <Button onClick={applyHiddenControlChanges}>Xong</Button>
      </DialogActions>
    </Dialog>

    <Dialog open={showExitConfirmation} onClose={() => setShowExitConfirmation(false)}>
      <DialogTitle>Thoát {game.title}?</DialogTitle>
      <DialogContent><DialogContentText>Tiến trình chưa lưu có thể bị mất.</DialogContentText></DialogContent>
      <DialogActions>
        <Button onClick={() => setShowExitConfirmation(false)}>Hủy</Button>
        <Button color="error" onClick={stop}>Thoát</Button>
      </DialogActions>
    </Dialog>
  </Box>;
}

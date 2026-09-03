export type ScreenGravity = "left" | "top" | "center" | "right" | "bottom";
export type ScaleType = "asIs" | "fit" | "fill";
export type KeyLayout = "nokiaSE" | "siemens" | "motorola" | "custom";
export type VirtualKeyboardType = "phone" | "phoneArrows" | "numbersArrows" | "arrowsNumbers" | "numbers" | "arrows";
export type ButtonShape = "oval" | "rectangle" | "roundedRectangle";
export type TranslationLanguage = "vi" | "zh-CN" | "zh-TW" | "ja" | "ko" | "en" | "ru" | "th" | "id" | "es" | "pt" | "fr" | "de";
export type TranslationSourceLanguage = "auto" | TranslationLanguage;
export type KeyboardControlOffset = { x: number; y: number };
export type KeyboardGroupScale = { width: number; height: number };

export type WebGameProfile = {
  screenWidth: number;
  screenHeight: number;
  preserveAspectRatio: boolean;
  scalePercent: number;
  screenGravity: ScreenGravity;
  scaleType: ScaleType;
  filtering: boolean;
  forceFullscreen: boolean;
  showFPS: boolean;
  showAppBar: boolean;
  showStatusBar: boolean;
  frameRateOverride: boolean;
  frameRateLimit: number;
  rotationLocked: boolean;
  autoTranslateEnabled: boolean;
  translationSourceLanguage: TranslationSourceLanguage;
  translationTargetLanguage: TranslationLanguage;
  heapSizeMegabytes: number;
  fontSmall: number;
  fontMedium: number;
  fontLarge: number;
  fontValuesAreScaledPixels: boolean;
  touchInput: boolean;
  keyLayout: KeyLayout;
  showVirtualKeyboard: boolean;
  virtualKeyboardType: VirtualKeyboardType;
  buttonShape: ButtonShape;
  hapticFeedback: boolean;
  keyboardOpacity: number;
  forceOpacityForOffscreenKeys: boolean;
  keyboardHideDelayMilliseconds: number;
  keyboardControlOffsets: Record<string, KeyboardControlOffset>;
  keyboardGroupScales: Record<string, KeyboardGroupScale>;
  hiddenKeyboardControlIds: string[];
};

export const DEFAULT_GAME_PROFILE: WebGameProfile = {
  screenWidth: 240,
  screenHeight: 320,
  preserveAspectRatio: true,
  scalePercent: 100,
  screenGravity: "top",
  scaleType: "fit",
  filtering: false,
  forceFullscreen: false,
  showFPS: false,
  showAppBar: true,
  showStatusBar: true,
  frameRateOverride: false,
  frameRateLimit: 30,
  rotationLocked: false,
  autoTranslateEnabled: false,
  translationSourceLanguage: "auto",
  translationTargetLanguage: "vi",
  heapSizeMegabytes: 128,
  fontSmall: 18,
  fontMedium: 22,
  fontLarge: 26,
  fontValuesAreScaledPixels: false,
  touchInput: true,
  keyLayout: "nokiaSE",
  showVirtualKeyboard: true,
  virtualKeyboardType: "arrowsNumbers",
  buttonShape: "roundedRectangle",
  hapticFeedback: false,
  keyboardOpacity: 0.20,
  forceOpacityForOffscreenKeys: false,
  keyboardHideDelayMilliseconds: 0,
  keyboardControlOffsets: {},
  keyboardGroupScales: {},
  hiddenKeyboardControlIds: []
};

export function normalizeGameProfile(value?: Partial<WebGameProfile> | null): WebGameProfile {
  const profile = { ...DEFAULT_GAME_PROFILE, ...(value ?? {}) };
  profile.screenWidth = Math.min(2_048, Math.max(1, Math.round(profile.screenWidth)));
  profile.screenHeight = Math.min(2_048, Math.max(1, Math.round(profile.screenHeight)));
  profile.scalePercent = Math.min(300, Math.max(10, Math.round(profile.scalePercent)));
  // Older web builds forcibly rewrote every profile to 30 FPS override during
  // normalization, so persisted profiles cannot represent an intentional
  // choice here. Migrate that legacy forced pair back to native pacing. Newer
  // profiles preserve explicit overrides and clamp them to the web display
  // ceiling.
  const legacyForcedThirtyFps = value?.frameRateOverride === true
    && Number(value.frameRateLimit) === 30;
  profile.frameRateOverride = legacyForcedThirtyFps
    ? false
    : Boolean(profile.frameRateOverride);
  profile.frameRateLimit = Math.min(60, Math.max(1, Math.round(profile.frameRateLimit || 30)));
  profile.heapSizeMegabytes = Math.min(192, Math.max(1, Math.round(profile.heapSizeMegabytes)));
  profile.fontSmall = Math.max(1, Math.round(profile.fontSmall));
  profile.fontMedium = Math.max(1, Math.round(profile.fontMedium));
  profile.fontLarge = Math.max(1, Math.round(profile.fontLarge));
  profile.keyboardOpacity = Math.min(1, Math.max(0.05, profile.keyboardOpacity));
  profile.keyboardHideDelayMilliseconds = Math.min(60_000, Math.max(0, Math.round(profile.keyboardHideDelayMilliseconds)));
  profile.keyboardControlOffsets = { ...(profile.keyboardControlOffsets ?? {}) };
  profile.keyboardGroupScales = { ...(profile.keyboardGroupScales ?? {}) };
  profile.hiddenKeyboardControlIds = [...(profile.hiddenKeyboardControlIds ?? [])];
  return profile;
}

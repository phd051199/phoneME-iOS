import Foundation

enum TranslationProvider: String, CaseIterable, Identifiable {
    case automatic
    case google
    case bing

    static let preferenceKey = "translationProvider"
    private static let automaticMigrationKey =
        "translationProviderAutomaticMigrationV1"
    static let defaultProvider = TranslationProvider.bing

    var id: String { rawValue }

    var title: String {
        switch self {
        case .automatic: return L10n.string("Automatic")
        case .google: return "Google Translate"
        case .bing: return "Bing Translator"
        }
    }

    var systemImage: String {
        switch self {
        case .automatic: return "arrow.triangle.2.circlepath"
        case .google: return "g.circle"
        case .bing: return "b.circle"
        }
    }

    var coreValue: Int32 {
        switch self {
        case .google: return 0
        case .bing: return 1
        case .automatic: return 2
        }
    }

    static var selected: TranslationProvider {
        let defaults = UserDefaults.standard
        if !defaults.bool(forKey: automaticMigrationKey) {
            defaults.set(true, forKey: automaticMigrationKey)
            defaults.set(defaultProvider.rawValue, forKey: preferenceKey)
            return defaultProvider
        }
        let stored = defaults.string(forKey: preferenceKey)
        return stored.flatMap(TranslationProvider.init(rawValue:)) ?? defaultProvider
    }
}

enum TranslationSourceLanguage: String, Codable, CaseIterable, Identifiable {
    case auto
    case vietnamese = "vi"
    case simplifiedChinese = "zh-CN"
    case traditionalChinese = "zh-TW"
    case japanese = "ja"
    case korean = "ko"
    case english = "en"
    case russian = "ru"
    case thai = "th"
    case indonesian = "id"
    case spanish = "es"
    case portuguese = "pt"
    case french = "fr"
    case german = "de"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .auto: return L10n.string("Auto detect")
        case .vietnamese: return L10n.string("Vietnamese")
        case .simplifiedChinese: return L10n.string("Chinese (Simplified)")
        case .traditionalChinese: return L10n.string("Chinese (Traditional)")
        case .japanese: return L10n.string("Japanese")
        case .korean: return L10n.string("Korean")
        case .english: return L10n.string("English")
        case .russian: return L10n.string("Russian")
        case .thai: return L10n.string("Thai")
        case .indonesian: return L10n.string("Indonesian")
        case .spanish: return L10n.string("Spanish")
        case .portuguese: return L10n.string("Portuguese")
        case .french: return L10n.string("French")
        case .german: return L10n.string("German")
        }
    }

    var systemImage: String {
        switch self {
        case .auto: return "wand.and.stars"
        case .simplifiedChinese, .traditionalChinese, .japanese, .korean:
            return "character"
        case .vietnamese, .english, .russian, .thai, .indonesian, .spanish,
             .portuguese, .french, .german:
            return "textformat.abc"
        }
    }
}

enum TranslationTargetLanguage: String, Codable, CaseIterable, Identifiable {
    case vietnamese = "vi"
    case simplifiedChinese = "zh-CN"
    case traditionalChinese = "zh-TW"
    case japanese = "ja"
    case korean = "ko"
    case english = "en"
    case russian = "ru"
    case thai = "th"
    case indonesian = "id"
    case spanish = "es"
    case portuguese = "pt"
    case french = "fr"
    case german = "de"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .vietnamese: return L10n.string("Vietnamese")
        case .simplifiedChinese: return L10n.string("Chinese (Simplified)")
        case .traditionalChinese: return L10n.string("Chinese (Traditional)")
        case .japanese: return L10n.string("Japanese")
        case .korean: return L10n.string("Korean")
        case .english: return L10n.string("English")
        case .russian: return L10n.string("Russian")
        case .thai: return L10n.string("Thai")
        case .indonesian: return L10n.string("Indonesian")
        case .spanish: return L10n.string("Spanish")
        case .portuguese: return L10n.string("Portuguese")
        case .french: return L10n.string("French")
        case .german: return L10n.string("German")
        }
    }

    var systemImage: String {
        switch self {
        case .simplifiedChinese, .traditionalChinese, .japanese, .korean:
            return "character"
        case .vietnamese, .english, .russian, .thai, .indonesian, .spanish,
             .portuguese, .french, .german:
            return "textformat.abc"
        }
    }
}

struct GameProfile: Codable, Equatable {
    enum Orientation: String, Codable, CaseIterable, Identifiable {
        case defaultValue
        case auto
        case portrait
        case landscape

        var id: String { rawValue }
        var title: String {
            switch self {
            case .defaultValue: return L10n.string("Default")
            case .auto: return L10n.string("Auto")
            case .portrait: return L10n.string("Portrait")
            case .landscape: return L10n.string("Landscape")
            }
        }
        var systemImage: String {
            switch self {
            case .defaultValue: return "rectangle"
            case .auto: return "arrow.triangle.2.circlepath"
            case .portrait: return "rectangle.portrait"
            case .landscape: return "rectangle"
            }
        }
    }

    enum ScreenGravity: String, Codable, CaseIterable, Identifiable {
        case left
        case top
        case center
        case right
        case bottom

        var id: String { rawValue }
        var title: String {
            switch self {
            case .left: return L10n.string("Left")
            case .top: return L10n.string("Top")
            case .center: return L10n.string("Center")
            case .right: return L10n.string("Right")
            case .bottom: return L10n.string("Bottom")
            }
        }
        var systemImage: String {
            switch self {
            case .left: return "arrow.left"
            case .top: return "arrow.up"
            case .center: return "scope"
            case .right: return "arrow.right"
            case .bottom: return "arrow.down"
            }
        }
    }

    enum ScaleType: String, Codable, CaseIterable, Identifiable {
        case asIs
        case fit
        case fill

        var id: String { rawValue }
        var title: String {
            switch self {
            case .asIs: return L10n.string("As is")
            case .fit: return L10n.string("Fit to window")
            case .fill: return L10n.string("Fill window (ignore aspect ratio)")
            }
        }
        var systemImage: String {
            switch self {
            case .asIs: return "square"
            case .fit: return "arrow.down.right.and.arrow.up.left"
            case .fill: return "arrow.up.left.and.arrow.down.right"
            }
        }
    }

    enum FramePacingMode: String, Codable, CaseIterable, Identifiable {
        case native
        case cap
        case overrideGameLoop

        var id: String { rawValue }
        var title: String {
            switch self {
            case .native: return L10n.string("Native game timing")
            case .cap: return L10n.string("Limit only")
            case .overrideGameLoop:
                return L10n.string("Override game loop")
            }
        }

        var systemImage: String {
            switch self {
            case .native: return "clock"
            case .cap: return "speedometer"
            case .overrideGameLoop: return "timer"
            }
        }

        var coreValue: Int32 {
            switch self {
            case .native: return 0
            case .cap: return 1
            case .overrideGameLoop: return 2
            }
        }
    }

    enum KeyLayout: String, Codable, CaseIterable, Identifiable {
        case nokiaSE
        case siemens
        case motorola
        case custom

        var id: String { rawValue }
        var title: String {
            switch self {
            case .nokiaSE: return "Nokia/SE"
            case .siemens: return "Siemens"
            case .motorola: return "Motorola"
            case .custom: return L10n.string("Custom")
            }
        }
        var systemImage: String {
            switch self {
            case .nokiaSE, .siemens, .motorola: return "phone"
            case .custom: return "slider.horizontal.3"
            }
        }
    }

    enum VirtualKeyboardType: String, Codable, CaseIterable, Identifiable {
        case custom
        case phone
        case phoneArrows
        case numbersArrows
        case arrowsNumbers
        case numbers
        case arrows

        var id: String { rawValue }
        var title: String {
            switch self {
            case .custom: return L10n.string("Custom")
            case .phone: return L10n.string("Phone")
            case .phoneArrows: return L10n.string("Phone (arrows)")
            case .numbersArrows: return L10n.string("Numbers & arrows")
            case .arrowsNumbers: return L10n.string("Arrows & numbers")
            case .numbers: return L10n.string("Numbers")
            case .arrows: return L10n.string("Arrows")
            }
        }
        var systemImage: String {
            switch self {
            case .custom: return "slider.horizontal.3"
            case .phone, .phoneArrows: return "phone"
            case .numbersArrows, .numbers: return "number"
            case .arrowsNumbers, .arrows:
                return "arrow.up.and.down.and.arrow.left.and.right"
            }
        }
    }

    enum ButtonShape: String, Codable, CaseIterable, Identifiable {
        case oval
        case rectangle
        case roundedRectangle

        var id: String { rawValue }
        var title: String {
            switch self {
            case .oval: return L10n.string("Oval")
            case .rectangle: return L10n.string("Rectangle")
            case .roundedRectangle: return L10n.string("Rounded rectangle")
            }
        }
        var systemImage: String {
            switch self {
            case .oval: return "capsule"
            case .rectangle: return "rectangle"
            case .roundedRectangle: return "rectangle.fill"
            }
        }
    }

    struct KeyboardTuning: Codable, Equatable {
        var scale = 1.0
        var spacingScale = 1.0
        var horizontalOffset = 0.0
        var verticalOffset = 0.0

        static let `default` = KeyboardTuning()

        var isDefault: Bool {
            abs(scale - 1.0) < 0.001
                && abs(spacingScale - 1.0) < 0.001
                && abs(horizontalOffset) < 0.001
                && abs(verticalOffset) < 0.001
        }
    }

    struct KeyboardControlOffset: Codable, Equatable {
        var x = 0.0
        var y = 0.0

        var isDefault: Bool {
            abs(x) < 0.0001 && abs(y) < 0.0001
        }
    }

    struct KeyboardGroupScale: Codable, Equatable {
        var width = 1.0
        var height = 1.0

        var isDefault: Bool {
            abs(width - 1.0) < 0.0001 && abs(height - 1.0) < 0.0001
        }
    }

    struct KeyboardLayoutCustomization: Codable, Equatable {
        var controlOffsets: [String: KeyboardControlOffset] = [:]
        var groupScales: [String: KeyboardGroupScale] = [:]
        var hiddenControlIDs: Set<String> = []

        var isDefault: Bool {
            controlOffsets.values.allSatisfy(\.isDefault)
                && groupScales.values.allSatisfy(\.isDefault)
                && hiddenControlIDs.isEmpty
        }
    }

    var screenWidth = 240
    var screenHeight = 320
    var preserveAspectRatio = true
    var scalePercent = 100
    var orientation: Orientation = .defaultValue
    var lockedOrientation: Orientation?
    var screenGravity: ScreenGravity = .top
    var scaleType: ScaleType = .fit
    var filtering = false
    var forceFullscreen = false
    var showFPS = false
    // Optional values keep older profile JSON decodable. Missing/nil follows
    // the app-wide chrome preference until the game gets an explicit override.
    var showAppBar: Bool?
    var showStatusBar: Bool?
    var frameRateLimit = Self.defaultFrameRate
    // Optional keeps older profile JSON decodable. Missing/nil uses the game's
    // native pacing, so FPS is not overridden unless the user enables it.
    var framePacingMode: FramePacingMode? = .native
    // Optional keeps older profile JSON decodable. Missing/nil uses the
    // runtime's 256 MiB default heap limit.
    var heapSizeMegabytes: Int?
    // Optional keeps older profile JSON decodable. Missing/nil means off.
    var autoTranslateToVietnamese: Bool?
    // Missing/nil preserves the legacy automatic source-language behavior.
    var autoTranslationSourceLanguage: TranslationSourceLanguage?
    // Missing/nil preserves the legacy behavior of translating into Vietnamese.
    var autoTranslationTargetLanguage: TranslationTargetLanguage?

    var fontSmall = 18
    var fontMedium = 22
    var fontLarge = 26
    var fontValuesAreScaledPixels = false
    var fontAntialiasing = false

    var touchInput = true
    var keyLayout: KeyLayout = .nokiaSE
    var customKeyMappings: [String: Int32]?
    var virtualKeyboardType: VirtualKeyboardType = .arrowsNumbers
    var showVirtualKeyboard = true
    var buttonShape: ButtonShape = .roundedRectangle
    var hapticFeedback = false
    var keyboardOpacity = 0.20
    var keyboardBackgroundHex = "F2F2F7"
    var keyboardForegroundHex = "1C1C1E"
    var keyboardSelectedBackgroundHex = "007AFF"
    var keyboardSelectedForegroundHex = "FFFFFF"
    var keyboardOutlineHex = "8E8E93"
    var forceOpacityForOffscreenKeys = false
    var keyboardHideDelayMilliseconds = 0
    var keyboardTuning: KeyboardTuning?
    var keyboardCustomBaseType: VirtualKeyboardType?
    var keyboardLayoutCustomization: KeyboardLayoutCustomization?

    static let defaultFrameRate = 30
    static let maximumFrameRate = 60
    static let defaultHeapSizeMegabytes = 128
    static let minimumHeapSizeMegabytes = 1
    static let maximumHeapSizeMegabytes = 512
    static let `default` = GameProfile()

    static func resolvedFrameRate(_ configuredValue: Int) -> Int {
        guard configuredValue > 0 else { return defaultFrameRate }
        return min(max(configuredValue, 1), maximumFrameRate)
    }

    static func resolvedFrameRate(
        _ configuredValue: Int,
        mode: FramePacingMode
    ) -> Int {
        // Native means preserve the MIDlet's own timing. The scheduler still
        // needs a ceiling for runaway repaint loops, but using the 30 FPS
        // override default here silently turned "Default / native" into a
        // 30 FPS cap on physical iPhone. Keep native timing up to the display
        // ceiling; explicit 30/60 choices continue to use overrideGameLoop.
        mode == .native
            ? maximumFrameRate
            : resolvedFrameRate(configuredValue)
    }

    var effectiveFramePacingMode: FramePacingMode {
        get {
            switch framePacingMode {
            case .native, nil:
                return .native
            case .cap, .overrideGameLoop:
                // `.cap` was written by a short-lived build that mislabeled
                // the FPS override as a publication limit. Treat it as the
                // intended render-loop override when loading old profiles.
                return .overrideGameLoop
            }
        }
        set {
            framePacingMode = newValue == .native ? .native : .overrideGameLoop
        }
    }

    var isFrameRateOverrideEnabled: Bool {
        get { effectiveFramePacingMode == .overrideGameLoop }
        set {
            effectiveFramePacingMode = newValue ? .overrideGameLoop : .native
            if newValue, frameRateLimit <= 0 {
                frameRateLimit = Self.defaultFrameRate
            }
        }
    }

    var effectiveHeapSizeMegabytes: Int {
        get {
            Self.resolvedHeapSizeMegabytes(heapSizeMegabytes)
        }
        set {
            let resolved = Self.resolvedHeapSizeMegabytes(newValue)
            heapSizeMegabytes = resolved == Self.defaultHeapSizeMegabytes
                ? nil
                : resolved
        }
    }

    static func resolvedHeapSizeMegabytes(_ configuredValue: Int?) -> Int {
        let value = configuredValue ?? defaultHeapSizeMegabytes
        return min(
            max(value, minimumHeapSizeMegabytes),
            maximumHeapSizeMegabytes
        )
    }

    static func resolvedHeapSizeMegabytes(_ configuredValue: Int) -> Int {
        resolvedHeapSizeMegabytes(Optional(configuredValue))
    }

    var isAutoTranslationEnabled: Bool {
        autoTranslateToVietnamese ?? false
    }

    mutating func setAutoTranslationEnabled(_ enabled: Bool) {
        autoTranslateToVietnamese = enabled ? true : nil
    }

    var effectiveAutoTranslationSourceLanguage: TranslationSourceLanguage {
        get { autoTranslationSourceLanguage ?? .auto }
        set { autoTranslationSourceLanguage = newValue == .auto ? nil : newValue }
    }

    var effectiveAutoTranslationTargetLanguage: TranslationTargetLanguage {
        get { autoTranslationTargetLanguage ?? .vietnamese }
        set {
            autoTranslationTargetLanguage = newValue == .vietnamese ? nil : newValue
        }
    }

    mutating func normalize() {
        screenWidth = min(max(screenWidth, 1), 2048)
        screenHeight = min(max(screenHeight, 1), 2048)
        scalePercent = min(max(scalePercent, 10), 300)
        frameRateLimit = min(
            max(frameRateLimit, 0),
            Self.maximumFrameRate
        )
        if framePacingMode == .cap {
            framePacingMode = .overrideGameLoop
        }
        if let heapSizeMegabytes {
            let resolved = Self.resolvedHeapSizeMegabytes(heapSizeMegabytes)
            self.heapSizeMegabytes = resolved == Self.defaultHeapSizeMegabytes
                ? nil
                : resolved
        }
        fontSmall = min(max(fontSmall, 1), 128)
        fontMedium = min(max(fontMedium, 1), 128)
        fontLarge = min(max(fontLarge, 1), 128)
        keyboardOpacity = min(max(keyboardOpacity, 0.05), 1)
        keyboardHideDelayMilliseconds = min(
            max(keyboardHideDelayMilliseconds, 0),
            60_000
        )
        keyboardBackgroundHex = Self.normalizedHex(
            keyboardBackgroundHex,
            fallback: "F2F2F7"
        )
        keyboardForegroundHex = Self.normalizedHex(
            keyboardForegroundHex,
            fallback: "1C1C1E"
        )
        keyboardSelectedBackgroundHex = Self.normalizedHex(
            keyboardSelectedBackgroundHex,
            fallback: "007AFF"
        )
        keyboardSelectedForegroundHex = Self.normalizedHex(
            keyboardSelectedForegroundHex,
            fallback: "FFFFFF"
        )
        keyboardOutlineHex = Self.normalizedHex(
            keyboardOutlineHex,
            fallback: "8E8E93"
        )
    }

    func normalized() -> GameProfile {
        var result = self
        result.normalize()
        return result
    }

    private static func normalizedHex(_ value: String, fallback: String) -> String {
        let cleaned = value
            .trimmingCharacters(in: CharacterSet.alphanumerics.inverted)
            .uppercased()
        guard cleaned.count == 6, UInt64(cleaned, radix: 16) != nil else {
            return fallback
        }
        return cleaned
    }

    func keyCode(for key: J2MEKey) -> Int32 {
        guard J2MEKey.configurableKeys.contains(key) else {
            return key.rawValue
        }

        if keyLayout == .custom,
           let customValue = customKeyMappings?[key.mappingID],
           customValue != 0 {
            return customValue
        }

        switch (keyLayout) {
        case .nokiaSE, .custom:
            return key.rawValue
        case .siemens:
            switch key {
            case .up: return -59
            case .down: return -60
            case .left: return -61
            case .right: return -62
            case .softLeft: return -1
            case .softRight: return -4
            default: return key.rawValue
            }
        case .motorola:
            switch key {
            case .up: return -1
            case .down: return -6
            case .left: return -2
            case .right: return -5
            case .fire: return -20
            case .softLeft: return -21
            case .softRight: return -22
            default: return key.rawValue
            }
        }
    }

    mutating func setCustomKeyCode(_ value: Int32, for key: J2MEKey) {
        guard J2MEKey.configurableKeys.contains(key), value != 0 else { return }
        var mappings = customKeyMappings ?? [:]
        mappings[key.mappingID] = value
        customKeyMappings = mappings
    }

    mutating func resetCustomKeyMappings() {
        customKeyMappings = nil
    }

    var effectiveKeyboardTuning: KeyboardTuning {
        keyboardTuning ?? .default
    }

    mutating func updateKeyboardTuning(_ update: (inout KeyboardTuning) -> Void) {
        var tuning = effectiveKeyboardTuning
        update(&tuning)
        keyboardTuning = tuning.isDefault ? nil : tuning
    }

    var resolvedKeyboardBaseType: VirtualKeyboardType {
        guard virtualKeyboardType == .custom else { return virtualKeyboardType }
        let base = keyboardCustomBaseType ?? .arrowsNumbers
        return base == .custom ? .arrowsNumbers : base
    }

    var effectiveKeyboardLayoutCustomization: KeyboardLayoutCustomization {
        guard virtualKeyboardType == .custom else {
            return KeyboardLayoutCustomization()
        }
        return keyboardLayoutCustomization ?? KeyboardLayoutCustomization()
    }

    mutating func updateKeyboardLayoutCustomization(
        _ update: (inout KeyboardLayoutCustomization) -> Void
    ) {
        var customization = effectiveKeyboardLayoutCustomization
        update(&customization)
        keyboardLayoutCustomization = customization.isDefault ? nil : customization
    }

    mutating func resetKeyboardLayoutCustomization() {
        keyboardTuning = nil
        keyboardCustomBaseType = nil
        keyboardLayoutCustomization = nil
        if virtualKeyboardType == .custom {
            virtualKeyboardType = .arrowsNumbers
        }
    }

    mutating func makeKeyboardLayoutCustom(baseType: VirtualKeyboardType? = nil) {
        let resolvedBase = baseType ?? resolvedKeyboardBaseType
        keyboardCustomBaseType = resolvedBase == .custom ? .arrowsNumbers : resolvedBase
        virtualKeyboardType = .custom
    }

    var usesNativeKeyboardPalette: Bool {
        keyboardBackgroundHex.caseInsensitiveCompare("F2F2F7") == .orderedSame
            && keyboardForegroundHex.caseInsensitiveCompare("1C1C1E") == .orderedSame
            && keyboardSelectedBackgroundHex.caseInsensitiveCompare("007AFF") == .orderedSame
            && keyboardSelectedForegroundHex.caseInsensitiveCompare("FFFFFF") == .orderedSame
            && keyboardOutlineHex.caseInsensitiveCompare("8E8E93") == .orderedSame
    }

    @discardableResult
    mutating func migrateLegacyKeyboardPaletteIfNeeded() -> Bool {
        let usesLegacyPalette = keyboardBackgroundHex.caseInsensitiveCompare("D0D0D0") == .orderedSame
            && keyboardForegroundHex.caseInsensitiveCompare("000080") == .orderedSame
            && keyboardSelectedBackgroundHex.caseInsensitiveCompare("000080") == .orderedSame
            && keyboardSelectedForegroundHex.caseInsensitiveCompare("FFFFFF") == .orderedSame
            && keyboardOutlineHex.caseInsensitiveCompare("FFFFFF") == .orderedSame
            && abs(keyboardOpacity - (64.0 / 255.0)) < 0.001

        guard usesLegacyPalette else { return false }
        keyboardOpacity = 0.20
        keyboardBackgroundHex = "F2F2F7"
        keyboardForegroundHex = "1C1C1E"
        keyboardSelectedBackgroundHex = "007AFF"
        keyboardSelectedForegroundHex = "FFFFFF"
        keyboardOutlineHex = "8E8E93"
        return true
    }

    @discardableResult
    mutating func migrateNativeInputDefaultsIfNeeded() -> Bool {
        var changed = false

        if abs(keyboardOpacity - 0.78) < 0.001 {
            keyboardOpacity = 0.20
            changed = true
        }
        if fontAntialiasing {
            fontAntialiasing = false
            changed = true
        }
        if !usesNativeKeyboardPalette {
            keyboardBackgroundHex = "F2F2F7"
            keyboardForegroundHex = "1C1C1E"
            keyboardSelectedBackgroundHex = "007AFF"
            keyboardSelectedForegroundHex = "FFFFFF"
            keyboardOutlineHex = "8E8E93"
            changed = true
        }

        return changed
    }

    static let heapSizePresets = [8, 16, 32, 64, 128, 256, 512]

    static let screenPresets: [(width: Int, height: Int)] = [
        (128, 128),
        (128, 160),
        (132, 176),
        (176, 220),
        (240, 320),
        (352, 416),
        (640, 360),
        (800, 480)
    ]
}

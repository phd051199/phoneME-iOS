import Foundation

enum AppLanguage: String, CaseIterable, Identifiable {
    case vietnamese = "vi"
    case english = "en"
    case chineseSimplified = "zh-Hans"

    static let preferenceKey = "appLanguage"
    static let defaultLanguage: AppLanguage = .vietnamese

    var id: String { rawValue }

    var locale: Locale {
        Locale(identifier: rawValue)
    }

    /// Human-readable language name written in that language itself.
    /// Keep these values independent from the currently selected app locale so
    /// users can always recognize the language they want to switch to.
    var nativeTitle: String {
        switch self {
        case .vietnamese:
            return "Tiếng Việt"
        case .english:
            return "English"
        case .chineseSimplified:
            return "简体中文"
        }
    }

    var systemImage: String {
        switch self {
        case .vietnamese: return "globe.asia.australia.fill"
        case .english: return "textformat.abc"
        case .chineseSimplified: return "character.book.closed.fill"
        }
    }

    static var selected: AppLanguage {
        guard let rawValue = UserDefaults.standard.string(forKey: preferenceKey),
              let language = AppLanguage(rawValue: rawValue) else {
            return defaultLanguage
        }
        return language
    }
}

enum L10n {
    static var locale: Locale {
        AppLanguage.selected.locale
    }

    private static var bundle: Bundle {
        guard let path = Bundle.main.path(
            forResource: AppLanguage.selected.rawValue,
            ofType: "lproj"
        ), let bundle = Bundle(path: path) else {
            return .main
        }
        return bundle
    }

    static func string(_ key: String) -> String {
        bundle.localizedString(forKey: key, value: key, table: nil)
    }

    static func format(_ key: String, _ arguments: CVarArg...) -> String {
        String(
            format: string(key),
            locale: locale,
            arguments: arguments
        )
    }
}

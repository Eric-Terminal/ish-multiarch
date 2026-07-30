import Foundation

enum WatchTerminalThemePaletteMode: String, Codable, Equatable, Sendable {
    case single
    case adaptive
}

enum WatchTerminalThemeColorScheme: Equatable, Sendable {
    case light
    case dark

    init(
        preference: String,
        environmentIsDark: Bool
    ) {
        switch preference {
        case "light":
            self = .light
        case "dark":
            self = .dark
        default:
            self = environmentIsDark ? .dark : .light
        }
    }
}

struct WatchTerminalThemePalette: Codable, Equatable, Sendable {
    var foreground: WatchTerminalRGB
    var background: WatchTerminalRGB
    var cursor: WatchTerminalRGB
    var ansiColors: [WatchTerminalRGB]

    init(
        foreground: WatchTerminalRGB,
        background: WatchTerminalRGB,
        cursor: WatchTerminalRGB,
        ansiColors: [WatchTerminalRGB]
    ) {
        precondition(
            ansiColors.count == 16,
            "终端主题调色板必须包含 16 个 ANSI 基础色")
        self.foreground = foreground
        self.background = background
        self.cursor = cursor
        self.ansiColors = ansiColors
    }

    init(_ palette: WatchTerminalPalette) {
        self.init(
            foreground: palette.foreground,
            background: palette.background,
            cursor: palette.cursor,
            ansiColors: palette.ansi16Colors)
    }

    func resolved(
        id: WatchTerminalPaletteID
    ) -> WatchTerminalPalette {
        WatchTerminalPalette(
            id: id,
            foreground: foreground,
            background: background,
            cursor: cursor,
            ansiColors: ansiColors)
    }
}

struct WatchCustomTerminalTheme: Codable, Equatable, Identifiable, Sendable {
    static let maximumNameLength = 40

    let id: UUID
    var name: String
    var basePaletteID: WatchTerminalPaletteID
    var paletteMode: WatchTerminalThemePaletteMode
    var lightPalette: WatchTerminalThemePalette
    var darkPalette: WatchTerminalThemePalette
    var lightOverride: Bool
    var darkOverride: Bool

    init(
        id: UUID = UUID(),
        name: String,
        basePaletteID: WatchTerminalPaletteID,
        foreground: WatchTerminalRGB,
        background: WatchTerminalRGB,
        cursor: WatchTerminalRGB,
        ansiColors: [WatchTerminalRGB],
        lightOverride: Bool = false,
        darkOverride: Bool = false
    ) {
        let palette = WatchTerminalThemePalette(
            foreground: foreground,
            background: background,
            cursor: cursor,
            ansiColors: ansiColors)
        self.init(
            id: id,
            name: name,
            basePaletteID: basePaletteID,
            paletteMode: .single,
            lightPalette: palette,
            darkPalette: palette,
            lightOverride: lightOverride,
            darkOverride: darkOverride)
    }

    init(
        id: UUID = UUID(),
        name: String,
        basePaletteID: WatchTerminalPaletteID,
        paletteMode: WatchTerminalThemePaletteMode,
        lightPalette: WatchTerminalThemePalette,
        darkPalette: WatchTerminalThemePalette,
        lightOverride: Bool = false,
        darkOverride: Bool = false
    ) {
        precondition(
            lightPalette.ansiColors.count == 16 &&
                darkPalette.ansiColors.count == 16,
            "自定义终端主题的每套配色必须包含 16 个 ANSI 基础色")
        self.id = id
        self.name = name
        self.basePaletteID = basePaletteID
        self.paletteMode = paletteMode
        self.lightPalette = lightPalette
        self.darkPalette =
            paletteMode == .single ? lightPalette : darkPalette
        self.lightOverride = lightOverride
        self.darkOverride = darkOverride
    }

    var selectionID: String {
        WatchTerminalThemeStore.customSelectionPrefix +
            id.uuidString.lowercased()
    }

    var foreground: WatchTerminalRGB {
        get { lightPalette.foreground }
        set {
            var palette = lightPalette
            palette.foreground = newValue
            setPalette(palette, for: .light)
        }
    }

    var background: WatchTerminalRGB {
        get { lightPalette.background }
        set {
            var palette = lightPalette
            palette.background = newValue
            setPalette(palette, for: .light)
        }
    }

    var cursor: WatchTerminalRGB {
        get { lightPalette.cursor }
        set {
            var palette = lightPalette
            palette.cursor = newValue
            setPalette(palette, for: .light)
        }
    }

    var ansiColors: [WatchTerminalRGB] {
        get { lightPalette.ansiColors }
        set {
            var palette = lightPalette
            palette.ansiColors = newValue
            setPalette(palette, for: .light)
        }
    }

    var palette: WatchTerminalPalette {
        palette(for: .light)
    }

    func palette(
        for colorScheme: WatchTerminalThemeColorScheme
    ) -> WatchTerminalPalette {
        let storedPalette =
            paletteMode == .adaptive && colorScheme == .dark ?
            darkPalette : lightPalette
        return storedPalette.resolved(id: basePaletteID)
    }

    mutating func setPalette(
        _ palette: WatchTerminalThemePalette,
        for colorScheme: WatchTerminalThemeColorScheme
    ) {
        switch colorScheme {
        case .light:
            lightPalette = palette
            if paletteMode == .single {
                darkPalette = palette
            }
        case .dark:
            if paletteMode == .adaptive {
                darkPalette = palette
            } else {
                lightPalette = palette
                darkPalette = palette
            }
        }
    }

    mutating func setPaletteMode(
        _ newMode: WatchTerminalThemePaletteMode
    ) {
        guard newMode != paletteMode else {
            return
        }
        // 两种切换方向都以浅色主配色为基准，
        // 避免留下不可见的旧深色值。
        darkPalette = lightPalette
        paletteMode = newMode
    }

    private enum CodingKeys: String, CodingKey {
        case id
        case name
        case basePaletteID
        case paletteMode
        case lightPalette
        case darkPalette
        case lightOverride
        case darkOverride
        case foreground
        case background
        case cursor
        case ansiColors
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(
            keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        basePaletteID = try container.decode(
            WatchTerminalPaletteID.self,
            forKey: .basePaletteID)
        lightOverride = try container.decodeIfPresent(
            Bool.self,
            forKey: .lightOverride) ?? false
        darkOverride = try container.decodeIfPresent(
            Bool.self,
            forKey: .darkOverride) ?? false

        if let storedMode = try container.decodeIfPresent(
            WatchTerminalThemePaletteMode.self,
            forKey: .paletteMode
        ) {
            paletteMode = storedMode
            lightPalette = try container.decode(
                WatchTerminalThemePalette.self,
                forKey: .lightPalette)
            darkPalette = try container.decode(
                WatchTerminalThemePalette.self,
                forKey: .darkPalette)
            guard lightPalette.ansiColors.count == 16,
                  darkPalette.ansiColors.count == 16 else {
                throw DecodingError.dataCorruptedError(
                    forKey: .darkPalette,
                    in: container,
                    debugDescription:
                        "每套终端主题配色必须包含 16 个 ANSI 基础色")
            }
            if paletteMode == .single {
                darkPalette = lightPalette
            }
        } else {
            let legacyANSIColors = try container.decode(
                [WatchTerminalRGB].self,
                forKey: .ansiColors)
            guard legacyANSIColors.count == 16 else {
                throw DecodingError.dataCorruptedError(
                    forKey: .ansiColors,
                    in: container,
                    debugDescription:
                        "旧终端主题必须包含 16 个 ANSI 基础色")
            }
            let legacyPalette = WatchTerminalThemePalette(
                foreground: try container.decode(
                    WatchTerminalRGB.self,
                    forKey: .foreground),
                background: try container.decode(
                    WatchTerminalRGB.self,
                    forKey: .background),
                cursor: try container.decode(
                    WatchTerminalRGB.self,
                    forKey: .cursor),
                ansiColors: legacyANSIColors)
            paletteMode = .single
            lightPalette = legacyPalette
            darkPalette = legacyPalette
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(
            keyedBy: CodingKeys.self)
        try container.encode(id, forKey: .id)
        try container.encode(name, forKey: .name)
        try container.encode(
            basePaletteID,
            forKey: .basePaletteID)
        try container.encode(paletteMode, forKey: .paletteMode)
        try container.encode(lightPalette, forKey: .lightPalette)
        try container.encode(darkPalette, forKey: .darkPalette)
        try container.encode(lightOverride, forKey: .lightOverride)
        try container.encode(darkOverride, forKey: .darkOverride)
    }
}

struct WatchTerminalThemeAppearanceResolution:
    Equatable,
    Sendable
{
    let terminalColorScheme: WatchTerminalThemeColorScheme
    let shellColorScheme: WatchTerminalThemeColorScheme

    init(
        preference: String,
        systemIsDark: Bool,
        lightOverride: Bool,
        darkOverride: Bool
    ) {
        terminalColorScheme = WatchTerminalThemeColorScheme(
            preference: preference,
            environmentIsDark: systemIsDark)

        switch preference {
        case "light", "dark":
            // 用户的显式全局选择同时约束外壳与终端配色。
            shellColorScheme = terminalColorScheme
        default:
            if systemIsDark {
                shellColorScheme = darkOverride ? .light : .dark
            } else {
                shellColorScheme = lightOverride ? .dark : .light
            }
        }
    }
}

enum WatchTerminalThemeNameIssue: Equatable, Sendable {
    case empty
    case tooLong
    case duplicate

    var message: String {
        switch self {
        case .empty:
            return "主题名称不能为空。"
        case .tooLong:
            return "主题名称不能超过 40 个字符。"
        case .duplicate:
            return "已有同名主题，请换一个名称。"
        }
    }
}

enum WatchTerminalThemeImportError: LocalizedError, Equatable {
    case unsupportedVersion
    case invalidDocument
    case invalidPalette

    var errorDescription: String? {
        switch self {
        case .unsupportedVersion:
            return "此主题版本不受支持。"
        case .invalidDocument:
            return "主题必须包含单套配色，或同时包含浅色与深色配色。"
        case .invalidPalette:
            return "主题颜色无效，或 ANSI 颜色超过 16 个。"
        }
    }
}

enum WatchTerminalThemeStore {
    static let storageKey = "watchCustomTerminalThemes"
    static let customSelectionPrefix = "custom:"
    static let maximumImportByteCount = 256 * 1024

    private struct ImportedDocument: Decodable {
        let version: Int
        let shared: ImportedPalette?
        let light: ImportedPalette?
        let dark: ImportedPalette?
        let appearance: ImportedAppearance?

        private enum CodingKeys: String, CodingKey {
            case version
            case shared
            case light
            case dark
            case appearance
        }

        init(from decoder: Decoder) throws {
            let container = try decoder.container(
                keyedBy: CodingKeys.self)
            version = try container.decode(Int.self, forKey: .version)
            shared = try container.decodeIfPresent(
                ImportedPalette.self,
                forKey: .shared)
            light = try container.decodeIfPresent(
                ImportedPalette.self,
                forKey: .light)
            dark = try container.decodeIfPresent(
                ImportedPalette.self,
                forKey: .dark)
            // iOS 会忽略缺失、非字典或字段不完整的 appearance，
            // 但仍继续导入有效调色板。
            appearance = try? container.decode(
                ImportedAppearance.self,
                forKey: .appearance)
        }
    }

    private struct ImportedPalette: Decodable {
        let foregroundColor: String
        let backgroundColor: String
        let cursorColor: String?
        let colorPaletteOverrides: [String]?
    }

    private struct ImportedAppearance: Decodable {
        let lightOverride: Bool
        let darkOverride: Bool

        private enum CodingKeys: String, CodingKey {
            case lightOverride
            case darkOverride
        }

        init(from decoder: Decoder) throws {
            let container = try decoder.container(
                keyedBy: CodingKeys.self)
            lightOverride = try Self.decodeNSNumberBool(
                from: container,
                forKey: .lightOverride)
            darkOverride = try Self.decodeNSNumberBool(
                from: container,
                forKey: .darkOverride)
        }

        private static func decodeNSNumberBool(
            from container: KeyedDecodingContainer<CodingKeys>,
            forKey key: CodingKeys
        ) throws -> Bool {
            if let value = try? container.decode(Bool.self, forKey: key) {
                return value
            }
            if let value = try? container.decode(Double.self, forKey: key) {
                return value != 0
            }
            throw DecodingError.dataCorruptedError(
                forKey: key,
                in: container,
                debugDescription:
                    "主题外观覆盖字段必须是 JSON 数字或布尔值")
        }
    }

    private struct ExportedDocument: Encodable {
        let version = 1
        let shared: ExportedPalette?
        let light: ExportedPalette?
        let dark: ExportedPalette?
        let appearance: ExportedAppearance?
    }

    private struct ExportedPalette: Encodable {
        let foregroundColor: String
        let backgroundColor: String
        let cursorColor: String
        let colorPaletteOverrides: [String]

        init(_ palette: WatchTerminalThemePalette) {
            foregroundColor = palette.foreground.hexString
            backgroundColor = palette.background.hexString
            cursorColor = palette.cursor.hexString
            colorPaletteOverrides =
                palette.ansiColors.map(\.hexString)
        }
    }

    private struct ExportedAppearance: Encodable {
        let lightOverride: Bool
        let darkOverride: Bool
    }

    static func load(
        from defaults: UserDefaults = .standard
    ) -> [WatchCustomTerminalTheme] {
        guard let data = defaults.data(forKey: storageKey) else {
            return []
        }
        return decode(data)
    }

    static func decode(_ data: Data) -> [WatchCustomTerminalTheme] {
        guard !data.isEmpty,
              let themes = try? JSONDecoder().decode(
                [WatchCustomTerminalTheme].self,
                from: data),
              themesAreWellFormed(themes) else {
            return []
        }
        return themes
    }

    static func save(
        _ themes: [WatchCustomTerminalTheme],
        to defaults: UserDefaults = .standard
    ) {
        defaults.set(encoded(themes), forKey: storageKey)
    }

    static func encoded(
        _ themes: [WatchCustomTerminalTheme]
    ) -> Data {
        precondition(
            themesAreWellFormed(themes),
            "不能保存无效的自定义终端主题")
        // 这些纯值类型没有编码失败分支，失败代表程序结构错误。
        return try! JSONEncoder().encode(themes)
    }

    static func theme(
        for selectionID: String,
        in themes: [WatchCustomTerminalTheme]
    ) -> WatchCustomTerminalTheme? {
        themes.first { $0.selectionID == selectionID }
    }

    static func displayName(
        for selectionID: String,
        in themes: [WatchCustomTerminalTheme]
    ) -> String {
        if let builtIn = WatchTerminalBuiltInThemeID(
            selectionID: selectionID
        ) {
            return builtIn.displayName
        }
        return theme(for: selectionID, in: themes)?.name ??
            WatchTerminalBuiltInThemeID.defaultTheme.displayName
    }

    static func makeTheme(
        copying paletteID: WatchTerminalPaletteID,
        existing themes: [WatchCustomTerminalTheme],
        usesGenericName: Bool = false
    ) -> WatchCustomTerminalTheme {
        makeTheme(
            copyingBuiltIn: paletteID.builtInThemeID,
            existing: themes,
            usesGenericName: usesGenericName)
    }

    static func makeTheme(
        copyingBuiltIn themeID: WatchTerminalBuiltInThemeID,
        existing themes: [WatchCustomTerminalTheme],
        usesGenericName: Bool = false
    ) -> WatchCustomTerminalTheme {
        let lightPalette = WatchTerminalThemePalette(
            themeID.palette(for: .light))
        let darkPalette = WatchTerminalThemePalette(
            themeID.palette(for: .dark))
        let requestedName = usesGenericName ?
            "自定义主题" : "\(themeID.displayName) 副本"
        return WatchCustomTerminalTheme(
            name: uniqueName(requestedName, in: themes),
            basePaletteID: themeID.basePaletteID,
            paletteMode: themeID.isAdaptive ? .adaptive : .single,
            lightPalette: lightPalette,
            darkPalette: darkPalette,
            lightOverride: themeID.lightOverride,
            darkOverride: themeID.darkOverride)
    }

    static func duplicate(
        _ theme: WatchCustomTerminalTheme,
        existing themes: [WatchCustomTerminalTheme]
    ) -> WatchCustomTerminalTheme {
        WatchCustomTerminalTheme(
            name: uniqueName("\(theme.name) 副本", in: themes),
            basePaletteID: theme.basePaletteID,
            paletteMode: theme.paletteMode,
            lightPalette: theme.lightPalette,
            darkPalette: theme.darkPalette,
            lightOverride: theme.lightOverride,
            darkOverride: theme.darkOverride)
    }

    static func importedTheme(
        from data: Data,
        suggestedName: String,
        existing themes: [WatchCustomTerminalTheme]
    ) throws -> WatchCustomTerminalTheme {
        guard !data.isEmpty,
              data.count <= maximumImportByteCount,
              let document = try? JSONDecoder().decode(
                ImportedDocument.self,
                from: data) else {
            throw WatchTerminalThemeImportError.invalidDocument
        }
        guard document.version == 1 else {
            throw WatchTerminalThemeImportError.unsupportedVersion
        }

        let requestedName = normalizedName(suggestedName)
        guard !requestedName.isEmpty else {
            throw WatchTerminalThemeImportError.invalidDocument
        }
        let name = uniqueImportedName(
            requestedName,
            in: themes)

        if let shared = document.shared,
           document.light == nil,
           document.dark == nil {
            let palette = try importedPalette(
                shared,
                fallback: .defaultDark)
            return WatchCustomTerminalTheme(
                name: name,
                basePaletteID: .defaultDark,
                paletteMode: .single,
                lightPalette: palette,
                darkPalette: palette,
                lightOverride:
                    document.appearance?.lightOverride ?? false,
                darkOverride:
                    document.appearance?.darkOverride ?? false)
        }
        if document.shared == nil,
           let light = document.light,
           let dark = document.dark {
            return WatchCustomTerminalTheme(
                name: name,
                basePaletteID: .defaultDark,
                paletteMode: .adaptive,
                lightPalette: try importedPalette(
                    light,
                    fallback: .defaultLight),
                darkPalette: try importedPalette(
                    dark,
                    fallback: .defaultDark),
                lightOverride:
                    document.appearance?.lightOverride ?? false,
                darkOverride:
                    document.appearance?.darkOverride ?? false)
        }
        throw WatchTerminalThemeImportError.invalidDocument
    }

    static func exportedData(
        for theme: WatchCustomTerminalTheme
    ) throws -> Data {
        let appearance: ExportedAppearance?
        if theme.lightOverride || theme.darkOverride {
            appearance = ExportedAppearance(
                lightOverride: theme.lightOverride,
                darkOverride: theme.darkOverride)
        } else {
            appearance = nil
        }
        let document: ExportedDocument
        switch theme.paletteMode {
        case .single:
            document = ExportedDocument(
                shared: ExportedPalette(theme.lightPalette),
                light: nil,
                dark: nil,
                appearance: appearance)
        case .adaptive:
            document = ExportedDocument(
                shared: nil,
                light: ExportedPalette(theme.lightPalette),
                dark: ExportedPalette(theme.darkPalette),
                appearance: appearance)
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(document)
    }

    static func appearanceResolution(
        for selectionID: String,
        in themes: [WatchCustomTerminalTheme],
        preference: String,
        systemIsDark: Bool
    ) -> WatchTerminalThemeAppearanceResolution {
        let overrides: (light: Bool, dark: Bool)
        if let builtIn = WatchTerminalBuiltInThemeID(
            selectionID: selectionID
        ) {
            overrides = (
                light: builtIn.lightOverride,
                dark: builtIn.darkOverride)
        } else if let theme = theme(
            for: selectionID,
            in: themes
        ) {
            overrides = (
                light: theme.lightOverride,
                dark: theme.darkOverride)
        } else {
            overrides = (light: false, dark: false)
        }
        return WatchTerminalThemeAppearanceResolution(
            preference: preference,
            systemIsDark: systemIsDark,
            lightOverride: overrides.light,
            darkOverride: overrides.dark)
    }

    static func nameIssue(
        for proposedName: String,
        excluding themeID: UUID? = nil,
        in themes: [WatchCustomTerminalTheme]
    ) -> WatchTerminalThemeNameIssue? {
        let name = proposedName.trimmingCharacters(
            in: .whitespacesAndNewlines)
        if name.isEmpty {
            return .empty
        }
        if name.count > WatchCustomTerminalTheme.maximumNameLength {
            return .tooLong
        }
        let comparableName = name.lowercased()
        if themes.contains(where: {
            $0.id != themeID &&
                $0.name.lowercased() == comparableName
        }) {
            return .duplicate
        }
        return nil
    }

    static func normalizedName(_ name: String) -> String {
        name.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static func uniqueName(
        _ requestedName: String,
        in themes: [WatchCustomTerminalTheme]
    ) -> String {
        let names = Set(themes.map { $0.name.lowercased() })
        if !names.contains(requestedName.lowercased()) {
            return requestedName
        }

        var suffix = 2
        while names.contains("\(requestedName) \(suffix)".lowercased()) {
            suffix += 1
        }
        return "\(requestedName) \(suffix)"
    }

    private static func uniqueImportedName(
        _ requestedName: String,
        in themes: [WatchCustomTerminalTheme]
    ) -> String {
        let maximumLength =
            WatchCustomTerminalTheme.maximumNameLength
        let baseName = String(requestedName.prefix(maximumLength))
        let names = Set(themes.map { $0.name.lowercased() })
        if !names.contains(baseName.lowercased()) {
            return baseName
        }

        var suffix = 2
        while true {
            let suffixText = " \(suffix)"
            let prefixLength = maximumLength - suffixText.count
            let candidate =
                String(baseName.prefix(prefixLength)) + suffixText
            if !names.contains(candidate.lowercased()) {
                return candidate
            }
            suffix += 1
        }
    }

    private static func importedPalette(
        _ palette: ImportedPalette,
        fallback: WatchTerminalPaletteID
    ) throws -> WatchTerminalThemePalette {
        guard let foreground = importedColor(
                palette.foregroundColor),
              let background = importedColor(
                palette.backgroundColor) else {
            throw WatchTerminalThemeImportError.invalidPalette
        }
        let cursor: WatchTerminalRGB
        if let cursorText = palette.cursorColor {
            guard let importedCursor = importedColor(cursorText) else {
                throw WatchTerminalThemeImportError.invalidPalette
            }
            cursor = importedCursor
        } else {
            cursor = foreground
        }

        var ansiColors = fallback.palette.ansi16Colors
        if let overrides = palette.colorPaletteOverrides {
            guard overrides.count <= ansiColors.count else {
                throw WatchTerminalThemeImportError.invalidPalette
            }
            for (index, text) in overrides.enumerated() {
                guard let color = importedColor(text) else {
                    throw WatchTerminalThemeImportError.invalidPalette
                }
                ansiColors[index] = color
            }
        }
        return WatchTerminalThemePalette(
            foreground: foreground,
            background: background,
            cursor: cursor,
            ansiColors: ansiColors)
    }

    private static func importedColor(
        _ text: String
    ) -> WatchTerminalRGB? {
        guard text.first == "#" else { return nil }
        let rgbText: String
        switch text.count {
        case 4:
            let digits = Array(text.dropFirst())
            rgbText = "#" + digits.map { "\($0)\($0)" }.joined()
        case 5:
            let digits = Array(text.dropFirst().prefix(3))
            rgbText = "#" + digits.map { "\($0)\($0)" }.joined()
        case 7:
            rgbText = text
        case 9:
            rgbText = String(text.prefix(7))
        default:
            return nil
        }
        return WatchTerminalRGB(hexString: rgbText)
    }

    private static func themesAreWellFormed(
        _ themes: [WatchCustomTerminalTheme]
    ) -> Bool {
        var ids = Set<UUID>()
        var names = Set<String>()
        for theme in themes {
            let name = normalizedName(theme.name)
            guard theme.name == name,
                  nameIssue(for: name, excluding: theme.id, in: themes) == nil,
                  theme.lightPalette.ansiColors.count == 16,
                  theme.darkPalette.ansiColors.count == 16,
                  theme.paletteMode == .adaptive ||
                    theme.lightPalette == theme.darkPalette,
                  ids.insert(theme.id).inserted,
                  names.insert(name.lowercased()).inserted else {
                return false
            }
        }
        return true
    }
}

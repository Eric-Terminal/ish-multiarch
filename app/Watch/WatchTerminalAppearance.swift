import CoreFoundation
import Foundation

enum WatchTerminalAppearancePreferenceKey {
    static let palette = "watchTerminalPalette"
    static let font = "watchTerminalFont"
    static let cursorShape = "watchTerminalCursorShape"
    static let cursorBlink = "watchTerminalCursorBlink"
}

enum WatchTerminalPaletteID: String, CaseIterable, Codable, Sendable {
    case defaultDark = "default-dark"
    case defaultLight = "default-light"
    case solarizedDark = "solarized-dark"
    case solarizedLight = "solarized-light"
    case classicGreen = "classic-green"

    var palette: WatchTerminalPalette {
        switch self {
        case .defaultDark:
            return .defaultDark
        case .defaultLight:
            return .defaultLight
        case .solarizedDark:
            return .solarizedDark
        case .solarizedLight:
            return .solarizedLight
        case .classicGreen:
            return .classicGreen
        }
    }

    var displayName: String {
        switch self {
        case .defaultDark:
            return "默认深色"
        case .defaultLight:
            return "默认浅色"
        case .solarizedDark:
            return "Solarized 深色"
        case .solarizedLight:
            return "Solarized 浅色"
        case .classicGreen:
            return "经典绿色"
        }
    }

    var builtInThemeID: WatchTerminalBuiltInThemeID {
        switch self {
        case .defaultDark, .defaultLight:
            return .defaultTheme
        case .solarizedDark, .solarizedLight:
            return .solarized
        case .classicGreen:
            return .classicGreen
        }
    }
}

enum WatchTerminalBuiltInThemeID: String, CaseIterable, Codable, Sendable {
    // 复用既有深色 ID 作为逻辑主题主 ID，避免已有持久选择失效。
    case defaultTheme = "default-dark"
    case solarized = "solarized-dark"
    case classicGreen = "classic-green"

    init?(selectionID: String) {
        switch selectionID {
        case WatchTerminalPaletteID.defaultDark.rawValue,
             WatchTerminalPaletteID.defaultLight.rawValue:
            self = .defaultTheme
        case WatchTerminalPaletteID.solarizedDark.rawValue,
             WatchTerminalPaletteID.solarizedLight.rawValue:
            self = .solarized
        case WatchTerminalPaletteID.classicGreen.rawValue:
            self = .classicGreen
        default:
            return nil
        }
    }

    var displayName: String {
        switch self {
        case .defaultTheme:
            return "默认"
        case .solarized:
            return "Solarized"
        case .classicGreen:
            return "经典绿色"
        }
    }

    var isAdaptive: Bool {
        self != .classicGreen
    }

    var lightOverride: Bool {
        self == .classicGreen
    }

    var darkOverride: Bool {
        false
    }

    var basePaletteID: WatchTerminalPaletteID {
        switch self {
        case .defaultTheme:
            return .defaultDark
        case .solarized:
            return .solarizedDark
        case .classicGreen:
            return .classicGreen
        }
    }

    func paletteID(
        for colorScheme: WatchTerminalThemeColorScheme
    ) -> WatchTerminalPaletteID {
        switch (self, colorScheme) {
        case (.defaultTheme, .light):
            return .defaultLight
        case (.defaultTheme, .dark):
            return .defaultDark
        case (.solarized, .light):
            return .solarizedLight
        case (.solarized, .dark):
            return .solarizedDark
        case (.classicGreen, _):
            return .classicGreen
        }
    }

    func palette(
        for colorScheme: WatchTerminalThemeColorScheme
    ) -> WatchTerminalPalette {
        paletteID(for: colorScheme).palette
    }

    func matches(selectionID: String) -> Bool {
        WatchTerminalBuiltInThemeID(selectionID: selectionID) == self
    }
}

enum WatchTerminalFontChoice: String, CaseIterable, Sendable {
    case systemMonospaced = "system-monospaced"
    case roundedMonospaced = "rounded-monospaced"
}

enum WatchTerminalCursorShape: String, CaseIterable, Sendable {
    case block
    case beam
    case underline
}

struct WatchTerminalAppearance: Equatable, Sendable {
    var paletteID: WatchTerminalPaletteID
    var font: WatchTerminalFontChoice
    var cursorShape: WatchTerminalCursorShape
    var cursorBlinks: Bool
    private var selectedPaletteID: String
    private var customPalette: WatchTerminalPalette?

    static let standard = WatchTerminalAppearance(
        builtInThemeID: .defaultTheme,
        colorScheme: .dark,
        font: .systemMonospaced,
        cursorShape: .block,
        cursorBlinks: false)

    init(
        paletteID: WatchTerminalPaletteID,
        font: WatchTerminalFontChoice,
        cursorShape: WatchTerminalCursorShape,
        cursorBlinks: Bool
    ) {
        self.init(
            builtInThemeID: paletteID.builtInThemeID,
            colorScheme: paletteID == .defaultLight ||
                paletteID == .solarizedLight ? .light : .dark,
            font: font,
            cursorShape: cursorShape,
            cursorBlinks: cursorBlinks)
    }

    init(
        builtInThemeID: WatchTerminalBuiltInThemeID,
        colorScheme: WatchTerminalThemeColorScheme,
        font: WatchTerminalFontChoice,
        cursorShape: WatchTerminalCursorShape,
        cursorBlinks: Bool
    ) {
        paletteID = builtInThemeID.paletteID(for: colorScheme)
        self.font = font
        self.cursorShape = cursorShape
        self.cursorBlinks = cursorBlinks
        selectedPaletteID = builtInThemeID.rawValue
        customPalette = nil
    }

    init(
        paletteSelectionID: String,
        customThemes: [WatchCustomTerminalTheme],
        colorScheme: WatchTerminalThemeColorScheme = .dark,
        font: WatchTerminalFontChoice,
        cursorShape: WatchTerminalCursorShape,
        cursorBlinks: Bool
    ) {
        self.font = font
        self.cursorShape = cursorShape
        self.cursorBlinks = cursorBlinks

        if let builtIn = WatchTerminalBuiltInThemeID(
            selectionID: paletteSelectionID
        ) {
            paletteID = builtIn.paletteID(for: colorScheme)
            selectedPaletteID = builtIn.rawValue
            customPalette = nil
        } else if let theme = customThemes.first(where: {
            $0.selectionID == paletteSelectionID
        }) {
            paletteID = theme.basePaletteID
            selectedPaletteID = theme.selectionID
            customPalette = theme.palette(for: colorScheme)
        } else {
            let fallbackTheme = WatchTerminalBuiltInThemeID.defaultTheme
            paletteID = fallbackTheme.paletteID(for: colorScheme)
            selectedPaletteID = fallbackTheme.rawValue
            customPalette = nil
        }
    }

    var paletteSelectionID: String {
        selectedPaletteID
    }

    var palette: WatchTerminalPalette {
        customPalette ?? paletteID.palette
    }

    static func load(
        from defaults: UserDefaults = .standard,
        colorScheme: WatchTerminalThemeColorScheme = .dark
    ) -> WatchTerminalAppearance {
        let fallback = WatchTerminalAppearance.standard
        let paletteSelectionID = strictString(
            from: defaults,
            key: WatchTerminalAppearancePreferenceKey.palette
        ) ?? fallback.paletteSelectionID
        let font = strictString(
            from: defaults,
            key: WatchTerminalAppearancePreferenceKey.font
        ).flatMap(WatchTerminalFontChoice.init(rawValue:)) ??
            fallback.font
        let cursorShape = strictString(
            from: defaults,
            key: WatchTerminalAppearancePreferenceKey.cursorShape
        ).flatMap(WatchTerminalCursorShape.init(rawValue:)) ??
            fallback.cursorShape
        let cursorBlinks = strictBool(
            from: defaults,
            key: WatchTerminalAppearancePreferenceKey.cursorBlink) ??
            fallback.cursorBlinks

        let appearance = WatchTerminalAppearance(
            paletteSelectionID: paletteSelectionID,
            customThemes: WatchTerminalThemeStore.load(from: defaults),
            colorScheme: colorScheme,
            font: font,
            cursorShape: cursorShape,
            cursorBlinks: cursorBlinks)
        if paletteSelectionID != appearance.paletteSelectionID {
            defaults.set(
                appearance.paletteSelectionID,
                forKey: WatchTerminalAppearancePreferenceKey.palette)
        }
        return appearance
    }

    func save(to defaults: UserDefaults = .standard) {
        defaults.set(
            paletteSelectionID,
            forKey: WatchTerminalAppearancePreferenceKey.palette)
        defaults.set(
            font.rawValue,
            forKey: WatchTerminalAppearancePreferenceKey.font)
        defaults.set(
            cursorShape.rawValue,
            forKey: WatchTerminalAppearancePreferenceKey.cursorShape)
        defaults.set(
            cursorBlinks,
            forKey: WatchTerminalAppearancePreferenceKey.cursorBlink)
    }

    private static func strictString(
        from defaults: UserDefaults,
        key: String
    ) -> String? {
        defaults.object(forKey: key) as? String
    }

    private static func strictBool(
        from defaults: UserDefaults,
        key: String
    ) -> Bool? {
        guard let number = defaults.object(forKey: key) as? NSNumber,
              CFGetTypeID(number) == CFBooleanGetTypeID() else {
            return nil
        }
        return number.boolValue
    }
}

struct WatchTerminalRGB: Codable, Equatable, Sendable {
    let red: UInt8
    let green: UInt8
    let blue: UInt8

    init(red: UInt8, green: UInt8, blue: UInt8) {
        self.red = red
        self.green = green
        self.blue = blue
    }

    init?(hexString: String) {
        guard hexString.count == 7,
              hexString.first == "#",
              let value = UInt32(hexString.dropFirst(), radix: 16) else {
            return nil
        }
        red = UInt8((value >> 16) & 0xff)
        green = UInt8((value >> 8) & 0xff)
        blue = UInt8(value & 0xff)
    }

    var hexString: String {
        String(format: "#%02X%02X%02X", red, green, blue)
    }

    var redComponent: Double {
        Double(red) / 255
    }

    var greenComponent: Double {
        Double(green) / 255
    }

    var blueComponent: Double {
        Double(blue) / 255
    }
}

struct WatchTerminalResolvedColor: Equatable, Sendable {
    let rgb: WatchTerminalRGB
    let opacity: Double
}

enum WatchTerminalTextWeight: Equatable, Sendable {
    case regular
    case bold
}

enum WatchTerminalTextDecoration: Equatable, Sendable {
    case none
    case underline
}

struct WatchTerminalResolvedStyle: Equatable, Sendable {
    let foreground: WatchTerminalResolvedColor
    let background: WatchTerminalResolvedColor
    let weight: WatchTerminalTextWeight
    let decoration: WatchTerminalTextDecoration
}

struct WatchTerminalPalette: Equatable, Sendable {
    let id: WatchTerminalPaletteID
    let foreground: WatchTerminalRGB
    let background: WatchTerminalRGB
    let cursor: WatchTerminalRGB
    private let ansiColors: [WatchTerminalRGB]

    init(
        id: WatchTerminalPaletteID,
        foreground: WatchTerminalRGB,
        background: WatchTerminalRGB,
        cursor: WatchTerminalRGB? = nil,
        ansiColors: [WatchTerminalRGB]
    ) {
        precondition(
            ansiColors.count == 16,
            "终端调色板必须包含 16 个 ANSI 基础色")
        self.id = id
        self.foreground = foreground
        self.background = background
        self.cursor = cursor ?? foreground
        self.ansiColors = ansiColors
    }

    var ansi16Colors: [WatchTerminalRGB] {
        ansiColors
    }

    static let defaultDark = WatchTerminalPalette(
        id: .defaultDark,
        foreground: rgb(0xe5e5e5),
        background: rgb(0x000000),
        ansiColors: darkANSIColors)

    static let defaultLight = WatchTerminalPalette(
        id: .defaultLight,
        foreground: rgb(0x1f2328),
        background: rgb(0xffffff),
        ansiColors: [
            rgb(0x24292f), rgb(0xcf222e),
            rgb(0x116329), rgb(0x9a6700),
            rgb(0x0969da), rgb(0x8250df),
            rgb(0x1b7c83), rgb(0x6e7781),
            rgb(0x57606a), rgb(0xa40e26),
            rgb(0x1a7f37), rgb(0xbf8700),
            rgb(0x218bff), rgb(0xa475f9),
            rgb(0x3192aa), rgb(0xafb8c1),
        ])

    static let classicGreen = WatchTerminalPalette(
        id: .classicGreen,
        foreground: rgb(0x33ff66),
        background: rgb(0x001004),
        ansiColors: darkANSIColors)

    static let solarizedDark = WatchTerminalPalette(
        id: .solarizedDark,
        foreground: rgb(0x839496),
        background: rgb(0x002b36),
        ansiColors: solarizedANSIColors)

    static let solarizedLight = WatchTerminalPalette(
        id: .solarizedLight,
        foreground: rgb(0x657b83),
        background: rgb(0xfdf6e3),
        ansiColors: solarizedANSIColors)

    func resolve(_ style: TerminalStyle) -> WatchTerminalResolvedStyle {
        var foreground = resolve(
            style.foreground,
            defaultColor: foreground)
        var background = resolve(
            style.background,
            defaultColor: background)
        if style.isInverse {
            swap(&foreground, &background)
        }

        return WatchTerminalResolvedStyle(
            foreground: WatchTerminalResolvedColor(
                rgb: foreground,
                opacity: style.isDim ? 0.55 : 1),
            background: WatchTerminalResolvedColor(
                rgb: background,
                opacity: 1),
            weight: style.isBold ? .bold : .regular,
            decoration: style.isUnderlined ? .underline : .none)
    }

    private func resolve(
        _ color: TerminalColor,
        defaultColor: WatchTerminalRGB
    ) -> WatchTerminalRGB {
        switch color {
        case .default:
            return defaultColor
        case let .rgb(red, green, blue):
            return WatchTerminalRGB(
                red: red,
                green: green,
                blue: blue)
        case let .indexed(index):
            return indexedColor(index)
        }
    }

    private func indexedColor(_ index: UInt8) -> WatchTerminalRGB {
        if index < 16 {
            return ansiColors[Int(index)]
        }
        if index < 232 {
            let cubeIndex = Int(index) - 16
            let levels: [UInt8] = [0, 95, 135, 175, 215, 255]
            return WatchTerminalRGB(
                red: levels[cubeIndex / 36],
                green: levels[(cubeIndex / 6) % 6],
                blue: levels[cubeIndex % 6])
        }
        let gray = UInt8(8 + (Int(index) - 232) * 10)
        return WatchTerminalRGB(
            red: gray,
            green: gray,
            blue: gray)
    }

    private static let darkANSIColors = [
        rgb(0x000000), rgb(0xcc0000),
        rgb(0x4e9a06), rgb(0xc4a000),
        rgb(0x3465a4), rgb(0x75507b),
        rgb(0x06989a), rgb(0xd3d7cf),
        rgb(0x555753), rgb(0xef2929),
        rgb(0x8ae234), rgb(0xfce94f),
        rgb(0x729fcf), rgb(0xad7fa8),
        rgb(0x34e2e2), rgb(0xeeeeec),
    ]

    private static let solarizedANSIColors = [
        rgb(0x073642), rgb(0xdc322f),
        rgb(0x859900), rgb(0xb58900),
        rgb(0x268bd2), rgb(0xd33682),
        rgb(0x2aa198), rgb(0xeee8d5),
        rgb(0x002b36), rgb(0xcb4b16),
        rgb(0x586e75), rgb(0x657b83),
        rgb(0x839496), rgb(0x6c71c4),
        rgb(0x93a1a1), rgb(0xfdf6e3),
    ]

    private static func rgb(_ value: UInt32) -> WatchTerminalRGB {
        WatchTerminalRGB(
            red: UInt8((value >> 16) & 0xff),
            green: UInt8((value >> 8) & 0xff),
            blue: UInt8(value & 0xff))
    }
}

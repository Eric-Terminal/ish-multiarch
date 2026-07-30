import Foundation

private struct LegacyWatchCustomTerminalTheme: Codable {
    let id: UUID
    let name: String
    let basePaletteID: WatchTerminalPaletteID
    let foreground: WatchTerminalRGB
    let background: WatchTerminalRGB
    let cursor: WatchTerminalRGB
    let ansiColors: [WatchTerminalRGB]
}

@main
struct WatchTerminalAppearanceTest {
    private static var failures = 0

    static func main() {
        testBuiltInChoices()
        testBuiltInSelectionMigration()
        testBuiltInThemeCopy()
        testPreferenceRoundTrip()
        testStrictPreferenceFallback()
        testANSIColors()
        testTrueColorAndDefaults()
        testTextAttributes()
        testHexColors()
        testCustomThemePersistence()
        testCustomThemeLifecycle()
        testColorSchemePreferenceResolution()
        testThemeAppearanceOverrides()
        testAdaptiveThemeModes()
        testAdaptiveThemePersistence()
        testLegacyThemeMigration()
        testImportedThemes()
        testThemeExportRoundTrip()
        testThemeSharedRoundTrip()
        testCorruptCustomThemeFallback()

        if failures == 0 {
            print("Watch 终端外观模型回归通过")
        } else {
            fatalError("Watch 终端外观模型回归失败：\(failures) 项")
        }
    }

    private static func expect(
        _ condition: @autoclosure () -> Bool,
        _ message: String
    ) {
        if !condition() {
            print("失败：\(message)")
            failures += 1
        }
    }

    private static func withDefaults(
        _ body: (UserDefaults) -> Void
    ) {
        let suite = "ish.watch.appearance.tests.\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suite) else {
            fatalError("无法创建隔离的 UserDefaults")
        }
        defer {
            defaults.removePersistentDomain(forName: suite)
        }
        body(defaults)
    }

    private static func testBuiltInChoices() {
        expect(
            WatchTerminalPaletteID.allCases == [
                .defaultDark, .defaultLight,
                .solarizedDark, .solarizedLight,
                .classicGreen,
            ],
            "五个既有调色板 ID 应继续作为持久数据兼容层")
        expect(
            WatchTerminalBuiltInThemeID.allCases == [
                .defaultTheme, .solarized, .classicGreen,
            ],
            "设置界面应只提供默认、Solarized 和经典绿色三个逻辑主题")
        expect(
            WatchTerminalFontChoice.allCases == [
                .systemMonospaced, .roundedMonospaced,
            ],
            "应只暴露适合小屏的内置等宽字体选择")
        expect(
            WatchTerminalCursorShape.allCases == [
                .block, .beam, .underline,
            ],
            "应提供块状、竖线和下划线光标")
        expect(
            WatchTerminalPaletteID.defaultDark.palette.id == .defaultDark &&
                WatchTerminalPaletteID.defaultLight.palette.id ==
                    .defaultLight &&
                WatchTerminalPaletteID.solarizedDark.palette.id ==
                    .solarizedDark &&
                WatchTerminalPaletteID.solarizedLight.palette.id ==
                    .solarizedLight &&
                WatchTerminalPaletteID.classicGreen.palette.id ==
                    .classicGreen,
            "调色板标识应稳定映射到对应内置值")
        expect(
            WatchTerminalBuiltInThemeID.defaultTheme.paletteID(
                for: .light) == .defaultLight &&
                WatchTerminalBuiltInThemeID.defaultTheme.paletteID(
                    for: .dark) == .defaultDark &&
                WatchTerminalBuiltInThemeID.solarized.paletteID(
                    for: .light) == .solarizedLight &&
                WatchTerminalBuiltInThemeID.solarized.paletteID(
                    for: .dark) == .solarizedDark &&
                WatchTerminalBuiltInThemeID.classicGreen.paletteID(
                    for: .light) == .classicGreen &&
                WatchTerminalBuiltInThemeID.classicGreen.paletteID(
                    for: .dark) == .classicGreen,
            "两个自适应主题应解析浅深双套配色，经典绿色应始终使用单套")
    }

    private static func testBuiltInSelectionMigration() {
        expect(
            WatchTerminalBuiltInThemeID(
                selectionID: "default-light") == .defaultTheme &&
                WatchTerminalBuiltInThemeID(
                    selectionID: "default-dark") == .defaultTheme &&
                WatchTerminalBuiltInThemeID(
                    selectionID: "solarized-light") == .solarized &&
                WatchTerminalBuiltInThemeID(
                    selectionID: "solarized-dark") == .solarized,
            "四个既有明暗选择 ID 应映射到对应逻辑主题")

        withDefaults { defaults in
            defaults.set(
                WatchTerminalPaletteID.defaultLight.rawValue,
                forKey: WatchTerminalAppearancePreferenceKey.palette)
            let dark = WatchTerminalAppearance.load(
                from: defaults,
                colorScheme: .dark)
            expect(
                dark.paletteSelectionID ==
                    WatchTerminalBuiltInThemeID.defaultTheme.rawValue &&
                    dark.paletteID == .defaultDark &&
                    dark.palette == .defaultDark &&
                    defaults.string(
                        forKey:
                            WatchTerminalAppearancePreferenceKey.palette) ==
                        WatchTerminalBuiltInThemeID.defaultTheme.rawValue,
                "旧默认浅色选择应迁移为逻辑默认主题并按当前深色外观解析")

            defaults.set(
                WatchTerminalPaletteID.solarizedLight.rawValue,
                forKey: WatchTerminalAppearancePreferenceKey.palette)
            let light = WatchTerminalAppearance.load(
                from: defaults,
                colorScheme: .light)
            expect(
                light.paletteSelectionID ==
                    WatchTerminalBuiltInThemeID.solarized.rawValue &&
                    light.paletteID == .solarizedLight &&
                    light.palette == .solarizedLight &&
                    defaults.string(
                        forKey:
                            WatchTerminalAppearancePreferenceKey.palette) ==
                        WatchTerminalBuiltInThemeID.solarized.rawValue,
                "旧 Solarized 浅色选择应迁移为逻辑主题并保留浅色解析")
        }

        let compatibleDark = WatchTerminalAppearance(
            paletteSelectionID:
                WatchTerminalPaletteID.solarizedDark.rawValue,
            customThemes: [],
            colorScheme: .light,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        expect(
            compatibleDark.paletteID == .solarizedLight &&
                WatchTerminalThemeStore.displayName(
                    for: WatchTerminalPaletteID.solarizedLight.rawValue,
                    in: []) ==
                    WatchTerminalBuiltInThemeID.solarized.displayName,
            "旧深色主 ID 也应作为逻辑选择，在浅色 App 外观下切换完整配色")
    }

    private static func testBuiltInThemeCopy() {
        let defaultCopy = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .defaultTheme,
            existing: [])
        let solarizedCopy = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .solarized,
            existing: [defaultCopy])
        let classicCopy = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .classicGreen,
            existing: [defaultCopy, solarizedCopy])
        let legacyLightCopy = WatchTerminalThemeStore.makeTheme(
            copying: .defaultLight,
            existing: [defaultCopy, solarizedCopy, classicCopy])

        expect(
            defaultCopy.paletteMode == .adaptive &&
                defaultCopy.lightPalette ==
                    WatchTerminalThemePalette(.defaultLight) &&
                defaultCopy.darkPalette ==
                    WatchTerminalThemePalette(.defaultDark) &&
                defaultCopy.lightPalette.ansiColors.count == 16 &&
                defaultCopy.darkPalette.ansiColors.count == 16,
            "复制默认主题应保留浅深两套各 19 个颜色")
        expect(
            solarizedCopy.paletteMode == .adaptive &&
                solarizedCopy.lightPalette ==
                    WatchTerminalThemePalette(.solarizedLight) &&
                solarizedCopy.darkPalette ==
                    WatchTerminalThemePalette(.solarizedDark),
            "复制 Solarized 应保留完整浅深双套配色")
        expect(
            classicCopy.paletteMode == .single &&
                classicCopy.lightPalette ==
                    WatchTerminalThemePalette(.classicGreen) &&
                classicCopy.darkPalette == classicCopy.lightPalette &&
                classicCopy.lightOverride &&
                !classicCopy.darkOverride,
            "复制经典绿色应保持单套配色与始终深色外壳")
        expect(
            legacyLightCopy.paletteMode == .adaptive &&
                legacyLightCopy.lightPalette ==
                    defaultCopy.lightPalette &&
                legacyLightCopy.darkPalette ==
                    defaultCopy.darkPalette &&
                !legacyLightCopy.lightOverride &&
                !legacyLightCopy.darkOverride &&
                !solarizedCopy.lightOverride &&
                !solarizedCopy.darkOverride,
            "通过旧浅色调色板入口复制时也不能丢失逻辑主题的自适应属性")
    }

    private static func testPreferenceRoundTrip() {
        withDefaults { defaults in
            let appearance = WatchTerminalAppearance(
                paletteID: .classicGreen,
                font: .roundedMonospaced,
                cursorShape: .underline,
                cursorBlinks: true)
            appearance.save(to: defaults)

            expect(
                WatchTerminalAppearance.load(from: defaults) == appearance,
                "有效外观偏好应完整往返")
            expect(
                defaults.string(
                    forKey: "watchTerminalPalette") == "classic-green" &&
                    defaults.string(
                        forKey: "watchTerminalFont") ==
                        "rounded-monospaced" &&
                    defaults.string(
                        forKey: "watchTerminalCursorShape") ==
                        "underline" &&
                    defaults.bool(
                        forKey: "watchTerminalCursorBlink"),
                "外观偏好应写入稳定 UserDefaults key")
        }
    }

    private static func testStrictPreferenceFallback() {
        withDefaults { defaults in
            defaults.set(
                "unknown",
                forKey: WatchTerminalAppearancePreferenceKey.palette)
            defaults.set(
                7,
                forKey: WatchTerminalAppearancePreferenceKey.font)
            defaults.set(
                "BLOCK",
                forKey: WatchTerminalAppearancePreferenceKey.cursorShape)
            defaults.set(
                1,
                forKey: WatchTerminalAppearancePreferenceKey.cursorBlink)

            expect(
                WatchTerminalAppearance.load(from: defaults) == .standard,
                "未知枚举、错误类型和数字布尔值都应严格回落默认值")
        }
    }

    private static func testANSIColors() {
        let palette = WatchTerminalPalette.defaultDark
        let red = palette.resolve(TerminalStyle(
            foreground: .indexed(1)))
        expect(
            red.foreground.rgb ==
                WatchTerminalRGB(red: 204, green: 0, blue: 0),
            "ANSI 16 色应使用当前调色板")

        let cubeBlack = palette.resolve(TerminalStyle(
            foreground: .indexed(16)))
        let cubeBlue = palette.resolve(TerminalStyle(
            foreground: .indexed(21)))
        let cubeWhite = palette.resolve(TerminalStyle(
            foreground: .indexed(231)))
        expect(
            cubeBlack.foreground.rgb ==
                WatchTerminalRGB(red: 0, green: 0, blue: 0) &&
                cubeBlue.foreground.rgb ==
                WatchTerminalRGB(red: 0, green: 0, blue: 255) &&
                cubeWhite.foreground.rgb ==
                WatchTerminalRGB(red: 255, green: 255, blue: 255),
            "ANSI 256 色立方体应使用 xterm 标准色阶")

        let firstGray = palette.resolve(TerminalStyle(
            foreground: .indexed(232)))
        let lastGray = palette.resolve(TerminalStyle(
            foreground: .indexed(255)))
        expect(
            firstGray.foreground.rgb ==
                WatchTerminalRGB(red: 8, green: 8, blue: 8) &&
                lastGray.foreground.rgb ==
                WatchTerminalRGB(red: 238, green: 238, blue: 238),
            "ANSI 256 色灰阶两端应准确映射")

        let solarizedBlue =
            WatchTerminalPalette.solarizedDark.resolve(
                TerminalStyle(foreground: .indexed(4)))
        expect(
            solarizedBlue.foreground.rgb ==
                WatchTerminalRGB(red: 38, green: 139, blue: 210),
            "Solarized 调色板应保留其 ANSI 基础色")
    }

    private static func testTrueColorAndDefaults() {
        let palette = WatchTerminalPalette.defaultLight
        let trueColor = palette.resolve(TerminalStyle(
            foreground: .rgb(red: 1, green: 127, blue: 255)))
        expect(
            trueColor.foreground.rgb ==
                WatchTerminalRGB(red: 1, green: 127, blue: 255),
            "ANSI 真彩色应原样映射 RGB 分量")

        let defaults = palette.resolve(TerminalStyle())
        expect(
            defaults.foreground.rgb == palette.foreground &&
                defaults.background.rgb == palette.background,
            "默认 ANSI 颜色应使用调色板前景与背景")
        expect(
            defaults.foreground.rgb.redComponent ==
                Double(palette.foreground.red) / 255,
            "颜色模型应提供 SwiftUI Color 可直接使用的归一化分量")
    }

    private static func testTextAttributes() {
        let palette = WatchTerminalPalette.defaultDark
        let style = TerminalStyle(
            foreground: .indexed(1),
            background: .indexed(4),
            isBold: true,
            isDim: true,
            isUnderlined: true,
            isInverse: true)
        let resolved = palette.resolve(style)

        expect(
            resolved.foreground.rgb ==
                WatchTerminalRGB(red: 52, green: 101, blue: 164) &&
                resolved.background.rgb ==
                WatchTerminalRGB(red: 204, green: 0, blue: 0),
            "Inverse 应在调色板解析后交换前景与背景")
        expect(
            resolved.foreground.opacity == 0.55 &&
                resolved.background.opacity == 1,
            "Dim 应只降低最终文字前景的不透明度")
        expect(
            resolved.weight == .bold &&
                resolved.decoration == .underline,
            "Bold 与下划线应成为 UI 可直接消费的纯值结果")
    }

    private static func testHexColors() {
        let color = WatchTerminalRGB(hexString: "#1a7FFF")
        expect(
            color == WatchTerminalRGB(red: 26, green: 127, blue: 255),
            "十六进制颜色应兼容大小写并准确解析")
        expect(
            color?.hexString == "#1A7FFF",
            "十六进制颜色应输出稳定的大写 #RRGGBB 格式")
        expect(
            WatchTerminalRGB(hexString: "1A7FFF") == nil &&
                WatchTerminalRGB(hexString: "#12345") == nil &&
                WatchTerminalRGB(hexString: "#GG0000") == nil,
            "十六进制颜色应严格拒绝缺少井号、错误长度和非十六进制字符")
    }

    private static func testCustomThemePersistence() {
        withDefaults { defaults in
            var theme = WatchTerminalThemeStore.makeTheme(
                copying: .solarizedDark,
                existing: [])
            theme.name = "深海"
            theme.foreground =
                WatchTerminalRGB(red: 1, green: 2, blue: 3)
            theme.cursor =
                WatchTerminalRGB(red: 250, green: 240, blue: 230)
            theme.ansiColors[4] =
                WatchTerminalRGB(red: 9, green: 8, blue: 7)

            WatchTerminalThemeStore.save([theme], to: defaults)
            let loaded = WatchTerminalThemeStore.load(from: defaults)
            expect(
                loaded == [theme],
                "自定义主题名称、主色、光标和 ANSI 颜色应完整持久化")

            let appearance = WatchTerminalAppearance(
                paletteSelectionID: theme.selectionID,
                customThemes: loaded,
                colorScheme: .light,
                font: .roundedMonospaced,
                cursorShape: .beam,
                cursorBlinks: true)
            appearance.save(to: defaults)
            let restored = WatchTerminalAppearance.load(
                from: defaults,
                colorScheme: .light)
            expect(
                restored == appearance,
                "选择的自定义主题应与其他外观偏好一起完整恢复")
            expect(
                restored.palette.foreground == theme.foreground &&
                    restored.palette.cursor == theme.cursor &&
                    restored.palette.ansi16Colors[4] ==
                        theme.ansiColors[4],
                "渲染器应解析自定义前景、独立光标与 ANSI 颜色")
        }
    }

    private static func testCustomThemeLifecycle() {
        var themes: [WatchCustomTerminalTheme] = []
        let first = WatchTerminalThemeStore.makeTheme(
            copying: .classicGreen,
            existing: themes,
            usesGenericName: true)
        themes.append(first)
        let second = WatchTerminalThemeStore.makeTheme(
            copying: .defaultDark,
            existing: themes,
            usesGenericName: true)
        themes.append(second)
        let duplicate = WatchTerminalThemeStore.duplicate(
            first,
            existing: themes)

        expect(
            first.name == "自定义主题" &&
                second.name == "自定义主题 2" &&
                duplicate.name == "自定义主题 副本",
            "新建与复制主题应生成不冲突的可读名称")
        expect(
            first.foreground ==
                WatchTerminalPalette.classicGreen.foreground &&
                first.background ==
                    WatchTerminalPalette.classicGreen.background &&
                first.cursor == WatchTerminalPalette.classicGreen.cursor &&
                first.ansiColors ==
                    WatchTerminalPalette.classicGreen.ansi16Colors,
            "从内置主题复制时应保留全部 19 个可编辑颜色")
        expect(
            WatchTerminalThemeStore.nameIssue(
                for: "  ",
                in: themes) == .empty &&
                WatchTerminalThemeStore.nameIssue(
                    for: String(repeating: "色", count: 41),
                    in: themes) == .tooLong &&
                WatchTerminalThemeStore.nameIssue(
                    for: "自定义主题",
                    excluding: second.id,
                    in: themes) == .duplicate,
            "主题名称应拒绝空值、过长值和重复值")
        expect(
            WatchTerminalThemeStore.displayName(
                for: first.selectionID,
                in: themes) == first.name,
            "主题选择标识应稳定解析为自定义名称")

        themes.removeAll { $0.id == first.id }
        let fallback = WatchTerminalAppearance(
            paletteSelectionID: first.selectionID,
            customThemes: themes,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        expect(
            fallback == .standard,
            "删除当前自定义主题后，失效选择应安全回落默认深色")
        let lightFallback = WatchTerminalAppearance(
            paletteSelectionID: first.selectionID,
            customThemes: themes,
            colorScheme: .light,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        expect(
            lightFallback.paletteID == .defaultLight &&
                lightFallback.palette == .defaultLight,
            "浅色外观下的失效选择应回落默认浅色")
    }

    private static func testAdaptiveThemeModes() {
        var theme = WatchTerminalThemeStore.makeTheme(
            copying: .classicGreen,
            existing: [])
        theme.lightPalette.foreground =
            WatchTerminalRGB(red: 10, green: 20, blue: 30)
        theme.lightPalette.cursor =
            WatchTerminalRGB(red: 40, green: 50, blue: 60)

        theme.setPaletteMode(.adaptive)
        expect(
            theme.paletteMode == .adaptive &&
                theme.darkPalette == theme.lightPalette,
            "从单套切到双套时应把当前主配色完整复制为深色配色")

        theme.darkPalette.foreground =
            WatchTerminalRGB(red: 210, green: 220, blue: 230)
        theme.darkPalette.cursor =
            WatchTerminalRGB(red: 200, green: 100, blue: 50)
        theme.darkPalette.ansiColors[2] =
            WatchTerminalRGB(red: 1, green: 99, blue: 2)

        let lightAppearance = WatchTerminalAppearance(
            paletteSelectionID: theme.selectionID,
            customThemes: [theme],
            colorScheme: .light,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        let darkAppearance = WatchTerminalAppearance(
            paletteSelectionID: theme.selectionID,
            customThemes: [theme],
            colorScheme: .dark,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        expect(
            lightAppearance.palette.foreground ==
                theme.lightPalette.foreground &&
                darkAppearance.palette.foreground ==
                    theme.darkPalette.foreground &&
                darkAppearance.palette.cursor ==
                    theme.darkPalette.cursor &&
                darkAppearance.palette.ansi16Colors[2] ==
                    theme.darkPalette.ansiColors[2],
            "实际终端外观应按当前 App 明暗解析对应的完整调色板")

        let duplicate = WatchTerminalThemeStore.duplicate(
            theme,
            existing: [theme])
        expect(
            duplicate.paletteMode == .adaptive &&
                duplicate.lightPalette == theme.lightPalette &&
                duplicate.darkPalette == theme.darkPalette &&
                duplicate.lightOverride == theme.lightOverride &&
                duplicate.darkOverride == theme.darkOverride,
            "复制双套主题时应保留模式、外观覆盖和两套完整配色")

        theme.setPaletteMode(.single)
        expect(
            theme.palette(for: .dark).foreground ==
                theme.lightPalette.foreground &&
                theme.darkPalette == theme.lightPalette,
            "切回单套时应保留浅色主配色并替换原深色配色")

        theme.setPaletteMode(.adaptive)
        expect(
            theme.darkPalette == theme.lightPalette,
            "重新开启双套时应再次从当前主配色复制深色配色")
    }

    private static func testColorSchemePreferenceResolution() {
        let explicitLight = WatchTerminalThemeColorScheme(
            preference: "light",
            environmentIsDark: true)
        let explicitDark = WatchTerminalThemeColorScheme(
            preference: "dark",
            environmentIsDark: false)
        expect(
            explicitLight == .light && explicitDark == .dark,
            "显式 App 明暗偏好应优先于系统外观")
        let lightAppearance = WatchTerminalAppearance(
            paletteSelectionID:
                WatchTerminalBuiltInThemeID.defaultTheme.rawValue,
            customThemes: [],
            colorScheme: explicitLight,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        let darkAppearance = WatchTerminalAppearance(
            paletteSelectionID:
                WatchTerminalBuiltInThemeID.solarized.rawValue,
            customThemes: [],
            colorScheme: explicitDark,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
        expect(
            lightAppearance.palette == .defaultLight &&
                darkAppearance.palette == .solarizedDark,
            "实际内置主题解析应消费显式 App 明暗结果")
        expect(
            WatchTerminalThemeColorScheme(
                preference: "system",
                environmentIsDark: true) == .dark &&
                WatchTerminalThemeColorScheme(
                    preference: "unknown",
                    environmentIsDark: false) == .light,
            "跟随系统和未知偏好应使用当前 SwiftUI 环境明暗")
    }

    private static func testThemeAppearanceOverrides() {
        let combinations: [
            (
                lightOverride: Bool,
                darkOverride: Bool,
                lightShell: WatchTerminalThemeColorScheme,
                darkShell: WatchTerminalThemeColorScheme
            )
        ] = [
            (false, false, .light, .dark),
            (true, false, .dark, .dark),
            (false, true, .light, .light),
            (true, true, .dark, .light),
        ]
        for combination in combinations {
            let onLight = WatchTerminalThemeAppearanceResolution(
                preference: "system",
                systemIsDark: false,
                lightOverride: combination.lightOverride,
                darkOverride: combination.darkOverride)
            let onDark = WatchTerminalThemeAppearanceResolution(
                preference: "system",
                systemIsDark: true,
                lightOverride: combination.lightOverride,
                darkOverride: combination.darkOverride)
            expect(
                onLight.terminalColorScheme == .light &&
                    onDark.terminalColorScheme == .dark &&
                    onLight.shellColorScheme ==
                        combination.lightShell &&
                    onDark.shellColorScheme ==
                        combination.darkShell,
                "四种 ThemeAppearance 组合都应只覆盖外壳，不改变终端配色")
        }

        let explicitLight = WatchTerminalThemeAppearanceResolution(
            preference: "light",
            systemIsDark: true,
            lightOverride: true,
            darkOverride: true)
        let explicitDark = WatchTerminalThemeAppearanceResolution(
            preference: "dark",
            systemIsDark: false,
            lightOverride: true,
            darkOverride: true)
        expect(
            explicitLight.terminalColorScheme == .light &&
                explicitLight.shellColorScheme == .light &&
                explicitDark.terminalColorScheme == .dark &&
                explicitDark.shellColorScheme == .dark,
            "显式全局明暗必须高于主题外观覆盖")

        let classic = WatchTerminalThemeStore.appearanceResolution(
            for:
                WatchTerminalBuiltInThemeID.classicGreen.rawValue,
            in: [],
            preference: "system",
            systemIsDark: false)
        let defaultTheme =
            WatchTerminalThemeStore.appearanceResolution(
                for:
                    WatchTerminalBuiltInThemeID.defaultTheme.rawValue,
                in: [],
                preference: "system",
                systemIsDark: false)
        let solarized =
            WatchTerminalThemeStore.appearanceResolution(
                for:
                    WatchTerminalBuiltInThemeID.solarized.rawValue,
                in: [],
                preference: "system",
                systemIsDark: true)
        expect(
            classic.terminalColorScheme == .light &&
                classic.shellColorScheme == .dark &&
                defaultTheme.shellColorScheme == .light &&
                solarized.shellColorScheme == .dark,
            "经典绿色应等效 1337 始终深色，默认与 Solarized 不覆盖外壳")
    }

    private static func testLegacyThemeMigration() {
        withDefaults { defaults in
            let palette = WatchTerminalPalette.classicGreen
            let legacy = LegacyWatchCustomTerminalTheme(
                id: UUID(),
                name: "旧主题",
                basePaletteID: .classicGreen,
                foreground: palette.foreground,
                background: palette.background,
                cursor: palette.cursor,
                ansiColors: palette.ansi16Colors)
            let legacyData = try! JSONEncoder().encode([legacy])
            defaults.set(
                legacyData,
                forKey: WatchTerminalThemeStore.storageKey)

            let migrated = WatchTerminalThemeStore.load(
                from: defaults)
            expect(
                migrated.count == 1 &&
                    migrated[0].paletteMode == .single &&
                    migrated[0].lightPalette ==
                        migrated[0].darkPalette &&
                    !migrated[0].lightOverride &&
                    !migrated[0].darkOverride &&
                    migrated[0].foreground == legacy.foreground &&
                    migrated[0].ansiColors == legacy.ansiColors,
                "旧单套 Codable 数据应无损迁移为两套内容相同的单套主题")

            WatchTerminalThemeStore.save(migrated, to: defaults)
            let rewritten = defaults.data(
                forKey: WatchTerminalThemeStore.storageKey) ?? Data()
            let rewrittenText =
                String(data: rewritten, encoding: .utf8) ?? ""
            expect(
                rewrittenText.contains("\"paletteMode\"") &&
                    rewrittenText.contains("\"lightOverride\"") &&
                    rewrittenText.contains("\"darkOverride\"") &&
                    WatchTerminalThemeStore.decode(rewritten) == migrated,
                "迁移后的主题应使用新结构稳定写回并可再次读取")

            var adaptive = WatchTerminalThemeStore.makeTheme(
                copyingBuiltIn: .solarized,
                existing: [])
            adaptive.lightOverride = true
            adaptive.darkOverride = true
            let encoded = WatchTerminalThemeStore.encoded([adaptive])
            var object = try! JSONSerialization.jsonObject(
                with: encoded) as! [[String: Any]]
            object[0].removeValue(forKey: "lightOverride")
            object[0].removeValue(forKey: "darkOverride")
            let priorAdaptiveData = try! JSONSerialization.data(
                withJSONObject: object)
            let migratedAdaptive =
                WatchTerminalThemeStore.decode(priorAdaptiveData)
            expect(
                migratedAdaptive.count == 1 &&
                    migratedAdaptive[0].paletteMode == .adaptive &&
                    !migratedAdaptive[0].lightOverride &&
                    !migratedAdaptive[0].darkOverride,
                "新增外观字段前保存的双套主题应默认不覆盖外壳")
        }
    }

    private static func testAdaptiveThemePersistence() {
        withDefaults { defaults in
            var theme = WatchTerminalThemeStore.makeTheme(
                copying: .solarizedLight,
                existing: [])
            theme.setPaletteMode(.adaptive)
            theme.darkPalette.background =
                WatchTerminalRGB(red: 3, green: 4, blue: 5)
            theme.darkPalette.cursor =
                WatchTerminalRGB(red: 245, green: 244, blue: 243)
            theme.lightOverride = true
            theme.darkOverride = false
            WatchTerminalThemeStore.save([theme], to: defaults)

            let selection = WatchTerminalAppearance(
                paletteSelectionID: theme.selectionID,
                customThemes: [theme],
                colorScheme: .light,
                font: .systemMonospaced,
                cursorShape: .beam,
                cursorBlinks: false)
            selection.save(to: defaults)

            let light = WatchTerminalAppearance.load(
                from: defaults,
                colorScheme: .light)
            let dark = WatchTerminalAppearance.load(
                from: defaults,
                colorScheme: .dark)
            expect(
                WatchTerminalThemeStore.load(from: defaults) == [theme] &&
                    light.palette.background ==
                        theme.lightPalette.background &&
                    dark.palette.background ==
                        theme.darkPalette.background &&
                    dark.palette.cursor == theme.darkPalette.cursor &&
                    WatchTerminalThemeStore.load(
                        from: defaults)[0].lightOverride &&
                    !WatchTerminalThemeStore.load(
                        from: defaults)[0].darkOverride,
                "双套主题、外观覆盖和当前选择应持久化，并按明暗分别解析")
        }
    }

    private static func testImportedThemes() {
        let sharedDocument = Data(
            """
            {
              "version": 1,
              "shared": {
                "foregroundColor": "#abc",
                "backgroundColor": "#1234",
                "colorPaletteOverrides": ["#010203", "#aabbccdd"]
              }
            }
            """.utf8)
        let imported = try? WatchTerminalThemeStore.importedTheme(
            from: sharedDocument,
            suggestedName: " 共享主题 ",
            existing: [])
        expect(
            imported?.name == "共享主题" &&
                imported?.paletteMode == .single &&
                imported?.lightPalette == imported?.darkPalette &&
                imported?.foreground ==
                    WatchTerminalRGB(
                        red: 0xaa,
                        green: 0xbb,
                        blue: 0xcc) &&
                imported?.background ==
                    WatchTerminalRGB(
                        red: 0x11,
                        green: 0x22,
                        blue: 0x33) &&
                imported?.cursor == imported?.foreground &&
                imported?.ansiColors[0] ==
                    WatchTerminalRGB(red: 1, green: 2, blue: 3) &&
                imported?.ansiColors[1] ==
                    WatchTerminalRGB(
                        red: 0xaa,
                        green: 0xbb,
                        blue: 0xcc) &&
                imported?.ansiColors[2] ==
                    WatchTerminalPalette.defaultDark.ansi16Colors[2],
            "iSH 单套主题应展开短颜色、忽略 alpha、补齐光标和缺省 ANSI 色")

        let adaptiveDocument = Data(
            """
            {
              "version": 1,
              "light": {
                "foregroundColor": "#112233",
                "backgroundColor": "#fefefe",
                "cursorColor": "#445566"
              },
              "dark": {
                "foregroundColor": "#ddeeff",
                "backgroundColor": "#010203",
                "cursorColor": "#abcdef"
              },
              "appearance": {
                "lightOverride": true,
                "darkOverride": 0
              }
            }
            """.utf8)
        let longName = String(repeating: "色", count: 40)
        let existing = WatchTerminalThemeStore.makeTheme(
            copying: .defaultDark,
            existing: [])
        let renamedExisting = WatchCustomTerminalTheme(
            id: existing.id,
            name: longName,
            basePaletteID: existing.basePaletteID,
            paletteMode: existing.paletteMode,
            lightPalette: existing.lightPalette,
            darkPalette: existing.darkPalette)
        let adaptive = try? WatchTerminalThemeStore.importedTheme(
            from: adaptiveDocument,
            suggestedName: longName + "被截断",
            existing: [renamedExisting])
        expect(
            adaptive?.paletteMode == .adaptive &&
                adaptive?.lightOverride == true &&
                adaptive?.darkOverride == false &&
                adaptive?.name.count == 40 &&
                adaptive?.name.hasSuffix(" 2") == true &&
                adaptive?.lightPalette.foreground ==
                    WatchTerminalRGB(
                        red: 0x11,
                        green: 0x22,
                        blue: 0x33) &&
                adaptive?.darkPalette.cursor ==
                    WatchTerminalRGB(
                        red: 0xab,
                        green: 0xcd,
                        blue: 0xef) &&
                adaptive?.lightPalette.ansiColors ==
                    WatchTerminalPalette.defaultLight.ansi16Colors &&
                adaptive?.darkPalette.ansiColors ==
                    WatchTerminalPalette.defaultDark.ansi16Colors,
            "浅深主题应保留两套配色，并生成不超长的唯一名称")

        let appearanceDocuments = [
            ##"{"version":1,"shared":{"foregroundColor":"#fff","backgroundColor":"#000"}}"##,
            ##"{"version":1,"shared":{"foregroundColor":"#fff","backgroundColor":"#000"},"appearance":{"lightOverride":true}}"##,
            ##"{"version":1,"shared":{"foregroundColor":"#fff","backgroundColor":"#000"},"appearance":"深色"}"##,
            ##"{"version":1,"shared":{"foregroundColor":"#fff","backgroundColor":"#000"},"appearance":{"lightOverride":[],"darkOverride":false}}"##,
        ]
        for document in appearanceDocuments {
            let theme = try? WatchTerminalThemeStore.importedTheme(
                from: Data(document.utf8),
                suggestedName: "兼容外观",
                existing: [])
            expect(
                theme != nil &&
                    theme?.lightOverride == false &&
                    theme?.darkOverride == false,
                "缺失或畸形 appearance 应按 iOS 语义忽略，不得拒绝有效主题")
        }

        let invalidDocuments = [
            ##"{"version":2,"shared":{"foregroundColor":"#fff","backgroundColor":"#000"}}"##,
            ##"{"version":1,"shared":{"foregroundColor":"#fff","backgroundColor":"#000"},"light":{"foregroundColor":"#000","backgroundColor":"#fff"},"dark":{"foregroundColor":"#fff","backgroundColor":"#000"}}"##,
            ##"{"version":1,"shared":{"foregroundColor":"red","backgroundColor":"#000"}}"##,
            """
            {"version":1,"shared":{
              "foregroundColor":"#fff","backgroundColor":"#000",
              "colorPaletteOverrides":[
                "#000","#000","#000","#000","#000","#000","#000","#000",
                "#000","#000","#000","#000","#000","#000","#000","#000",
                "#000"
              ]}}
            """,
        ]
        for document in invalidDocuments {
            expect(
                (try? WatchTerminalThemeStore.importedTheme(
                    from: Data(document.utf8),
                    suggestedName: "无效",
                    existing: [])) == nil,
                "无效版本、混合结构、错误颜色和过长 ANSI 表必须被拒绝")
        }
        expect(
            (try? WatchTerminalThemeStore.importedTheme(
                from: Data(
                    repeating: 0x20,
                    count:
                        WatchTerminalThemeStore.maximumImportByteCount + 1),
                suggestedName: "过大",
                existing: [])) == nil,
            "导入主题必须在解析前限制文件大小")
    }

    private static func testThemeExportRoundTrip() {
        var single = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .classicGreen,
            existing: [])
        single.name = "导出单套"
        single.darkOverride = true
        let singleData = try! WatchTerminalThemeStore.exportedData(
            for: single)
        let singleObject = try! JSONSerialization.jsonObject(
            with: singleData) as! [String: Any]
        let importedSingle =
            try! WatchTerminalThemeStore.importedTheme(
                from: singleData,
                suggestedName: single.name,
                existing: [])
        expect(
            singleObject["shared"] != nil &&
                singleObject["light"] == nil &&
                singleObject["dark"] == nil &&
                singleObject["appearance"] != nil &&
                importedSingle.paletteMode == .single &&
                importedSingle.lightPalette == single.lightPalette &&
                importedSingle.darkPalette == single.darkPalette &&
                importedSingle.lightOverride == single.lightOverride &&
                importedSingle.darkOverride == single.darkOverride,
            "单套主题导出应使用 shared，并可连同 appearance 完整导回")

        var adaptive = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .solarized,
            existing: [])
        adaptive.name = "导出双套"
        adaptive.lightOverride = true
        adaptive.darkOverride = true
        adaptive.lightPalette.cursor =
            WatchTerminalRGB(red: 1, green: 2, blue: 3)
        adaptive.darkPalette.ansiColors[15] =
            WatchTerminalRGB(red: 4, green: 5, blue: 6)
        let adaptiveData = try! WatchTerminalThemeStore.exportedData(
            for: adaptive)
        let adaptiveObject = try! JSONSerialization.jsonObject(
            with: adaptiveData) as! [String: Any]
        let importedAdaptive =
            try! WatchTerminalThemeStore.importedTheme(
                from: adaptiveData,
                suggestedName: adaptive.name,
                existing: [])
        expect(
            adaptiveObject["shared"] == nil &&
                adaptiveObject["light"] != nil &&
                adaptiveObject["dark"] != nil &&
                importedAdaptive.paletteMode == .adaptive &&
                importedAdaptive.lightPalette ==
                    adaptive.lightPalette &&
                importedAdaptive.darkPalette ==
                    adaptive.darkPalette &&
                importedAdaptive.lightOverride &&
                importedAdaptive.darkOverride,
            "双套主题导出应使用 light/dark，并完整往返两套颜色与覆盖")

        var noOverride = adaptive
        noOverride.lightOverride = false
        noOverride.darkOverride = false
        let noOverrideData =
            try! WatchTerminalThemeStore.exportedData(
                for: noOverride)
        let noOverrideObject = try! JSONSerialization.jsonObject(
            with: noOverrideData) as! [String: Any]
        expect(
            noOverrideObject["appearance"] == nil,
            "没有外观覆盖时应省略可选 appearance 字段")
    }

    private static func testThemeSharedRoundTrip() {
        let fileManager = FileManager.default
        let directory = fileManager.temporaryDirectory
            .appendingPathComponent(
                "ish-watch-theme-roundtrip-\(UUID().uuidString)",
                isDirectory: true)
        do {
            try fileManager.createDirectory(
                at: directory,
                withIntermediateDirectories: false)
        } catch {
            expect(false, "无法创建主题共享往返测试目录：\(error)")
            return
        }
        defer {
            try? fileManager.removeItem(at: directory)
        }

        var source = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .solarized,
            existing: [])
        source.name = "共享往返主题"
        source.lightOverride = true
        source.darkOverride = true
        source.lightPalette.foreground =
            WatchTerminalRGB(red: 0x12, green: 0x34, blue: 0x56)
        source.darkPalette.cursor =
            WatchTerminalRGB(red: 0xab, green: 0xcd, blue: 0xef)

        do {
            let exported = try WatchTerminalThemeStore.exportedData(
                for: source)
            let sharedFile = try WatchSharedFiles.write(
                exported,
                preferredFileName: source.name,
                to: directory)
            let restoredData = try WatchSharedFiles.read(
                sharedFile,
                from: directory,
                maximumByteCount:
                    WatchTerminalThemeStore.maximumImportByteCount)
            let restored = try WatchTerminalThemeStore.importedTheme(
                from: restoredData,
                suggestedName: source.name,
                existing: [])
            let residue = try fileManager.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: nil)
                .filter { $0.lastPathComponent.hasSuffix(".partial") }

            expect(
                sharedFile.name == "共享往返主题.json" &&
                    restored.paletteMode == source.paletteMode &&
                    restored.lightPalette == source.lightPalette &&
                    restored.darkPalette == source.darkPalette &&
                    restored.lightOverride == source.lightOverride &&
                    restored.darkOverride == source.darkOverride &&
                    residue.isEmpty,
                "主题必须经 Shared 原子写入与安全读取后完整导回")
        } catch {
            expect(false, "主题 Shared 完整往返失败：\(error)")
        }
    }

    private static func testCorruptCustomThemeFallback() {
        withDefaults { defaults in
            defaults.set(
                Data("{\"themes\":\"broken\"}".utf8),
                forKey: WatchTerminalThemeStore.storageKey)
            expect(
                WatchTerminalThemeStore.load(from: defaults).isEmpty,
                "损坏或版本不匹配的主题数据应回落为空列表")

            defaults.set(
                "not-data",
                forKey: WatchTerminalThemeStore.storageKey)
            expect(
                WatchTerminalThemeStore.load(from: defaults).isEmpty,
                "UserDefaults 中错误类型的主题值应被忽略")

            let palette = WatchTerminalPalette.defaultDark
            let malformedLegacy = LegacyWatchCustomTerminalTheme(
                id: UUID(),
                name: "损坏主题",
                basePaletteID: .defaultDark,
                foreground: palette.foreground,
                background: palette.background,
                cursor: palette.cursor,
                ansiColors: Array(
                    palette.ansi16Colors.dropLast()))
            defaults.set(
                try! JSONEncoder().encode([malformedLegacy]),
                forKey: WatchTerminalThemeStore.storageKey)
            expect(
                WatchTerminalThemeStore.load(from: defaults).isEmpty,
                "ANSI 色数量错误的旧主题应被安全拒绝而不是触发崩溃")
        }
    }
}

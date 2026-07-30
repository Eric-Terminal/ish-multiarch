import SwiftUI

private struct WatchTerminalColorSchemeKey: EnvironmentKey {
    static let defaultValue = WatchTerminalThemeColorScheme.dark
}

extension EnvironmentValues {
    var watchTerminalColorScheme: WatchTerminalThemeColorScheme {
        get { self[WatchTerminalColorSchemeKey.self] }
        set { self[WatchTerminalColorSchemeKey.self] = newValue }
    }
}

private enum WatchThemePaletteRole {
    case light
    case dark

    var colorScheme: WatchTerminalThemeColorScheme {
        switch self {
        case .light:
            return .light
        case .dark:
            return .dark
        }
    }

    func palette(
        in theme: WatchCustomTerminalTheme
    ) -> WatchTerminalThemePalette {
        switch self {
        case .light:
            return theme.lightPalette
        case .dark:
            return theme.darkPalette
        }
    }

    func setPalette(
        _ palette: WatchTerminalThemePalette,
        in theme: inout WatchCustomTerminalTheme
    ) {
        theme.setPalette(palette, for: colorScheme)
    }
}

private enum WatchThemeConfirmation: Equatable {
    case singlePalette
    case deletion
}

struct WatchTerminalThemesView: View {
    @Environment(\.watchTerminalColorScheme)
    private var activeColorScheme
    @Binding var selectionID: String
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()

    private var customThemes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }

    var body: some View {
        List {
            Section("内置主题") {
                ForEach(
                    WatchTerminalBuiltInThemeID.allCases,
                    id: \.rawValue
                ) { themeID in
                    NavigationLink {
                        WatchBuiltInThemeDetailView(
                            themeID: themeID,
                            selectionID: $selectionID)
                    } label: {
                        themeRow(
                            name: themeID.displayName,
                            palette: themeID.palette(
                                for: activeColorScheme),
                            isAdaptive: themeID.isAdaptive,
                            isSelected: themeID.matches(
                                selectionID: selectionID))
                    }
                    .accessibilityIdentifier(
                        "watch-built-in-theme-\(themeID.rawValue)")
                }
            }

            Section {
                if customThemes.isEmpty {
                    Text("还没有自定义主题")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(customThemes) { theme in
                        NavigationLink {
                            WatchCustomThemeEditorView(
                                themeID: theme.id,
                                selectionID: $selectionID)
                        } label: {
                            themeRow(
                                name: theme.name,
                                palette: theme.palette(
                                    for: activeColorScheme),
                                isAdaptive:
                                    theme.paletteMode == .adaptive,
                                isSelected: selectionID ==
                                    theme.selectionID)
                        }
                        .accessibilityIdentifier(
                            "watch-custom-theme-\(theme.id.uuidString)")
                    }
                }

                Button {
                    createTheme()
                } label: {
                    Label("新建自定义主题", systemImage: "plus")
                }
                .accessibilityIdentifier("create-watch-custom-theme")

                NavigationLink {
                    WatchTerminalThemeImportView(
                        selectionID: $selectionID)
                } label: {
                    Label(
                        "从共享文件导入",
                        systemImage: "square.and.arrow.down")
                }
                .accessibilityIdentifier("import-watch-custom-theme")
            } header: {
                Text("自定义主题")
            } footer: {
                Text(
                    "新建主题继承默认主题的浅色与深色配色，" +
                    "也可复制内置主题，或从 /mnt/shared 导入 JSON。")
            }
        }
        .navigationTitle("终端主题")
        .accessibilityIdentifier("watch-terminal-themes-view")
    }

    private func themeRow(
        name: String,
        palette: WatchTerminalPalette,
        isAdaptive: Bool,
        isSelected: Bool
    ) -> some View {
        HStack(spacing: 8) {
            WatchThemeSwatch(
                foreground: palette.foreground,
                background: palette.background)
            Text(name)
                .lineLimit(1)
            if isAdaptive {
                Image(systemName: "circle.lefthalf.filled")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
            }
            Spacer()
            if isSelected {
                Image(systemName: "checkmark")
                    .foregroundStyle(Color.accentColor)
            }
        }
    }

    private func createTheme() {
        var themes = customThemes
        let theme = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: .defaultTheme,
            existing: themes,
            usesGenericName: true)
        themes.append(theme)
        customThemesData = WatchTerminalThemeStore.encoded(themes)
        selectionID = theme.selectionID
    }
}

private struct WatchBuiltInThemeDetailView: View {
    @Environment(\.dismiss) private var dismiss
    let themeID: WatchTerminalBuiltInThemeID
    @Binding var selectionID: String
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()

    var body: some View {
        List {
            Section("预览") {
                builtInPreview(
                    title: themeID.isAdaptive ? "浅色" : "单套配色",
                    colorScheme: .light)
                if themeID.isAdaptive {
                    builtInPreview(
                        title: "深色",
                        colorScheme: .dark)
                }
            }

            Section {
                Button {
                    selectionID = themeID.rawValue
                } label: {
                    Label(
                        isSelected ?
                            "正在使用" : "使用此主题",
                        systemImage:
                            isSelected ?
                            "checkmark.circle.fill" : "circle")
                }
                .disabled(isSelected)
                .accessibilityIdentifier("watch-built-in-theme-use")

                Button {
                    copyTheme()
                    dismiss()
                } label: {
                    Label(
                        "复制为自定义主题",
                        systemImage: "doc.on.doc")
                }
                .accessibilityIdentifier("copy-watch-built-in-theme")
            } footer: {
                Text(detailExplanation)
            }
        }
        .navigationTitle(themeID.displayName)
    }

    private var isSelected: Bool {
        themeID.matches(selectionID: selectionID)
    }

    private var detailExplanation: String {
        if themeID.isAdaptive {
            return "此主题会跟随 App 外观切换完整的浅色与深色配色；" +
                "复制后仍可分别调整两套颜色。"
        }
        if themeID == .classicGreen {
            return "经典绿色在“跟随系统”时使用深色界面，" +
                "并在两种外观下共用一套终端配色。"
        }
        return "此主题在两种 App 外观下共用一套配色；" +
            "复制后可调整全部颜色。"
    }

    private func builtInPreview(
        title: String,
        colorScheme: WatchTerminalThemeColorScheme
    ) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.caption2)
                .foregroundStyle(.secondary)
            WatchAppearancePreview(
                appearance: WatchTerminalAppearance(
                    builtInThemeID: themeID,
                    colorScheme: colorScheme,
                    font: .systemMonospaced,
                    cursorShape: .block,
                    cursorBlinks: false),
                fontSize: 11)
                .frame(height: 56)
                .clipShape(RoundedRectangle(cornerRadius: 8))
        }
    }

    private func copyTheme() {
        var themes = WatchTerminalThemeStore.decode(customThemesData)
        let theme = WatchTerminalThemeStore.makeTheme(
            copyingBuiltIn: themeID,
            existing: themes)
        themes.append(theme)
        customThemesData = WatchTerminalThemeStore.encoded(themes)
        selectionID = theme.selectionID
    }
}

private struct WatchCustomThemeEditorView: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.watchTerminalColorScheme)
    private var activeColorScheme
    let themeID: UUID
    @Binding var selectionID: String
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()
    @State private var validationMessage: String?
    @State private var pendingConfirmation: WatchThemeConfirmation?
    @State private var exportErrorMessage: String?
    @State private var exportedFileName: String?

    private var themes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }

    private var theme: WatchCustomTerminalTheme? {
        themes.first { $0.id == themeID }
    }

    var body: some View {
        List {
            if let theme {
                Section("预览") {
                    WatchAppearancePreview(
                        appearance: appearance(
                            for: theme,
                            colorScheme: activeColorScheme),
                        fontSize: 11)
                        .frame(height: 64)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                }

                Section("主题") {
                    TextFieldLink(prompt: Text(theme.name)) {
                        LabeledContent("名称", value: theme.name)
                    } onSubmit: { value in
                        renameTheme(value)
                    }
                    .accessibilityIdentifier("watch-custom-theme-name")
                }

                Section {
                    Toggle(
                        "分别设置浅色与深色",
                        isOn: adaptiveBinding(for: theme))
                        .accessibilityIdentifier(
                            "watch-custom-theme-adaptive-toggle")
                } header: {
                    Text("配色模式")
                } footer: {
                    Text(modeExplanation(for: theme.paletteMode))
                }

                Section("调色板") {
                    if theme.paletteMode == .adaptive {
                        paletteLink(
                            theme: theme,
                            role: .light,
                            title: "浅色主配色",
                            identifier:
                                "watch-custom-theme-light-palette")
                        paletteLink(
                            theme: theme,
                            role: .dark,
                            title: "深色配色",
                            identifier:
                                "watch-custom-theme-dark-palette")
                    } else {
                        paletteLink(
                            theme: theme,
                            role: .light,
                            title: "单套配色",
                            identifier:
                                "watch-custom-theme-primary-palette")
                    }
                }

                Section {
                    Toggle(
                        "浅色系统显示深色",
                        isOn: overrideBinding(
                            for: theme,
                            keyPath: \.lightOverride))
                        .accessibilityIdentifier(
                            "watch-custom-theme-light-override")
                    Toggle(
                        "深色系统显示浅色",
                        isOn: overrideBinding(
                            for: theme,
                            keyPath: \.darkOverride))
                        .accessibilityIdentifier(
                            "watch-custom-theme-dark-override")
                } header: {
                    Text("外观覆盖")
                } footer: {
                    Text(
                        "仅在 App 外观为“跟随系统”时改变界面明暗。" +
                        "终端配色仍按系统外观选择。")
                }

                Section {
                    Button {
                        selectionID = theme.selectionID
                    } label: {
                        Label(
                            selectionID == theme.selectionID ?
                                "正在使用" : "使用此主题",
                            systemImage:
                                selectionID == theme.selectionID ?
                                "checkmark.circle.fill" : "circle")
                    }
                    .disabled(selectionID == theme.selectionID)
                    .accessibilityIdentifier("watch-custom-theme-use")

                    Button {
                        duplicate(theme)
                    } label: {
                        Label("复制主题", systemImage: "doc.on.doc")
                    }
                    .accessibilityIdentifier("duplicate-watch-custom-theme")

                    Button {
                        exportTheme(theme)
                    } label: {
                        Label(
                            "导出到共享文件",
                            systemImage: "square.and.arrow.up")
                    }
                    .accessibilityIdentifier("export-watch-custom-theme")

                    Button(role: .destructive) {
                        pendingConfirmation = .deletion
                    } label: {
                        Label("删除主题", systemImage: "trash")
                    }
                    .accessibilityIdentifier("watch-custom-theme-delete")
                }
            } else {
                ContentUnavailableView(
                    "主题不存在",
                    systemImage: "paintpalette")
            }
        }
        .navigationTitle(theme?.name ?? "自定义主题")
        .accessibilityIdentifier("watch-custom-theme-editor")
        .alert(
            "无法保存",
            isPresented: Binding(
                get: { validationMessage != nil },
                set: { if !$0 { validationMessage = nil } })
        ) {
            Button("好") {
                validationMessage = nil
            }
        } message: {
            Text(validationMessage ?? "")
        }
        .alert(
            "无法导出主题",
            isPresented: Binding(
                get: { exportErrorMessage != nil },
                set: { if !$0 { exportErrorMessage = nil } })
        ) {
            Button("好") {
                exportErrorMessage = nil
            }
        } message: {
            Text(exportErrorMessage ?? "")
        }
        .alert(
            "主题已导出",
            isPresented: Binding(
                get: { exportedFileName != nil },
                set: { if !$0 { exportedFileName = nil } })
        ) {
            Button("好") {
                exportedFileName = nil
            }
        } message: {
            Text(
                "已保存到 /mnt/shared/" +
                (exportedFileName ?? "") +
                "，可在“共享文件”中分享。")
        }
        .confirmationDialog(
            confirmationTitle,
            isPresented: confirmationIsPresented,
            titleVisibility: .visible
        ) {
            if pendingConfirmation == .singlePalette {
                Button("保留浅色主配色", role: .destructive) {
                    updateTheme {
                        $0.setPaletteMode(.single)
                    }
                    pendingConfirmation = nil
                }
            } else if pendingConfirmation == .deletion {
                Button("永久删除", role: .destructive) {
                    deleteTheme()
                }
            }
            Button("取消", role: .cancel) {
                pendingConfirmation = nil
            }
        } message: {
            Text(confirmationMessage)
        }
    }

    private var confirmationIsPresented: Binding<Bool> {
        Binding(
            get: {
                pendingConfirmation != nil
            },
            set: { isPresented in
                if !isPresented {
                    pendingConfirmation = nil
                }
            })
    }

    private var confirmationTitle: String {
        switch pendingConfirmation {
        case .singlePalette:
            return "切回单套配色？"
        case .deletion:
            return "删除这个主题？"
        case nil:
            return ""
        }
    }

    private var confirmationMessage: String {
        switch pendingConfirmation {
        case .singlePalette:
            return "深色配色会被浅色主配色替换。"
        case .deletion:
            return "删除后无法恢复，终端内容不会受影响。"
        case nil:
            return ""
        }
    }

    private func adaptiveBinding(
        for theme: WatchCustomTerminalTheme
    ) -> Binding<Bool> {
        Binding(
            get: {
                theme.paletteMode == .adaptive
            },
            set: { isAdaptive in
                if isAdaptive {
                    updateTheme {
                        $0.setPaletteMode(.adaptive)
                    }
                } else {
                    pendingConfirmation = .singlePalette
                }
            })
    }

    private func overrideBinding(
        for theme: WatchCustomTerminalTheme,
        keyPath: WritableKeyPath<WatchCustomTerminalTheme, Bool>
    ) -> Binding<Bool> {
        Binding(
            get: {
                theme[keyPath: keyPath]
            },
            set: { value in
                updateTheme {
                    $0[keyPath: keyPath] = value
                }
            })
    }

    private func modeExplanation(
        for mode: WatchTerminalThemePaletteMode
    ) -> String {
        switch mode {
        case .single:
            return "两种 App 外观共用一套配色；" +
                "开启时会复制它作为深色配色。"
        case .adaptive:
            return "浅色为主配色；关闭时会用它替换深色配色。"
        }
    }

    private func paletteLink(
        theme: WatchCustomTerminalTheme,
        role: WatchThemePaletteRole,
        title: String,
        identifier: String
    ) -> some View {
        let palette = role.palette(in: theme)
        return NavigationLink {
            WatchTerminalPaletteEditorView(
                themeID: themeID,
                role: role,
                title: title)
        } label: {
            HStack(spacing: 8) {
                WatchThemeSwatch(
                    foreground: palette.foreground,
                    background: palette.background)
                Text(title)
            }
        }
        .accessibilityIdentifier(identifier)
    }

    private func appearance(
        for theme: WatchCustomTerminalTheme,
        colorScheme: WatchTerminalThemeColorScheme
    ) -> WatchTerminalAppearance {
        WatchTerminalAppearance(
            paletteSelectionID: theme.selectionID,
            customThemes: [theme],
            colorScheme: colorScheme,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
    }

    private func renameTheme(_ proposedName: String) {
        if let issue = WatchTerminalThemeStore.nameIssue(
            for: proposedName,
            excluding: themeID,
            in: themes
        ) {
            validationMessage = issue.message
            return
        }
        updateTheme {
            $0.name = WatchTerminalThemeStore.normalizedName(proposedName)
        }
    }

    private func updateTheme(
        _ update: (inout WatchCustomTerminalTheme) -> Void
    ) {
        var updatedThemes = themes
        guard let index = updatedThemes.firstIndex(where: {
            $0.id == themeID
        }) else {
            return
        }
        update(&updatedThemes[index])
        customThemesData = WatchTerminalThemeStore.encoded(updatedThemes)
    }

    private func duplicate(_ source: WatchCustomTerminalTheme) {
        var updatedThemes = themes
        let copy = WatchTerminalThemeStore.duplicate(
            source,
            existing: updatedThemes)
        updatedThemes.append(copy)
        customThemesData = WatchTerminalThemeStore.encoded(updatedThemes)
        selectionID = copy.selectionID
        dismiss()
    }

    private func exportTheme(_ theme: WatchCustomTerminalTheme) {
        do {
            let data = try WatchTerminalThemeStore.exportedData(
                for: theme)
            let file = try WatchSharedFiles.writeSystemFile(
                data,
                preferredFileName: theme.name)
            exportErrorMessage = nil
            exportedFileName = file.name
        } catch {
            exportedFileName = nil
            exportErrorMessage = error.localizedDescription
        }
    }

    private func deleteTheme() {
        let currentSelectionID = theme?.selectionID
        customThemesData = WatchTerminalThemeStore.encoded(
            themes.filter { $0.id != themeID })
        if selectionID == currentSelectionID {
            selectionID =
                WatchTerminalBuiltInThemeID.defaultTheme.rawValue
        }
        dismiss()
    }
}

private struct WatchTerminalPaletteEditorView: View {
    let themeID: UUID
    let role: WatchThemePaletteRole
    let title: String
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()
    @State private var validationMessage: String?

    private var themes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }

    private var theme: WatchCustomTerminalTheme? {
        themes.first { $0.id == themeID }
    }

    private var palette: WatchTerminalThemePalette? {
        theme.map { role.palette(in: $0) }
    }

    var body: some View {
        List {
            if let theme, let palette {
                Section("预览") {
                    WatchAppearancePreview(
                        appearance: appearance(for: theme),
                        fontSize: 11)
                        .frame(height: 64)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                }

                Section("主要颜色") {
                    colorField(
                        "前景",
                        color: palette.foreground,
                        identifier: "watch-custom-theme-foreground"
                    ) { value in
                        updatePalette { $0.foreground = value }
                    }

                    colorField(
                        "背景",
                        color: palette.background,
                        identifier: "watch-custom-theme-background"
                    ) { value in
                        updatePalette { $0.background = value }
                    }

                    colorField(
                        "光标",
                        color: palette.cursor,
                        identifier: "watch-custom-theme-cursor"
                    ) { value in
                        updatePalette { $0.cursor = value }
                    }
                }

                Section("ANSI 颜色") {
                    NavigationLink {
                        WatchANSIColorSettingsView(
                            themeID: themeID,
                            role: role,
                            range: 0..<8,
                            title: "标准色")
                    } label: {
                        Label("标准色 0–7", systemImage: "circle.grid.2x2")
                    }
                    .accessibilityIdentifier(
                        "watch-custom-theme-ansi-normal")

                    NavigationLink {
                        WatchANSIColorSettingsView(
                            themeID: themeID,
                            role: role,
                            range: 8..<16,
                            title: "高亮色")
                    } label: {
                        Label(
                            "高亮色 8–15",
                            systemImage: "circle.grid.2x2.fill")
                    }
                    .accessibilityIdentifier(
                        "watch-custom-theme-ansi-bright")
                }
            } else {
                ContentUnavailableView(
                    "配色不存在",
                    systemImage: "paintpalette")
            }
        }
        .navigationTitle(title)
        .accessibilityIdentifier("watch-custom-theme-palette-editor")
        .alert(
            "无法保存",
            isPresented: Binding(
                get: { validationMessage != nil },
                set: { if !$0 { validationMessage = nil } })
        ) {
            Button("好") {
                validationMessage = nil
            }
        } message: {
            Text(validationMessage ?? "")
        }
    }

    private func appearance(
        for theme: WatchCustomTerminalTheme
    ) -> WatchTerminalAppearance {
        WatchTerminalAppearance(
            paletteSelectionID: theme.selectionID,
            customThemes: [theme],
            colorScheme: role.colorScheme,
            font: .systemMonospaced,
            cursorShape: .block,
            cursorBlinks: false)
    }

    private func colorField(
        _ title: String,
        color: WatchTerminalRGB,
        identifier: String,
        onSubmit: @escaping (WatchTerminalRGB) -> Void
    ) -> some View {
        TextFieldLink(prompt: Text(color.hexString)) {
            HStack {
                Text(title)
                Spacer()
                WatchThemeColorChip(color: color)
                Text(verbatim: color.hexString)
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(.secondary)
            }
        } onSubmit: { value in
            guard let color = WatchTerminalRGB(hexString: value) else {
                validationMessage =
                    "颜色必须使用 #RRGGBB 格式，例如 #33FF66。"
                return
            }
            onSubmit(color)
        }
        .textInputAutocapitalization(.characters)
        .autocorrectionDisabled()
        .accessibilityIdentifier(identifier)
    }

    private func updatePalette(
        _ update: (inout WatchTerminalThemePalette) -> Void
    ) {
        var updatedThemes = themes
        guard let index = updatedThemes.firstIndex(where: {
            $0.id == themeID
        }) else {
            return
        }
        var palette = role.palette(in: updatedThemes[index])
        update(&palette)
        role.setPalette(palette, in: &updatedThemes[index])
        customThemesData = WatchTerminalThemeStore.encoded(updatedThemes)
    }
}

private struct WatchANSIColorSettingsView: View {
    let themeID: UUID
    let role: WatchThemePaletteRole
    let range: Range<Int>
    let title: String
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()
    @State private var validationMessage: String?

    private var themes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }

    var body: some View {
        List {
            ForEach(Array(range), id: \.self) { index in
                if let color = color(at: index) {
                    TextFieldLink(prompt: Text(color.hexString)) {
                        HStack {
                            Text(ansiName(at: index))
                            Spacer()
                            WatchThemeColorChip(color: color)
                            Text(verbatim: color.hexString)
                                .font(.system(
                                    .caption2,
                                    design: .monospaced))
                                .foregroundStyle(.secondary)
                        }
                    } onSubmit: { value in
                        save(value, at: index)
                    }
                    .textInputAutocapitalization(.characters)
                    .autocorrectionDisabled()
                    .accessibilityIdentifier(
                        "watch-custom-theme-ansi-color-\(index)")
                }
            }
        }
        .navigationTitle(title)
        .alert(
            "无法保存",
            isPresented: Binding(
                get: { validationMessage != nil },
                set: { if !$0 { validationMessage = nil } })
        ) {
            Button("好") {
                validationMessage = nil
            }
        } message: {
            Text(validationMessage ?? "")
        }
    }

    private func color(at index: Int) -> WatchTerminalRGB? {
        guard let theme = themes.first(where: { $0.id == themeID }) else {
            return nil
        }
        let palette = role.palette(in: theme)
        guard palette.ansiColors.indices.contains(index) else {
            return nil
        }
        return palette.ansiColors[index]
    }

    private func save(_ value: String, at index: Int) {
        guard let color = WatchTerminalRGB(hexString: value) else {
            validationMessage =
                "颜色必须使用 #RRGGBB 格式，例如 #33FF66。"
            return
        }
        var updatedThemes = themes
        guard let themeIndex = updatedThemes.firstIndex(where: {
            $0.id == themeID
        }) else {
            return
        }
        var palette = role.palette(in: updatedThemes[themeIndex])
        palette.ansiColors[index] = color
        role.setPalette(palette, in: &updatedThemes[themeIndex])
        customThemesData = WatchTerminalThemeStore.encoded(updatedThemes)
    }

    private func ansiName(at index: Int) -> String {
        let names = [
            "黑", "红", "绿", "黄",
            "蓝", "紫", "青", "白",
        ]
        let prefix = index < 8 ? "" : "亮"
        return "\(prefix)\(names[index % 8])"
    }
}

struct WatchAppearancePreview: View {
    let appearance: WatchTerminalAppearance
    let fontSize: Double

    private var screen: TerminalScreen {
        var screen = TerminalScreen(columns: 30, rows: 3)
        screen.append(Array(
            (
                "\u{1b}[32mroot@watch\u{1b}[0m:~# " +
                "\u{1b}[36muname -m\u{1b}[0m\r\naarch64"
            ).utf8))
        return screen
    }

    var body: some View {
        WatchTerminalView(
            screen: screen,
            appearance: appearance,
            fontSize: fontSize,
            followsOutput: false)
            .allowsHitTesting(false)
    }
}

private struct WatchThemeSwatch: View {
    let foreground: WatchTerminalRGB
    let background: WatchTerminalRGB

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 4)
                .fill(watchColor(background))
            Text("A")
                .font(.system(.caption2, design: .monospaced))
                .foregroundStyle(watchColor(foreground))
        }
        .frame(width: 27, height: 21)
        .overlay {
            RoundedRectangle(cornerRadius: 4)
                .stroke(.secondary.opacity(0.4), lineWidth: 0.5)
        }
    }
}

private struct WatchThemeColorChip: View {
    let color: WatchTerminalRGB

    var body: some View {
        Circle()
            .fill(watchColor(color))
            .frame(width: 15, height: 15)
            .overlay {
                Circle()
                    .stroke(.secondary.opacity(0.4), lineWidth: 0.5)
            }
    }
}

private func watchColor(_ color: WatchTerminalRGB) -> Color {
    Color(
        .sRGB,
        red: color.redComponent,
        green: color.greenComponent,
        blue: color.blueComponent,
        opacity: 1)
}

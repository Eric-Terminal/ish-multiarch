import Foundation
import SwiftUI

struct ContentView: View {
    // watchOS 没有系统级浅色外观；SwiftUI 的 colorScheme 会被
    // preferredColorScheme 改写，不能反过来当作系统来源。
    private static let nativeSystemIsDark = true

    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject var runtime: WatchRuntime

    @AppStorage("watchTerminalFontSize")
    private var terminalFontSize = 11.0
    @AppStorage(WatchPreferenceKeys.colorScheme)
    private var colorSchemePreference = "system"
    @AppStorage(WatchTerminalAppearancePreferenceKey.palette)
    private var paletteID =
        WatchTerminalAppearance.standard.paletteID.rawValue
    @AppStorage(WatchTerminalAppearancePreferenceKey.font)
    private var terminalFont =
        WatchTerminalAppearance.standard.font.rawValue
    @AppStorage(WatchTerminalAppearancePreferenceKey.cursorShape)
    private var cursorShape =
        WatchTerminalAppearance.standard.cursorShape.rawValue
    @AppStorage(WatchTerminalAppearancePreferenceKey.cursorBlink)
    private var cursorBlinks =
        WatchTerminalAppearance.standard.cursorBlinks
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()

    @State private var command = ""
    @State private var dockPage = 1
    @State private var followsOutput = true
    @State private var isShowingSettings = false
    @State private var isShowingSessions = false
    @State private var isShowingKeypad = false
    @State private var isFocusMode = false

    var body: some View {
        NavigationStack {
            ZStack {
                terminalBackground
                    .ignoresSafeArea()

                terminalContent

                if !followsOutput,
                   runtime.activeSession?.hasOutput == true {
                    returnToLatestButton
                }
            }
            .safeAreaInset(edge: .bottom, spacing: 2) {
                terminalDock
            }
            .toolbar {
                if !isFocusMode {
                    ToolbarItem(placement: .topBarLeading) {
                        Button {
                            isShowingSettings = true
                        } label: {
                            Image(systemName: "gearshape.fill")
                                .overlay(alignment: .topTrailing) {
                                    WatchRepositoryStatusBadge(
                                        indicator:
                                            repositoryStatus.indicator,
                                        compact: true)
                                        .offset(x: 3, y: -2)
                                        .accessibilityHidden(true)
                                }
                        }
                        .accessibilityLabel("设置")
                        .accessibilityValue(
                            repositoryStatus.accessibilitySummary)
                        .accessibilityIdentifier("watch-settings-button")
                    }

                    ToolbarItem(placement: .topBarTrailing) {
                        Button {
                            isShowingSessions = true
                        } label: {
                            HStack(spacing: 4) {
                                Circle()
                                    .fill(statusColor)
                                    .frame(width: 7, height: 7)
                                Image(systemName: "rectangle.stack")
                            }
                        }
                        .accessibilityLabel("终端会话")
                        .accessibilityValue(
                            "\(runtime.sessionSnapshots.count) 个，\(runtime.status)")
                        .accessibilityIdentifier("watch-sessions-button")
                    }
                }
            }
        }
        .sheet(isPresented: $isShowingSettings) {
            WatchSettingsView(
                runtime: runtime,
                terminalFontSize: $terminalFontSize)
        }
        .sheet(isPresented: $isShowingSessions) {
            WatchSessionsView(runtime: runtime)
        }
        .sheet(isPresented: $isShowingKeypad) {
            TerminalKeypadView(
                runtime: runtime,
                command: $command)
        }
        .task(id: scenePhase) {
            let isActive = scenePhase == .active
            runtime.setForegroundActive(isActive)
            guard isActive else { return }
            await runtime.run()
        }
        .onChange(of: runtime.selectedSessionID) { _, _ in
            command = ""
            followsOutput = true
            dockPage = isFocusMode ? 3 : 1
        }
        .onChange(of: runtime.canReopenSession) { _, canReopen in
            if canReopen {
                command = ""
                dockPage = isFocusMode ? 3 : 1
            }
        }
        .environment(
            \.watchTerminalColorScheme,
            themeAppearanceResolution.terminalColorScheme)
        .preferredColorScheme(preferredColorScheme)
    }

    private var repositoryStatus:
        WatchAPKRepositoryStatusPresentation
    {
        WatchAPKRepositoryStatusPresentation(
            coordinator: runtime.apkRepositoryMigration)
    }

    @ViewBuilder
    private var terminalContent: some View {
        if let session = runtime.activeSession {
            WatchTerminalView(
                snapshot: session,
                appearance: appearance,
                fontSize: terminalFontSize,
                showsCursor: session.acceptsInput,
                followsOutput: followsOutput,
                onManualScroll: {
                    followsOutput = false
                },
                onViewportChange: { columns, rows in
                    runtime.resizeSession(
                        session.id,
                        columns: columns,
                        rows: rows)
                })
                .id(session.id)

            if !session.hasOutput {
                TerminalEmptyState(
                    title: session.title,
                    status: runtime.status,
                    isFailure: session.canReopenSession ||
                        runtime.hasRuntimeFailure,
                    retryAction: runtime.canRetryPreparation ?
                        { runtime.retryPreparation() } : nil)
            }
        } else {
            TerminalEmptyState(
                title: "iSH",
                status: runtime.status,
                isFailure: runtime.hasRuntimeFailure,
                retryAction: runtime.canRetryPreparation ?
                    { runtime.retryPreparation() } : nil)
        }
    }

    private var terminalDock: some View {
        ZStack {
            Capsule()
                .fill(.thinMaterial)
            Capsule()
                .stroke(.secondary.opacity(0.35), lineWidth: 0.5)

            TabView(selection: $dockPage) {
                leadingShortcutPage
                    .padding(.horizontal, 10)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .tag(0)
                commandPage
                    .padding(.horizontal, 10)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .tag(1)
                trailingShortcutPage
                    .padding(.horizontal, 10)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .tag(2)
                applicationShortcutPage
                    .padding(.horizontal, 10)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .tag(3)
            }
            .tabViewStyle(.page(indexDisplayMode: .never))
            .frame(height: 38)
            .clipped()

            HStack {
                Image(systemName: "chevron.left")
                    .opacity(dockPage > 0 ? 1 : 0)
                Spacer()
                Image(systemName: "chevron.right")
                    .opacity(dockPage < 3 ? 1 : 0)
            }
            .font(.system(size: 7, weight: .semibold))
            .foregroundStyle(.secondary.opacity(0.65))
            .padding(.horizontal, 3)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        }
        .frame(height: 38)
        .padding(.horizontal, 4)
        .contentShape(Capsule())
        .accessibilityElement(children: .contain)
        .accessibilityLabel("终端快捷栏")
        .accessibilityValue(dockPageAccessibilityValue)
        .accessibilityHint("左右轻扫切换快捷栏页面")
        .accessibilityAdjustableAction(adjustDockPage)
        .accessibilityIdentifier("watch-terminal-dock")
    }

    private var commandPage: some View {
        HStack(spacing: 6) {
            Group {
                if runtime.canReopenSession {
                    reopenButton
                } else if runtime.activeSession == nil {
                    createFirstSessionButton
                } else {
                    commandField
                }
            }
            .frame(minWidth: 0, maxWidth: .infinity)
            .clipped()

            if runtime.activeSession != nil &&
                    !runtime.canReopenSession {
                Button(action: sendReturn) {
                    Image(systemName: "arrow.up")
                        .font(.system(size: 14, weight: .bold))
                        .frame(width: 29, height: 29)
                        .background(Color.accentColor, in: Circle())
                        .foregroundStyle(.white)
                        .frame(width: 36, height: 36)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .fixedSize()
                .layoutPriority(1)
                .zIndex(1)
                .disabled(!runtime.acceptsInput)
                .accessibilityLabel("回车")
                .accessibilityHint("向当前终端发送回车")
                .accessibilityIdentifier("send-command")
            }
        }
        .frame(maxWidth: .infinity)
    }

    private var commandField: some View {
        TextFieldLink(prompt: Text("命令")) {
            HStack(spacing: 5) {
                Image(systemName: "keyboard")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(command.isEmpty ? "命令" : command)
                    .foregroundStyle(
                        command.isEmpty ? .secondary : .primary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxWidth: .infinity, minHeight: 30)
            .contentShape(Rectangle())
        } onSubmit: { submittedText in
            // Quickboard 完成只提交文字；Enter 由圆形按钮独立发送。
            if runtime.sendText(submittedText) {
                command = ""
            } else {
                command = submittedText
            }
            followsOutput = true
        }
        .buttonStyle(.plain)
        .font(.body)
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
        .frame(minWidth: 0, maxWidth: .infinity)
        .disabled(!runtime.acceptsInput)
        .accessibilityLabel("命令")
        .accessibilityValue(command)
        .accessibilityHint("完成只发送文字，使用旁边的回车键执行")
        .accessibilityIdentifier("command-input")
    }

    private var reopenButton: some View {
        Button {
            command = ""
            followsOutput = true
            runtime.reopenSession()
        } label: {
            Label("重新打开终端", systemImage: "arrow.clockwise")
                .font(.caption.weight(.semibold))
                .frame(maxWidth: .infinity, minHeight: 30)
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("reopen-terminal-session")
    }

    private var createFirstSessionButton: some View {
        Button {
            if runtime.createSession() != nil {
                followsOutput = true
            }
        } label: {
            Label("新建终端", systemImage: "plus")
                .font(.caption.weight(.semibold))
                .frame(maxWidth: .infinity, minHeight: 30)
        }
        .buttonStyle(.plain)
        .disabled(!runtime.canCreateSession)
        .accessibilityIdentifier("create-first-watch-session")
    }

    private var leadingShortcutPage: some View {
        HStack(spacing: 5) {
            shortcutButton(
                "⌃C",
                identifier: "send-control-c",
                accessibilityLabel: "Control-C",
                accessibilityHint: "中断当前前台程序"
            ) {
                interrupt()
            }
            shortcutButton(
                "Tab",
                identifier: "send-tab",
                accessibilityHint: "发送制表符，Shell 中通常用于命令补全"
            ) {
                complete()
            }
            shortcutButton(
                "Esc",
                identifier: "send-escape",
                accessibilityLabel: "Escape",
                accessibilityHint: "向当前终端发送 Escape"
            ) {
                escape()
            }
            shortcutButton(
                "更多",
                systemImage: "ellipsis",
                identifier: "show-more-terminal-keys",
                accessibilityHint: "打开完整的 Ctrl、Meta 和终端快捷键"
            ) {
                isShowingKeypad = true
            }
        }
    }

    private var trailingShortcutPage: some View {
        HStack(spacing: 4) {
            shortcutButton(
                nil,
                systemImage: "arrow.left",
                identifier: "send-arrow-left",
                accessibilityLabel: "左方向键",
                accessibilityHint: "向当前终端发送左方向键"
            ) {
                if runtime.sendArrowLeft(after: command) {
                    command = ""
                    followsOutput = true
                }
            }
            shortcutButton(
                nil,
                systemImage: "arrow.up",
                identifier: "send-arrow-up",
                accessibilityLabel: "上方向键",
                accessibilityHint: "Shell 中通常浏览上一条命令"
            ) {
                historyUp()
            }
            shortcutButton(
                nil,
                systemImage: "arrow.down",
                identifier: "send-arrow-down",
                accessibilityLabel: "下方向键",
                accessibilityHint: "Shell 中通常浏览下一条命令"
            ) {
                historyDown()
            }
            shortcutButton(
                nil,
                systemImage: "arrow.right",
                identifier: "send-arrow-right",
                accessibilityLabel: "右方向键",
                accessibilityHint: "向当前终端发送右方向键"
            ) {
                if runtime.sendArrowRight(after: command) {
                    command = ""
                    followsOutput = true
                }
            }
        }
    }

    private var applicationShortcutPage: some View {
        HStack(spacing: 5) {
            applicationShortcutButton(
                "设置",
                systemImage: "gearshape.fill",
                identifier: "open-watch-settings-from-dock",
                accessibilityHint: "打开终端、Linux 和 App 设置"
            ) {
                isShowingSettings = true
            }

            applicationShortcutButton(
                "会话",
                systemImage: "rectangle.stack",
                identifier: "open-watch-sessions-from-dock",
                accessibilityHint: "切换或管理终端会话"
            ) {
                isShowingSessions = true
            }

            applicationShortcutButton(
                isFocusMode ? "退出" : "专注",
                systemImage: isFocusMode ?
                    "arrow.down.right.and.arrow.up.left" :
                    "arrow.up.left.and.arrow.down.right",
                identifier: "toggle-watch-terminal-focus",
                accessibilityHint: isFocusMode ?
                    "恢复顶部设置与会话按钮" :
                    "隐藏顶部按钮，为终端让出更多空间"
            ) {
                isFocusMode.toggle()
                dockPage = 3
            }

            ShareLink(item: shareableTranscript) {
                shortcutLabel(
                    "分享",
                    systemImage: "square.and.arrow.up")
            }
            .buttonStyle(.plain)
            .disabled(shareableTranscript.isEmpty)
            .accessibilityLabel("分享终端记录")
            .accessibilityHint("分享当前终端保留的文字记录")
            .accessibilityIdentifier("share-watch-terminal-transcript")
        }
    }

    private func applicationShortcutButton(
        _ title: String,
        systemImage: String,
        identifier: String,
        accessibilityHint: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            shortcutLabel(title, systemImage: systemImage)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
        .accessibilityHint(accessibilityHint)
        .accessibilityIdentifier(identifier)
    }

    private func shortcutLabel(
        _ title: String,
        systemImage: String
    ) -> some View {
        VStack(spacing: 1) {
            Image(systemName: systemImage)
            Text(title)
                .lineLimit(1)
        }
        .font(.system(size: 8, weight: .semibold))
        .frame(maxWidth: .infinity, minHeight: 28)
        .background(.quaternary, in: Capsule())
    }

    private func shortcutButton(
        _ title: String?,
        systemImage: String? = nil,
        identifier: String,
        accessibilityLabel: String? = nil,
        accessibilityHint: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Group {
                if let systemImage {
                    Image(systemName: systemImage)
                } else {
                    Text(title ?? "")
                }
            }
            .font(.caption2.weight(.semibold))
            .frame(maxWidth: .infinity, minHeight: 28)
            .background(.quaternary, in: Capsule())
        }
        .buttonStyle(.plain)
        .disabled(!runtime.acceptsInput)
        .accessibilityLabel(accessibilityLabel ?? title ?? identifier)
        .accessibilityHint(accessibilityHint)
        .accessibilityIdentifier(identifier)
    }

    private var dockPageAccessibilityValue: String {
        let name: String
        switch dockPage {
        case 0:
            name = "常用快捷键"
        case 2:
            name = "光标移动"
        case 3:
            name = "应用操作"
        default:
            name = "命令输入"
        }
        return "\(name)，第 \(dockPage + 1) 页，共 4 页"
    }

    private func adjustDockPage(
        _ direction: AccessibilityAdjustmentDirection
    ) {
        let nextPage: Int
        switch direction {
        case .increment:
            nextPage = min(dockPage + 1, 3)
        case .decrement:
            nextPage = max(dockPage - 1, 0)
        @unknown default:
            return
        }
        guard nextPage != dockPage else { return }
        dockPage = nextPage
    }

    private var returnToLatestButton: some View {
        VStack {
            Spacer()
            HStack {
                Spacer()
                Button {
                    followsOutput = true
                } label: {
                    Image(systemName: "arrow.down.to.line")
                        .font(.caption.weight(.bold))
                        .padding(8)
                        .background(.thinMaterial, in: Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("回到最新输出")
                .accessibilityIdentifier("follow-latest-output")
            }
        }
        .padding(.trailing, 7)
        .padding(.bottom, 4)
    }

    private var appearance: WatchTerminalAppearance {
        WatchTerminalAppearance(
            paletteSelectionID: paletteID,
            customThemes: customThemes,
            colorScheme:
                themeAppearanceResolution.terminalColorScheme,
            font:
                WatchTerminalFontChoice(rawValue: terminalFont) ??
                .systemMonospaced,
            cursorShape:
                WatchTerminalCursorShape(rawValue: cursorShape) ??
                .block,
            cursorBlinks: cursorBlinks)
    }

    private var terminalBackground: Color {
        let color = appearance.palette.background
        return Color(
            .sRGB,
            red: color.redComponent,
            green: color.greenComponent,
            blue: color.blueComponent,
            opacity: 1)
    }

    private var statusColor: Color {
        if runtime.acceptsInput {
            return .green
        }
        if runtime.canReopenSession {
            return .orange
        }
        if runtime.hasRuntimeFailure {
            return .red
        }
        return .secondary
    }

    private var shareableTranscript: String {
        guard let session = runtime.activeSession,
              !session.text.isEmpty else {
            return ""
        }
        return "\(session.title)\n\n\(session.text)"
    }

    private var preferredColorScheme: ColorScheme? {
        switch themeAppearanceResolution.shellColorScheme {
        case .light:
            return .light
        case .dark:
            return .dark
        }
    }

    private var customThemes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }

    private var themeAppearanceResolution:
        WatchTerminalThemeAppearanceResolution
    {
        WatchTerminalThemeStore.appearanceResolution(
            for: paletteID,
            in: customThemes,
            preference: colorSchemePreference,
            systemIsDark: Self.nativeSystemIsDark)
    }

    private func sendReturn() {
        if runtime.sendReturn(after: command) {
            command = ""
            followsOutput = true
        }
    }

    private func interrupt() {
        if runtime.sendControl("C", after: command) {
            command = ""
            followsOutput = true
        }
    }

    private func complete() {
        if runtime.sendTab(after: command) {
            command = ""
            followsOutput = true
        }
    }

    private func escape() {
        if runtime.sendEscape(after: command) {
            command = ""
            followsOutput = true
        }
    }

    private func historyUp() {
        if runtime.sendArrowUp(after: command) {
            command = ""
            followsOutput = true
        }
    }

    private func historyDown() {
        if runtime.sendArrowDown(after: command) {
            command = ""
            followsOutput = true
        }
    }
}

private struct TerminalEmptyState: View {
    let title: String
    let status: String
    let isFailure: Bool
    let retryAction: (() -> Void)?

    var body: some View {
        VStack(spacing: 5) {
            Image(systemName: isFailure ?
                "exclamationmark.terminal" : "terminal")
                .font(.title3)
            Text(title)
                .font(.caption.weight(.semibold))
            Text(status)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            if let retryAction {
                Button("重新准备", action: retryAction)
                    .font(.caption.weight(.semibold))
                    .buttonStyle(.bordered)
                    .accessibilityIdentifier(
                        "retry-watch-runtime-preparation")
            }
        }
        .padding(12)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("watch-terminal-empty-state")
    }
}

private struct TerminalKeypadView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var runtime: WatchRuntime
    @Binding var command: String

    private let columns = [
        GridItem(.flexible(), spacing: 6),
        GridItem(.flexible(), spacing: 6),
    ]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    keySection("光标") {
                        key("上移", systemImage: "arrow.up") {
                            if runtime.sendArrowUp(after: command) {
                                command = ""
                            }
                        }
                        key("下移", systemImage: "arrow.down") {
                            if runtime.sendArrowDown(after: command) {
                                command = ""
                            }
                        }
                        key("左移", systemImage: "arrow.left") {
                            if runtime.sendArrowLeft(after: command) {
                                command = ""
                            }
                        }
                        key("右移", systemImage: "arrow.right") {
                            if runtime.sendArrowRight(after: command) {
                                command = ""
                            }
                        }
                    }

                    keySection("终端") {
                        key("Tab", systemImage: "arrow.right.to.line") {
                            sendPending([0x09])
                        }
                        key("Esc", systemImage: "escape") {
                            sendPending([0x1b])
                        }
                        key("DEL", systemImage: "delete.left") {
                            if runtime.sendDelete(after: command) {
                                command = ""
                            }
                        }
                        metaInput
                    }

                    keySection("常用 Ctrl") {
                        controlKey("Ctrl-A", character: "A")
                        controlKey("Ctrl-C", character: "C")
                        controlKey("Ctrl-D", character: "D")
                        controlKey("Ctrl-E", character: "E")
                        controlKey("Ctrl-L", character: "L")
                        controlKey("Ctrl-U", character: "U")
                        controlKey("Ctrl-Z", character: "Z")
                    }

                    NavigationLink {
                        WatchControlInputView(
                            runtime: runtime,
                            command: $command)
                    } label: {
                        Label(
                            "完整 Ctrl 键",
                            systemImage: "control")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .accessibilityIdentifier(
                        "show-all-control-keys")
                }
                .padding(.horizontal, 6)
                .padding(.bottom, 8)
            }
            .navigationTitle("快捷键")
            .accessibilityIdentifier("watch-terminal-keypad")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("完成") {
                        dismiss()
                    }
                    .accessibilityIdentifier("close-terminal-keypad")
                }
            }
        }
    }

    private func keySection<Content: View>(
        _ title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title)
                .font(.caption2)
                .foregroundStyle(.secondary)
            LazyVGrid(columns: columns, spacing: 6) {
                content()
            }
        }
    }

    private func key(
        _ title: String,
        systemImage: String? = nil,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            if let systemImage {
                Label(title, systemImage: systemImage)
                    .frame(maxWidth: .infinity)
            } else {
                Text(title)
                    .frame(maxWidth: .infinity)
            }
        }
        .buttonStyle(.bordered)
        .disabled(!runtime.acceptsInput)
        .accessibilityIdentifier(
            "terminal-key-\(title.lowercased())")
    }

    private func controlKey(
        _ title: String,
        character: Character
    ) -> some View {
        key(title) {
            if runtime.sendControl(
                character,
                after: command
            ) {
                command = ""
            }
        }
    }

    private var metaInput: some View {
        TextFieldLink(prompt: Text("Meta 文本")) {
            Label("Meta", systemImage: "option")
                .frame(maxWidth: .infinity)
        } onSubmit: { value in
            if runtime.sendMeta(value, after: command) {
                command = ""
            }
        }
        .buttonStyle(.bordered)
        .disabled(!runtime.acceptsInput)
        .accessibilityLabel("Meta 输入")
        .accessibilityHint("发送 Escape 前缀文本")
        .accessibilityIdentifier("send-meta-input")
    }

    private func sendPending(_ bytes: [UInt8]) {
        if runtime.sendInput(
            WatchTerminalInput.sequence(bytes, after: command)
        ) {
            command = ""
        }
    }
}

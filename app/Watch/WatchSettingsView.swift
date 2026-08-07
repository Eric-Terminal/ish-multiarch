import SwiftUI

struct WatchSettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var runtime: WatchRuntime
    @Binding var terminalFontSize: Double
    @State private var isShowingThirdPartyNotices = false

    var body: some View {
        NavigationStack {
            List {
                Section("终端") {
                    NavigationLink {
                        TerminalDisplaySettingsView(
                            terminalFontSize: $terminalFontSize)
                    } label: {
                        Label("显示与外观", systemImage: "textformat.size")
                    }
                    .accessibilityIdentifier("watch-display-settings-link")

                    NavigationLink {
                        WatchSessionsView(
                            runtime: runtime,
                            isPresentedModally: false)
                    } label: {
                        Label("终端会话", systemImage: "rectangle.stack")
                    }
                    .accessibilityIdentifier("watch-session-settings-link")
                }

                Section("Linux") {
                    NavigationLink {
                        WatchRuntimeDetailsView(
                            runtime: runtime,
                            rootStore: runtime.rootStore,
                            onSessionCreated: {
                                dismiss()
                            })
                    } label: {
                        Label("Linux 运行详情", systemImage: "cpu")
                    }
                    .accessibilityIdentifier("watch-runtime-details-link")

                    NavigationLink {
                        WatchFileSystemsView(
                            runtime: runtime,
                            store: runtime.rootStore,
                            onSessionCreated: {
                                dismiss()
                            })
                    } label: {
                        Label("文件系统", systemImage: "internaldrive")
                    }
                    .accessibilityIdentifier("watch-filesystems-link")

                    NavigationLink {
                        WatchSharedFilesView(
                            runtime: runtime,
                            onSessionCreated: {
                                dismiss()
                            })
                    } label: {
                        Label(
                            "共享文件",
                            systemImage: "folder.badge.plus")
                    }
                    .accessibilityIdentifier("watch-shared-files-link")

                    NavigationLink {
                        WatchStartupSettingsView(runtime: runtime)
                    } label: {
                        Label("启动与主机名", systemImage: "power")
                    }
                    .accessibilityIdentifier("watch-startup-settings-link")

                    NavigationLink {
                        WatchSoftwareMaintenanceView(
                            runtime: runtime,
                            onSessionCreated: {
                                dismiss()
                            })
                    } label: {
                        HStack {
                            Label(
                                "软件维护",
                                systemImage:
                                    "shippingbox.and.arrow.backward")
                            Spacer()
                            WatchRepositoryStatusBadge(
                                indicator:
                                    repositoryStatus.indicator)
                                .accessibilityHidden(true)
                        }
                    }
                    .accessibilityValue(
                        repositoryStatus.accessibilitySummary)
                    .accessibilityIdentifier(
                        "watch-software-maintenance-link")
                }

                Section("信息") {
                    NavigationLink {
                        WatchAboutView()
                    } label: {
                        Label("关于 iSH", systemImage: "info.circle")
                    }
                    .accessibilityIdentifier("watch-about-link")

                    Button {
                        isShowingThirdPartyNotices = true
                    } label: {
                        Label("许可证与源码", systemImage: "doc.text")
                    }
                    .accessibilityIdentifier("third-party-notices-button")
                }
            }
            .navigationTitle("设置")
            .accessibilityIdentifier("watch-settings-view")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("完成") {
                        dismiss()
                    }
                    .accessibilityIdentifier("close-watch-settings")
                }
            }
        }
        .onChange(
            of: runtime.apkRepositoryMigration.upgradeSessionID
        ) { oldSessionID, newSessionID in
            if oldSessionID == nil && newSessionID != nil {
                dismiss()
            }
        }
        .sheet(isPresented: $isShowingThirdPartyNotices) {
            ThirdPartyNoticesView()
        }
    }

    private var repositoryStatus:
        WatchAPKRepositoryStatusPresentation
    {
        WatchAPKRepositoryStatusPresentation(
            coordinator: runtime.apkRepositoryMigration)
    }
}

struct WatchRepositoryStatusBadge: View {
    let indicator: WatchAPKRepositoryStatusIndicator
    var compact = false

    var body: some View {
        if indicator != .current {
            if compact {
                Circle()
                    .fill(color)
                    .frame(width: 7, height: 7)
            } else {
                Image(systemName: symbolName)
                    .font(.caption2)
                    .foregroundStyle(color)
            }
        }
    }

    private var symbolName: String {
        switch indicator {
        case .current:
            return "checkmark.circle.fill"
        case .working:
            return "clock.fill"
        case .migrationRequired:
            return "arrow.triangle.2.circlepath.circle.fill"
        case .error:
            return "exclamationmark.triangle.fill"
        }
    }

    private var color: Color {
        switch indicator {
        case .current:
            return .green
        case .working:
            return .blue
        case .migrationRequired:
            return .orange
        case .error:
            return .red
        }
    }
}

private struct TerminalDisplaySettingsView: View {
    @Environment(\.watchTerminalColorScheme)
    private var terminalColorScheme
    @Binding var terminalFontSize: Double
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

    var body: some View {
        List {
            Section("预览") {
                WatchAppearancePreview(
                    appearance: appearance,
                    fontSize: terminalFontSize)
                    .frame(height: 56)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                    .accessibilityIdentifier(
                        "watch-terminal-appearance-preview")
            }

            Section("主题") {
                NavigationLink {
                    WatchTerminalThemesView(selectionID: $paletteID)
                } label: {
                    LabeledContent(
                        "终端配色",
                        value: WatchTerminalThemeStore.displayName(
                            for: paletteID,
                            in: customThemes))
                }
                .accessibilityIdentifier("watch-terminal-themes-link")
            }

            Section {
                Picker("字体", selection: $terminalFont) {
                    Text("系统等宽")
                        .tag(
                            WatchTerminalFontChoice
                                .systemMonospaced.rawValue)
                    Text("圆角等宽")
                        .tag(
                            WatchTerminalFontChoice
                                .roundedMonospaced.rawValue)
                }
                .accessibilityIdentifier("watch-terminal-font-picker")

                Picker("字号", selection: $terminalFontSize) {
                    ForEach(
                        WatchPreferences.terminalFontSizeRange,
                        id: \.self
                    ) { size in
                        Text("\(size)").tag(Double(size))
                    }
                }
                .accessibilityIdentifier("watch-terminal-size-picker")
            } header: {
                Text("字体")
            } footer: {
                Text("选择字号时可旋转数码表冠，在 7–24 之间调整。")
            }

            Section("光标") {
                Picker("形状", selection: $cursorShape) {
                    Text("块状")
                        .tag(WatchTerminalCursorShape.block.rawValue)
                    Text("竖线")
                        .tag(WatchTerminalCursorShape.beam.rawValue)
                    Text("下划线")
                        .tag(WatchTerminalCursorShape.underline.rawValue)
                }
                .accessibilityIdentifier("watch-cursor-shape-picker")

                Toggle("闪烁", isOn: $cursorBlinks)
                    .accessibilityIdentifier("watch-cursor-blink-toggle")
            }

            Section("App 外观") {
                Picker("明暗模式", selection: $colorSchemePreference) {
                    Text("跟随系统").tag("system")
                    Text("浅色").tag("light")
                    Text("深色").tag("dark")
                }
                .accessibilityIdentifier("watch-color-scheme-picker")
            }
        }
        .navigationTitle("显示与外观")
        .accessibilityIdentifier("watch-display-settings-view")
    }

    private var appearance: WatchTerminalAppearance {
        WatchTerminalAppearance(
            paletteSelectionID: paletteID,
            customThemes: customThemes,
            colorScheme: terminalColorScheme,
            font:
                WatchTerminalFontChoice(rawValue: terminalFont) ??
                .systemMonospaced,
            cursorShape:
                WatchTerminalCursorShape(rawValue: cursorShape) ??
                .block,
            cursorBlinks: cursorBlinks)
    }

    private var customThemes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }
}

private struct WatchRuntimeDetailsView: View {
    @ObservedObject var runtime: WatchRuntime
    @ObservedObject var rootStore: WatchRootStore
    let onSessionCreated: () -> Void

    var body: some View {
        List {
            Section("当前会话") {
                LabeledContent("状态", value: runtime.status)
                LabeledContent(
                    "终端",
                    value: "\(runtime.sessionSnapshots.count)")
                if let active = runtime.activeSession {
                    LabeledContent("当前终端", value: active.title)
                    LabeledContent(
                        "窗口",
                        value: "\(active.columns) × \(active.rows)")
                }
                LabeledContent("架构", value: "AArch64")
                LabeledContent("主机名", value: runtime.hostname)
                LabeledContent(
                    "系统启动",
                    value: runtime.bootCommand)
                LabeledContent(
                    "终端命令",
                    value: runtime.launchCommand)
            }

            Section("数据") {
                LabeledContent(
                    "RootFS",
                    value: rootStore.activeName ?? "准备中")
                LabeledContent("存储", value: "本地持久化")
                Text("Linux 文件保存在手表 App 的应用支持目录中。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let recovery = runtime.recovery {
                Section {
                    NavigationLink {
                        WatchFileSystemsView(
                            runtime: runtime,
                            store: rootStore,
                            onSessionCreated: onSessionCreated)
                    } label: {
                        Label(
                            "检查文件系统",
                            systemImage: "internaldrive")
                    }
                    .accessibilityIdentifier(
                        "watch-runtime-recovery-filesystems")

                    NavigationLink {
                        WatchStartupSettingsView(runtime: runtime)
                    } label: {
                        Label(
                            "检查启动设置",
                            systemImage: "power")
                    }
                    .accessibilityIdentifier(
                        "watch-runtime-recovery-startup")

                    if recovery.allowsPreparationRetry &&
                            runtime.canRetryPreparation {
                        Button {
                            runtime.retryPreparation()
                        } label: {
                            Label(
                                "重新准备 Linux",
                                systemImage: "arrow.clockwise")
                        }
                        .accessibilityIdentifier(
                            "retry-watch-runtime-from-settings")
                    }
                } header: {
                    Text("恢复")
                } footer: {
                    Text(recovery.settingsInstruction)
                        .accessibilityIdentifier(
                            "watch-runtime-recovery-guidance")
                }
            }
        }
        .navigationTitle("运行详情")
        .accessibilityIdentifier("watch-runtime-details-view")
    }
}

private struct WatchSoftwareMaintenanceView: View {
    @ObservedObject var runtime: WatchRuntime
    let onSessionCreated: () -> Void

    private static let updateBody = "/sbin/apk update"
    private static let upgradeBody =
        "/sbin/apk update && /sbin/apk upgrade"

    var body: some View {
        List {
            Section {
                LabeledContent("Linux", value: runtime.status)
                LabeledContent(
                    "可用终端",
                    value: "\(runtime.sessionSnapshots.count)")
            }

            Section("软件仓库") {
                LabeledContent(
                    "已应用",
                    value: installedRepositoryVersion)
                    .accessibilityIdentifier(
                        "watch-apk-installed-version")
                LabeledContent(
                    "目标",
                    value:
                        WatchAPKRepositoryMigrationService
                            .currentVersionName)
                    .accessibilityIdentifier(
                        "watch-apk-target-version")
                Text(repositoryMigrationDescription)
                    .font(.caption)
                    .foregroundStyle(
                        runtime.apkRepositoryMigration.errorMessage == nil ?
                            Color.secondary : Color.red)
                    .accessibilityIdentifier(
                        "watch-apk-migration-status")
            }

            Section {
                maintenanceButton(
                    "更新软件索引",
                    systemImage: "arrow.triangle.2.circlepath",
                    title: "更新索引",
                    operation: .updateIndex,
                    body: Self.updateBody,
                    identifier: "watch-apk-update")

                Button {
                    _ = runtime.startPackageUpgrade(
                        title: "软件升级",
                        body: Self.upgradeBody)
                } label: {
                    Label(
                        "升级已安装软件",
                        systemImage:
                            "shippingbox.and.arrow.backward")
                }
                .disabled(!runtime.canStartPackageUpgrade)
                .accessibilityIdentifier("watch-apk-upgrade")
            } footer: {
                Text(
                    "更新索引不会推进迁移状态；软件升级会先准备仓库，" +
                    "成功后才记录目标版本。操作在独立终端中运行。")
            }

            if !runtime.canStartSoftwareMaintenance {
                Section {
                    Text(unavailableReason)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("软件维护")
        .accessibilityIdentifier("watch-software-maintenance-view")
    }

    private var installedRepositoryVersion: String {
        guard let version =
                runtime.apkRepositoryMigration.installedVersion else {
            return runtime.apkRepositoryMigration.activity == .checking ?
                "检查中" : "未知"
        }
        guard version > 0 else { return "未记录" }
        let major = version / 10_000
        let minor = version / 100 % 100
        let patch = version % 100
        if patch == 0 {
            return "Alpine v\(major).\(minor)"
        }
        return "Alpine v\(major).\(minor).\(patch)"
    }

    private var repositoryMigrationDescription: String {
        let migration = runtime.apkRepositoryMigration
        if let errorMessage = migration.errorMessage {
            return errorMessage
        }
        switch migration.activity {
        case .checking:
            return "正在检查已应用的软件仓库策略。"
        case .preparingRepositories:
            return "正在准备 Alpine v3.24 软件仓库。"
        case let .upgrading(_, finishesMigration):
            return finishesMigration ?
                "等待软件升级成功后记录 Alpine v3.24。" :
                "软件升级正在独立终端中运行。"
        case .finishingMigration:
            return "软件升级成功，正在记录迁移完成状态。"
        case .idle:
            break
        }
        switch migration.status {
        case .migrationRequired:
            return "需要迁移；只有软件升级成功后才会更新已应用版本。"
        case .current:
            return "AArch64 软件仓库策略已是当前版本。"
        case .newerThanApp:
            return "文件系统版本较新，本 App 已安全跳过仓库改写。"
        case nil:
            return "尚未读取软件仓库迁移状态。"
        }
    }

    private var unavailableReason: String {
        if !runtime.activityState.isForegroundActive {
            return "请回到前台后再开始软件维护。"
        }
        switch runtime.apkRepositoryMigration.activity {
        case .checking:
            return "正在检查软件仓库，请稍候。"
        case .preparingRepositories:
            return "正在准备软件仓库，完成后会打开升级终端。"
        case .upgrading:
            return "已有软件升级正在运行。"
        case .finishingMigration:
            return "正在记录软件仓库迁移状态。"
        case .idle:
            break
        }
        if runtime.sessionSnapshots.contains(where: {
            $0.purpose.maintenanceOperation != nil &&
                $0.maintenancePhase == .executing
        }) {
            return "已有软件维护正在运行，请等待其结束。"
        }
        return "Linux 准备完成后即可开始软件维护。"
    }

    private func maintenanceButton(
        _ title: String,
        systemImage: String,
        title sessionTitle: String,
        operation: WatchSoftwareMaintenanceOperation,
        body: String,
        identifier: String
    ) -> some View {
        Button {
            if runtime.createMaintenanceSession(
                title: sessionTitle,
                operation: operation,
                body: body) != nil {
                onSessionCreated()
            }
        } label: {
            Label(title, systemImage: systemImage)
        }
        .disabled(!runtime.canStartSoftwareMaintenance)
        .accessibilityIdentifier(identifier)
    }
}

private struct WatchStartupSettingsView: View {
    @ObservedObject var runtime: WatchRuntime
    @AppStorage(WatchPreferenceKeys.customHostnameEnabled)
    private var usesCustomHostname = false
    @AppStorage(WatchPreferenceKeys.customHostname)
    private var customHostname = ""
    @AppStorage(WatchPreferenceKeys.bootCommand)
    private var bootCommand = WatchPreferences.defaultBootCommand
    @AppStorage(WatchPreferenceKeys.launchCommand)
    private var launchCommand = WatchPreferences.defaultLaunchCommand
    @State private var validationMessage: String?

    private var nextHostname: String {
        WatchPreferences.hostname(
            deviceName: runtime.automaticHostname,
            usesCustomHostname: usesCustomHostname,
            customHostname: customHostname)
    }

    var body: some View {
        List {
            Section {
                Toggle("使用自定义名称", isOn: $usesCustomHostname)
                    .accessibilityIdentifier(
                        "watch-custom-hostname-toggle")

                if usesCustomHostname {
                    TextFieldLink(prompt: Text("主机名")) {
                        LabeledContent(
                            "名称",
                            value: customHostname.isEmpty ?
                                "点按输入" : customHostname)
                    } onSubmit: { value in
                        saveHostname(value)
                    }
                    .accessibilityIdentifier(
                        "watch-custom-hostname-input")
                }

                LabeledContent("下次启动", value: nextHostname)
                    .accessibilityIdentifier("watch-next-hostname-value")
            } header: {
                Text("主机名")
            } footer: {
                Text("默认使用手表名称；自定义值会在下次启动 App 时应用到 Linux。")
            }

            Section {
                TextFieldLink(prompt: Text("系统启动命令")) {
                    Text(verbatim: bootCommand)
                        .font(.system(.caption, design: .monospaced))
                        .lineLimit(2)
                } onSubmit: { value in
                    saveBootCommand(value)
                }
                .accessibilityIdentifier("watch-boot-command-input")

                Button("恢复默认系统命令") {
                    bootCommand = WatchPreferences.defaultBootCommand
                }
                .disabled(bootCommand == WatchPreferences.defaultBootCommand)
                .accessibilityIdentifier("reset-watch-boot-command")
            } header: {
                Text("Linux 系统")
            } footer: {
                Text("系统命令作为 PID 1 运行；默认由 /sbin/init 管理并回收终端进程。")
            }

            Section {
                TextFieldLink(prompt: Text("终端命令")) {
                    Text(verbatim: launchCommand)
                        .font(.system(.caption, design: .monospaced))
                        .lineLimit(2)
                } onSubmit: { value in
                    saveLaunchCommand(value)
                }
                .accessibilityIdentifier("watch-launch-command-input")

                Button("恢复默认命令") {
                    launchCommand = WatchPreferences.defaultLaunchCommand
                    _ = runtime.updateLaunchCommand(
                        WatchPreferences.defaultLaunchCommand)
                }
                .disabled(
                    launchCommand == WatchPreferences.defaultLaunchCommand)
                .accessibilityIdentifier(
                    "reset-watch-launch-command")
            } header: {
                Text("终端启动命令")
            } footer: {
                Text("每个新终端都由 /bin/sh 执行这条命令。")
            }
        }
        .navigationTitle("启动")
        .accessibilityIdentifier("watch-startup-settings-view")
        .alert(
            "无法保存设置",
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

    private func saveHostname(_ value: String) {
        let hostname = value.trimmingCharacters(
            in: .whitespacesAndNewlines)
        if let issue = WatchPreferences.hostnameValidationIssue(hostname) {
            validationMessage = message(
                for: issue, field: "主机名")
            return
        }
        customHostname = hostname
    }

    private func saveLaunchCommand(_ value: String) {
        let command = value.trimmingCharacters(
            in: .whitespacesAndNewlines)
        if let issue =
                WatchPreferences.launchCommandValidationIssue(command) {
            validationMessage = message(
                for: issue, field: "启动命令")
            return
        }
        launchCommand = command
        _ = runtime.updateLaunchCommand(command)
    }

    private func saveBootCommand(_ value: String) {
        let command = value.trimmingCharacters(
            in: .whitespacesAndNewlines)
        if let issue =
                WatchPreferences.launchCommandValidationIssue(command) {
            validationMessage = message(
                for: issue, field: "系统启动命令")
            return
        }
        bootCommand = command
    }

    private func message(
        for issue: WatchPreferences.ValidationIssue,
        field: String
    ) -> String {
        switch issue {
        case .empty:
            return "\(field)不能为空。"
        case .tooLong:
            return "\(field)太长。"
        case .controlCharacter:
            return "\(field)不能包含控制字符。"
        }
    }
}

private struct WatchAboutView: View {
    private static let sourceURL = URL(
        string: "https://github.com/Eric-Terminal/ish-multiarch")!
    private static let issuesURL = URL(
        string:
            "https://github.com/Eric-Terminal/ish-multiarch/issues/new")!
    private static let discordURL = URL(
        string: "https://discord.gg/HFAXj44")!
    private static let fediverseURL = URL(
        string: "https://publ.ish.app/ish")!

    private var version: String {
        Bundle.main.object(
            forInfoDictionaryKey: "CFBundleShortVersionString"
        ) as? String ?? "未知"
    }

    private var build: String {
        Bundle.main.object(
            forInfoDictionaryKey: "CFBundleVersion"
        ) as? String ?? "未知"
    }

    var body: some View {
        List {
            Section("iSH") {
                LabeledContent("版本", value: version)
                    .accessibilityIdentifier("watch-about-version")
                LabeledContent("构建", value: build)
                    .accessibilityIdentifier("watch-about-build")
                LabeledContent("Guest", value: "AArch64 Linux")
                    .accessibilityIdentifier("watch-about-guest")
                LabeledContent(
                    "宿主指针",
                    value: "\(MemoryLayout<UnsafeRawPointer>.size * 8) 位")
                    .accessibilityIdentifier("watch-about-host-pointer")
            }

            Section {
                Text("iSH 在用户态运行 Linux 程序，文件保存在 App 的私有容器中。")
                    .foregroundStyle(.secondary)
            }

            Section("项目") {
                Link(destination: Self.sourceURL) {
                    Label(
                        "查看源代码",
                        systemImage:
                            "chevron.left.forwardslash.chevron.right")
                }
                .accessibilityIdentifier("watch-about-source-link")

                Link(destination: Self.issuesURL) {
                    Label(
                        "报告问题",
                        systemImage: "exclamationmark.bubble")
                }
                .accessibilityIdentifier("watch-about-issues-link")
            }

            Section("社区") {
                Link(destination: Self.discordURL) {
                    Label("Discord", systemImage: "bubble.left.and.bubble.right")
                }
                .accessibilityIdentifier("watch-about-discord-link")

                Link(destination: Self.fediverseURL) {
                    Label("Fediverse", systemImage: "person.3")
                }
                .accessibilityIdentifier("watch-about-fediverse-link")
            }
        }
        .navigationTitle("关于")
        .accessibilityIdentifier("watch-about-view")
    }
}

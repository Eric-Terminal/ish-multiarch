import SwiftUI

struct WatchSettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var runtime: WatchRuntime
    @Binding var terminalFontSize: Double
    @State private var isShowingThirdPartyNotices = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    NavigationLink {
                        TerminalDisplaySettingsView(
                            terminalFontSize: $terminalFontSize)
                    } label: {
                        Label("显示与外观", systemImage: "textformat.size")
                    }
                    .accessibilityIdentifier("watch-display-settings-link")

                    NavigationLink {
                        WatchRuntimeDetailsView(
                            runtime: runtime,
                            rootStore: runtime.rootStore)
                    } label: {
                        Label("Linux 运行详情", systemImage: "cpu")
                    }
                    .accessibilityIdentifier("watch-runtime-details-link")

                    NavigationLink {
                        WatchFileSystemsView(store: runtime.rootStore)
                    } label: {
                        Label("文件系统", systemImage: "internaldrive")
                    }
                    .accessibilityIdentifier("watch-filesystems-link")

                    NavigationLink {
                        WatchStartupSettingsView(runtime: runtime)
                    } label: {
                        Label("启动与主机名", systemImage: "power")
                    }
                    .accessibilityIdentifier("watch-startup-settings-link")
                }

                Section {
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
        .sheet(isPresented: $isShowingThirdPartyNotices) {
            ThirdPartyNoticesView()
        }
    }
}

private struct TerminalDisplaySettingsView: View {
    @Binding var terminalFontSize: Double
    @AppStorage(WatchPreferenceKeys.colorScheme)
    private var colorSchemePreference = "system"

    var body: some View {
        List {
            Section("字号") {
                Picker("终端字号", selection: $terminalFontSize) {
                    Text("小").tag(9.0)
                    Text("标准").tag(11.0)
                    Text("大").tag(13.0)
                    Text("特大").tag(15.0)
                }
            }

            Section("界面外观") {
                Picker("明暗模式", selection: $colorSchemePreference) {
                    Text("跟随系统").tag("system")
                    Text("浅色").tag("light")
                    Text("深色").tag("dark")
                }
                .accessibilityIdentifier("watch-color-scheme-picker")
            }

            Section("预览") {
                Text("root@iSH:~# uname -m")
                    .font(.system(
                        size: terminalFontSize,
                        design: .monospaced))
            }
        }
        .navigationTitle("显示与外观")
        .accessibilityIdentifier("watch-display-settings-view")
    }
}

private struct WatchRuntimeDetailsView: View {
    @ObservedObject var runtime: WatchRuntime
    @ObservedObject var rootStore: WatchRootStore

    var body: some View {
        List {
            Section("当前会话") {
                LabeledContent("状态", value: runtime.status)
                LabeledContent("架构", value: "AArch64")
                LabeledContent("主机名", value: runtime.hostname)
                LabeledContent(
                    "启动命令",
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
        }
        .navigationTitle("运行详情")
        .accessibilityIdentifier("watch-runtime-details-view")
    }
}

private struct WatchStartupSettingsView: View {
    @ObservedObject var runtime: WatchRuntime
    @AppStorage(WatchPreferenceKeys.customHostnameEnabled)
    private var usesCustomHostname = false
    @AppStorage(WatchPreferenceKeys.customHostname)
    private var customHostname = ""
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
                TextFieldLink(prompt: Text("启动命令")) {
                    Text(verbatim: launchCommand)
                        .font(.system(.caption, design: .monospaced))
                        .lineLimit(2)
                } onSubmit: { value in
                    saveLaunchCommand(value)
                }
                .accessibilityIdentifier("watch-launch-command-input")

                Button("恢复默认命令") {
                    launchCommand = WatchPreferences.defaultLaunchCommand
                }
                .disabled(
                    launchCommand == WatchPreferences.defaultLaunchCommand)
                .accessibilityIdentifier(
                    "reset-watch-launch-command")
            } header: {
                Text("终端启动命令")
            } footer: {
                Text("命令由 /bin/sh 执行，并在下次启动 App 时生效。")
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
        }
        .navigationTitle("关于")
        .accessibilityIdentifier("watch-about-view")
    }
}

private struct WatchFileSystemsView: View {
    @ObservedObject var store: WatchRootStore

    var body: some View {
        List {
            if store.entries.isEmpty && store.isWorking {
                ProgressView("准备环境")
            }

            Section("Linux 环境") {
                ForEach(store.entries) { entry in
                    NavigationLink {
                        WatchFileSystemDetailView(
                            entry: entry,
                            store: store)
                    } label: {
                        HStack {
                            Text(entry.name)
                            Spacer()
                            if entry.name == store.activeName {
                                Image(systemName: "play.fill")
                                    .foregroundStyle(.green)
                                    .accessibilityLabel("正在使用")
                                    .accessibilityIdentifier(
                                        "watch-active-filesystem")
                            } else if entry.name == store.claimedName {
                                Image(systemName: "hourglass")
                                    .foregroundStyle(.blue)
                                    .accessibilityLabel("本次启动")
                                    .accessibilityIdentifier(
                                        "watch-claimed-filesystem")
                            } else if entry.name == store.selectedName {
                                Image(systemName: "arrow.clockwise")
                                    .foregroundStyle(.orange)
                                    .accessibilityLabel("下次启动")
                                    .accessibilityIdentifier(
                                        "watch-next-filesystem")
                            }
                        }
                    }
                    .accessibilityIdentifier(
                        "watch-filesystem-\(entry.name)")
                }
            }

            Section {
                Button {
                    Task {
                        await store.createAndSelect()
                    }
                } label: {
                    Label("新建 Linux 环境", systemImage: "plus")
                }
                .disabled(store.isWorking || !store.isAvailable)
                .accessibilityIdentifier("create-watch-filesystem")
            } footer: {
                Text("新环境创建后会在下次启动 App 时使用。")
            }
        }
        .navigationTitle("文件系统")
        .overlay {
            if store.isWorking && !store.entries.isEmpty {
                ProgressView()
            }
        }
        .task {
            await store.refresh()
        }
        .alert(
            "文件系统操作失败",
            isPresented: Binding(
                get: { store.errorMessage != nil },
                set: { if !$0 { store.clearError() } })
        ) {
            Button("好") {
                store.clearError()
            }
        } message: {
            Text(store.errorMessage ?? "")
        }
        .accessibilityIdentifier("watch-filesystems-view")
    }
}

private struct WatchFileSystemDetailView: View {
    @Environment(\.dismiss) private var dismiss
    let entry: WatchRootEntry
    @ObservedObject var store: WatchRootStore
    @State private var confirmsDeletion = false

    private var isActive: Bool {
        entry.name == store.activeName
    }

    private var isClaimed: Bool {
        entry.name == store.claimedName
    }

    private var isSelected: Bool {
        entry.name == store.selectedName
    }

    var body: some View {
        List {
            Section("状态") {
                LabeledContent(
                    "当前进程",
                    value: isActive ? "正在使用" :
                        (isClaimed ? "本次启动" : "未使用"))
                    .accessibilityIdentifier(
                        "watch-filesystem-current-state")
                LabeledContent(
                    "下次启动",
                    value: isSelected ? "已选择" : "未选择")
                    .accessibilityIdentifier(
                        "watch-filesystem-next-state")
                LabeledContent("架构", value: "AArch64")
            }

            Section {
                Button {
                    store.selectForNextLaunch(entry.name)
                } label: {
                    Label(
                        isSelected ? "已用于下次启动" : "下次启动使用",
                        systemImage: isSelected ?
                            "checkmark.circle.fill" : "arrow.clockwise")
                }
                .disabled(isSelected || store.isWorking)
                .accessibilityIdentifier("select-watch-filesystem")

                Button(role: .destructive) {
                    confirmsDeletion = true
                } label: {
                    Label("删除文件系统", systemImage: "trash")
                }
                .disabled(isClaimed || isSelected || store.isWorking)
                .accessibilityIdentifier("delete-watch-filesystem")
            } footer: {
                if isClaimed {
                    Text("本次启动使用的环境会保持到 App 结束。")
                } else if isSelected {
                    Text("请先选择另一个环境，再删除这一项。")
                }
            }
        }
        .navigationTitle(entry.name)
        .accessibilityIdentifier(
            "watch-filesystem-detail-\(entry.name)")
        .confirmationDialog(
            "删除 \(entry.name)？",
            isPresented: $confirmsDeletion,
            titleVisibility: .visible
        ) {
            Button("永久删除", role: .destructive) {
                Task {
                    if await store.delete(entry.name) {
                        dismiss()
                    }
                }
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("其中的 Linux 文件和已安装软件都会被删除。")
        }
        .overlay {
            if store.isWorking {
                ProgressView()
            }
        }
        .alert(
            "文件系统操作失败",
            isPresented: Binding(
                get: { store.errorMessage != nil },
                set: { if !$0 { store.clearError() } })
        ) {
            Button("好") {
                store.clearError()
            }
        } message: {
            Text(store.errorMessage ?? "")
        }
    }
}

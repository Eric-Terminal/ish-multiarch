import SwiftUI

struct WatchFileSystemsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var runtime: WatchRuntime
    @ObservedObject var store: WatchRootStore
    let onSessionCreated: () -> Void

    var body: some View {
        List {
            if store.entries.isEmpty && store.isWorking {
                ProgressView("准备环境")
            }

            Section("Linux 环境") {
                LabeledContent(
                    "可用环境",
                    value: "\(store.entries.count) 个")
                    .foregroundStyle(.secondary)
                    .accessibilityIdentifier(
                        "watch-filesystems-list-start")

                ForEach(store.entries) { entry in
                    NavigationLink {
                        WatchFileSystemDetailView(
                            entry: entry,
                            runtime: runtime,
                            store: store,
                            onSessionCreated: onSessionCreated)
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(store.displayName(for: entry.name))
                                    .lineLimit(1)
                                if store.displayName(for: entry.name) !=
                                        entry.name {
                                    Text(entry.name)
                                        .font(.caption2.monospaced())
                                        .foregroundStyle(.secondary)
                                }
                            }
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

                NavigationLink {
                    WatchRootArchiveImportView(store: store)
                } label: {
                    Label(
                        "从共享文件恢复",
                        systemImage: "archivebox")
                }
                .disabled(store.isWorking || !store.isAvailable)
                .accessibilityIdentifier(
                    "restore-watch-filesystem-from-shared")
            } footer: {
                Text("新建或恢复的环境会在下次启动 App 时使用。")
            }
        }
        .navigationTitle("文件系统")
        .accessibilityValue(store.isWorking ? "正在准备" : "已就绪")
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button {
                    dismiss()
                } label: {
                    Image(systemName: "chevron.backward")
                }
                .accessibilityLabel("返回设置")
                .accessibilityIdentifier(
                    "back-from-watch-filesystems")
            }
        }
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
    @ObservedObject var runtime: WatchRuntime
    @ObservedObject var store: WatchRootStore
    let onSessionCreated: () -> Void
    @State private var confirmsDeletion = false
    @State private var archiveErrorMessage: String?
    @State private var archiveSuccessMessage: String?

    private var isActive: Bool {
        entry.name == store.activeName
    }

    private var isClaimed: Bool {
        entry.name == store.claimedName
    }

    private var isSelected: Bool {
        entry.name == store.selectedName
    }

    private var displayName: String {
        store.displayName(for: entry.name)
    }

    private var canCopy: Bool {
        !isActive && !isClaimed
    }

    private var isCopyScheduled: Bool {
        store.pendingCopyName == entry.name
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
                TextFieldLink(prompt: Text(displayName)) {
                    LabeledContent("显示名称", value: displayName)
                } onSubmit: { value in
                    _ = store.rename(entry.name, to: value)
                }
                .accessibilityIdentifier(
                    "watch-filesystem-display-name")

                LabeledContent("存储标识", value: entry.name)
                    .accessibilityIdentifier(
                        "watch-filesystem-storage-name")

                if displayName != entry.name {
                    Button {
                        store.restoreDefaultName(for: entry.name)
                    } label: {
                        Label(
                            "恢复默认名称",
                            systemImage: "arrow.uturn.backward")
                    }
                    .disabled(store.isWorking)
                    .accessibilityIdentifier(
                        "restore-watch-filesystem-name")
                }
            } header: {
                Text("名称")
            } footer: {
                Text("显示名称只用于界面，不会移动 Linux 数据目录。")
            }

            if canCopy {
                Section {
                    Button {
                        Task {
                            if await store.copyAndSelect(entry.name) {
                                dismiss()
                            }
                        }
                    } label: {
                        Label("复制为新环境", systemImage: "doc.on.doc")
                    }
                    .disabled(store.isWorking)
                    .accessibilityIdentifier("copy-watch-filesystem")
                } footer: {
                    Text("副本会保留 Linux 文件和已安装软件，并用于下次启动。")
                }
            } else {
                Section {
                    if isCopyScheduled {
                        Label(
                            "已安排安全副本",
                            systemImage: "checkmark.circle")
                            .accessibilityIdentifier(
                                "watch-filesystem-copy-scheduled")

                        Button(role: .destructive) {
                            store.cancelScheduledCopy()
                        } label: {
                            Label(
                                "取消复制计划",
                                systemImage: "xmark.circle")
                        }
                        .disabled(store.isWorking)
                        .accessibilityIdentifier(
                            "cancel-scheduled-watch-filesystem-copy")
                    } else {
                        Button {
                            store.scheduleCopyForNextLaunch(entry.name)
                        } label: {
                            Label(
                                "下次启动前复制",
                                systemImage: "doc.on.doc")
                        }
                        .disabled(store.isWorking)
                        .accessibilityIdentifier(
                            "schedule-watch-filesystem-copy")
                    }
                } header: {
                    Text("安全副本")
                } footer: {
                    Text(
                        "运行中的环境会在下次启动 Linux 之前复制，" +
                        "副本随后成为启动环境。")
                }
            }

            Section {
                if !isActive,
                   case let .exporting(exportingName) =
                        store.archiveOperation,
                   exportingName == entry.name,
                   let progress = store.archiveProgress {
                    VStack(alignment: .leading, spacing: 8) {
                        ProgressView(value: progress.fraction)
                            .accessibilityIdentifier(
                                "watch-root-export-progress")
                        Text(progress.message)
                            .font(.caption2)
                            .accessibilityIdentifier(
                                "watch-root-export-progress-message")
                        Button("取消导出", role: .cancel) {
                            store.cancelArchiveOperation()
                        }
                        .accessibilityIdentifier(
                            "cancel-watch-root-export")
                    }
                } else {
                    Button {
                        if isActive {
                            exportCurrentFileSystem()
                        } else {
                            Task {
                                if let fileName =
                                        await store.exportArchive(entry.name) {
                                    archiveSuccessMessage =
                                        "导出完成：/mnt/shared/\(fileName)"
                                }
                            }
                        }
                    } label: {
                        Label(
                            "导出文件系统",
                            systemImage: "archivebox")
                    }
                    .disabled(
                        store.isWorking ||
                            (isActive && !runtime.canCreateSession) ||
                            (isClaimed && !isActive))
                    .accessibilityIdentifier(
                        "export-current-watch-filesystem")
                }
            } header: {
                Text("导出")
            } footer: {
                if isActive {
                    Text(
                        "仅归档当前运行环境。归档会保存到共享文件；" +
                        "导出期间请避免修改 Linux 文件。")
                } else {
                    Text(
                        "未运行的环境会直接归档到共享文件，" +
                        "无需切换或启动终端。")
                }
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
        .navigationTitle(displayName)
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button {
                    dismiss()
                } label: {
                    Image(systemName: "chevron.backward")
                }
                .accessibilityLabel("返回文件系统")
                .accessibilityIdentifier(
                    "back-from-watch-filesystem-detail")
            }
        }
        .accessibilityIdentifier(
            "watch-filesystem-detail-\(entry.name)")
        .confirmationDialog(
            "删除 \(displayName)？",
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
            .accessibilityIdentifier(
                "confirm-delete-watch-filesystem")
            Button("取消", role: .cancel) {}
        } message: {
            Text("其中的 Linux 文件和已安装软件都会被删除。")
        }
        .overlay {
            if store.isWorking && store.archiveProgress == nil {
                ProgressView()
            }
        }
        .alert(
            "文件系统操作失败",
            isPresented: Binding(
                get: {
                    store.errorMessage != nil ||
                        archiveErrorMessage != nil
                },
                set: {
                    if !$0 {
                        store.clearError()
                        archiveErrorMessage = nil
                    }
                })
        ) {
            Button("好") {
                store.clearError()
                archiveErrorMessage = nil
            }
        } message: {
            Text(archiveErrorMessage ?? store.errorMessage ?? "")
        }
        .alert(
            "导出完成",
            isPresented: Binding(
                get: { archiveSuccessMessage != nil },
                set: {
                    if !$0 {
                        archiveSuccessMessage = nil
                    }
                })
        ) {
            Button("好") {
                archiveSuccessMessage = nil
            }
            .accessibilityIdentifier(
                "dismiss-watch-root-export-success")
        } message: {
            Text(archiveSuccessMessage ?? "")
                .accessibilityIdentifier(
                    "watch-root-export-success-message")
        }
    }

    private func exportCurrentFileSystem() {
        guard runtime.createRootArchiveSession(
                rootName: entry.name) != nil else {
            archiveErrorMessage =
                "无法创建导出终端，请确认这是当前运行环境并关闭一个终端后重试。"
            return
        }
        onSessionCreated()
    }
}

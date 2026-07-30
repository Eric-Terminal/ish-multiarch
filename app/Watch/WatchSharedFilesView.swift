import SwiftUI

struct WatchSharedFilesView: View {
    @ObservedObject var runtime: WatchRuntime
    let onSessionCreated: () -> Void

    @State private var files: [WatchSharedFile] = []
    @State private var pendingDeletion: WatchSharedFile?
    @State private var errorMessage: String?
    @State private var isRefreshing = false

    var body: some View {
        List {
            Section {
                Button {
                    openSharedTerminal()
                } label: {
                    Label("打开共享终端", systemImage: "terminal")
                }
                .disabled(!runtime.canCreateSession)
                .accessibilityIdentifier("watch-open-shared-terminal")
            } footer: {
                Text(
                    "Linux 会把交换目录挂载到 /mnt/shared。" +
                    "在这里创建的文件可回到本页分享。")
            }

            Section("共享文件") {
                if isRefreshing && files.isEmpty {
                    ProgressView("正在刷新")
                } else if files.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        Label("暂无共享文件", systemImage: "doc")
                        Text("请在共享终端的 /mnt/shared 中创建文件。")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .accessibilityIdentifier("watch-shared-files-empty")
                } else {
                    ForEach(files) { file in
                        VStack(alignment: .leading, spacing: 5) {
                            Text(file.name)
                                .lineLimit(2)
                            Text(byteCountDescription(file.byteCount))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                            HStack(spacing: 8) {
                                ShareLink(
                                    item: WatchSharedTransfer(file: file),
                                    preview: SharePreview(file.name)
                                ) {
                                    Label(
                                        "分享",
                                        systemImage: "square.and.arrow.up")
                                        .frame(
                                            maxWidth: .infinity,
                                            minHeight: 44)
                                        .background(
                                            .thinMaterial,
                                            in: Capsule())
                                }
                                // List 的自动按钮样式会让相邻操作竞争整行命中区。
                                .buttonStyle(.plain)
                                .contentShape(Capsule())
                                .accessibilityIdentifier(
                                    "watch-share-file-\(file.name)")

                                Button(role: .destructive) {
                                    pendingDeletion = file
                                } label: {
                                    Label("删除", systemImage: "trash")
                                        .frame(
                                            maxWidth: .infinity,
                                            minHeight: 44)
                                        .background(
                                            .thinMaterial,
                                            in: Capsule())
                                }
                                .buttonStyle(.plain)
                                .contentShape(Capsule())
                                .foregroundStyle(.red)
                                .accessibilityLabel("删除 \(file.name)")
                                .accessibilityIdentifier(
                                    "watch-delete-file-\(file.name)")
                            }
                        }
                    }
                }
            }
        }
        .navigationTitle("共享文件")
        .accessibilityIdentifier("watch-shared-files-view")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    Task {
                        await refresh()
                    }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(isRefreshing)
                .accessibilityLabel("刷新共享文件")
                .accessibilityIdentifier("watch-refresh-shared-files")
            }
        }
        .task {
            await refresh()
        }
        .confirmationDialog(
            "删除 \(pendingDeletion?.name ?? "文件")？",
            isPresented: Binding(
                get: { pendingDeletion != nil },
                set: { if !$0 { pendingDeletion = nil } }),
            titleVisibility: .visible
        ) {
            Button("永久删除", role: .destructive) {
                Task {
                    await deletePendingFile()
                }
            }
            .accessibilityIdentifier(
                "confirm-delete-watch-shared-file")
            Button("取消", role: .cancel) {
                pendingDeletion = nil
            }
        } message: {
            Text("删除后无法恢复。")
        }
        .alert(
            "共享文件操作失败",
            isPresented: Binding(
                get: { errorMessage != nil },
                set: { if !$0 { errorMessage = nil } })
        ) {
            Button("好") {
                errorMessage = nil
            }
        } message: {
            Text(errorMessage ?? "")
        }
    }

    private func openSharedTerminal() {
        guard runtime.createSharedFilesSession() != nil else {
            errorMessage = "无法创建共享终端，请先关闭一个终端后重试。"
            return
        }
        onSessionCreated()
    }

    private func refresh() async {
        isRefreshing = true
        defer { isRefreshing = false }
        do {
            files = try await WatchSharedFiles.loadSystemFiles()
            errorMessage = nil
        } catch {
            files = []
            errorMessage = error.localizedDescription
        }
    }

    private func deletePendingFile() async {
        guard let file = pendingDeletion else { return }
        pendingDeletion = nil
        do {
            try await Task.detached(priority: .userInitiated) {
                try WatchSharedFiles.deleteSystemFile(file)
            }.value
            await refresh()
        } catch {
            let message = error.localizedDescription
            await refresh()
            errorMessage = message
        }
    }

    private func byteCountDescription(_ byteCount: Int) -> String {
        ByteCountFormatter.string(
            fromByteCount: Int64(byteCount),
            countStyle: .file)
    }
}

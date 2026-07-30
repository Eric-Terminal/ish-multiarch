import SwiftUI

struct WatchRootArchiveImportView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var store: WatchRootStore
    @State private var files: [WatchSharedFile] = []
    @State private var isRefreshing = false
    @State private var loadingError: String?

    var body: some View {
        List {
            if isRefreshing, files.isEmpty {
                ProgressView("正在查找归档")
            } else if files.isEmpty, loadingError == nil {
                ContentUnavailableView(
                    "没有可恢复的归档",
                    systemImage: "archivebox",
                    description: Text(
                        "请先把 .tar、.tar.gz 或 .tgz 文件放入共享文件。"))
            } else {
                ForEach(files) { file in
                    Button {
                        Task {
                            if await store.importArchive(file) {
                                dismiss()
                            }
                        }
                    } label: {
                        VStack(alignment: .leading) {
                            Text(file.name)
                            Text(ByteCountFormatter.string(
                                fromByteCount: Int64(file.byteCount),
                                countStyle: .file))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .disabled(store.isWorking)
                    .accessibilityIdentifier(
                        "import-watch-root-\(file.name)")
                }
            }
        }
        .navigationTitle("恢复文件系统")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    Task {
                        await reload()
                    }
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(isRefreshing || store.isWorking)
                .accessibilityLabel("刷新文件系统归档")
                .accessibilityIdentifier(
                    "watch-refresh-root-archives")
            }
        }
        .overlay {
            if let progress = store.archiveProgress {
                VStack(spacing: 8) {
                    ProgressView(value: progress.fraction)
                    Text(progress.message)
                        .font(.caption2)
                        .multilineTextAlignment(.center)
                    Button("取消", role: .cancel) {
                        store.cancelArchiveOperation()
                    }
                    .accessibilityIdentifier(
                        "cancel-watch-root-import")
                }
                .padding()
                .background(
                    .regularMaterial,
                    in: RoundedRectangle(cornerRadius: 12))
            }
        }
        .task {
            await reload()
        }
        .alert(
            "无法读取归档",
            isPresented: Binding(
                get: {
                    loadingError != nil || store.errorMessage != nil
                },
                set: {
                    if !$0 {
                        loadingError = nil
                        store.clearError()
                    }
                })
        ) {
            Button("好") {
                loadingError = nil
                store.clearError()
            }
        } message: {
            Text(loadingError ?? store.errorMessage ?? "")
        }
        .accessibilityIdentifier("watch-root-archive-import-view")
    }

    @MainActor
    private func reload() async {
        isRefreshing = true
        defer { isRefreshing = false }
        do {
            files = try await WatchSharedFiles.loadSystemFiles().filter {
                WatchHostRootArchive.supportsImport(fileName: $0.name)
            }
            loadingError = nil
        } catch {
            files = []
            loadingError = error.localizedDescription
        }
    }
}

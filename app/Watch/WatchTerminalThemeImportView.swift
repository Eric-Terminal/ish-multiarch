import SwiftUI

struct WatchTerminalThemeImportView: View {
    @Environment(\.dismiss) private var dismiss
    @Binding var selectionID: String
    @AppStorage(WatchTerminalThemeStore.storageKey)
    private var customThemesData = Data()

    @State private var files: [WatchSharedFile] = []
    @State private var isRefreshing = false
    @State private var errorMessage: String?
    @State private var importedThemeName: String?

    private var customThemes: [WatchCustomTerminalTheme] {
        WatchTerminalThemeStore.decode(customThemesData)
    }

    var body: some View {
        List {
            Section {
                if isRefreshing && files.isEmpty {
                    ProgressView("正在查找主题")
                } else if files.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        Label("没有可导入的主题", systemImage: "paintpalette")
                        Text("请先把 iSH 主题 JSON 放入 /mnt/shared。")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .accessibilityIdentifier(
                        "watch-theme-import-empty")
                } else {
                    ForEach(files) { file in
                        Button {
                            importTheme(from: file)
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                Label(
                                    file.name,
                                    systemImage: "doc.badge.plus")
                                Text(byteCountDescription(file.byteCount))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .accessibilityIdentifier(
                            "watch-import-theme-\(file.name)")
                    }
                }
            } header: {
                Text("共享目录")
            } footer: {
                Text(
                    "支持 iSH 版本 1 的单套或浅色/深色主题。" +
                    "导入不会删除原文件。")
            }
        }
        .navigationTitle("导入主题")
        .accessibilityIdentifier("watch-theme-import-view")
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
                .accessibilityLabel("刷新主题文件")
                .accessibilityIdentifier("watch-refresh-theme-imports")
            }
        }
        .task {
            await refresh()
        }
        .alert(
            "无法导入主题",
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
        .alert(
            "主题已导入",
            isPresented: Binding(
                get: { importedThemeName != nil },
                set: { if !$0 { importedThemeName = nil } })
        ) {
            Button("完成") {
                importedThemeName = nil
                dismiss()
            }
        } message: {
            Text("已选择“\(importedThemeName ?? "")”。")
        }
    }

    private func refresh() async {
        isRefreshing = true
        defer { isRefreshing = false }
        do {
            files = try await WatchSharedFiles.loadSystemFiles().filter {
                $0.url.pathExtension.lowercased() == "json"
            }
            errorMessage = nil
        } catch {
            files = []
            errorMessage = error.localizedDescription
        }
    }

    private func importTheme(from file: WatchSharedFile) {
        do {
            let data = try WatchSharedFiles.readSystemFile(
                file,
                maximumByteCount:
                    WatchTerminalThemeStore.maximumImportByteCount)
            let theme = try WatchTerminalThemeStore.importedTheme(
                from: data,
                suggestedName:
                    file.url.deletingPathExtension().lastPathComponent,
                existing: customThemes)
            var themes = customThemes
            themes.append(theme)
            customThemesData = WatchTerminalThemeStore.encoded(themes)
            selectionID = theme.selectionID
            importedThemeName = theme.name
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func byteCountDescription(_ byteCount: Int) -> String {
        ByteCountFormatter.string(
            fromByteCount: Int64(byteCount),
            countStyle: .file)
    }
}

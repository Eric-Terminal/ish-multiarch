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
                        Label("终端显示", systemImage: "textformat.size")
                    }

                    NavigationLink {
                        WatchRuntimeDetailsView(runtime: runtime)
                    } label: {
                        Label("Linux 运行详情", systemImage: "cpu")
                    }
                }

                Section {
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

            Section("预览") {
                Text("root@iSH:~# uname -m")
                    .font(.system(
                        size: terminalFontSize,
                        design: .monospaced))
            }
        }
        .navigationTitle("终端显示")
    }
}

private struct WatchRuntimeDetailsView: View {
    @ObservedObject var runtime: WatchRuntime

    var body: some View {
        List {
            Section("当前会话") {
                LabeledContent("状态", value: runtime.status)
                LabeledContent("架构", value: "AArch64")
                LabeledContent("主机名", value: runtime.hostname)
            }

            Section("数据") {
                LabeledContent("RootFS", value: "本地持久化")
                Text("Linux 文件保存在手表 App 的应用支持目录中。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("运行详情")
    }
}

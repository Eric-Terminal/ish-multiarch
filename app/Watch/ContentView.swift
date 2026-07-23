import SwiftUI

struct ContentView: View {
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject var runtime: WatchRuntime
    @State private var command = ""

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 5) {
                Circle()
                    .fill(runtime.acceptsInput ? Color.green : Color.secondary)
                    .frame(width: 6, height: 6)
                Text(runtime.status)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .accessibilityIdentifier("runtime-status")
                Spacer(minLength: 0)
            }

            ScrollViewReader { proxy in
                ScrollView {
                    Text(runtime.output.isEmpty ? " " : runtime.output)
                        .font(.system(.caption2, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .topLeading)
                        .accessibilityIdentifier("terminal-output")
                    Color.clear
                        .frame(height: 1)
                        .id("terminal-bottom")
                }
                .onChange(of: runtime.revision) { _, _ in
                    proxy.scrollTo("terminal-bottom", anchor: .bottom)
                }
            }

            HStack(spacing: 4) {
                TextField("命令", text: $command)
                    .font(.caption)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .accessibilityIdentifier("command-input")
                Button(action: submit) {
                    Image(systemName: "arrow.up.circle.fill")
                }
                .buttonStyle(.plain)
                .accessibilityLabel("发送命令")
                .accessibilityIdentifier("send-command")
            }
            .disabled(!runtime.acceptsInput)

            HStack(spacing: 5) {
                quickKey("^C", action: runtime.sendControlC)
                    .accessibilityIdentifier("send-control-c")
                quickKey("Tab", action: complete)
                    .accessibilityIdentifier("send-tab")
                quickKey("Esc", action: runtime.sendEscape)
                    .accessibilityIdentifier("send-escape")
            }
            .disabled(!runtime.acceptsInput)
        }
        .padding(.horizontal, 4)
        .task(id: scenePhase) {
            if scenePhase == .active {
                await runtime.run()
            }
        }
    }

    private func submit() {
        runtime.submit(command)
        command = ""
    }

    private func complete() {
        runtime.sendTab(after: command)
        command = ""
    }

    private func quickKey(
            _ title: String, action: @escaping () -> Void) -> some View {
        Button(title, action: action)
            .font(.caption2.monospaced())
            .buttonStyle(.bordered)
            .controlSize(.mini)
    }
}

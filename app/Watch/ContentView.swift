import Foundation
import SwiftUI

struct ContentView: View {
    private static let initialLineCount = 24
    private static let historyChunkSize = 32
    private static let linesPerTextBlock = 32
    private static let bottomAnchorID = "terminal-input-bottom"

    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject var runtime: WatchRuntime
    @AppStorage("watchTerminalFontSize")
    private var terminalFontSize = 11.0
    @State private var command = ""
    @State private var earliestVisibleLineID: UInt64?
    @State private var latestVisibleLineID: UInt64?
    @State private var followsOutput = true
    @State private var bottomVisibilityWorkItem: DispatchWorkItem?
    @State private var isShowingSettings = false
    @State private var isShowingKeypad = false

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                List {
                    if earlierLineCount > 0 {
                        loadEarlierButton(proxy: proxy)
                    }

                    ForEach(visibleTextBlocks) { block in
                        terminalTextBlock(block)
                            .id(block.id)
                    }

                    inputRow
                        .id(Self.bottomAnchorID)
                        .onAppear {
                            bottomVisibilityWorkItem?.cancel()
                            bottomVisibilityWorkItem = nil
                            followsOutput = true
                            followLatestOutput()
                        }
                        .onDisappear {
                            scheduleBottomVisibilityUpdate()
                        }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .environment(\.defaultMinListRowHeight, 0)
                .accessibilityIdentifier("terminal-transcript")
                .navigationTitle("iSH")
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) {
                        Button {
                            isShowingSettings = true
                        } label: {
                            Image(systemName: "gearshape.fill")
                        }
                        .accessibilityLabel("设置")
                        .accessibilityIdentifier("watch-settings-button")
                    }
                    ToolbarItem(placement: .topBarTrailing) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 8, height: 8)
                            .accessibilityElement()
                            .accessibilityLabel("Linux 状态")
                            .accessibilityValue(runtime.status)
                            .accessibilityIdentifier("runtime-status")
                    }
                }
                .onAppear {
                    followLatestOutput()
                    scrollToBottom(proxy: proxy, animated: false)
                }
                .onChange(of: runtime.revision) { _, _ in
                    if followsOutput {
                        followLatestOutput()
                        scrollToBottom(proxy: proxy, animated: false)
                    }
                }
            }
        }
        .sheet(isPresented: $isShowingSettings) {
            WatchSettingsView(
                runtime: runtime,
                terminalFontSize: $terminalFontSize)
        }
        .sheet(isPresented: $isShowingKeypad) {
            TerminalKeypadView(
                runtime: runtime,
                command: $command)
        }
        .task(id: scenePhase) {
            if scenePhase == .active {
                await runtime.run()
            }
        }
        .onDisappear {
            bottomVisibilityWorkItem?.cancel()
            bottomVisibilityWorkItem = nil
        }
    }

    private var visibleLines: [TerminalLine] {
        guard !runtime.outputLines.isEmpty else { return [] }
        guard let startIndex = visibleStartIndex,
              let endIndex = visibleEndIndex,
              startIndex <= endIndex else {
            return Array(runtime.outputLines.suffix(Self.initialLineCount))
        }
        return Array(runtime.outputLines[startIndex...endIndex])
    }

    private var visibleStartIndex: Int? {
        guard !runtime.outputLines.isEmpty else { return nil }
        guard let earliestVisibleLineID else {
            return max(
                runtime.outputLines.count - Self.initialLineCount,
                runtime.outputLines.startIndex)
        }
        return runtime.outputLines.firstIndex {
            $0.id >= earliestVisibleLineID
        } ?? max(
            runtime.outputLines.count - Self.initialLineCount,
            runtime.outputLines.startIndex)
    }

    private var visibleEndIndex: Int? {
        guard !runtime.outputLines.isEmpty else { return nil }
        guard let latestVisibleLineID else {
            return runtime.outputLines.index(before: runtime.outputLines.endIndex)
        }
        return runtime.outputLines.lastIndex {
            $0.id <= latestVisibleLineID
        }
    }

    private var earlierLineCount: Int {
        visibleStartIndex ?? 0
    }

    private var visibleTextBlocks: [TerminalTextBlock] {
        let lines = visibleLines
        return stride(
            from: lines.startIndex,
            to: lines.endIndex,
            by: Self.linesPerTextBlock
        ).map { startIndex in
            let endIndex = min(
                startIndex + Self.linesPerTextBlock,
                lines.endIndex)
            let blockLines = lines[startIndex..<endIndex]
            return TerminalTextBlock(
                id: lines[startIndex].id,
                lines: Array(blockLines))
        }
    }

    private var statusColor: Color {
        if runtime.acceptsInput {
            return .green
        }
        if runtime.status.contains("失败") {
            return .red
        }
        return .secondary
    }

    private func terminalTextBlock(
            _ block: TerminalTextBlock) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(block.lines) { line in
                Text(verbatim: line.text.isEmpty ? " " : line.text)
                    .font(.system(
                        size: terminalFontSize,
                        weight: .regular,
                        design: .monospaced))
                    .frame(
                        maxWidth: .infinity,
                        alignment: .topLeading)
                    .fixedSize(horizontal: false, vertical: true)
                    .id(line.id)
                    .accessibilityIdentifier(
                        "terminal-line-\(line.id)")
            }
        }
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .listRowInsets(EdgeInsets(
            top: 0,
            leading: 4,
            bottom: 0,
            trailing: 4))
        .listRowBackground(Color.clear)
    }

    private func loadEarlierButton(proxy: ScrollViewProxy) -> some View {
        let count = min(Self.historyChunkSize, earlierLineCount)
        return Button {
            loadEarlierLines(proxy: proxy)
        } label: {
            Label(
                "向上加载 \(count) 行",
                systemImage: "arrow.up.circle")
                .font(.caption)
                .lineLimit(1)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(.thinMaterial, in: Capsule())
        }
        .buttonStyle(.plain)
        .frame(maxWidth: .infinity)
        .listRowInsets(EdgeInsets(
            top: 0,
            leading: 4,
            bottom: 6,
            trailing: 4))
        .listRowBackground(Color.clear)
        .accessibilityIdentifier("load-earlier-lines")
    }

    private var inputRow: some View {
        HStack(spacing: 8) {
            commandField

            Button(action: submit) {
                Image(systemName: "arrow.up")
                    .font(.system(size: 16, weight: .semibold))
                    .frame(width: 36, height: 36)
                    .background(Color.accentColor, in: Circle())
                    .foregroundStyle(.white)
            }
            .buttonStyle(.plain)
            .disabled(!runtime.acceptsInput)
            .accessibilityLabel("发送命令")
            .accessibilityIdentifier("send-command")
        }
        .padding(.vertical, 4)
        .listRowInsets(EdgeInsets(
            top: 2,
            leading: 4,
            bottom: 2,
            trailing: 4))
        .listRowBackground(Color.clear)
        .swipeActions(edge: .leading, allowsFullSwipe: false) {
            Button(action: interrupt) {
                Label("Ctrl-C", systemImage: "stop.fill")
            }
            .tint(.red)
            .accessibilityIdentifier("send-control-c")

            Button(action: complete) {
                Label("Tab", systemImage: "arrow.right.to.line")
            }
            .tint(.blue)
            .accessibilityIdentifier("send-tab")

            Button(action: escape) {
                Label("Esc", systemImage: "escape")
            }
            .tint(.orange)
            .accessibilityIdentifier("send-escape")
        }
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            Button(action: historyUp) {
                Label("上一条", systemImage: "arrow.up")
            }
            .tint(.indigo)
            .accessibilityIdentifier("send-arrow-up")

            Button(action: historyDown) {
                Label("下一条", systemImage: "arrow.down")
            }
            .tint(.teal)
            .accessibilityIdentifier("send-arrow-down")

            Button {
                isShowingKeypad = true
            } label: {
                Label("更多", systemImage: "ellipsis")
            }
            .tint(.gray)
            .accessibilityIdentifier("show-more-terminal-keys")
        }
        .accessibilityAction(named: Text("发送 Ctrl-C")) {
            interrupt()
        }
        .accessibilityAction(named: Text("发送 Tab")) {
            complete()
        }
        .accessibilityAction(named: Text("发送 Esc")) {
            escape()
        }
        .accessibilityAction(named: Text("上一条命令")) {
            historyUp()
        }
        .accessibilityAction(named: Text("下一条命令")) {
            historyDown()
        }
    }

    @ViewBuilder
    private var commandField: some View {
        let field = TextFieldLink(prompt: Text("命令")) {
            Text(command.isEmpty ? "命令" : command)
                .foregroundStyle(
                    command.isEmpty ? .secondary : .primary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Capsule())
        } onSubmit: { submittedText in
            command = submittedText.trimmingCharacters(
                in: .newlines)
        }
            .buttonStyle(.plain)
            .buttonBorderShape(.capsule)
            .contentShape(Capsule())
            .font(.body)
            .padding(.horizontal, 12)
            .frame(maxWidth: .infinity, minHeight: 36, maxHeight: 36)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .disabled(!runtime.acceptsInput)
            .accessibilityLabel("命令")
            .accessibilityValue(command)
            .accessibilityIdentifier("command-input")

        if #available(watchOS 26.0, *) {
            field.glassEffect(.clear, in: Capsule())
        } else {
            field
                .background(.thinMaterial, in: Capsule())
                .overlay {
                    Capsule()
                        .stroke(.secondary.opacity(0.45), lineWidth: 0.5)
                }
        }
    }

    private func followLatestOutput() {
        guard let latestLine = runtime.outputLines.last else { return }
        let index = max(
            runtime.outputLines.count - Self.initialLineCount,
            runtime.outputLines.startIndex)
        earliestVisibleLineID = runtime.outputLines[index].id
        latestVisibleLineID = latestLine.id
    }

    private func loadEarlierLines(proxy: ScrollViewProxy) {
        guard let oldStartIndex = visibleStartIndex,
              oldStartIndex > runtime.outputLines.startIndex else {
            return
        }
        let oldFirstID = runtime.outputLines[oldStartIndex].id
        let newStartIndex = max(
            runtime.outputLines.startIndex,
            oldStartIndex - Self.historyChunkSize)
        earliestVisibleLineID = runtime.outputLines[newStartIndex].id
        followsOutput = false
        DispatchQueue.main.async {
            proxy.scrollTo(oldFirstID, anchor: .top)
        }
    }

    private func scheduleBottomVisibilityUpdate() {
        bottomVisibilityWorkItem?.cancel()
        let workItem = DispatchWorkItem {
            followsOutput = false
            bottomVisibilityWorkItem = nil
        }
        bottomVisibilityWorkItem = workItem
        DispatchQueue.main.asyncAfter(
            deadline: .now() + 0.15,
            execute: workItem)
    }

    private func scrollToBottom(
            proxy: ScrollViewProxy, animated: Bool) {
        DispatchQueue.main.async {
            if animated {
                withAnimation(.easeOut(duration: 0.18)) {
                    proxy.scrollTo(Self.bottomAnchorID, anchor: .bottom)
                }
            } else {
                proxy.scrollTo(Self.bottomAnchorID, anchor: .bottom)
            }
        }
    }

    private func submit() {
        runtime.submit(command)
        command = ""
    }

    private func interrupt() {
        command = ""
        runtime.sendControlC()
    }

    private func complete() {
        runtime.sendTab(after: command)
        command = ""
    }

    private func escape() {
        runtime.sendEscape(after: command)
        command = ""
    }

    private func historyUp() {
        command = ""
        runtime.sendArrowUp()
    }

    private func historyDown() {
        command = ""
        runtime.sendArrowDown()
    }
}

private struct TerminalTextBlock: Identifiable {
    let id: UInt64
    let lines: [TerminalLine]
}

private struct TerminalKeypadView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var runtime: WatchRuntime
    @Binding var command: String

    var body: some View {
        NavigationStack {
            List {
                Section("光标") {
                    key("上移", systemImage: "arrow.up") {
                        command = ""
                        runtime.sendArrowUp()
                    }
                    key("下移", systemImage: "arrow.down") {
                        command = ""
                        runtime.sendArrowDown()
                    }
                    key("左移", systemImage: "arrow.left") {
                        runtime.sendArrowLeft(after: command)
                        command = ""
                    }
                    key("右移", systemImage: "arrow.right") {
                        runtime.sendArrowRight(after: command)
                        command = ""
                    }
                }
                Section("控制") {
                    key("Ctrl-C", systemImage: "stop.fill") {
                        command = ""
                        runtime.sendControlC()
                    }
                    key("Tab", systemImage: "arrow.right.to.line") {
                        runtime.sendTab(after: command)
                        command = ""
                    }
                    key("Esc", systemImage: "escape") {
                        runtime.sendEscape(after: command)
                        command = ""
                    }
                }
            }
            .navigationTitle("快捷键")
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

    private func key(
            _ title: String,
            systemImage: String,
            action: @escaping () -> Void) -> some View {
        Button {
            action()
            dismiss()
        } label: {
            Label(title, systemImage: systemImage)
        }
        .disabled(!runtime.acceptsInput)
    }
}

import Foundation
import SwiftUI

struct WatchSessionsView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var runtime: WatchRuntime
    let isPresentedModally: Bool
    @State private var pendingClose:
        WatchTerminalSessionSnapshot?
    @State private var pendingScrollbackClear:
        WatchTerminalSessionSnapshot?
    @State private var scrollbackFeedback:
        ScrollbackFeedback?

    private struct ScrollbackFeedback {
        let sessionID: UUID
        let sessionTitle: String
        let message: String
        let isSuccess: Bool
    }

    init(
        runtime: WatchRuntime,
        isPresentedModally: Bool = true
    ) {
        self.runtime = runtime
        self.isPresentedModally = isPresentedModally
    }

    @ViewBuilder
    var body: some View {
        if isPresentedModally {
            NavigationStack {
                content
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("完成") {
                                dismiss()
                            }
                            .accessibilityIdentifier(
                                "close-watch-sessions")
                        }
                    }
            }
        } else {
            content
        }
    }

    private var content: some View {
        List {
            Section {
                ForEach(runtime.sessionSnapshots) { session in
                    sessionRow(session)
                        .swipeActions(
                            edge: .leading,
                            allowsFullSwipe: true
                        ) {
                            if session.canReopenSession {
                                Button {
                                    _ = runtime.selectSession(session.id)
                                    runtime.reopenSession()
                                } label: {
                                    Label(
                                        "重开",
                                        systemImage: "arrow.clockwise")
                                }
                                .tint(.orange)
                            }
                        }
                        .swipeActions(
                            edge: .trailing,
                            allowsFullSwipe: false
                        ) {
                            Button(role: .destructive) {
                                pendingClose = session
                            } label: {
                                Label("关闭", systemImage: "xmark")
                            }

                            Button {
                                requestScrollbackClear(for: session)
                            } label: {
                                Label("清历史", systemImage: "eraser")
                            }
                            .tint(.orange)
                            .disabled(session.scrollbackLineCount == 0)
                            .accessibilityIdentifier(
                                "clear-watch-session-scrollback-" +
                                session.id.uuidString)
                        }
                }
            } header: {
                Text("\(runtime.sessionSnapshots.count) 个终端")
                    .accessibilityIdentifier("watch-session-count")
            } footer: {
                Text(
                    "每个终端都有独立的 PTY、滚动历史和运行状态；" +
                    "轻扫会话可管理历史或关闭终端。")
            }

            Section {
                Button {
                    if runtime.createSession() != nil &&
                            isPresentedModally {
                        dismiss()
                    }
                } label: {
                    Label("新建终端", systemImage: "plus")
                }
                .disabled(!runtime.canCreateSession)
                .accessibilityIdentifier("create-watch-session")
            }
        }
        .navigationTitle("终端")
        .accessibilityIdentifier("watch-sessions-view")
        .confirmationDialog(
            closeDialogTitle,
            isPresented: Binding(
                get: { pendingClose != nil },
                set: { if !$0 { pendingClose = nil } }),
            titleVisibility: .visible
        ) {
            Button(closeActionTitle, role: .destructive) {
                if let pendingClose {
                    _ = runtime.closeSession(pendingClose.id)
                }
                self.pendingClose = nil
            }
            .accessibilityIdentifier(closeActionIdentifier)
            Button("取消", role: .cancel) {
                pendingClose = nil
            }
        } message: {
            Text(closeDialogMessage)
                .accessibilityIdentifier(closeMessageIdentifier)
        }
        .confirmationDialog(
            scrollbackClearDialogTitle,
            isPresented: Binding(
                get: { pendingScrollbackClear != nil },
                set: {
                    if !$0 {
                        pendingScrollbackClear = nil
                    }
                }),
            titleVisibility: .visible
        ) {
            Button("清除滚动历史", role: .destructive) {
                clearPendingScrollback()
            }
            .accessibilityIdentifier(
                "confirm-clear-watch-session-scrollback")
            Button("取消", role: .cancel) {
                pendingScrollbackClear = nil
            }
        } message: {
            Text(
                "已滚出当前屏幕的内容会被永久移除；" +
                "当前屏幕、光标和正在运行的程序都会保留。")
                .accessibilityIdentifier(
                    "watch-session-scrollback-clear-warning")
        }
    }

    private var scrollbackClearDialogTitle: String {
        "清除“\(pendingScrollbackClear?.title ?? "终端")”的滚动历史？"
    }

    private func clearPendingScrollback() {
        guard let pendingScrollbackClear else { return }
        let removedLineCount = runtime.clearScrollback(
            for: pendingScrollbackClear.id)
        if let removedLineCount {
            scrollbackFeedback = ScrollbackFeedback(
                sessionID: pendingScrollbackClear.id,
                sessionTitle: pendingScrollbackClear.title,
                message: removedLineCount == 0 ?
                    "没有可清除的滚动历史" :
                    "已清除 \(removedLineCount) 行滚动历史",
                isSuccess: removedLineCount > 0)
        } else {
            scrollbackFeedback = ScrollbackFeedback(
                sessionID: pendingScrollbackClear.id,
                sessionTitle: pendingScrollbackClear.title,
                message: "终端已关闭，未清除滚动历史",
                isSuccess: false)
        }
        self.pendingScrollbackClear = nil
    }

    private func requestScrollbackClear(
        for session: WatchTerminalSessionSnapshot
    ) {
        scrollbackFeedback = nil
        pendingScrollbackClear = session
    }

    private var closeProtection: WatchTerminalCloseProtection {
        pendingClose?.closeProtection ?? .standard
    }

    private var closeDialogTitle: String {
        switch closeProtection {
        case .standard:
            return "关闭 \(pendingClose?.title ?? "终端")？"
        case .softwareIndexUpdate:
            return "中断软件索引更新？"
        case .packageDatabaseWrite:
            return "中断软件升级？"
        }
    }

    private var closeActionTitle: String {
        switch closeProtection {
        case .standard:
            return "关闭终端"
        case .softwareIndexUpdate:
            return "仍要中断更新"
        case .packageDatabaseWrite:
            return "仍要中断升级"
        }
    }

    private var closeActionIdentifier: String {
        switch closeProtection {
        case .standard:
            return "confirm-close-watch-session"
        case .softwareIndexUpdate:
            return "confirm-interrupt-index-update"
        case .packageDatabaseWrite:
            return "confirm-interrupt-package-upgrade"
        }
    }

    private var closeDialogMessage: String {
        switch closeProtection {
        case .standard:
            return "其中运行的前台任务会结束，其他终端不受影响。"
        case .softwareIndexUpdate:
            return "软件索引可能不完整；中断后应重新更新索引。"
        case .packageDatabaseWrite:
            return "软件包数据库可能正在写入。强制关闭可能损坏已安装软件状态，请尽量等待升级结束。"
        }
    }

    private var closeMessageIdentifier: String {
        switch closeProtection {
        case .standard:
            return "watch-session-close-warning"
        case .softwareIndexUpdate:
            return "watch-index-update-close-warning"
        case .packageDatabaseWrite:
            return "watch-package-database-close-warning"
        }
    }

    private func sessionRow(
        _ session: WatchTerminalSessionSnapshot
    ) -> some View {
        Button {
            _ = runtime.selectSession(session.id)
            dismiss()
        } label: {
            HStack(spacing: 8) {
                Circle()
                    .fill(color(for: session))
                    .frame(width: 7, height: 7)
                VStack(alignment: .leading, spacing: 1) {
                    Text(session.title)
                    Text(session.status)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    if let feedback = scrollbackFeedback,
                       feedback.sessionID == session.id {
                        Label(
                            feedback.message,
                            systemImage: feedback.isSuccess ?
                                "checkmark.circle.fill" : "info.circle")
                            .font(.caption2)
                            .foregroundStyle(
                                feedback.isSuccess ?
                                    Color.green : Color.secondary)
                            .lineLimit(2)
                            .accessibilityLabel(
                                "\(feedback.sessionTitle)：\(feedback.message)")
                            .accessibilityIdentifier(
                                "watch-scrollback-clear-feedback")
                    }
                }
                Spacer()
                if session.closeProtection != .standard {
                    Image(systemName: "exclamationmark.shield")
                        .foregroundStyle(.orange)
                        .accessibilityLabel("受保护的维护会话")
                }
                if runtime.selectedSessionID == session.id {
                    Image(systemName: "checkmark")
                        .foregroundStyle(Color.accentColor)
                }
            }
        }
        .accessibilityValue(
            runtime.selectedSessionID == session.id ?
                "当前终端，\(session.status)" : session.status)
        .accessibilityAction(
            named: Text("清除滚动历史")
        ) {
            requestScrollbackClear(for: session)
        }
        .accessibilityIdentifier(
            session.title == "共享文件" ?
                "watch-session-shared-files-\(session.id.uuidString)" :
                "watch-session-\(session.id.uuidString)")
    }

    private func color(
        for session: WatchTerminalSessionSnapshot
    ) -> Color {
        if session.closeProtection != .standard {
            return .orange
        }
        if session.acceptsInput {
            return .green
        }
        if session.canReopenSession {
            return .orange
        }
        return .secondary
    }
}

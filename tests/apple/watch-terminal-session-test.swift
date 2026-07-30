import Foundation

@main
struct WatchTerminalSessionTest {
    private static var failures = 0
    private static let upgradeToken =
        "0123456789abcdef0123456789abcdef"
    private static let indexToken =
        "abcdef0123456789abcdef0123456789"
    private static let wrongToken =
        "11111111111111111111111111111111"

    static func main() {
        testLimitTitlesAndSelection()
        testIndependentSessionState()
        testClearBackgroundSessionScrollback()
        testReopenIdentityAndGeneration()
        testCursorModeAndPendingInput()
        testTerminalQueryResponses()
        testOSCTitlePropagation()
        testResizeState()
        testLaunchCompletionBoundaries()
        testRuntimeRecoveryGuidance()
        testRuntimeActivityState()
        testMaintenanceCompletionCommand()
        testMaintenanceCompletionFrames()
        testMaintenanceProtection()

        if failures == 0 {
            print("Watch 多终端会话模型回归通过")
        } else {
            fatalError("Watch 多终端会话模型回归失败：\(failures) 项")
        }
    }

    private static func expect(
        _ condition: @autoclosure () -> Bool,
        _ message: String
    ) {
        if !condition() {
            print("失败：\(message)")
            failures += 1
        }
    }

    private static func testLimitTitlesAndSelection() {
        var sessions = WatchTerminalSessions(
            columns: 8,
            rows: 3,
            createsInitialSession: false)
        let ids = (0..<WatchTerminalSessions.limit).compactMap { _ in
            sessions.add()
        }

        expect(
            ids.count == 4 && Set(ids).count == 4,
            "最多四个会话应分别获得稳定 UUID")
        expect(
            sessions.sessions.map(\.title) ==
                ["终端 1", "终端 2", "终端 3", "终端 4"],
            "默认标题应稳定且可区分")
        expect(
            sessions.add() == nil,
            "第五个可见会话应被拒绝")
        expect(
            sessions.selectedSessionID == ids[3],
            "新建成功后应立即选择新会话")

        expect(
            sessions.select(ids[2]),
            "应能选择已有会话")
        let removed = sessions.remove(ids[2])
        expect(
            removed?.id == ids[2] &&
                sessions.selectedSessionID == ids[3],
            "关闭活动会话后应选择相邻会话")
        expect(
            sessions.rename(ids[0], title: "  构建  "),
            "非空标题应可修改")
        expect(
            sessions.session(id: ids[0])?.title == "构建",
            "会话标题应清理首尾空白")
    }

    private static func testIndependentSessionState() {
        var sessions = WatchTerminalSessions(
            columns: 8,
            rows: 3,
            createsInitialSession: false)
        let first = sessions.add(title: "主终端")!
        let second = sessions.add(title: "日志")!

        sessions.update(first) {
            $0.sessionID = UInt64.max - 1
            $0.markRunning()
            $0.appendOutput(Array("first".utf8))
            _ = $0.enqueue(Array("A".utf8))
        }
        sessions.update(second) {
            $0.sessionID = UInt64.max
            $0.markRunning()
            $0.appendOutput(Array(
                "second\u{1b}[?1h".utf8))
            _ = $0.enqueue(Array("B".utf8))
        }

        let firstState = sessions.session(id: first)!
        let secondState = sessions.session(id: second)!
        expect(
            firstState.snapshot.text.contains("first") &&
                !firstState.snapshot.text.contains("second"),
            "首个会话应持有独立终端屏幕")
        expect(
            secondState.snapshot.text.contains("second") &&
                secondState.screen.modes.usesApplicationCursorKeys,
            "第二个会话应持有独立解析器和模式")
        expect(
            firstState.pendingInput == Array("A".utf8) &&
                secondState.pendingInput == Array("B".utf8),
            "每个会话应持有独立输入队列")
        expect(
            firstState.sessionID == UInt64.max - 1 &&
                secondState.sessionID == UInt64.max,
            "C 会话句柄应保持完整 uint64 宽度")

        let secondSelectedSnapshots = sessions.snapshots
        expect(
            secondSelectedSnapshots[0].renderedLines.isEmpty &&
                secondSelectedSnapshots[1].text.contains("second"),
            "发布会话列表时只应渲染当前终端的历史")
        _ = sessions.select(first)
        let firstSelectedSnapshots = sessions.snapshots
        expect(
            firstSelectedSnapshots[0].text.contains("first") &&
                firstSelectedSnapshots[1].renderedLines.isEmpty,
            "切换终端后应只渲染新的当前终端")
    }

    private static func testClearBackgroundSessionScrollback() {
        var sessions = WatchTerminalSessions(
            columns: 8,
            rows: 2,
            createsInitialSession: false)
        let background = sessions.add(title: "后台")!
        let selected = sessions.add(title: "当前")!

        sessions.update(background) {
            $0.sessionID = 41
            $0.markRunning()
            $0.appendOutput(Array(
                "旧输出\r\n当前一\r\n当前二".utf8))
            _ = $0.enqueue(Array("pending".utf8))
        }
        sessions.update(selected) {
            $0.sessionID = 42
            $0.markRunning()
            $0.appendOutput(Array(
                "保留\r\n前台一\r\n前台二".utf8))
        }

        let before = sessions.session(id: background)!.snapshot
        let selectedHistoryBefore =
            sessions.session(id: selected)!.screen.scrollback
        expect(
            before.scrollbackLineCount > 0 &&
                sessions.snapshots[0].scrollbackLineCount ==
                    before.scrollbackLineCount,
            "后台会话快照应公开可清理的历史行数")

        let removedLineCount = sessions.clearScrollback(background)
        let after = sessions.session(id: background)!.snapshot
        expect(
            removedLineCount == before.scrollbackLineCount &&
                after.scrollbackLines.isEmpty &&
                after.scrollbackLineCount == 0,
            "按 UUID 清理后台会话时应只移除其回滚缓冲")
        expect(
            after.visibleLines == before.visibleLines &&
                after.cursor == before.cursor &&
                after.sessionID == before.sessionID &&
                after.acceptsInput == before.acceptsInput &&
                sessions.session(id: background)?.pendingInput ==
                    Array("pending".utf8),
            "清理回滚缓冲必须保留当前屏幕、光标、PTY 与输入队列")
        expect(
            sessions.selectedSessionID == selected &&
                sessions.session(id: selected)?.screen.scrollback ==
                    selectedHistoryBefore,
            "清理后台会话不得切换当前终端或修改其他会话")
        expect(
            sessions.clearScrollback(UUID()) == nil,
            "不存在的会话不应报告清理成功")
    }

    private static func testReopenIdentityAndGeneration() {
        var session = WatchTerminalSession(
            title: "开发",
            columns: 8,
            rows: 3,
            lifecycle: .opening)
        let stableID = session.id
        let firstGeneration = session.beginOperation()
        session.finishOpening(
            sessionID: 11,
            createdColumns: 8,
            createdRows: 3,
            separatesPreviousOutput: false)
        session.markRunning()
        session.appendOutput(Array("before".utf8))
        session.markExited("会话已退出")

        let secondGeneration = session.beginOperation()
        session.beginOpening()
        session.finishOpening(
            sessionID: 22,
            createdColumns: 8,
            createdRows: 3,
            separatesPreviousOutput: true)

        expect(
            session.id == stableID &&
                session.title == "开发" &&
                session.sessionID == 22,
            "退出后重开应保留逻辑 UUID 与标题并更换 C 句柄")
        expect(
            session.snapshot.text.contains("before"),
            "重开会话应保留之前的终端历史")
        expect(
            !session.matchesOperation(
                generation: firstGeneration) &&
                session.matchesOperation(
                    generation: secondGeneration,
                    sessionID: 22),
            "晚到的旧代回调不应匹配新一代 sessionID")
    }

    private static func testCursorModeAndPendingInput() {
        var session = WatchTerminalSession(
            title: "按键",
            sessionID: 7,
            columns: 8,
            rows: 3,
            lifecycle: .running)
        expect(
            session.cursorKey(0x41) == [0x1b, 0x5b, 0x41],
            "普通光标模式应发送 CSI 方向键")

        session.appendOutput(Array("\u{1b}[?1h".utf8))
        expect(
            session.cursorKey(0x41) == [0x1b, 0x4f, 0x41],
            "DEC application cursor 模式应发送 SS3 方向键")

        expect(
            session.enqueue(Array("abc".utf8)),
            "运行中会话应接收输入")
        session.consumePendingInput(2)
        expect(
            session.pendingInput == Array("c".utf8),
            "部分写入后应只移除已消费字节")
        session.consumePendingInput(1)
        expect(
            session.pendingInput.isEmpty,
            "输入完全写入后应清空该会话队列")

        let oversized = [UInt8](
            repeating: 0x61,
            count: WatchTerminalSession.inputLimit + 1)
        expect(
            !session.enqueue(oversized) &&
                session.status == "输入队列已满",
            "单会话输入队列应有明确上限")
    }

    private static func testTerminalQueryResponses() {
        var session = WatchTerminalSession(
            title: "查询",
            sessionID: 8,
            columns: 8,
            rows: 3,
            lifecycle: .running)
        session.appendOutput(Array((
            "\u{1b}[?1049h\u{1b}[999;999H" +
            "\u{1b}[c\u{1b}[5n\u{1b}[6n"
        ).utf8))
        expect(
            session.pendingInput == Array((
                "\u{1b}[?1;2c\u{1b}[0n\u{1b}[3;8R"
            ).utf8) &&
                session.screen.modes.usesAlternateScreen,
            "vi 的备用屏 DA/DSR/CPR 应按夹取后的坐标回到对应 PTY")
        expect(
            session.snapshot.renderedLines.allSatisfy(\.text.isEmpty),
            "协议响应不应被当作终端输出显示")

        session.consumePendingInput(session.pendingInput.count)
        _ = session.enqueue(Array("x".utf8))
        session.appendOutput(Array("\u{1b}[?6".utf8))
        session.appendOutput(Array("n".utf8))
        expect(
            session.pendingInput ==
                Array("x\u{1b}[?3;8R".utf8),
            "用户输入与跨分片 DECXCPR 应按队列顺序送往同一 PTY")

        var pressuredSession = WatchTerminalSession(
            title: "背压",
            sessionID: 9,
            columns: 8,
            rows: 3,
            lifecycle: .running)
        expect(
            pressuredSession.enqueue([UInt8](
                repeating: 0x61,
                count: WatchTerminalSession.inputLimit)),
            "测试前置输入应能填满有界队列")
        pressuredSession.appendOutput(Array("\u{1b}[5n".utf8))
        expect(
            pressuredSession.pendingInput.count ==
                WatchTerminalSession.inputLimit &&
                pressuredSession.screen.pendingResponseBytes ==
                    Array("\u{1b}[0n".utf8),
            "PTY 输入背压时应暂存而非丢弃终端查询响应")
        pressuredSession.consumePendingInput(
            WatchTerminalSession.inputLimit)
        expect(
            pressuredSession.pendingInput ==
                Array("\u{1b}[0n".utf8) &&
                pressuredSession.screen.pendingResponseBytes.isEmpty,
            "输入腾出空间后应自动续接暂存的终端响应")
    }

    private static func testOSCTitlePropagation() {
        var interactive = WatchTerminalSession(
            title: "终端 1",
            sessionID: 10,
            columns: 20,
            rows: 3,
            lifecycle: .running)
        interactive.appendOutput(Array("\u{1b}]0;root@watch:~/".utf8))
        interactive.appendOutput(Array("src\u{1b}\\prompt".utf8))
        expect(
            interactive.title == "root@watch:~/src" &&
                interactive.snapshot.text.contains("prompt"),
            "交互会话应采用跨分片 OSC 0 标题且不显示协议负载")

        interactive.appendOutput(Array("\u{1b}]2;构建\u{07}".utf8))
        expect(
            interactive.title == "构建",
            "交互会话应采用 OSC 2 标题")

        var maintenance = WatchTerminalSession(
            title: "软件升级",
            sessionID: 11,
            columns: 20,
            rows: 3,
            lifecycle: .running,
            purpose: .softwareMaintenance(.upgradePackages),
            maintenanceCompletionToken: upgradeToken)
        maintenance.appendOutput(Array(
            "\u{1b}]2;不应覆盖\u{07}正在运行".utf8))
        expect(
            maintenance.title == "软件升级" &&
                maintenance.snapshot.text.contains("正在运行") &&
                maintenance.screen.takePendingTitle() == nil,
            "维护会话应消费但忽略 OSC 标题，保留操作语义名称")
    }

    private static func testResizeState() {
        var session = WatchTerminalSession(
            title: "尺寸",
            sessionID: 9,
            columns: 8,
            rows: 3,
            lifecycle: .running)
        session.appendOutput(Array("A\r\nB\r\nC".utf8))

        expect(
            session.resize(columns: 12, rows: 2),
            "合法终端尺寸应被接受")
        expect(
            session.screen.columns == 12 &&
                session.screen.rows == 2 &&
                session.windowSizeNeedsUpdate,
            "resize 应同时更新屏幕并标记 C winsize 待同步")
        expect(
            !session.resize(columns: 0, rows: 2) &&
                session.screen.columns == 12,
            "非法尺寸不应破坏已有终端状态")
    }

    private static func testLaunchCompletionBoundaries() {
        let defaultRequest = WatchTerminalSessionLaunchRequest(
            defaultCommand: "exec /bin/login -f root",
            command: nil,
            columns: 40,
            rows: 18)
        let maintenanceRequest = WatchTerminalSessionLaunchRequest(
            defaultCommand: "exec /bin/login -f root",
            command: "exec apk upgrade",
            columns: 48,
            rows: 12)
        expect(
            defaultRequest.command == "exec /bin/login -f root" &&
                maintenanceRequest.command == "exec apk upgrade",
            "自定义命令应只进入该次会话创建请求")

        var sessions = WatchTerminalSessions()
        let initialID = sessions.selectedSessionID!
        var generation: UInt64 = 0
        sessions.update(initialID) {
            generation = $0.beginOperation()
        }
        sessions.remove(initialID)
        let acceptsLateResult =
            sessions.session(id: initialID)?
                .matchesOperation(generation: generation) == true
        expect(
            !acceptsLateResult,
            "准备阶段删除初始会话后，晚到的创建结果必须视为孤儿")

        let replacementID = sessions.add(
            title: "软件维护",
            lifecycle: .opening)
        expect(
            replacementID != nil &&
                sessions.selectedSessionID == replacementID,
            "新建维护终端后应立即选择该会话")
    }

    private static func testRuntimeRecoveryGuidance() {
        let retry = WatchRuntimeRecovery.retryPreparation
        expect(
            retry.allowsPreparationRetry &&
                retry.settingsInstruction.contains("内核启动前") &&
                retry.settingsInstruction.contains("安全重新准备") &&
                retry.settingsInstruction.contains("重新打开"),
            "准备阶段失败应只在内核启动前允许安全重试，并说明启动设置边界")
        expect(
            retry.status(appendingTo: "文件系统不可用")
                .contains("文件系统不可用\nLinux 内核尚未启动"),
            "空状态应同时显示准备失败原因与可执行恢复指引")

        let reopen = WatchRuntimeRecovery.reopenApplication
        expect(
            !reopen.allowsPreparationRetry &&
                reopen.settingsInstruction.contains("不能原地重启") &&
                reopen.settingsInstruction.contains("文件系统") &&
                reopen.settingsInstruction.contains("启动命令") &&
                reopen.settingsInstruction.contains("App 切换器"),
            "内核停止或失败后只能指引检查设置并结束、重新打开 App")
        expect(
            reopen.status(appendingTo: "运行失败（Linux errno 5）")
                .contains("在 App 切换器中结束 iSH 后重新打开"),
            "运行失败空状态不应提供虚假的进程内重启")
    }

    private static func testRuntimeActivityState() {
        var activity = WatchRuntimeActivityState()
        expect(
            activity.isForegroundActive &&
                activity.allowsStartingHeavyWork,
            "无 UI/XCTest 初始化默认应允许前台重任务")
        activity.update(isForegroundActive: false)
        expect(
            !activity.isForegroundActive &&
                !activity.allowsStartingHeavyWork,
            "进入后台后不得再启动软件升级")
        activity.update(isForegroundActive: true)
        expect(
            activity.allowsStartingHeavyWork,
            "重新回到前台后应允许用户重试")
    }

    private static func testMaintenanceCompletionCommand() {
        expect(
            WatchSoftwareMaintenanceCompletionProtocol.token(
                bytes: [
                    0x01, 0x23, 0x45, 0x67,
                    0x89, 0xab, 0xcd, 0xef,
                    0x01, 0x23, 0x45, 0x67,
                    0x89, 0xab, 0xcd, 0xef,
                ]) == upgradeToken &&
                WatchSoftwareMaintenanceCompletionProtocol.token(
                    bytes: [0]) == nil,
            "维护 nonce 应由 128 位随机值编码为固定长度小写十六进制")
        expect(
            WatchSoftwareMaintenanceCompletionProtocol.isValidToken(
                upgradeToken) &&
                !WatchSoftwareMaintenanceCompletionProtocol.isValidToken(
                    String(repeating: "g", count: 32)) &&
                !WatchSoftwareMaintenanceCompletionProtocol.isValidToken(
                    "abcd"),
            "维护 nonce 只能是 32 字节十六进制 ASCII")

        let body = "printf '%s' \"$HOME\"; exit 7"
        let command =
            WatchSoftwareMaintenanceCompletionProtocol.shellCommand(
                body: body,
                operation: .upgradePackages,
                token: upgradeToken)
        expect(
            command.contains(
                "/bin/sh -c 'printf '\"'\"'%s'\"'\"' " +
                    "\"$HOME\"; exit 7'") &&
                command.contains(
                    "printf '\\033_ish-watch-maintenance/1;" +
                    "complete;upgrade-packages;" +
                    "\(upgradeToken);%s\\033\\\\'") &&
                command.contains("status=$?") &&
                command.contains("软件升级结束（状态 %s）"),
            "Runtime wrapper 应安全单引号封装 body，并打印不可见 nonce 帧")

        let generatedTokens = (0..<16).map { _ in
            WatchSoftwareMaintenanceCompletionProtocol.makeToken()
        }
        expect(
            generatedTokens.allSatisfy(
                WatchSoftwareMaintenanceCompletionProtocol
                    .isValidToken) &&
                Set(generatedTokens).count == generatedTokens.count,
            "每次维护操作都应生成独立的 128 位随机 nonce")
    }

    private static func testMaintenanceCompletionFrames() {
        let success = Array(
            completionFrame(
                operation: "upgrade-packages",
                token: upgradeToken,
                status: "0").utf8)
        let maximumFailure = Array(
            completionFrame(
                operation: "upgrade-packages",
                token: upgradeToken,
                status: "255").utf8)
        expect(
            WatchSoftwareMaintenanceCompletionProtocol.status(
                in: success,
                operation: .upgradePackages,
                token: upgradeToken) == 0 &&
                WatchSoftwareMaintenanceCompletionProtocol.status(
                    in: maximumFailure,
                    operation: .upgradePackages,
                    token: upgradeToken) == 255,
            "完成协议应接受 0...255 的规范十进制状态")

        let malformedStatuses = [
            "", "00", "+0", "-1", " 0", "256", "0;extra",
        ]
        expect(
            malformedStatuses.allSatisfy { status in
                WatchSoftwareMaintenanceCompletionProtocol.status(
                    in: Array(completionFrame(
                        operation: "upgrade-packages",
                        token: upgradeToken,
                        status: status).utf8),
                    operation: .upgradePackages,
                    token: upgradeToken) == nil
            },
            "空状态、前导零、符号、空白、越界和额外字段都应被拒绝")
        expect(
            WatchSoftwareMaintenanceCompletionProtocol.status(
                in: Array(completionFrame(
                    operation: "update-index",
                    token: upgradeToken,
                    status: "0").utf8),
                operation: .upgradePackages,
                token: upgradeToken) == nil,
            "正确 nonce 但错误操作名不得完成维护")

        let malformedThenValid =
            completionFrame(
                operation: "upgrade-packages",
                token: upgradeToken,
                status: "00") +
            completionFrame(
                operation: "upgrade-packages",
                token: upgradeToken,
                status: "0")
        expect(
            WatchSoftwareMaintenanceCompletionProtocol.status(
                in: Array(malformedThenValid.utf8),
                operation: .upgradePackages,
                token: upgradeToken) == 0,
            "畸形候选帧不得阻止其后的合法完成帧")

        for split in 1..<success.count {
            var session = WatchTerminalSession(
                title: "逐字节分片",
                sessionID: 80,
                lifecycle: .running,
                purpose: .softwareMaintenance(.upgradePackages),
                maintenanceCompletionToken: upgradeToken)
            session.appendOutput(Array(success.prefix(split)))
            expect(
                session.maintenancePhase == .executing,
                "完成帧第 \(split) 个分片边界不得提前完成")
            session.appendOutput(Array(success.dropFirst(split)))
            expect(
                session.maintenancePhase == .completed,
                "完成帧应支持第 \(split) 个字节边界的跨分片解析")
        }

        var dropped = WatchTerminalSession(
            title: "丢包边界",
            sessionID: 81,
            lifecycle: .running,
            purpose: .softwareMaintenance(.upgradePackages),
            maintenanceCompletionToken: upgradeToken)
        let split = success.count / 2
        dropped.appendOutput(Array(success.prefix(split)))
        dropped.reportDroppedBytes(1)
        dropped.appendOutput(Array(success.dropFirst(split)))
        expect(
            dropped.maintenancePhase == .executing,
            "输出丢包后不得把前后两段拼成伪完整帧")
        dropped.appendOutput(success)
        expect(
            dropped.maintenancePhase == .completed,
            "丢包清空探针后仍应接受后续完整完成帧")

        var otherSession = WatchTerminalSession(
            title: "另一个升级",
            sessionID: 82,
            lifecycle: .running,
            purpose: .softwareMaintenance(.upgradePackages),
            maintenanceCompletionToken: wrongToken)
        otherSession.appendOutput(success)
        expect(
            otherSession.maintenancePhase == .executing,
            "同类维护会话之间不得共享 nonce")
        otherSession.appendOutput(Array(
            completionFrame(
                operation: "upgrade-packages",
                token: wrongToken,
                status: "255").utf8))
        expect(
            otherSession.maintenancePhase == .failed(255) &&
                !otherSession.snapshot.text.contains(
                    "ish-watch-maintenance") &&
                !otherSession.snapshot.text.contains(wrongToken),
            "会话自己的 nonce 应报告失败，内部 APC 负载不得显示")
    }

    private static func testMaintenanceProtection() {
        var sessions = WatchTerminalSessions(
            createsInitialSession: false)
        let upgradeID = sessions.add(
            title: "软件升级",
            lifecycle: .running,
            purpose: .softwareMaintenance(.upgradePackages),
            maintenanceCompletionToken: upgradeToken)!
        sessions.update(upgradeID) {
            $0.sessionID = 71
        }

        var upgrade = sessions.session(id: upgradeID)!
        expect(
            upgrade.maintenancePhase == .executing &&
                upgrade.closeProtection == .packageDatabaseWrite &&
                upgrade.status == "正在升级软件" &&
                upgrade.maintenanceCompletionToken == upgradeToken &&
                sessions.hasExecutingSoftwareMaintenance,
            "升级执行期间应公开包数据库写入保护状态")
        expect(
            upgrade.snapshot.purpose ==
                .softwareMaintenance(.upgradePackages) &&
                upgrade.snapshot.closeProtection ==
                .packageDatabaseWrite,
            "升级保护状态应进入可观察会话快照")

        upgrade.appendOutput(Array(
            "\r\n软件升级结束（状态 0）\r\n".utf8))
        upgrade.appendOutput(Array((
            "printf '\\033_ish-watch-maintenance/1;" +
            "complete;upgrade-packages;\(upgradeToken);" +
            "0\\033\\\\'\r\n"
        ).utf8))
        upgrade.appendOutput(Array((
            WatchSoftwareMaintenanceCompletionProtocol.frame(
                operation: .upgradePackages,
                token: wrongToken,
                status: 0) + "\r\n"
        ).utf8))
        let invalidStatus =
            String(
                WatchSoftwareMaintenanceCompletionProtocol.frame(
                    operation: .upgradePackages,
                    token: upgradeToken,
                    status: 0).dropLast()) + "256\r\n"
        upgrade.appendOutput(Array(invalidStatus.utf8))
        expect(
            upgrade.maintenancePhase == .executing,
            "旧文案、输入回显、错误 nonce 与越界状态不得完成维护")

        let completion = Array((
            "\r\n" +
            WatchSoftwareMaintenanceCompletionProtocol.frame(
                operation: .upgradePackages,
                token: upgradeToken,
                status: 0) +
            "\r\n"
        ).utf8)
        upgrade.appendOutput(Array(completion.prefix(11)))
        expect(
            upgrade.maintenancePhase == .executing,
            "分片结束标记未完整到达前不得解除升级保护")
        upgrade.appendOutput(Array(completion.dropFirst(11).dropLast(4)))
        expect(
            upgrade.maintenancePhase == .executing,
            "状态码结束符未完整到达前不得误报维护完成")
        upgrade.appendOutput(Array(completion.suffix(4)))
        sessions.update(upgradeID) {
            $0 = upgrade
        }
        expect(
            upgrade.maintenancePhase == .completed &&
                upgrade.closeProtection == .standard &&
                upgrade.status == "维护已完成" &&
                !sessions.hasExecutingSoftwareMaintenance,
            "状态 0 的完整 APC 到达后应解除升级关闭保护")

        var indexUpdate = WatchTerminalSession(
            title: "更新索引",
            sessionID: 72,
            lifecycle: .running,
            purpose: .softwareMaintenance(.updateIndex),
            maintenanceCompletionToken: indexToken)
        expect(
            indexUpdate.closeProtection == .softwareIndexUpdate &&
                indexUpdate.status == "正在更新软件索引",
            "索引更新应使用独立于普通终端的关闭保护")
        let failedCompletion = Array((
            "\r\n" +
            WatchSoftwareMaintenanceCompletionProtocol.frame(
                operation: .updateIndex,
                token: indexToken,
                status: 17) +
            "\r\n"
        ).utf8)
        indexUpdate.appendOutput(
            Array(failedCompletion.dropLast(6)))
        expect(
            indexUpdate.maintenancePhase == .executing,
            "非零状态码未完整到达前仍应保持执行状态")
        indexUpdate.appendOutput(Array(failedCompletion.suffix(6)))
        expect(
            indexUpdate.maintenancePhase == .failed(17) &&
                indexUpdate.closeProtection == .standard &&
                indexUpdate.status == "维护失败（状态 17）",
            "非零状态应明确报告维护失败并解除关闭保护")
        indexUpdate.markExited("会话已退出")
        expect(
            indexUpdate.maintenancePhase == .failed(17) &&
                indexUpdate.closeProtection == .standard,
            "失败状态不应在维护终端退出时被覆盖")

        var interruptedUpdate = WatchTerminalSession(
            title: "中断索引更新",
            sessionID: 73,
            lifecycle: .running,
            purpose: .softwareMaintenance(.updateIndex),
            maintenanceCompletionToken: indexToken)
        interruptedUpdate.markExited("会话已退出")
        expect(
            interruptedUpdate.maintenancePhase == .interrupted &&
                interruptedUpdate.closeProtection == .standard,
            "没有结束标记的维护进程退出后应记录中断")

        upgrade.beginOpening(purpose: .interactive)
        expect(
            upgrade.purpose == .interactive &&
                upgrade.maintenancePhase == nil &&
                upgrade.maintenanceCompletionToken == nil &&
                upgrade.closeProtection == .standard,
            "维护终端重开为普通登录终端时应清除维护状态")
    }

    private static func completionFrame(
        operation: String,
        token: String,
        status: String
    ) -> String {
        "\u{1b}_ish-watch-maintenance/1;complete;" +
            "\(operation);\(token);\(status)\u{1b}\\"
    }
}

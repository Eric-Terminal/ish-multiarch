import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func test多终端输出隔离且关闭后其余会话可继续() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let firstInput = commandInput(in: app)
        XCTAssertTrue(
            firstInput.waitForExistence(timeout: 180),
            "首个终端的命令输入框没有出现")
        let firstInputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: firstInput)
        wait(for: [firstInputReady], timeout: 180)

        let firstSend = app.buttons["send-command"]
        let firstTerminal = terminalTranscript(in: app)
        XCTAssertTrue(
            firstSend.waitForExistence(timeout: 10) &&
                firstTerminal.waitForExistence(timeout: 10),
            "首个终端的输入或输出控件没有出现")

        let token = String(UUID().uuidString.prefix(8))
        let firstMarker = "ISH-MULTI:\(token):FIRST"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(firstMarker)",
                pass: firstMarker,
                fail: "ISH-MULTI:\(token):FIRST:FAIL",
                timeout: 60,
                app: app,
                input: firstInput,
                send: firstSend,
                terminal: firstTerminal),
            "首个终端无法执行隔离检查命令")

        let sessionsButton = app.buttons.matching(
            identifier: "watch-sessions-button").firstMatch
        XCTAssertTrue(
            sessionsButton.waitForExistence(timeout: 10),
            "终端会话入口没有出现")
        sessionsButton.tap()

        let sessionsView = app.descendants(matching: .any)[
            "watch-sessions-view"]
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "终端会话页没有打开")
        let sessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-"))
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 1, timeout: 10),
            "终端会话页没有显示首个终端")
        let firstSessionIdentifier = sessions.firstMatch.identifier

        let create = app.buttons["create-watch-session"]
        scrollToElement(create, in: app)
        XCTAssertTrue(create.exists, "终端会话页缺少新建入口")
        XCTAssertTrue(create.isEnabled, "仍有空位时新建终端入口不可用")
        create.tap()

        let secondInput = commandInput(in: app)
        XCTAssertTrue(
            secondInput.waitForExistence(timeout: 180),
            "新建后第二个终端的命令输入框没有出现")
        let secondInputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: secondInput)
        wait(for: [secondInputReady], timeout: 180)

        let secondSend = app.buttons["send-command"]
        let secondTerminal = terminalTranscript(in: app)
        let secondMarker = "ISH-MULTI:\(token):SECOND"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(secondMarker)",
                pass: secondMarker,
                fail: "ISH-MULTI:\(token):SECOND:FAIL",
                timeout: 60,
                app: app,
                input: secondInput,
                send: secondSend,
                terminal: secondTerminal),
            "第二个终端无法执行隔离检查命令")

        sessionsButton.tap()
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "第二次打开终端会话页失败")
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 2, timeout: 30),
            "新建后终端会话列表没有增加")

        let firstSession = app.buttons[firstSessionIdentifier]
        scrollToElement(firstSession, in: app)
        XCTAssertTrue(firstSession.exists, "无法在会话列表中找到首个终端")
        firstSession.tap()

        let restoredFirstTerminal = terminalTranscript(in: app)
        let restoredFirstOutput = waitForTerminalOutput(
            containing: [firstMarker],
            timeout: 30,
            terminal: restoredFirstTerminal)
        XCTAssertTrue(
            restoredFirstOutput.contains(firstMarker),
            "切回首个终端后没有恢复它自己的输出：\(restoredFirstOutput)")
        XCTAssertFalse(
            restoredFirstOutput.contains(secondMarker),
            "第二个终端的输出泄漏到了首个终端")

        sessionsButton.tap()
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "关闭终端前无法重新打开会话页")
        let selectedFirstSession = app.buttons[firstSessionIdentifier]
        scrollToElement(selectedFirstSession, in: app)
        XCTAssertTrue(
            selectedFirstSession.isHittable,
            "首个终端行无法执行关闭手势")
        selectedFirstSession.swipeLeft()

        let closeAction = app.buttons.matching(
            NSPredicate(format: "label == %@", "关闭")).firstMatch
        XCTAssertTrue(
            closeAction.waitForExistence(timeout: 10),
            "终端行没有显示关闭操作")
        closeAction.tap()
        let confirmClose = app.buttons.matching(
            NSPredicate(format: "label == %@", "关闭终端")).firstMatch
        XCTAssertTrue(
            confirmClose.waitForExistence(timeout: 10),
            "关闭终端确认按钮没有出现")
        confirmClose.tap()
        XCTAssertTrue(
            waitForExactElementCount(sessions, expected: 1, timeout: 30),
            "关闭首个终端后其余终端也消失了")

        sessions.firstMatch.tap()
        let remainingInput = commandInput(in: app)
        XCTAssertTrue(
            remainingInput.waitForExistence(timeout: 30),
            "关闭首个终端后其余终端没有恢复输入")
        let remainingReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: remainingInput)
        wait(for: [remainingReady], timeout: 30)
        let remainingTerminal = terminalTranscript(in: app)
        let continueMarker = "ISH-MULTI:\(token):CONTINUE"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(continueMarker)",
                pass: continueMarker,
                fail: "ISH-MULTI:\(token):CONTINUE:FAIL",
                timeout: 60,
                app: app,
                input: remainingInput,
                send: app.buttons["send-command"],
                terminal: remainingTerminal),
            "关闭首个终端后第二个终端无法继续执行命令")
        XCTAssertTrue(
            terminalContains(secondMarker, in: remainingTerminal),
            "关闭其他终端后第二个终端丢失了自己的输出历史")
    }

    func test清除滚动历史保留当前屏幕且终端可继续() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)

        let send = app.buttons["send-command"]
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            send.waitForExistence(timeout: 10) &&
                terminal.waitForExistence(timeout: 10),
            "滚动历史测试所需的输入或输出控件没有出现")

        let token = String(UUID().uuidString.prefix(8))
        // 标记必须短于最窄 Watch 终端的一行，否则合法自动换行会让
        // “命令已完成”探针误判为超时。
        let visibleMarker = "SB:\(token):V"
        let generated = submitGuestLine(
            "clear; t=\(token); i=1; while test \"$i\" -le 96; do " +
                "printf 'ISH-SCROLLBACK:%s:%03d\\n' \"$t\" \"$i\"; " +
                "i=$((i+1)); done; " +
                "printf 'SB:%s:V\\n' \"$t\"",
            pass: visibleMarker,
            fail: "SB:\(token):GF",
            timeout: 60,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        XCTAssertTrue(generated, "真实终端没有生成滚动历史")

        let sessionsButton = app.buttons.matching(
            identifier: "watch-sessions-button").firstMatch
        XCTAssertTrue(
            sessionsButton.waitForExistence(timeout: 10),
            "终端会话入口没有出现")
        sessionsButton.tap()

        let sessionsView = app.descendants(matching: .any)[
            "watch-sessions-view"]
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "终端会话页没有打开")
        let sessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-"))
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 1, timeout: 10),
            "终端会话页没有显示当前终端")
        let currentSession = sessions.matching(
            NSPredicate(
                format: "value CONTAINS %@",
                "当前终端")
        ).firstMatch
        XCTAssertTrue(
            currentSession.waitForExistence(timeout: 10),
            "终端会话页没有标记当前终端")
        let currentSessionIdentifier = currentSession.identifier
        scrollToElement(currentSession, in: app)
        XCTAssertTrue(
            currentSession.isHittable,
            "当前终端行无法执行历史管理手势")
        currentSession.swipeLeft()

        let clearHistory = app.buttons.matching(
            NSPredicate(format: "label == %@", "清历史")
        ).firstMatch
        XCTAssertTrue(
            clearHistory.waitForExistence(timeout: 10),
            "当前终端左滑后没有显示清历史操作")
        XCTAssertTrue(
            clearHistory.isEnabled,
            "真实终端生成内容后仍没有可清除的滚动历史")
        clearHistory.tap()

        // watchOS 26 的 confirmationDialog 不保留子视图标识，
        // 因此按用户实际可见的清除边界说明验证。
        let clearWarning = app.staticTexts.matching(
            NSPredicate(
                format: "label CONTAINS %@",
                "已滚出当前屏幕的内容")
        ).firstMatch
        XCTAssertTrue(
            clearWarning.waitForExistence(timeout: 10),
            "清除滚动历史前没有说明清除范围")
        XCTAssertTrue(
            clearWarning.label.contains("当前屏幕") &&
                clearWarning.label.contains("都会保留"),
            "确认说明没有明确保留当前屏幕：\(clearWarning.label)")

        let confirmClear = app.buttons.matching(
            NSPredicate(
                format: "label == %@",
                "清除滚动历史")
        ).firstMatch
        XCTAssertTrue(
            confirmClear.waitForExistence(timeout: 10),
            "清除滚动历史的二次确认按钮没有出现")
        confirmClear.tap()

        let feedback = app.descendants(matching: .any).matching(
            NSPredicate(
                format: "identifier == %@ OR label CONTAINS %@",
                "watch-scrollback-clear-feedback",
                "已清除")
        ).firstMatch
        XCTAssertTrue(
            feedback.waitForExistence(timeout: 10),
            "确认后没有反馈滚动历史清除结果")
        XCTAssertNotNil(
            feedback.label.range(
                of: "已清除 [1-9][0-9]* 行滚动历史",
                options: .regularExpression),
            "清除反馈没有报告正数行数：\(feedback.label)")

        let restoredSession = app.buttons[currentSessionIdentifier]
        scrollToElement(restoredSession, in: app)
        XCTAssertTrue(
            restoredSession.waitForExistence(timeout: 10),
            "清除历史后当前终端行消失")
        restoredSession.tap()

        let restoredTerminal = terminalTranscript(in: app)
        let retainedOutput = waitForTerminalOutput(
            containing: [visibleMarker],
            timeout: 30,
            terminal: restoredTerminal)
        XCTAssertTrue(
            retainedOutput.contains(visibleMarker),
            "清除滚动历史误删了当前屏幕：\(retainedOutput)")

        let continueMarker = "SB:\(token):C"
        XCTAssertTrue(
            submitGuestLine(
                "t=\(token); " +
                    "printf 'SB:%s:C\\n' \"$t\"",
                pass: continueMarker,
                fail: "SB:\(token):CF",
                timeout: 60,
                app: app,
                input: commandInput(in: app),
                send: app.buttons["send-command"],
                terminal: restoredTerminal),
            "清除滚动历史后 shell 无法继续执行命令")
    }

    func test完整Ctrl面板覆盖主终端控制键() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)

        input.swipeRight()
        let more = app.buttons["show-more-terminal-keys"]
        XCTAssertTrue(
            more.waitForExistence(timeout: 10),
            "常用快捷键页缺少更多入口")
        more.tap()

        let keypad = app.descendants(matching: .any)[
            "watch-terminal-keypad"]
        XCTAssertTrue(
            keypad.waitForExistence(timeout: 10),
            "完整快捷键页没有打开")
        let completeControl = app.buttons["show-all-control-keys"]
        scrollToElement(completeControl, in: app)
        XCTAssertTrue(
            completeControl.exists,
            "快捷键页缺少完整 Ctrl 输入入口")
        completeControl.tap()

        let controlView = app.descendants(matching: .any)[
            "watch-control-input-view"]
        XCTAssertTrue(
            controlView.waitForExistence(timeout: 10),
            "完整 Ctrl 输入面板没有打开")

        let firstLetter = app.buttons["send-full-control-a"]
        XCTAssertTrue(
            firstLetter.waitForExistence(timeout: 10),
            "Ctrl 面板缺少字母 A")
        let lastLetter = app.buttons["send-full-control-z"]
        scrollToElement(lastLetter, in: app)
        XCTAssertTrue(lastLetter.exists, "Ctrl 面板缺少字母 Z")

        for identifier in [
            "send-full-control-at",
            "send-full-control-caret",
            "send-full-control-2",
            "send-full-control-6",
            "send-full-control-minus",
            "send-full-control-left-bracket",
            "send-full-control-right-bracket",
            "send-full-control-backslash",
            "send-full-control-space",
        ] {
            let key = app.buttons[identifier]
            scrollToElement(key, in: app)
            XCTAssertTrue(
                key.exists && key.isEnabled,
                "Ctrl 面板缺少可用按键：\(identifier)")
        }
    }

}

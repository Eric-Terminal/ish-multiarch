import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func test软件维护创建独立终端并可安全中止() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "Linux 准备完成前命令输入框没有出现")
        let inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)

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
            "维护前无法打开终端会话页")
        let sessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-"))
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 1, timeout: 10),
            "维护前没有可用终端")
        let initialSessionCount = sessions.count
        let closeSessions = app.buttons.matching(
            identifier: "close-watch-sessions").firstMatch
        XCTAssertTrue(
            closeSessions.waitForExistence(timeout: 10),
            "终端会话页没有完成按钮")
        closeSessions.tap()

        openSettings(in: app)
        openSettingsPage(
            "watch-software-maintenance-link",
            page: "watch-software-maintenance-view",
            in: app)
        let update = app.buttons["watch-apk-update"]
        scrollToElement(update, in: app)
        XCTAssertTrue(update.exists, "软件维护页缺少更新软件索引操作")
        let upgrade = app.buttons["watch-apk-upgrade"]
        scrollToElement(upgrade, in: app)
        XCTAssertTrue(upgrade.exists, "软件维护页缺少升级已安装软件操作")
        scrollToElement(update, in: app)
        let updateReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: update)
        wait(for: [updateReady], timeout: 30)
        update.tap()

        let maintenanceInput = commandInput(in: app)
        XCTAssertTrue(
            maintenanceInput.waitForExistence(timeout: 30),
            "更新软件索引没有切换到独立维护终端")
        let maintenanceReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: maintenanceInput)
        wait(for: [maintenanceReady], timeout: 30)
        maintenanceInput.swipeRight()

        let interrupt = app.buttons["send-control-c"]
        XCTAssertTrue(
            interrupt.waitForExistence(timeout: 10),
            "维护终端没有可用的 Ctrl-C 快捷键")
        interrupt.tap()

        sessionsButton.tap()
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "中止更新后无法打开终端会话页")
        XCTAssertTrue(
            waitForElementCount(
                sessions,
                atLeast: initialSessionCount + 1,
                timeout: 30),
            "更新软件索引没有创建独立终端")
        let maintenanceSession = sessions.matching(
            NSPredicate(format: "label CONTAINS %@", "更新索引")
        ).firstMatch
        XCTAssertTrue(
            maintenanceSession.waitForExistence(timeout: 10),
            "会话列表没有标记更新索引终端")
        scrollToElement(maintenanceSession, in: app)
        maintenanceSession.swipeLeft()

        let closeAction = app.buttons.matching(
            NSPredicate(format: "label == %@", "关闭")).firstMatch
        XCTAssertTrue(
            closeAction.waitForExistence(timeout: 10),
            "维护终端没有显示关闭操作")
        closeAction.tap()
        let confirmClose = app.buttons.matching(
            NSPredicate(
                format: "label IN %@",
                [
                    "仍要中断更新",
                    "关闭终端",
                ])).firstMatch
        XCTAssertTrue(
            confirmClose.waitForExistence(timeout: 10),
            "关闭维护终端确认按钮没有出现")
        confirmClose.tap()
        XCTAssertTrue(
            waitForExactElementCount(
                sessions,
                expected: initialSessionCount,
                timeout: 30),
            "关闭维护终端后会话数量没有恢复")

        let originalSession = sessions.firstMatch
        XCTAssertTrue(
            originalSession.waitForExistence(timeout: 10),
            "关闭维护终端后原终端没有保留")
        originalSession.tap()

        let restoredInput = commandInput(in: app)
        XCTAssertTrue(
            restoredInput.waitForExistence(timeout: 30),
            "关闭维护终端后原终端没有恢复输入")
        let restoredReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: restoredInput)
        wait(for: [restoredReady], timeout: 30)
        let token = String(UUID().uuidString.prefix(8))
        let restoredMarker = "ISH-MAINT:\(token):RESTORED"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(restoredMarker)",
                pass: restoredMarker,
                fail: "ISH-MAINT:\(token):RESTORE:FAIL",
                timeout: 60,
                app: app,
                input: restoredInput,
                send: app.buttons["send-command"],
                terminal: terminalTranscript(in: app)),
            "关闭维护终端后原终端无法继续执行命令")
    }

    func test升级会话关闭前显示包数据库风险确认() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "Linux 准备完成前命令输入框没有出现")
        let inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)

        openSettings(in: app)
        openSettingsPage(
            "watch-software-maintenance-link",
            page: "watch-software-maintenance-view",
            in: app)
        let upgrade = app.buttons["watch-apk-upgrade"]
        scrollToElement(upgrade, in: app)
        XCTAssertTrue(upgrade.exists, "软件维护页缺少软件升级操作")
        let upgradeReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: upgrade)
        wait(for: [upgradeReady], timeout: 30)
        upgrade.tap()

        let maintenanceInput = commandInput(in: app)
        XCTAssertTrue(
            maintenanceInput.waitForExistence(timeout: 30),
            "软件升级没有创建独立维护终端")
        let sessionsButton = app.buttons.matching(
            identifier: "watch-sessions-button").firstMatch
        XCTAssertTrue(
            sessionsButton.waitForExistence(timeout: 10),
            "升级终端缺少会话入口")
        sessionsButton.tap()

        let sessionsView = app.descendants(matching: .any)[
            "watch-sessions-view"]
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "升级期间无法打开终端会话页")
        let upgradeSession = app.buttons.matching(
            NSPredicate(format: "label CONTAINS %@", "软件升级")
        ).firstMatch
        XCTAssertTrue(
            upgradeSession.waitForExistence(timeout: 30),
            "会话列表没有显示软件升级终端")
        scrollToElement(upgradeSession, in: app)
        upgradeSession.swipeLeft()

        let closeAction = app.buttons.matching(
            NSPredicate(format: "label == %@", "关闭")).firstMatch
        XCTAssertTrue(
            closeAction.waitForExistence(timeout: 10),
            "升级终端没有显示关闭操作")
        closeAction.tap()

        // watchOS 26 的 confirmationDialog 不保留子视图标识，
        // 因此按用户实际可见的风险说明和动作验证。
        let databaseWarning = app.staticTexts.matching(
            NSPredicate(
                format: "label CONTAINS %@",
                "软件包数据库")
        ).firstMatch
        XCTAssertTrue(
            databaseWarning.waitForExistence(timeout: 10),
            "升级执行期间没有显示软件包数据库风险")
        XCTAssertTrue(
            databaseWarning.label.contains("软件包数据库"),
            "升级关闭警告没有说明包数据库风险")
        let confirm = app.buttons["仍要中断升级"]
        XCTAssertTrue(
            confirm.waitForExistence(timeout: 10),
            "升级执行期间仍在使用通用关闭确认")
        confirm.tap()

        let sessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-"))
        XCTAssertTrue(
            waitForExactElementCount(
                sessions,
                expected: 1,
                timeout: 30),
            "确认中断升级后维护终端没有关闭")
        sessions.firstMatch.tap()
        let restoredInput = commandInput(in: app)
        XCTAssertTrue(
            restoredInput.waitForExistence(timeout: 30),
            "中断升级后原终端没有恢复")
    }

    func test软件升级成功后提交仓库迁移版本() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        var input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "Linux 准备完成前命令输入框没有出现")
        let initialReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [initialReady], timeout: 180)

        let token = String(UUID().uuidString.prefix(8))
        let resetMarker = "ISH-MIGRATION:\(token):RESET"
        XCTAssertTrue(
            submitGuestLine(
                "rm -f /ish/apk-version; echo \(resetMarker)",
                pass: resetMarker,
                fail: "ISH-MIGRATION:\(token):RESET:FAIL",
                timeout: 60,
                app: app,
                input: input,
                send: app.buttons["send-command"],
                terminal: terminalTranscript(in: app)),
            "无法准备仓库迁移测试状态")

        app.terminate()
        app.launch()
        input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "重启后 Linux 没有恢复")
        let restartedReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [restartedReady], timeout: 180)

        openSettings(in: app)
        openSettingsPage(
            "watch-software-maintenance-link",
            page: "watch-software-maintenance-view",
            in: app)
        let installedVersion = app.descendants(matching: .any)[
            "watch-apk-installed-version"]
        XCTAssertTrue(
            installedVersion.waitForExistence(timeout: 30),
            "软件维护页没有显示已应用仓库版本")

        let upgrade = app.buttons["watch-apk-upgrade"]
        scrollToElement(upgrade, in: app)
        let upgradeReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: upgrade)
        wait(for: [upgradeReady], timeout: 30)
        upgrade.tap()

        let maintenanceTerminal = terminalTranscript(in: app)
        let completionTitle = waitForTerminalOutput(
            containing: ["软件升级结束"],
            timeout: 900,
            terminal: maintenanceTerminal)
        XCTAssertTrue(
            completionTitle.contains("软件升级结束"),
            "apk 升级没有输出完成标记：\(completionTitle)")
        let completionStatus = waitForTerminalOutput(
            containing: ["状态 0"],
            timeout: 30,
            terminal: maintenanceTerminal)
        XCTAssertTrue(
            completionStatus.contains("状态 0"),
            "apk 升级没有成功完成：\(completionStatus)")

        openSettings(in: app)
        openSettingsPage(
            "watch-software-maintenance-link",
            page: "watch-software-maintenance-view",
            in: app)
        let committedVersion = app.descendants(matching: .any)[
            "watch-apk-installed-version"]
        let versionCommitted = expectation(
            for: NSPredicate(
                format:
                    "label CONTAINS %@ OR value CONTAINS %@",
                "Alpine v3.24",
                "Alpine v3.24"),
            evaluatedWith: committedVersion)
        wait(for: [versionCommitted], timeout: 60)

        app.terminate()
        app.launch()
        input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "提交仓库迁移后 Linux 没有恢复")
        let finalReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [finalReady], timeout: 180)

        // 完成标记必须完整落在最窄 Watch 终端的一行内。
        let verifiedMarker = "MG:\(token):OK"
        XCTAssertTrue(
            submitGuestLine(
                "test \"$(cat /ish/apk-version)\" = 32400 && " +
                    "test \"$(sed -n '1p' /etc/apk/repositories)\" = " +
                    "https://dl-cdn.alpinelinux.org/alpine/v3.24/main && " +
                    "test \"$(sed -n '2p' /etc/apk/repositories)\" = " +
                    "https://dl-cdn.alpinelinux.org/alpine/v3.24/community " +
                    "&& echo \(verifiedMarker)",
                pass: verifiedMarker,
                fail: "MG:\(token):ERR",
                timeout: 60,
                app: app,
                input: input,
                send: app.buttons["send-command"],
                terminal: terminalTranscript(in: app)),
            "仓库迁移版本或 repositories 没有持久化")
    }

}

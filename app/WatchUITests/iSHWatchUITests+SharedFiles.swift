import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func test共享文件双向可见且终端满载禁用() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        var input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "Linux 准备完成前命令输入框没有出现")
        var inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)

        var send = app.buttons["send-command"]
        var terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            send.waitForExistence(timeout: 10) &&
                terminal.waitForExistence(timeout: 10),
            "共享文件检查所需的终端控件没有出现")

        let token = String(UUID().uuidString.prefix(8))
        let fileName = "ish-ui-\(token).txt"
        let preparePass = "ISH-SHARED:\(token):PREPARED"
        let prepareFail = "ISH-SHARED:\(token):PREPARE-FAIL"
        XCTAssertTrue(
            submitGuestLine(
                "if rm -f '/mnt/shared/\(fileName)'; then " +
                    "printf 'ISH-SHARED:%s:PREPARED\\n' '\(token)'; " +
                    "else printf 'ISH-SHARED:%s:PREPARE-FAIL\\n' " +
                    "'\(token)'; fi",
                pass: preparePass,
                fail: prepareFail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "无法清理本次共享文件测试的唯一文件")

        openSettings(in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        let sharedPage = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        let openSharedTerminal = app.buttons[
            "watch-open-shared-terminal"]
        scrollToElement(openSharedTerminal, in: app)
        XCTAssertTrue(
            openSharedTerminal.exists && openSharedTerminal.isEnabled,
            "共享文件页缺少可用的共享终端入口")
        openSharedTerminal.tap()

        let firstDismissal = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: sharedPage)
        wait(for: [firstDismissal], timeout: 10)

        input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "创建共享终端后命令输入框没有出现")
        inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)
        send = app.buttons["send-command"]
        terminal = terminalTranscript(in: app)

        let createPass = "ISH-SHARED:\(token):CREATED"
        let createFail = "ISH-SHARED:\(token):CREATE-FAIL"
        XCTAssertTrue(
            submitGuestLine(
                "if test \"$(pwd)\" = /mnt/shared && " +
                    "printf '%s\\n' '\(token)' >'\(fileName)' && sync; " +
                    "then printf 'ISH-SHARED:%s:CREATED\\n' '\(token)'; " +
                    "else printf 'ISH-SHARED:%s:CREATE-FAIL\\n' " +
                    "'\(token)'; fi",
                pass: createPass,
                fail: createFail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "共享终端没有在 /mnt/shared 中创建测试文件")

        openSettings(in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        let shareFile = app.descendants(matching: .any)[
            "watch-share-file-\(fileName)"]
        scrollToElement(shareFile, in: app)
        XCTAssertTrue(
            shareFile.waitForExistence(timeout: 10) && shareFile.isEnabled,
            "guest 创建的文件没有出现在宿主分享列表")

        let deleteFile = sharedPage.descendants(matching: .button)
            .matching(identifier: "watch-delete-file-\(fileName)")
            .firstMatch
        scrollToElement(deleteFile, in: app)
        XCTAssertTrue(
            deleteFile.waitForExistence(timeout: 10) &&
                deleteFile.isEnabled &&
                deleteFile.isHittable,
            "共享文件缺少删除入口")
        deleteFile.tap()
        // watchOS 26 的 confirmationDialog 有时只保留按钮文案。
        let confirmDelete = app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "confirm-delete-watch-shared-file",
                "永久删除")
        ).firstMatch
        XCTAssertTrue(
            confirmDelete.waitForExistence(timeout: 10),
            "共享文件删除确认没有出现")
        confirmDelete.tap()
        let fileRemoved = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: deleteFile)
        wait(for: [fileRemoved], timeout: 10)

        let secondSharedPage = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        let reopenSharedTerminal = app.buttons[
            "watch-open-shared-terminal"]
        scrollToElement(reopenSharedTerminal, in: app)
        XCTAssertTrue(
            reopenSharedTerminal.exists && reopenSharedTerminal.isEnabled,
            "删除文件后无法再次打开共享终端")
        reopenSharedTerminal.tap()
        let secondDismissal = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: secondSharedPage)
        wait(for: [secondDismissal], timeout: 10)

        input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "第二个共享终端的命令输入框没有出现")
        inputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [inputReady], timeout: 180)
        send = app.buttons["send-command"]
        terminal = terminalTranscript(in: app)

        let deletePass = "ISH-SHARED:\(token):DELETED"
        let deleteFail = "ISH-SHARED:\(token):DELETE-FAIL"
        XCTAssertTrue(
            submitGuestLine(
                "if test \"$(pwd)\" = /mnt/shared && " +
                    "test ! -e '\(fileName)'; then " +
                    "printf 'ISH-SHARED:%s:DELETED\\n' '\(token)'; " +
                    "else printf 'ISH-SHARED:%s:DELETE-FAIL\\n' " +
                    "'\(token)'; fi",
                pass: deletePass,
                fail: deleteFail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "宿主删除没有立即反映到共享终端")

        let sessionsButton = app.buttons.matching(
            identifier: "watch-sessions-button").firstMatch
        XCTAssertTrue(
            sessionsButton.waitForExistence(timeout: 10),
            "共享终端缺少会话入口")
        sessionsButton.tap()
        let sessionsView = app.descendants(matching: .any)[
            "watch-sessions-view"]
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "共享终端会话页没有打开")

        let sharedSessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-shared-files-"))
        XCTAssertTrue(
            waitForElementCount(
                sharedSessions,
                atLeast: 2,
                timeout: 30),
            "会话列表没有显示共享终端的语义标识")
        XCTAssertTrue(
            sharedSessions.firstMatch.label.contains("共享文件"),
            "共享终端会话没有显示正确标题")

        let sessionCount = app.descendants(matching: .any)[
            "watch-session-count"]
        XCTAssertTrue(
            sessionCount.waitForExistence(timeout: 10) &&
                sessionCount.label.contains("3/4"),
            "创建两个共享终端后会话计数不正确")

        let createSession = app.buttons["create-watch-session"]
        scrollToElement(createSession, in: app)
        XCTAssertTrue(
            createSession.exists && createSession.isEnabled,
            "第三个会话存在时无法创建最后一个终端")
        createSession.tap()

        let sessionsDismissed = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: sessionsView)
        wait(for: [sessionsDismissed], timeout: 10)
        let sessionsButtonReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: sessionsButton)
        wait(for: [sessionsButtonReady], timeout: 10)
        XCTAssertTrue(
            sessionsButton.waitForExistence(timeout: 10),
            "创建第四个终端后会话入口没有恢复")
        sessionsButton.tap()
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "无法检查满载终端会话页")
        let fullSessionCount = app.descendants(matching: .any)[
            "watch-session-count"]
        XCTAssertTrue(
            fullSessionCount.waitForExistence(timeout: 10) &&
                fullSessionCount.label.contains("4/4"),
            "满载会话计数没有显示 4/4")
        let disabledCreateSession = app.buttons["create-watch-session"]
        scrollToExistingElement(disabledCreateSession, in: app)
        XCTAssertTrue(
            disabledCreateSession.exists,
            "满载会话页缺少新建终端入口")
        XCTAssertFalse(
            disabledCreateSession.isEnabled,
            "达到四槽上限后新建终端仍可用")

        let closeSessions = app.buttons.matching(
            identifier: "close-watch-sessions").firstMatch
        XCTAssertTrue(
            closeSessions.waitForExistence(timeout: 10),
            "满载会话页缺少完成按钮")
        closeSessions.tap()
        let fullSessionsDismissed = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: sessionsView)
        wait(for: [fullSessionsDismissed], timeout: 10)

        openSettings(in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        let disabledSharedTerminal = app.buttons[
            "watch-open-shared-terminal"]
        scrollToExistingElement(disabledSharedTerminal, in: app)
        XCTAssertTrue(
            disabledSharedTerminal.exists,
            "满载时共享文件页缺少共享终端入口")
        XCTAssertFalse(
            disabledSharedTerminal.isEnabled,
            "达到四槽上限后共享终端仍可创建")
    }

    func test共享TarGzip可启动分享且应用保持运行() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "Linux 准备完成前命令输入框没有出现")
        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        let terminal = terminalTranscript(in: app)
        let token = String(UUID().uuidString.prefix(8))
        let fileName =
            "ish-ui-share-\(token.lowercased()).tar.gz"
        XCTAssertTrue(
            submitGuestLine(
                "if printf '%s\\n' '\(token)' | gzip -c " +
                    ">'/mnt/shared/\(fileName)' && sync; then " +
                    "printf 'ISH-SHARE:%s:CREATED\\n' '\(token)'; " +
                    "else printf 'ISH-SHARE:%s:CREATE-FAIL\\n' " +
                    "'\(token)'; fi",
                pass: "ISH-SHARE:\(token):CREATED",
                fail: "ISH-SHARE:\(token):CREATE-FAIL",
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "无法创建本次分享门禁的唯一 tar.gz 文件")

        openSettings(in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        let sharedPage = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        let share = sharedPage.descendants(matching: .button)
            .matching(identifier: "watch-share-file-\(fileName)")
            .firstMatch
        scrollToElement(share, in: app)
        XCTAssertTrue(
            share.waitForExistence(timeout: 10) &&
                share.isEnabled &&
                share.isHittable,
            "共享 tar.gz 缺少可用的分享入口")
        let deleteConfirmation =
            app.buttons["confirm-delete-watch-shared-file"]
        XCTAssertFalse(
            deleteConfirmation.exists,
            "点击分享前不应出现删除确认框")
        share.tap()

        XCTAssertFalse(
            deleteConfirmation.waitForExistence(timeout: 2),
            "分享操作误触发了相邻的删除确认框")
        let shareActivity = app.descendants(matching: .any).matching(
            NSPredicate(
                format: "label IN %@",
                [
                    "Messages",
                    "信息",
                    "AirDrop",
                    "隔空投送",
                    "Copy",
                    "拷贝",
                ])
        ).firstMatch
        XCTAssertTrue(
            shareActivity.waitForExistence(timeout: 15),
            "系统分享页没有显示可选活动")
        XCTAssertFalse(
            app.descendants(matching: .any).matching(
                NSPredicate(
                    format: "identifier == %@ OR label == %@",
                    "confirm-delete-watch-shared-file",
                    "永久删除")
            ).firstMatch.exists,
            "系统分享页出现时不应同时显示删除确认")
        XCTAssertNotEqual(
            app.state,
            .notRunning,
            "启动 tar.gz 分享导致 Watch App 退出")

        let closeShare = app.descendants(matching: .any).matching(
            NSPredicate(
                format: "label IN %@",
                ["Close", "关闭"])
        ).firstMatch
        XCTAssertTrue(
            closeShare.waitForExistence(timeout: 10),
            "系统分享页缺少关闭入口")
        closeShare.tap()
        XCTAssertTrue(
            sharedPage.waitForExistence(timeout: 10),
            "关闭系统分享页后没有回到共享文件列表")

        app.terminate()
        app.launch()
        XCTAssertTrue(
            commandInput(in: app).waitForExistence(timeout: 180),
            "关闭系统分享页后 Watch App 无法重新打开")
        openSettings(in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        deleteSharedFile(named: fileName, in: app)
    }

}

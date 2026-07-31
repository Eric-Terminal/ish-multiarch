import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func test终端退出后可重新打开() throws {
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
            "终端输入或输出控件没有出现")

        let firstToken = String(UUID().uuidString.prefix(8))
        let firstPass = "ISH-SESSION:\(firstToken):INIT:PASS"
        let firstFail = "ISH-SESSION:\(firstToken):INIT:FAIL"
        XCTAssertTrue(
            submitGuestLine(
                "if grep -q '^1 (init) ' /proc/1/stat; then " +
                    "echo \(firstPass); else echo \(firstFail); fi; exit",
                pass: firstPass,
                fail: firstFail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "可见终端没有运行在 init 管理的子会话中")

        let reopen = app.buttons["reopen-terminal-session"]
        XCTAssertTrue(
            reopen.waitForExistence(timeout: 60),
            "退出终端后没有出现重新打开入口")
        reopen.tap()

        let reopenedInput = commandInput(in: app)
        XCTAssertTrue(
            reopenedInput.waitForExistence(timeout: 180),
            "重新打开后命令输入框没有恢复")
        let reopenedReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: reopenedInput)
        wait(for: [reopenedReady], timeout: 180)

        let reopenedSend = app.buttons["send-command"]
        let secondToken = String(UUID().uuidString.prefix(8))
        // 标记短于 Watch 终端一行，并在命令中分段构造，
        // 只有 shell 真正执行后才会出现完整成功标记。
        let secondPass = "ISH-R:\(secondToken):PASS"
        let secondFail = "ISH-R:\(secondToken):FAIL"
        XCTAssertTrue(
            submitGuestLine(
                "printf 'ISH-R:%s:PASS\\n' '\(secondToken)'",
                pass: secondPass,
                fail: secondFail,
                timeout: 60,
                app: app,
                input: reopenedInput,
                send: reopenedSend,
                terminal: terminal),
            "重新打开的终端无法继续执行 Linux 命令")
    }

    func test文件系统显示名称与双路径复制可用于下次启动() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        openSettings(in: app)
        openSettingsPage(
            "watch-filesystems-link",
            page: "watch-filesystems-view",
            in: app)
        let rows = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-filesystem-"))
        XCTAssertTrue(
            rows.firstMatch.waitForExistence(timeout: 180),
            "文件系统页没有可检查的 Linux 环境")

        let currentMarker = app.descendants(matching: .any).matching(
            NSPredicate(
                format: "identifier IN %@",
                [
                    "watch-active-filesystem",
                    "watch-claimed-filesystem",
                ])
        ).firstMatch
        XCTAssertTrue(
            currentMarker.waitForExistence(timeout: 180),
            "文件系统页没有标记本次运行使用的环境")
        let currentRow = rows.matching(
            NSPredicate(
                format: "label CONTAINS %@ OR label CONTAINS %@",
                "正在使用",
                "本次启动")
        ).firstMatch
        XCTAssertTrue(
            currentRow.waitForExistence(timeout: 10),
            "文件系统页无法定位本次运行使用的环境")
        currentRow.tap()
        XCTAssertTrue(
            app.descendants(matching: .any).matching(
                NSPredicate(
                    format: "identifier BEGINSWITH %@",
                    "watch-filesystem-detail-")
            ).firstMatch.waitForExistence(timeout: 10),
            "当前文件系统详情页没有打开")
        XCTAssertFalse(
            app.buttons["copy-watch-filesystem"].exists,
            "正在使用的文件系统不应显示复制操作")
        let storageName = app.descendants(matching: .any)[
            "watch-filesystem-storage-name"]
        scrollToElement(storageName, in: app)
        XCTAssertTrue(
            storageName.exists,
            "文件系统详情缺少稳定存储标识")

        let alias =
            "测试环境-\(String(UUID().uuidString.prefix(6)))"
        let displayName = app.descendants(matching: .any)[
            "watch-filesystem-display-name"]
        scrollToElement(displayName, in: app)
        submitWatchText(alias, through: displayName, in: app)
        XCTAssertTrue(
            app.staticTexts[alias].waitForExistence(timeout: 10),
            "文件系统显示名称没有立即更新")

        let scheduleCopy = app.buttons[
            "schedule-watch-filesystem-copy"]
        scrollToElement(scheduleCopy, in: app)
        XCTAssertTrue(
            scheduleCopy.waitForExistence(timeout: 10),
            "当前文件系统缺少下次启动前安全复制")
        scheduleCopy.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-filesystem-copy-scheduled"
            ].waitForExistence(timeout: 10),
            "安全复制计划没有被持久化显示")

        reopenSettings(in: app)
        openSettingsPage(
            "watch-filesystems-link",
            page: "watch-filesystems-view",
            in: app)
        let scheduledCopyRow = rows.matching(
            NSPredicate(
                format: "label CONTAINS %@",
                "\(alias) 副本")
        ).firstMatch
        scrollToElement(scheduledCopyRow, in: app)
        XCTAssertTrue(
            scheduledCopyRow.waitForExistence(timeout: 180),
            "重新启动后没有在 runtime claim 前完成安全副本")
        XCTAssertTrue(
            app.staticTexts[alias].waitForExistence(timeout: 10) &&
                app.staticTexts["\(alias) 副本"].waitForExistence(
                    timeout: 10),
            "安全副本没有保留唯一显示名称")
        let create = app.buttons["create-watch-filesystem"]
        scrollToElement(create, in: app)
        XCTAssertTrue(create.exists, "文件系统页缺少新建环境入口")
        let createReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: create)
        wait(for: [createReady], timeout: 180)
        create.tap()

        let selectedMarker = app.descendants(matching: .any)[
            "watch-next-filesystem"]
        XCTAssertTrue(
            selectedMarker.waitForExistence(timeout: 30),
            "新环境没有被标记为下次启动")
        let selectedRow = rows.matching(
            NSPredicate(
                format: "label CONTAINS %@",
                "下次启动")
        ).firstMatch
        XCTAssertTrue(
            selectedRow.waitForExistence(timeout: 10),
            "文件系统页无法定位下次启动使用的环境")
        let selectedStorageIdentifier = selectedRow.identifier
        selectedRow.tap()

        let copy = app.buttons["copy-watch-filesystem"]
        XCTAssertTrue(
            copy.waitForExistence(timeout: 10),
            "非活动文件系统详情页缺少复制操作")
        copy.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-filesystems-view"].waitForExistence(timeout: 180),
            "复制成功后没有返回文件系统列表")
        let copiedSelectedRow = rows.matching(
            NSPredicate(
                format: "label CONTAINS %@",
                "下次启动")
        ).firstMatch
        scrollToElement(copiedSelectedRow, in: app)
        XCTAssertTrue(
            copiedSelectedRow.waitForExistence(timeout: 30) &&
                copiedSelectedRow.identifier !=
                selectedStorageIdentifier,
            "复制出的新环境没有被选为下次启动")
    }

    func test原始输入独立回车并可编辑BusyBoxVi() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
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
            "终端输入或输出控件没有出现")
        XCTAssertGreaterThanOrEqual(
            send.frame.width, 35,
            "独立回车的命中区域被输入框挤窄")
        XCTAssertTrue(
            send.isEnabled,
            "独立回车在终端可输入时仍被禁用")
        XCTAssertLessThanOrEqual(
            send.frame.maxX, app.frame.maxX - 4,
            "独立回车被裁到 Watch 可视区域之外")

        let rawSeed = Int(
            String(UUID().uuidString.prefix(6)),
            radix: 16) ?? 1_000_000
        let rawMarker = "X\(rawSeed + 1)"
        let rawCommand = "echo X$((\(rawSeed)+1))"
        submitWatchText(rawCommand, through: input, in: app)

        let rawTextOutput = waitForTerminalOutput(
            containing: [String(rawSeed)],
            timeout: 10,
            terminal: terminal)
        XCTAssertTrue(
            rawTextOutput.contains(String(rawSeed)),
            "Quickboard 完成后原始文字没有进入终端")
        let prematureOutput = waitForTerminalOutput(
            containing: [rawMarker],
            timeout: 2,
            terminal: terminal)
        XCTAssertFalse(
            prematureOutput.contains(rawMarker),
            "Quickboard 完成不应隐式发送回车：\(prematureOutput)")

        let sendReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [sendReady], timeout: 10)
        send.tap()
        let executedOutput = waitForTerminalOutput(
            containing: [rawMarker],
            timeout: 30,
            terminal: terminal)
        XCTAssertTrue(
            executedOutput.contains(rawMarker),
            "独立回车没有执行已经发送的原始命令：\(executedOutput)")

        let token = String(UUID().uuidString.prefix(8))
        let file = "/tmp/ish-watch-vi-\(token).txt"
        let seed = "VI-SEED-\(token)"
        let encodedSeed = seed.utf8.map {
            String(format: "\\%03o", $0)
        }.joined()
        let setupPass = "ISH-VI:\(token):SETUP:PASS"
        let setupFail = "ISH-VI:\(token):SETUP:FAIL"
        XCTAssertTrue(
            submitGuestLine(
                "t=\(token); if printf '\(encodedSeed)\\012' >'\(file)'; " +
                "then printf 'ISH-VI:%s:SETUP:PASS\\n' \"$t\"; " +
                "else printf 'ISH-VI:%s:SETUP:FAIL\\n' \"$t\"; fi",
                pass: setupPass,
                fail: setupFail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "无法准备 BusyBox vi 测试文件")

        submitWatchText(
            "/usr/bin/vi \(file)",
            through: input,
            in: app)
        let viLaunchReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [viLaunchReady], timeout: 10)
        send.tap()

        // BusyBox vi 会先请求 CSI 6n；看到文件内容说明 CPR 已返回并完成首屏。
        let viOutput = waitForTerminalOutput(
            containing: [seed],
            timeout: 30,
            terminal: terminal)
        XCTAssertTrue(
            viOutput.contains(seed),
            "BusyBox vi 没有越过光标位置查询完成首屏：\(viOutput)")

        let editedText = "VI-EDIT-\(token)"
        submitWatchText(
            "i\(editedText)",
            through: input,
            in: app)
        let insertedOutput = waitForTerminalOutput(
            containing: [editedText],
            timeout: 15,
            terminal: terminal)
        XCTAssertTrue(
            insertedOutput.contains(editedText),
            "原始文字没有进入 BusyBox vi 插入模式：\(insertedOutput)")

        input.swipeRight()
        let escape = app.buttons["send-escape"]
        XCTAssertTrue(
            escape.waitForExistence(timeout: 10),
            "常用快捷键页缺少 Esc")
        escape.tap()

        escape.swipeLeft()
        let commandPageReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: input)
        wait(for: [commandPageReady], timeout: 10)
        submitWatchText(":wq", through: input, in: app)
        let saveReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [saveReady], timeout: 10)
        send.tap()

        // 小尺寸终端中 BusyBox vi 会在写入摘要后要求再按一次回车。
        // 预先排入第二个 CR；无需确认的实现只会让 shell 执行一个空行。
        let continueReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [continueReady], timeout: 10)
        send.tap()

        let verifyPass = "ISH-VI:\(token):VERIFY:PASS"
        let verifyFail = "ISH-VI:\(token):VERIFY:FAIL"
        let verified = submitGuestLine(
            "t=\(token); if grep -Fq '\(editedText)' '\(file)'; then r=0; " +
            "else r=1; fi; rm -f '\(file)'; if test \"$r\" -eq 0; " +
            "then printf 'ISH-VI:%s:VERIFY:PASS\\n' \"$t\"; " +
            "else printf 'ISH-VI:%s:VERIFY:FAIL\\n' \"$t\"; fi",
            pass: verifyPass,
            fail: verifyFail,
            timeout: 60,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        guestRecoveryRequired = !verified
        XCTAssertTrue(verified, "BusyBox vi 没有保存唯一编辑内容")
    }

}

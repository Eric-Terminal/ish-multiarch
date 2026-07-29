import Foundation
import CryptoKit
import XCTest

@MainActor
final class iSHWatchUITests: XCTestCase {
    private enum GuestTransportResult: Equatable {
        case pass
        case retryableFail
        case fail
        case timeout
    }

    private var didWarmSystemInput = false
    private var didRecoverGuestState = false
    private var guestRecoveryRequired = false
    private var recoveryApp: XCUIApplication?

    override func setUpWithError() throws {
        continueAfterFailure = false
        didWarmSystemInput = false
        didRecoverGuestState = false
        guestRecoveryRequired = false
        recoveryApp = nil
    }

    override func tearDownWithError() throws {
        defer {
            recoveryApp?.terminate()
            recoveryApp = nil
        }

        if guestRecoveryRequired, let app = recoveryApp {
            app.terminate()
            app.launch()
            didWarmSystemInput = false

            let input = commandInput(in: app)
            let inputExists = input.waitForExistence(timeout: 180)
            XCTAssertTrue(inputExists, "异常退出后无法重新打开命令输入框执行恢复")
            if inputExists {
                let inputReady = XCTNSPredicateExpectation(
                    predicate: NSPredicate(format: "enabled == true"),
                    object: input)
                let readyResult = XCTWaiter.wait(
                    for: [inputReady],
                    timeout: 180)
                XCTAssertEqual(
                    readyResult,
                    .completed,
                    "异常退出后命令输入框没有恢复可用")

                let send = app.buttons["send-command"]
                let terminal = terminalTranscript(in: app)
                let controlsExist =
                    send.waitForExistence(timeout: 10) &&
                    terminal.waitForExistence(timeout: 10)
                XCTAssertTrue(controlsExist, "异常退出后恢复所需控件没有出现")
                if readyResult == .completed && controlsExist {
                    let recovered = recoverGuestState(
                        timeout: 120,
                        app: app,
                        input: input,
                        send: send,
                        terminal: terminal)
                    guestRecoveryRequired = !recovered
                    XCTAssertTrue(recovered, "异常退出后 guest 测试状态恢复失败")
                }
            }
        }

        try super.tearDownWithError()
    }

    func test许可证与源码无需等待Linux即可查看() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        // watchOS 26 会把 toolbar button 暴露成同标识的嵌套按钮节点。
        let settings = app.buttons.matching(
            identifier: "watch-settings-button").firstMatch
        XCTAssertTrue(
            settings.waitForExistence(timeout: 10),
            "Watch 设置入口没有出现")
        settings.tap()

        let settingsView = app.descendants(
            matching: .any)["watch-settings-view"]
        XCTAssertTrue(
            settingsView.waitForExistence(timeout: 10),
            "Watch 设置页没有打开")

        let entry = app.buttons["third-party-notices-button"]
        XCTAssertTrue(
            entry.waitForExistence(timeout: 10),
            "许可证与源码入口没有出现")
        XCTAssertTrue(
            entry.isEnabled,
            "许可证与源码入口不应受 Linux 状态禁用")

        let entryReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: entry)
        wait(for: [entryReady], timeout: 10)
        entry.tap()

        let content = app.scrollViews["third-party-notices-content"]
        XCTAssertTrue(
            content.waitForExistence(timeout: 10),
            "许可证与源码正文没有加载")

        let sourceLink = app.descendants(
            matching: .any)["project-source-link"]
        XCTAssertTrue(
            sourceLink.waitForExistence(timeout: 10),
            "公开源码入口没有出现")

        // watchOS 26 会把 toolbar button 暴露成同标识的嵌套按钮节点。
        let close = app.buttons.matching(
            identifier: "close-third-party-notices").firstMatch
        XCTAssertTrue(
            close.waitForExistence(timeout: 10),
            "许可证与源码查看页没有完成按钮")
        let closeReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: close)
        wait(for: [closeReady], timeout: 10)
        close.tap()

        let dismissed = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: content)
        wait(for: [dismissed], timeout: 10)
        XCTAssertTrue(entry.exists, "关闭许可证与源码后没有返回设置")

        let closeSettings = app.buttons.matching(
            identifier: "close-watch-settings").firstMatch
        XCTAssertTrue(
            closeSettings.waitForExistence(timeout: 10),
            "Watch 设置页没有完成按钮")
        closeSettings.tap()
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "关闭设置后没有返回终端")
    }

    func testAArch64终端命令与快捷键() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现：\n\(app.debugDescription)")

        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        XCTAssertTrue(
            send.waitForExistence(timeout: 10),
            "发送按钮没有出现")

        input.tap()
        let systemInput = app.textViews.firstMatch
        XCTAssertTrue(
            systemInput.waitForExistence(timeout: 60),
            "watchOS 系统输入框没有出现")
        systemInput.typeText("clear; uname -m")

        let done = app.buttons.matching(
            NSPredicate(format: "label IN %@", ["Done", "完成"])).firstMatch
        XCTAssertTrue(
            done.waitForExistence(timeout: 10),
            "watchOS 系统输入界面没有完成按钮")
        done.tap()
        let sendReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [sendReady], timeout: 10)
        send.tap()

        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")
        let architectureOutput = waitForTerminalOutput(
            containing: ["aarch64"],
            timeout: 60,
            terminal: terminal)
        XCTAssertTrue(
            architectureOutput.contains("aarch64"),
            architectureOutput)

        let historyToken = String(UUID().uuidString.prefix(8))
        let firstHistoryLine = "ISH-HISTORY:\(historyToken):1"
        let lastHistoryLine = "ISH-HISTORY:\(historyToken):40"
        input.tap()
        let historyInput = app.textViews.firstMatch
        XCTAssertTrue(
            historyInput.waitForExistence(timeout: 60),
            "历史窗口命令的 watchOS 系统输入框没有出现")
        historyInput.typeText(
            "for i in $(seq 1 40); do echo " +
            "ISH-HISTORY:\(historyToken):$i; done")
        XCTAssertTrue(
            done.waitForExistence(timeout: 10),
            "历史窗口命令的 watchOS 系统输入界面没有完成按钮")
        done.tap()
        let historySendReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [historySendReady], timeout: 10)
        send.tap()
        let latestHistoryOutput = waitForTerminalOutput(
            containing: [lastHistoryLine],
            timeout: 60,
            terminal: terminal)
        XCTAssertTrue(
            latestHistoryOutput.contains(lastHistoryLine),
            latestHistoryOutput)

        let loadEarlier = app.buttons.matching(
            identifier: "load-earlier-lines").firstMatch
        let earlierDragStart = terminal.coordinate(
            withNormalizedOffset: CGVector(dx: 0.5, dy: 0.35))
        let earlierDragEnd = terminal.coordinate(
            withNormalizedOffset: CGVector(dx: 0.5, dy: 0.85))
        for _ in 0..<8 where !loadEarlier.exists {
            earlierDragStart.press(
                forDuration: 0.1,
                thenDragTo: earlierDragEnd)
        }
        XCTAssertTrue(
            loadEarlier.waitForExistence(timeout: 10),
            "超过首屏行数后没有出现向上加载入口：\n" +
            app.debugDescription)
        XCTAssertTrue(loadEarlier.isHittable, "向上加载入口不可点击")
        loadEarlier.tap()

        var earlierHistoryOutput = terminalOutput(terminal)
        for _ in 0..<8 where
                !earlierHistoryOutput.contains(firstHistoryLine) {
            earlierDragStart.press(
                forDuration: 0.1,
                thenDragTo: earlierDragEnd)
            earlierHistoryOutput = terminalOutput(terminal)
        }
        XCTAssertTrue(
            earlierHistoryOutput.contains(firstHistoryLine),
            earlierHistoryOutput)

        let laterDragStart = terminal.coordinate(
            withNormalizedOffset: CGVector(dx: 0.5, dy: 0.8))
        let laterDragEnd = terminal.coordinate(
            withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3))
        for _ in 0..<12 where !input.isHittable {
            laterDragStart.press(
                forDuration: 0.1,
                thenDragTo: laterDragEnd)
        }
        XCTAssertTrue(input.isHittable, "加载历史后无法返回命令输入胶囊")

        recoveryApp = app
        didWarmSystemInput = true
        guestRecoveryRequired = true
        let recovered = recoverGuestState(
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        guestRecoveryRequired = !recovered
        didRecoverGuestState = recovered
        XCTAssertTrue(recovered, "上轮 guest 测试状态没有恢复")

        guestRecoveryRequired = true
        let clearToken = String(UUID().uuidString.prefix(8))
        let clearPass = "ISH-DNS:\(clearToken):CLEAR:PASS"
        let clearFail = "ISH-DNS:\(clearToken):CLEAR:FAIL"
        input.tap()
        let clearInput = app.textViews.firstMatch
        XCTAssertTrue(
            clearInput.waitForExistence(timeout: 60),
            "清理解析器配置的 watchOS 系统输入框没有出现")
        clearInput.typeText(
            "t=\(clearToken); if rm -f /etc/resolv.conf && sync; then " +
            "printf 'ISH-DNS:%s:CLEAR:PASS\\n' \"$t\"; else " +
            "printf 'ISH-DNS:%s:CLEAR:FAIL\\n' \"$t\"; fi")
        XCTAssertTrue(
            done.waitForExistence(timeout: 10),
            "清理解析器配置的 watchOS 系统输入界面没有完成按钮")
        done.tap()
        let clearSendReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [clearSendReady], timeout: 10)
        send.tap()
        let clearOutput = waitForTerminalOutput(
            containing: [clearPass, clearFail],
            timeout: 60,
            terminal: terminal)
        XCTAssertTrue(clearOutput.contains(clearPass), clearOutput)

        app.terminate()
        app.launch()

        let restartedInput = commandInput(in: app)
        XCTAssertTrue(
            restartedInput.waitForExistence(timeout: 180),
            "重启后命令输入框没有在期限内出现")
        let restartedReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: restartedInput)
        wait(for: [restartedReady], timeout: 180)

        let restartedSend = app.buttons["send-command"]
        XCTAssertTrue(
            restartedSend.waitForExistence(timeout: 10),
            "重启后发送按钮没有出现")
        let restartedTerminal = terminalTranscript(in: app)
        XCTAssertTrue(
            restartedTerminal.waitForExistence(timeout: 10),
            "重启后终端输出没有出现")

        let resolverToken = String(UUID().uuidString.prefix(8))
        let resolverPass = "ISH-DNS:\(resolverToken):PASS"
        let resolverFail = "ISH-DNS:\(resolverToken):FAIL"
        restartedInput.tap()
        let resolverInput = app.textViews.firstMatch
        XCTAssertTrue(
            resolverInput.waitForExistence(timeout: 60),
            "解析器门禁的 watchOS 系统输入框没有出现")
        resolverInput.typeText(
            "t=\(resolverToken); " +
            "grep -Eq '^nameserver[[:space:]]+[^[:space:]]+' " +
            "/etc/resolv.conf && " +
            "test \"$(stat -c '%u:%g:%a' /etc/resolv.conf)\" = " +
            "'0:0:644' && " +
            "printf 'ISH-DNS:%s:PASS\\n' \"$t\" || " +
            "printf 'ISH-DNS:%s:FAIL\\n' \"$t\"")
        let restartedDone = app.buttons.matching(
            NSPredicate(format: "label IN %@", ["Done", "完成"])).firstMatch
        XCTAssertTrue(
            restartedDone.waitForExistence(timeout: 10),
            "解析器门禁的 watchOS 系统输入界面没有完成按钮")
        restartedDone.tap()
        let resolverSendReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: restartedSend)
        wait(for: [resolverSendReady], timeout: 10)
        restartedSend.tap()
        let resolverOutput = waitForTerminalOutput(
            containing: [resolverPass, resolverFail],
            timeout: 60,
            terminal: restartedTerminal)
        let resolverRecovered = resolverOutput.contains(resolverPass)
        XCTAssertTrue(resolverRecovered, resolverOutput)
        if resolverRecovered {
            guestRecoveryRequired = false
        }

        let inputRow = restartedInput
        inputRow.swipeRight()

        let tab = app.buttons["send-tab"]
        var escape = app.buttons["send-escape"]
        var controlC = app.buttons["send-control-c"]
        XCTAssertTrue(tab.waitForExistence(timeout: 10), "Tab 按钮没有出现")
        XCTAssertTrue(escape.waitForExistence(timeout: 10), "Esc 按钮没有出现")
        XCTAssertTrue(controlC.waitForExistence(timeout: 10), "Ctrl-C 按钮没有出现")
        XCTAssertTrue(tab.isHittable, "Tab 按钮不可点击")
        XCTAssertTrue(escape.isHittable, "Esc 按钮不可点击")
        XCTAssertTrue(controlC.isHittable, "Ctrl-C 按钮不可点击")

        inputRow.swipeLeft()
        restartedInput.tap()
        let completionInput = app.textViews.firstMatch
        XCTAssertTrue(
            completionInput.waitForExistence(timeout: 60),
            "补全命令的 watchOS 系统输入框没有出现")
        completionInput.typeText("/ro")
        XCTAssertTrue(
            restartedDone.waitForExistence(timeout: 10),
            "补全命令的 watchOS 系统输入界面没有完成按钮")
        restartedDone.tap()
        inputRow.swipeRight()
        let tabReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: tab)
        wait(for: [tabReady], timeout: 10)
        tab.tap()

        let completionOutput = waitForTerminalOutput(
            containing: ["/root/"],
            timeout: 30,
            terminal: restartedTerminal)
        XCTAssertTrue(
            completionOutput.contains("/root/"),
            completionOutput)

        inputRow.swipeRight()
        escape = app.buttons["send-escape"]
        XCTAssertTrue(
            escape.waitForExistence(timeout: 10),
            "再次滑开后 Esc 按钮没有出现")
        escape.tap()

        inputRow.swipeRight()
        controlC = app.buttons["send-control-c"]
        XCTAssertTrue(
            controlC.waitForExistence(timeout: 10),
            "再次滑开后 Ctrl-C 按钮没有出现")
        controlC.tap()
    }

    func testAArch64基础网络与软件源() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        XCTAssertTrue(
            send.waitForExistence(timeout: 10),
            "发送按钮没有出现")
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")

        runGuestStage(
            "RESOLVER",
            command:
                "grep -Eq '^nameserver[[:space:]]+[^[:space:]]+' " +
                "/etc/resolv.conf >\"$l\" 2>&1",
            timeout: 30,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "GETENT",
            command:
                "timeout -k 15 90 getent hosts dl-cdn.alpinelinux.org " +
                ">\"$l\" 2>&1",
            timeout: 150,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "NSLOOKUP",
            command:
                "timeout -k 15 90 nslookup dl-cdn.alpinelinux.org " +
                ">\"$l\" 2>&1",
            timeout: 150,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "HTTP-IPV4",
            command:
                "ip=$(timeout -k 15 90 nslookup dl-cdn.alpinelinux.org | " +
                "awk '$1 == \"Address:\" && $2 ~ /^[0-9.]+$/ " +
                "{ print $2; exit }'); test -n \"$ip\" && " +
                "printf 'GET / HTTP/1.0\\r\\nHost: " +
                "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | " +
                "timeout -k 15 90 nc \"$ip\" 80 >\"$l\" 2>&1 && " +
                "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\"",
            timeout: 270,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "HTTP",
            command:
                "printf 'GET / HTTP/1.0\\r\\nHost: " +
                "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | " +
                "timeout -k 15 90 nc dl-cdn.alpinelinux.org 80 " +
                ">\"$l\" 2>&1 && " +
                "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\"",
            timeout: 150,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "HTTPS",
            command: commandUsingCDNIPv4(
                "a=/tmp/ish-apkindex.tar.gz; " +
                "m=/tmp/ish-apkindex-$t.list; " +
                "url=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/" +
                "aarch64/APKINDEX.tar.gz; wget_ok=; " +
                "for attempt in 1 2; do rm -f \"$a\" \"$m\"; " +
                "if timeout -k 15 300 wget -Y off -T 90 -O \"$a\" " +
                "\"$url\" >>\"$l\" 2>&1 && test -s \"$a\" && " +
                "timeout -k 15 120 tar -tzf \"$a\" " +
                ">\"$m\" 2>>\"$l\" && " +
                "grep -qx APKINDEX \"$m\"; then " +
                "wget_ok=1; break; fi; " +
                "printf 'HTTPS_IPV4_ATTEMPT_%s_FAILED\\n' " +
                "\"$attempt\" >>\"$l\"; done; " +
                "test \"$wget_ok\" = 1 && " +
                "grep -F \"Connecting to $host ($ip:\" \"$l\" >/dev/null && " +
                "rm -f \"$m\" \"$a\""),
            timeout: 1260,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "APK-CONFIG",
            command:
                "test \"$(apk --print-arch 2>\"$l\")\" = aarch64 && " +
                "grep -Fx 'https://dl-cdn.alpinelinux.org/alpine/" +
                "v3.24/main' /etc/apk/repositories >/dev/null 2>>\"$l\" && " +
                "grep -Fx 'https://dl-cdn.alpinelinux.org/alpine/" +
                "v3.24/community' /etc/apk/repositories " +
                ">/dev/null 2>>\"$l\"",
            timeout: 30,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "APK-UPDATE",
            command: commandUsingCDNIPv4(
                "s=/tmp/ish-apk-search.log; rm -f \"$s\"; " +
                "apk_ok=; for attempt in 1 2; do " +
                "if timeout -k 15 900 apk --timeout 120 update " +
                ">>\"$l\" 2>&1; then apk_ok=1; break; fi; " +
                "printf 'APK_UPDATE_IPV4_ATTEMPT_%s_FAILED\\n' " +
                "\"$attempt\" >>\"$l\"; done; test \"$apk_ok\" = 1 && " +
                "timeout -k 15 1200 apk --network=no search -x busybox " +
                ">\"$s\" 2>>\"$l\" && " +
                "grep -Eq '^busybox-[0-9]' \"$s\""),
            timeout: 3420,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    func testAArch64SQLite持久化() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        XCTAssertTrue(
            send.waitForExistence(timeout: 10),
            "发送按钮没有出现")
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")

        runGuestStage(
            "INSTALL",
            suite: "SQLITE",
            command: commandUsingCDNIPv4(
                "apk_ok=; for attempt in 1 2; do " +
                "if { if apk info -e 'sqlite=3.53.2-r0' >/dev/null 2>&1; then :; " +
                "else timeout -k 15 900 apk --timeout 120 " +
                "--cache-max-age 10080 add --no-progress " +
                "'sqlite=3.53.2-r0' >>\"$l\" 2>&1; fi; } && " +
                "timeout -k 15 900 apk --timeout 120 " +
                "--cache-max-age 10080 fix --no-progress " +
                "ncurses-terminfo-base libncursesw readline sqlite " +
                ">>\"$l\" 2>&1; then apk_ok=1; break; fi; " +
                "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' " +
                "\"$attempt\" >>\"$l\"; done; test \"$apk_ok\" = 1 && " +
                "apk info -e 'sqlite=3.53.2-r0' >>\"$l\" 2>&1 && " +
                "sqlite3 --version >>\"$l\" 2>&1 && " +
                "test \"$(sqlite3 --version | awk '{print $1}')\" = 3.53.2"),
            timeout: 4050,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "WAL",
            suite: "SQLITE",
            command:
                "d=/root/.ish-sqlite-gate; db=$d/matrix.db; o=$d/result; " +
                "rm -rf \"$d\"; mkdir -p \"$d\" && " +
                "sqlite3 \"$db\" 'PRAGMA journal_mode=WAL; " +
                "CREATE TABLE values_under_test(value INTEGER); " +
                "BEGIN; INSERT INTO values_under_test VALUES(40),(2); " +
                "COMMIT; SELECT sum(value) FROM values_under_test; " +
                "PRAGMA integrity_check;' >\"$o\" 2>\"$l\" && " +
                "test \"$(sed -n '1p' \"$o\")\" = wal && " +
                "test \"$(sed -n '2p' \"$o\")\" = 42 && " +
                "test \"$(sed -n '3p' \"$o\")\" = ok && " +
                "test -s \"$db\" && sync",
            timeout: 180,
            app: app,
            input: input,
            send: send,
            terminal: terminal)

        app.terminate()
        app.launch()

        let restartedInput = commandInput(in: app)
        XCTAssertTrue(
            restartedInput.waitForExistence(timeout: 180),
            "重启后命令输入框没有在期限内出现")
        let restartedReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: restartedInput)
        wait(for: [restartedReady], timeout: 180)

        let restartedSend = app.buttons["send-command"]
        XCTAssertTrue(
            restartedSend.waitForExistence(timeout: 10),
            "重启后发送按钮没有出现")
        let restartedTerminal = terminalTranscript(in: app)
        XCTAssertTrue(
            restartedTerminal.waitForExistence(timeout: 10),
            "重启后终端输出没有出现")
        runGuestStage(
            "RESTART",
            suite: "SQLITE",
            command:
                "d=/root/.ish-sqlite-gate; db=$d/matrix.db; " +
                "command -v sqlite3 >\"$l\" 2>&1 && " +
                "test \"$(sqlite3 \"$db\" 'SELECT sum(value) " +
                "FROM values_under_test;')\" = 42 && " +
                "test \"$(sqlite3 \"$db\" 'PRAGMA integrity_check;')\" = ok && " +
                "rm -rf \"$d\"",
            timeout: 180,
            app: app,
            input: restartedInput,
            send: restartedSend,
            terminal: restartedTerminal)
    }

    func testAArch64Python运行时() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        XCTAssertTrue(
            send.waitForExistence(timeout: 10),
            "发送按钮没有出现")
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")

        runGuestStage(
            "INSTALL",
            suite: "PYTHON",
            command: commandUsingCDNIPv4(
                "if apk info -e 'python3=3.14.5-r0' " +
                ">/dev/null 2>&1; then :; else " +
                "apk_ok=; for attempt in 1 2; do " +
                "if timeout -k 30 3600 apk --timeout 120 " +
                "--cache-max-age 10080 add --no-progress " +
                "'python3=3.14.5-r0' >>\"$l\" 2>&1; then " +
                "apk_ok=1; break; fi; " +
                "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' " +
                "\"$attempt\" >>\"$l\"; done; " +
                "test \"$apk_ok\" = 1; fi && " +
                "apk info -e 'python3=3.14.5-r0' >>\"$l\" 2>&1 && " +
                "timeout -k 30 900 python3 -I -c 'import platform, sys; " +
                "assert platform.machine() == \"aarch64\"; " +
                "assert sys.version_info[:3] == (3, 14, 5)' " +
                ">>\"$l\" 2>&1"),
            timeout: 8700,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "STDLIB",
            suite: "PYTHON",
            command:
                "timeout -k 15 300 python3 -I -c 'import hashlib, json, " +
                "math, sqlite3, ssl, zlib; " +
                "payload = {\"arch\": \"aarch64\", " +
                "\"value\": 42}; assert json.loads(json.dumps(payload)) " +
                "== payload; assert hashlib.sha256(b\"ish\").hexdigest() " +
                "== \"b7d4d64a63e537b82a0340732bd0bc89649b6dd0f6c66416711f2dcd7de2b3a2\"; " +
                "assert math.factorial(10) == 3628800; " +
                "assert sqlite3.connect(\":memory:\").execute(" +
                "\"select 6 * 7\").fetchone()[0] == 42; " +
                "assert ssl.OPENSSL_VERSION.startswith(\"OpenSSL \"); " +
                "assert zlib.decompress(zlib.compress(b\"python\")) " +
                "== b\"python\"' >\"$l\" 2>&1",
            timeout: 420,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "FILE",
            suite: "PYTHON",
            command:
                "d=/root/.ish-python-gate-$t; export d; rm -rf \"$d\"; " +
                "mkdir -p \"$d\" && timeout -k 15 300 python3 -I -c " +
                "'from pathlib import Path; import os; " +
                "p = Path(os.environ[\"d\"]); " +
                "f = open(p / \"state\", \"wb\"); " +
                "assert f.write(b\"python-watch\\n\" * 64) == 832; " +
                "f.flush(); os.fsync(f.fileno()); f.close(); " +
                "(p / \"state\").rename(p / \"renamed\")' " +
                ">\"$l\" 2>&1 && timeout -k 15 300 python3 -I -c " +
                "'from pathlib import Path; import os; " +
                "p = Path(os.environ[\"d\"]); " +
                "assert (p / \"renamed\").read_bytes() == " +
                "b\"python-watch\\n\" * 64; " +
                "assert (p / \"renamed\").stat().st_size == 832' " +
                ">>\"$l\" 2>&1 && sync && rm -rf \"$d\"",
            timeout: 720,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "THREAD",
            suite: "PYTHON",
            command:
                "timeout -k 15 300 python3 -I -c 'import queue, threading; " +
                "gate = threading.Barrier(5, timeout=120); " +
                "results = queue.Queue(); " +
                "main_id = threading.get_native_id(); " +
                "threads = [threading.Thread(target=lambda n=n: " +
                "(gate.wait(), results.put((threading.get_native_id(), " +
                "n * n))), daemon=True) for n in range(4)]; " +
                "[thread.start() for thread in threads]; gate.wait(); " +
                "[thread.join(60) for thread in threads]; " +
                "assert all(not thread.is_alive() for thread in threads); " +
                "pairs = [results.get_nowait() for _ in range(4)]; " +
                "ids = [pair[0] for pair in pairs]; " +
                "assert len(set(ids)) == 4 and main_id not in ids; " +
                "assert sorted(pair[1] for pair in pairs) == " +
                "[0, 1, 4, 9]' >\"$l\" 2>&1",
            timeout: 420,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    func testAArch64本地编译与Pthread线程() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        XCTAssertTrue(
            send.waitForExistence(timeout: 10),
            "发送按钮没有出现")
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")

        runGuestStage(
            "INSTALL",
            suite: "BUILD",
            command: commandUsingCDNIPv4(
                "if apk info -e 'build-base=0.5-r4' " +
                ">/dev/null 2>&1; then :; else " +
                "apk_ok=; for attempt in 1 2; do " +
                "if timeout -k 30 7200 apk --timeout 120 " +
                "--cache-max-age 10080 add --no-progress " +
                "'build-base=0.5-r4' >>\"$l\" 2>&1; then " +
                "apk_ok=1; break; fi; " +
                "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' " +
                "\"$attempt\" >>\"$l\"; done; " +
                "test \"$apk_ok\" = 1; fi && " +
                "apk info -e 'build-base=0.5-r4' >>\"$l\" 2>&1 && " +
                "command -v cc >>\"$l\" 2>&1 && " +
                "machine=$(timeout -k 15 300 cc -dumpmachine 2>>\"$l\") && " +
                "test \"$machine\" = aarch64-alpine-linux-musl"),
            timeout: 15300,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "C",
            suite: "BUILD",
            command:
                "d=/root/.ish-build-c-$t; rm -rf \"$d\"; " +
                "mkdir -p \"$d\" && printf '%s\\n' " +
                "'#include <stdint.h>' '#include <stdio.h>' " +
                "'int main(void) {' " +
                "'    uint64_t value = 1;' " +
                "'    for (uint64_t factor = 2; factor <= 10; factor++) {' " +
                "'        value *= factor;' '    }' " +
                "'    if (value != 3628800ULL) return 1;' " +
                "'    printf(\"C_OK:%llu\\n\", " +
                "(unsigned long long)value);' " +
                "'    return 0;' '}' >\"$d/main.c\" && " +
                "timeout -k 30 1800 cc -std=c11 -O2 -Wall -Wextra -Werror " +
                "\"$d/main.c\" -o \"$d/main\" >>\"$l\" 2>&1 && " +
                "timeout -k 15 300 \"$d/main\" >\"$d/result\" 2>>\"$l\" && " +
                "cat \"$d/result\" >>\"$l\" && " +
                "grep -qx 'C_OK:3628800' \"$d/result\" && rm -rf \"$d\"",
            timeout: 2400,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "PTHREAD",
            suite: "BUILD",
            command:
                "d=/root/.ish-build-pthread-$t; rm -rf \"$d\"; " +
                "mkdir -p \"$d\" && printf '%s\\n' " +
                "'#define _POSIX_C_SOURCE 200809L' " +
                "'#include <pthread.h>' '#include <stdint.h>' " +
                "'#include <stdio.h>' " +
                "'struct job { unsigned index; uint64_t result; };' " +
                "'static void *worker(void *opaque) {' " +
                "'    struct job *job = opaque;' " +
                "'    job->result = (uint64_t)(job->index + 1U) * " +
                "(job->index + 11U);' '    return job;' '}' " +
                "'int main(void) {' '    pthread_t threads[4];' " +
                "'    struct job jobs[4] = " +
                "{{0, 0}, {1, 0}, {2, 0}, {3, 0}};' " +
                "'    for (unsigned i = 0; i < 4; i++) {' " +
                "'        if (pthread_create(&threads[i], 0, worker, " +
                "&jobs[i]) != 0) return 10 + (int)i;' '    }' " +
                "'    uint64_t total = 0;' " +
                "'    for (unsigned i = 0; i < 4; i++) {' " +
                "'        void *returned = 0;' " +
                "'        if (pthread_join(threads[i], &returned) != 0 || " +
                "returned != &jobs[i]) return 20 + (int)i;' " +
                "'        total += jobs[i].result;' '    }' " +
                "'    if (total != 130) return 30;' " +
                "'    printf(\"PTHREAD_OK:%llu\\n\", " +
                "(unsigned long long)total);' " +
                "'    return 0;' '}' >\"$d/main.c\" && " +
                "timeout -k 30 1800 cc -std=c11 -O2 -Wall -Wextra -Werror " +
                "-pthread \"$d/main.c\" -o \"$d/main\" >>\"$l\" 2>&1 && " +
                "timeout -k 15 300 \"$d/main\" >\"$d/result\" 2>>\"$l\" && " +
                "cat \"$d/result\" >>\"$l\" && " +
                "grep -qx 'PTHREAD_OK:130' \"$d/result\" && rm -rf \"$d\"",
            timeout: 2400,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    func testAArch64Git与SSH客户端() throws {
        let app = XCUIApplication()
        recoveryApp = app
        guestRecoveryRequired = true
        app.launch()

        let input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "命令输入框没有在期限内出现")
        let ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        let send = app.buttons["send-command"]
        XCTAssertTrue(
            send.waitForExistence(timeout: 10),
            "发送按钮没有出现")
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")

        runGuestStage(
            "INSTALL",
            suite: "DEV",
            command: commandUsingCDNIPv4(
                "if apk info -e 'git=2.54.0-r0' >/dev/null 2>&1 && " +
                "apk info -e 'openssh-client-default=10.3_p1-r0' " +
                ">/dev/null 2>&1; then :; else " +
                "apk_ok=; for attempt in 1 2; do " +
                "if timeout -k 30 7200 apk --timeout 120 " +
                "--cache-max-age 10080 add --no-progress " +
                "'git=2.54.0-r0' " +
                "'openssh-client-default=10.3_p1-r0' " +
                ">>\"$l\" 2>&1; then apk_ok=1; break; fi; " +
                "printf 'APK_INSTALL_IPV4_ATTEMPT_%s_FAILED\\n' " +
                "\"$attempt\" >>\"$l\"; done; " +
                "test \"$apk_ok\" = 1; fi && " +
                "apk info -e 'git=2.54.0-r0' >>\"$l\" 2>&1 && " +
                "apk info -e 'openssh-client-default=10.3_p1-r0' " +
                ">>\"$l\" 2>&1 && " +
                "test \"$(git --version)\" = 'git version 2.54.0' && " +
                "ssh -V 2>&1 | grep -F 'OpenSSH_10.3p1' >>\"$l\""),
            timeout: 15000,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "GIT",
            suite: "DEV",
            command:
                "d=/root/.ish-git-gate-$t; rm -rf \"$d\"; " +
                "mkdir -p \"$d/repo\" && git -C \"$d/repo\" init -q && " +
                "git -C \"$d/repo\" config user.name 'iSH Watch Gate' && " +
                "git -C \"$d/repo\" config user.email 'watch@localhost' && " +
                "printf 'alpha\\n' >\"$d/repo/state.txt\" && " +
                "git -C \"$d/repo\" add state.txt && " +
                "git -C \"$d/repo\" commit -q -m initial && " +
                "printf 'beta\\n' >>\"$d/repo/state.txt\" && " +
                "git -C \"$d/repo\" commit -qam update && sync && " +
                "test \"$(git -C \"$d/repo\" rev-list --count HEAD)\" = 2 && " +
                "test \"$(git -C \"$d/repo\" cat-file -t HEAD)\" = commit && " +
                "git -C \"$d/repo\" show HEAD:state.txt >\"$d/readback\" && " +
                "test \"$(sed -n '1p' \"$d/readback\")\" = alpha && " +
                "test \"$(sed -n '2p' \"$d/readback\")\" = beta && " +
                "test -z \"$(sed -n '3p' \"$d/readback\")\" && " +
                "test -z \"$(git -C \"$d/repo\" status --porcelain)\" && " +
                "git -C \"$d/repo\" fsck --strict --full --no-dangling " +
                ">>\"$l\" 2>&1 && rm -rf \"$d\"",
            timeout: 900,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "SSH",
            suite: "DEV",
            command:
                "d=/root/.ish-ssh-gate-$t; rm -rf \"$d\"; " +
                "mkdir -m 700 \"$d\" && " +
                "timeout -k 15 600 ssh-keygen -q -t ed25519 -N '' " +
                "-C ish-watch -f \"$d/id_ed25519\" >>\"$l\" 2>&1 && " +
                "test \"$(stat -c '%a' \"$d/id_ed25519\")\" = 600 && " +
                "test \"$(stat -c '%a' \"$d/id_ed25519.pub\")\" = 644 && " +
                "ssh-keygen -y -f \"$d/id_ed25519\" >\"$d/derived.pub\" && " +
                "test \"$(cut -d ' ' -f 1-2 \"$d/derived.pub\")\" = " +
                "\"$(cut -d ' ' -f 1-2 \"$d/id_ed25519.pub\")\" && " +
                "ssh-keygen -lf \"$d/id_ed25519.pub\" " +
                ">>\"$l\" 2>&1 && " +
                "ssh -Q key | grep -qx ssh-ed25519 && " +
                "ssh -G -F /dev/null -o BatchMode=yes -p 2222 " +
                "example.invalid >\"$d/config\" 2>>\"$l\" && " +
                "grep -qx 'port 2222' \"$d/config\" && " +
                "grep -qx 'batchmode yes' \"$d/config\" && rm -rf \"$d\"",
            timeout: 900,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    private func commandInput(in app: XCUIApplication) -> XCUIElement {
        app.descendants(
            matching: .any
        ).matching(
            identifier: "command-input"
        ).firstMatch
    }

    private func terminalTranscript(
            in app: XCUIApplication) -> XCUIElement {
        app.descendants(
            matching: .any
        ).matching(
            identifier: "terminal-transcript"
        ).firstMatch
    }

    private func terminalOutput(_ terminal: XCUIElement) -> String {
        let blocks = terminal.descendants(
            matching: .staticText
        ).matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "terminal-line-"))
        return (0..<blocks.count).map {
            blocks.element(boundBy: $0).label
        }.joined(separator: "\n")
    }

    private func waitForTerminalOutput(
        containing candidates: [String],
        timeout: TimeInterval,
        pollInterval: TimeInterval = 0.5,
        terminal: XCUIElement
    ) -> String {
        let deadline = Date().addingTimeInterval(timeout)
        var output = terminalOutput(terminal)
        while !candidates.contains(where: output.contains) {
            let remaining = deadline.timeIntervalSinceNow
            guard remaining > 0 else { break }
            Thread.sleep(
                forTimeInterval: min(pollInterval, remaining))
            output = terminalOutput(terminal)
        }
        return output
    }

    private func commandUsingCDNIPv4(_ command: String) -> String {
        // 公网软件门禁仍使用原 hostname、SNI 与证书，只稳定选择 DNS 当前的 A 记录。
        let prefix =
            "(hosts=/etc/hosts; host=dl-cdn.alpinelinux.org; " +
            "b=/root/.ish-watch-ipv4-gate-hosts; n=$b.new; g=; m=; " +
            "restore_hosts() { r=$1; trap - 0 HUP INT TERM; " +
            "if test -n \"$g\" && ! rm -f \"$g\"; then r=125; fi; " +
            "if test -n \"$m\" && ! rm -f \"$m\"; then r=125; fi; " +
            "if cp -p \"$b\" \"$hosts\" && cmp -s \"$b\" \"$hosts\" && " +
            "test \"$(stat -c '%u:%g:%a' \"$b\")\" = " +
            "\"$(stat -c '%u:%g:%a' \"$hosts\")\"; then " +
            "if ! rm -f \"$b\" \"$n\"; then r=125; fi; " +
            "else r=125; rm -f \"$n\" || :; fi; exit \"$r\"; }; " +
            "if test -f \"$b\"; then " +
            "if cp -p \"$b\" \"$hosts\" && cmp -s \"$b\" \"$hosts\" && " +
            "test \"$(stat -c '%u:%g:%a' \"$b\")\" = " +
            "\"$(stat -c '%u:%g:%a' \"$hosts\")\" && rm -f \"$b\" \"$n\"; " +
            "then :; else rm -f \"$n\" || :; exit 125; fi; fi; " +
            "rm -f \"$n\" || exit 125; " +
            "unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY " +
            "ALL_PROXY all_proxy SSL_NO_VERIFY_HOSTNAME; " +
            "NO_PROXY=\"$host\"; no_proxy=\"$host\"; export NO_PROXY no_proxy; " +
            "ip=$(timeout -k 15 90 nslookup -type=A \"$host\" 2>>\"$l\" | " +
            "awk '$1 == \"Name:\" { answer=1; next } " +
            "answer && $1 == \"Address:\" && $2 ~ /^[0-9.]+$/ " +
            "{ print $2; exit }'); test -n \"$ip\" || exit; " +
            "printf 'PINNED_IPV4=%s\\n' \"$ip\" >>\"$l\" || exit; " +
            "cp -p \"$hosts\" \"$n\" && mv \"$n\" \"$b\" || exit; " +
            "trap 'restore_hosts $?' 0; trap 'restore_hosts 129' HUP; " +
            "trap 'restore_hosts 130' INT; trap 'restore_hosts 143' TERM; " +
            "printf '\\n%s\\t%s\\n' \"$ip\" \"$host\" >>\"$hosts\" || exit; " +
            "g=/tmp/ish-ipv4-getent-$t; rm -f \"$g\" || exit; " +
            "timeout -k 15 90 getent ahostsv4 \"$host\" " +
            ">\"$g\" 2>>\"$l\" || exit; " +
            "awk -v ip=\"$ip\" " +
            "'NF { seen=1; if ($1 != ip) bad=1 } " +
            "END { exit !(seen && !bad) }' \"$g\" || exit; " +
            "rm -f \"$g\" || exit; { "
        let suffix =
            "; }; r=$?; restore_hosts \"$r\")"
        return prefix + command + suffix
    }

    private func typeGuestLine(
        _ line: String,
        into systemInput: XCUIElement
    ) {
        // 分批合成键盘事件，但只把一条完整的短命令交给 Quickboard。
        var start = line.startIndex
        while start != line.endIndex {
            let end = line.index(
                start,
                offsetBy: 512,
                limitedBy: line.endIndex) ?? line.endIndex
            systemInput.typeText(String(line[start..<end]))
            start = end
        }
    }

    private func waitForTransportPass(
        _ pass: String,
        fail: String,
        timeout: TimeInterval,
        terminal: XCUIElement
    ) -> GuestTransportResult {
        let pollInterval: TimeInterval
        if timeout >= 3600 {
            pollInterval = 60
        } else if timeout >= 600 {
            pollInterval = 30
        } else {
            pollInterval = 1
        }

        let output = waitForTerminalOutput(
            containing: [pass, fail],
            timeout: timeout,
            pollInterval: pollInterval,
            terminal: terminal)

        guard output.contains(pass) || output.contains(fail) else {
            return .timeout
        }
        if output.contains(pass) {
            return .pass
        }
        let retryableFail = fail + ":1"
        if let range = output.range(of: retryableFail),
           range.upperBound == output.endIndex ||
               !output[range.upperBound].isNumber {
            return .retryableFail
        }
        return .fail
    }

    private func submitGuestLineResult(
        _ line: String,
        pass: String,
        fail: String,
        timeout: TimeInterval,
        app: XCUIApplication,
        input: XCUIElement,
        send: XCUIElement,
        terminal: XCUIElement
    ) -> GuestTransportResult {
        let lineLength = line.lengthOfBytes(using: .utf8) + 1
        // BusyBox ash 的交互编辑缓冲最多接收 2046 个 ASCII 载荷字符。
        XCTAssertLessThanOrEqual(lineLength, 1800, "guest 传输命令超过安全行长")
        guard lineLength <= 1800 else { return .fail }

        input.tap()
        let systemInput = app.textViews.firstMatch
        if !didWarmSystemInput {
            // 首次冷启动期间不做 AX 查询，避免拖慢只有五秒展示期限的 Quickboard。
            Thread.sleep(forTimeInterval: 20)
            didWarmSystemInput = true
        } else {
            _ = systemInput.waitForExistence(timeout: 5)
        }
        if !systemInput.exists {
            // 冷启动错过系统展示期限后，复用已经预热的服务重试一次。
            input.tap()
        }
        let inputExists = systemInput.waitForExistence(timeout: 60)
        XCTAssertTrue(inputExists, "watchOS 系统输入框没有出现")
        guard inputExists else { return .timeout }
        typeGuestLine(line, into: systemInput)

        let done = app.buttons.matching(
            NSPredicate(format: "label IN %@", ["Done", "完成"])).firstMatch
        let doneExists = done.waitForExistence(timeout: 10)
        XCTAssertTrue(doneExists, "watchOS 系统输入界面没有完成按钮")
        guard doneExists else { return .timeout }
        done.tap()

        let sendReady = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == true"),
            object: send)
        let sendResult = XCTWaiter.wait(for: [sendReady], timeout: 10)
        XCTAssertEqual(sendResult, .completed, "发送按钮没有恢复可点击状态")
        guard sendResult == .completed else { return .timeout }
        send.tap()
        return waitForTransportPass(
            pass,
            fail: fail,
            timeout: timeout,
            terminal: terminal)
    }

    private func submitGuestLine(
        _ line: String,
        pass: String,
        fail: String,
        timeout: TimeInterval,
        app: XCUIApplication,
        input: XCUIElement,
        send: XCUIElement,
        terminal: XCUIElement
    ) -> Bool {
        let result = submitGuestLineResult(
            line,
            pass: pass,
            fail: fail,
            timeout: timeout,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        XCTAssertNotEqual(
            result,
            .timeout,
            "guest 命令传输或执行未在期限内确认：" +
            terminalOutput(terminal))
        guard result != .timeout else { return false }
        XCTAssertEqual(
            result,
            .pass,
            "guest 脚本传输失败：" + terminalOutput(terminal))
        return result == .pass
    }

    private func recoverGuestState(
        timeout: TimeInterval,
        app: XCUIApplication,
        input: XCUIElement,
        send: XCUIElement,
        terminal: XCUIElement
    ) -> Bool {
        let token = String(UUID().uuidString.prefix(8))
        let pass = "ISH-RECOVER:\(token):PASS"
        let fail = "ISH-RECOVER:\(token):FAIL"
        let line =
            "h=/etc/hosts; b=/root/.ish-watch-ipv4-gate-hosts; " +
            "n=$b.new; r=0; if test -f \"$b\"; then " +
            "if cp -p \"$b\" \"$h\" && cmp -s \"$b\" \"$h\" && " +
            "test \"$(stat -c '%u:%g:%a' \"$b\")\" = " +
            "\"$(stat -c '%u:%g:%a' \"$h\")\"; then " +
            "if ! rm -f \"$b\" \"$n\"; then r=125; fi; " +
            "else r=125; rm -f \"$n\" || :; fi; " +
            "elif ! rm -f \"$n\"; then r=125; fi; " +
            "if ! grep -Eq '^nameserver[[:space:]]+[^[:space:]]+' " +
            "/etc/resolv.conf || " +
            "test \"$(stat -c '%u:%g:%a' /etc/resolv.conf 2>/dev/null)\" " +
            "!= '0:0:644'; then r=125; fi; " +
            "if ! rm -f /tmp/.ish-watch-uitest-stage.b64 " +
            "/tmp/.ish-watch-uitest-stage.sh " +
            "/tmp/ish-ipv4-getent-* /tmp/ish-apkindex-*.list " +
            "/tmp/ish-apkindex.tar.gz; " +
            "then r=125; fi; if test \"$r\" -eq 0; then " +
            "printf 'ISH-RECOVER:%s:PASS\\n' '\(token)'; else " +
            "printf 'ISH-RECOVER:%s:FAIL:%s\\n' '\(token)' \"$r\"; fi"
        return submitGuestLine(
            line,
            pass: pass,
            fail: fail,
            timeout: timeout,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    private func submitGuestScript(
        _ script: String,
        token: String,
        timeout: TimeInterval,
        app: XCUIApplication,
        input: XCUIElement,
        send: XCUIElement,
        terminal: XCUIElement
    ) -> Bool {
        let scriptData = Data(script.utf8)
        let encoded = scriptData.base64EncodedString()
        let expectedSHA256 = SHA256.hash(data: scriptData)
            .map { String(format: "%02x", $0) }
            .joined()
        let base64Path = "/tmp/.ish-watch-uitest-stage.b64"
        let scriptPath = "/tmp/.ish-watch-uitest-stage.sh"
        let payloadLength = 1024
        let maximumAttempts = 2

        for attempt in 1...maximumAttempts {
            let transportToken = "\(token)-\(attempt)"
            var start = encoded.startIndex
            var cumulative = 0
            var fragment = 0
            var shouldRetry = false

            while start != encoded.endIndex {
                let end = encoded.index(
                    start,
                    offsetBy: payloadLength,
                    limitedBy: encoded.endIndex) ?? encoded.endIndex
                let payload = String(encoded[start..<end])
                cumulative += payload.utf8.count
                fragment += 1
                let number = String(format: "%04d", fragment)
                let pass = "ISH-XFER:\(transportToken):\(number):ACK"
                let fail = "ISH-XFER:\(transportToken):\(number):FAIL"
                let prepare = fragment == 1
                    ? "rm -f \"$b\" \"$s\" && "
                    : ""
                let redirect = fragment == 1 ? ">" : ">>"
                let line =
                    "b='\(base64Path)'; s='\(scriptPath)'; " +
                    "if \(prepare)printf '%s' '\(payload)' \(redirect)\"$b\" && " +
                    "test \"$(/bin/busybox wc -c <\"$b\")\" -eq \(cumulative); " +
                    "then printf 'ISH-XFER:%s:%s:ACK\\n' " +
                    "'\(transportToken)' '\(number)'; " +
                    "else r=$?; if ! rm -f \"$b\" \"$s\"; then r=125; fi; " +
                    "printf 'ISH-XFER:%s:%s:FAIL:%s\\n' " +
                    "'\(transportToken)' '\(number)' \"$r\"; fi"
                let result = submitGuestLineResult(
                    line,
                    pass: pass,
                    fail: fail,
                    timeout: 90,
                    app: app,
                    input: input,
                    send: send,
                    terminal: terminal)
                if result == .timeout {
                    XCTFail(
                        "guest 脚本分片状态不确定，禁止盲目重试：" +
                        terminalOutput(terminal))
                    return false
                }
                if result == .retryableFail {
                    shouldRetry = true
                    break
                }
                if result == .fail {
                    XCTFail(
                        "guest 脚本分片清理状态不安全，禁止重试：" +
                        terminalOutput(terminal))
                    return false
                }
                start = end
            }
            if shouldRetry {
                continue
            }

            let done = "ISH-XFER:\(transportToken):FINAL:DONE"
            let fail = "ISH-XFER:\(transportToken):FINAL:FAIL"
            let execute =
                "b='\(base64Path)'; s='\(scriptPath)'; " +
                "t='\(transportToken)'; r=0; " +
                "if /bin/busybox base64 -d \"$b\" >\"$s\" && " +
                "test \"$(/bin/busybox wc -c <\"$s\")\" -eq " +
                "\(scriptData.count) && " +
                "actual=$(/bin/busybox sha256sum \"$s\") && " +
                "actual=${actual%% *} && test \"$actual\" = " +
                "'\(expectedSHA256)' && " +
                "rm -f \"$b\"; then /bin/busybox sh \"$s\"; r=$?; " +
                "else r=125; fi; " +
                "if ! rm -f \"$b\" \"$s\"; then r=125; fi; " +
                "if test \"$r\" -eq 0; then " +
                "printf 'ISH-XFER:%s:FINAL:DONE\\n' \"$t\"; else " +
                "printf 'ISH-XFER:%s:FINAL:FAIL:%s\\n' \"$t\" \"$r\"; fi"
            return submitGuestLine(
                execute,
                pass: done,
                fail: fail,
                timeout: timeout,
                app: app,
                input: input,
                send: send,
                terminal: terminal)
        }

        XCTFail(
            "guest 脚本分片在一次安全重试后仍传输失败：" +
            terminalOutput(terminal))
        return false
    }

    private func runGuestStage(
        _ stage: String,
        suite: String = "NET",
        command: String,
        timeout: TimeInterval,
        app: XCUIApplication,
        input: XCUIElement,
        send: XCUIElement,
        terminal: XCUIElement
    ) {
        recoveryApp = app
        if !didRecoverGuestState {
            guestRecoveryRequired = true
            let recovered = recoverGuestState(
                timeout: 120,
                app: app,
                input: input,
                send: send,
                terminal: terminal)
            guestRecoveryRequired = !recovered
            didRecoverGuestState = recovered
            XCTAssertTrue(recovered, "上轮 guest 测试状态没有恢复")
            guard recovered else { return }
        }

        let token = String(UUID().uuidString.prefix(8))
        let pass = "ISH-\(suite):\(token):\(stage):PASS"
        let fail = "ISH-\(suite):\(token):\(stage):FAIL"
        let log = "/tmp/ish-\(suite.lowercased())-\(stage.lowercased()).log"
        let script =
            "t=\(token); l=\(log); " +
            "if rm -f \"$l\" && { \(command); }; then " +
            "printf 'ISH-\(suite):%s:\(stage):PASS\\n' \"$t\"; else r=$?; " +
            "tail -c 4096 \"$l\" 2>/dev/null; " +
            "printf '\\nISH-\(suite):%s:\(stage):FAIL:%s\\n' \"$t\" \"$r\"; fi"
        let usesIPv4Scope =
            command.contains("/root/.ish-watch-ipv4-gate-hosts")
        // 任一传输中断都可能留下固定 stage 文件；业务通过后再解除恢复责任。
        guestRecoveryRequired = true
        guard submitGuestScript(
            script,
            token: token,
            timeout: timeout,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        else {
            return
        }
        if !usesIPv4Scope {
            guestRecoveryRequired = false
        }

        let output = waitForTerminalOutput(
            containing: [pass, fail],
            timeout: 30,
            terminal: terminal)
        let didPass = output.contains(pass)
        XCTAssertTrue(didPass, output)
        if didPass && usesIPv4Scope {
            guestRecoveryRequired = false
        }
    }
}

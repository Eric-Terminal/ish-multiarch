import XCTest

@MainActor
final class iSHWatchUITests: XCTestCase {
    private var didWarmSystemInput = false

    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testAArch64终端命令与快捷键() throws {
        let app = XCUIApplication()
        app.launch()

        let input = app.textFields["command-input"]
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

        input.tap()
        let systemInput = app.textViews.firstMatch
        XCTAssertTrue(
            systemInput.waitForExistence(timeout: 10),
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

        let terminal = app.staticTexts["terminal-output"]
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")
        let architecture = expectation(
            for: NSPredicate(format: "label CONTAINS %@", "aarch64"),
            evaluatedWith: terminal)
        wait(for: [architecture], timeout: 60)

        let clearToken = String(UUID().uuidString.prefix(8))
        let clearPass = "ISH-DNS:\(clearToken):CLEAR:PASS"
        let clearFail = "ISH-DNS:\(clearToken):CLEAR:FAIL"
        input.tap()
        let clearInput = app.textViews.firstMatch
        XCTAssertTrue(
            clearInput.waitForExistence(timeout: 10),
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
        let clearFinished = expectation(
            for: NSPredicate(
                format: "label CONTAINS %@ OR label CONTAINS %@",
                clearPass,
                clearFail),
            evaluatedWith: terminal)
        wait(for: [clearFinished], timeout: 60)
        XCTAssertTrue(terminal.label.contains(clearPass), terminal.label)

        app.terminate()
        app.launch()

        let restartedInput = app.textFields["command-input"]
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
        let restartedTerminal = app.staticTexts["terminal-output"]
        XCTAssertTrue(
            restartedTerminal.waitForExistence(timeout: 10),
            "重启后终端输出没有出现")

        let resolverToken = String(UUID().uuidString.prefix(8))
        let resolverPass = "ISH-DNS:\(resolverToken):PASS"
        let resolverFail = "ISH-DNS:\(resolverToken):FAIL"
        restartedInput.tap()
        let resolverInput = app.textViews.firstMatch
        XCTAssertTrue(
            resolverInput.waitForExistence(timeout: 10),
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
        let resolverFinished = expectation(
            for: NSPredicate(
                format: "label CONTAINS %@ OR label CONTAINS %@",
                resolverPass,
                resolverFail),
            evaluatedWith: restartedTerminal)
        wait(for: [resolverFinished], timeout: 60)
        XCTAssertTrue(
            restartedTerminal.label.contains(resolverPass),
            restartedTerminal.label)

        let tab = app.buttons["send-tab"]
        let escape = app.buttons["send-escape"]
        let controlC = app.buttons["send-control-c"]
        XCTAssertTrue(tab.waitForExistence(timeout: 10), "Tab 按钮没有出现")
        XCTAssertTrue(escape.waitForExistence(timeout: 10), "Esc 按钮没有出现")
        XCTAssertTrue(controlC.waitForExistence(timeout: 10), "Ctrl-C 按钮没有出现")
        XCTAssertTrue(tab.isHittable, "Tab 按钮不可点击")
        XCTAssertTrue(escape.isHittable, "Esc 按钮不可点击")
        XCTAssertTrue(controlC.isHittable, "Ctrl-C 按钮不可点击")

        restartedInput.tap()
        let completionInput = app.textViews.firstMatch
        XCTAssertTrue(
            completionInput.waitForExistence(timeout: 10),
            "补全命令的 watchOS 系统输入框没有出现")
        completionInput.typeText("/ro")
        XCTAssertTrue(
            restartedDone.waitForExistence(timeout: 10),
            "补全命令的 watchOS 系统输入界面没有完成按钮")
        restartedDone.tap()
        let tabReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: tab)
        wait(for: [tabReady], timeout: 10)
        tab.tap()
        let completion = expectation(
            for: NSPredicate(format: "label CONTAINS %@", "/root/"),
            evaluatedWith: restartedTerminal)
        wait(for: [completion], timeout: 30)
        escape.tap()
        controlC.tap()
    }

    func testAArch64基础网络与软件源() throws {
        let app = XCUIApplication()
        app.launch()

        let input = app.textFields["command-input"]
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
        let terminal = app.staticTexts["terminal-output"]
        XCTAssertTrue(
            terminal.waitForExistence(timeout: 10),
            "终端输出没有出现")

        runNetworkStage(
            "RESOLVER",
            command:
                "grep -Eq '^nameserver[[:space:]]+[^[:space:]]+' " +
                "/etc/resolv.conf >\"$l\" 2>&1",
            timeout: 30,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runNetworkStage(
            "GETENT",
            command:
                "timeout 90 getent hosts dl-cdn.alpinelinux.org " +
                ">\"$l\" 2>&1",
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runNetworkStage(
            "NSLOOKUP",
            command:
                "timeout 90 nslookup dl-cdn.alpinelinux.org " +
                ">\"$l\" 2>&1",
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runNetworkStage(
            "HTTP-IPV4",
            command:
                "ip=$(nslookup dl-cdn.alpinelinux.org | " +
                "awk '$1 == \"Address:\" && $2 ~ /^[0-9.]+$/ " +
                "{ print $2; exit }'); test -n \"$ip\" && " +
                "printf 'GET / HTTP/1.0\\r\\nHost: " +
                "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | " +
                "timeout 90 nc \"$ip\" 80 >\"$l\" 2>&1; " +
                "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\"",
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runNetworkStage(
            "HTTP",
            command:
                "printf 'GET / HTTP/1.0\\r\\nHost: " +
                "dl-cdn.alpinelinux.org\\r\\nConnection: close\\r\\n\\r\\n' | " +
                "timeout 90 nc dl-cdn.alpinelinux.org 80 " +
                ">\"$l\" 2>&1; " +
                "grep -Eq '^HTTP/1\\.[01] [0-9]{3}' \"$l\"",
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runNetworkStage(
            "HTTPS",
            command:
                "a=/tmp/ish-apkindex.tar.gz; rm -f \"$a\"; " +
                "timeout -k 15 300 wget -q -T 60 -t 2 -O \"$a\" " +
                "https://dl-cdn.alpinelinux.org/alpine/v3.24/main/" +
                "aarch64/APKINDEX.tar.gz >\"$l\" 2>&1 && " +
                "test -s \"$a\" && " +
                "tar -tzf \"$a\" 2>>\"$l\" | grep -qx APKINDEX",
            timeout: 360,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runNetworkStage(
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
        runNetworkStage(
            "APK-UPDATE",
            command:
                "s=/tmp/ish-apk-search.log; rm -f \"$s\"; " +
                "timeout -k 15 900 apk update >\"$l\" 2>&1 && " +
                "apk search -x busybox >\"$s\" 2>>\"$l\" && " +
                "grep -Eq '^busybox-[0-9]' \"$s\"",
            timeout: 1020,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    private func runNetworkStage(
        _ stage: String,
        command: String,
        timeout: TimeInterval,
        app: XCUIApplication,
        input: XCUIElement,
        send: XCUIElement,
        terminal: XCUIElement
    ) {
        let token = String(UUID().uuidString.prefix(8))
        let pass = "ISH-NET:\(token):\(stage):PASS"
        let fail = "ISH-NET:\(token):\(stage):FAIL"
        let log = "/tmp/ish-net-\(stage.lowercased()).log"
        let script =
            "t=\(token); l=\(log); rm -f \"$l\"; if \(command); then " +
            "printf 'ISH-NET:%s:\(stage):PASS\\n' \"$t\"; else r=$?; " +
            "tail -c 4096 \"$l\" 2>/dev/null; " +
            "printf '\\nISH-NET:%s:\(stage):FAIL:%s\\n' \"$t\" \"$r\"; fi"

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
        XCTAssertTrue(
            systemInput.waitForExistence(timeout: 60),
            "\(stage) 的 watchOS 系统输入框没有出现")
        systemInput.typeText(script)

        let done = app.buttons.matching(
            NSPredicate(format: "label IN %@", ["Done", "完成"])).firstMatch
        XCTAssertTrue(
            done.waitForExistence(timeout: 10),
            "\(stage) 的 watchOS 系统输入界面没有完成按钮")
        done.tap()
        let sendReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: send)
        wait(for: [sendReady], timeout: 10)
        send.tap()

        let finished = expectation(
            for: NSPredicate(
                format: "label CONTAINS %@ OR label CONTAINS %@",
                pass,
                fail),
            evaluatedWith: terminal)
        wait(for: [finished], timeout: timeout)
        XCTAssertTrue(terminal.label.contains(pass), terminal.label)
    }
}

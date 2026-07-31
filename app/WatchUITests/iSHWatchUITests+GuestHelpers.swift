import CryptoKit
import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func commandInput(in app: XCUIApplication) -> XCUIElement {
        app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "command-input",
                "命令")
        ).firstMatch
    }

    func terminalTranscript(
            in app: XCUIApplication) -> XCUIElement {
        app.descendants(
            matching: .any
        ).matching(
            identifier: "terminal-transcript"
        ).firstMatch
    }

    func terminalOutput(_ terminal: XCUIElement) -> String {
        terminal.debugDescription
    }

    func terminalContains(
        _ candidate: String,
        in terminal: XCUIElement
    ) -> Bool {
        terminal.descendants(
            matching: .any
        ).matching(
            NSPredicate(
                format:
                    "identifier BEGINSWITH %@ AND label CONTAINS %@",
                "terminal-line-",
                candidate)
        ).firstMatch.exists
    }

    func waitForTerminalOutput(
        containing candidates: [String],
        timeout: TimeInterval,
        pollInterval: TimeInterval = 0.5,
        terminal: XCUIElement
    ) -> String {
        let deadline = Date().addingTimeInterval(timeout)
        while true {
            if let found = candidates.first(where: {
                terminalContains($0, in: terminal)
            }) {
                return found
            }
            let remaining = deadline.timeIntervalSinceNow
            guard remaining > 0 else { break }
            Thread.sleep(
                forTimeInterval: min(pollInterval, remaining))
        }
        return terminalOutput(terminal)
    }

    func commandUsingCDNIPv4(_ command: String) -> String {
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

    func typeGuestLine(
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

    func waitForTransportPass(
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

    func submitGuestLineResult(
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

        var activeInput = input
        let wasColdPresentation = !didWarmSystemInput
        activeInput.tap()
        if wasColdPresentation {
            // 首次冷启动期间不做 AX 查询，避免拖慢只有五秒展示期限的 Quickboard。
            Thread.sleep(forTimeInterval: 20)
            didWarmSystemInput = true
        }

        var systemInput = app.textViews.firstMatch
        var inputExists = systemInput.exists
        if !inputExists && wasColdPresentation {
            // Quickboard 冷启动若错过系统展示期限，同一宿主进程无法再次呈现输入页。
            // 此时命令尚未送入 guest，可安全重启一次，并在已预热的系统服务上重试。
            app.terminate()
            app.launch()

            activeInput = commandInput(in: app)
            let relaunchedInputExists =
                activeInput.waitForExistence(timeout: 180)
            XCTAssertTrue(
                relaunchedInputExists,
                "预热 Quickboard 后命令输入框没有重新出现")
            guard relaunchedInputExists else { return .timeout }

            let relaunchedInputReady = XCTNSPredicateExpectation(
                predicate: NSPredicate(
                    format: "enabled == true AND hittable == true"),
                object: activeInput)
            let relaunchedReadyResult = XCTWaiter.wait(
                for: [relaunchedInputReady],
                timeout: 180)
            XCTAssertEqual(
                relaunchedReadyResult,
                .completed,
                "预热 Quickboard 后命令输入框没有恢复可用")
            guard relaunchedReadyResult == .completed else {
                return .timeout
            }

            activeInput.tap()
            // 系统服务已启动，但仍要让远程输入层级完整挂回宿主 AX 树。
            Thread.sleep(forTimeInterval: 20)
            systemInput = app.textViews.firstMatch
            inputExists = systemInput.waitForExistence(timeout: 60)
        } else if !inputExists {
            inputExists = systemInput.waitForExistence(timeout: 60)
        }
        XCTAssertTrue(inputExists, "watchOS 系统输入框没有出现")
        guard inputExists else { return .timeout }
        typeGuestLine(line, into: systemInput)

        let done = app.buttons.matching(
            NSPredicate(format: "label IN %@", ["Done", "完成"])).firstMatch
        let doneExists = done.waitForExistence(timeout: 10)
        XCTAssertTrue(doneExists, "watchOS 系统输入界面没有完成按钮")
        guard doneExists else { return .timeout }
        done.tap()

        let inputDismissed =
            systemInput.waitForNonExistence(timeout: 60)
        XCTAssertTrue(
            inputDismissed,
            "watchOS 系统输入界面没有在完成后退出")
        guard inputDismissed else { return .timeout }

        // Quickboard 退场会替换远程层级，必须重新查询宿主控件。
        let activeSend = app.buttons["send-command"]
        let sendReady = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "hittable == true"),
            object: activeSend)
        let sendResult = XCTWaiter.wait(for: [sendReady], timeout: 60)
        XCTAssertEqual(sendResult, .completed, "发送按钮没有恢复可点击状态")
        guard sendResult == .completed else { return .timeout }
        activeSend.tap()

        let activeTerminal = terminalTranscript(in: app)
        let terminalExists =
            activeTerminal.waitForExistence(timeout: 10)
        XCTAssertTrue(terminalExists, "发送命令后终端输出区域没有出现")
        guard terminalExists else { return .timeout }
        return waitForTransportPass(
            pass,
            fail: fail,
            timeout: timeout,
            terminal: activeTerminal)
    }

    func submitGuestLine(
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

    func recoverGuestState(
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

    func submitGuestScript(
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

    func runGuestStage(
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

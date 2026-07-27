import XCTest

@MainActor
final class iSHWatchUITests: XCTestCase {
    private var didWarmSystemInput = false

    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func test许可证与源码无需等待Linux即可查看() throws {
        let app = XCUIApplication()
        app.launch()

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
        XCTAssertTrue(entry.exists, "关闭许可证与源码后没有返回终端")
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
                "timeout 90 getent hosts dl-cdn.alpinelinux.org " +
                ">\"$l\" 2>&1",
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
            "NSLOOKUP",
            command:
                "timeout 90 nslookup dl-cdn.alpinelinux.org " +
                ">\"$l\" 2>&1",
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        runGuestStage(
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
        runGuestStage(
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
        runGuestStage(
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

    func testAArch64SQLite持久化() throws {
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

        runGuestStage(
            "INSTALL",
            suite: "SQLITE",
            command:
                "if apk info -e sqlite >/dev/null 2>&1; then :; " +
                "else timeout -k 15 900 apk --cache-max-age 10080 " +
                "add --no-progress 'sqlite=3.53.2-r0' " +
                ">\"$l\" 2>&1; fi && " +
                "timeout -k 15 900 apk --cache-max-age 10080 " +
                "fix --no-progress ncurses-terminfo-base " +
                "libncursesw readline sqlite " +
                ">>\"$l\" 2>&1 && " +
                "sqlite3 --version >>\"$l\" 2>&1",
            timeout: 1020,
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

        runGuestStage(
            "INSTALL",
            suite: "PYTHON",
            command:
                "if apk info -e 'python3=3.14.5-r0' " +
                ">/dev/null 2>&1; then :; else " +
                "timeout -k 30 3600 apk --cache-max-age 10080 " +
                "add --no-progress 'python3=3.14.5-r0' >\"$l\" 2>&1; fi && " +
                "apk info -e 'python3=3.14.5-r0' >>\"$l\" 2>&1 && " +
                "timeout -k 30 900 python3 -I -c 'import platform, sys; " +
                "assert platform.machine() == \"aarch64\"; " +
                "assert sys.version_info[:3] == (3, 14, 5)' " +
                ">>\"$l\" 2>&1",
            timeout: 4680,
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
                ">\"$l\" 2>&1 && sync && rm -rf \"$d\"",
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

        runGuestStage(
            "INSTALL",
            suite: "BUILD",
            command:
                "if apk info -e 'build-base=0.5-r4' " +
                ">/dev/null 2>&1; then :; else " +
                "timeout -k 30 7200 apk --cache-max-age 10080 " +
                "add --no-progress 'build-base=0.5-r4' >\"$l\" 2>&1; fi && " +
                "apk info -e 'build-base=0.5-r4' >>\"$l\" 2>&1 && " +
                "command -v cc >>\"$l\" 2>&1 && " +
                "test \"$(cc -dumpmachine)\" = aarch64-alpine-linux-musl",
            timeout: 8280,
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

        runGuestStage(
            "INSTALL",
            suite: "DEV",
            command:
                "if apk info -e 'git=2.54.0-r0' >/dev/null 2>&1 && " +
                "apk info -e 'openssh-client-default=10.3_p1-r0' " +
                ">/dev/null 2>&1; then :; else " +
                "timeout -k 30 7200 apk --cache-max-age 10080 " +
                "add --no-progress 'git=2.54.0-r0' " +
                "'openssh-client-default=10.3_p1-r0' >\"$l\" 2>&1; fi && " +
                "apk info -e 'git=2.54.0-r0' >>\"$l\" 2>&1 && " +
                "apk info -e 'openssh-client-default=10.3_p1-r0' " +
                ">>\"$l\" 2>&1 && " +
                "test \"$(git --version)\" = 'git version 2.54.0' && " +
                "ssh -V 2>&1 | grep -F 'OpenSSH_10.3p1' >>\"$l\"",
            timeout: 8280,
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
        let token = String(UUID().uuidString.prefix(8))
        let pass = "ISH-\(suite):\(token):\(stage):PASS"
        let fail = "ISH-\(suite):\(token):\(stage):FAIL"
        let log = "/tmp/ish-\(suite.lowercased())-\(stage.lowercased()).log"
        let script =
            "t=\(token); l=\(log); rm -f \"$l\"; if \(command); then " +
            "printf 'ISH-\(suite):%s:\(stage):PASS\\n' \"$t\"; else r=$?; " +
            "tail -c 4096 \"$l\" 2>/dev/null; " +
            "printf '\\nISH-\(suite):%s:\(stage):FAIL:%s\\n' \"$t\" \"$r\"; fi"

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

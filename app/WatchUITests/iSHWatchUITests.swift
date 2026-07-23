import XCTest

@MainActor
final class iSHWatchUITests: XCTestCase {
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

        let tab = app.buttons["send-tab"]
        let escape = app.buttons["send-escape"]
        let controlC = app.buttons["send-control-c"]
        XCTAssertTrue(tab.waitForExistence(timeout: 10), "Tab 按钮没有出现")
        XCTAssertTrue(escape.waitForExistence(timeout: 10), "Esc 按钮没有出现")
        XCTAssertTrue(controlC.waitForExistence(timeout: 10), "Ctrl-C 按钮没有出现")
        XCTAssertTrue(tab.isHittable, "Tab 按钮不可点击")
        XCTAssertTrue(escape.isHittable, "Esc 按钮不可点击")
        XCTAssertTrue(controlC.isHittable, "Ctrl-C 按钮不可点击")

        input.tap()
        let completionInput = app.textViews.firstMatch
        XCTAssertTrue(
            completionInput.waitForExistence(timeout: 10),
            "补全命令的 watchOS 系统输入框没有出现")
        completionInput.typeText("/ro")
        XCTAssertTrue(
            done.waitForExistence(timeout: 10),
            "补全命令的 watchOS 系统输入界面没有完成按钮")
        done.tap()
        let tabReady = expectation(
            for: NSPredicate(format: "hittable == true"),
            evaluatedWith: tab)
        wait(for: [tabReady], timeout: 10)
        tab.tap()
        let completion = expectation(
            for: NSPredicate(format: "label CONTAINS %@", "/root/"),
            evaluatedWith: terminal)
        wait(for: [completion], timeout: 30)
        escape.tap()
        controlC.tap()
    }
}

import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func openSettings(in app: XCUIApplication) {
        let settings = app.buttons.matching(
            identifier: "watch-settings-button").firstMatch
        XCTAssertTrue(
            settings.waitForExistence(timeout: 30),
            "Watch 设置入口没有出现")
        settings.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-settings-view"].waitForExistence(timeout: 10),
            "Watch 设置页没有打开")
    }

    func reopenSettings(in app: XCUIApplication) {
        app.terminate()
        app.launch()
        openSettings(in: app)
    }

    func elementContains(
        _ element: XCUIElement,
        text: String
    ) -> Bool {
        if element.label.contains(text) {
            return true
        }
        return (element.value as? String)?.contains(text) == true
    }

    func navigateBack(
        button identifier: String,
        destination destinationIdentifier: String,
        in app: XCUIApplication
    ) {
        let back = app.buttons.matching(
            identifier: identifier
        ).firstMatch
        XCTAssertTrue(
            back.waitForExistence(timeout: 10),
            "导航页缺少稳定返回入口：\(identifier)")
        back.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                destinationIdentifier].waitForExistence(timeout: 10),
            "返回后没有到达目标页：\(destinationIdentifier)")
    }

    func waitUntil(
        timeout: TimeInterval,
        condition: () -> Bool
    ) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() {
            let remaining = deadline.timeIntervalSinceNow
            guard remaining > 0 else { return false }
            Thread.sleep(forTimeInterval: min(0.25, remaining))
        }
        return true
    }

    func openTerminalThemes(in app: XCUIApplication) {
        openSettings(in: app)
        openSettingsPage(
            "watch-display-settings-link",
            page: "watch-display-settings-view",
            in: app)
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-terminal-appearance-preview"].waitForExistence(
                    timeout: 10),
            "显示与外观页缺少终端主题预览")
        let themes = app.buttons["watch-terminal-themes-link"]
        scrollToElement(themes, in: app)
        XCTAssertTrue(themes.exists, "显示与外观页缺少终端主题入口")
        themes.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-terminal-themes-view"].waitForExistence(
                    timeout: 10),
            "终端主题页没有打开")
    }

    func reopenTerminalThemes(in app: XCUIApplication) {
        app.terminate()
        app.launch()
        openTerminalThemes(in: app)
    }

    func openWatchCustomTheme(
        named name: String,
        rows: XCUIElementQuery,
        in app: XCUIApplication
    ) {
        let theme = rows.matching(
            NSPredicate(format: "label CONTAINS %@", name)
        ).firstMatch
        scrollToElement(theme, in: app)
        XCTAssertTrue(
            theme.waitForExistence(timeout: 10),
            "找不到自定义主题：\(name)")
        theme.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-custom-theme-editor"].waitForExistence(timeout: 10),
            "自定义主题编辑页没有打开：\(name)")
    }

    func adaptiveThemeToggle(
        in app: XCUIApplication
    ) -> XCUIElement {
        app.switches["watch-custom-theme-adaptive-toggle"]
    }

    func openWatchThemePalette(
        _ identifier: String,
        in app: XCUIApplication
    ) {
        let palette = app.descendants(matching: .any)[identifier]
        scrollToElement(palette, in: app)
        XCTAssertTrue(
            palette.waitForExistence(timeout: 10),
            "主题编辑页缺少调色板入口：\(identifier)")
        palette.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-custom-theme-palette-editor"].waitForExistence(
                    timeout: 10),
            "调色板编辑页没有打开：\(identifier)")
    }

    func assertWatchThemeValue(
        _ expectedValue: String,
        identifier: String,
        message: String,
        in app: XCUIApplication
    ) {
        let field = app.descendants(matching: .any)[identifier]
        scrollToElement(field, in: app)
        XCTAssertTrue(
            field.waitForExistence(timeout: 10),
            "主题字段不存在：\(identifier)")
        let value = field.value as? String ?? ""
        XCTAssertTrue(
            field.label.contains(expectedValue) ||
                value.contains(expectedValue),
            message)
    }

    func submitWatchText(
        _ text: String,
        through field: XCUIElement,
        in app: XCUIApplication
    ) {
        scrollToElement(field, in: app)
        XCTAssertTrue(field.exists, "待编辑的 Watch 文本入口没有出现")
        field.tap()

        let systemInput = app.textViews.firstMatch
        if !didWarmSystemInput {
            Thread.sleep(forTimeInterval: 20)
            didWarmSystemInput = true
        } else {
            _ = systemInput.waitForExistence(timeout: 5)
        }
        if !systemInput.exists {
            field.tap()
        }
        XCTAssertTrue(
            systemInput.waitForExistence(timeout: 60),
            "watchOS 系统文本输入框没有出现")
        typeGuestLine(text, into: systemInput)

        let done = app.buttons.matching(
            NSPredicate(format: "label IN %@", ["Done", "完成"])).firstMatch
        XCTAssertTrue(
            done.waitForExistence(timeout: 10),
            "watchOS 系统文本输入界面没有完成按钮")
        done.tap()
    }

    func openSettingsPage(
        _ linkIdentifier: String,
        page pageIdentifier: String,
        in app: XCUIApplication
    ) {
        let link = app.buttons[linkIdentifier]
        scrollToElement(link, in: app)
        XCTAssertTrue(link.exists, "设置页缺少入口 \(linkIdentifier)")
        link.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                pageIdentifier].waitForExistence(timeout: 10),
            "设置子页没有打开：\(pageIdentifier)")
    }

    func scrollToElement(
        _ element: XCUIElement,
        in app: XCUIApplication
    ) {
        let searchPasses: [(CGFloat, CGFloat)] = [
            (0.72, 0.54),
            (0.28, 0.46),
        ]
        for (start, end) in searchPasses {
            for _ in 0..<18 {
                if element.waitForExistence(timeout: 0.75) {
                    let identifier = element.identifier
                    let requiresSafeFrame =
                        identifier.hasPrefix("watch-share-file-") ||
                        identifier.hasPrefix("watch-delete-file-")
                    let safeFrame = app.frame.insetBy(dx: 0, dy: 12)
                    if element.isHittable &&
                        (!requiresSafeFrame ||
                         safeFrame.contains(element.frame)) {
                        return
                    }

                    // Watch 列表一次滚动可能让目标从屏幕下方直接越过到导航栏后。
                    // 目标已经进入无障碍树时，按其位置反向微调，避免继续同向滚动。
                    let targetY = element.frame.midY
                    let screenY = app.frame.midY
                    let adjustment: (CGFloat, CGFloat) =
                        targetY < screenY ?
                        (0.42, 0.54) : (0.58, 0.46)
                    app.coordinate(
                        withNormalizedOffset:
                            CGVector(dx: 0.5, dy: adjustment.0)
                    ).press(
                        forDuration: 0.05,
                        thenDragTo: app.coordinate(
                            withNormalizedOffset:
                                CGVector(dx: 0.5, dy: adjustment.1)),
                        withVelocity: .slow,
                        thenHoldForDuration: 0.05)
                    Thread.sleep(forTimeInterval: 0.35)
                    continue
                }
                app.coordinate(
                    withNormalizedOffset: CGVector(dx: 0.5, dy: start)
                ).press(
                    forDuration: 0.05,
                    thenDragTo: app.coordinate(
                        withNormalizedOffset: CGVector(dx: 0.5, dy: end)),
                    withVelocity: .slow,
                    thenHoldForDuration: 0.05)
                Thread.sleep(forTimeInterval: 0.5)
            }
        }
    }

    func scrollToExistingElement(
        _ element: XCUIElement,
        in app: XCUIApplication
    ) {
        let scrollPasses: [(CGFloat, CGFloat)] = [
            (0.72, 0.54),
            (0.28, 0.46),
        ]
        for (start, end) in scrollPasses {
            for _ in 0..<15 {
                if element.waitForExistence(timeout: 0.75) {
                    return
                }
                app.coordinate(
                    withNormalizedOffset: CGVector(dx: 0.5, dy: start)
                ).press(
                    forDuration: 0.05,
                    thenDragTo: app.coordinate(
                        withNormalizedOffset: CGVector(dx: 0.5, dy: end)),
                    withVelocity: .slow,
                    thenHoldForDuration: 0.05)
                Thread.sleep(forTimeInterval: 0.5)
            }
        }
    }

    func waitForElementCount(
        _ query: XCUIElementQuery,
        atLeast expectedCount: Int,
        timeout: TimeInterval
    ) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while query.count < expectedCount {
            let remaining = deadline.timeIntervalSinceNow
            guard remaining > 0 else { return false }
            Thread.sleep(forTimeInterval: min(0.25, remaining))
        }
        return true
    }

    func waitForExactElementCount(
        _ query: XCUIElementQuery,
        expected expectedCount: Int,
        timeout: TimeInterval
    ) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while query.count != expectedCount {
            let remaining = deadline.timeIntervalSinceNow
            guard remaining > 0 else { return false }
            Thread.sleep(forTimeInterval: min(0.25, remaining))
        }
        return true
    }

}

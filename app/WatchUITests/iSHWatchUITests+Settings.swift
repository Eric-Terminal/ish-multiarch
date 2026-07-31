import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func test许可证与源码无需等待Linux即可查看() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        // watchOS 26 会把 toolbar button 暴露成同标识的嵌套按钮节点。
        let settings = app.buttons.matching(
            identifier: "watch-settings-button"
        ).matching(
            NSPredicate(format: "value CONTAINS %@", "软件仓库")
        ).firstMatch
        XCTAssertTrue(
            settings.waitForExistence(timeout: 10),
            "设置入口没有发布软件仓库迁移或错误语义")
        settings.tap()

        let settingsView = app.descendants(
            matching: .any)["watch-settings-view"]
        XCTAssertTrue(
            settingsView.waitForExistence(timeout: 10),
            "Watch 设置页没有打开")

        let entry = app.buttons["third-party-notices-button"]
        scrollToElement(entry, in: app)
        XCTAssertTrue(entry.exists, "许可证与源码入口没有出现")
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

    func test设置包含显示启动文件系统与运行信息() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        openSettings(in: app)
        openSettingsPage(
            "watch-display-settings-link",
            page: "watch-display-settings-view",
            in: app)
        let appearancePreview = app.descendants(matching: .any)[
            "watch-terminal-appearance-preview"]
        XCTAssertTrue(
            appearancePreview.waitForExistence(timeout: 10),
            "终端显示页缺少外观预览")

        let palette = app.buttons["watch-terminal-themes-link"]
        scrollToElement(palette, in: app)
        XCTAssertTrue(palette.exists, "终端显示页缺少终端主题设置")
        XCTAssertTrue(palette.isEnabled, "终端主题设置不可用")

        let font = app.descendants(matching: .any)[
            "watch-terminal-font-picker"]
        scrollToElement(font, in: app)
        XCTAssertTrue(font.exists, "终端显示页缺少字体设置")
        XCTAssertTrue(font.isEnabled, "终端字体设置不可用")

        let size = app.descendants(matching: .any)[
            "watch-terminal-size-picker"]
        scrollToElement(size, in: app)
        XCTAssertTrue(size.exists, "终端显示页缺少字号设置")
        XCTAssertTrue(size.isEnabled, "终端字号设置不可用")

        let cursorShape = app.descendants(matching: .any)[
            "watch-cursor-shape-picker"]
        scrollToElement(cursorShape, in: app)
        XCTAssertTrue(cursorShape.exists, "终端显示页缺少光标形状设置")
        XCTAssertTrue(cursorShape.isEnabled, "终端光标形状设置不可用")

        let cursorBlink = app.descendants(matching: .any)[
            "watch-cursor-blink-toggle"]
        scrollToElement(cursorBlink, in: app)
        XCTAssertTrue(cursorBlink.exists, "终端显示页缺少光标闪烁设置")
        XCTAssertTrue(cursorBlink.isEnabled, "终端光标闪烁设置不可用")

        let colorScheme = app.descendants(matching: .any)[
            "watch-color-scheme-picker"]
        scrollToElement(colorScheme, in: app)
        XCTAssertTrue(colorScheme.exists, "终端显示页缺少 App 配色设置")

        reopenSettings(in: app)
        let sessionSettings = app.buttons["watch-session-settings-link"]
        scrollToElement(sessionSettings, in: app)
        XCTAssertTrue(
            sessionSettings.exists,
            "Watch 设置缺少终端会话入口")
        openSettingsPage(
            "watch-startup-settings-link",
            page: "watch-startup-settings-view",
            in: app)
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-custom-hostname-toggle"].exists,
            "启动页缺少自定义主机名开关")
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-next-hostname-value"].exists,
            "启动页缺少下次启动主机名预览")
        let bootCommand = app.descendants(matching: .any)[
            "watch-boot-command-input"]
        scrollToExistingElement(bootCommand, in: app)
        XCTAssertTrue(bootCommand.exists, "启动页缺少 Linux 系统命令入口")
        let launchCommand = app.descendants(matching: .any)[
            "watch-launch-command-input"]
        scrollToExistingElement(launchCommand, in: app)
        XCTAssertTrue(launchCommand.exists, "启动页缺少启动命令入口")

        reopenSettings(in: app)
        openSettingsPage(
            "watch-filesystems-link",
            page: "watch-filesystems-view",
            in: app)
        let root = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-filesystem-")).firstMatch
        XCTAssertTrue(
            root.waitForExistence(timeout: 180),
            "文件系统页没有显示本次启动使用的 Linux 环境")
        let currentRootMarker = app.descendants(
            matching: .any
        ).matching(
            NSPredicate(
                format: "identifier IN %@",
                [
                    "watch-active-filesystem",
                    "watch-claimed-filesystem",
                ])
        ).firstMatch
        scrollToElement(currentRootMarker, in: app)
        XCTAssertTrue(
            currentRootMarker.exists,
            "文件系统页没有标记本次启动使用的环境")
        let createRoot = app.buttons["create-watch-filesystem"]
        scrollToElement(createRoot, in: app)
        XCTAssertTrue(createRoot.exists, "文件系统页缺少新建环境入口")

        reopenSettings(in: app)
        openSettingsPage(
            "watch-runtime-details-link",
            page: "watch-runtime-details-view",
            in: app)

        reopenSettings(in: app)
        openSettingsPage(
            "watch-about-link",
            page: "watch-about-view",
            in: app)
        XCTAssertTrue(
            app.descendants(matching: .any)["watch-about-guest"].exists,
            "关于页缺少 Guest 架构")
        let hostPointer = app.descendants(matching: .any)[
            "watch-about-host-pointer"]
        scrollToElement(hostPointer, in: app)
        XCTAssertTrue(hostPointer.exists, "关于页缺少宿主 ABI 信息")
        let source = app.descendants(matching: .any)[
            "watch-about-source-link"]
        scrollToElement(source, in: app)
        XCTAssertTrue(source.exists, "关于页缺少当前项目源码入口")
        let issues = app.descendants(matching: .any)[
            "watch-about-issues-link"]
        scrollToElement(issues, in: app)
        XCTAssertTrue(issues.exists, "关于页缺少当前项目问题反馈入口")
        let discord = app.descendants(matching: .any)[
            "watch-about-discord-link"]
        scrollToElement(discord, in: app)
        XCTAssertTrue(discord.exists, "关于页缺少 Discord 社区入口")
        let fediverse = app.descendants(matching: .any)[
            "watch-about-fediverse-link"]
        scrollToElement(fediverse, in: app)
        XCTAssertTrue(fediverse.exists, "关于页缺少 Fediverse 社区入口")
    }

    func testWatch平台桥接公开默认项并拒绝非法写入() throws {
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

        let send = app.buttons["send-command"]
        let terminal = terminalTranscript(in: app)
        XCTAssertTrue(
            send.waitForExistence(timeout: 10) &&
                terminal.waitForExistence(timeout: 10),
            "默认项检查所需的终端控件没有出现")

        let token = String(UUID().uuidString.prefix(8))
        let pass = "ISH-DEFAULTS:\(token):PASS"
        let fail = "ISH-DEFAULTS:\(token):FAIL"
        let names =
            "font_family font_size theme color_scheme cursor_style " +
            "blink_cursor launch_command boot_command hostname_override"
        let command =
            "d=/proc/ish/defaults; ok=1; set -- \"$d\"/*; " +
            "test \"$#\" -eq 9 || ok=0; for k in \(names); do " +
            "test -L \"$d/$k\" && test -r \"$d/$k\" && " +
            "cat \"$d/$k\" >/dev/null || ok=0; done; " +
            "before=$(cat \"$d/blink_cursor\") || ok=0; " +
            "printf '%s' \"$before\" >\"$d/blink_cursor\" " +
            "2>/dev/null || ok=0; " +
            "write_succeeded=0; " +
            "if printf '{' >\"$d/blink_cursor\" 2>/dev/null; then " +
            "write_succeeded=1; fi; " +
            "after=$(cat \"$d/blink_cursor\") || ok=0; " +
            "test \"$write_succeeded\" -eq 0 || ok=0; " +
            "test \"$after\" = \"$before\" || ok=0; " +
            "if test \"$ok\" -eq 1; then " +
            "printf 'ISH-DEFAULTS:%s:PASS\\n' '\(token)'; else " +
            "printf 'ISH-DEFAULTS:%s:FAIL\\n' '\(token)'; fi"
        XCTAssertTrue(
            submitGuestLine(
                command,
                pass: pass,
                fail: fail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "Watch /proc/ish/defaults 桥接行为不符合约定")
    }

}

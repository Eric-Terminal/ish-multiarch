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

    private final class RootArchiveCleanupState {
        let baselinePath: String
        let sourceArchiveName: String
        var originalRootIdentifier: String?
        var baselineRootIdentifiers: Set<String>?
        var importedRootIdentifier: String?
        var hostArchiveName: String?
        var importMayHaveChangedSelection = false
        var rootInventoryReconciled = false
        var originalSelectionRestored = false
        var importedRootDeleted = false
        var sourceArchiveDeleted = false
        var hostArchiveDeleted = false
        var guestArchiveRenamed = false
        var completed = false

        init(token: String) {
            baselinePath =
                "/tmp/.ish-root-export-\(token).before"
            sourceArchiveName =
                "ish-ui-root-\(token.lowercased())-source.tar.gz"
        }
    }

    private var didWarmSystemInput = false
    private var didRecoverGuestState = false
    private var guestRecoveryRequired = false
    private var recoveryApp: XCUIApplication?
    private var rootArchiveCleanupState: RootArchiveCleanupState?
    private var rootArchiveCleanupApp: XCUIApplication?

    override func setUpWithError() throws {
        continueAfterFailure = false
        didWarmSystemInput = false
        didRecoverGuestState = false
        guestRecoveryRequired = false
        recoveryApp = nil
        rootArchiveCleanupState = nil
        rootArchiveCleanupApp = nil
    }

    override func tearDownWithError() throws {
        defer {
            recoveryApp?.terminate()
            recoveryApp = nil
            rootArchiveCleanupState = nil
            rootArchiveCleanupApp = nil
        }

        if let state = rootArchiveCleanupState,
           let app = rootArchiveCleanupApp,
           !state.completed {
            bestEffortCleanupRootArchiveTest(state, app: app)
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

    func test当前文件系统可经共享归档恢复并离线导出后清理() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        var input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 180),
            "Linux 准备完成前命令输入框没有出现")
        var ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 180)

        var send = app.buttons["send-command"]
        var terminal = terminalTranscript(in: app)
        let token = String(UUID().uuidString.prefix(8))
        let cleanupState = RootArchiveCleanupState(token: token)
        rootArchiveCleanupState = cleanupState
        rootArchiveCleanupApp = app

        let baselinePass = "BL:\(token):OK"
        let baselineFail = "BL:\(token):ERR"
        let baseline =
            "b='\(cleanupState.baselinePath)'; ok=1; " +
            "rm -f \"$b\" && : >\"$b\" || ok=0; " +
            "if test \"$ok\" -eq 1; then " +
            "for f in /mnt/shared/ish-*.tar.gz; do " +
            "test -f \"$f\" || continue; " +
            "printf '%s\\n' \"${f##*/}\" >>\"$b\" || ok=0; done; fi; " +
            "if test \"$ok\" -eq 1; then " +
            "printf 'BL:%s:OK\\n' '\(token)'; else " +
            "printf 'BL:%s:ERR\\n' '\(token)'; fi"
        XCTAssertTrue(
            submitGuestLine(
                baseline,
                pass: baselinePass,
                fail: baselineFail,
                timeout: 60,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "无法记录导出前已有的共享归档")

        openSettings(in: app)
        openSettingsPage(
            "watch-filesystems-link",
            page: "watch-filesystems-view",
            in: app)
        let activeRootMarker =
            app.images["watch-active-filesystem"]
        scrollToElement(activeRootMarker, in: app)
        let currentRootMarker: XCUIElement
        if activeRootMarker.exists {
            currentRootMarker = activeRootMarker
        } else {
            let claimedRootMarker =
                app.images["watch-claimed-filesystem"]
            scrollToElement(claimedRootMarker, in: app)
            currentRootMarker = claimedRootMarker
        }
        XCTAssertTrue(
            currentRootMarker.waitForExistence(timeout: 180),
            "文件系统列表没有标出当前运行环境")
        let rootRows = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-filesystem-"))
        var currentRootRow: XCUIElement?
        let markerY = currentRootMarker.frame.midY
        for index in 0..<rootRows.count {
            let candidate = rootRows.element(boundBy: index)
            let rowFrame = candidate.frame
            if markerY >= rowFrame.minY &&
                    markerY <= rowFrame.maxY {
                currentRootRow = candidate
                break
            }
        }
        XCTAssertNotNil(
            currentRootRow,
            "无法把当前运行标记映射到文件系统导航行")
        let originalRootRow = try XCTUnwrap(
            currentRootRow,
            "无法取得当前运行环境的导航行")
        let originalRootIdentifier = originalRootRow.identifier
        XCTAssertTrue(
            originalRootIdentifier.hasPrefix("watch-filesystem-"),
            "当前运行环境没有稳定的文件系统 identifier")
        cleanupState.originalRootIdentifier =
            originalRootIdentifier
        let baselineRootIdentifiers = try XCTUnwrap(
            bestEffortRootInventory(
                containing: originalRootIdentifier,
                in: app,
                timeout: 180),
            "无法完整记录导入前的文件系统清单")
        XCTAssertTrue(
            baselineRootIdentifiers.contains(originalRootIdentifier),
            "导入前清单没有包含当前运行环境")
        cleanupState.baselineRootIdentifiers =
            baselineRootIdentifiers
        let refreshedOriginalRootRow =
            app.buttons[originalRootIdentifier]
        scrollToElement(refreshedOriginalRootRow, in: app)
        XCTAssertTrue(
            refreshedOriginalRootRow.waitForExistence(timeout: 10),
            "盘点文件系统后无法重新定位当前运行环境")
        refreshedOriginalRootRow.tap()

        let rootName = String(
            originalRootIdentifier.dropFirst(
                "watch-filesystem-".count))
        let detail = app.descendants(matching: .any)[
            "watch-filesystem-detail-\(rootName)"]
        XCTAssertTrue(
            detail.waitForExistence(timeout: 10),
            "点击当前运行标记后没有打开文件系统详情")

        let export = app.buttons[
            "export-current-watch-filesystem"]
        scrollToElement(export, in: app)
        XCTAssertTrue(
            export.exists && export.isEnabled,
            "当前运行环境缺少可用的导出操作")
        export.tap()

        let settingsDismissed = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: detail)
        wait(for: [settingsDismissed], timeout: 10)

        terminal = terminalTranscript(in: app)
        let progressMarker = "正在导出文件系统"
        let successMarker = "导出完成：/mnt/shared/"
        let failureMarker = "导出失败（状态"
        let startupMarker = waitForTerminalOutput(
            containing: [
                progressMarker,
                successMarker,
                failureMarker,
            ],
            timeout: 30,
            pollInterval: 1,
            terminal: terminal)
        XCTAssertTrue(
            [
                progressMarker,
                successMarker,
                failureMarker,
            ].contains(startupMarker),
            "导出终端在 30 秒内没有报告启动状态：" +
                terminalOutput(terminal))
        let completionMarker = waitForTerminalOutput(
            containing: [
                successMarker,
                failureMarker,
            ],
            timeout: 900,
            pollInterval: 2,
            terminal: terminal)
        XCTAssertEqual(
            completionMarker,
            successMarker,
            "guest tar 没有成功发布归档：" +
                terminalOutput(terminal))

        input = commandInput(in: app)
        XCTAssertTrue(
            input.waitForExistence(timeout: 30),
            "导出完成后没有保留可检查归档的登录 shell")
        ready = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: input)
        wait(for: [ready], timeout: 30)
        send = app.buttons["send-command"]

        let renamePass = "RN:\(token):OK"
        let renameFail = "RN:\(token):ERR"
        let rename =
            "b='\(cleanupState.baselinePath)'; " +
            "a='/mnt/shared/\(cleanupState.sourceArchiveName)'; " +
            "n=; c=0; " +
            "for f in /mnt/shared/ish-*.tar.gz; do " +
            "test -f \"$f\" || continue; x=${f##*/}; " +
            "if ! grep -Fqx \"$x\" \"$b\"; then n=$f; c=$((c+1)); fi; " +
            "done; if test \"$c\" -eq 1 && test ! -e \"$a\" && " +
            "mv \"$n\" \"$a\"; then " +
            "printf 'RN:%s:OK\\n' '\(token)'; else " +
            "printf 'RN:%s:ERR\\n' '\(token)'; fi"
        XCTAssertTrue(
            submitGuestLine(
                rename,
                pass: renamePass,
                fail: renameFail,
                timeout: 120,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "无法唯一识别并登记本次导出的归档")
        cleanupState.guestArchiveRenamed = true

        let verifyPass = "ISH-EXPORT:\(token):PASS"
        let verifyFail = "ISH-EXPORT:\(token):FAIL"
        let verify =
            "b='\(cleanupState.baselinePath)'; " +
            "a='/mnt/shared/\(cleanupState.sourceArchiveName)'; ok=1; " +
            "l=/tmp/ish-export-\(token).list; " +
            "rm -f \"$b\" || ok=0; " +
            "/bin/tar -tzf \"$a\" >\"$l\" 2>/dev/null || ok=0; " +
            "for d in dev proc mnt sys; do " +
            "grep -Eq \"^\\\\./$d/?$\" \"$l\" || ok=0; done; " +
            "if grep -Eq '^\\\\./proc/.+|^\\\\./dev/pts/.+|" +
            "^\\\\./mnt/shared/.+|^\\\\./sys/.+' \"$l\"; then ok=0; fi; " +
            "if test \"$ok\" -eq 1; then rm -f \"$l\" || ok=0; " +
            "else rm -f \"$l\"; fi; " +
            "if test \"$ok\" -eq 1; then " +
            "printf 'ISH-EXPORT:%s:PASS\\n' '\(token)'; else " +
            "printf 'ISH-EXPORT:%s:FAIL\\n' '\(token)'; fi"
        XCTAssertTrue(
            submitGuestLine(
                verify,
                pass: verifyPass,
                fail: verifyFail,
                // tar 列表会完整读取压缩流，无需再用 gzip 重复解压。
                timeout: 600,
                app: app,
                input: input,
                send: send,
                terminal: terminal),
            "导出归档未保留 mountpoint，或包含动态挂载内容")

        openSettings(in: app)
        openSettingsPage(
            "watch-filesystems-link",
            page: "watch-filesystems-view",
            in: app)
        let restore = app.buttons[
            "restore-watch-filesystem-from-shared"]
        scrollToElement(restore, in: app)
        XCTAssertTrue(
            restore.waitForExistence(timeout: 10) && restore.isEnabled,
            "文件系统页缺少可用的共享归档恢复入口")
        restore.tap()

        let importView = app.descendants(matching: .any)[
            "watch-root-archive-import-view"]
        XCTAssertTrue(
            importView.waitForExistence(timeout: 10),
            "共享归档恢复页没有打开")
        let refreshArchives = app.buttons.matching(
            identifier: "watch-refresh-root-archives"
        ).firstMatch
        XCTAssertTrue(
            refreshArchives.waitForExistence(timeout: 10) &&
                refreshArchives.isEnabled,
            "共享归档恢复页缺少可用的刷新入口")
        refreshArchives.tap()
        let importArchive = app.buttons[
            "import-watch-root-\(cleanupState.sourceArchiveName)"]
        scrollToElement(importArchive, in: app)
        XCTAssertTrue(
            importArchive.waitForExistence(timeout: 10) &&
                importArchive.isEnabled,
            "恢复页没有显示刚导出的固定名称归档")
        cleanupState.importMayHaveChangedSelection = true
        importArchive.tap()

        let importDismissed = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: importView)
        wait(for: [importDismissed], timeout: 600)
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-filesystems-view"].waitForExistence(timeout: 10),
            "归档恢复完成后没有返回文件系统列表")

        let rootsAfterImport = try XCTUnwrap(
            bestEffortRootInventory(
                containing: originalRootIdentifier,
                in: app,
                timeout: 180),
            "恢复归档后无法读取完整文件系统清单")
        XCTAssertTrue(
            baselineRootIdentifiers.isSubset(
                of: rootsAfterImport),
            "恢复归档意外移除了导入前已有的文件系统")
        let importedIdentifiers =
            rootsAfterImport.subtracting(
                baselineRootIdentifiers)
        XCTAssertEqual(
            importedIdentifiers.count,
            1,
            "恢复归档没有且仅生成一个新文件系统")
        let importedRootIdentifier = try XCTUnwrap(
            importedIdentifiers.first,
            "无法取得恢复后新增文件系统的稳定 identifier")
        let selectedImportedRow =
            app.buttons[importedRootIdentifier]
        scrollToElement(selectedImportedRow, in: app)
        XCTAssertTrue(
            selectedImportedRow.waitForExistence(timeout: 60) &&
                elementContains(
                    selectedImportedRow,
                    text: "下次启动"),
            "恢复后的唯一新增环境没有被选为下次启动环境")
        cleanupState.importedRootIdentifier =
            importedRootIdentifier
        cleanupState.rootInventoryReconciled = true
        XCTAssertNotEqual(
            importedRootIdentifier,
            originalRootIdentifier,
            "恢复归档错误复用了当前运行环境")

        let importedRootRow =
            app.buttons[importedRootIdentifier]
        scrollToElement(importedRootRow, in: app)
        XCTAssertTrue(
            importedRootRow.waitForExistence(timeout: 10),
            "文件系统列表找不到恢复后的环境")
        importedRootRow.tap()

        let importedDetail = app.descendants(
            matching: .any
        ).matching(
            identifier:
                importedRootIdentifier.replacingOccurrences(
                    of: "watch-filesystem-",
                    with: "watch-filesystem-detail-")
        ).firstMatch
        XCTAssertTrue(
            importedDetail.waitForExistence(timeout: 10),
            "恢复后的文件系统详情页没有打开")
        let importedCurrentState = importedDetail.descendants(
            matching: .any
        ).matching(
            identifier: "watch-filesystem-current-state"
        ).firstMatch
        XCTAssertTrue(
            importedCurrentState.waitForExistence(timeout: 10) &&
                elementContains(
                    importedCurrentState,
                    text: "未使用"),
            "恢复后的文件系统不是非活动状态")
        let importedNextState = importedDetail.descendants(
            matching: .any
        ).matching(
            identifier: "watch-filesystem-next-state"
        ).firstMatch
        XCTAssertTrue(
            importedNextState.waitForExistence(timeout: 10) &&
                elementContains(
                    importedNextState,
                    text: "已选择"),
            "恢复后的文件系统没有被选为下次启动环境")

        let hostExport = app.buttons[
            "export-current-watch-filesystem"]
        scrollToElement(hostExport, in: app)
        XCTAssertTrue(
            hostExport.waitForExistence(timeout: 10) &&
                hostExport.isEnabled,
            "非活动文件系统缺少宿主直接导出入口")
        let hostProgress = app.descendants(
            matching: .any
        ).matching(
            identifier: "watch-root-export-progress"
        ).firstMatch
        let hostProgressMessage = app.descendants(
            matching: .any
        ).matching(
            identifier: "watch-root-export-progress-message"
        ).firstMatch
        let cancelHostExport = app.buttons.matching(
            identifier: "cancel-watch-root-export"
        ).firstMatch
        let hostSuccess = app.descendants(matching: .any).matching(
            NSPredicate(
                format:
                    "identifier == %@ OR label BEGINSWITH %@",
                "watch-root-export-success-message",
                "导出完成：/mnt/shared/")
        ).firstMatch
        hostExport.tap()
        // 热缓存下小型 Root 可能在首个 AX 轮询前完成；此时成功提示就是
        // 完整反馈，不应为制造可取消窗口而给产品加入人工延迟。
        XCTAssertTrue(
            waitUntil(timeout: 10) {
                hostProgress.exists || hostSuccess.exists
            },
            "非活动文件系统导出既没有进度，也没有完成反馈")
        if !hostSuccess.exists {
            XCTAssertTrue(
                hostProgressMessage.waitForExistence(timeout: 10) ||
                    hostSuccess.exists,
                "进行中的非活动文件系统导出没有进度说明")
            if !hostSuccess.exists {
                scrollToElement(cancelHostExport, in: app)
                XCTAssertTrue(
                    (cancelHostExport.waitForExistence(timeout: 10) &&
                        cancelHostExport.isHittable &&
                        cancelHostExport.isEnabled) ||
                        hostSuccess.exists,
                    "进行中的非活动文件系统导出缺少可操作的取消入口")
            }
        }

        XCTAssertTrue(
            hostSuccess.waitForExistence(timeout: 600),
            "非活动文件系统宿主导出没有完成")
        let successPrefix = "导出完成：/mnt/shared/"
        let hostSuccessText =
            (hostSuccess.label.isEmpty ?
                hostSuccess.value as? String :
                hostSuccess.label) ?? ""
        if hostSuccessText.hasPrefix(successPrefix) {
            cleanupState.hostArchiveName =
                String(hostSuccessText.dropFirst(successPrefix.count))
        }
        let hostArchiveName = try XCTUnwrap(
            cleanupState.hostArchiveName,
            "宿主导出成功提示没有给出共享归档文件名")
        XCTAssertTrue(
            hostArchiveName.hasSuffix(".tar.gz") &&
                !hostArchiveName.contains("/"),
            "宿主导出给出了非法共享归档文件名：\(hostArchiveName)")

        let dismissHostSuccess = app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "dismiss-watch-root-export-success",
                "好")
        ).firstMatch
        XCTAssertTrue(
            dismissHostSuccess.waitForExistence(timeout: 10),
            "宿主导出成功提示缺少关闭操作")
        dismissHostSuccess.tap()

        navigateBack(
            button: "back-from-watch-filesystem-detail",
            destination: "watch-filesystems-view",
            in: app)
        let originalRow = app.buttons[originalRootIdentifier]
        scrollToElement(originalRow, in: app)
        XCTAssertTrue(
            originalRow.waitForExistence(timeout: 10),
            "导出后无法找到原运行环境")
        originalRow.tap()
        let originalDetail = app.descendants(
            matching: .any
        ).matching(
            identifier:
                originalRootIdentifier.replacingOccurrences(
                    of: "watch-filesystem-",
                    with: "watch-filesystem-detail-")
        ).firstMatch
        XCTAssertTrue(
            originalDetail.waitForExistence(timeout: 10),
            "原运行环境详情页没有重新打开")

        let restoreOriginalSelection = originalDetail.descendants(
            matching: .button
        ).matching(
            identifier: "select-watch-filesystem"
        ).firstMatch
        scrollToElement(restoreOriginalSelection, in: app)
        XCTAssertTrue(
            restoreOriginalSelection.waitForExistence(timeout: 10) &&
                restoreOriginalSelection.isEnabled,
            "原运行环境无法恢复为下次启动选择")
        restoreOriginalSelection.tap()
        let originalNextState = originalDetail.descendants(
            matching: .any
        ).matching(
            identifier: "watch-filesystem-next-state"
        ).firstMatch
        scrollToElement(originalNextState, in: app)
        let originalSelectionRestored = waitUntil(timeout: 10) {
            elementContains(
                originalNextState,
                text: "已选择")
        }
        cleanupState.originalSelectionRestored =
            originalSelectionRestored
        XCTAssertTrue(
            originalSelectionRestored,
            "原运行环境选择没有立即持久化")

        navigateBack(
            button: "back-from-watch-filesystem-detail",
            destination: "watch-filesystems-view",
            in: app)
        let importedRowForDeletion =
            app.buttons[importedRootIdentifier]
        scrollToElement(importedRowForDeletion, in: app)
        XCTAssertTrue(
            importedRowForDeletion.waitForExistence(timeout: 10),
            "恢复原选择后找不到待清理的导入环境")
        importedRowForDeletion.tap()

        let deleteImported = importedDetail.descendants(
            matching: .button
        ).matching(
            identifier: "delete-watch-filesystem"
        ).firstMatch
        scrollToElement(deleteImported, in: app)
        XCTAssertTrue(
            deleteImported.waitForExistence(timeout: 10) &&
                deleteImported.isEnabled,
            "导入环境在恢复原选择后仍不可删除")
        deleteImported.tap()
        let confirmDeleteImported = app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "confirm-delete-watch-filesystem",
                "永久删除")
        ).firstMatch
        XCTAssertTrue(
            confirmDeleteImported.waitForExistence(timeout: 10),
            "删除导入环境前没有二次确认")
        confirmDeleteImported.tap()

        let importedDetailDismissed = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: importedDetail)
        wait(for: [importedDetailDismissed], timeout: 300)
        let rootsAfterDeletion = try XCTUnwrap(
            bestEffortRootInventory(
                containing: originalRootIdentifier,
                in: app,
                timeout: 180),
            "删除导入环境后无法重新读取完整文件系统清单")
        cleanupState.importedRootDeleted =
            rootsAfterDeletion == baselineRootIdentifiers
        cleanupState.rootInventoryReconciled =
            cleanupState.importedRootDeleted
        XCTAssertEqual(
            rootsAfterDeletion,
            baselineRootIdentifiers,
            "删除导入环境后没有完整保留导入前的文件系统清单")

        navigateBack(
            button: "back-from-watch-filesystems",
            destination: "watch-settings-view",
            in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        deleteSharedFile(
            named: cleanupState.sourceArchiveName,
            in: app)
        cleanupState.sourceArchiveDeleted = true
        deleteSharedFile(
            named: hostArchiveName,
            in: app)
        cleanupState.hostArchiveDeleted = true
        cleanupState.completed = true
    }

    func test专注模式释放顶部空间且可恢复() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let dock = app.descendants(
            matching: .any)["watch-terminal-dock"]
        XCTAssertTrue(
            dock.waitForExistence(timeout: 30),
            "终端快捷栏没有出现")

        dock.swipeLeft()
        dock.swipeLeft()

        let focus = app.buttons["toggle-watch-terminal-focus"]
        XCTAssertTrue(
            focus.waitForExistence(timeout: 10),
            "应用操作页缺少专注模式入口")
        XCTAssertTrue(
            app.buttons["open-watch-settings-from-dock"].exists,
            "专注模式所在页面缺少设置入口")
        XCTAssertTrue(
            app.buttons["open-watch-sessions-from-dock"].exists,
            "专注模式所在页面缺少会话入口")
        XCTAssertTrue(
            app.buttons["share-watch-terminal-transcript"].exists,
            "专注模式所在页面缺少终端记录分享入口")

        let topSettings = app.buttons.matching(
            identifier: "watch-settings-button").firstMatch
        let topSessions = app.buttons.matching(
            identifier: "watch-sessions-button").firstMatch
        XCTAssertTrue(topSettings.exists, "进入专注模式前缺少顶部设置按钮")
        XCTAssertTrue(topSessions.exists, "进入专注模式前缺少顶部会话按钮")

        focus.tap()
        let settingsHidden = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: topSettings)
        let sessionsHidden = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: topSessions)
        wait(for: [settingsHidden, sessionsHidden], timeout: 10)
        XCTAssertTrue(
            focus.exists && focus.isHittable,
            "专注模式隐藏顶部按钮后无法从快捷栏退出")

        focus.tap()
        XCTAssertTrue(
            topSettings.waitForExistence(timeout: 10),
            "退出专注模式后顶部设置按钮没有恢复")
        XCTAssertTrue(
            topSessions.waitForExistence(timeout: 10),
            "退出专注模式后顶部会话按钮没有恢复")
    }

    func test自定义终端主题可编辑选用持久化并删除() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        openTerminalThemes(in: app)
        let customRows = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-custom-theme-"))
        let create = app.buttons["create-watch-custom-theme"]
        scrollToElement(create, in: app)
        XCTAssertTrue(create.exists, "终端主题页缺少新建自定义主题入口")
        let initialThemeCount = customRows.count
        create.tap()
        XCTAssertTrue(
            waitForElementCount(
                customRows,
                atLeast: initialThemeCount + 1,
                timeout: 10),
            "新建后自定义主题列表没有增加")
        customRows.element(boundBy: customRows.count - 1).tap()

        let editor = app.descendants(matching: .any)[
            "watch-custom-theme-editor"]
        XCTAssertTrue(
            editor.waitForExistence(timeout: 10),
            "自定义主题编辑页没有打开")

        let token = String(UUID().uuidString.prefix(8))
        let themeName = "手表主题-\(token)"
        submitWatchText(
            themeName,
            through: app.descendants(matching: .any)[
                "watch-custom-theme-name"],
            in: app)
        let initialAdaptiveToggle = adaptiveThemeToggle(in: app)
        scrollToElement(initialAdaptiveToggle, in: app)
        XCTAssertTrue(
            initialAdaptiveToggle.exists,
            "新建主题缺少浅色与深色配色开关")
        initialAdaptiveToggle.tap()
        let initialSinglePaletteConfirmation = app.buttons[
            "保留浅色主配色"]
        XCTAssertTrue(
            initialSinglePaletteConfirmation.waitForExistence(timeout: 10),
            "新建主题切到单套配色前没有显示保留规则")
        initialSinglePaletteConfirmation.tap()
        openWatchThemePalette(
            "watch-custom-theme-primary-palette",
            in: app)
        submitWatchText(
            "#DDEEFF",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-foreground"],
            in: app)
        submitWatchText(
            "#101820",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-background"],
            in: app)
        submitWatchText(
            "#FF3366",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-cursor"],
            in: app)

        let normalColors = app.buttons[
            "watch-custom-theme-ansi-normal"]
        scrollToElement(normalColors, in: app)
        XCTAssertTrue(normalColors.exists, "主题编辑页缺少 ANSI 标准色入口")
        let brightColors = app.buttons[
            "watch-custom-theme-ansi-bright"]
        scrollToElement(brightColors, in: app)
        XCTAssertTrue(brightColors.exists, "主题编辑页缺少 ANSI 高亮色入口")
        scrollToElement(normalColors, in: app)
        normalColors.tap()
        let firstANSIColor = app.descendants(matching: .any)[
            "watch-custom-theme-ansi-color-0"]
        XCTAssertTrue(
            firstANSIColor.waitForExistence(timeout: 10),
            "ANSI 标准色页缺少颜色 0")
        submitWatchText(
            "#224466",
            through: firstANSIColor,
            in: app)

        reopenTerminalThemes(in: app)
        openWatchCustomTheme(
            named: themeName,
            rows: customRows,
            in: app)
        openWatchThemePalette(
            "watch-custom-theme-primary-palette",
            in: app)
        for (identifier, expectedValue) in [
            ("watch-custom-theme-foreground", "#DDEEFF"),
            ("watch-custom-theme-background", "#101820"),
            ("watch-custom-theme-cursor", "#FF3366"),
        ] {
            assertWatchThemeValue(
                expectedValue,
                identifier: identifier,
                message:
                    "重启后主题颜色没有持久化：\(identifier)",
                in: app)
        }
        let persistedNormalColors = app.buttons[
            "watch-custom-theme-ansi-normal"]
        scrollToElement(persistedNormalColors, in: app)
        persistedNormalColors.tap()
        let persistedANSI = app.descendants(matching: .any)[
            "watch-custom-theme-ansi-color-0"]
        XCTAssertTrue(
            persistedANSI.waitForExistence(timeout: 10),
            "重启后无法检查 ANSI 标准色")
        assertWatchThemeValue(
            "#224466",
            identifier: "watch-custom-theme-ansi-color-0",
            message: "重启后 ANSI 标准色没有持久化",
            in: app)

        reopenTerminalThemes(in: app)
        openWatchCustomTheme(
            named: themeName,
            rows: customRows,
            in: app)
        let adaptiveToggle = adaptiveThemeToggle(in: app)
        scrollToElement(adaptiveToggle, in: app)
        XCTAssertTrue(
            adaptiveToggle.waitForExistence(timeout: 10),
            "自定义主题缺少双明暗配色开关")
        adaptiveToggle.tap()
        let lightPalette = app.descendants(matching: .any)[
            "watch-custom-theme-light-palette"]
        let darkPalette = app.descendants(matching: .any)[
            "watch-custom-theme-dark-palette"]
        scrollToExistingElement(lightPalette, in: app)
        XCTAssertTrue(
            lightPalette.exists,
            "开启双套后没有显示浅色调色板")
        scrollToExistingElement(darkPalette, in: app)
        XCTAssertTrue(
            darkPalette.exists,
            "开启双套后没有显示深色调色板")

        openWatchThemePalette(
            "watch-custom-theme-dark-palette",
            in: app)
        for (identifier, expectedValue) in [
            ("watch-custom-theme-foreground", "#DDEEFF"),
            ("watch-custom-theme-background", "#101820"),
            ("watch-custom-theme-cursor", "#FF3366"),
        ] {
            assertWatchThemeValue(
                expectedValue,
                identifier: identifier,
                message:
                    "首次开启双套时深色配色没有复制主配色：\(identifier)",
                in: app)
        }
        submitWatchText(
            "#99BBDD",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-foreground"],
            in: app)
        submitWatchText(
            "#020408",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-background"],
            in: app)
        submitWatchText(
            "#FFAA00",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-cursor"],
            in: app)

        let darkNormalColors = app.buttons[
            "watch-custom-theme-ansi-normal"]
        scrollToElement(darkNormalColors, in: app)
        darkNormalColors.tap()
        assertWatchThemeValue(
            "#224466",
            identifier: "watch-custom-theme-ansi-color-0",
            message: "首次开启双套时深色 ANSI 色没有复制主配色",
            in: app)
        submitWatchText(
            "#663399",
            through: app.descendants(matching: .any)[
                "watch-custom-theme-ansi-color-0"],
            in: app)

        reopenTerminalThemes(in: app)
        openWatchCustomTheme(
            named: themeName,
            rows: customRows,
            in: app)
        let persistedLightPalette = app.descendants(matching: .any)[
            "watch-custom-theme-light-palette"]
        scrollToExistingElement(persistedLightPalette, in: app)
        XCTAssertTrue(
            persistedLightPalette.exists,
            "重启后浅色配色没有持久化")
        let persistedDarkPalette = app.descendants(matching: .any)[
            "watch-custom-theme-dark-palette"]
        scrollToExistingElement(persistedDarkPalette, in: app)
        XCTAssertTrue(
            persistedDarkPalette.exists,
            "重启后深色配色没有持久化")
        openWatchThemePalette(
            "watch-custom-theme-dark-palette",
            in: app)
        for (identifier, expectedValue) in [
            ("watch-custom-theme-foreground", "#99BBDD"),
            ("watch-custom-theme-background", "#020408"),
            ("watch-custom-theme-cursor", "#FFAA00"),
        ] {
            assertWatchThemeValue(
                expectedValue,
                identifier: identifier,
                message:
                    "重启后深色配色没有持久化：\(identifier)",
                in: app)
        }
        let persistedDarkNormalColors = app.buttons[
            "watch-custom-theme-ansi-normal"]
        scrollToElement(persistedDarkNormalColors, in: app)
        persistedDarkNormalColors.tap()
        assertWatchThemeValue(
            "#663399",
            identifier: "watch-custom-theme-ansi-color-0",
            message: "重启后深色 ANSI 色没有持久化",
            in: app)

        reopenTerminalThemes(in: app)
        openWatchCustomTheme(
            named: themeName,
            rows: customRows,
            in: app)
        let persistedAdaptiveToggle = adaptiveThemeToggle(in: app)
        scrollToElement(persistedAdaptiveToggle, in: app)
        persistedAdaptiveToggle.tap()
        let confirmSinglePalette = app.buttons[
            "保留浅色主配色"]
        XCTAssertTrue(
            confirmSinglePalette.waitForExistence(timeout: 10),
            "切回单套配色前没有明确主配色保留规则")
        confirmSinglePalette.tap()
        let restoredPrimaryPalette = app.descendants(matching: .any)[
            "watch-custom-theme-primary-palette"]
        scrollToExistingElement(restoredPrimaryPalette, in: app)
        XCTAssertTrue(
            restoredPrimaryPalette.exists,
            "切回单套后没有恢复主配色入口")
        openWatchThemePalette(
            "watch-custom-theme-primary-palette",
            in: app)
        for (identifier, expectedValue) in [
            ("watch-custom-theme-foreground", "#DDEEFF"),
            ("watch-custom-theme-background", "#101820"),
            ("watch-custom-theme-cursor", "#FF3366"),
        ] {
            assertWatchThemeValue(
                expectedValue,
                identifier: identifier,
                message:
                    "切回单套后没有保留浅色主配色：\(identifier)",
                in: app)
        }

        reopenTerminalThemes(in: app)
        openWatchCustomTheme(
            named: themeName,
            rows: customRows,
            in: app)
        let singleToggle = adaptiveThemeToggle(in: app)
        scrollToElement(singleToggle, in: app)
        singleToggle.tap()
        openWatchThemePalette(
            "watch-custom-theme-dark-palette",
            in: app)
        for (identifier, expectedValue) in [
            ("watch-custom-theme-foreground", "#DDEEFF"),
            ("watch-custom-theme-background", "#101820"),
            ("watch-custom-theme-cursor", "#FF3366"),
        ] {
            assertWatchThemeValue(
                expectedValue,
                identifier: identifier,
                message:
                    "再次开启双套时没有重新复制主配色：\(identifier)",
                in: app)
        }
        let recopiedNormalColors = app.buttons[
            "watch-custom-theme-ansi-normal"]
        scrollToElement(recopiedNormalColors, in: app)
        recopiedNormalColors.tap()
        assertWatchThemeValue(
            "#224466",
            identifier: "watch-custom-theme-ansi-color-0",
            message: "再次开启双套时没有重新复制主配色 ANSI 色",
            in: app)

        reopenTerminalThemes(in: app)
        let builtIn = app.buttons[
            "watch-built-in-theme-default-dark"]
        XCTAssertTrue(
            builtIn.waitForExistence(timeout: 10),
            "终端主题页缺少默认深色主题")
        builtIn.tap()
        let useBuiltIn = app.buttons["watch-built-in-theme-use"]
        scrollToElement(useBuiltIn, in: app)
        XCTAssertTrue(useBuiltIn.exists, "内置主题详情缺少选用操作")
        let copyBuiltIn = app.buttons[
            "copy-watch-built-in-theme"]
        scrollToElement(copyBuiltIn, in: app)
        XCTAssertTrue(
            copyBuiltIn.exists && copyBuiltIn.isEnabled,
            "内置主题详情缺少复制为自定义主题操作")
        scrollToElement(useBuiltIn, in: app)
        useBuiltIn.tap()
        let builtInSelected = expectation(
            for: NSPredicate(format: "enabled == false"),
            evaluatedWith: useBuiltIn)
        wait(for: [builtInSelected], timeout: 10)

        reopenTerminalThemes(in: app)
        let editableTheme = customRows.matching(
            NSPredicate(format: "label CONTAINS %@", themeName)
        ).firstMatch
        scrollToElement(editableTheme, in: app)
        editableTheme.tap()
        let useCustom = app.buttons["watch-custom-theme-use"]
        scrollToElement(useCustom, in: app)
        XCTAssertTrue(
            useCustom.waitForExistence(timeout: 10),
            "自定义主题缺少选用操作")
        useCustom.tap()
        let selected = expectation(
            for: NSPredicate(format: "enabled == false"),
            evaluatedWith: useCustom)
        wait(for: [selected], timeout: 10)

        app.terminate()
        app.launch()
        XCTAssertTrue(
            terminalTranscript(in: app).waitForExistence(timeout: 180),
            "选用自定义主题后终端没有恢复")
        openTerminalThemes(in: app)
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-terminal-themes-view"].exists,
            "从终端重新进入时主题设置没有保留")
        let selectedTheme = customRows.matching(
            NSPredicate(format: "label CONTAINS %@", themeName)
        ).firstMatch
        scrollToElement(selectedTheme, in: app)
        selectedTheme.tap()

        let delete = app.buttons["watch-custom-theme-delete"]
        scrollToElement(delete, in: app)
        XCTAssertTrue(delete.exists, "自定义主题缺少删除操作")
        delete.tap()
        let confirmDelete = app.buttons.matching(
            NSPredicate(format: "label == %@", "永久删除")).firstMatch
        XCTAssertTrue(
            confirmDelete.waitForExistence(timeout: 10),
            "删除自定义主题前没有二次确认")
        confirmDelete.tap()
        XCTAssertTrue(
            waitForExactElementCount(
                customRows,
                expected: initialThemeCount,
                timeout: 10),
            "删除后自定义主题列表没有恢复")

        reopenTerminalThemes(in: app)
        let fallbackTheme = app.buttons[
            "watch-built-in-theme-default-dark"]
        XCTAssertTrue(
            fallbackTheme.waitForExistence(timeout: 10),
            "删除当前主题后默认主题不可用")
        fallbackTheme.tap()
        let fallbackUse = app.buttons["watch-built-in-theme-use"]
        scrollToElement(fallbackUse, in: app)
        XCTAssertTrue(fallbackUse.exists, "删除当前主题后无法检查回退主题")
        XCTAssertFalse(
            fallbackUse.isEnabled,
            "删除正在使用的自定义主题后没有回退默认深色")
    }

    func test主题外观覆盖可导出并从共享文件导回() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        openTerminalThemes(in: app)
        let customRows = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-custom-theme-"))
        let create = app.buttons["create-watch-custom-theme"]
        scrollToElement(create, in: app)
        XCTAssertTrue(create.exists, "主题页缺少新建入口")
        let initialThemeCount = customRows.count
        let initialThemeIdentifiers = Set(
            customRows.allElementsBoundByIndex.map(\.identifier))
        create.tap()
        XCTAssertTrue(
            waitForElementCount(
                customRows,
                atLeast: initialThemeCount + 1,
                timeout: 10),
            "新建主题后列表没有更新")
        let createdRows = customRows.allElementsBoundByIndex.filter {
            !initialThemeIdentifiers.contains($0.identifier)
        }
        XCTAssertEqual(createdRows.count, 1, "无法唯一识别本轮新建的主题")
        guard let createdRow = createdRows.first else {
            return
        }
        let createdThemeIdentifier = createdRow.identifier
        createdRow.tap()

        let token = String(UUID().uuidString.prefix(8))
        let themeName = "外观往返-\(token)"
        let fileName = "\(themeName).json"
        submitWatchText(
            themeName,
            through: app.descendants(matching: .any)[
                "watch-custom-theme-name"],
            in: app)

        for identifier in [
            "watch-custom-theme-light-override",
            "watch-custom-theme-dark-override",
        ] {
            let toggle = app.switches[identifier]
            scrollToElement(toggle, in: app)
            XCTAssertTrue(
                toggle.waitForExistence(timeout: 10),
                "主题编辑页缺少外观覆盖开关：\(identifier)")
            toggle.tap()
            let enabled = expectation(
                for: NSPredicate(format: "value == '1'"),
                evaluatedWith: toggle)
            wait(for: [enabled], timeout: 10)
        }

        let export = app.buttons["export-watch-custom-theme"]
        scrollToElement(export, in: app)
        XCTAssertTrue(export.exists, "自定义主题缺少共享导出入口")
        export.tap()
        XCTAssertTrue(
            app.staticTexts["主题已导出"].waitForExistence(timeout: 10),
            "主题导出成功后没有反馈")
        XCTAssertTrue(
            app.staticTexts.matching(
                NSPredicate(
                    format: "label CONTAINS %@",
                    "/mnt/shared/\(fileName)")
            ).firstMatch.exists,
            "主题导出反馈没有给出 Shared 文件路径")
        let acknowledge = app.buttons.matching(
            NSPredicate(format: "label == %@", "好")).firstMatch
        XCTAssertTrue(
            acknowledge.waitForExistence(timeout: 10),
            "主题导出反馈缺少关闭操作")
        acknowledge.tap()

        let delete = app.buttons["watch-custom-theme-delete"]
        scrollToElement(delete, in: app)
        delete.tap()
        let confirmDelete = app.buttons.matching(
            NSPredicate(format: "label == %@", "永久删除")).firstMatch
        XCTAssertTrue(
            confirmDelete.waitForExistence(timeout: 10),
            "导入前删除源主题没有二次确认")
        confirmDelete.tap()
        let sourceThemeRemoved = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: app.buttons[createdThemeIdentifier])
        wait(for: [sourceThemeRemoved], timeout: 10)

        let importTheme = app.buttons["import-watch-custom-theme"]
        scrollToElement(importTheme, in: app)
        XCTAssertTrue(importTheme.exists, "主题页缺少 Shared 导入入口")
        importTheme.tap()
        XCTAssertTrue(
            app.descendants(matching: .any)[
                "watch-theme-import-view"].waitForExistence(timeout: 10),
            "主题导入页没有打开")
        let exportedFile = app.buttons["watch-import-theme-\(fileName)"]
        scrollToElement(exportedFile, in: app)
        XCTAssertTrue(
            exportedFile.waitForExistence(timeout: 10),
            "主题导入页没有显示刚导出的 JSON")
        exportedFile.tap()
        XCTAssertTrue(
            app.staticTexts["主题已导入"].waitForExistence(timeout: 10),
            "主题导入成功后没有反馈")
        let finishImport = app.buttons["完成"]
        XCTAssertTrue(
            finishImport.waitForExistence(timeout: 10),
            "主题导入反馈缺少完成操作")
        finishImport.tap()

        XCTAssertTrue(
            waitForElementCount(
                customRows,
                atLeast: initialThemeCount + 1,
                timeout: 10),
            "导回主题后列表没有恢复")
        let importedThemeRow = customRows.matching(
            NSPredicate(format: "label CONTAINS %@", themeName)
        ).firstMatch
        scrollToElement(importedThemeRow, in: app)
        XCTAssertTrue(
            importedThemeRow.waitForExistence(timeout: 10),
            "找不到导回的主题：\(themeName)")
        let importedThemeIdentifier = importedThemeRow.identifier
        openWatchCustomTheme(
            named: themeName,
            rows: customRows,
            in: app)
        for identifier in [
            "watch-custom-theme-light-override",
            "watch-custom-theme-dark-override",
        ] {
            let toggle = app.switches[identifier]
            scrollToElement(toggle, in: app)
            XCTAssertEqual(
                toggle.value as? String,
                "1",
                "导回主题没有保留外观覆盖：\(identifier)")
        }

        let deleteImported = app.buttons["watch-custom-theme-delete"]
        scrollToElement(deleteImported, in: app)
        deleteImported.tap()
        XCTAssertTrue(
            confirmDelete.waitForExistence(timeout: 10),
            "删除导回主题没有二次确认")
        confirmDelete.tap()
        let importedThemeRemoved = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: app.buttons[importedThemeIdentifier])
        wait(for: [importedThemeRemoved], timeout: 10)

        reopenSettings(in: app)
        openSettingsPage(
            "watch-shared-files-link",
            page: "watch-shared-files-view",
            in: app)
        let sharedPage = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        let deleteFile = sharedPage.descendants(matching: .button)
            .matching(identifier: "watch-delete-file-\(fileName)")
            .firstMatch
        scrollToElement(deleteFile, in: app)
        XCTAssertTrue(
            deleteFile.waitForExistence(timeout: 10) &&
                deleteFile.isEnabled &&
                deleteFile.isHittable,
            "共享文件页没有显示主题导出文件")
        deleteFile.tap()
        // watchOS 26 的 confirmationDialog 有时只保留按钮文案。
        let confirmDeleteFile = app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "confirm-delete-watch-shared-file",
                "永久删除")
        ).firstMatch
        XCTAssertTrue(
            confirmDeleteFile.waitForExistence(timeout: 10),
            "删除主题导出文件前没有二次确认")
        confirmDeleteFile.tap()
        let fileRemoved = expectation(
            for: NSPredicate(format: "exists == false"),
            evaluatedWith: deleteFile)
        wait(for: [fileRemoved], timeout: 10)
    }

    func test多终端输出隔离且关闭后其余会话可继续() throws {
        let app = XCUIApplication()
        recoveryApp = app
        app.launch()

        let firstInput = commandInput(in: app)
        XCTAssertTrue(
            firstInput.waitForExistence(timeout: 180),
            "首个终端的命令输入框没有出现")
        let firstInputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: firstInput)
        wait(for: [firstInputReady], timeout: 180)

        let firstSend = app.buttons["send-command"]
        let firstTerminal = terminalTranscript(in: app)
        XCTAssertTrue(
            firstSend.waitForExistence(timeout: 10) &&
                firstTerminal.waitForExistence(timeout: 10),
            "首个终端的输入或输出控件没有出现")

        let token = String(UUID().uuidString.prefix(8))
        let firstMarker = "ISH-MULTI:\(token):FIRST"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(firstMarker)",
                pass: firstMarker,
                fail: "ISH-MULTI:\(token):FIRST:FAIL",
                timeout: 60,
                app: app,
                input: firstInput,
                send: firstSend,
                terminal: firstTerminal),
            "首个终端无法执行隔离检查命令")

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
            "终端会话页没有打开")
        let sessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-"))
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 1, timeout: 10),
            "终端会话页没有显示首个终端")
        let firstSessionIdentifier = sessions.firstMatch.identifier

        let create = app.buttons["create-watch-session"]
        scrollToElement(create, in: app)
        XCTAssertTrue(create.exists, "终端会话页缺少新建入口")
        XCTAssertTrue(create.isEnabled, "仍有空位时新建终端入口不可用")
        create.tap()

        let secondInput = commandInput(in: app)
        XCTAssertTrue(
            secondInput.waitForExistence(timeout: 180),
            "新建后第二个终端的命令输入框没有出现")
        let secondInputReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: secondInput)
        wait(for: [secondInputReady], timeout: 180)

        let secondSend = app.buttons["send-command"]
        let secondTerminal = terminalTranscript(in: app)
        let secondMarker = "ISH-MULTI:\(token):SECOND"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(secondMarker)",
                pass: secondMarker,
                fail: "ISH-MULTI:\(token):SECOND:FAIL",
                timeout: 60,
                app: app,
                input: secondInput,
                send: secondSend,
                terminal: secondTerminal),
            "第二个终端无法执行隔离检查命令")

        sessionsButton.tap()
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "第二次打开终端会话页失败")
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 2, timeout: 30),
            "新建后终端会话列表没有增加")

        let firstSession = app.buttons[firstSessionIdentifier]
        scrollToElement(firstSession, in: app)
        XCTAssertTrue(firstSession.exists, "无法在会话列表中找到首个终端")
        firstSession.tap()

        let restoredFirstTerminal = terminalTranscript(in: app)
        let restoredFirstOutput = waitForTerminalOutput(
            containing: [firstMarker],
            timeout: 30,
            terminal: restoredFirstTerminal)
        XCTAssertTrue(
            restoredFirstOutput.contains(firstMarker),
            "切回首个终端后没有恢复它自己的输出：\(restoredFirstOutput)")
        XCTAssertFalse(
            restoredFirstOutput.contains(secondMarker),
            "第二个终端的输出泄漏到了首个终端")

        sessionsButton.tap()
        XCTAssertTrue(
            sessionsView.waitForExistence(timeout: 10),
            "关闭终端前无法重新打开会话页")
        let selectedFirstSession = app.buttons[firstSessionIdentifier]
        scrollToElement(selectedFirstSession, in: app)
        XCTAssertTrue(
            selectedFirstSession.isHittable,
            "首个终端行无法执行关闭手势")
        selectedFirstSession.swipeLeft()

        let closeAction = app.buttons.matching(
            NSPredicate(format: "label == %@", "关闭")).firstMatch
        XCTAssertTrue(
            closeAction.waitForExistence(timeout: 10),
            "终端行没有显示关闭操作")
        closeAction.tap()
        let confirmClose = app.buttons.matching(
            NSPredicate(format: "label == %@", "关闭终端")).firstMatch
        XCTAssertTrue(
            confirmClose.waitForExistence(timeout: 10),
            "关闭终端确认按钮没有出现")
        confirmClose.tap()
        XCTAssertTrue(
            waitForExactElementCount(sessions, expected: 1, timeout: 30),
            "关闭首个终端后其余终端也消失了")

        sessions.firstMatch.tap()
        let remainingInput = commandInput(in: app)
        XCTAssertTrue(
            remainingInput.waitForExistence(timeout: 30),
            "关闭首个终端后其余终端没有恢复输入")
        let remainingReady = expectation(
            for: NSPredicate(format: "enabled == true"),
            evaluatedWith: remainingInput)
        wait(for: [remainingReady], timeout: 30)
        let remainingTerminal = terminalTranscript(in: app)
        let continueMarker = "ISH-MULTI:\(token):CONTINUE"
        XCTAssertTrue(
            submitGuestLine(
                "echo \(continueMarker)",
                pass: continueMarker,
                fail: "ISH-MULTI:\(token):CONTINUE:FAIL",
                timeout: 60,
                app: app,
                input: remainingInput,
                send: app.buttons["send-command"],
                terminal: remainingTerminal),
            "关闭首个终端后第二个终端无法继续执行命令")
        XCTAssertTrue(
            terminalContains(secondMarker, in: remainingTerminal),
            "关闭其他终端后第二个终端丢失了自己的输出历史")
    }

    func test清除滚动历史保留当前屏幕且终端可继续() throws {
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
            "滚动历史测试所需的输入或输出控件没有出现")

        let token = String(UUID().uuidString.prefix(8))
        // 标记必须短于最窄 Watch 终端的一行，否则合法自动换行会让
        // “命令已完成”探针误判为超时。
        let visibleMarker = "SB:\(token):V"
        let generated = submitGuestLine(
            "clear; t=\(token); i=1; while test \"$i\" -le 96; do " +
                "printf 'ISH-SCROLLBACK:%s:%03d\\n' \"$t\" \"$i\"; " +
                "i=$((i+1)); done; " +
                "printf 'SB:%s:V\\n' \"$t\"",
            pass: visibleMarker,
            fail: "SB:\(token):GF",
            timeout: 60,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
        XCTAssertTrue(generated, "真实终端没有生成滚动历史")

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
            "终端会话页没有打开")
        let sessions = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-session-"))
        XCTAssertTrue(
            waitForElementCount(sessions, atLeast: 1, timeout: 10),
            "终端会话页没有显示当前终端")
        let currentSession = sessions.matching(
            NSPredicate(
                format: "value CONTAINS %@",
                "当前终端")
        ).firstMatch
        XCTAssertTrue(
            currentSession.waitForExistence(timeout: 10),
            "终端会话页没有标记当前终端")
        let currentSessionIdentifier = currentSession.identifier
        scrollToElement(currentSession, in: app)
        XCTAssertTrue(
            currentSession.isHittable,
            "当前终端行无法执行历史管理手势")
        currentSession.swipeLeft()

        let clearHistory = app.buttons.matching(
            NSPredicate(format: "label == %@", "清历史")
        ).firstMatch
        XCTAssertTrue(
            clearHistory.waitForExistence(timeout: 10),
            "当前终端左滑后没有显示清历史操作")
        XCTAssertTrue(
            clearHistory.isEnabled,
            "真实终端生成内容后仍没有可清除的滚动历史")
        clearHistory.tap()

        // watchOS 26 的 confirmationDialog 不保留子视图标识，
        // 因此按用户实际可见的清除边界说明验证。
        let clearWarning = app.staticTexts.matching(
            NSPredicate(
                format: "label CONTAINS %@",
                "已滚出当前屏幕的内容")
        ).firstMatch
        XCTAssertTrue(
            clearWarning.waitForExistence(timeout: 10),
            "清除滚动历史前没有说明清除范围")
        XCTAssertTrue(
            clearWarning.label.contains("当前屏幕") &&
                clearWarning.label.contains("都会保留"),
            "确认说明没有明确保留当前屏幕：\(clearWarning.label)")

        let confirmClear = app.buttons.matching(
            NSPredicate(
                format: "label == %@",
                "清除滚动历史")
        ).firstMatch
        XCTAssertTrue(
            confirmClear.waitForExistence(timeout: 10),
            "清除滚动历史的二次确认按钮没有出现")
        confirmClear.tap()

        let feedback = app.descendants(matching: .any).matching(
            NSPredicate(
                format: "identifier == %@ OR label CONTAINS %@",
                "watch-scrollback-clear-feedback",
                "已清除")
        ).firstMatch
        XCTAssertTrue(
            feedback.waitForExistence(timeout: 10),
            "确认后没有反馈滚动历史清除结果")
        XCTAssertNotNil(
            feedback.label.range(
                of: "已清除 [1-9][0-9]* 行滚动历史",
                options: .regularExpression),
            "清除反馈没有报告正数行数：\(feedback.label)")

        let restoredSession = app.buttons[currentSessionIdentifier]
        scrollToElement(restoredSession, in: app)
        XCTAssertTrue(
            restoredSession.waitForExistence(timeout: 10),
            "清除历史后当前终端行消失")
        restoredSession.tap()

        let restoredTerminal = terminalTranscript(in: app)
        let retainedOutput = waitForTerminalOutput(
            containing: [visibleMarker],
            timeout: 30,
            terminal: restoredTerminal)
        XCTAssertTrue(
            retainedOutput.contains(visibleMarker),
            "清除滚动历史误删了当前屏幕：\(retainedOutput)")

        let continueMarker = "SB:\(token):C"
        XCTAssertTrue(
            submitGuestLine(
                "t=\(token); " +
                    "printf 'SB:%s:C\\n' \"$t\"",
                pass: continueMarker,
                fail: "SB:\(token):CF",
                timeout: 60,
                app: app,
                input: commandInput(in: app),
                send: app.buttons["send-command"],
                terminal: restoredTerminal),
            "清除滚动历史后 shell 无法继续执行命令")
    }

    func test完整Ctrl面板覆盖主终端控制键() throws {
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

        input.swipeRight()
        let more = app.buttons["show-more-terminal-keys"]
        XCTAssertTrue(
            more.waitForExistence(timeout: 10),
            "常用快捷键页缺少更多入口")
        more.tap()

        let keypad = app.descendants(matching: .any)[
            "watch-terminal-keypad"]
        XCTAssertTrue(
            keypad.waitForExistence(timeout: 10),
            "完整快捷键页没有打开")
        let completeControl = app.buttons["show-all-control-keys"]
        scrollToElement(completeControl, in: app)
        XCTAssertTrue(
            completeControl.exists,
            "快捷键页缺少完整 Ctrl 输入入口")
        completeControl.tap()

        let controlView = app.descendants(matching: .any)[
            "watch-control-input-view"]
        XCTAssertTrue(
            controlView.waitForExistence(timeout: 10),
            "完整 Ctrl 输入面板没有打开")

        let firstLetter = app.buttons["send-full-control-a"]
        XCTAssertTrue(
            firstLetter.waitForExistence(timeout: 10),
            "Ctrl 面板缺少字母 A")
        let lastLetter = app.buttons["send-full-control-z"]
        scrollToElement(lastLetter, in: app)
        XCTAssertTrue(lastLetter.exists, "Ctrl 面板缺少字母 Z")

        for identifier in [
            "send-full-control-at",
            "send-full-control-caret",
            "send-full-control-2",
            "send-full-control-6",
            "send-full-control-minus",
            "send-full-control-left-bracket",
            "send-full-control-right-bracket",
            "send-full-control-backslash",
            "send-full-control-space",
        ] {
            let key = app.buttons[identifier]
            scrollToElement(key, in: app)
            XCTAssertTrue(
                key.exists && key.isEnabled,
                "Ctrl 面板缺少可用按键：\(identifier)")
        }
    }

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

        let earlierDragStart = terminal.coordinate(
            withNormalizedOffset: CGVector(dx: 0.5, dy: 0.35))
        let earlierDragEnd = terminal.coordinate(
            withNormalizedOffset: CGVector(dx: 0.5, dy: 0.85))
        var foundEarlierHistory = terminalContains(
            firstHistoryLine,
            in: terminal)
        for _ in 0..<16 where
                !foundEarlierHistory {
            earlierDragStart.press(
                forDuration: 0.1,
                thenDragTo: earlierDragEnd)
            foundEarlierHistory = terminalContains(
                firstHistoryLine,
                in: terminal)
        }
        XCTAssertTrue(
            foundEarlierHistory,
            terminalOutput(terminal))

        let followLatest = app.buttons["follow-latest-output"]
        XCTAssertTrue(
            followLatest.waitForExistence(timeout: 10),
            "手动查看历史后没有出现回到最新输出入口")
        followLatest.tap()
        let returnedOutput = waitForTerminalOutput(
            containing: [lastHistoryLine],
            timeout: 10,
            terminal: terminal)
        XCTAssertTrue(
            returnedOutput.contains(lastHistoryLine),
            "回到最新输出后没有重新显示末行：\(returnedOutput)")
        XCTAssertTrue(input.isHittable, "命令输入胶囊不应随终端历史滚走")

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

    private func openSettings(in app: XCUIApplication) {
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

    private func reopenSettings(in app: XCUIApplication) {
        app.terminate()
        app.launch()
        openSettings(in: app)
    }

    private func elementContains(
        _ element: XCUIElement,
        text: String
    ) -> Bool {
        if element.label.contains(text) {
            return true
        }
        return (element.value as? String)?.contains(text) == true
    }

    private func navigateBack(
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

    private func deleteSharedFile(
        named fileName: String,
        in app: XCUIApplication
    ) {
        let sharedPage = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        let delete = sharedPage.descendants(matching: .button)
            .matching(identifier: "watch-delete-file-\(fileName)")
            .firstMatch
        scrollToElement(delete, in: app)
        XCTAssertTrue(
            delete.waitForExistence(timeout: 10) &&
                delete.isEnabled &&
                delete.isHittable,
            "共享文件页没有待清理归档：\(fileName)")
        delete.tap()

        // watchOS 26 有时只保留 confirmationDialog 的按钮文案。
        let confirm = app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "confirm-delete-watch-shared-file",
                "永久删除")
        ).firstMatch
        XCTAssertTrue(
            confirm.waitForExistence(timeout: 10),
            "共享归档删除确认没有出现：\(fileName)")
        confirm.tap()

        let removed = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "exists == false"),
            object: delete)
        XCTAssertEqual(
            XCTWaiter.wait(for: [removed], timeout: 30),
            .completed,
            "共享归档没有被删除：\(fileName)")
    }

    private func bestEffortCleanupRootArchiveTest(
        _ state: RootArchiveCleanupState,
        app: XCUIApplication
    ) {
        guard !state.completed else { return }

        if !state.guestArchiveRenamed {
            // 导入尚未开始，重启仍只会 claim 原 Root。若导出在改为测试
            // 固定名之前中断，无法证明新增归档一定属于本用例，宁可留下它。
            bestEffortCleanupGuestArchiveBaseline(state, app: app)
            return
        }

        guard bestEffortEnsureRootArchiveAppRunning(app) else {
            return
        }
        captureAndDismissRootArchiveResult(state, app: app)
        cancelRootArchiveOperationIfNeeded(in: app)
        captureAndDismissRootArchiveResult(state, app: app)

        if state.importMayHaveChangedSelection &&
                !state.originalSelectionRestored {
            bestEffortRestoreOriginalRootSelection(
                state,
                app: app)
        }
        if state.originalSelectionRestored &&
                !state.importedRootDeleted {
            bestEffortDeleteImportedRoot(state, app: app)
            if !state.importedRootDeleted {
                bestEffortDeleteImportedRoot(state, app: app)
            }
        }

        guard !state.importMayHaveChangedSelection ||
                state.originalSelectionRestored else {
            return
        }
        guard state.importedRootIdentifier == nil ||
                state.importedRootDeleted else {
            return
        }
        guard !state.importMayHaveChangedSelection ||
                state.rootInventoryReconciled else {
            return
        }
        guard bestEffortOpenSharedFiles(in: app) else {
            return
        }
        if !state.sourceArchiveDeleted {
            state.sourceArchiveDeleted =
                bestEffortDeleteSharedFile(
                    named: state.sourceArchiveName,
                    in: app)
        }
        if !state.hostArchiveDeleted,
           let hostArchiveName = state.hostArchiveName {
            state.hostArchiveDeleted =
                bestEffortDeleteSharedFile(
                    named: hostArchiveName,
                    in: app)
        }
    }

    private func bestEffortEnsureRootArchiveAppRunning(
        _ app: XCUIApplication
    ) -> Bool {
        switch app.state {
        case .runningForeground:
            return true
        case .runningBackground, .runningBackgroundSuspended:
            app.activate()
        case .notRunning, .unknown:
            app.launch()
            didWarmSystemInput = false
        @unknown default:
            app.launch()
            didWarmSystemInput = false
        }
        return waitUntil(timeout: 30) {
            app.state == .runningForeground
        }
    }

    private func captureAndDismissRootArchiveResult(
        _ state: RootArchiveCleanupState,
        app: XCUIApplication
    ) {
        let success = app.descendants(matching: .any).matching(
            NSPredicate(
                format:
                    "identifier == %@ OR label BEGINSWITH %@",
                "watch-root-export-success-message",
                "导出完成：/mnt/shared/")
        ).firstMatch
        if success.waitForExistence(timeout: 1) {
            let text =
                (success.label.isEmpty ?
                    success.value as? String :
                    success.label) ?? ""
            let prefix = "导出完成：/mnt/shared/"
            if text.hasPrefix(prefix) {
                state.hostArchiveName =
                    String(text.dropFirst(prefix.count))
            }
            let dismiss = app.buttons.matching(
                NSPredicate(
                    format: "identifier == %@ OR label == %@",
                    "dismiss-watch-root-export-success",
                    "好")
            ).firstMatch
            if dismiss.waitForExistence(timeout: 1) {
                dismiss.tap()
            }
        }

        let operationError = app.staticTexts.matching(
            NSPredicate(
                format:
                    "label == %@ OR label == %@",
                "文件系统操作失败",
                "无法读取归档")
        ).firstMatch
        if operationError.exists {
            let acknowledge = app.buttons.matching(
                NSPredicate(format: "label == %@", "好")
            ).firstMatch
            if acknowledge.exists {
                acknowledge.tap()
            }
        }
    }

    private func cancelRootArchiveOperationIfNeeded(
        in app: XCUIApplication
    ) {
        for identifier in [
            "cancel-watch-root-export",
            "cancel-watch-root-import",
        ] {
            let cancel = app.buttons[identifier]
            if cancel.waitForExistence(timeout: 2) && cancel.isEnabled {
                cancel.tap()
                _ = waitUntil(timeout: 60) {
                    !cancel.exists
                }
            }
        }
    }

    private func bestEffortRestoreOriginalRootSelection(
        _ state: RootArchiveCleanupState,
        app: XCUIApplication
    ) {
        guard let originalIdentifier =
                state.originalRootIdentifier,
              bestEffortOpenFileSystems(in: app) else {
            return
        }

        guard let currentIdentifiers =
                bestEffortRootInventory(
                    containing: originalIdentifier,
                    in: app,
                    timeout: 180) else {
            return
        }
        if let baseline = state.baselineRootIdentifiers,
           baseline.isSubset(of: currentIdentifiers) {
            let added = currentIdentifiers.subtracting(baseline)
            if let importedIdentifier =
                    state.importedRootIdentifier {
                state.rootInventoryReconciled =
                    added == Set([importedIdentifier])
            } else if added.count == 1 {
                state.importedRootIdentifier = added.first
                state.rootInventoryReconciled = true
            } else if added.isEmpty {
                state.rootInventoryReconciled = true
            }
        }

        let original = app.buttons[originalIdentifier]
        scrollToElement(original, in: app)
        guard original.exists && original.isHittable else {
            return
        }
        original.tap()

        let detailIdentifier =
            originalIdentifier.replacingOccurrences(
                of: "watch-filesystem-",
                with: "watch-filesystem-detail-")
        let detail = app.descendants(matching: .any).matching(
            identifier: detailIdentifier
        ).firstMatch
        guard detail.waitForExistence(timeout: 5) else {
            return
        }
        let nextState = detail.descendants(
            matching: .any
        ).matching(
            identifier: "watch-filesystem-next-state"
        ).firstMatch
        guard nextState.waitForExistence(timeout: 5) else {
            return
        }
        if !elementContains(nextState, text: "已选择") {
            let select = detail.descendants(
                matching: .button
            ).matching(
                identifier: "select-watch-filesystem"
            ).firstMatch
            scrollToElement(select, in: app)
            guard waitUntil(timeout: 60, condition: {
                select.exists && select.isEnabled
            }) else {
                return
            }
            select.tap()
            scrollToElement(nextState, in: app)
        }
        state.originalSelectionRestored = waitUntil(timeout: 10) {
            nextState.exists &&
                elementContains(nextState, text: "已选择")
        }

        let back = app.buttons.matching(
            identifier: "back-from-watch-filesystem-detail"
        ).firstMatch
        if back.exists {
            back.tap()
            _ = app.descendants(matching: .any)[
                "watch-filesystems-view"].waitForExistence(timeout: 5)
        }
    }

    private func bestEffortDeleteImportedRoot(
        _ state: RootArchiveCleanupState,
        app: XCUIApplication
    ) {
        guard let importedIdentifier =
                state.importedRootIdentifier,
              let baselineIdentifiers =
                state.baselineRootIdentifiers,
              state.rootInventoryReconciled else {
            return
        }

        for attempt in 0..<2 {
            guard bestEffortOpenFileSystems(in: app) else {
                return
            }
            guard let originalIdentifier =
                    state.originalRootIdentifier,
                  let currentIdentifiers =
                    bestEffortRootInventory(
                        containing: originalIdentifier,
                        in: app,
                        timeout: 180) else {
                return
            }
            if !currentIdentifiers.contains(importedIdentifier) {
                state.importedRootDeleted =
                    currentIdentifiers == baselineIdentifiers
                state.rootInventoryReconciled =
                    state.importedRootDeleted
                return
            }
            guard currentIdentifiers ==
                    baselineIdentifiers.union(
                        [importedIdentifier]) else {
                state.rootInventoryReconciled = false
                return
            }
            let imported = app.buttons[importedIdentifier]
            scrollToElement(imported, in: app)
            guard imported.isHittable else { return }
            imported.tap()

            let importedDetailIdentifier =
                importedIdentifier.replacingOccurrences(
                    of: "watch-filesystem-",
                    with: "watch-filesystem-detail-")
            let importedDetail = app.descendants(
                matching: .any
            ).matching(
                identifier: importedDetailIdentifier
            ).firstMatch
            guard importedDetail.waitForExistence(timeout: 5) else {
                return
            }
            let delete = importedDetail.descendants(
                matching: .button
            ).matching(
                identifier: "delete-watch-filesystem"
            ).firstMatch
            scrollToElement(delete, in: app)
            guard delete.waitForExistence(timeout: 5) else {
                return
            }
            if !delete.isEnabled {
                let back = app.buttons.matching(
                    identifier: "back-from-watch-filesystem-detail"
                ).firstMatch
                if back.exists {
                    back.tap()
                }
                guard attempt == 0,
                      state.originalSelectionRestored else {
                    return
                }
                // 只有原 Root 已重新选中后才允许重启；这样即使导入
                // Root 曾因异常重启被 claim，也会在本次重启后释放。
                app.terminate()
                app.launch()
                continue
            }

            delete.tap()
            let confirm = app.buttons.matching(
                NSPredicate(
                    format: "identifier == %@ OR label == %@",
                    "confirm-delete-watch-filesystem",
                    "永久删除")
            ).firstMatch
            guard confirm.waitForExistence(timeout: 5) else {
                return
            }
            confirm.tap()
            let remainingIdentifiers =
                bestEffortRootInventory(
                    containing: originalIdentifier,
                    in: app,
                    timeout: 180)
            state.importedRootDeleted =
                remainingIdentifiers == baselineIdentifiers
            if state.importedRootDeleted {
                state.rootInventoryReconciled = true
            }
            return
        }
    }

    private func bestEffortRootInventory(
        containing knownIdentifier: String,
        in app: XCUIApplication,
        timeout: TimeInterval
    ) -> Set<String>? {
        let deadline = Date().addingTimeInterval(timeout)
        while deadline.timeIntervalSinceNow > 0 {
            guard bestEffortOpenFileSystems(in: app) else {
                Thread.sleep(forTimeInterval: 0.5)
                continue
            }
            let fileSystems = app.descendants(matching: .any)[
                "watch-filesystems-view"]
            if elementContains(fileSystems, text: "已就绪"),
               let identifiers = fileSystemIdentifiers(in: app),
               identifiers.contains(knownIdentifier) {
                return identifiers
            }
            Thread.sleep(forTimeInterval: 0.5)
        }
        return nil
    }

    private func fileSystemIdentifiers(
        in app: XCUIApplication
    ) -> Set<String>? {
        let listStart = app.staticTexts[
            "watch-filesystems-list-start"]
        scrollToElement(listStart, in: app)
        guard listStart.exists && listStart.isHittable else {
            return nil
        }

        let rows = app.buttons.matching(
            NSPredicate(
                format: "identifier BEGINSWITH %@",
                "watch-filesystem-"))
        let listEnd = app.buttons[
            "restore-watch-filesystem-from-shared"]
        var identifiers: Set<String> = []

        for _ in 0..<80 {
            for index in 0..<rows.count {
                let identifier =
                    rows.element(boundBy: index).identifier
                if identifier.hasPrefix("watch-filesystem-") {
                    identifiers.insert(identifier)
                }
            }
            if listEnd.exists && listEnd.isHittable {
                return identifiers
            }
            app.coordinate(
                withNormalizedOffset:
                    CGVector(dx: 0.5, dy: 0.72)
            ).press(
                forDuration: 0.05,
                thenDragTo: app.coordinate(
                    withNormalizedOffset:
                        CGVector(dx: 0.5, dy: 0.30)),
                withVelocity: .slow,
                thenHoldForDuration: 0.05)
            Thread.sleep(forTimeInterval: 0.35)
        }
        return nil
    }

    private func bestEffortOpenFileSystems(
        in app: XCUIApplication
    ) -> Bool {
        let fileSystems = app.descendants(matching: .any)[
            "watch-filesystems-view"]
        if fileSystems.exists {
            return true
        }

        let detailBack = app.buttons.matching(
            identifier: "back-from-watch-filesystem-detail"
        ).firstMatch
        if detailBack.exists {
            detailBack.tap()
            if fileSystems.waitForExistence(timeout: 5) {
                return true
            }
        }

        let importView = app.descendants(matching: .any)[
            "watch-root-archive-import-view"]
        if importView.exists {
            let defaultBack = app.buttons.matching(
                NSPredicate(
                    format:
                        "label == %@ OR label CONTAINS %@",
                    "文件系统",
                    "返回文件系统")
            ).firstMatch
            if defaultBack.exists {
                defaultBack.tap()
                if fileSystems.waitForExistence(timeout: 5) {
                    return true
                }
            }
        }

        let settings = app.descendants(matching: .any)[
            "watch-settings-view"]
        if !settings.exists {
            let settingsButton = app.buttons.matching(
                identifier: "watch-settings-button"
            ).firstMatch
            if settingsButton.waitForExistence(timeout: 5) {
                settingsButton.tap()
            }
        }
        guard settings.waitForExistence(timeout: 5) else {
            return false
        }

        let link = app.buttons["watch-filesystems-link"]
        scrollToElement(link, in: app)
        guard link.exists && link.isHittable else {
            return false
        }
        link.tap()
        return fileSystems.waitForExistence(timeout: 10)
    }

    private func bestEffortOpenSharedFiles(
        in app: XCUIApplication
    ) -> Bool {
        let shared = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        if shared.exists {
            return true
        }

        let detailBack = app.buttons.matching(
            identifier: "back-from-watch-filesystem-detail"
        ).firstMatch
        if detailBack.exists {
            detailBack.tap()
        }
        let fileSystems = app.descendants(matching: .any)[
            "watch-filesystems-view"]
        if fileSystems.exists {
            let back = app.buttons.matching(
                identifier: "back-from-watch-filesystems"
            ).firstMatch
            if back.exists {
                back.tap()
            }
        }

        let settings = app.descendants(matching: .any)[
            "watch-settings-view"]
        if !settings.exists {
            let settingsButton = app.buttons.matching(
                identifier: "watch-settings-button"
            ).firstMatch
            if settingsButton.waitForExistence(timeout: 5) {
                settingsButton.tap()
            }
        }
        guard settings.waitForExistence(timeout: 5) else {
            return false
        }

        let link = app.buttons["watch-shared-files-link"]
        scrollToElement(link, in: app)
        guard link.exists && link.isHittable else {
            return false
        }
        link.tap()
        return shared.waitForExistence(timeout: 10)
    }

    private func bestEffortDeleteSharedFile(
        named fileName: String,
        in app: XCUIApplication
    ) -> Bool {
        let sharedPage = app.descendants(matching: .any)[
            "watch-shared-files-view"]
        let delete = sharedPage.descendants(matching: .button)
            .matching(identifier: "watch-delete-file-\(fileName)")
            .firstMatch
        scrollToElement(delete, in: app)
        if !delete.exists {
            return true
        }
        guard delete.isEnabled && delete.isHittable else {
            return false
        }
        delete.tap()

        let confirm = app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "confirm-delete-watch-shared-file",
                "永久删除")
        ).firstMatch
        guard confirm.waitForExistence(timeout: 5) else {
            return false
        }
        confirm.tap()
        return waitUntil(timeout: 30) {
            !delete.exists
        }
    }

    private func bestEffortCleanupGuestArchiveBaseline(
        _ state: RootArchiveCleanupState,
        app: XCUIApplication
    ) {
        guard !state.importMayHaveChangedSelection else {
            return
        }
        app.terminate()
        app.launch()
        didWarmSystemInput = false

        let input = commandInput(in: app)
        guard input.waitForExistence(timeout: 180) else {
            return
        }
        let inputReady = XCTNSPredicateExpectation(
            predicate: NSPredicate(format: "enabled == true"),
            object: input)
        guard XCTWaiter.wait(
                for: [inputReady],
                timeout: 180) == .completed else {
            return
        }
        let send = app.buttons["send-command"]
        let terminal = terminalTranscript(in: app)
        guard send.waitForExistence(timeout: 10),
              terminal.waitForExistence(timeout: 10) else {
            return
        }

        let token = String(UUID().uuidString.prefix(8))
        let pass = "RC:\(token):OK"
        let fail = "RC:\(token):ERR"
        let line =
            "b='\(state.baselinePath)'; ok=1; " +
            "rm -f \"$b\" || ok=0; " +
            "if test \"$ok\" -eq 1; then " +
            "printf 'RC:%s:OK\\n' '\(token)'; else " +
            "printf 'RC:%s:ERR\\n' '\(token)'; fi"
        _ = submitGuestLine(
            line,
            pass: pass,
            fail: fail,
            timeout: 120,
            app: app,
            input: input,
            send: send,
            terminal: terminal)
    }

    private func waitUntil(
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

    private func openTerminalThemes(in app: XCUIApplication) {
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

    private func reopenTerminalThemes(in app: XCUIApplication) {
        app.terminate()
        app.launch()
        openTerminalThemes(in: app)
    }

    private func openWatchCustomTheme(
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

    private func adaptiveThemeToggle(
        in app: XCUIApplication
    ) -> XCUIElement {
        app.switches["watch-custom-theme-adaptive-toggle"]
    }

    private func openWatchThemePalette(
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

    private func assertWatchThemeValue(
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

    private func submitWatchText(
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

    private func openSettingsPage(
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

    private func scrollToElement(
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

    private func scrollToExistingElement(
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

    private func waitForElementCount(
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

    private func waitForExactElementCount(
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

    private func commandInput(in app: XCUIApplication) -> XCUIElement {
        app.buttons.matching(
            NSPredicate(
                format: "identifier == %@ OR label == %@",
                "command-input",
                "命令")
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
        terminal.debugDescription
    }

    private func terminalContains(
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

    private func waitForTerminalOutput(
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

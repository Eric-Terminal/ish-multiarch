import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
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

}

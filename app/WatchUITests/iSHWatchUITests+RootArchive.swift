import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
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

}

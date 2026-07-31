import Foundation
import XCTest

@MainActor
extension iSHWatchUITests {
    func deleteSharedFile(
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

    func bestEffortCleanupRootArchiveTest(
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

    func bestEffortEnsureRootArchiveAppRunning(
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

    func captureAndDismissRootArchiveResult(
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

    func cancelRootArchiveOperationIfNeeded(
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

    func bestEffortRestoreOriginalRootSelection(
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

    func bestEffortDeleteImportedRoot(
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

    func bestEffortRootInventory(
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

    func fileSystemIdentifiers(
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

    func bestEffortOpenFileSystems(
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

    func bestEffortOpenSharedFiles(
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

    func bestEffortDeleteSharedFile(
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

    func bestEffortCleanupGuestArchiveBaseline(
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

}

import Foundation
import XCTest

@MainActor
final class iSHWatchUITests: XCTestCase {
    enum GuestTransportResult: Equatable {
        case pass
        case retryableFail
        case fail
        case timeout
    }

    final class RootArchiveCleanupState {
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

    var didWarmSystemInput = false
    var didRecoverGuestState = false
    var guestRecoveryRequired = false
    var recoveryApp: XCUIApplication?
    var rootArchiveCleanupState: RootArchiveCleanupState?
    var rootArchiveCleanupApp: XCUIApplication?

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
}

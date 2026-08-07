import Foundation

enum WatchRuntimeRecovery: Equatable, Sendable {
    case retryPreparation
    case reopenApplication

    var allowsPreparationRetry: Bool {
        self == .retryPreparation
    }

    var emptyStateInstruction: String {
        switch self {
        case .retryPreparation:
            return "Linux 内核尚未启动。请检查文件系统后重新准备；启动设置更改需重新打开 App。"
        case .reopenApplication:
            return "请检查文件系统和启动命令，在 App 切换器中结束 iSH 后重新打开。"
        }
    }

    var settingsInstruction: String {
        switch self {
        case .retryPreparation:
            return "这次失败发生在 Linux 内核启动前，可以安全重新准备。若修改了系统启动命令，请在 App 切换器中结束 iSH 后重新打开，修改才会生效。"
        case .reopenApplication:
            return "本次 App 进程中的 Linux 内核不能原地重启。请先检查文件系统和启动命令，再在 App 切换器中结束 iSH，并从 App 列表重新打开。"
        }
    }

    func status(appendingTo detail: String) -> String {
        "\(detail)\n\(emptyStateInstruction)"
    }
}

struct WatchRuntimeActivityState: Equatable, Sendable {
    private(set) var isForegroundActive = true

    var allowsStartingHeavyWork: Bool {
        isForegroundActive
    }

    mutating func update(isForegroundActive: Bool) {
        self.isForegroundActive = isForegroundActive
    }
}

#if os(watchOS)
import Combine
import WatchKit

@MainActor
final class WatchRuntime: ObservableObject {
    let automaticHostname: String
    let hostname: String
    let bootCommand: String
    let rootStore: WatchRootStore

    @Published private(set) var launchCommand: String
    @Published private(set) var sessionSnapshots:
        [WatchTerminalSessionSnapshot] = []
    @Published private(set) var selectedSessionID: UUID?
    @Published private(set) var activeSession:
        WatchTerminalSessionSnapshot?
    @Published private(set) var renderedLines: [TerminalRenderLine] = []
    @Published private(set) var cursor = TerminalCursor(
        row: 0, column: 0, isVisible: true)
    @Published private(set) var modes = TerminalModes()
    @Published private(set) var canCreateSession = false
    @Published private(set) var canRetryPreparation = false
    @Published private(set) var hasRuntimeFailure = false
    @Published var recovery: WatchRuntimeRecovery?
    @Published private(set) var activityState =
        WatchRuntimeActivityState()
    @Published var apkRepositoryMigration =
        WatchAPKRepositoryMigrationCoordinator()

    @Published private(set) var status = "准备中"
    @Published private(set) var acceptsInput = false
    @Published private(set) var canReopenSession = false
    @Published private(set) var revision = 0

    private struct RuntimePaths: Sendable {
        let rootData: String
        let documentsDirectory: String
        let socketPrefix: String
        let hostname: String
        let bootCommand: String
        let launchCommand: String
        let columns: UInt16
        let rows: UInt16
    }

    private struct StartResult: Sendable {
        let runtime: Int32
        let session: Int32
        let sessionID: UInt64
    }

    struct APKRepositoryInspectionResult: Sendable {
        let status: WatchAPKRepositoryMigrationStatus?
        let errorMessage: String?
    }

    enum APKRepositoryOperationResult: Sendable {
        case success(WatchAPKRepositoryMigrationOutcome)
        case failure(String)
    }

    var terminalSessions = WatchTerminalSessions()
    private var startTask: Task<Void, Never>?
    var sessionTasks: [UUID: Task<Void, Never>] = [:]
    var apkRepositoryTask: Task<Void, Never>?
    var apkRepositoryTaskToken: UUID?
    var pendingClose: Set<UUID> = []
    private var started = false
    var guestPhase = WatchGuestRuntimePhase.idle
    var globalStatusOverride: String? = "准备中"
    var pollTick: UInt64 = 0
    var inactivePollIndex = 0
    var readBuffer = [UInt8](repeating: 0, count: 4096)
    private var setupFailure: String?

    convenience init() {
        self.init(rootStore: WatchRootStore())
    }

    init(rootStore: WatchRootStore) {
        self.rootStore = rootStore
        let defaults = UserDefaults.standard
        let deviceName = WKInterfaceDevice.current().name
        automaticHostname = WatchPreferences.hostname(
            deviceName: deviceName,
            usesCustomHostname: false,
            customHostname: "")
        hostname = WatchPreferences.hostname(
            deviceName: deviceName,
            usesCustomHostname: defaults.bool(
                forKey: WatchPreferenceKeys.customHostnameEnabled),
            customHostname: defaults.string(
                forKey: WatchPreferenceKeys.customHostname) ?? "")
        bootCommand = WatchPreferences.bootCommand(
            defaults.string(forKey: WatchPreferenceKeys.bootCommand))
        launchCommand = WatchPreferences.launchCommand(
            defaults.string(forKey: WatchPreferenceKeys.launchCommand))
        WatchPlatformBridge.install(
            automaticHostname: automaticHostname)
        refreshPublishedState()
    }

    func run() async {
        startIfNeeded()
        while !Task.isCancelled {
            if setupFailure != nil {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                continue
            }
            guard poll() else { return }
            try? await Task.sleep(nanoseconds: 200_000_000)
        }
    }

    func setForegroundActive(_ isActive: Bool) {
        var nextState = activityState
        nextState.update(isForegroundActive: isActive)
        if nextState != activityState {
            activityState = nextState
        }
    }

    func retryPreparation() {
        guard canRetryPreparation,
              guestPhase == .failed,
              startTask == nil else {
            return
        }
        started = false
        guestPhase = .idle
        setupFailure = nil
        canRetryPreparation = false
        recovery = nil
        globalStatusOverride = "准备中"
        updateAPKRepositoryMigration {
            $0.beginChecking()
        }
        refreshPublishedState()
        startIfNeeded()
    }

    @discardableResult
    func updateLaunchCommand(_ command: String) -> Bool {
        guard WatchPreferences.launchCommandValidationIssue(command) ==
                nil else {
            return false
        }
        launchCommand = command
        return true
    }

    @discardableResult
    func createSession(
        title: String? = nil,
        command: String? = nil
    ) -> UUID? {
        createSession(
            title: title,
            command: command,
            purpose: .interactive)
    }

    @discardableResult
    func createSharedFilesSession() -> UUID? {
        createSession(
            title: "共享文件",
            command: WatchSharedFiles.terminalCommand,
            purpose: .interactive)
    }

    @discardableResult
    func createRootArchiveSession(rootName: String) -> UUID? {
        guard rootStore.activeName == rootName else { return nil }
        let request = WatchRootArchiveRequest(rootName: rootName)
        return createSession(
            title: "导出 \(rootStore.displayName(for: rootName))",
            command: request.command,
            purpose: .interactive)
    }

    @discardableResult
    func createMaintenanceSession(
        title: String,
        operation: WatchSoftwareMaintenanceOperation,
        body: String
    ) -> UUID? {
        guard canStartSoftwareMaintenance else { return nil }
        return createApprovedMaintenanceSession(
            title: title,
            operation: operation,
            body: body)
    }

    var canStartSoftwareMaintenance: Bool {
        canCreateSession &&
            activityState.allowsStartingHeavyWork &&
            apkRepositoryTask == nil &&
            !apkRepositoryMigration.isBusy &&
            !terminalSessions.hasExecutingSoftwareMaintenance
    }

    var canStartPackageUpgrade: Bool {
        canStartSoftwareMaintenance &&
            apkRepositoryMigration.canStartPackageUpgrade
    }

    @discardableResult
    func startPackageUpgrade(
        title: String,
        body: String
    ) -> Bool {
        guard canStartPackageUpgrade else { return false }
        var migration = apkRepositoryMigration
        guard migration.beginUpgradePreparation() else {
            return false
        }
        apkRepositoryMigration = migration

        let token = UUID()
        apkRepositoryTaskToken = token
        apkRepositoryTask = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Self.prepareAPKRepositoriesForUpgrade()
            }.value
            guard let self,
                  self.apkRepositoryTaskToken == token else {
                return
            }
            self.apkRepositoryTask = nil
            self.apkRepositoryTaskToken = nil

            switch result {
            case let .failure(message):
                self.updateAPKRepositoryMigration {
                    $0.recordFailure(message)
                }
            case let .success(outcome):
                guard self.activityState.allowsStartingHeavyWork else {
                    self.updateAPKRepositoryMigration {
                        $0.recordPreparedUpgradeFailure(
                            outcome: outcome,
                            message:
                                "App 已进入后台，未启动软件升级；" +
                                "请回到前台后重试")
                    }
                    return
                }
                guard self.canCreateSession,
                      !self.terminalSessions
                        .hasExecutingSoftwareMaintenance,
                      let sessionID =
                        self.createApprovedMaintenanceSession(
                        title: title,
                        operation: .upgradePackages,
                        body: body) else {
                    self.updateAPKRepositoryMigration {
                        $0.recordPreparedUpgradeFailure(
                            outcome: outcome,
                            message:
                                "无法创建升级终端；" +
                                "请关闭一个终端后重试")
                    }
                    return
                }
                self.updateAPKRepositoryMigration {
                    $0.recordPreparedUpgrade(
                        sessionID: sessionID,
                        outcome: outcome)
                }
            }
        }
        return true
    }

    private func createApprovedMaintenanceSession(
        title: String,
        operation: WatchSoftwareMaintenanceOperation,
        body: String
    ) -> UUID? {
        guard WatchPreferences.launchCommandValidationIssue(body) ==
                nil else {
            return nil
        }
        let completionToken =
            WatchSoftwareMaintenanceCompletionProtocol.makeToken()
        let command =
            WatchSoftwareMaintenanceCompletionProtocol.shellCommand(
                body: body,
                operation: operation,
                token: completionToken)
        return createSession(
            title: title,
            command: command,
            purpose: .softwareMaintenance(operation),
            maintenanceCompletionToken: completionToken)
    }

    private func createSession(
        title: String?,
        command: String?,
        purpose: WatchTerminalSessionPurpose,
        maintenanceCompletionToken: String? = nil
    ) -> UUID? {
        if command == nil {
            refreshLaunchCommandFromDefaults()
        }
        let sessionCommand = command ?? launchCommand
        guard guestPhase == .running,
              WatchPreferences.launchCommandValidationIssue(
                sessionCommand) == nil,
              let id = terminalSessions.add(
                title: title,
                lifecycle: .opening,
                purpose: purpose,
                maintenanceCompletionToken:
                    maintenanceCompletionToken),
              let session = terminalSessions.session(id: id) else {
            return nil
        }
        globalStatusOverride = nil
        refreshPublishedState()
        let request = WatchTerminalSessionLaunchRequest(
            defaultCommand: launchCommand,
            command: sessionCommand,
            columns: session.screen.columns,
            rows: session.screen.rows)
        scheduleSessionCreation(
            id: id,
            request: request)
        return id
    }

    @discardableResult
    func selectSession(_ id: UUID) -> Bool {
        guard terminalSessions.select(id) else { return false }
        refreshPublishedState()
        if guestPhase == .running {
            _ = pollSession(id: id, maximumReads: 4)
            refreshPublishedState()
        }
        return true
    }

    @discardableResult
    func renameSession(_ id: UUID, title: String) -> Bool {
        guard terminalSessions.rename(id, title: title) else {
            return false
        }
        refreshPublishedState()
        return true
    }

    @discardableResult
    func clearScrollback(for id: UUID) -> Int? {
        guard let removedLineCount =
                terminalSessions.clearScrollback(id) else {
            return nil
        }
        refreshPublishedState()
        return removedLineCount
    }

    @discardableResult
    func closeSession(_ id: UUID) -> Bool {
        guard let session = terminalSessions.session(id: id) else {
            return false
        }
        recordAPKUpgradeSessionClosed(id)
        if sessionTasks[id] != nil {
            if session.lifecycle == .closing {
                return true
            }
            pendingClose.insert(id)
            terminalSessions.update(id) { $0.lifecycle = .closing }
            refreshPublishedState()
            return true
        }
        guard session.sessionID != 0 else {
            terminalSessions.remove(id)
            refreshPublishedState()
            return true
        }
        scheduleSessionClose(id: id, sessionID: session.sessionID)
        return true
    }

    @discardableResult
    func closeActiveSession() -> Bool {
        guard let id = terminalSessions.selectedSessionID else {
            return false
        }
        return closeSession(id)
    }

    func reopenSession() {
        guard let id = terminalSessions.selectedSessionID,
              let session = terminalSessions.session(id: id),
              session.canReopenSession,
              sessionTasks[id] == nil else {
            return
        }
        recordAPKUpgradeSessionClosed(id)

        _ = drainOutput(id: id, maximumReads: 4)
        guard let refreshedSession =
                terminalSessions.session(id: id) else {
            return
        }
        let previousSessionID = refreshedSession.sessionID
        let columns = refreshedSession.screen.columns
        let rows = refreshedSession.screen.rows
        let separatesOutput = refreshedSession.sawOutput
        refreshLaunchCommandFromDefaults()
        let command = launchCommand
        var generation: UInt64 = 0
        terminalSessions.update(id) {
            generation = $0.beginOperation()
            $0.beginOpening(purpose: .interactive)
        }
        refreshPublishedState()

        sessionTasks[id] = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                let closeResult = previousSessionID == 0 ?
                    0 : ish_watch_session_close(previousSessionID)
                guard closeResult == 0 else {
                    return (
                        result: closeResult,
                        id: UInt64(0),
                        previousClosed: false)
                }
                let created = Self.createCSession(
                    command: command,
                    columns: columns,
                    rows: rows)
                return (
                    result: created.result,
                    id: created.id,
                    previousClosed: true)
            }.value
            guard let self else {
                if result.result >= 0 {
                    _ = await Task.detached {
                        ish_watch_session_close(result.id)
                    }.value
                }
                return
            }
            self.sessionTasks[id] = nil
            guard let current =
                    self.terminalSessions.session(id: id),
                  current.matchesOperation(
                    generation: generation) else {
                self.pendingClose.remove(id)
                if result.result >= 0 {
                    _ = await Task.detached {
                        ish_watch_session_close(result.id)
                    }.value
                }
                return
            }

            if self.pendingClose.remove(id) != nil {
                self.finishPendingClose(
                    id: id,
                    createdResult: result.result,
                    createdSessionID: result.id,
                    previousSessionID: previousSessionID,
                    previousClosed: result.previousClosed)
                return
            }

            if result.result < 0 {
                let retainedID = result.previousClosed ?
                    UInt64(0) : previousSessionID
                self.terminalSessions.update(id) {
                    $0.markFailed(
                        WatchTerminalStatusText.errorMessage(
                            prefix: "重新打开失败",
                            code: result.result),
                        sessionID: retainedID)
                }
            } else {
                self.terminalSessions.update(id) {
                    $0.finishOpening(
                        sessionID: result.id,
                        createdColumns: columns,
                        createdRows: rows,
                        separatesPreviousOutput: separatesOutput)
                }
            }
            self.refreshPublishedState()
        }
    }

    func resize(columns: Int, rows: Int) {
        guard let id = terminalSessions.selectedSessionID else { return }
        resizeSession(id, columns: columns, rows: rows)
    }

    func resizeSession(_ id: UUID, columns: Int, rows: Int) {
        var resized = false
        terminalSessions.update(id) {
            resized = $0.resize(columns: columns, rows: rows)
        }
        guard resized else { return }
        if guestPhase == .running {
            _ = applyWindowSize(id: id)
        }
        refreshPublishedState()
    }

    @discardableResult
    func sendInput(_ bytes: [UInt8]) -> Bool {
        enqueue(bytes)
    }

    @discardableResult
    func sendText(_ text: String) -> Bool {
        enqueue(WatchTerminalInput.text(text))
    }

    @discardableResult
    func sendReturn(after pendingText: String = "") -> Bool {
        enqueue(WatchTerminalInput.enter(after: pendingText))
    }

    @discardableResult
    func sendDelete(after pendingText: String = "") -> Bool {
        enqueue(WatchTerminalInput.delete(after: pendingText))
    }

    @discardableResult
    func sendMeta(
        _ text: String,
        after pendingText: String = ""
    ) -> Bool {
        guard let bytes = WatchTerminalInput.meta(
            text,
            after: pendingText
        ) else {
            return false
        }
        return enqueue(bytes)
    }

    @discardableResult
    func sendControlC() -> Bool {
        sendControl("C")
    }

    @discardableResult
    func sendControl(
        _ character: Character,
        after pendingText: String = ""
    ) -> Bool {
        guard let byte = WatchControlInput.byte(
            for: character) else {
            return false
        }
        return enqueue(
            WatchTerminalInput.sequence([byte], after: pendingText))
    }

    @discardableResult
    func sendTab(after pendingText: String) -> Bool {
        enqueue(WatchTerminalInput.sequence([0x09], after: pendingText))
    }

    @discardableResult
    func sendEscape(after pendingText: String) -> Bool {
        enqueue(WatchTerminalInput.sequence(
            [WatchTerminalInput.escape],
            after: pendingText))
    }

    @discardableResult
    func sendArrowUp(after pendingText: String = "") -> Bool {
        sendCursorKey(0x41, after: pendingText)
    }

    @discardableResult
    func sendArrowDown(after pendingText: String = "") -> Bool {
        sendCursorKey(0x42, after: pendingText)
    }

    @discardableResult
    func sendArrowRight(after pendingText: String) -> Bool {
        sendCursorKey(0x43, after: pendingText)
    }

    @discardableResult
    func sendArrowLeft(after pendingText: String) -> Bool {
        sendCursorKey(0x44, after: pendingText)
    }

    private func startIfNeeded() {
        guard !started else { return }
        started = true
        canRetryPreparation = false
        recovery = nil
        globalStatusOverride = "准备 Linux"
        refreshPublishedState()

        let initialID = terminalSessions.selectedSessionID
        var initialGeneration: UInt64 = 0
        if let initialID {
            terminalSessions.update(initialID) {
                initialGeneration = $0.beginOperation()
            }
        }
        startTask = Task { [weak self] in
            guard let self else { return }
            defer {
                self.startTask = nil
            }
            do {
                let root = try await self.rootStore.prepareForLaunch()
                let initialSession = initialID.flatMap {
                    self.terminalSessions.session(id: $0)
                } ?? WatchTerminalSession(
                    title: "终端",
                    columns: 40,
                    rows: 18)
                let sharedDirectory =
                    try WatchSharedFiles.ensureSystemDirectory()
                _ = try await Task.detached(priority: .utility) {
                    try WatchSharedFiles
                        .cleanupInterruptedRootArchivePartials(
                            in: sharedDirectory)
                }.value
                let paths = try self.preparePaths(
                    rootData: root.dataPath,
                    documentsDirectory: sharedDirectory.path,
                    columns: initialSession.screen.columns,
                    rows: initialSession.screen.rows)
                let result = await Task.detached(
                    priority: .userInitiated
                ) {
                    Self.startRuntimeAndSession(paths)
                }.value

                if result.runtime < 0 {
                    self.guestPhase = .failed
                    self.recordAPKRepositoryRuntimeUnavailable()
                    self.reportRecovery(
                        detail: WatchTerminalStatusText.errorMessage(
                            prefix: "启动失败", code: result.runtime),
                        recovery: .reopenApplication)
                } else {
                    self.guestPhase = .running
                    self.recovery = nil
                    self.globalStatusOverride = nil
                    self.rootStore.activate(root.name)
                    self.scheduleAPKRepositoryInspection()
                    let initialSessionMatches: Bool
                    if let initialID {
                        initialSessionMatches =
                            self.terminalSessions.session(id: initialID)?
                                .matchesOperation(
                                    generation: initialGeneration) == true
                    } else {
                        initialSessionMatches = false
                    }
                    if result.session < 0 {
                        if let initialID, initialSessionMatches {
                            self.terminalSessions.update(initialID) {
                                $0.markFailed(
                                    WatchTerminalStatusText.errorMessage(
                                    prefix: "终端启动失败",
                                    code: result.session))
                            }
                        }
                    } else if !initialSessionMatches {
                        _ = await Task.detached {
                            ish_watch_session_close(result.sessionID)
                        }.value
                    } else if let initialID {
                        self.terminalSessions.update(initialID) {
                            $0.finishOpening(
                                sessionID: result.sessionID,
                                createdColumns: Int(paths.columns),
                                createdRows: Int(paths.rows),
                                separatesPreviousOutput: false)
                        }
                    }
                }
                self.refreshPublishedState()
                if self.guestPhase == .running {
                    _ = self.pollSessions()
                    self.refreshPublishedState()
                }
            } catch {
                self.guestPhase = .failed
                self.setupFailure = error.localizedDescription
                self.canRetryPreparation = true
                self.recordAPKRepositoryRuntimeUnavailable()
                // C runtime 尚未被调用，只有这个边界允许复用当前 App 进程。
                self.reportRecovery(
                    detail: self.setupFailure ?? "准备 Linux 失败",
                    recovery: .retryPreparation)
                self.refreshPublishedState()
            }
        }
    }

    private func preparePaths(
        rootData: String,
        documentsDirectory: String,
        columns: Int,
        rows: Int
    ) throws -> RuntimePaths {
        refreshLaunchCommandFromDefaults()
        // 真机去掉等价的 /private 前缀，使绝对路径满足 Darwin sun_path 上限。
        let socketPrefix: String
#if targetEnvironment(simulator)
        socketPrefix = "/tmp/ishsock"
#else
        var temporaryDirectory = FileManager.default.temporaryDirectory.path
        if temporaryDirectory.hasPrefix("/private/var/") {
            temporaryDirectory.removeFirst("/private".count)
        }
        socketPrefix = (temporaryDirectory as NSString)
            .appendingPathComponent("s")
#endif
        guard socketPrefix.utf8.count <= 82 else {
            throw WatchRuntimeSetupError.socketPrefixTooLong
        }
        return RuntimePaths(
            rootData: rootData,
            documentsDirectory: documentsDirectory,
            socketPrefix: socketPrefix,
            hostname: hostname,
            bootCommand: bootCommand,
            launchCommand: launchCommand,
            columns: UInt16(columns),
            rows: UInt16(rows))
    }

    private func refreshLaunchCommandFromDefaults() {
        let command = WatchPreferences.launchCommand(
            UserDefaults.standard.string(
                forKey: WatchPreferenceKeys.launchCommand))
        if launchCommand != command {
            launchCommand = command
        }
    }

    nonisolated private static func startRuntimeAndSession(
        _ paths: RuntimePaths
    ) -> StartResult {
        let runtimeResult = paths.rootData.withCString { rootData in
            paths.documentsDirectory.withCString { documentsDirectory in
                paths.socketPrefix.withCString { socketPrefix in
                    paths.hostname.withCString { hostname in
                        paths.bootCommand.withCString { bootCommand in
                            ish_watch_runtime_start(
                                rootData,
                                documentsDirectory,
                                socketPrefix,
                                hostname,
                                bootCommand)
                        }
                    }
                }
            }
        }
        guard runtimeResult == 0 else {
            return StartResult(
                runtime: runtimeResult,
                session: 0,
                sessionID: 0)
        }
        let sessionResult = createCSession(
            command: paths.launchCommand,
            columns: Int(paths.columns),
            rows: Int(paths.rows))
        return StartResult(
            runtime: runtimeResult,
            session: sessionResult.result,
            sessionID: sessionResult.id)
    }

    nonisolated static func createCSession(
        command: String,
        columns: Int,
        rows: Int
    ) -> (result: Int32, id: UInt64) {
        var id: UInt64 = 0
        let result = command.withCString {
            ish_watch_session_create(
                $0, UInt16(columns), UInt16(rows), &id)
        }
        return (result, id)
    }

    func refreshRuntimeAvailability() {
        let nextCanCreate =
            globalStatusOverride == nil &&
            guestPhase == .running
        if canCreateSession != nextCanCreate {
            canCreateSession = nextCanCreate
        }
    }

    func reportRecovery(
        detail: String,
        recovery: WatchRuntimeRecovery
    ) {
        self.recovery = recovery
        globalStatusOverride = recovery.status(appendingTo: detail)
    }

    func refreshPublishedState() {
        let snapshots = terminalSessions.snapshots
        let selection = terminalSessions.selectedSessionID
        let nextActive = snapshots.first { $0.id == selection }
        let nextRendered = nextActive?.renderedLines ?? []
        let nextCursor = nextActive?.cursor ??
            TerminalCursor(row: 0, column: 0, isVisible: true)
        let nextModes = nextActive?.modes ?? TerminalModes()
        let visualChanged =
            selectedSessionID != selection ||
            activeSession?.hasOutput != nextActive?.hasOutput ||
            renderedLines != nextRendered ||
            cursor != nextCursor ||
            modes != nextModes

        if sessionSnapshots != snapshots {
            sessionSnapshots = snapshots
        }
        if selectedSessionID != selection {
            selectedSessionID = selection
        }
        if activeSession != nextActive {
            activeSession = nextActive
        }
        if renderedLines != nextRendered {
            renderedLines = nextRendered
        }
        if cursor != nextCursor {
            cursor = nextCursor
        }
        if modes != nextModes {
            modes = nextModes
        }

        let hasRuntime = globalStatusOverride == nil &&
            guestPhase == .running
        let nextStatus = globalStatusOverride ??
            nextActive?.status ?? "没有终端"
        let nextAcceptsInput =
            hasRuntime && (nextActive?.acceptsInput ?? false)
        let nextCanReopen =
            hasRuntime && (nextActive?.canReopenSession ?? false)
        let nextHasRuntimeFailure = recovery != nil
        if status != nextStatus {
            status = nextStatus
        }
        if acceptsInput != nextAcceptsInput {
            acceptsInput = nextAcceptsInput
        }
        if canReopenSession != nextCanReopen {
            canReopenSession = nextCanReopen
        }
        if hasRuntimeFailure != nextHasRuntimeFailure {
            hasRuntimeFailure = nextHasRuntimeFailure
        }
        refreshRuntimeAvailability()

        if visualChanged {
            revision &+= 1
        }
    }

}
#endif

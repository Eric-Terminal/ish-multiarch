import Foundation

enum WatchTerminalSessionLifecycle: Equatable {
    case preparing
    case opening
    case running
    case exited(String)
    case failed(String)
    case closing
}

enum WatchSoftwareMaintenanceOperation: Equatable, Sendable {
    case updateIndex
    case upgradePackages

    fileprivate var completionFrameOperation: String {
        switch self {
        case .updateIndex:
            return "update-index"
        case .upgradePackages:
            return "upgrade-packages"
        }
    }

    fileprivate var completionDisplayName: String {
        switch self {
        case .updateIndex:
            return "索引更新结束"
        case .upgradePackages:
            return "软件升级结束"
        }
    }

    fileprivate var runningStatus: String {
        switch self {
        case .updateIndex:
            return "正在更新软件索引"
        case .upgradePackages:
            return "正在升级软件"
        }
    }
}

struct WatchSoftwareMaintenanceCompletionProtocol {
    static let tokenLength = 32
    private static let frameIntroducer = "\u{1b}_"
    private static let payloadVersion =
        "ish-watch-maintenance/1;complete;"
    private static let frameSuffix: [UInt8] = [0x1b, 0x5c]

    static func makeToken() -> String {
        var generator = SystemRandomNumberGenerator()
        let bytes = (0..<16).map { _ in
            UInt8.random(in: UInt8.min...UInt8.max, using: &generator)
        }
        return token(bytes: bytes)!
    }

    static func token(bytes: [UInt8]) -> String? {
        guard bytes.count == 16 else { return nil }
        let digits = Array("0123456789abcdef".utf8)
        var result = [UInt8]()
        result.reserveCapacity(tokenLength)
        for byte in bytes {
            result.append(digits[Int(byte >> 4)])
            result.append(digits[Int(byte & 0x0f)])
        }
        return String(decoding: result, as: UTF8.self)
    }

    static func isValidToken(_ token: String) -> Bool {
        token.utf8.count == tokenLength &&
            token.utf8.allSatisfy {
                (UInt8(ascii: "0")...UInt8(ascii: "9"))
                    .contains($0) ||
                (UInt8(ascii: "a")...UInt8(ascii: "f"))
                    .contains($0)
            }
    }

    static func shellCommand(
        body: String,
        operation: WatchSoftwareMaintenanceOperation,
        token: String
    ) -> String {
        precondition(!body.isEmpty)
        precondition(isValidToken(token))
        let payloadPrefix = prefix(
            operation: operation,
            token: token)
        return "/bin/sh -c \(shellSingleQuoted(body)); status=$?; " +
            "printf '\\n\(operation.completionDisplayName)" +
            "（状态 %s）\\n' \"$status\"; " +
            "printf '\\033_\(payloadPrefix)%s\\033\\\\' " +
            "\"$status\"; " +
            "exec /bin/login -f root"
    }

    static func frame(
        operation: WatchSoftwareMaintenanceOperation,
        token: String,
        status: UInt8
    ) -> String {
        precondition(isValidToken(token))
        return frameIntroducer +
            prefix(operation: operation, token: token) +
            String(status) +
            "\u{1b}\\"
    }

    static func status(
        in bytes: [UInt8],
        operation: WatchSoftwareMaintenanceOperation,
        token: String
    ) -> Int? {
        guard isValidToken(token) else { return nil }
        let expectedPrefix = Array((
            frameIntroducer +
            prefix(operation: operation, token: token)
        ).utf8)
        let minimumFrameByteCount =
            expectedPrefix.count + 1 + frameSuffix.count
        guard bytes.count >= minimumFrameByteCount else { return nil }

        for start in 0...(bytes.count - minimumFrameByteCount) {
            guard bytes[start..<(start + expectedPrefix.count)]
                    .elementsEqual(expectedPrefix) else {
                continue
            }
            let statusStart = start + expectedPrefix.count
            for statusCount in 1...3 {
                let suffixStart = statusStart + statusCount
                guard suffixStart + frameSuffix.count <= bytes.count,
                      bytes[suffixStart..<(suffixStart + frameSuffix.count)]
                        .elementsEqual(frameSuffix) else {
                    continue
                }
                let statusBytes =
                    bytes[statusStart..<suffixStart]
                guard isCanonicalStatus(statusBytes),
                      let status = Int(String(
                        decoding: statusBytes,
                        as: UTF8.self)),
                      status <= 255 else {
                    continue
                }
                return status
            }
        }
        return nil
    }

    static func maximumFrameByteCount(
        operation: WatchSoftwareMaintenanceOperation,
        token: String
    ) -> Int {
        frameIntroducer.utf8.count +
            prefix(operation: operation, token: token).utf8.count +
            3 +
            frameSuffix.count
    }

    private static func prefix(
        operation: WatchSoftwareMaintenanceOperation,
        token: String
    ) -> String {
        payloadVersion +
            "\(operation.completionFrameOperation);\(token);"
    }

    private static func isCanonicalStatus(
        _ bytes: ArraySlice<UInt8>
    ) -> Bool {
        guard !bytes.isEmpty,
              bytes.allSatisfy({
                  (UInt8(ascii: "0")...UInt8(ascii: "9"))
                      .contains($0)
              }) else {
            return false
        }
        return bytes.count == 1 || bytes.first != UInt8(ascii: "0")
    }

    private static func shellSingleQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(
            of: "'", with: "'\"'\"'") + "'"
    }
}

enum WatchSoftwareMaintenancePhase: Equatable, Sendable {
    case executing
    case completed
    case failed(Int)
    case interrupted
}

enum WatchTerminalSessionPurpose: Equatable, Sendable {
    case interactive
    case softwareMaintenance(WatchSoftwareMaintenanceOperation)

    var maintenanceOperation: WatchSoftwareMaintenanceOperation? {
        guard case let .softwareMaintenance(operation) = self else {
            return nil
        }
        return operation
    }
}

enum WatchTerminalCloseProtection: Equatable, Sendable {
    case standard
    case softwareIndexUpdate
    case packageDatabaseWrite
}

enum WatchGuestRuntimePhase: Int32 {
    case idle = 0
    case preparing = 1
    case running = 2
    case stopped = 3
    case failed = 4
}

enum WatchCSessionPhase: Int32 {
    case starting = 1
    case running = 2
    case exited = 3
}

enum WatchRuntimeSetupError: LocalizedError {
    case socketPrefixTooLong

    var errorDescription: String? {
        "应用临时目录过长，无法创建本地 socket"
    }
}

enum WatchTerminalStatusText {
    static func exitDescription(_ rawWaitStatus: Int32) -> String {
        let waitStatus = UInt32(bitPattern: rawWaitStatus)
        let signal = waitStatus & 0x7f
        if signal == 0 {
            let exitCode = (waitStatus >> 8) & 0xff
            return exitCode == 0 ?
                "会话已退出" : "会话已退出（状态 \(exitCode)）"
        }
        return "会话被信号 \(signal) 终止"
    }

    static func errorMessage(prefix: String, code: Int32) -> String {
        "\(prefix)（Linux errno \(-code)）"
    }

    static func errorMessage(prefix: String, code: Int) -> String {
        "\(prefix)（Linux errno \(-code)）"
    }
}

struct WatchTerminalSessionSnapshot: Identifiable, Equatable {
    let id: UUID
    let title: String
    let sessionID: UInt64
    let status: String
    let acceptsInput: Bool
    let canReopenSession: Bool
    let hasOutput: Bool
    let columns: Int
    let rows: Int
    let renderedLines: [TerminalRenderLine]
    let visibleLines: [TerminalRenderLine]
    let scrollbackLines: [TerminalRenderLine]
    let scrollbackLineCount: Int
    let cursor: TerminalCursor
    let modes: TerminalModes
    let purpose: WatchTerminalSessionPurpose
    let maintenancePhase: WatchSoftwareMaintenancePhase?
    let closeProtection: WatchTerminalCloseProtection

    var text: String {
        renderedLines.map(\.text).joined(separator: "\n")
    }
}

struct WatchTerminalSessionLaunchRequest: Equatable, Sendable {
    let command: String
    let columns: Int
    let rows: Int

    init(
        defaultCommand: String,
        command: String?,
        columns: Int,
        rows: Int
    ) {
        self.command = command ?? defaultCommand
        self.columns = columns
        self.rows = rows
    }
}

struct WatchTerminalSession: Identifiable {
    static let inputLimit = 16 * 1024

    let id: UUID
    var title: String
    var sessionID: UInt64
    var operationGeneration: UInt64 = 0
    var screen: TerminalScreen
    var pendingInput: [UInt8] = []
    var lifecycle: WatchTerminalSessionLifecycle
    var sawOutput = false
    var inputNotice: String?
    var windowSizeNeedsUpdate = false
    var purpose: WatchTerminalSessionPurpose
    var maintenancePhase: WatchSoftwareMaintenancePhase?
    private(set) var maintenanceCompletionToken: String?
    private var maintenanceOutputProbe: [UInt8] = []

    init(
        id: UUID = UUID(),
        title: String,
        sessionID: UInt64 = 0,
        columns: Int = 40,
        rows: Int = 18,
        lifecycle: WatchTerminalSessionLifecycle = .preparing,
        purpose: WatchTerminalSessionPurpose = .interactive,
        maintenanceCompletionToken: String? = nil
    ) {
        precondition(
            purpose.maintenanceOperation == nil ?
                maintenanceCompletionToken == nil :
                maintenanceCompletionToken.map(
                    WatchSoftwareMaintenanceCompletionProtocol
                        .isValidToken) == true)
        self.id = id
        self.title = title
        self.sessionID = sessionID
        screen = TerminalScreen(columns: columns, rows: rows)
        self.lifecycle = lifecycle
        self.purpose = purpose
        self.maintenanceCompletionToken =
            maintenanceCompletionToken
        maintenancePhase =
            purpose.maintenanceOperation == nil ? nil : .executing
    }

    var status: String {
        switch lifecycle {
        case .preparing:
            return "准备 Linux"
        case .opening:
            return "打开终端"
        case .running:
            if let inputNotice {
                return inputNotice
            }
            if let operation = purpose.maintenanceOperation,
               let maintenancePhase {
                switch maintenancePhase {
                case .executing:
                    return operation.runningStatus
                case .completed:
                    return "维护已完成"
                case let .failed(status):
                    return "维护失败（状态 \(status)）"
                case .interrupted:
                    return "维护已中断"
                }
            }
            return sawOutput ? "运行中" : "等待终端"
        case let .exited(description), let .failed(description):
            return description
        case .closing:
            return "关闭终端"
        }
    }

    var acceptsInput: Bool {
        guard sessionID != 0 else { return false }
        if case .running = lifecycle {
            return true
        }
        return false
    }

    var canReopenSession: Bool {
        switch lifecycle {
        case .exited, .failed:
            return true
        default:
            return false
        }
    }

    var closeProtection: WatchTerminalCloseProtection {
        guard maintenancePhase == .executing,
              let operation = purpose.maintenanceOperation else {
            return .standard
        }
        switch operation {
        case .updateIndex:
            return .softwareIndexUpdate
        case .upgradePackages:
            return .packageDatabaseWrite
        }
    }

    var snapshot: WatchTerminalSessionSnapshot {
        snapshot(includingScreen: true)
    }

    func snapshot(
        includingScreen: Bool
    ) -> WatchTerminalSessionSnapshot {
        let visibleLines = includingScreen ? screen.visibleLines : []
        let scrollbackLines =
            includingScreen && !screen.modes.usesAlternateScreen ?
            screen.scrollbackLines : []
        let renderedLines =
            screen.modes.usesAlternateScreen ?
            visibleLines : scrollbackLines + visibleLines
        return WatchTerminalSessionSnapshot(
            id: id,
            title: title,
            sessionID: sessionID,
            status: status,
            acceptsInput: acceptsInput,
            canReopenSession: canReopenSession,
            hasOutput: sawOutput,
            columns: screen.columns,
            rows: screen.rows,
            renderedLines: renderedLines,
            visibleLines: visibleLines,
            scrollbackLines: scrollbackLines,
            scrollbackLineCount: screen.scrollback.count,
            cursor: screen.cursor,
            modes: screen.modes,
            purpose: purpose,
            maintenancePhase: maintenancePhase,
            closeProtection: closeProtection)
    }

    mutating func beginOperation() -> UInt64 {
        operationGeneration &+= 1
        return operationGeneration
    }

    func matchesOperation(
        generation: UInt64,
        sessionID expectedSessionID: UInt64? = nil
    ) -> Bool {
        guard operationGeneration == generation else { return false }
        if let expectedSessionID {
            return sessionID == expectedSessionID
        }
        return true
    }

    mutating func appendOutput(_ bytes: [UInt8]) {
        guard !bytes.isEmpty else { return }
        screen.append(bytes)
        if let terminalTitle = screen.takePendingTitle(),
           purpose == .interactive {
            title = terminalTitle
        }
        drainTerminalResponses()
        observeMaintenanceOutput(bytes)
        sawOutput = true
    }

    mutating func reportDroppedBytes(_ count: UInt64) {
        guard count != 0 else { return }
        screen.reportDroppedBytes(count)
        maintenanceOutputProbe.removeAll(keepingCapacity: false)
        sawOutput = true
    }

    mutating func enqueue(_ bytes: [UInt8]) -> Bool {
        guard acceptsInput else { return false }
        guard pendingInput.count + bytes.count <= Self.inputLimit else {
            inputNotice = "输入队列已满"
            return false
        }
        pendingInput.append(contentsOf: bytes)
        inputNotice = nil
        return true
    }

    mutating func consumePendingInput(_ count: Int) {
        guard count > 0 else { return }
        pendingInput.removeFirst(min(count, pendingInput.count))
        if pendingInput.isEmpty {
            inputNotice = nil
        }
        drainTerminalResponses()
    }

    func cursorKey(_ finalByte: UInt8) -> [UInt8] {
        let introducer: UInt8 =
            screen.modes.usesApplicationCursorKeys ? 0x4f : 0x5b
        return [0x1b, introducer, finalByte]
    }

    mutating func beginOpening(
        purpose newPurpose: WatchTerminalSessionPurpose? = nil,
        maintenanceCompletionToken newCompletionToken: String? = nil
    ) {
        sessionID = 0
        pendingInput.removeAll(keepingCapacity: false)
        screen.discardPendingResponses()
        inputNotice = nil
        if let newPurpose {
            precondition(
                newPurpose.maintenanceOperation == nil ?
                    newCompletionToken == nil :
                    newCompletionToken.map(
                        WatchSoftwareMaintenanceCompletionProtocol
                            .isValidToken) == true)
            purpose = newPurpose
            maintenanceCompletionToken = newCompletionToken
            maintenancePhase =
                newPurpose.maintenanceOperation == nil ?
                    nil : .executing
            maintenanceOutputProbe.removeAll(
                keepingCapacity: false)
        }
        lifecycle = .opening
    }

    mutating func finishOpening(
        sessionID newSessionID: UInt64,
        createdColumns: Int,
        createdRows: Int,
        separatesPreviousOutput: Bool
    ) {
        sessionID = newSessionID
        lifecycle = .opening
        if separatesPreviousOutput && sawOutput {
            screen.carriageReturn()
            screen.lineFeed()
        }
        windowSizeNeedsUpdate =
            screen.columns != createdColumns ||
            screen.rows != createdRows
    }

    mutating func markRunning() {
        lifecycle = .running
    }

    mutating func markExited(_ description: String) {
        pendingInput.removeAll(keepingCapacity: false)
        screen.discardPendingResponses()
        inputNotice = nil
        markMaintenanceInterrupted()
        lifecycle = .exited(description)
    }

    mutating func markFailed(
        _ description: String,
        sessionID retainedSessionID: UInt64? = nil
    ) {
        if let retainedSessionID {
            sessionID = retainedSessionID
        }
        pendingInput.removeAll(keepingCapacity: false)
        screen.discardPendingResponses()
        inputNotice = nil
        markMaintenanceInterrupted()
        lifecycle = .failed(description)
    }

    mutating func resize(columns: Int, rows: Int) -> Bool {
        guard (1...Int(UInt16.max)).contains(columns),
              (1...Int(UInt16.max)).contains(rows) else {
            return false
        }
        screen.resize(columns: columns, rows: rows)
        switch lifecycle {
        case .opening, .running:
            windowSizeNeedsUpdate = sessionID != 0
        default:
            windowSizeNeedsUpdate = false
        }
        return true
    }

    @discardableResult
    mutating func clearScrollback() -> Int {
        // 只移除已滚出屏幕的本地历史，PTY 和活动屏状态保持不变。
        let removedLineCount = screen.scrollback.count
        screen.clearScrollback()
        return removedLineCount
    }

    private mutating func drainTerminalResponses() {
        let available = Self.inputLimit - pendingInput.count
        guard available > 0 else { return }
        pendingInput.append(contentsOf:
            screen.takePendingResponseBytes(maximumCount: available))
    }

    private mutating func observeMaintenanceOutput(
        _ bytes: [UInt8]
    ) {
        guard maintenancePhase == .executing,
              let operation = purpose.maintenanceOperation,
              let maintenanceCompletionToken else {
            return
        }
        maintenanceOutputProbe.append(contentsOf: bytes)
        if let status =
                WatchSoftwareMaintenanceCompletionProtocol.status(
                    in: maintenanceOutputProbe,
                    operation: operation,
                    token: maintenanceCompletionToken) {
            maintenancePhase =
                status == 0 ? .completed : .failed(status)
            maintenanceOutputProbe.removeAll(
                keepingCapacity: false)
            return
        }

        let maximumFrameByteCount =
            WatchSoftwareMaintenanceCompletionProtocol
                .maximumFrameByteCount(
                    operation: operation,
                    token: maintenanceCompletionToken)
        if maintenanceOutputProbe.count >= maximumFrameByteCount {
            maintenanceOutputProbe = Array(
                maintenanceOutputProbe.suffix(
                    maximumFrameByteCount - 1))
        }
    }

    private mutating func markMaintenanceInterrupted() {
        if maintenancePhase == .executing {
            maintenancePhase = .interrupted
        }
        maintenanceOutputProbe.removeAll(keepingCapacity: false)
    }
}

struct WatchTerminalSessions {
    private(set) var sessions: [WatchTerminalSession] = []
    private(set) var selectedSessionID: UUID?
    private var nextTitleNumber = 1
    private let initialColumns: Int
    private let initialRows: Int

    init(
        columns: Int = 40,
        rows: Int = 18,
        createsInitialSession: Bool = true
    ) {
        initialColumns = columns
        initialRows = rows
        if createsInitialSession {
            _ = add(lifecycle: .preparing)
        }
    }

    var activeSession: WatchTerminalSession? {
        guard let selectedSessionID else { return nil }
        return session(id: selectedSessionID)
    }

    var hasExecutingSoftwareMaintenance: Bool {
        sessions.contains {
            $0.purpose.maintenanceOperation != nil &&
                $0.maintenancePhase == .executing
        }
    }

    var snapshots: [WatchTerminalSessionSnapshot] {
        sessions.map {
            $0.snapshot(includingScreen: $0.id == selectedSessionID)
        }
    }

    func session(id: UUID) -> WatchTerminalSession? {
        sessions.first { $0.id == id }
    }

    @discardableResult
    mutating func add(
        title suppliedTitle: String? = nil,
        lifecycle: WatchTerminalSessionLifecycle = .opening,
        purpose: WatchTerminalSessionPurpose = .interactive,
        maintenanceCompletionToken: String? = nil
    ) -> UUID? {
        let defaultTitle = "终端 \(nextTitleNumber)"
        nextTitleNumber += 1
        let trimmedTitle = suppliedTitle?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let title: String
        if let trimmedTitle, !trimmedTitle.isEmpty {
            title = trimmedTitle
        } else {
            title = defaultTitle
        }
        let session = WatchTerminalSession(
            title: title,
            columns: initialColumns,
            rows: initialRows,
            lifecycle: lifecycle,
            purpose: purpose,
            maintenanceCompletionToken:
                maintenanceCompletionToken)
        sessions.append(session)
        selectedSessionID = session.id
        return session.id
    }

    @discardableResult
    mutating func select(_ id: UUID) -> Bool {
        guard sessions.contains(where: { $0.id == id }) else {
            return false
        }
        selectedSessionID = id
        return true
    }

    @discardableResult
    mutating func update(
        _ id: UUID,
        _ body: (inout WatchTerminalSession) -> Void
    ) -> Bool {
        guard let index = sessions.firstIndex(
                where: { $0.id == id }) else {
            return false
        }
        body(&sessions[index])
        return true
    }

    @discardableResult
    mutating func rename(_ id: UUID, title: String) -> Bool {
        let trimmed = title.trimmingCharacters(
            in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return false }
        return update(id) { $0.title = trimmed }
    }

    @discardableResult
    mutating func clearScrollback(_ id: UUID) -> Int? {
        guard let index = sessions.firstIndex(
                where: { $0.id == id }) else {
            return nil
        }
        return sessions[index].clearScrollback()
    }

    @discardableResult
    mutating func remove(_ id: UUID) -> WatchTerminalSession? {
        guard let index = sessions.firstIndex(
                where: { $0.id == id }) else {
            return nil
        }
        let removed = sessions.remove(at: index)
        if selectedSessionID == id {
            if sessions.isEmpty {
                selectedSessionID = nil
            } else {
                selectedSessionID =
                    sessions[min(index, sessions.count - 1)].id
            }
        }
        return removed
    }
}

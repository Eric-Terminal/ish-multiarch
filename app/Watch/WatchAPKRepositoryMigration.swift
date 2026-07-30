import Foundation

enum WatchAPKRepositoryMigrationStatus: Equatable, Sendable {
    case migrationRequired(
        installedVersion: Int,
        targetVersion: Int)
    case current(version: Int)
    case newerThanApp(
        installedVersion: Int,
        targetVersion: Int)
}

enum WatchAPKRepositoryMigrationOutcome: Equatable, Sendable {
    case skipped(WatchAPKRepositoryMigrationStatus)
    case repositoriesPrepared(
        status: WatchAPKRepositoryMigrationStatus,
        changed: Bool)
    case migrationCompleted(
        previousVersion: Int,
        targetVersion: Int,
        repositoriesChanged: Bool)
    case repositoriesSynchronized(
        version: Int,
        repositoriesChanged: Bool,
        markerChanged: Bool)
}

enum WatchAPKRepositoriesTemplateError: LocalizedError, Equatable {
    case missingBundledTemplate
    case empty
    case tooLarge
    case invalidUTF8
    case missingTrailingNewline
    case alreadyManaged
    case invalidRepository(line: Int)

    var errorDescription: String? {
        switch self {
        case .missingBundledTemplate:
            return "App 中缺少 aarch64-repositories.txt"
        case .empty:
            return "软件仓库模板不能为空"
        case .tooLarge:
            return "软件仓库模板超过大小上限"
        case .invalidUTF8:
            return "软件仓库模板不是有效的 UTF-8"
        case .missingTrailingNewline:
            return "软件仓库模板末尾缺少换行"
        case .alreadyManaged:
            return "软件仓库模板不应包含 iSH 托管说明"
        case let .invalidRepository(line):
            return "软件仓库模板第 \(line) 行不是有效的 HTTP(S) 仓库地址"
        }
    }
}

struct WatchAPKRepositoriesTemplate: Equatable, Sendable {
    static let maximumByteCount = 64 * 1024

    static let managedHeader =
        "# This file contains pinned repositories managed by iSH. " +
        "If the /ish directory\n" +
        "# exists, iSH uses the metadata stored in it to keep this " +
        "file up to date (by\n" +
        "# overwriting the contents on boot.)\n"
    static let maximumManagedByteCount =
        maximumByteCount + managedHeader.utf8.count

    let data: Data

    init(validating data: Data) throws {
        guard !data.isEmpty else {
            throw WatchAPKRepositoriesTemplateError.empty
        }
        guard data.count <= Self.maximumByteCount else {
            throw WatchAPKRepositoriesTemplateError.tooLarge
        }
        guard let contents = String(data: data, encoding: .utf8) else {
            throw WatchAPKRepositoriesTemplateError.invalidUTF8
        }
        guard contents.hasSuffix("\n") else {
            throw WatchAPKRepositoriesTemplateError
                .missingTrailingNewline
        }
        guard !contents.hasPrefix(Self.managedHeader) else {
            throw WatchAPKRepositoriesTemplateError.alreadyManaged
        }

        var repositoryCount = 0
        for (index, line) in contents.split(
            separator: "\n",
            omittingEmptySubsequences: false
        ).dropLast().enumerated() {
            if line.isEmpty || line.hasPrefix("#") {
                continue
            }
            guard Self.isValidRepository(String(line)) else {
                throw WatchAPKRepositoriesTemplateError
                    .invalidRepository(line: index + 1)
            }
            repositoryCount += 1
        }
        guard repositoryCount > 0 else {
            throw WatchAPKRepositoriesTemplateError.empty
        }
        self.data = data
    }

    init(contentsOf url: URL) throws {
        try self.init(validating: Data(contentsOf: url))
    }

    static func bundled(
        in bundle: Bundle = .main,
        resourceName: String = "aarch64-repositories"
    ) throws -> Self {
        guard let url = bundle.url(
                forResource: resourceName,
                withExtension: "txt") else {
            throw WatchAPKRepositoriesTemplateError
                .missingBundledTemplate
        }
        return try Self(contentsOf: url)
    }

    var managedData: Data {
        var result = Data(Self.managedHeader.utf8)
        result.append(data)
        return result
    }

    private static func isValidRepository(_ line: String) -> Bool {
        guard line == line.trimmingCharacters(
                in: .whitespacesAndNewlines),
              !line.unicodeScalars.contains(where: {
                  CharacterSet.whitespacesAndNewlines.contains($0) ||
                      CharacterSet.controlCharacters.contains($0)
              }),
              let components = URLComponents(string: line),
              let scheme = components.scheme?.lowercased(),
              scheme == "http" || scheme == "https",
              components.host?.isEmpty == false,
              components.user == nil,
              components.password == nil,
              components.query == nil,
              components.fragment == nil else {
            return false
        }
        return true
    }
}

enum WatchAPKRepositoryMigrationError: LocalizedError {
    case rollbackFailed(
        writeError: String,
        rollbackError: String)

    var errorDescription: String? {
        switch self {
        case let .rollbackFailed(writeError, rollbackError):
            return "软件仓库更新失败（\(writeError)），恢复原文件也失败" +
                "（\(rollbackError)）"
        }
    }
}

protocol WatchAPKRepositoryGuestFiles: AnyObject {
    // 实现应绑定到一个由 WatchRootStore 选出的受管 guest root。
    // 适配器必须经过 guest 文件语义，不能把 fakefs 的宿主 data 目录
    // 当作普通目录直接增删文件，否则不会同步 meta.db。
    func dataIfPresent(atGuestPath path: String) throws -> Data?

    // nil 表示删除；单次调用必须原子提交，抛错表示目标没有改变。
    func replaceAtomically(
        atGuestPath path: String,
        with data: Data?
    ) throws
}

enum WatchAPKRepositoryGuestFileError: LocalizedError, Equatable {
    case unsupportedPath(String)
    case fileTooLarge(path: String, maximumByteCount: Int)
    case readFailed(path: String, linuxErrno: Int)
    case replaceFailed(path: String, linuxErrno: Int)

    var errorDescription: String? {
        switch self {
        case let .unsupportedPath(path):
            return "不允许访问 guest 文件：\(path)"
        case let .fileTooLarge(path, maximumByteCount):
            return "guest 文件 \(path) 超过读取上限 " +
                "\(maximumByteCount) 字节"
        case let .readFailed(path, linuxErrno):
            return "读取 guest 文件 \(path) 失败" +
                "（Linux errno \(linuxErrno)）"
        case let .replaceFailed(path, linuxErrno):
            return "替换 guest 文件 \(path) 失败" +
                "（Linux errno \(linuxErrno)）"
        }
    }
}

final class WatchAPKRepositoryGuestFileStore:
    WatchAPKRepositoryGuestFiles
{
    typealias ReadOperation = (
        _ fileID: Int32,
        _ buffer: UnsafeMutableRawPointer?,
        _ capacity: Int
    ) -> Int
    typealias ReplaceOperation = (
        _ fileID: Int32,
        _ bytes: UnsafeRawPointer?,
        _ length: Int,
        _ removeFile: Int32
    ) -> Int32

    private let repositoriesFileID: Int32
    private let apkVersionFileID: Int32
    private let readOperation: ReadOperation
    private let replaceOperation: ReplaceOperation

    init(
        repositoriesFileID: Int32,
        apkVersionFileID: Int32,
        read: @escaping ReadOperation,
        replace: @escaping ReplaceOperation
    ) {
        self.repositoriesFileID = repositoriesFileID
        self.apkVersionFileID = apkVersionFileID
        readOperation = read
        replaceOperation = replace
    }

#if os(watchOS)
    convenience init() {
        self.init(
            repositoriesFileID: Int32(
                ISH_WATCH_GUEST_FILE_REPOSITORIES.rawValue),
            apkVersionFileID: Int32(
                ISH_WATCH_GUEST_FILE_APK_VERSION.rawValue),
            read: { fileID, buffer, capacity in
                Int(ish_watch_guest_file_read(
                    fileID, buffer, capacity))
            },
            replace: { fileID, bytes, length, removeFile in
                ish_watch_guest_file_replace(
                    fileID, bytes, length, removeFile)
            })
    }
#endif

    func dataIfPresent(
        atGuestPath path: String
    ) throws -> Data? {
        let mapping = try fileMapping(forExactPath: path)
        var data = Data(count: mapping.maximumByteCount)
        let result = data.withUnsafeMutableBytes {
            readOperation(
                mapping.fileID,
                $0.baseAddress,
                $0.count)
        }
        if result == -Int(ENOENT) {
            return nil
        }
        if result == -Int(EFBIG) {
            throw WatchAPKRepositoryGuestFileError.fileTooLarge(
                path: path,
                maximumByteCount: mapping.maximumByteCount)
        }
        guard result >= 0 else {
            throw WatchAPKRepositoryGuestFileError.readFailed(
                path: path,
                linuxErrno: -result)
        }
        guard result <= mapping.maximumByteCount else {
            throw WatchAPKRepositoryGuestFileError.fileTooLarge(
                path: path,
                maximumByteCount: mapping.maximumByteCount)
        }
        data.count = result
        return data
    }

    func replaceAtomically(
        atGuestPath path: String,
        with data: Data?
    ) throws {
        let mapping = try fileMapping(forExactPath: path)
        if let data,
           data.count > mapping.maximumByteCount {
            throw WatchAPKRepositoryGuestFileError.fileTooLarge(
                path: path,
                maximumByteCount: mapping.maximumByteCount)
        }

        let result: Int32
        if let data {
            result = data.withUnsafeBytes {
                replaceOperation(
                    mapping.fileID,
                    $0.baseAddress,
                    $0.count,
                    0)
            }
        } else {
            result = replaceOperation(
                mapping.fileID, nil, 0, 1)
        }
        guard result == 0 else {
            throw WatchAPKRepositoryGuestFileError.replaceFailed(
                path: path,
                linuxErrno: Int(result < 0 ? -result : result))
        }
    }

    private func fileMapping(
        forExactPath path: String
    ) throws -> (fileID: Int32, maximumByteCount: Int) {
        switch path {
        case WatchAPKRepositoryMigrationService.repositoriesPath:
#if os(watchOS)
            let maximumByteCount =
                Int(ISH_WATCH_REPOSITORIES_LIMIT)
#else
            let maximumByteCount =
                WatchAPKRepositoriesTemplate.maximumManagedByteCount
#endif
            return (
                repositoriesFileID,
                maximumByteCount)
        case WatchAPKRepositoryMigrationService.apkVersionPath:
#if os(watchOS)
            let maximumByteCount =
                Int(ISH_WATCH_APK_VERSION_LIMIT)
#else
            let maximumByteCount =
                WatchAPKRepositoryMigrationService
                    .maximumMarkerByteCount
#endif
            return (
                apkVersionFileID,
                maximumByteCount)
        default:
            throw WatchAPKRepositoryGuestFileError
                .unsupportedPath(path)
        }
    }
}

struct WatchAPKRepositoryMigrationCoordinator: Equatable {
    enum Activity: Equatable {
        case checking
        case idle
        case preparingRepositories
        case upgrading(
            sessionID: UUID,
            finishesMigration: Bool)
        case finishingMigration(sessionID: UUID)
    }

    enum UpgradePhase: Equatable {
        case executing
        case completed
        case failed(Int)
        case interrupted
    }

    private(set) var status: WatchAPKRepositoryMigrationStatus?
    private(set) var activity = Activity.checking
    private(set) var errorMessage: String?

    var isBusy: Bool {
        activity != .idle
    }

    var canStartPackageUpgrade: Bool {
        activity == .idle
    }

    var upgradeSessionID: UUID? {
        switch activity {
        case let .upgrading(sessionID, _),
             let .finishingMigration(sessionID):
            return sessionID
        default:
            return nil
        }
    }

    var installedVersion: Int? {
        switch status {
        case let .migrationRequired(installedVersion, _),
             let .newerThanApp(installedVersion, _):
            return installedVersion
        case let .current(version):
            return version
        case nil:
            return nil
        }
    }

    mutating func beginChecking() {
        status = nil
        activity = .checking
        errorMessage = nil
    }

    mutating func recordInspection(
        _ status: WatchAPKRepositoryMigrationStatus
    ) {
        self.status = status
        activity = .idle
        errorMessage = nil
    }

    @discardableResult
    mutating func beginUpgradePreparation() -> Bool {
        guard activity == .idle else { return false }
        activity = .preparingRepositories
        errorMessage = nil
        return true
    }

    mutating func recordPreparedUpgrade(
        sessionID: UUID,
        outcome: WatchAPKRepositoryMigrationOutcome
    ) {
        guard activity == .preparingRepositories else { return }
        status = outcome.resultingStatus
        activity = .upgrading(
            sessionID: sessionID,
            finishesMigration: outcome.requiresMigrationFinish)
        errorMessage = nil
    }

    mutating func recordPreparedUpgradeFailure(
        outcome: WatchAPKRepositoryMigrationOutcome,
        message: String
    ) {
        guard activity == .preparingRepositories else { return }
        status = outcome.resultingStatus
        activity = .idle
        errorMessage = message
    }

    // true 只会在匹配 UUID 的升级会话首次成功时返回。
    @discardableResult
    mutating func observeMaintenance(
        sessionID: UUID,
        phase: UpgradePhase
    ) -> Bool {
        guard case let .upgrading(
                expectedSessionID,
                finishesMigration) = activity,
              expectedSessionID == sessionID else {
            return false
        }
        switch phase {
        case .completed:
            if finishesMigration {
                activity = .finishingMigration(
                    sessionID: sessionID)
                errorMessage = nil
                return true
            }
            activity = .idle
            errorMessage = nil
        case let .failed(status):
            activity = .idle
            errorMessage = finishesMigration ?
                "软件升级失败（状态 \(status)），迁移未完成" :
                "软件升级失败（状态 \(status)）"
        case .interrupted:
            activity = .idle
            errorMessage = finishesMigration ?
                "软件升级已中断，迁移未完成" :
                "软件升级已中断"
        case .executing:
            break
        }
        return false
    }

    mutating func recordSessionClosed(_ sessionID: UUID) {
        guard case let .upgrading(
                expectedSessionID,
                finishesMigration) = activity,
              expectedSessionID == sessionID else {
            return
        }
        activity = .idle
        errorMessage = finishesMigration ?
            "软件升级已中断，迁移未完成" :
            "软件升级已中断"
    }

    mutating func recordMigrationFinished(
        sessionID: UUID,
        outcome: WatchAPKRepositoryMigrationOutcome
    ) {
        guard case let .finishingMigration(
                expectedSessionID) = activity,
              expectedSessionID == sessionID else {
            return
        }
        status = outcome.resultingStatus
        activity = .idle
        errorMessage = nil
    }

    mutating func recordMigrationFinishFailure(
        sessionID: UUID,
        message: String
    ) {
        guard case let .finishingMigration(
                expectedSessionID) = activity,
              expectedSessionID == sessionID else {
            return
        }
        activity = .idle
        errorMessage = message
    }

    mutating func recordFailure(
        _ message: String,
        preserving status: WatchAPKRepositoryMigrationStatus? = nil
    ) {
        if let status {
            self.status = status
        }
        activity = .idle
        errorMessage = message
    }

    mutating func recordRuntimeUnavailable(_ message: String) {
        switch activity {
        case .finishingMigration:
            // 已确认 status 0 的短小提交允许继续完成。
            return
        case let .upgrading(_, finishesMigration):
            activity = .idle
            errorMessage = finishesMigration ?
                "\(message)；软件升级已中断，迁移未完成" :
                "\(message)；软件升级已中断"
        case .checking, .preparingRepositories, .idle:
            activity = .idle
            errorMessage = message
        }
    }
}

enum WatchAPKRepositoryStatusIndicator: Equatable, Sendable {
    case current
    case working
    case migrationRequired
    case error
}

struct WatchAPKRepositoryStatusPresentation: Equatable, Sendable {
    let indicator: WatchAPKRepositoryStatusIndicator
    let accessibilitySummary: String

    init(coordinator: WatchAPKRepositoryMigrationCoordinator) {
        if let errorMessage = coordinator.errorMessage {
            indicator = .error
            accessibilitySummary = "软件仓库错误：\(errorMessage)"
            return
        }

        switch coordinator.activity {
        case .checking:
            indicator = .working
            accessibilitySummary = "正在检查软件仓库状态"
            return
        case .preparingRepositories:
            indicator = .working
            accessibilitySummary = "正在准备软件仓库迁移"
            return
        case .upgrading:
            indicator = .working
            accessibilitySummary = "软件升级正在运行"
            return
        case .finishingMigration:
            indicator = .working
            accessibilitySummary = "正在记录软件仓库迁移结果"
            return
        case .idle:
            break
        }

        switch coordinator.status {
        case .migrationRequired:
            indicator = .migrationRequired
            accessibilitySummary = "软件仓库需要迁移"
        case .current:
            indicator = .current
            accessibilitySummary = "软件仓库已是当前版本"
        case .newerThanApp:
            indicator = .current
            accessibilitySummary = "软件仓库版本比 App 更新，已安全保留"
        case nil:
            indicator = .working
            accessibilitySummary = "软件仓库状态尚未就绪"
        }
    }
}

struct WatchAPKRepositoryMigrationService {
    static let currentVersion = 32_400
    static let currentVersionName = "Alpine v3.24"
    static let apkVersionPath = "/ish/apk-version"
    static let repositoriesPath = "/etc/apk/repositories"
    static let maximumMarkerByteCount = 999

    let targetVersion: Int
    private let guestFiles: WatchAPKRepositoryGuestFiles

    private enum CommitResult {
        case newerThanApp(installedVersion: Int)
        case committed(repositories: Bool, marker: Bool)
    }

    init(
        targetVersion: Int =
            WatchAPKRepositoryMigrationService.currentVersion,
        guestFiles: WatchAPKRepositoryGuestFiles
    ) {
        precondition(targetVersion > 0)
        self.targetVersion = targetVersion
        self.guestFiles = guestFiles
    }

    func status() throws -> WatchAPKRepositoryMigrationStatus {
        let installedVersion = try readVersion(
            atGuestPath: Self.apkVersionPath)
        if installedVersion < targetVersion {
            return .migrationRequired(
                installedVersion: installedVersion,
                targetVersion: targetVersion)
        }
        if installedVersion == targetVersion {
            return .current(version: installedVersion)
        }
        return .newerThanApp(
            installedVersion: installedVersion,
            targetVersion: targetVersion)
    }

    // 与 iOS 的升级前阶段一致：只发布新仓库，版本标记仍保持旧值。
    func prepareRepositories(
        template: WatchAPKRepositoriesTemplate
    ) throws -> WatchAPKRepositoryMigrationOutcome {
        let migrationStatus = try status()
        switch migrationStatus {
        case .newerThanApp:
            return .skipped(migrationStatus)
        case .migrationRequired:
            let changed = try replaceRepositoriesIfNeeded(
                with: template.managedData)
            return .repositoriesPrepared(
                status: migrationStatus,
                changed: changed)
        case let .current(version):
            let changed = try replaceRepositoriesIfNeeded(
                with: template.managedData)
            return .repositoriesSynchronized(
                version: version,
                repositoriesChanged: changed,
                markerChanged: false)
        }
    }

    // 只应在 APK 升级成功后调用；apk-version 是最后提交的事务记录。
    // 中途退出只会保留“新仓库、旧标记”的可重试准备状态。
    func finishMigration(
        template: WatchAPKRepositoriesTemplate
    ) throws -> WatchAPKRepositoryMigrationOutcome {
        let migrationStatus = try status()
        switch migrationStatus {
        case .newerThanApp:
            return .skipped(migrationStatus)
        case let .migrationRequired(installedVersion, _):
            let result = try commitRepositoriesAndMarker(
                repositories: template.managedData)
            switch result {
            case let .newerThanApp(newerVersion):
                return .skipped(.newerThanApp(
                    installedVersion: newerVersion,
                    targetVersion: targetVersion))
            case let .committed(repositoriesChanged, _):
                return .migrationCompleted(
                    previousVersion: installedVersion,
                    targetVersion: targetVersion,
                    repositoriesChanged: repositoriesChanged)
            }
        case let .current(version):
            let result = try commitRepositoriesAndMarker(
                repositories: template.managedData)
            switch result {
            case let .newerThanApp(newerVersion):
                return .skipped(.newerThanApp(
                    installedVersion: newerVersion,
                    targetVersion: targetVersion))
            case let .committed(
                    repositoriesChanged,
                    markerChanged):
                return .repositoriesSynchronized(
                    version: version,
                    repositoriesChanged: repositoriesChanged,
                    markerChanged: markerChanged)
            }
        }
    }

    private func replaceRepositoriesIfNeeded(
        with repositories: Data
    ) throws -> Bool {
        guard try guestFiles.dataIfPresent(
            atGuestPath: Self.repositoriesPath) !=
                repositories else {
            return false
        }
        try guestFiles.replaceAtomically(
            atGuestPath: Self.repositoriesPath,
            with: repositories)
        return true
    }

    private func commitRepositoriesAndMarker(
        repositories: Data
    ) throws -> CommitResult {
        let oldRepositories = try guestFiles.dataIfPresent(
            atGuestPath: Self.repositoriesPath)
        let oldMarker = try guestFiles.dataIfPresent(
            atGuestPath: Self.apkVersionPath)
        let observedVersion = version(from: oldMarker)
        if observedVersion > targetVersion {
            return .newerThanApp(
                installedVersion: observedVersion)
        }
        let marker = Data("\(targetVersion)\n".utf8)
        let repositoriesChanged = oldRepositories != repositories
        let markerChanged = oldMarker != marker

        guard repositoriesChanged || markerChanged else {
            return .committed(
                repositories: false,
                marker: false)
        }

        var repositoriesCommitted = false
        do {
            if repositoriesChanged {
                try guestFiles.replaceAtomically(
                    atGuestPath: Self.repositoriesPath,
                    with: repositories)
                repositoriesCommitted = true
            }
            if markerChanged {
                try guestFiles.replaceAtomically(
                    atGuestPath: Self.apkVersionPath,
                    with: marker)
            }
        } catch {
            guard repositoriesCommitted else {
                throw error
            }
            do {
                try guestFiles.replaceAtomically(
                    atGuestPath: Self.repositoriesPath,
                    with: oldRepositories)
            } catch let rollbackError {
                throw WatchAPKRepositoryMigrationError.rollbackFailed(
                    writeError: error.localizedDescription,
                    rollbackError:
                        rollbackError.localizedDescription)
            }
            throw error
        }
        return .committed(
            repositories: repositoriesChanged,
            marker: markerChanged)
    }

    private func readVersion(atGuestPath path: String) throws -> Int {
        guard let data = try guestFiles.dataIfPresent(
                  atGuestPath: path) else { return 0 }
        return version(from: data)
    }

    private func version(from data: Data?) -> Int {
        guard let data,
              data.count <= Self.maximumMarkerByteCount,
              let contents = String(data: data, encoding: .utf8) else {
            return 0
        }
        let trimmed = contents.trimmingCharacters(
            in: .whitespacesAndNewlines)
        return Int(trimmed) ?? 0
    }
}

private extension WatchAPKRepositoryMigrationOutcome {
    var resultingStatus: WatchAPKRepositoryMigrationStatus {
        switch self {
        case let .skipped(status),
             let .repositoriesPrepared(status, _):
            return status
        case let .migrationCompleted(_, targetVersion, _):
            return .current(version: targetVersion)
        case let .repositoriesSynchronized(version, _, _):
            return .current(version: version)
        }
    }

    var requiresMigrationFinish: Bool {
        guard case let .repositoriesPrepared(status, _) = self,
              case .migrationRequired = status else {
            return false
        }
        return true
    }
}

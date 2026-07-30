import Foundation

private func require(
    _ condition: Bool,
    _ message: String
) {
    if !condition {
        FileHandle.standardError.write(
            Data("失败：\(message)\n".utf8))
        exit(1)
    }
}

private enum InjectedFailure: Error {
    case requested
}

private final class RecordingGuestFileStore:
    WatchAPKRepositoryGuestFiles
{
    private(set) var files: [String: Data]
    private(set) var replacementAttempts: [String] = []
    var failingReplacementPath: String?
    var apkVersionMutationOnRead:
        (readNumber: Int, contents: String)?
    private var didInjectFailure = false
    private var apkVersionReadCount = 0

    init(
        apkVersion: String? = nil,
        repositories: String? = "旧仓库\n"
    ) {
        var files: [String: Data] = [:]
        if let apkVersion {
            files[WatchAPKRepositoryMigrationService.apkVersionPath] =
                Data(apkVersion.utf8)
        }
        if let repositories {
            files[WatchAPKRepositoryMigrationService.repositoriesPath] =
                Data(repositories.utf8)
        }
        self.files = files
    }

    func dataIfPresent(
        atGuestPath path: String
    ) throws -> Data? {
        if path == WatchAPKRepositoryMigrationService.apkVersionPath {
            apkVersionReadCount += 1
            if let mutation = apkVersionMutationOnRead,
               mutation.readNumber == apkVersionReadCount {
                files[path] = Data(mutation.contents.utf8)
            }
        }
        return files[path]
    }

    func replaceAtomically(
        atGuestPath path: String,
        with data: Data?
    ) throws {
        replacementAttempts.append(path)
        if path == failingReplacementPath && !didInjectFailure {
            didInjectFailure = true
            throw InjectedFailure.requested
        }
        files[path] = data
    }

    func string(at path: String) -> String? {
        guard let data = files[path] else { return nil }
        return String(data: data, encoding: .utf8)
    }

    func resetRecording() {
        replacementAttempts = []
    }
}

private final class AdapterBackend {
    struct ReadCall: Equatable {
        let fileID: Int32
        let capacity: Int
    }

    struct ReplaceCall: Equatable {
        let fileID: Int32
        let data: Data?
        let removeFile: Int32
    }

    var files: [Int32: Data] = [:]
    var readFailures: [Int32: Int] = [:]
    var replaceFailures: [Int32: Int32] = [:]
    var oversizedReadFileID: Int32?
    private(set) var readCalls: [ReadCall] = []
    private(set) var replaceCalls: [ReplaceCall] = []

    func read(
        fileID: Int32,
        buffer: UnsafeMutableRawPointer?,
        capacity: Int
    ) -> Int {
        readCalls.append(ReadCall(
            fileID: fileID,
            capacity: capacity))
        if let linuxErrno = readFailures[fileID] {
            return -linuxErrno
        }
        if oversizedReadFileID == fileID {
            return capacity + 1
        }
        guard let data = files[fileID] else {
            return -Int(ENOENT)
        }
        guard data.count <= capacity, let buffer else {
            return -Int(EFBIG)
        }
        data.copyBytes(
            to: buffer.assumingMemoryBound(to: UInt8.self),
            count: data.count)
        return data.count
    }

    func replace(
        fileID: Int32,
        bytes: UnsafeRawPointer?,
        length: Int,
        removeFile: Int32
    ) -> Int32 {
        let data: Data?
        if removeFile != 0 {
            data = nil
        } else if length == 0 {
            data = Data()
        } else {
            data = Data(bytes: bytes!, count: length)
        }
        replaceCalls.append(ReplaceCall(
            fileID: fileID,
            data: data,
            removeFile: removeFile))
        if let failure = replaceFailures[fileID] {
            return -failure
        }
        files[fileID] = data
        return 0
    }
}

@main
private struct WatchAPKRepositoryMigrationTest {
    private static let targetVersion =
        WatchAPKRepositoryMigrationService.currentVersion
    private static let templateText =
        "https://dl-cdn.alpinelinux.org/alpine/v3.24/main\n" +
        "https://dl-cdn.alpinelinux.org/alpine/v3.24/community\n"

    static func main() {
        do {
            try testTemplateValidation()
            try testStatus()
            try testPreparation()
            try testMigrationCompletion()
            try testFutureMarkerRace()
            try testAtomicFailures()
            try testCAdapterMappingAndErrors()
            try testUpgradeOrchestration()
            testCoordinatorFailureSemantics()
            testStatusPresentation()
            print("Watch APK 仓库迁移回归通过")
        } catch {
            FileHandle.standardError.write(
                Data("失败：\(error)\n".utf8))
            exit(1)
        }
    }

    private static func makeTemplate() throws ->
        WatchAPKRepositoriesTemplate
    {
        try WatchAPKRepositoriesTemplate(
            validating: Data(templateText.utf8))
    }

    private static func makeService(
        files: RecordingGuestFileStore
    ) -> WatchAPKRepositoryMigrationService {
        WatchAPKRepositoryMigrationService(
            targetVersion: targetVersion,
            guestFiles: files)
    }

    private static func requireTemplateError(
        _ expected: WatchAPKRepositoriesTemplateError,
        _ contents: Data
    ) {
        do {
            _ = try WatchAPKRepositoriesTemplate(
                validating: contents)
            require(false, "模板应被拒绝：\(expected)")
        } catch let error as WatchAPKRepositoriesTemplateError {
            require(error == expected, "模板错误不匹配：\(error)")
        } catch {
            require(false, "模板返回了意外错误：\(error)")
        }
    }

    private static func testTemplateValidation() throws {
        let template = try makeTemplate()
        let expected =
            WatchAPKRepositoriesTemplate.managedHeader +
            templateText
        require(
            String(data: template.managedData, encoding: .utf8) ==
                expected,
            "托管文件必须逐字沿用 iOS 说明并追加随包模板")

        requireTemplateError(.empty, Data())
        requireTemplateError(
            .invalidUTF8,
            Data([0xff, 0xfe, 0xfd]))
        requireTemplateError(
            .missingTrailingNewline,
            Data("https://example.com/alpine/main".utf8))
        requireTemplateError(
            .alreadyManaged,
            Data((
                WatchAPKRepositoriesTemplate.managedHeader +
                "https://example.com/alpine/main\n"
            ).utf8))
        requireTemplateError(
            .invalidRepository(line: 1),
            Data("file:///tmp/alpine\n".utf8))
        requireTemplateError(
            .invalidRepository(line: 1),
            Data("https://example.com/alpine main\n".utf8))
        requireTemplateError(
            .tooLarge,
            Data(
                repeating: UInt8(ascii: "a"),
                count:
                    WatchAPKRepositoriesTemplate.maximumByteCount + 1))

        let comments = try WatchAPKRepositoriesTemplate(
            validating: Data((
                "# 固定镜像\n\n" +
                "https://example.com/alpine/main\n"
            ).utf8))
        require(
            comments.data.count > 0,
            "模板应允许注释和空行")
    }

    private static func testStatus() throws {
        require(
            WatchAPKRepositoryMigrationService.currentVersionName ==
                "Alpine v3.24",
            "迁移版本名称必须与当前 AArch64 seed 一致")

        let missingMarker = RecordingGuestFileStore()
        require(
            try makeService(files: missingMarker).status() ==
                .migrationRequired(
                    installedVersion: 0,
                    targetVersion: targetVersion),
            "受管 Watch root 缺少 apk-version 时应按版本 0 迁移")

        let malformedMarker = RecordingGuestFileStore(
            apkVersion: "不是版本\n")
        require(
            try makeService(files: malformedMarker).status() ==
                .migrationRequired(
                    installedVersion: 0,
                    targetVersion: targetVersion),
            "无效 apk-version 应与 iOS 一样按版本 0 处理")

        let current = RecordingGuestFileStore(
            apkVersion: "\(targetVersion)\n")
        require(
            try makeService(files: current).status() ==
                .current(version: targetVersion),
            "相同版本应处于当前状态")

        let newer = RecordingGuestFileStore(
            apkVersion: "\(targetVersion + 1)\n")
        require(
            try makeService(files: newer).status() ==
                .newerThanApp(
                    installedVersion: targetVersion + 1,
                    targetVersion: targetVersion),
            "较新文件系统必须显式报告，不能被旧 App 降级")
    }

    private static func testPreparation() throws {
        let template = try makeTemplate()
        let expectedRepositories =
            WatchAPKRepositoriesTemplate.managedHeader +
            templateText
        let files = RecordingGuestFileStore(
            apkVersion: "31900\n")
        let service = makeService(files: files)

        let outcome = try service.prepareRepositories(
            template: template)
        require(
            outcome == .repositoriesPrepared(
                status: .migrationRequired(
                    installedVersion: 31_900,
                    targetVersion: targetVersion),
                changed: true),
            "升级前应明确报告仓库已准备")
        require(
            files.string(
                at: WatchAPKRepositoryMigrationService
                    .repositoriesPath) == expectedRepositories,
            "升级前应原子发布托管仓库")
        require(
            files.string(
                at: WatchAPKRepositoryMigrationService
                    .apkVersionPath) == "31900\n",
            "升级前绝不能提前推进版本标记")
        require(
            files.replacementAttempts == [
                WatchAPKRepositoryMigrationService.repositoriesPath,
            ],
            "准备阶段只能替换 repositories")

        files.resetRecording()
        let repeated = try service.prepareRepositories(
            template: template)
        require(
            repeated == .repositoriesPrepared(
                status: .migrationRequired(
                    installedVersion: 31_900,
                    targetVersion: targetVersion),
                changed: false),
            "重复准备相同仓库应为幂等操作")
        require(
            files.replacementAttempts.isEmpty,
            "仓库未变化时不应重复写入")

        let currentFiles = RecordingGuestFileStore(
            apkVersion: "\(targetVersion)\n")
        let currentOutcome = try makeService(
            files: currentFiles
        ).prepareRepositories(template: template)
        require(
            currentOutcome == .repositoriesSynchronized(
                version: targetVersion,
                repositoriesChanged: true,
                markerChanged: false),
            "当前版本应与 iOS 一样静默重铺随包仓库")
        require(
            currentFiles.replacementAttempts == [
                WatchAPKRepositoryMigrationService.repositoriesPath,
            ],
            "静默同步不能重写版本标记")

        let newerFiles = RecordingGuestFileStore(
            apkVersion: "\(targetVersion + 1)\n")
        let newerRepositories = newerFiles.string(
            at: WatchAPKRepositoryMigrationService.repositoriesPath)
        require(
            try makeService(files: newerFiles)
                .prepareRepositories(template: template) ==
                .skipped(
                    .newerThanApp(
                        installedVersion: targetVersion + 1,
                        targetVersion: targetVersion)),
            "旧 App 应跳过较新版本的文件系统")
        require(
            newerFiles.string(
                at: WatchAPKRepositoryMigrationService
                    .repositoriesPath) == newerRepositories &&
                newerFiles.replacementAttempts.isEmpty,
            "跳过较新文件系统时不能降级仓库")
    }

    private static func testMigrationCompletion() throws {
        let template = try makeTemplate()
        let expectedRepositories =
            WatchAPKRepositoriesTemplate.managedHeader +
            templateText
        let files = RecordingGuestFileStore(
            apkVersion: "31900\n")
        let service = makeService(files: files)

        let outcome = try service.finishMigration(
            template: template)
        require(
            outcome == .migrationCompleted(
                previousVersion: 31_900,
                targetVersion: targetVersion,
                repositoriesChanged: true),
            "成功迁移应报告前后版本")
        require(
            files.string(
                at: WatchAPKRepositoryMigrationService
                    .repositoriesPath) == expectedRepositories,
            "成功迁移应提交新仓库")
        require(
            files.string(
                at: WatchAPKRepositoryMigrationService
                    .apkVersionPath) == "\(targetVersion)\n",
            "只有仓库成功后才能写入新版本标记")
        require(
            files.replacementAttempts == [
                WatchAPKRepositoryMigrationService.repositoriesPath,
                WatchAPKRepositoryMigrationService.apkVersionPath,
            ],
            "提交顺序必须先 repositories、后 apk-version")

        let normalizedFiles = RecordingGuestFileStore(
            apkVersion: " \(targetVersion) \n",
            repositories: expectedRepositories)
        let normalizedService = makeService(files: normalizedFiles)
        let synchronized = try normalizedService.finishMigration(
            template: template)
        require(
            synchronized == .repositoriesSynchronized(
                version: targetVersion,
                repositoriesChanged: false,
                markerChanged: true),
            "当前版本应静默规范化版本标记")
        require(
            normalizedFiles.replacementAttempts == [
                WatchAPKRepositoryMigrationService.apkVersionPath,
            ],
            "仓库未变化时只应规范化版本标记")

        normalizedFiles.resetRecording()
        let unchanged = try normalizedService.finishMigration(
            template: template)
        require(
            unchanged == .repositoriesSynchronized(
                version: targetVersion,
                repositoriesChanged: false,
                markerChanged: false) &&
                normalizedFiles.replacementAttempts.isEmpty,
            "完全一致时不应重复写入 guest 文件")
    }

    private static func testAtomicFailures() throws {
        let template = try makeTemplate()

        let repositoryFailure = RecordingGuestFileStore(
            apkVersion: "31900\n")
        repositoryFailure.failingReplacementPath =
            WatchAPKRepositoryMigrationService.repositoriesPath
        try requireFinishFailure(
            files: repositoryFailure,
            template: template,
            expectedAttempts: [
                WatchAPKRepositoryMigrationService.repositoriesPath,
            ],
            message: "仓库提交失败")

        let markerFailure = RecordingGuestFileStore(
            apkVersion: "31900\n")
        markerFailure.failingReplacementPath =
            WatchAPKRepositoryMigrationService.apkVersionPath
        try requireFinishFailure(
            files: markerFailure,
            template: template,
            expectedAttempts: [
                WatchAPKRepositoryMigrationService.repositoriesPath,
                WatchAPKRepositoryMigrationService.apkVersionPath,
                WatchAPKRepositoryMigrationService.repositoriesPath,
            ],
            message: "版本标记提交失败")

        let missingRepository = RecordingGuestFileStore(
            apkVersion: "31900\n",
            repositories: nil)
        missingRepository.failingReplacementPath =
            WatchAPKRepositoryMigrationService.apkVersionPath
        try requireFinishFailure(
            files: missingRepository,
            template: template,
            expectedAttempts: [
                WatchAPKRepositoryMigrationService.repositoriesPath,
                WatchAPKRepositoryMigrationService.apkVersionPath,
                WatchAPKRepositoryMigrationService.repositoriesPath,
            ],
            message: "缺失仓库后的版本标记提交失败")

        let preparedRepositories =
            WatchAPKRepositoriesTemplate.managedHeader +
            templateText
        let prepared = RecordingGuestFileStore(
            apkVersion: "31900\n",
            repositories: preparedRepositories)
        prepared.failingReplacementPath =
            WatchAPKRepositoryMigrationService.apkVersionPath
        let oldPrepared = prepared.files
        do {
            _ = try makeService(files: prepared)
                .finishMigration(template: template)
            require(false, "版本标记故障应让完成阶段失败")
        } catch InjectedFailure.requested {
            require(
                prepared.files == oldPrepared,
                "已经准备好的仓库应保持可重试状态")
            require(
                prepared.replacementAttempts == [
                    WatchAPKRepositoryMigrationService.apkVersionPath,
                ],
                "仓库已准备时失败不得产生多余写入")
        }

        let prepareFailure = RecordingGuestFileStore(
            apkVersion: "31900\n")
        prepareFailure.failingReplacementPath =
            WatchAPKRepositoryMigrationService.repositoriesPath
        let oldPrepare = prepareFailure.files
        do {
            _ = try makeService(files: prepareFailure)
                .prepareRepositories(template: template)
            require(false, "仓库故障应让准备阶段失败")
        } catch InjectedFailure.requested {
            require(
                prepareFailure.files == oldPrepare,
                "准备失败不能留下半更新状态")
            require(
                prepareFailure.replacementAttempts == [
                    WatchAPKRepositoryMigrationService.repositoriesPath,
                ],
                "准备失败绝不能尝试版本标记")
        }
    }

    private static func testFutureMarkerRace() throws {
        let template = try makeTemplate()
        let futureVersion = targetVersion + 100
        let files = RecordingGuestFileStore(
            apkVersion: "31900\n",
            repositories: "用户仓库\n")
        files.apkVersionMutationOnRead = (
            readNumber: 2,
            contents: "\(futureVersion)\n")

        let outcome = try makeService(files: files)
            .finishMigration(template: template)
        require(
            outcome == .skipped(.newerThanApp(
                installedVersion: futureVersion,
                targetVersion: targetVersion)),
            "finish 第二次读取到 future marker 时必须重新判定并跳过")
        require(
            files.replacementAttempts.isEmpty &&
                files.string(
                    at:
                        WatchAPKRepositoryMigrationService
                            .apkVersionPath) == "\(futureVersion)\n" &&
                files.string(
                    at:
                        WatchAPKRepositoryMigrationService
                            .repositoriesPath) == "用户仓库\n",
            "future marker 竞态不得降级 marker 或改写仓库")
    }

    private static func testCAdapterMappingAndErrors() throws {
        let repositoriesID: Int32 = 71
        let markerID: Int32 = 72
        let backend = AdapterBackend()
        backend.files[repositoriesID] = Data("仓库\n".utf8)
        backend.files[markerID] = Data("32400\n".utf8)
        let store = WatchAPKRepositoryGuestFileStore(
            repositoriesFileID: repositoriesID,
            apkVersionFileID: markerID,
            read: backend.read,
            replace: backend.replace)

        require(
            try store.dataIfPresent(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .repositoriesPath) == Data("仓库\n".utf8),
            "adapter 应把 repositories 精确映射到对应 C ID")
        require(
            try store.dataIfPresent(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .apkVersionPath) == Data("32400\n".utf8),
            "adapter 应把 apk-version 精确映射到对应 C ID")
        require(
            backend.readCalls == [
                AdapterBackend.ReadCall(
                    fileID: repositoriesID,
                    capacity:
                        WatchAPKRepositoriesTemplate
                            .maximumManagedByteCount),
                AdapterBackend.ReadCall(
                    fileID: markerID,
                    capacity:
                        WatchAPKRepositoryMigrationService
                            .maximumMarkerByteCount),
            ],
            "两类 guest 文件必须分别使用模板与 marker 的固定上限")

        let callsBeforeInvalidPath = backend.readCalls
        do {
            _ = try store.dataIfPresent(
                atGuestPath: "/etc/apk/repositories/")
            require(false, "近似路径不应通过 exact-path 映射")
        } catch let error as WatchAPKRepositoryGuestFileError {
            require(
                error == .unsupportedPath(
                    "/etc/apk/repositories/"),
                "近似路径应返回明确的不支持错误")
        }
        require(
            backend.readCalls == callsBeforeInvalidPath,
            "未知路径不得进入 C adapter")

        backend.files[markerID] = nil
        require(
            try store.dataIfPresent(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .apkVersionPath) == nil,
            "C read 的 ENOENT 应映射为 nil")

        backend.readFailures[markerID] = Int(EACCES)
        do {
            _ = try store.dataIfPresent(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .apkVersionPath)
            require(false, "其他负 errno 不得伪装成文件缺失")
        } catch let error as WatchAPKRepositoryGuestFileError {
            require(
                error == .readFailed(
                    path:
                        WatchAPKRepositoryMigrationService
                            .apkVersionPath,
                    linuxErrno: Int(EACCES)),
                "read errno 应保留路径和正 errno")
            require(
                error.localizedDescription.contains("读取 guest 文件"),
                "read errno 应提供中文 LocalizedError")
        }
        backend.readFailures[markerID] = nil

        backend.readFailures[markerID] = Int(EFBIG)
        do {
            _ = try store.dataIfPresent(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .apkVersionPath)
            require(false, "C 的 EFBIG 应映射为文件超限")
        } catch let error as WatchAPKRepositoryGuestFileError {
            require(
                error == .fileTooLarge(
                    path:
                        WatchAPKRepositoryMigrationService
                            .apkVersionPath,
                    maximumByteCount:
                        WatchAPKRepositoryMigrationService
                            .maximumMarkerByteCount),
                "EFBIG 应保留对应文件的管理上限")
        }
        backend.readFailures[markerID] = nil

        backend.oversizedReadFileID = markerID
        do {
            _ = try store.dataIfPresent(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .apkVersionPath)
            require(false, "超过 marker 上限的 read 结果应被拒绝")
        } catch let error as WatchAPKRepositoryGuestFileError {
            require(
                error == .fileTooLarge(
                    path:
                        WatchAPKRepositoryMigrationService
                            .apkVersionPath,
                    maximumByteCount:
                        WatchAPKRepositoryMigrationService
                            .maximumMarkerByteCount),
                "超限 read 应报告对应边界")
        }
        backend.oversizedReadFileID = nil

        try store.replaceAtomically(
            atGuestPath:
                WatchAPKRepositoryMigrationService.repositoriesPath,
            with: Data("新仓库\n".utf8))
        try store.replaceAtomically(
            atGuestPath:
                WatchAPKRepositoryMigrationService.apkVersionPath,
            with: nil)
        require(
            backend.replaceCalls.suffix(2) == [
                AdapterBackend.ReplaceCall(
                    fileID: repositoriesID,
                    data: Data("新仓库\n".utf8),
                    removeFile: 0),
                AdapterBackend.ReplaceCall(
                    fileID: markerID,
                    data: nil,
                    removeFile: 1),
            ],
            "replace 与删除必须使用正确 ID 和 remove_file 合同")

        backend.replaceFailures[repositoriesID] = Int32(EROFS)
        do {
            try store.replaceAtomically(
                atGuestPath:
                    WatchAPKRepositoryMigrationService
                        .repositoriesPath,
                with: Data("失败\n".utf8))
            require(false, "replace 负 errno 应抛错")
        } catch let error as WatchAPKRepositoryGuestFileError {
            require(
                error == .replaceFailed(
                    path:
                        WatchAPKRepositoryMigrationService
                            .repositoriesPath,
                    linuxErrno: Int(EROFS)),
                "replace errno 应保留路径和正 errno")
        }

        do {
            try store.replaceAtomically(
                atGuestPath:
                    WatchAPKRepositoryMigrationService.apkVersionPath,
                with: Data(
                    repeating: 0x31,
                    count:
                        WatchAPKRepositoryMigrationService
                            .maximumMarkerByteCount + 1))
            require(false, "超长 marker 不得进入 C replace")
        } catch let error as WatchAPKRepositoryGuestFileError {
            require(
                error == .fileTooLarge(
                    path:
                        WatchAPKRepositoryMigrationService
                            .apkVersionPath,
                    maximumByteCount:
                        WatchAPKRepositoryMigrationService
                            .maximumMarkerByteCount),
                "replace 应复用 marker 上限")
        }
    }

    private static func testUpgradeOrchestration() throws {
        let template = try makeTemplate()
        let oldFiles = RecordingGuestFileStore(
            apkVersion: "31900\n")
        let oldService = makeService(files: oldFiles)
        let oldStatus = try oldService.status()
        var migration = WatchAPKRepositoryMigrationCoordinator()
        migration.recordInspection(oldStatus)
        require(
            migration.canStartPackageUpgrade,
            "检查完成且空闲时应允许开始升级")
        require(
            migration.beginUpgradePreparation(),
            "旧 marker 应允许开始仓库准备")
        require(
            !migration.canStartPackageUpgrade &&
                !migration.beginUpgradePreparation(),
            "仓库准备期间必须禁用并拒绝重复升级")
        let prepared = try oldService.prepareRepositories(
            template: template)
        let upgradeID = UUID()
        migration.recordPreparedUpgrade(
            sessionID: upgradeID,
            outcome: prepared)
        require(
            migration.activity == .upgrading(
                sessionID: upgradeID,
                finishesMigration: true) &&
                !migration.canStartPackageUpgrade,
            "旧 marker 的升级会话应等待成功后收尾")
        require(
            !migration.observeMaintenance(
                sessionID: UUID(),
                phase: .completed),
            "错误 UUID 的成功状态不得触发 finish")
        require(
            migration.observeMaintenance(
                sessionID: upgradeID,
                phase: .completed) &&
                !migration.canStartPackageUpgrade,
            "匹配升级会话的 status 0 应首次触发 finish")
        require(
            !migration.observeMaintenance(
                sessionID: upgradeID,
                phase: .completed),
            "重复 poll 不得重复触发 finish")
        let finished = try oldService.finishMigration(
            template: template)
        migration.recordMigrationFinished(
            sessionID: upgradeID,
            outcome: finished)
        require(
            oldFiles.string(
                at:
                    WatchAPKRepositoryMigrationService
                        .apkVersionPath) == "\(targetVersion)\n" &&
                migration.status == .current(
                    version: targetVersion) &&
                migration.activity == .idle &&
                migration.canStartPackageUpgrade,
            "旧 marker 应在 prepare 与成功升级后才推进到目标版本")

        for phase in [
            WatchAPKRepositoryMigrationCoordinator
                .UpgradePhase.failed(17),
            .interrupted,
        ] {
            let failedFiles = RecordingGuestFileStore(
                apkVersion: "31900\n")
            let service = makeService(files: failedFiles)
            var failed = WatchAPKRepositoryMigrationCoordinator()
            failed.recordInspection(try service.status())
            require(
                failed.beginUpgradePreparation(),
                "失败路径应能开始准备")
            failed.recordPreparedUpgrade(
                sessionID: upgradeID,
                outcome: try service.prepareRepositories(
                    template: template))
            require(
                !failed.observeMaintenance(
                    sessionID: upgradeID,
                    phase: phase),
                "非零或中断绝不能请求 finish")
            require(
                failedFiles.string(
                    at:
                        WatchAPKRepositoryMigrationService
                            .apkVersionPath) == "31900\n" &&
                    failed.activity == .idle &&
                    failed.errorMessage != nil,
                "非零或中断必须保留旧 marker 并发布可重试错误")
        }

        let futureFiles = RecordingGuestFileStore(
            apkVersion: "\(targetVersion + 100)\n")
        let futureService = makeService(files: futureFiles)
        var future = WatchAPKRepositoryMigrationCoordinator()
        future.recordInspection(try futureService.status())
        require(
            future.beginUpgradePreparation(),
            "未来 marker 仍可运行普通 apk upgrade")
        let futureOutcome = try futureService.prepareRepositories(
            template: template)
        future.recordPreparedUpgrade(
            sessionID: upgradeID,
            outcome: futureOutcome)
        require(
            future.activity == .upgrading(
                sessionID: upgradeID,
                finishesMigration: false) &&
                !future.observeMaintenance(
                    sessionID: upgradeID,
                    phase: .completed) &&
                future.status == .newerThanApp(
                    installedVersion: targetVersion + 100,
                    targetVersion: targetVersion) &&
                futureFiles.replacementAttempts.isEmpty,
            "未来 marker 必须跳过仓库与 marker 改写")

        var closed = WatchAPKRepositoryMigrationCoordinator()
        closed.recordInspection(.migrationRequired(
            installedVersion: 31_900,
            targetVersion: targetVersion))
        require(closed.beginUpgradePreparation(), "关闭测试应开始准备")
        closed.recordPreparedUpgrade(
            sessionID: upgradeID,
            outcome: prepared)
        closed.recordSessionClosed(upgradeID)
        require(
            closed.activity == .idle &&
                closed.errorMessage?.contains("中断") == true,
            "关闭匹配升级会话应取消待收尾状态")
    }

    private static func testCoordinatorFailureSemantics() {
        let sessionID = UUID()
        let target = targetVersion

        var current = WatchAPKRepositoryMigrationCoordinator()
        current.recordInspection(.current(version: target))
        require(
            current.beginUpgradePreparation(),
            "current 状态应允许普通升级")
        current.recordPreparedUpgrade(
            sessionID: sessionID,
            outcome: .repositoriesSynchronized(
                version: target,
                repositoriesChanged: false,
                markerChanged: false))
        _ = current.observeMaintenance(
            sessionID: sessionID,
            phase: .failed(17))
        require(
            current.errorMessage == "软件升级失败（状态 17）" &&
                current.errorMessage?.contains("迁移未完成") == false,
            "current 的普通升级失败不应谎称迁移未完成")

        var future = WatchAPKRepositoryMigrationCoordinator()
        let futureStatus =
            WatchAPKRepositoryMigrationStatus.newerThanApp(
                installedVersion: target + 100,
                targetVersion: target)
        future.recordInspection(futureStatus)
        require(
            future.beginUpgradePreparation(),
            "future 状态应允许普通升级")
        future.recordPreparedUpgrade(
            sessionID: sessionID,
            outcome: .skipped(futureStatus))
        future.recordSessionClosed(sessionID)
        require(
            future.errorMessage == "软件升级已中断" &&
                future.status == futureStatus,
            "future 的关闭错误不应声称迁移未完成或丢失状态")

        var unavailable = WatchAPKRepositoryMigrationCoordinator()
        unavailable.recordRuntimeUnavailable("Linux 未运行，无法检查")
        require(
            unavailable.activity == .idle &&
                unavailable.errorMessage == "Linux 未运行，无法检查" &&
                unavailable.canStartPackageUpgrade,
            "Linux 启动失败应结束永久 checking 并保留可重试状态")
    }

    private static func testStatusPresentation() {
        var migration = WatchAPKRepositoryMigrationCoordinator()
        var presentation = WatchAPKRepositoryStatusPresentation(
            coordinator: migration)
        require(
            presentation.indicator == .working &&
                presentation.accessibilitySummary.contains("检查"),
            "首次检查应发布静态工作 badge 的语义摘要")

        migration.recordInspection(.migrationRequired(
            installedVersion: 31_900,
            targetVersion: targetVersion))
        presentation = WatchAPKRepositoryStatusPresentation(
            coordinator: migration)
        require(
            presentation.indicator == .migrationRequired &&
                presentation.accessibilitySummary.contains("需要迁移"),
            "待迁移状态应在设置入口发布提醒 badge")

        migration.recordFailure("读取仓库失败")
        presentation = WatchAPKRepositoryStatusPresentation(
            coordinator: migration)
        require(
            presentation.indicator == .error &&
                presentation.accessibilitySummary ==
                    "软件仓库错误：读取仓库失败",
            "仓库错误应覆盖普通迁移提醒并保留具体语义")

        migration.recordInspection(.current(version: targetVersion))
        presentation = WatchAPKRepositoryStatusPresentation(
            coordinator: migration)
        require(
            presentation.indicator == .current &&
                presentation.accessibilitySummary.contains("当前版本"),
            "当前仓库不应显示注意 badge，但仍应提供明确语义")
    }

    private static func requireFinishFailure(
        files: RecordingGuestFileStore,
        template: WatchAPKRepositoriesTemplate,
        expectedAttempts: [String],
        message: String
    ) throws {
        let originalFiles = files.files
        do {
            _ = try makeService(files: files)
                .finishMigration(template: template)
            require(false, "\(message)时迁移应失败")
        } catch InjectedFailure.requested {
            require(
                files.files == originalFiles,
                "\(message)后所有 guest 文件必须恢复原状")
            require(
                files.replacementAttempts == expectedAttempts,
                "\(message)时原子替换顺序不正确")
            require(
                files.string(
                    at: WatchAPKRepositoryMigrationService
                        .apkVersionPath) ==
                    String(
                        data:
                            originalFiles[
                                WatchAPKRepositoryMigrationService
                                    .apkVersionPath] ?? Data(),
                        encoding: .utf8),
                "\(message)后版本标记不能前进")
        }
    }
}

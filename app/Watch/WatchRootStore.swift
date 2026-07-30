import Combine
import Foundation

private final class WatchRootArchiveProgressContext:
        @unchecked Sendable {
    let id: UUID
    private let lock = NSLock()
    private var cancelled = false
    private let update: @Sendable (Double, String) -> Void

    init(
        id: UUID,
        update: @escaping @Sendable (Double, String) -> Void
    ) {
        self.id = id
        self.update = update
    }

    func report(fraction: Double, message: String) {
        update(fraction, message)
    }

    func cancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }

    func isCancelled() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }
}

private let watchRootArchiveProgressCallback:
        @convention(c) (
            UnsafeMutableRawPointer?,
            Double,
            UnsafePointer<CChar>?,
            UnsafeMutablePointer<Bool>?
        ) -> Void = { cookie, fraction, message, cancelOut in
    guard let cookie else { return }
    let context = Unmanaged<WatchRootArchiveProgressContext>
        .fromOpaque(cookie)
        .takeUnretainedValue()
    context.report(
        fraction: fraction,
        message: message.map(String.init(cString:)) ?? "")
    cancelOut?.pointee = context.isCancelled()
}

struct WatchRootEntry: Identifiable, Equatable, Sendable {
    let name: String

    var id: String { name }
}

@MainActor
final class WatchRootStore: ObservableObject {
    struct PreparedRoot: Sendable {
        let name: String
        let dataPath: String
        fileprivate let claimFile: Int32
    }

    private struct Paths: Sendable {
        let seed: String
        let parent: String
    }

    private enum StoreError: LocalizedError {
        case missingSeed
        case missingApplicationSupport
        case cannotCreateDirectory(String)
        case catalog(operation: String, code: Int32)
        case invalidSelection
        case protectedRoot
        case activeRoot
        case operationInProgress
        case archive(operation: String, code: Int32, detail: String?)

        var errorDescription: String? {
            switch self {
            case .missingSeed:
                return "App 中缺少 AArch64 Linux 种子"
            case .missingApplicationSupport:
                return "无法取得应用支持目录"
            case let .cannotCreateDirectory(message):
                return "无法创建 Linux 数据目录：\(message)"
            case let .catalog(operation, code):
                return "\(operation)失败（POSIX errno \(code)）"
            case .invalidSelection:
                return "这个 Linux 文件系统已不存在"
            case .protectedRoot:
                return "正在使用或下次启动选中的文件系统不能删除"
            case .activeRoot:
                return "正在使用的 Linux 文件系统不能复制"
            case .operationInProgress:
                return "Linux 文件系统正在准备中"
            case let .archive(operation, code, detail):
                let suffix = detail.flatMap {
                    $0.isEmpty ? nil : "：\($0)"
                } ?? "（POSIX errno \(code)）"
                return "\(operation)失败\(suffix)"
            }
        }
    }

    @Published private(set) var entries: [WatchRootEntry] = []
    @Published private(set) var claimedName: String?
    @Published private(set) var activeName: String?
    @Published private(set) var isWorking = false
    @Published private(set) var errorMessage: String?
    @Published private(set)
    var archiveOperation: WatchHostRootArchiveOperation?
    @Published private(set)
    var archiveProgress: WatchHostRootArchiveProgress?
    @Published private var metadata: WatchRootMetadata

    var isAvailable: Bool { paths != nil }
    var selectedName: String? { metadata.selectedRootName }
    var pendingCopyName: String? { metadata.pendingCopySource }

    private static let legacySelectedRootKey = "watchSelectedRoot"
    private let defaults: UserDefaults
    private let paths: Paths?
    private let setupError: Error?
    private var activeClaimFile: Int32 = -1
    private var archiveProgressContext:
        WatchRootArchiveProgressContext?

    init(
        bundle: Bundle = .main,
        fileManager: FileManager = .default,
        defaults: UserDefaults = .standard
    ) {
        self.defaults = defaults
        let loadedMetadata = WatchRootMetadataPersistence.load(
            from: defaults,
            legacySelectedRootName: defaults.string(
                forKey: Self.legacySelectedRootKey))
        metadata = loadedMetadata
        WatchRootMetadataPersistence.save(
            loadedMetadata,
            to: defaults)
        defaults.removeObject(
            forKey: Self.legacySelectedRootKey)

        do {
            guard let seed = bundle.url(
                    forResource: "AArch64Rootfs",
                    withExtension: "seed") else {
                throw StoreError.missingSeed
            }
            guard let applicationSupport = fileManager.urls(
                    for: .applicationSupportDirectory,
                    in: .userDomainMask).first else {
                throw StoreError.missingApplicationSupport
            }
            let parent = applicationSupport.appendingPathComponent(
                "LinuxRoots", isDirectory: true)
            do {
                try fileManager.createDirectory(
                    at: parent, withIntermediateDirectories: true)
            } catch {
                throw StoreError.cannotCreateDirectory(
                    error.localizedDescription)
            }
            paths = Paths(seed: seed.path, parent: parent.path)
            setupError = nil
        } catch {
            paths = nil
            setupError = error
        }
        if let setupError {
            errorMessage = setupError.localizedDescription
        }
    }

    deinit {
        if activeClaimFile >= 0 {
            _ = ish_apple_root_catalog_release_active(activeClaimFile)
        }
    }

    func prepareForLaunch() async throws -> PreparedRoot {
        guard let paths else {
            throw setupError ?? StoreError.missingApplicationSupport
        }
        guard !isWorking else {
            throw StoreError.operationInProgress
        }
        isWorking = true
        defer { isWorking = false }

        let cleanupError = await Task.detached(
            priority: .utility
        ) {
            paths.parent.withCString {
                ish_apple_watch_root_archive_cleanup($0)
            }
        }.value
        if cleanupError != 0 {
            errorMessage = StoreError.catalog(
                operation: "清理归档临时文件",
                code: cleanupError).localizedDescription
        }

        var pendingCopyError: String?
        if let request = metadata.pendingCopy {
            do {
                let copiedName = try await Task.detached(
                    priority: .userInitiated
                ) {
                    try Self.resumeCopy(
                        paths: paths,
                        request: request)
                }.value
                var updatedMetadata = metadata
                if updatedMetadata.completeScheduledCopy(
                    request,
                    destinationName: copiedName,
                    existingRootNames:
                        entries.map(\.name) +
                        Array(metadata.aliases.keys)
                ) {
                    updateMetadata(updatedMetadata)
                }
            } catch {
                pendingCopyError =
                    "计划的文件系统副本未完成：\(error.localizedDescription)"
            }
        }
        let preferred = selectedName
        let prepared = try await Task.detached(priority: .userInitiated) {
            try Self.prepare(paths: paths, preferred: preferred)
        }.value
        let previousClaim = activeClaimFile
        activeClaimFile = prepared.claimFile
        if previousClaim >= 0 {
            _ = ish_apple_root_catalog_release_active(previousClaim)
        }
        // 从交给 runtime 起就保护该目录，避免 mount 期间被设置页删除。
        claimedName = prepared.name
        if selectedName != prepared.name {
            setSelectedName(prepared.name)
        }
        do {
            adoptEntries(try await Self.loadEntries(paths: paths))
            errorMessage = pendingCopyError
        } catch {
            // 设置页枚举失败不能阻止已经准备完成的 Linux 环境启动。
            errorMessage = pendingCopyError.map {
                "\($0)\n列表刷新失败：\(error.localizedDescription)"
            } ?? error.localizedDescription
        }
        return prepared
    }

    func activate(_ name: String) {
        activeName = name
    }

    func refresh() async {
        guard !isWorking else { return }
        guard let paths else {
            errorMessage = (
                setupError ?? StoreError.missingApplicationSupport
            ).localizedDescription
            return
        }
        isWorking = true
        defer { isWorking = false }
        do {
            adoptEntries(try await Self.loadEntries(paths: paths))
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func createAndSelect() async {
        guard !isWorking else { return }
        guard let paths else {
            errorMessage = (
                setupError ?? StoreError.missingApplicationSupport
            ).localizedDescription
            return
        }
        isWorking = true
        defer { isWorking = false }
        do {
            let name = try await Task.detached(priority: .userInitiated) {
                try Self.create(paths: paths)
            }.value
            setSelectedName(name)
            adoptEntries(try await Self.loadEntries(paths: paths))
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @discardableResult
    func importArchive(_ file: WatchSharedFile) async -> Bool {
        guard !isWorking else { return false }
        guard let paths else {
            errorMessage = (
                setupError ?? StoreError.missingApplicationSupport
            ).localizedDescription
            return false
        }
        guard WatchHostRootArchive.supportsImport(
                fileName: file.name) else {
            errorMessage = "只支持普通 .tar、.tar.gz 或 .tgz 归档。"
            return false
        }

        isWorking = true
        archiveOperation = .importing(file.name)
        let context = makeArchiveProgressContext()
        defer {
            archiveProgressContext = nil
            archiveProgress = nil
            archiveOperation = nil
            isWorking = false
        }
        do {
            let sharedDirectory =
                file.url.deletingLastPathComponent().path
            let name = try await Task.detached(
                priority: .userInitiated
            ) {
                try Self.importArchive(
                    paths: paths,
                    sharedDirectory: sharedDirectory,
                    fileName: file.name,
                    context: context)
            }.value
            setSelectedName(name)
            do {
                adoptEntries(try await Self.loadEntries(paths: paths))
                errorMessage = nil
            } catch {
                errorMessage =
                    "恢复已完成，但暂时无法刷新列表：\(error.localizedDescription)"
            }
            return true
        } catch is CancellationError {
            errorMessage = nil
            return false
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    @discardableResult
    func exportArchive(_ name: String) async -> String? {
        guard !isWorking else { return nil }
        guard let paths else {
            errorMessage = (
                setupError ?? StoreError.missingApplicationSupport
            ).localizedDescription
            return nil
        }
        guard name != claimedName, name != activeName else {
            errorMessage = "正在使用的文件系统请通过终端导出。"
            return nil
        }
        guard entries.contains(where: { $0.name == name }) else {
            errorMessage = StoreError.invalidSelection.localizedDescription
            return nil
        }

        isWorking = true
        archiveOperation = .exporting(name)
        let context = makeArchiveProgressContext()
        defer {
            archiveProgressContext = nil
            archiveProgress = nil
            archiveOperation = nil
            isWorking = false
        }
        do {
            let sharedDirectory =
                try WatchSharedFiles.ensureSystemDirectory().path
            let outputName = WatchHostRootArchive.exportFileName(
                rootName: name)
            let active = claimedName ?? activeName
            try await Task.detached(priority: .userInitiated) {
                try Self.exportArchive(
                    paths: paths,
                    rootName: name,
                    activeName: active,
                    sharedDirectory: sharedDirectory,
                    outputName: outputName,
                    context: context)
            }.value
            errorMessage = nil
            return outputName
        } catch is CancellationError {
            errorMessage = nil
            return nil
        } catch {
            errorMessage = error.localizedDescription
            return nil
        }
    }

    func selectForNextLaunch(_ name: String) {
        guard !isWorking else {
            errorMessage = StoreError.operationInProgress.localizedDescription
            return
        }
        guard entries.contains(where: { $0.name == name }) else {
            errorMessage = StoreError.invalidSelection.localizedDescription
            return
        }
        setSelectedName(name)
        errorMessage = nil
    }

    func displayName(for name: String) -> String {
        metadata.displayName(for: name)
    }

    @discardableResult
    func rename(_ name: String, to displayName: String) -> Bool {
        guard !isWorking else {
            errorMessage = StoreError.operationInProgress.localizedDescription
            return false
        }
        var updatedMetadata = metadata
        do {
            try updatedMetadata.rename(
                name,
                to: displayName,
                existingRootNames: entries.map(\.name))
            updateMetadata(updatedMetadata)
            errorMessage = nil
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    func restoreDefaultName(for name: String) {
        guard !isWorking else {
            errorMessage = StoreError.operationInProgress.localizedDescription
            return
        }
        var updatedMetadata = metadata
        updatedMetadata.restoreDefaultName(
            for: name,
            existingRootNames: entries.map(\.name))
        updateMetadata(updatedMetadata)
        errorMessage = nil
    }

    func scheduleCopyForNextLaunch(_ name: String) {
        guard !isWorking else {
            errorMessage = StoreError.operationInProgress.localizedDescription
            return
        }
        guard entries.contains(where: { $0.name == name }) else {
            errorMessage = StoreError.invalidSelection.localizedDescription
            return
        }
        var updatedMetadata = metadata
        updatedMetadata.scheduleCopy(of: name)
        updateMetadata(updatedMetadata)
        errorMessage = nil
    }

    func cancelScheduledCopy() {
        guard !isWorking else {
            errorMessage = StoreError.operationInProgress.localizedDescription
            return
        }
        var updatedMetadata = metadata
        updatedMetadata.cancelScheduledCopy()
        updateMetadata(updatedMetadata)
        errorMessage = nil
    }

    func copyAndSelect(_ name: String) async -> Bool {
        guard !isWorking else { return false }
        guard let paths else {
            errorMessage = (
                setupError ?? StoreError.missingApplicationSupport
            ).localizedDescription
            return false
        }
        guard name != claimedName, name != activeName else {
            errorMessage = StoreError.activeRoot.localizedDescription
            return false
        }
        guard entries.contains(where: { $0.name == name }) else {
            errorMessage = StoreError.invalidSelection.localizedDescription
            return false
        }

        isWorking = true
        defer { isWorking = false }
        do {
            let active = claimedName ?? activeName
            let copiedName = try await Task.detached(
                priority: .userInitiated
            ) {
                try Self.copy(
                    paths: paths,
                    source: name,
                    active: active)
            }.value
            var updatedMetadata = metadata
            updatedMetadata.recordImmediateCopyAndSelect(
                from: name,
                to: copiedName,
                existingRootNames: entries.map(\.name))
            updateMetadata(updatedMetadata)
            do {
                adoptEntries(try await Self.loadEntries(paths: paths))
                errorMessage = nil
            } catch {
                errorMessage =
                    "复制已完成，但暂时无法刷新列表：\(error.localizedDescription)"
            }
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    func delete(_ name: String) async -> Bool {
        guard !isWorking else { return false }
        guard let paths else {
            errorMessage = (
                setupError ?? StoreError.missingApplicationSupport
            ).localizedDescription
            return false
        }
        guard name != claimedName, name != selectedName else {
            errorMessage = StoreError.protectedRoot.localizedDescription
            return false
        }
        guard entries.contains(where: { $0.name == name }) else {
            errorMessage = StoreError.invalidSelection.localizedDescription
            return false
        }

        isWorking = true
        defer { isWorking = false }
        do {
            let claimed = claimedName ?? ""
            let selected = selectedName ?? ""
            try await Task.detached(priority: .userInitiated) {
                let code = paths.parent.withCString { parent in
                    name.withCString { root in
                        claimed.withCString { activeRoot in
                            selected.withCString { selectedRoot in
                                ish_apple_root_catalog_delete(
                                    parent, root,
                                    activeRoot, selectedRoot)
                            }
                        }
                    }
                }
                guard code == 0 else {
                    throw StoreError.catalog(
                        operation: "删除 Linux 文件系统",
                        code: code)
                }
            }.value
            var updatedMetadata = metadata
            updatedMetadata.remove(name)
            updateMetadata(updatedMetadata)
            adoptEntries(try await Self.loadEntries(paths: paths))
            errorMessage = nil
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    func clearError() {
        errorMessage = nil
    }

    func cancelArchiveOperation() {
        archiveProgressContext?.cancel()
        if archiveProgressContext != nil {
            archiveProgress = WatchHostRootArchiveProgress(
                fraction: archiveProgress?.fraction ?? 0,
                message: "正在取消…")
        }
    }

    private func makeArchiveProgressContext()
            -> WatchRootArchiveProgressContext {
        let id = UUID()
        let context = WatchRootArchiveProgressContext(id: id) {
            [weak self] fraction, message in
            Task { @MainActor [weak self] in
                guard let self,
                      self.archiveProgressContext?.id == id else {
                    return
                }
                self.archiveProgress = WatchHostRootArchiveProgress(
                    fraction: fraction,
                    message: message)
            }
        }
        archiveProgressContext = context
        archiveProgress = WatchHostRootArchiveProgress(
            fraction: 0,
            message: "正在准备…")
        return context
    }

    private func setSelectedName(_ name: String) {
        var updatedMetadata = metadata
        updatedMetadata.select(name)
        updateMetadata(updatedMetadata)
    }

    private func updateMetadata(_ updatedMetadata: WatchRootMetadata) {
        WatchRootMetadataPersistence.save(
            updatedMetadata,
            to: defaults)
        metadata = updatedMetadata
    }

    private func adoptEntries(_ loadedEntries: [WatchRootEntry]) {
        var updatedMetadata = metadata
        updatedMetadata.removeMissingRoots(
            loadedEntries.map(\.name))
        if updatedMetadata != metadata {
            updateMetadata(updatedMetadata)
        }
        entries = loadedEntries
    }

    nonisolated private static func prepare(
        paths: Paths,
        preferred: String?
    ) throws -> PreparedRoot {
        var name = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_NAME_CAPACITY))
        var result = ISH_APPLE_ROOTFS_SEED_INSTALLED
        let code = name.withUnsafeMutableBufferPointer { nameBuffer in
            paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    if let preferred, !preferred.isEmpty {
                        return preferred.withCString { preferredName in
                            ish_apple_root_catalog_prepare(
                                seed, parent, preferredName,
                                nameBuffer.baseAddress, &result)
                        }
                    }
                    return ish_apple_root_catalog_prepare(
                        seed, parent, nil,
                        nameBuffer.baseAddress, &result)
                }
            }
        }
        guard code == 0 else {
            throw StoreError.catalog(
                operation: "准备 Linux 文件系统",
                code: code)
        }

        let rootName = String(cString: name)
        var dataPath = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_PATH_CAPACITY))
        let pathCode = dataPath.withUnsafeMutableBufferPointer {
            pathBuffer in
            paths.parent.withCString { parent in
                rootName.withCString { root in
                    ish_apple_root_catalog_data_path(
                        parent, root,
                        pathBuffer.baseAddress, pathBuffer.count)
                }
            }
        }
        guard pathCode == 0 else {
            throw StoreError.catalog(
                operation: "解析 Linux 文件系统路径",
                code: pathCode)
        }
        var claimFile: Int32 = -1
        let claimCode = paths.parent.withCString { parent in
            rootName.withCString { root in
                ish_apple_root_catalog_claim_active(
                    parent, root, &claimFile)
            }
        }
        guard claimCode == 0 else {
            throw StoreError.catalog(
                operation: "锁定 Linux 文件系统",
                code: claimCode)
        }
        return PreparedRoot(
            name: rootName,
            dataPath: String(cString: dataPath),
            claimFile: claimFile)
    }

    nonisolated private static func create(
        paths: Paths
    ) throws -> String {
        var name = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_NAME_CAPACITY))
        let code = name.withUnsafeMutableBufferPointer { nameBuffer in
            paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    ish_apple_root_catalog_create(
                        seed, parent, nameBuffer.baseAddress)
                }
            }
        }
        guard code == 0 else {
            throw StoreError.catalog(
                operation: "创建 Linux 文件系统",
                code: code)
        }
        return String(cString: name)
    }

    nonisolated private static func copy(
        paths: Paths,
        source: String,
        active: String?
    ) throws -> String {
        var name = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_NAME_CAPACITY))
        let code = name.withUnsafeMutableBufferPointer { nameBuffer in
            paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    source.withCString { sourceName in
                        if let active {
                            return active.withCString { activeName in
                                ish_apple_root_catalog_copy(
                                    seed, parent, sourceName,
                                    activeName, nameBuffer.baseAddress)
                            }
                        } else {
                            return ish_apple_root_catalog_copy(
                                seed, parent, sourceName,
                                nil, nameBuffer.baseAddress)
                        }
                    }
                }
            }
        }
        guard code == 0 else {
            throw StoreError.catalog(
                operation: "复制 Linux 文件系统",
                code: code)
        }
        return String(cString: name)
    }

    nonisolated private static func importArchive(
        paths: Paths,
        sharedDirectory: String,
        fileName: String,
        context: WatchRootArchiveProgressContext
    ) throws -> String {
        var name = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_NAME_CAPACITY))
        var archiveError = ish_apple_watch_root_archive_error()
        let callback = progress(
            cookie: Unmanaged.passUnretained(context).toOpaque(),
            callback: watchRootArchiveProgressCallback)
        let code = name.withUnsafeMutableBufferPointer { nameBuffer in
            paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    sharedDirectory.withCString { shared in
                        fileName.withCString { archive in
                            ish_apple_watch_root_archive_import(
                                seed, parent, shared, archive,
                                nameBuffer.baseAddress, callback,
                                &archiveError)
                        }
                    }
                }
            }
        }
        if code == ECANCELED {
            throw CancellationError()
        }
        guard code == 0 else {
            throw StoreError.archive(
                operation: "恢复 Linux 文件系统",
                code: code,
                detail: archiveErrorMessage(archiveError))
        }
        return String(cString: name)
    }

    nonisolated private static func exportArchive(
        paths: Paths,
        rootName: String,
        activeName: String?,
        sharedDirectory: String,
        outputName: String,
        context: WatchRootArchiveProgressContext
    ) throws {
        var archiveError = ish_apple_watch_root_archive_error()
        let callback = progress(
            cookie: Unmanaged.passUnretained(context).toOpaque(),
            callback: watchRootArchiveProgressCallback)
        let code = paths.parent.withCString { parent in
            rootName.withCString { root in
                sharedDirectory.withCString { shared in
                    outputName.withCString { output in
                        if let activeName {
                            return activeName.withCString { active in
                                ish_apple_watch_root_archive_export(
                                    parent, root, active, shared,
                                    output, callback, &archiveError)
                            }
                        }
                        return ish_apple_watch_root_archive_export(
                            parent, root, nil, shared,
                            output, callback, &archiveError)
                    }
                }
            }
        }
        if code == ECANCELED {
            throw CancellationError()
        }
        guard code == 0 else {
            throw StoreError.archive(
                operation: "导出 Linux 文件系统",
                code: code,
                detail: archiveErrorMessage(archiveError))
        }
    }

    nonisolated private static func archiveErrorMessage(
        _ error: ish_apple_watch_root_archive_error
    ) -> String? {
        var mutableError = error
        return withUnsafePointer(to: &mutableError.message) {
            $0.withMemoryRebound(
                to: CChar.self,
                capacity: Int(
                    ISH_APPLE_WATCH_ROOT_ARCHIVE_MESSAGE_CAPACITY)
            ) {
                $0.pointee == 0 ? nil : String(cString: $0)
            }
        }
    }

    nonisolated private static func resumeCopy(
        paths: Paths,
        request: WatchRootCopyRequest
    ) throws -> String {
        var name = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_NAME_CAPACITY))
        let code = name.withUnsafeMutableBufferPointer { nameBuffer in
            paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    request.sourceName.withCString { sourceName in
                        request.tokenString.withCString { token in
                            ish_apple_root_catalog_copy_resumable(
                                seed, parent, sourceName, nil, token,
                                nameBuffer.baseAddress)
                        }
                    }
                }
            }
        }
        guard code == 0 else {
            throw StoreError.catalog(
                operation: "恢复计划的 Linux 文件系统复制",
                code: code)
        }
        return String(cString: name)
    }

    nonisolated private static func loadEntries(
        paths: Paths
    ) async throws -> [WatchRootEntry] {
        try await Task.detached(priority: .utility) {
            var count = 0
            let countCode = paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    ish_apple_root_catalog_list(
                        seed, parent, nil, 0, &count)
                }
            }
            guard countCode == 0 || countCode == ERANGE else {
                throw StoreError.catalog(
                    operation: "列出 Linux 文件系统",
                    code: countCode)
            }
            guard count > 0 else { return [] }

            let records = UnsafeMutablePointer<
                ish_apple_root_entry
            >.allocate(capacity: count)
            defer { records.deallocate() }
            var loadedCount = count
            let listCode = paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    ish_apple_root_catalog_list(
                        seed, parent, records, count, &loadedCount)
                }
            }
            guard listCode == 0 else {
                throw StoreError.catalog(
                    operation: "列出 Linux 文件系统",
                    code: listCode)
            }
            return (0..<loadedCount).map { index in
                var record = records[index]
                let name = withUnsafePointer(to: &record.name) {
                    $0.withMemoryRebound(
                        to: CChar.self,
                        capacity: Int(ISH_APPLE_ROOT_NAME_CAPACITY)
                    ) {
                        String(cString: $0)
                    }
                }
                return WatchRootEntry(name: name)
            }
        }.value
    }
}

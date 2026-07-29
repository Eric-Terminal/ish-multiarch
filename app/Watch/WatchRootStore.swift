import Combine
import Foundation

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
            }
        }
    }

    @Published private(set) var entries: [WatchRootEntry] = []
    @Published private(set) var claimedName: String?
    @Published private(set) var activeName: String?
    @Published private(set) var selectedName: String?
    @Published private(set) var isWorking = false
    @Published private(set) var errorMessage: String?

    var isAvailable: Bool { paths != nil }

    private static let selectedRootKey = "watchSelectedRoot"
    private let defaults: UserDefaults
    private let paths: Paths?
    private let setupError: Error?
    private var activeClaimFile: Int32 = -1

    init(
        bundle: Bundle = .main,
        fileManager: FileManager = .default,
        defaults: UserDefaults = .standard
    ) {
        self.defaults = defaults
        selectedName = defaults.string(
            forKey: Self.selectedRootKey)

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
        isWorking = true
        defer { isWorking = false }

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
        setSelectedName(prepared.name)
        do {
            entries = try await Self.loadEntries(paths: paths)
            errorMessage = nil
        } catch {
            // 设置页枚举失败不能阻止已经准备完成的 Linux 环境启动。
            errorMessage = error.localizedDescription
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
            entries = try await Self.loadEntries(paths: paths)
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
            entries = try await Self.loadEntries(paths: paths)
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func selectForNextLaunch(_ name: String) {
        guard entries.contains(where: { $0.name == name }) else {
            errorMessage = StoreError.invalidSelection.localizedDescription
            return
        }
        setSelectedName(name)
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
            guard let active = claimedName ?? activeName else {
                throw StoreError.activeRoot
            }
            let copiedName = try await Task.detached(
                priority: .userInitiated
            ) {
                try Self.copy(
                    paths: paths,
                    source: name,
                    active: active)
            }.value
            setSelectedName(copiedName)
            do {
                entries = try await Self.loadEntries(paths: paths)
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
            entries = try await Self.loadEntries(paths: paths)
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

    private func setSelectedName(_ name: String) {
        selectedName = name
        defaults.set(name, forKey: Self.selectedRootKey)
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
        active: String
    ) throws -> String {
        var name = [CChar](
            repeating: 0,
            count: Int(ISH_APPLE_ROOT_NAME_CAPACITY))
        let code = name.withUnsafeMutableBufferPointer { nameBuffer in
            paths.seed.withCString { seed in
                paths.parent.withCString { parent in
                    source.withCString { sourceName in
                        active.withCString { activeName in
                            ish_apple_root_catalog_copy(
                                seed, parent, sourceName,
                                activeName, nameBuffer.baseAddress)
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

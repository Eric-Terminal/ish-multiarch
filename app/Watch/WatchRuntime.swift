import Foundation
import Combine

@MainActor
final class WatchRuntime: ObservableObject {
    @Published private(set) var output = ""
    @Published private(set) var status = "准备中"
    @Published private(set) var acceptsInput = false
    @Published private(set) var revision = 0

    private enum GuestPhase: Int32 {
        case idle = 0
        case preparing = 1
        case running = 2
        case stopped = 3
        case failed = 4
    }

    private struct RuntimePaths: Sendable {
        let seed: String
        let persistentParent: String
        let socketPrefix: String
    }

    private enum SetupError: LocalizedError {
        case missingSeed
        case missingApplicationSupport
        case cannotCreateRoot(String)
        case socketPrefixTooLong

        var errorDescription: String? {
            switch self {
            case .missingSeed:
                return "App 中缺少 AArch64 Linux 种子"
            case .missingApplicationSupport:
                return "无法取得应用支持目录"
            case let .cannotCreateRoot(message):
                return "无法创建 Linux 数据目录：\(message)"
            case .socketPrefixTooLong:
                return "应用临时目录过长，无法创建本地 socket"
            }
        }
    }

    private var decoder = TerminalDecoder()
    private var pendingInput: [UInt8] = []
    private var startTask: Task<Void, Never>?
    private var started = false
    private var sawOutput = false
    private var setupFailure: String?
    private var inputFailure: String?

    func run() async {
        startIfNeeded()
        while !Task.isCancelled {
            guard poll() else { return }
            try? await Task.sleep(nanoseconds: 150_000_000)
        }
    }

    func submit(_ command: String) {
        enqueue(Array(command.utf8) + [0x0a])
    }

    func sendControlC() {
        enqueue([0x03])
    }

    func sendTab(after pendingText: String) {
        enqueue(Array(pendingText.utf8) + [0x09])
    }

    func sendEscape() {
        enqueue([0x1b])
    }

    private func startIfNeeded() {
        guard !started else { return }
        started = true
        status = "准备 Linux"

        let paths: RuntimePaths
        do {
            paths = try preparePaths()
        } catch {
            setupFailure = error.localizedDescription
            status = setupFailure ?? "准备 Linux 失败"
            return
        }

        startTask = Task { [weak self] in
            let result = await Task.detached(priority: .userInitiated) {
                Self.startRuntime(paths)
            }.value
            guard let self else { return }
            if result < 0 {
                self.publish(
                    status: Self.errorMessage(prefix: "启动失败", code: result),
                    acceptsInput: false)
            } else {
                self.status = "等待终端"
                _ = self.poll()
            }
        }
    }

    private func preparePaths() throws -> RuntimePaths {
        guard let seed = Bundle.main.url(
                forResource: "AArch64Rootfs", withExtension: "seed") else {
            throw SetupError.missingSeed
        }
        guard let applicationSupport = FileManager.default.urls(
                for: .applicationSupportDirectory,
                in: .userDomainMask).first else {
            throw SetupError.missingApplicationSupport
        }
        let roots = applicationSupport.appendingPathComponent(
            "LinuxRoots", isDirectory: true)
        do {
            try FileManager.default.createDirectory(
                at: roots, withIntermediateDirectories: true)
        } catch {
            throw SetupError.cannotCreateRoot(error.localizedDescription)
        }

        // 真机去掉等价的 /private 前缀，使容器内绝对路径落入 Darwin sun_path 上限。
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
            throw SetupError.socketPrefixTooLong
        }
        return RuntimePaths(
            seed: seed.path,
            persistentParent: roots.path,
            socketPrefix: socketPrefix)
    }

    nonisolated private static func startRuntime(
            _ paths: RuntimePaths) -> Int32 {
        paths.seed.withCString { seed in
            paths.persistentParent.withCString { persistentParent in
                paths.socketPrefix.withCString { socketPrefix in
                    ish_watch_runtime_start(
                        seed, persistentParent, socketPrefix)
                }
            }
        }
    }

    private func poll() -> Bool {
        if let setupFailure {
            publish(status: setupFailure, acceptsInput: false)
            return false
        }
        drainOutput()
        flushPendingInput()

        switch GuestPhase(rawValue: ish_watch_runtime_current_phase()) {
        case .idle:
            publish(status: "准备中", acceptsInput: false)
            return true
        case .preparing:
            publish(status: "准备 Linux", acceptsInput: false)
            return true
        case .running:
            if let inputFailure {
                publish(status: inputFailure, acceptsInput: false)
            } else {
                publish(
                    status: sawOutput ? "运行中" : "等待终端",
                    acceptsInput: sawOutput)
            }
            return true
        case .stopped:
            publish(status: "已停止", acceptsInput: false)
            return false
        case .failed:
            publish(
                status: Self.errorMessage(
                    prefix: "运行失败",
                    code: ish_watch_runtime_last_error()),
                acceptsInput: false)
            return false
        case .none:
            publish(status: "未知运行状态", acceptsInput: false)
            return false
        }
    }

    private func publish(status newStatus: String, acceptsInput newValue: Bool) {
        if status != newStatus {
            status = newStatus
        }
        if acceptsInput != newValue {
            acceptsInput = newValue
        }
    }

    private func drainOutput() {
        var buffer = [UInt8](repeating: 0, count: 4096)
        var changed = false
        for _ in 0..<16 {
            var dropped: UInt64 = 0
            let count = buffer.withUnsafeMutableBytes { rawBuffer in
                ish_watch_runtime_read_output(
                    rawBuffer.baseAddress, rawBuffer.count, &dropped)
            }
            if dropped != 0 {
                decoder.reportDroppedBytes(dropped)
                changed = true
            }
            if count != 0 {
                decoder.append(Array(buffer.prefix(count)))
                sawOutput = true
                changed = true
            }
            if count < buffer.count {
                break
            }
        }
        if changed {
            output = decoder.text
            revision &+= 1
        }
    }

    private func enqueue(_ bytes: [UInt8]) {
        guard acceptsInput else { return }
        guard pendingInput.count + bytes.count <= 16 * 1024 else {
            status = "输入队列已满"
            return
        }
        pendingInput.append(contentsOf: bytes)
        flushPendingInput()
    }

    private func flushPendingInput() {
        guard inputFailure == nil else { return }
        guard GuestPhase(rawValue: ish_watch_runtime_current_phase()) ==
                .running else { return }
        // 每轮限制调用次数，避免极小的部分写入长时间占住 Watch 主线程。
        for _ in 0..<16 where !pendingInput.isEmpty {
            let result = pendingInput.withUnsafeBytes { rawBuffer in
                ish_watch_runtime_send_input(
                    rawBuffer.baseAddress, rawBuffer.count)
            }
            if result > 0 {
                pendingInput.removeFirst(result)
            } else if result == -11 || result == 0 {
                return
            } else {
                inputFailure = Self.errorMessage(
                    prefix: "输入失败", code: result)
                pendingInput.removeAll(keepingCapacity: false)
                return
            }
        }
    }

    nonisolated private static func errorMessage(
            prefix: String, code: Int32) -> String {
        "\(prefix)（Linux errno \(-code)）"
    }

    nonisolated private static func errorMessage(
            prefix: String, code: Int) -> String {
        "\(prefix)（Linux errno \(-code)）"
    }
}

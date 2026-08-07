import Foundation
import iSHApple

private let terminalOutputChunkMaximum = 16_384
private let terminalLinuxEAGAIN: Int32 = ISH_APPLE_LINUX_EAGAIN
private let terminalLinuxESHUTDOWN: Int32 = ISH_APPLE_LINUX_ESHUTDOWN

private struct TerminalNativeHandle: @unchecked Sendable {
  let pointer: OpaquePointer
}

@available(iOS 15.0, watchOS 10.0, *)
private final class TerminalResultState: @unchecked Sendable {
  private let lock = NSLock()
  private var value: Result<TerminalResult, any Error>?
  private var waiters: [
    CheckedContinuation<TerminalResult, any Error>
  ] = []

  func complete(_ result: TerminalResult) {
    resolve(.success(result))
  }

  func fail(_ error: any Error) {
    resolve(.failure(error))
  }

  private func resolve(_ resolution: Result<TerminalResult, any Error>) {
    var pending: [
      CheckedContinuation<TerminalResult, any Error>
    ] = []
    lock.lock()
    if value == nil {
      value = resolution
      pending = waiters
      waiters.removeAll(keepingCapacity: false)
    }
    lock.unlock()
    for continuation in pending {
      continuation.resume(with: resolution)
    }
  }

  func wait() async throws -> TerminalResult {
    try await withCheckedThrowingContinuation { continuation in
      var immediate: Result<TerminalResult, any Error>?
      lock.lock()
      if let value {
        immediate = value
      } else {
        waiters.append(continuation)
      }
      lock.unlock()
      if let immediate {
        continuation.resume(with: immediate)
      }
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private final class TerminalPollingBridge: @unchecked Sendable {
  let output: TerminalOutputChannel
  let result = TerminalResultState()

  private let lock = NSLock()
  private var pollingTask: Task<Void, Never>?

  init(eventBufferByteCapacity: Int) {
    output = TerminalOutputChannel(capacity: eventBufferByteCapacity)
  }

  func start(native: OpaquePointer, terminalID: UInt64) {
    guard let retainedPointer =
      ish_apple_terminal_session_retain(native)
    else {
      let error = BridgeError.nativeFailure(
        operation: "保留交互终端",
        code: -1
      )
      result.fail(error)
      output.finish(throwing: error)
      return
    }
    let retained = TerminalNativeHandle(pointer: retainedPointer)
    let output = output
    let result = result
    let task = Task.detached(priority: .utility) {
      defer {
        ish_apple_terminal_session_release(retained.pointer)
      }
      var buffer = [UInt8](
        repeating: 0,
        count: terminalOutputChunkMaximum
      )

      while !Task.isCancelled {
        var count: UInt32 = 0
        var dropped: UInt64 = 0
        let readStatus = buffer.withUnsafeMutableBytes { bytes in
          ish_apple_terminal_session_read_output(
            retained.pointer,
            bytes.baseAddress,
            UInt32(bytes.count),
            &count,
            &dropped
          )
        }
        if readStatus == 0 && (count != 0 || dropped != 0) {
          output.send(
            TerminalOutputEvent(
              terminalID: terminalID,
              bytes: Array(buffer.prefix(Int(count))),
              droppedBytes: dropped
            )
          )
        } else if readStatus != 0 &&
          readStatus != terminalLinuxESHUTDOWN
        {
          let error = BridgeError.nativeFailure(
            operation: "读取交互终端输出",
            code: readStatus
          )
          result.fail(error)
          output.finish(throwing: error)
          return
        }

        var nativeResult = ish_apple_terminal_result_v1()
        let resultStatus = withUnsafeMutablePointer(
          to: &nativeResult
        ) { pointer in
          ish_apple_terminal_session_copy_result(
            retained.pointer,
            pointer
          )
        }
        if resultStatus == 0 {
          // 进程退出与 PTY 缓冲清空不是同一个瞬间；先把退出后仍滞留的
          // raw bytes 全部交付，再发布最终结果和结束 AsyncSequence。
          if count != 0 || dropped != 0 {
            continue
          }
          result.complete(TerminalResult(native: nativeResult))
          output.finish()
          return
        }
        if resultStatus != terminalLinuxEAGAIN {
          let error = BridgeError.nativeFailure(
            operation: "读取交互终端结果",
            code: resultStatus
          )
          result.fail(error)
          output.finish(throwing: error)
          return
        }

        do {
          try await Task<Never, Never>.sleep(
            nanoseconds: 5_000_000
          )
        } catch {
          break
        }
      }
    }
    lock.lock()
    pollingTask = task
    lock.unlock()
  }

  func stop(native: OpaquePointer) {
    lock.lock()
    let task = pollingTask
    pollingTask = nil
    lock.unlock()
    task?.cancel()
    _ = ish_apple_terminal_session_cancel(native)
    output.finish(throwing: CancellationError())
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct TerminalOutputEvents: AsyncSequence, Sendable {
  public typealias Element = TerminalOutputEvent

  private let channel: TerminalOutputChannel
  private let session: TerminalSession

  fileprivate init(
    channel: TerminalOutputChannel,
    session: TerminalSession
  ) {
    self.channel = channel
    self.session = session
  }

  public func makeAsyncIterator() -> AsyncIterator {
    AsyncIterator(channel: channel, session: session)
  }

  public struct AsyncIterator: AsyncIteratorProtocol, Sendable {
    private let channel: TerminalOutputChannel
    private let session: TerminalSession

    fileprivate init(
      channel: TerminalOutputChannel,
      session: TerminalSession
    ) {
      self.channel = channel
      self.session = session
    }

    public mutating func next() async throws -> TerminalOutputEvent? {
      let channel = channel
      let session = session
      return try await withTaskCancellationHandler {
        try await channel.next()
      } onCancel: {
        session.cancelFromTask()
      }
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public final class TerminalSession: @unchecked Sendable {
  private let native: OpaquePointer
  private let bridge: TerminalPollingBridge

  private init(
    native: OpaquePointer,
    bridge: TerminalPollingBridge,
    terminalID: UInt64
  ) {
    self.native = native
    self.bridge = bridge
    bridge.start(native: native, terminalID: terminalID)
  }

  deinit {
    bridge.stop(native: native)
    ish_apple_terminal_session_release(native)
  }

  public static func start(
    _ request: TerminalRequest,
    eventBufferByteCapacity: Int = 65_536
  ) throws -> TerminalSession {
    try TerminalABI.validate(request)
    guard eventBufferByteCapacity >= terminalOutputChunkMaximum else {
      throw BridgeError.invalidEventBufferCapacity(
        minimum: terminalOutputChunkMaximum
      )
    }

    var native: OpaquePointer?
    let status = withTerminalSpec(request) { spec in
      var spec = spec
      return withUnsafePointer(to: &spec) { pointer in
        ish_apple_terminal_session_start(pointer, &native)
      }
    }
    guard status == 0, let native else {
      throw BridgeError.nativeFailure(
        operation: "启动交互终端",
        code: status
      )
    }
    let bridge = TerminalPollingBridge(
      eventBufferByteCapacity: eventBufferByteCapacity
    )
    return TerminalSession(
      native: native,
      bridge: bridge,
      terminalID: request.terminalID
    )
  }

  public var output: TerminalOutputEvents {
    TerminalOutputEvents(channel: bridge.output, session: self)
  }

  public func send(_ bytes: [UInt8]) async throws {
    try await withTaskCancellationHandler {
      var offset = 0
      while offset < bytes.count {
        try Task.checkCancellation()
        var accepted: UInt32 = 0
        let length = min(bytes.count - offset, 1_048_576)
        let status = bytes.withUnsafeBytes { buffer in
          ish_apple_terminal_session_write_input(
            native,
            buffer.baseAddress?.advanced(by: offset),
            UInt32(length),
            &accepted
          )
        }
        if status == terminalLinuxEAGAIN ||
          (status == 0 && accepted == 0)
        {
          try await Task<Never, Never>.sleep(
            nanoseconds: 1_000_000
          )
          continue
        }
        guard status == 0 else {
          throw BridgeError.nativeFailure(
            operation: "写入交互终端",
            code: status
          )
        }
        offset += Int(accepted)
      }
    } onCancel: {
      cancelFromTask()
    }
  }

  public func finishInput() throws {
    let status = ish_apple_terminal_session_finish_input(native)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "结束交互终端输入",
        code: status
      )
    }
  }

  public func resize(columns: UInt16, rows: UInt16) throws {
    let status = ish_apple_terminal_session_resize(
      native,
      columns,
      rows
    )
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "调整交互终端尺寸",
        code: status
      )
    }
  }

  public func interrupt() throws {
    let status = ish_apple_terminal_session_interrupt(native)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "中断交互终端",
        code: status
      )
    }
  }

  public func cancel() throws {
    let status = ish_apple_terminal_session_cancel(native)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "取消交互终端",
        code: status
      )
    }
  }

  public func result() async throws -> TerminalResult {
    let result = try await withTaskCancellationHandler {
      try await bridge.result.wait()
    } onCancel: {
      cancelFromTask()
    }
    try Task.checkCancellation()
    return result
  }

  fileprivate func cancelFromTask() {
    _ = ish_apple_terminal_session_cancel(native)
    bridge.output.finish(throwing: CancellationError())
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private func withTerminalSpec<Result>(
  _ request: TerminalRequest,
  _ body: (ish_apple_terminal_spec_v1) throws -> Result
) rethrows -> Result {
  try request.executable.withCString { executable in
    try withCStringPointers(request.argv) { arguments in
      try withOptionalCStringPointers(request.environment) { environment in
        try withOptionalCString(request.workingDirectory) {
          workingDirectory in
          try body(
            ish_apple_terminal_spec_v1(
              version: TerminalABI.version,
              structure_size: UInt32(
                MemoryLayout<ish_apple_terminal_spec_v1>.size
              ),
              columns: request.columns,
              rows: request.rows,
              reserved_0: 0,
              terminal_id: request.terminalID,
              reserved: (0, 0),
              executable: executable,
              arguments: arguments,
              environment: environment,
              working_directory: workingDirectory,
              argument_count: UInt32(request.argv.count),
              environment_count: UInt32(
                request.environment.count
              )
            )
          )
        }
      }
    }
  }
}

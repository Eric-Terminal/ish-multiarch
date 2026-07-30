import Foundation
import iSHApple

private let commandOutputChunkMaximum = 16_384
private let linuxEAGAIN: Int32 = ISH_APPLE_LINUX_EAGAIN

@available(iOS 15.0, watchOS 10.0, *)
private final class CommandResultState: @unchecked Sendable {
  private let lock = NSLock()
  private var value: CommandResult?
  private var waiters: [CheckedContinuation<CommandResult, Never>] = []

  func complete(_ result: CommandResult) {
    var pending: [CheckedContinuation<CommandResult, Never>] = []

    lock.lock()
    if value == nil {
      value = result
      pending = waiters
      waiters.removeAll(keepingCapacity: false)
    }
    lock.unlock()

    for continuation in pending {
      continuation.resume(returning: result)
    }
  }

  func wait() async -> CommandResult {
    await withCheckedContinuation { continuation in
      var immediate: CommandResult?

      lock.lock()
      if let value {
        immediate = value
      } else {
        waiters.append(continuation)
      }
      lock.unlock()

      if let immediate {
        continuation.resume(returning: immediate)
      }
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private final class CommandCallbackBridge: @unchecked Sendable {
  let output: BoundedOutputChannel
  let result = CommandResultState()

  private let lock = NSLock()
  private var endedStreams: UInt8 = 0
  private var streamError: BridgeError?

  init(eventBufferByteCapacity: Int) {
    output = BoundedOutputChannel(capacity: eventBufferByteCapacity)
  }

  func receive(
    requestID: UInt64,
    rawStream: UInt32,
    bytes: UnsafeRawPointer?,
    length: UInt32,
    terminalError: Int32
  ) {
    guard let stream = CommandStream(rawValue: rawStream) else {
      output.finish(
        throwing: BridgeError.unexpectedStream(rawValue: rawStream)
      )
      return
    }

    if length > 0 {
      guard let bytes else {
        output.finish(
          throwing: BridgeError.streamFailure(
            stream: stream,
            code: -1
          )
        )
        return
      }
      let payload = Array(
        UnsafeRawBufferPointer(
          start: bytes,
          count: Int(length)
        )
      )
      output.send(
        CommandOutputEvent(
          requestID: requestID,
          stream: stream,
          bytes: payload
        )
      )
      return
    }

    var shouldFinish = false
    var error: BridgeError?

    lock.lock()
    endedStreams |= stream.endMask
    if terminalError != 0 && streamError == nil {
      streamError = .streamFailure(stream: stream, code: terminalError)
    }
    if endedStreams == 3 {
      shouldFinish = true
      error = streamError
    }
    lock.unlock()

    if shouldFinish {
      output.finish(throwing: error)
    }
  }

  func complete(_ native: ish_apple_command_result_v1) {
    result.complete(CommandResult(native: native))
  }

  func cancelOutput() {
    output.finish(throwing: CancellationError())
  }
}

private func commandStreamCallback(
  context: UnsafeMutableRawPointer?,
  session: OpaquePointer,
  requestID: UInt64,
  stream: UInt32,
  bytes: UnsafeRawPointer?,
  length: UInt32,
  terminalError: Int32
) {
  guard let context else {
    return
  }
  let bridge = Unmanaged<CommandCallbackBridge>
    .fromOpaque(context)
    .takeUnretainedValue()
  bridge.receive(
    requestID: requestID,
    rawStream: stream,
    bytes: bytes,
    length: length,
    terminalError: terminalError
  )
}

private func commandCompletionCallback(
  context: UnsafeMutableRawPointer?,
  session: OpaquePointer,
  result: UnsafePointer<ish_apple_command_result_v1>
) {
  guard let context else {
    return
  }

  // C 持有的 context 引用只在最终回调中交还；Swift start 的局部强引用
  // 因此能够覆盖“回调早于 start 返回”的合法时序。
  let bridge = Unmanaged<CommandCallbackBridge>
    .fromOpaque(context)
    .takeRetainedValue()
  bridge.complete(result.pointee)
}

@available(iOS 15.0, watchOS 10.0, *)
public struct CommandOutputEvents: AsyncSequence, Sendable {
  public typealias Element = CommandOutputEvent

  private let channel: BoundedOutputChannel
  private let session: CommandSession

  fileprivate init(channel: BoundedOutputChannel, session: CommandSession) {
    self.channel = channel
    self.session = session
  }

  public func makeAsyncIterator() -> AsyncIterator {
    AsyncIterator(channel: channel, session: session)
  }

  public struct AsyncIterator: AsyncIteratorProtocol, Sendable {
    private let channel: BoundedOutputChannel
    private let session: CommandSession

    fileprivate init(
      channel: BoundedOutputChannel,
      session: CommandSession
    ) {
      self.channel = channel
      self.session = session
    }

    public mutating func next() async throws -> CommandOutputEvent? {
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
public final class CommandSession: @unchecked Sendable {
  private let native: OpaquePointer
  private let bridge: CommandCallbackBridge

  private init(native: OpaquePointer, bridge: CommandCallbackBridge) {
    self.native = native
    self.bridge = bridge
  }

  deinit {
    _ = ish_apple_command_session_cancel(native)
    bridge.cancelOutput()
    ish_apple_command_session_release(native)
  }

  /// 固定字节容量会把消费速度反压到 guest，避免在 Swift 中无界积累输出。
  public static func start(
    _ request: CommandRequest,
    eventBufferByteCapacity: Int = 65_536
  ) throws -> CommandSession {
    try CommandABI.validate(request)
    guard eventBufferByteCapacity >= commandOutputChunkMaximum else {
      throw BridgeError.invalidEventBufferCapacity(
        minimum: commandOutputChunkMaximum
      )
    }

    let bridge = CommandCallbackBridge(
      eventBufferByteCapacity: eventBufferByteCapacity
    )
    let contextReference = Unmanaged.passRetained(bridge)
    var callbacks = ish_apple_command_callbacks_v1(
      version: CommandABI.version,
      structure_size: UInt32(
        MemoryLayout<ish_apple_command_callbacks_v1>.size
      ),
      context: contextReference.toOpaque(),
      stream: commandStreamCallback,
      completed: commandCompletionCallback,
      reserved: (0, 0)
    )
    var native: OpaquePointer?

    let status = withCommandSpec(request) { spec in
      var spec = spec
      return withUnsafePointer(to: &spec) { specPointer in
        withUnsafePointer(to: &callbacks) { callbacksPointer in
          ish_apple_command_session_start(
            specPointer,
            callbacksPointer,
            &native
          )
        }
      }
    }

    guard status == 0 else {
      contextReference.release()
      throw BridgeError.nativeFailure(
        operation: "启动结构化命令",
        code: status
      )
    }

    return CommandSession(native: native!, bridge: bridge)
  }

  public var output: CommandOutputEvents {
    CommandOutputEvents(channel: bridge.output, session: self)
  }

  public func send(_ bytes: [UInt8]) async throws {
    try await withTaskCancellationHandler {
      var offset = 0
      while offset < bytes.count {
        try Task.checkCancellation()
        var accepted: UInt32 = 0
        let length = min(bytes.count - offset, 1_048_576)
        let status = bytes.withUnsafeBytes { buffer in
          ish_apple_command_session_write_stdin(
            native,
            buffer.baseAddress?.advanced(by: offset),
            UInt32(length),
            &accepted
          )
        }

        if status == linuxEAGAIN || (status == 0 && accepted == 0) {
          try await Task<Never, Never>.sleep(
            nanoseconds: 1_000_000
          )
          continue
        }
        guard status == 0 else {
          throw BridgeError.nativeFailure(
            operation: "写入命令 stdin",
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
    let status = ish_apple_command_session_close_stdin(native)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "关闭命令 stdin",
        code: status
      )
    }
  }

  public func interrupt() throws {
    let status = ish_apple_command_session_interrupt(native)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "中断命令",
        code: status
      )
    }
  }

  public func cancel() throws {
    let status = ish_apple_command_session_cancel(native)
    if status == 0 {
      bridge.cancelOutput()
    }
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "取消命令",
        code: status
      )
    }
  }

  /// 有界输出通道会施加反压；应与 `output` 的消费任务并发等待结果。
  public func result() async throws -> CommandResult {
    let result = await withTaskCancellationHandler {
      await bridge.result.wait()
    } onCancel: {
      cancelFromTask()
    }
    try Task.checkCancellation()
    return result
  }

  fileprivate func cancelFromTask() {
    _ = ish_apple_command_session_cancel(native)
    bridge.cancelOutput()
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private func withCommandSpec<Result>(
  _ request: CommandRequest,
  _ body: (ish_apple_command_spec_v1) throws -> Result
) rethrows -> Result {
  try request.executable.withCString { executable in
    try withCStringPointers(request.argv) { arguments in
      try withOptionalCStringPointers(request.environment) { environment in
        try withOptionalCString(request.workingDirectory) {
          workingDirectory in
          try body(
            ish_apple_command_spec_v1(
              version: CommandABI.version,
              structure_size: UInt32(
                MemoryLayout<ish_apple_command_spec_v1>.size
              ),
              timeout_milliseconds: request.timeoutMilliseconds,
              reserved_0: 0,
              request_id: request.requestID,
              output_byte_limit: request.outputLimitBytes,
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

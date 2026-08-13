import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public enum DiagnosticScope: Sendable, Equatable {
  case runtime
  case command(requestID: UInt64)
  case terminal(terminalID: UInt64)
  case guestFile(requestID: UInt64)
  case unknown(rawValue: UInt32, requestID: UInt64)

  var native: (scope: UInt32, requestID: UInt64) {
    switch self {
    case .runtime:
      return (ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME, 0)
    case .command(let requestID):
      return (ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND, requestID)
    case .terminal(let terminalID):
      return (ISH_APPLE_DIAGNOSTIC_SCOPE_TERMINAL, terminalID)
    case .guestFile(let requestID):
      return (ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE, requestID)
    case .unknown(let rawValue, let requestID):
      return (rawValue, requestID)
    }
  }

  init(nativeScope: UInt32, requestID: UInt64) {
    switch nativeScope {
    case ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME:
      self = .runtime
    case ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND:
      self = .command(requestID: requestID)
    case ISH_APPLE_DIAGNOSTIC_SCOPE_TERMINAL:
      self = .terminal(terminalID: requestID)
    case ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE:
      self = .guestFile(requestID: requestID)
    default:
      self = .unknown(
        rawValue: nativeScope,
        requestID: requestID
      )
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public enum DiagnosticCategory: Sendable, Equatable {
  case instruction
  case syscall
  case filesystem
  case runtime
  case unknown(UInt32)

  init(rawValue: UInt32) {
    switch rawValue {
    case ISH_APPLE_DIAGNOSTIC_CATEGORY_INSTRUCTION:
      self = .instruction
    case ISH_APPLE_DIAGNOSTIC_CATEGORY_SYSCALL:
      self = .syscall
    case ISH_APPLE_DIAGNOSTIC_CATEGORY_FILESYSTEM:
      self = .filesystem
    case ISH_APPLE_DIAGNOSTIC_CATEGORY_RUNTIME:
      self = .runtime
    default:
      self = .unknown(rawValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public enum DiagnosticKind: Sendable, Equatable {
  case undefinedInstruction
  case unsupportedSyscall
  case unsupportedFilesystemOperation
  case runtimeStartFailed
  case runtimeBridgeFailed
  case unknown(category: UInt32, kind: UInt32)

  init(category: UInt32, kind: UInt32) {
    switch (category, kind) {
    case (
      ISH_APPLE_DIAGNOSTIC_CATEGORY_INSTRUCTION,
      ISH_APPLE_DIAGNOSTIC_INSTRUCTION_UNDEFINED
    ):
      self = .undefinedInstruction
    case (
      ISH_APPLE_DIAGNOSTIC_CATEGORY_SYSCALL,
      ISH_APPLE_DIAGNOSTIC_SYSCALL_UNSUPPORTED
    ):
      self = .unsupportedSyscall
    case (
      ISH_APPLE_DIAGNOSTIC_CATEGORY_FILESYSTEM,
      ISH_APPLE_DIAGNOSTIC_FILESYSTEM_UNSUPPORTED
    ):
      self = .unsupportedFilesystemOperation
    case (
      ISH_APPLE_DIAGNOSTIC_CATEGORY_RUNTIME,
      ISH_APPLE_DIAGNOSTIC_RUNTIME_START_FAILED
    ):
      self = .runtimeStartFailed
    case (
      ISH_APPLE_DIAGNOSTIC_CATEGORY_RUNTIME,
      ISH_APPLE_DIAGNOSTIC_RUNTIME_BRIDGE_FAILED
    ):
      self = .runtimeBridgeFailed
    default:
      self = .unknown(category: category, kind: kind)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public enum GuestArchitecture: Sendable, Equatable {
  case aarch64
  case unknown(UInt32)

  init(rawValue: UInt32) {
    if rawValue == ISH_APPLE_DIAGNOSTIC_ARCHITECTURE_AARCH64 {
      self = .aarch64
    } else {
      self = .unknown(rawValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public enum RuntimeBackend: Sendable, Equatable {
  case c
  case threaded
  case unknown(UInt32)

  init(rawValue: UInt32) {
    switch rawValue {
    case ISH_APPLE_DIAGNOSTIC_BACKEND_C:
      self = .c
    case ISH_APPLE_DIAGNOSTIC_BACKEND_THREADED:
      self = .threaded
    default:
      self = .unknown(rawValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct DiagnosticEvent: Sendable, Equatable {
  public let sequence: UInt64
  public let scope: DiagnosticScope
  public let requestID: UInt64
  public let category: DiagnosticCategory
  public let kind: DiagnosticKind
  public let guestPC: UInt64
  public let opcode: UInt32
  public let syscallNumber: UInt64
  public let syscallName: String
  public let linuxError: Int32
  public let signal: Int32
  public let guestProcessID: UInt32
  public let guestThreadGroupID: UInt32
  public let processName: String
  public let architecture: GuestArchitecture
  public let backend: RuntimeBackend
  public let buildIdentity: String

  init(native: ish_apple_diagnostic_event_v1) {
    sequence = native.sequence
    scope = DiagnosticScope(
      nativeScope: native.scope,
      requestID: native.request_id
    )
    requestID = native.request_id
    category = DiagnosticCategory(rawValue: native.category)
    kind = DiagnosticKind(
      category: native.category,
      kind: native.kind
    )
    guestPC = native.guest_pc
    opcode = native.opcode
    syscallNumber = native.syscall_number
    syscallName = decodeDiagnosticCString(native.syscall_name)
    linuxError = native.linux_error
    signal = native.signal
    guestProcessID = native.guest_process_id
    guestThreadGroupID = native.guest_thread_group_id
    processName = decodeDiagnosticCString(native.process_name)
    architecture = GuestArchitecture(rawValue: native.architecture)
    backend = RuntimeBackend(rawValue: native.backend)
    buildIdentity = decodeDiagnosticCString(native.build_identity)
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public actor RuntimeDiagnostics {
  public init() {}

  public func pendingCount(for scope: DiagnosticScope) throws -> UInt32 {
    let query = scope.native
    var count: UInt32 = 0
    let status = ish_apple_diagnostics_drain(
      query.scope,
      query.requestID,
      nil,
      0,
      &count
    )
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "查询 Linux 兼容性诊断",
        code: status
      )
    }
    return count
  }

  /// 分批消费，不限制底层待处理事件总数。
  public func drain(
    _ scope: DiagnosticScope,
    maximumEventCount: UInt32 = 128
  ) throws -> [DiagnosticEvent] {
    guard maximumEventCount != 0,
      UInt64(maximumEventCount) <= UInt64(Int.max)
    else {
      throw BridgeError.invalidArguments
    }
    let query = scope.native
    var nativeEvents = [ish_apple_diagnostic_event_v1](
      repeating: ish_apple_diagnostic_event_v1(),
      count: Int(maximumEventCount)
    )
    var count: UInt32 = 0
    let status = nativeEvents.withUnsafeMutableBufferPointer { buffer in
      ish_apple_diagnostics_drain(
        query.scope,
        query.requestID,
        buffer.baseAddress,
        maximumEventCount,
        &count
      )
    }
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "消费 Linux 兼容性诊断",
        code: status
      )
    }
    return nativeEvents.prefix(Int(count)).map(DiagnosticEvent.init)
  }

  @discardableResult
  public func clear(_ scope: DiagnosticScope) throws -> UInt32 {
    let query = scope.native
    var count: UInt32 = 0
    let status = ish_apple_diagnostics_clear(
      query.scope,
      query.requestID,
      &count
    )
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "清理 Linux 兼容性诊断",
        code: status
      )
    }
    return count
  }
}

private func decodeDiagnosticCString<Value>(_ value: Value) -> String {
  withUnsafeBytes(of: value) { bytes in
    let end = bytes.firstIndex(of: 0) ?? bytes.endIndex
    return String(decoding: bytes[..<end], as: UTF8.self)
  }
}

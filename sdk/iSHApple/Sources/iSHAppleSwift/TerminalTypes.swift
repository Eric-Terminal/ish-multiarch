import iSHApple

enum TerminalABI {
  static let version: UInt32 = 1
  static let argumentCountMaximum = 4_096
  static let exited: Int32 = 1
  static let signaled: Int32 = 2
  static let cancelled: Int32 = 3
  static let runtimeFailure: Int32 = 4
}

@available(iOS 15.0, watchOS 10.0, *)
public struct TerminalRequest: Sendable, Equatable {
  public var terminalID: UInt64
  public var executable: String
  public var argv: [String]
  public var environment: [String]
  public var workingDirectory: String?
  public var columns: UInt16
  public var rows: UInt16

  public init(
    terminalID: UInt64,
    executable: String,
    argv: [String],
    environment: [String] = [],
    workingDirectory: String? = nil,
    columns: UInt16 = 80,
    rows: UInt16 = 24
  ) {
    self.terminalID = terminalID
    self.executable = executable
    self.argv = argv
    self.environment = environment
    self.workingDirectory = workingDirectory
    self.columns = columns
    self.rows = rows
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct TerminalOutputEvent: Sendable, Equatable {
  public let terminalID: UInt64
  public let bytes: [UInt8]
  public let droppedBytes: UInt64
}

@available(iOS 15.0, watchOS 10.0, *)
public enum TerminalCompletionReason: Sendable, Equatable {
  case exited
  case signaled
  case cancelled
  case runtimeFailure
  case unknown(Int32)

  init(rawValue: Int32) {
    switch rawValue {
    case TerminalABI.exited:
      self = .exited
    case TerminalABI.signaled:
      self = .signaled
    case TerminalABI.cancelled:
      self = .cancelled
    case TerminalABI.runtimeFailure:
      self = .runtimeFailure
    default:
      self = .unknown(rawValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct TerminalResult: Sendable, Equatable {
  public let terminalID: UInt64
  public let reason: TerminalCompletionReason
  public let exitCode: Int32
  public let terminationSignal: Int32
  public let errorCode: Int32
  public let outputBytes: UInt64
  public let droppedBytes: UInt64
  public let elapsedMilliseconds: UInt64

  init(native: ish_apple_terminal_result_v1) {
    terminalID = native.terminal_id
    reason = TerminalCompletionReason(rawValue: native.reason)
    exitCode = native.exit_code
    terminationSignal = native.termination_signal
    errorCode = native.error
    outputBytes = native.output_bytes
    droppedBytes = native.dropped_bytes
    elapsedMilliseconds = native.elapsed_milliseconds
  }
}

extension TerminalABI {
  static func validate(_ request: TerminalRequest) throws {
    guard request.terminalID != 0,
      request.columns != 0,
      request.rows != 0,
      !request.argv.isEmpty
    else {
      throw BridgeError.invalidArguments
    }
    guard request.argv.count <= argumentCountMaximum,
      request.environment.count <= argumentCountMaximum
    else {
      throw BridgeError.tooManyArguments(limit: argumentCountMaximum)
    }

    try CommandABI.validateCString(
      request.executable,
      field: "executable"
    )
    for argument in request.argv {
      try CommandABI.validateCString(argument, field: "argv")
    }
    for variable in request.environment {
      try CommandABI.validateCString(variable, field: "environment")
    }
    if let workingDirectory = request.workingDirectory {
      try CommandABI.validateCString(
        workingDirectory,
        field: "workingDirectory"
      )
    }
  }
}

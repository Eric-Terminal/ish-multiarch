import iSHApple

enum CommandABI {
  static let version: UInt32 = 1
  static let argumentCountMaximum = 4_096
  static let stdout: UInt32 = 1
  static let stderr: UInt32 = 2
  static let exited: Int32 = 1
  static let signaled: Int32 = 2
  static let cancelled: Int32 = 3
  static let timedOut: Int32 = 4
  static let outputLimit: Int32 = 5
  static let runtimeFailure: Int32 = 6
}

@available(iOS 15.0, watchOS 10.0, *)
public struct CommandRequest: Sendable, Equatable {
  public var requestID: UInt64
  public var executable: String
  public var argv: [String]
  public var environment: [String]
  public var workingDirectory: String?
  public var timeoutMilliseconds: UInt32
  public var outputLimitBytes: UInt64

  public init(
    requestID: UInt64,
    executable: String,
    argv: [String],
    environment: [String] = [],
    workingDirectory: String? = nil,
    timeoutMilliseconds: UInt32 = 0,
    outputLimitBytes: UInt64 = 0
  ) {
    self.requestID = requestID
    self.executable = executable
    self.argv = argv
    self.environment = environment
    self.workingDirectory = workingDirectory
    self.timeoutMilliseconds = timeoutMilliseconds
    self.outputLimitBytes = outputLimitBytes
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public enum CommandStream: Sendable, Equatable {
  case stdout
  case stderr

  init?(rawValue: UInt32) {
    switch rawValue {
    case CommandABI.stdout:
      self = .stdout
    case CommandABI.stderr:
      self = .stderr
    default:
      return nil
    }
  }

  var endMask: UInt8 {
    switch self {
    case .stdout:
      return 1
    case .stderr:
      return 2
    }
  }

  var description: String {
    switch self {
    case .stdout:
      return "stdout"
    case .stderr:
      return "stderr"
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct CommandOutputEvent: Sendable, Equatable {
  public let requestID: UInt64
  public let stream: CommandStream
  public let bytes: [UInt8]
}

@available(iOS 15.0, watchOS 10.0, *)
public enum CommandCompletionReason: Sendable, Equatable {
  case exited
  case signaled
  case cancelled
  case timedOut
  case outputLimit
  case runtimeFailure
  case unknown(Int32)

  init(rawValue: Int32) {
    switch rawValue {
    case CommandABI.exited:
      self = .exited
    case CommandABI.signaled:
      self = .signaled
    case CommandABI.cancelled:
      self = .cancelled
    case CommandABI.timedOut:
      self = .timedOut
    case CommandABI.outputLimit:
      self = .outputLimit
    case CommandABI.runtimeFailure:
      self = .runtimeFailure
    default:
      self = .unknown(rawValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct CommandResult: Sendable, Equatable {
  public let requestID: UInt64
  public let reason: CommandCompletionReason
  public let exitCode: Int32
  public let terminationSignal: Int32
  public let errorCode: Int32
  public let stdoutBytes: UInt64
  public let stderrBytes: UInt64
  public let elapsedMilliseconds: UInt64

  init(native: ish_apple_command_result_v1) {
    requestID = native.request_id
    reason = CommandCompletionReason(rawValue: native.reason)
    exitCode = native.exit_code
    terminationSignal = native.termination_signal
    errorCode = native.error
    stdoutBytes = native.stdout_bytes
    stderrBytes = native.stderr_bytes
    elapsedMilliseconds = native.elapsed_milliseconds
  }
}

extension CommandABI {
  static func validate(_ request: CommandRequest) throws {
    guard !request.argv.isEmpty else {
      throw BridgeError.invalidArguments
    }
    guard request.argv.count <= argumentCountMaximum,
      request.environment.count <= argumentCountMaximum
    else {
      throw BridgeError.tooManyArguments(limit: argumentCountMaximum)
    }

    try validateCString(request.executable, field: "executable")
    for argument in request.argv {
      try validateCString(argument, field: "argv")
    }
    for variable in request.environment {
      try validateCString(variable, field: "environment")
    }
    if let workingDirectory = request.workingDirectory {
      try validateCString(workingDirectory, field: "workingDirectory")
    }
  }

  static func validateCString(_ value: String, field: String) throws {
    if value.utf8.contains(0) {
      throw BridgeError.embeddedNull(field: field)
    }
  }
}

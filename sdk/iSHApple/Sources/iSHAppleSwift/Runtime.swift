import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public struct RuntimeConfiguration: Sendable, Equatable {
  public var rootData: String
  public var sharedDirectory: String
  public var socketPrefix: String
  public var hostname: String
  public var bootCommand: String

  public init(
    rootData: String,
    sharedDirectory: String,
    socketPrefix: String,
    hostname: String,
    bootCommand: String
  ) {
    self.rootData = rootData
    self.sharedDirectory = sharedDirectory
    self.socketPrefix = socketPrefix
    self.hostname = hostname
    self.bootCommand = bootCommand
  }
}

/// 串行化进程级 Linux runtime 的启动。
@available(iOS 15.0, watchOS 10.0, *)
public actor Runtime {
  public init() {}

  public func start(_ configuration: RuntimeConfiguration) throws {
    try validate(configuration)

    let status = configuration.rootData.withCString { rootData in
      configuration.sharedDirectory.withCString { sharedDirectory in
        configuration.socketPrefix.withCString { socketPrefix in
          configuration.hostname.withCString { hostname in
            configuration.bootCommand.withCString { bootCommand in
              var spec = ish_apple_runtime_spec_v1(
                version: CommandABI.version,
                structure_size: UInt32(
                  MemoryLayout<ish_apple_runtime_spec_v1>.size
                ),
                reserved: (0, 0),
                root_data: rootData,
                shared_directory: sharedDirectory,
                socket_prefix: socketPrefix,
                hostname: hostname,
                boot_command: bootCommand
              )
              return withUnsafePointer(to: &spec) {
                ish_apple_runtime_start($0)
              }
            }
          }
        }
      }
    }

    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "启动 Linux runtime",
        code: status
      )
    }
  }

  public var phase: Int32 {
    ish_apple_runtime_current_phase()
  }

  public var lastError: Int32 {
    ish_apple_runtime_last_error()
  }

  private func validate(_ configuration: RuntimeConfiguration) throws {
    try CommandABI.validateCString(configuration.rootData, field: "rootData")
    try CommandABI.validateCString(
      configuration.sharedDirectory,
      field: "sharedDirectory"
    )
    try CommandABI.validateCString(
      configuration.socketPrefix,
      field: "socketPrefix"
    )
    try CommandABI.validateCString(configuration.hostname, field: "hostname")
    try CommandABI.validateCString(
      configuration.bootCommand,
      field: "bootCommand"
    )
  }
}

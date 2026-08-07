import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public struct RuntimeConfiguration: Sendable, Equatable {
  public var rootData: String
  public var sharedDirectory: String
  public var socketPrefix: String
  public var hostname: String
  public var bootCommand: String
  public var startupMounts: [RuntimeMountConfiguration]

  public init(
    rootData: String,
    sharedDirectory: String,
    socketPrefix: String,
    hostname: String,
    bootCommand: String,
    startupMounts: [RuntimeMountConfiguration] = []
  ) {
    self.rootData = rootData
    self.sharedDirectory = sharedDirectory
    self.socketPrefix = socketPrefix
    self.hostname = hostname
    self.bootCommand = bootCommand
    self.startupMounts = startupMounts
  }
}

/// 串行化进程级 Linux runtime 的启动。
@available(iOS 15.0, watchOS 10.0, *)
public actor Runtime {
  public init() {}

  public func start(_ configuration: RuntimeConfiguration) throws {
    try validate(configuration)

    let status = try withNativeMountSpecs(
      configuration.startupMounts
    ) { mounts, mountCount in
      configuration.rootData.withCString { rootData in
        configuration.sharedDirectory.withCString { sharedDirectory in
          configuration.socketPrefix.withCString { socketPrefix in
            configuration.hostname.withCString { hostname in
              configuration.bootCommand.withCString { bootCommand in
                var spec = ish_apple_runtime_spec_v2(
                  version: CommandABI.version,
                  structure_size: UInt32(
                    MemoryLayout<ish_apple_runtime_spec_v2>.size
                  ),
                  reserved: (0, 0),
                  root_data: rootData,
                  shared_directory: sharedDirectory,
                  socket_prefix: socketPrefix,
                  hostname: hostname,
                  boot_command: bootCommand,
                  mounts: mounts,
                  mount_count: mountCount,
                  reserved_0: 0
                )
                return withUnsafePointer(to: &spec) {
                  ish_apple_runtime_start_v2($0)
                }
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
    for mount in configuration.startupMounts {
      try validateMountConfiguration(mount)
    }
  }
}

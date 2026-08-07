import Foundation
import iSHApple

enum MountABI {
  static let version: UInt32 = 1
  static let readOnly: Int32 = ISH_APPLE_MOUNT_ACCESS_READ_ONLY
  static let readWrite: Int32 = ISH_APPLE_MOUNT_ACCESS_READ_WRITE
}

@available(iOS 15.0, watchOS 10.0, *)
public enum MountAccess: Sendable, Equatable {
  case readOnly
  case readWrite

  var nativeValue: Int32 {
    switch self {
    case .readOnly:
      return MountABI.readOnly
    case .readWrite:
      return MountABI.readWrite
    }
  }

  init(nativeValue: Int32) {
    self = nativeValue == MountABI.readOnly ? .readOnly : .readWrite
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct RuntimeMountConfiguration: Sendable, Equatable {
  public var id: UUID
  public var hostDirectoryDescriptor: Int32
  public var guestDirectory: String
  public var access: MountAccess

  public init(
    id: UUID,
    hostDirectoryDescriptor: Int32,
    guestDirectory: String,
    access: MountAccess
  ) {
    self.id = id
    self.hostDirectoryDescriptor = hostDirectoryDescriptor
    self.guestDirectory = guestDirectory
    self.access = access
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public enum RuntimeMountState: Sendable, Equatable {
  case staged
  case active
  case draining
  case removed
  case unknown(Int32)

  init(nativeValue: Int32) {
    switch nativeValue {
    case ISH_APPLE_MOUNT_STATE_STAGED:
      self = .staged
    case ISH_APPLE_MOUNT_STATE_ACTIVE:
      self = .active
    case ISH_APPLE_MOUNT_STATE_DRAINING:
      self = .draining
    case ISH_APPLE_MOUNT_STATE_REMOVED:
      self = .removed
    default:
      self = .unknown(nativeValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct RuntimeMountInfo: Sendable, Equatable {
  public let id: UUID
  public let guestDirectory: String
  public let access: MountAccess
  public let state: RuntimeMountState
  public let activeLeases: UInt64
  public let activeReferences: UInt64
}

func nativeMountID(_ id: UUID) -> ish_apple_mount_id {
  var bytes = id.uuid
  return withUnsafeBytes(of: &bytes) { buffer in
    var high: UInt64 = 0
    var low: UInt64 = 0
    for byte in buffer.prefix(8) {
      high = (high << 8) | UInt64(byte)
    }
    for byte in buffer.dropFirst(8) {
      low = (low << 8) | UInt64(byte)
    }
    return ish_apple_mount_id(high: high, low: low)
  }
}

func uuidFromNativeMountID(_ id: ish_apple_mount_id) -> UUID {
  var bytes = [UInt8](repeating: 0, count: 16)
  for index in 0..<8 {
    let shift = UInt64((7 - index) * 8)
    bytes[index] = UInt8(truncatingIfNeeded: id.high >> shift)
    bytes[index + 8] = UInt8(truncatingIfNeeded: id.low >> shift)
  }
  return UUID(
    uuid: (
      bytes[0], bytes[1], bytes[2], bytes[3],
      bytes[4], bytes[5], bytes[6], bytes[7],
      bytes[8], bytes[9], bytes[10], bytes[11],
      bytes[12], bytes[13], bytes[14], bytes[15]
    )
  )
}

@available(iOS 15.0, watchOS 10.0, *)
func validateMountConfiguration(
  _ configuration: RuntimeMountConfiguration
) throws {
  guard configuration.hostDirectoryDescriptor >= 0 else {
    throw BridgeError.invalidArguments
  }
  try CommandABI.validateCString(
    configuration.guestDirectory,
    field: "guestDirectory"
  )
}

@available(iOS 15.0, watchOS 10.0, *)
func withNativeMountSpecs<Result>(
  _ configurations: [RuntimeMountConfiguration],
  _ body: (
    UnsafePointer<ish_apple_mount_spec_v1>?, UInt32
  ) throws -> Result
) throws -> Result {
  guard UInt64(configurations.count) <= UInt64(UInt32.max) else {
    throw BridgeError.invalidArguments
  }
  for configuration in configurations {
    try validateMountConfiguration(configuration)
  }
  if configurations.isEmpty {
    return try body(nil, 0)
  }

  return try withCStringPointers(
    configurations.map(\.guestDirectory)
  ) { guestDirectories in
    var specs: [ish_apple_mount_spec_v1] = []
    specs.reserveCapacity(configurations.count)
    for (index, configuration) in configurations.enumerated() {
      specs.append(
        ish_apple_mount_spec_v1(
          version: MountABI.version,
          structure_size: UInt32(
            MemoryLayout<ish_apple_mount_spec_v1>.size
          ),
          reserved: (0, 0),
          mount_id: nativeMountID(configuration.id),
          access: configuration.access.nativeValue,
          host_directory_fd: configuration.hostDirectoryDescriptor,
          guest_directory: guestDirectories[index]
        )
      )
    }
    return try specs.withUnsafeBufferPointer { buffer in
      try body(buffer.baseAddress, UInt32(buffer.count))
    }
  }
}

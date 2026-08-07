import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public enum GuestFileType: Sendable, Equatable {
  case regular
  case directory
  case symbolicLink
  case blockDevice
  case characterDevice
  case fifo
  case socket
  case unknown(UInt32)

  init(mode: UInt32) {
    switch mode & 0o170000 {
    case 0o100000:
      self = .regular
    case 0o040000:
      self = .directory
    case 0o120000:
      self = .symbolicLink
    case 0o060000:
      self = .blockDevice
    case 0o020000:
      self = .characterDevice
    case 0o010000:
      self = .fifo
    case 0o140000:
      self = .socket
    default:
      self = .unknown(mode & 0o170000)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct GuestFileTimestamp: Sendable, Equatable {
  public let seconds: Int64
  public let nanoseconds: UInt32
}

@available(iOS 15.0, watchOS 10.0, *)
public struct GuestFileInfo: Sendable, Equatable {
  public let requestID: UInt64
  public let type: GuestFileType
  public let mode: UInt32
  public let device: UInt64
  public let inode: UInt64
  public let size: UInt64
  public let blocks: UInt64
  public let linkCount: UInt32
  public let userID: UInt32
  public let groupID: UInt32
  public let blockSize: UInt32
  public let accessTime: GuestFileTimestamp
  public let modificationTime: GuestFileTimestamp
  public let statusChangeTime: GuestFileTimestamp

  init(native: ish_apple_guest_file_info_v1) {
    requestID = native.request_id
    type = GuestFileType(mode: native.mode)
    mode = native.mode
    device = native.device
    inode = native.inode
    size = native.size
    blocks = native.blocks
    linkCount = native.link_count
    userID = native.user_id
    groupID = native.group_id
    blockSize = native.block_size
    accessTime = GuestFileTimestamp(
      seconds: native.access_time_seconds,
      nanoseconds: native.access_time_nanoseconds
    )
    modificationTime = GuestFileTimestamp(
      seconds: native.modification_time_seconds,
      nanoseconds: native.modification_time_nanoseconds
    )
    statusChangeTime = GuestFileTimestamp(
      seconds: native.status_change_time_seconds,
      nanoseconds: native.status_change_time_nanoseconds
    )
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct GuestDirectoryEntry: Sendable, Equatable {
  public let name: String
  public let info: GuestFileInfo
}

@available(iOS 15.0, watchOS 10.0, *)
public struct GuestDirectoryPage: Sendable, Equatable {
  public let entries: [GuestDirectoryEntry]
  public let nextCursor: UInt64
  public let isAtEnd: Bool
}

@available(iOS 15.0, watchOS 10.0, *)
public struct GuestFileReadPage: Sendable, Equatable {
  public let offset: UInt64
  public let bytes: [UInt8]
  public let totalSize: UInt64
  public let isAtEnd: Bool
}

@available(iOS 15.0, watchOS 10.0, *)
func withNativeGuestFileRequest<Result>(
  path: String,
  requestID: UInt64,
  followSymbolicLinks: Bool,
  _ body: (
    UnsafePointer<ish_apple_guest_file_request_v1>
  ) throws -> Result
) throws -> Result {
  guard requestID != 0,
    path.hasPrefix("/"),
    UInt64(path.utf8.count) <=
      UInt64(ISH_APPLE_GUEST_FILE_PATH_BYTES_MAX)
  else {
    throw BridgeError.invalidArguments
  }
  guard !path.utf8.contains(0) else {
    throw BridgeError.embeddedNull(field: "path")
  }

  return try path.withCString { pathPointer in
    var request = ish_apple_guest_file_request_v1(
      version: ISH_APPLE_ABI_VERSION,
      structure_size: UInt32(
        MemoryLayout<ish_apple_guest_file_request_v1>.size
      ),
      flags: followSymbolicLinks ?
        0 : ISH_APPLE_GUEST_FILE_REQUEST_NOFOLLOW,
      reserved_0: 0,
      request_id: requestID,
      reserved: (0, 0),
      path: pathPointer
    )
    return try withUnsafePointer(to: &request, body)
  }
}

@available(iOS 15.0, watchOS 10.0, *)
func decodeGuestDirectoryEntry(
  _ native: ish_apple_guest_file_directory_entry_v1
) throws -> GuestDirectoryEntry {
  guard native.name_bytes <= ISH_APPLE_GUEST_FILE_NAME_BYTES_MAX else {
    throw BridgeError.nativeFailure(
      operation: "解码 Linux 目录项",
      code: ISH_APPLE_LINUX_EOVERFLOW
    )
  }
  var copy = native
  let name = withUnsafeBytes(of: &copy.name) { bytes in
    String(
      decoding: bytes.prefix(Int(native.name_bytes)),
      as: UTF8.self
    )
  }
  return GuestDirectoryEntry(
    name: name,
    info: GuestFileInfo(native: native.info)
  )
}

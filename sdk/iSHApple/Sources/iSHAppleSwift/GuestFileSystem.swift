import iSHApple

/// 同步 guest 文件接口；App 应从后台任务调用，避免把文件 I/O 放进 UI 渲染链。
@available(iOS 15.0, watchOS 10.0, *)
public struct GuestFileSystem: Sendable {
  public init() {}

  public func stat(
    path: String,
    requestID: UInt64,
    followSymbolicLinks: Bool = false
  ) throws -> GuestFileInfo {
    var native = ish_apple_guest_file_info_v1()
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: followSymbolicLinks
    ) { request in
      ish_apple_guest_file_stat(request, &native)
    }
    try requireSuccess(status, operation: "读取 Linux 文件状态")
    return GuestFileInfo(native: native)
  }

  public func list(
    path: String,
    requestID: UInt64,
    cursor: UInt64 = 0,
    maximumEntryCount: UInt32 = 128,
    followSymbolicLinks: Bool = true
  ) throws -> GuestDirectoryPage {
    guard maximumEntryCount != 0,
      UInt64(maximumEntryCount) <= UInt64(Int.max)
    else {
      throw BridgeError.invalidArguments
    }
    var nativeEntries = [ish_apple_guest_file_directory_entry_v1](
      repeating: ish_apple_guest_file_directory_entry_v1(),
      count: Int(maximumEntryCount)
    )
    var count: UInt32 = 0
    var nextCursor = cursor
    var eof: Int32 = 0
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: followSymbolicLinks
    ) { request in
      nativeEntries.withUnsafeMutableBufferPointer { buffer in
        ish_apple_guest_file_list(
          request,
          cursor,
          buffer.baseAddress!,
          maximumEntryCount,
          &count,
          &nextCursor,
          &eof
        )
      }
    }
    try requireSuccess(status, operation: "列出 Linux 目录")
    return GuestDirectoryPage(
      entries: try nativeEntries.prefix(Int(count)).map(
        decodeGuestDirectoryEntry
      ),
      nextCursor: nextCursor,
      isAtEnd: eof != 0
    )
  }

  public func read(
    path: String,
    requestID: UInt64,
    offset: UInt64 = 0,
    maximumByteCount: UInt32 = 64 * 1024,
    followSymbolicLinks: Bool = true
  ) throws -> GuestFileReadPage {
    guard UInt64(maximumByteCount) <= UInt64(Int.max) else {
      throw BridgeError.invalidArguments
    }
    var bytes = [UInt8](
      repeating: 0,
      count: Int(maximumByteCount)
    )
    var count: UInt32 = 0
    var totalSize: UInt64 = 0
    var eof: Int32 = 0
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: followSymbolicLinks
    ) { request in
      bytes.withUnsafeMutableBytes { buffer in
        ish_apple_guest_file_read(
          request,
          offset,
          buffer.baseAddress,
          maximumByteCount,
          &count,
          &totalSize,
          &eof
        )
      }
    }
    try requireSuccess(status, operation: "读取 Linux 文件")
    return GuestFileReadPage(
      offset: offset,
      bytes: Array(bytes.prefix(Int(count))),
      totalSize: totalSize,
      isAtEnd: eof != 0
    )
  }

  public func write(
    path: String,
    requestID: UInt64,
    bytes: [UInt8],
    mode: UInt32 = 0o644
  ) throws {
    guard UInt64(bytes.count) <= UInt64(UInt32.max) else {
      throw BridgeError.invalidArguments
    }
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: false
    ) { request in
      bytes.withUnsafeBytes { buffer in
        ish_apple_guest_file_write(
          request,
          buffer.baseAddress,
          UInt32(buffer.count),
          mode
        )
      }
    }
    try requireSuccess(status, operation: "原子写入 Linux 文件")
  }

  public func edit(
    path: String,
    requestID: UInt64,
    offset: UInt64,
    removedByteCount: UInt64,
    replacement: [UInt8]
  ) throws {
    guard UInt64(replacement.count) <= UInt64(UInt32.max) else {
      throw BridgeError.invalidArguments
    }
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: false
    ) { request in
      replacement.withUnsafeBytes { buffer in
        ish_apple_guest_file_edit(
          request,
          offset,
          removedByteCount,
          buffer.baseAddress,
          UInt32(buffer.count)
        )
      }
    }
    try requireSuccess(status, operation: "编辑 Linux 文件")
  }

  /// 在 guest 内流式复制普通文件，内存占用不随文件大小增长。
  public func copy(
    path: String,
    to destination: String,
    requestID: UInt64
  ) throws {
    guard !destination.utf8.contains(0) else {
      throw BridgeError.embeddedNull(field: "destination")
    }
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: false
    ) { request in
      destination.withCString { destinationPointer in
        ish_apple_guest_file_copy(request, destinationPointer)
      }
    }
    try requireSuccess(status, operation: "复制 Linux 文件")
  }

  public func remove(
    path: String,
    requestID: UInt64,
    recursively: Bool = false
  ) throws {
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: false
    ) { request in
      ish_apple_guest_file_remove(
        request,
        recursively ? ISH_APPLE_GUEST_FILE_REMOVE_RECURSIVE : 0
      )
    }
    try requireSuccess(status, operation: "删除 Linux 文件")
  }

  public func rename(
    path: String,
    to destination: String,
    requestID: UInt64
  ) throws {
    guard !destination.utf8.contains(0) else {
      throw BridgeError.embeddedNull(field: "destination")
    }
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: false
    ) { request in
      destination.withCString { destinationPointer in
        ish_apple_guest_file_rename(request, destinationPointer)
      }
    }
    try requireSuccess(status, operation: "重命名 Linux 文件")
  }

  public func createDirectory(
    path: String,
    requestID: UInt64,
    mode: UInt32 = 0o755,
    withIntermediateDirectories: Bool = false
  ) throws {
    let status = try withNativeGuestFileRequest(
      path: path,
      requestID: requestID,
      followSymbolicLinks: false
    ) { request in
      ish_apple_guest_file_mkdir(
        request,
        mode,
        withIntermediateDirectories ?
          ISH_APPLE_GUEST_FILE_MKDIR_PARENTS : 0
      )
    }
    try requireSuccess(status, operation: "创建 Linux 目录")
  }

  private func requireSuccess(
    _ status: Int32,
    operation: String
  ) throws {
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: operation,
        code: status
      )
    }
  }
}

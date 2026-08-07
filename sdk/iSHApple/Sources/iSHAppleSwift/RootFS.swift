import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public enum RootFSInstallDisposition: Sendable, Equatable {
  case installed
  case alreadyPresent
}

@available(iOS 15.0, watchOS 10.0, *)
public enum RootFSArchivePhase: Sendable, Equatable {
  case verifying
  case extracting
  case validatingSeed
  case publishing
  case complete
  case unknown(UInt32)

  fileprivate init(rawValue: UInt32) {
    switch rawValue {
    case UInt32(ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VERIFY): self = .verifying
    case UInt32(ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT): self = .extracting
    case UInt32(ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VALIDATE_SEED):
      self = .validatingSeed
    case UInt32(ISH_APPLE_ROOTFS_ARCHIVE_PHASE_PUBLISH): self = .publishing
    case UInt32(ISH_APPLE_ROOTFS_ARCHIVE_PHASE_COMPLETE): self = .complete
    default: self = .unknown(rawValue)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public struct RootFSArchiveProgress: Sendable, Equatable {
  public let phase: RootFSArchivePhase
  public let compressedBytesCompleted: UInt64
  public let compressedBytesTotal: UInt64
  public let extractedBytesCompleted: UInt64
  public let extractedBytesTotal: UInt64
  public let entriesCompleted: UInt64
  public let entriesTotal: UInt64
  public let currentPath: String?
  public let currentPathWasTruncated: Bool

  fileprivate init(native: ish_apple_rootfs_archive_progress_v1) {
    phase = RootFSArchivePhase(rawValue: native.phase)
    compressedBytesCompleted = native.compressed_bytes_completed
    compressedBytesTotal = native.compressed_bytes_total
    extractedBytesCompleted = native.extracted_bytes_completed
    extractedBytesTotal = native.extracted_bytes_total
    entriesCompleted = native.entries_completed
    entriesTotal = native.entries_total
    currentPathWasTruncated = native.flags & UInt32(
      ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_PATH_TRUNCATED
    ) != 0
    currentPath = withUnsafeBytes(of: native.current_path) { bytes in
      guard let end = bytes.firstIndex(of: 0), end != 0 else {
        return nil
      }
      return String(decoding: bytes[..<end], as: UTF8.self)
    }
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private final class RootFSArchiveProgressBridge: @unchecked Sendable {
  let shouldContinue: @Sendable (RootFSArchiveProgress) -> Bool

  init(
    shouldContinue: @escaping @Sendable (RootFSArchiveProgress) -> Bool
  ) {
    self.shouldContinue = shouldContinue
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private func rootFSArchiveProgressCallback(
  context: UnsafeMutableRawPointer?,
  progress: UnsafePointer<ish_apple_rootfs_archive_progress_v1>
) -> Int32 {
  guard let context else {
    return Int32(ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CONTINUE)
  }
  let bridge = Unmanaged<RootFSArchiveProgressBridge>
    .fromOpaque(context)
    .takeUnretainedValue()
  return bridge.shouldContinue(RootFSArchiveProgress(native: progress.pointee))
    ? Int32(ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CONTINUE)
    : Int32(ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CANCEL)
}

@available(iOS 15.0, watchOS 10.0, *)
public enum RootFS {
  public static func installSeed(
    seedRoot: String,
    persistentParent: String,
    rootName: String
  ) throws -> RootFSInstallDisposition {
    try CommandABI.validateCString(seedRoot, field: "seedRoot")
    try CommandABI.validateCString(
      persistentParent,
      field: "persistentParent"
    )
    try CommandABI.validateCString(rootName, field: "rootName")

    var disposition: Int32 = 0
    let status = seedRoot.withCString { seedRoot in
      persistentParent.withCString { persistentParent in
        rootName.withCString { rootName in
          ish_apple_rootfs_install_seed(
            seedRoot,
            persistentParent,
            rootName,
            &disposition
          )
        }
      }
    }
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "安装 Linux 文件系统",
        code: status
      )
    }

    switch disposition {
    case 0:
      return .installed
    case 1:
      return .alreadyPresent
    default:
      throw BridgeError.unexpectedRootFSDisposition(
        rawValue: disposition
      )
    }
  }

  /// 同步执行磁盘安装；宿主应从后台任务调用。进度闭包返回 false 即请求取消。
  public static func installArchive(
    archivePath: String,
    expectedSHA256: String,
    expectedUncompressedBytes: UInt64,
    expectedEntryCount: UInt64,
    persistentParent: String,
    rootName: String,
    shouldContinue: (@Sendable (RootFSArchiveProgress) -> Bool)? = nil
  ) throws -> RootFSInstallDisposition {
    try CommandABI.validateCString(archivePath, field: "archivePath")
    try CommandABI.validateCString(
      persistentParent,
      field: "persistentParent"
    )
    try CommandABI.validateCString(rootName, field: "rootName")
    guard expectedSHA256.utf8.count == 64,
      expectedSHA256.utf8.allSatisfy({ byte in
        (byte >= 48 && byte <= 57) || (byte >= 97 && byte <= 102)
      }),
      expectedUncompressedBytes != 0,
      expectedEntryCount != 0
    else {
      throw BridgeError.nativeFailure(
        operation: "校验 Linux 文件系统归档参数",
        code: Int32(ISH_APPLE_LINUX_EINVAL)
      )
    }

    var disposition: Int32 = 0
    let status = archivePath.withCString { archivePath in
      expectedSHA256.withCString { expectedSHA256 in
        persistentParent.withCString { persistentParent in
          rootName.withCString { rootName in
            var spec = ish_apple_rootfs_archive_spec_v1(
              version: ISH_APPLE_ABI_VERSION,
              structure_size: UInt32(
                MemoryLayout<ish_apple_rootfs_archive_spec_v1>.size
              ),
              archive_path: archivePath,
              expected_sha256: expectedSHA256,
              persistent_parent: persistentParent,
              root_name: rootName,
              expected_uncompressed_bytes: expectedUncompressedBytes,
              expected_entry_count: expectedEntryCount,
              reserved: (0, 0)
            )
            return withUnsafePointer(to: &spec) { specPointer in
              guard let shouldContinue else {
                return ish_apple_rootfs_install_archive(
                  specPointer,
                  nil,
                  &disposition
                )
              }
              let bridge = RootFSArchiveProgressBridge(
                shouldContinue: shouldContinue
              )
              var callbacks = ish_apple_rootfs_archive_callbacks_v1(
                version: ISH_APPLE_ABI_VERSION,
                structure_size: UInt32(
                  MemoryLayout<ish_apple_rootfs_archive_callbacks_v1>.size
                ),
                context: Unmanaged.passUnretained(bridge).toOpaque(),
                progress: rootFSArchiveProgressCallback,
                reserved: (0, 0)
              )
              return withExtendedLifetime(bridge) {
                withUnsafePointer(to: &callbacks) { callbacksPointer in
                  ish_apple_rootfs_install_archive(
                    specPointer,
                    callbacksPointer,
                    &disposition
                  )
                }
              }
            }
          }
        }
      }
    }
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "安装压缩 Linux 文件系统",
        code: status
      )
    }
    switch disposition {
    case ISH_APPLE_ROOTFS_INSTALL_RESULT_INSTALLED:
      return .installed
    case ISH_APPLE_ROOTFS_INSTALL_RESULT_ALREADY_PRESENT:
      return .alreadyPresent
    default:
      throw BridgeError.unexpectedRootFSDisposition(
        rawValue: disposition
      )
    }
  }
}

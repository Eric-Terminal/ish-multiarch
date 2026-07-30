import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public enum RootFSInstallDisposition: Sendable, Equatable {
  case installed
  case alreadyPresent
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
}

import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public struct RuntimeFeatureSet: OptionSet, Sendable, Equatable {
  public let rawValue: UInt64

  public init(rawValue: UInt64) {
    self.rawValue = rawValue
  }

  public static let pseudoTerminals = RuntimeFeatureSet(
    rawValue: ISH_APPLE_RUNTIME_CAPABILITY_PTY
  )
  public static let liveMounts = RuntimeFeatureSet(
    rawValue: ISH_APPLE_RUNTIME_CAPABILITY_LIVE_MOUNTS
  )
  public static let diagnostics = RuntimeFeatureSet(
    rawValue: ISH_APPLE_RUNTIME_CAPABILITY_DIAGNOSTICS
  )
  public static let guestFiles = RuntimeFeatureSet(
    rawValue: ISH_APPLE_RUNTIME_CAPABILITY_GUEST_FILES
  )
}

@available(iOS 15.0, watchOS 10.0, *)
public struct RuntimeCapabilities: Sendable, Equatable {
  public let features: RuntimeFeatureSet
  public let guestArchitecture: GuestArchitecture
  public let backend: RuntimeBackend
  public let publicABIVersion: UInt32

  init(native: ish_apple_runtime_capabilities_v1) {
    features = RuntimeFeatureSet(rawValue: native.feature_flags)
    guestArchitecture = GuestArchitecture(
      rawValue: native.guest_architecture
    )
    backend = RuntimeBackend(rawValue: native.backend)
    publicABIVersion = native.public_abi_version
  }
}

@available(iOS 15.0, watchOS 10.0, *)
extension Runtime {
  public func capabilities() throws -> RuntimeCapabilities {
    var native = ish_apple_runtime_capabilities_v1()
    let status = ish_apple_runtime_copy_capabilities(&native)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "读取 Linux runtime 能力",
        code: status
      )
    }
    return RuntimeCapabilities(native: native)
  }
}

import Foundation
import iSHApple

@available(iOS 15.0, watchOS 10.0, *)
public final class RuntimeMountLease: @unchecked Sendable {
  private let native: OpaquePointer

  fileprivate init(native: OpaquePointer) {
    self.native = native
  }

  deinit {
    ish_apple_mount_lease_release(native)
  }
}

@available(iOS 15.0, watchOS 10.0, *)
public actor RuntimeMounts {
  public init() {}

  public func add(_ configuration: RuntimeMountConfiguration) throws {
    try validateMountConfiguration(configuration)
    let status = try withNativeMountSpecs([configuration]) {
      specs, _ in
      ish_apple_mount_add(specs!)
    }
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "挂载 guest 目录",
        code: status
      )
    }
  }

  public func remove(id: UUID, force: Bool = false) throws {
    let flags = force ? ISH_APPLE_MOUNT_REMOVE_FORCE : 0
    let status = ish_apple_mount_remove(nativeMountID(id), flags)
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "卸载 guest 目录",
        code: status
      )
    }
  }

  public func acquireLease(id: UUID) throws -> RuntimeMountLease {
    var native: OpaquePointer?
    let status = ish_apple_mount_lease_acquire(
      nativeMountID(id),
      &native
    )
    guard status == 0, let native else {
      throw BridgeError.nativeFailure(
        operation: "获取 mount lease",
        code: status
      )
    }
    return RuntimeMountLease(native: native)
  }

  public func list() throws -> [RuntimeMountInfo] {
    while true {
      var count: UInt32 = 0
      let countStatus = ish_apple_mount_list(nil, 0, &count)
      guard countStatus == 0 else {
        throw BridgeError.nativeFailure(
          operation: "读取 mount 数量",
          code: countStatus
        )
      }
      if count == 0 {
        return []
      }

      var nativeInfos = [ish_apple_mount_info_v1](
        repeating: ish_apple_mount_info_v1(),
        count: Int(count)
      )
      var actualCount = count
      let listStatus = nativeInfos.withUnsafeMutableBufferPointer {
        buffer in
        ish_apple_mount_list(
          buffer.baseAddress,
          UInt32(buffer.count),
          &actualCount
        )
      }
      if listStatus == ISH_APPLE_LINUX_ENOSPC {
        continue
      }
      guard listStatus == 0 else {
        throw BridgeError.nativeFailure(
          operation: "读取 mount 状态",
          code: listStatus
        )
      }

      return try nativeInfos.prefix(Int(actualCount)).map { native in
        let id = uuidFromNativeMountID(native.mount_id)
        return RuntimeMountInfo(
          id: id,
          guestDirectory: try copyGuestDirectory(id: id),
          access: MountAccess(nativeValue: native.access),
          state: RuntimeMountState(nativeValue: native.state),
          activeLeases: native.active_leases,
          activeReferences: native.active_references
        )
      }
    }
  }

  private func copyGuestDirectory(id: UUID) throws -> String {
    var required: UInt32 = 0
    var status = ish_apple_mount_copy_guest_directory(
      nativeMountID(id),
      nil,
      0,
      &required
    )
    guard status == 0, required != 0 else {
      throw BridgeError.nativeFailure(
        operation: "读取 guest 挂载路径长度",
        code: status
      )
    }
    var buffer = [CChar](repeating: 0, count: Int(required))
    status = buffer.withUnsafeMutableBufferPointer { bytes in
      ish_apple_mount_copy_guest_directory(
        nativeMountID(id),
        bytes.baseAddress,
        UInt32(bytes.count),
        &required
      )
    }
    guard status == 0 else {
      throw BridgeError.nativeFailure(
        operation: "读取 guest 挂载路径",
        code: status
      )
    }
    let terminator = buffer.firstIndex(of: 0) ?? buffer.endIndex
    return String(
      decoding: buffer[..<terminator].map { UInt8(bitPattern: $0) },
      as: UTF8.self
    )
  }
}

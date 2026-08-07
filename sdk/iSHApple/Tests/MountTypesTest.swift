import Foundation
import iSHApple

@main
struct MountTypesTest {
  static func main() throws {
    let id = UUID(uuidString: "01234567-89AB-CDEF-0123-456789ABCDEF")!
    let native = nativeMountID(id)
    precondition(native.high == 0x0123_4567_89AB_CDEF)
    precondition(native.low == 0x0123_4567_89AB_CDEF)
    precondition(uuidFromNativeMountID(native) == id)

    let configuration = RuntimeMountConfiguration(
      id: id,
      hostDirectoryDescriptor: 3,
      guestDirectory: "/mnt/etos/example",
      access: .readWrite
    )
    try validateMountConfiguration(configuration)
    let count = try withNativeMountSpecs([configuration]) {
      specs, count in
      precondition(specs?.pointee.mount_id.high == native.high)
      precondition(
        specs?.pointee.access == ISH_APPLE_MOUNT_ACCESS_READ_WRITE
      )
      precondition(
        String(cString: specs!.pointee.guest_directory) ==
          configuration.guestDirectory
      )
      return count
    }
    precondition(count == 1)
  }
}

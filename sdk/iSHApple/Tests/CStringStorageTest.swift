@main
struct CStringStorageTest {
  static func main() {
    let values = (0..<4_096).map { "参数-\($0)" }
    let matches = withCStringPointers(values) { pointers in
      for index in values.indices {
        if String(cString: pointers[index]) != values[index] {
          return false
        }
      }
      return true
    }

    guard matches else {
      fatalError("4096 项 C 字符串稳定缓冲验证失败")
    }
    print("4096 项 C 字符串稳定缓冲验证通过")
  }
}

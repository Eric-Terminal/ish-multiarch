@main
struct BoundedOutputChannelTest {
  static func main() async throws {
    let channel = BoundedOutputChannel(capacity: 4)
    let first = CommandOutputEvent(
      requestID: 7,
      stream: .stdout,
      bytes: [1, 2, 3, 4]
    )
    let second = CommandOutputEvent(
      requestID: 7,
      stream: .stderr,
      bytes: [5, 6, 7, 8]
    )

    let producer = Task.detached {
      channel.send(first)
      channel.send(second)
      channel.finish()
    }

    guard try await channel.next() == first,
      try await channel.next() == second,
      try await channel.next() == nil
    else {
      fatalError("有界输出通道的顺序或结束状态错误")
    }
    await producer.value

    let interrupted = BoundedOutputChannel(capacity: 4)
    interrupted.send(first)
    interrupted.finish(throwing: CancellationError())
    guard try await interrupted.next() == first else {
      fatalError("有界输出通道没有在错误结束前排空数据")
    }
    do {
      _ = try await interrupted.next()
      fatalError("有界输出通道没有传递结束错误")
    } catch is CancellationError {
      print("有界输出通道反压、排空与结束验证通过")
    }
  }
}

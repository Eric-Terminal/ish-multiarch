@main
struct TerminalOutputChannelTest {
  static func main() async throws {
    let channel = TerminalOutputChannel(capacity: 4)
    let first = TerminalOutputEvent(
      terminalID: 9,
      bytes: [1, 2, 3, 4],
      droppedBytes: 0
    )
    let second = TerminalOutputEvent(
      terminalID: 9,
      bytes: [5, 6],
      droppedBytes: 3
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
      fatalError("终端输出通道的顺序、丢包标记或结束状态错误")
    }
    await producer.value

    let interrupted = TerminalOutputChannel(capacity: 4)
    interrupted.send(first)
    interrupted.finish(throwing: CancellationError())
    guard try await interrupted.next() == first else {
      fatalError("终端输出通道没有在错误结束前排空数据")
    }
    do {
      _ = try await interrupted.next()
      fatalError("终端输出通道没有传递结束错误")
    } catch is CancellationError {
      print("终端输出通道反压、排空与结束验证通过")
    }
  }
}

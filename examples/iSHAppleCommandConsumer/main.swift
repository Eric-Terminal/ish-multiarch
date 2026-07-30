import iSHAppleSwift

@main
struct ISHAppleCommandConsumer {
  static func main() async throws {
    guard CommandLine.arguments.count == 4 else {
      print("用法：iSHAppleCommandConsumer <root-data> <shared-dir> <socket-prefix>")
      return
    }

    let runtime = Runtime()
    try await runtime.start(
      RuntimeConfiguration(
        rootData: CommandLine.arguments[1],
        sharedDirectory: CommandLine.arguments[2],
        socketPrefix: CommandLine.arguments[3],
        hostname: "ish-consumer",
        bootCommand: "/bin/true"
      )
    )

    let request = CommandRequest(
      requestID: 1,
      executable: "/bin/sh",
      argv: ["/bin/sh", "-lc", "uname -a"],
      environment: ["PATH=/usr/bin:/bin"],
      workingDirectory: "/",
      timeoutMilliseconds: 30_000,
      outputLimitBytes: 1_048_576
    )
    let session = try CommandSession.start(request)

    async let output: Void = consume(session.output)
    let result = try await session.result()
    try await output

    print(
      "\n请求 \(result.requestID) 完成：\(result.reason)，"
        + "exit=\(result.exitCode)"
    )
  }

  private static func consume(_ events: CommandOutputEvents) async throws {
    for try await event in events {
      let text = String(decoding: event.bytes, as: UTF8.self)
      switch event.stream {
      case .stdout:
        print(text, terminator: "")
      case .stderr:
        print("[stderr] \(text)", terminator: "")
      }
    }
  }
}

import iSHAppleSwift

@available(iOS 15.0, watchOS 10.0, *)
func compilePublicAPI(runtime: Runtime) async throws {
  let _: RootFSInstallDisposition = try RootFS.installSeed(
    seedRoot: "/bundle/rootfs",
    persistentParent: "/app/data",
    rootName: "default"
  )

  let request = CommandRequest(
    requestID: 42,
    executable: "/bin/cat",
    argv: ["/bin/cat"],
    environment: ["PATH=/usr/bin:/bin"],
    workingDirectory: "/",
    timeoutMilliseconds: 10_000,
    outputLimitBytes: 65_536
  )
  let unlimitedRequest = CommandRequest(
    requestID: 43,
    executable: "/bin/cat",
    argv: ["/bin/cat"],
    timeoutMilliseconds: CommandRequest.timeoutDisabled,
    outputLimitBytes: CommandRequest.outputLimitDisabled
  )
  _ = unlimitedRequest
  let session = try CommandSession.start(
    request,
    eventBufferByteCapacity: 32_768
  )

  async let consume: Void = consumeEvents(session.output)
  try await session.send(Array("hello\n".utf8))
  try session.finishInput()
  _ = try await session.result()
  try await consume

  let terminal = try TerminalSession.start(
    TerminalRequest(
      terminalID: 44,
      executable: "/bin/sh",
      argv: ["/bin/sh", "-l"],
      environment: ["TERM=xterm-256color"],
      workingDirectory: "/",
      columns: 80,
      rows: 24
    )
  )
  async let consumeTerminal: Void = consumeTerminalEvents(
    terminal.output
  )
  try await terminal.send(Array("printf hello\\n".utf8))
  try terminal.resize(columns: 100, rows: 30)
  try terminal.interrupt()
  try terminal.finishInput()
  _ = try await terminal.result()
  try await consumeTerminal
}

@available(iOS 15.0, watchOS 10.0, *)
private func consumeEvents(_ events: CommandOutputEvents) async throws {
  for try await event in events {
    let _: CommandStream = event.stream
    let _: [UInt8] = event.bytes
  }
}

@available(iOS 15.0, watchOS 10.0, *)
private func consumeTerminalEvents(
  _ events: TerminalOutputEvents
) async throws {
  for try await event in events {
    let _: UInt64 = event.terminalID
    let _: [UInt8] = event.bytes
    let _: UInt64 = event.droppedBytes
  }
}

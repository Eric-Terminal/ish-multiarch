import Foundation

@available(iOS 15.0, watchOS 10.0, *)
final class BoundedOutputChannel: @unchecked Sendable {
  private typealias Continuation =
    CheckedContinuation<CommandOutputEvent?, any Error>

  private let condition = NSCondition()
  private let capacity: Int
  private var queue: [CommandOutputEvent] = []
  private var queuedBytes = 0
  private var waiters: [Continuation] = []
  private var isFinished = false
  private var terminalError: (any Error)?

  init(capacity: Int) {
    self.capacity = capacity
  }

  func send(_ event: CommandOutputEvent) {
    var waiter: Continuation?

    condition.lock()
    while !isFinished && waiters.isEmpty
      && queuedBytes + event.bytes.count > capacity
    {
      condition.wait()
    }

    if !isFinished {
      if waiters.isEmpty {
        queue.append(event)
        queuedBytes += event.bytes.count
      } else {
        waiter = waiters.removeFirst()
      }
    }
    condition.unlock()

    waiter?.resume(returning: event)
  }

  func next() async throws -> CommandOutputEvent? {
    try await withCheckedThrowingContinuation { continuation in
      var immediate: Result<CommandOutputEvent?, any Error>?

      condition.lock()
      if !queue.isEmpty {
        let event = queue.removeFirst()
        queuedBytes -= event.bytes.count
        immediate = .success(event)
        condition.broadcast()
      } else if isFinished {
        if let terminalError {
          immediate = .failure(terminalError)
        } else {
          immediate = .success(nil)
        }
      } else {
        waiters.append(continuation)
      }
      condition.unlock()

      if let immediate {
        continuation.resume(with: immediate)
      }
    }
  }

  func finish(throwing error: (any Error)? = nil) {
    var pending: [Continuation] = []

    condition.lock()
    if !isFinished {
      isFinished = true
      terminalError = error
      if queue.isEmpty {
        pending = waiters
        waiters.removeAll(keepingCapacity: false)
      }
      condition.broadcast()
    }
    condition.unlock()

    for continuation in pending {
      if let error {
        continuation.resume(throwing: error)
      } else {
        continuation.resume(returning: nil)
      }
    }
  }
}

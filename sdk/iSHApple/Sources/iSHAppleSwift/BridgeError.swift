import Foundation

/// 公共 Swift 包装层报告的参数或原生调用错误。
@available(iOS 15.0, watchOS 10.0, *)
public enum BridgeError: Error, Sendable, Equatable {
  case invalidArguments
  case tooManyArguments(limit: Int)
  case embeddedNull(field: String)
  case invalidEventBufferCapacity(minimum: Int)
  case nativeFailure(operation: String, code: Int32)
  case unexpectedRootFSDisposition(rawValue: Int32)
  case unexpectedStream(rawValue: UInt32)
  case streamFailure(stream: CommandStream, code: Int32)
}

@available(iOS 15.0, watchOS 10.0, *)
extension BridgeError: LocalizedError {
  public var errorDescription: String? {
    switch self {
    case .invalidArguments:
      return "argv 至少需要包含 argv[0]"
    case .tooManyArguments(let limit):
      return "argv 和 environment 分别不能超过 \(limit) 项"
    case .embeddedNull(let field):
      return "\(field) 不能包含空字符"
    case .invalidEventBufferCapacity(let minimum):
      return "事件缓冲区至少需要 \(minimum) 字节"
    case .nativeFailure(let operation, let code):
      return "\(operation) 失败，Linux errno 为 \(code)"
    case .unexpectedRootFSDisposition(let rawValue):
      return "原生层返回了未知文件系统安装结果 \(rawValue)"
    case .unexpectedStream(let rawValue):
      return "原生层返回了未知输出流 \(rawValue)"
    case .streamFailure(let stream, let code):
      return "\(stream.description) 输出流异常结束，Linux errno 为 \(code)"
    }
  }
}

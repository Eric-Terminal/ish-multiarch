func withCStringPointers<Result>(
  _ strings: [String],
  _ body: (UnsafePointer<UnsafePointer<CChar>>) throws -> Result
) rethrows -> Result {
  var allocations: [UnsafeMutablePointer<CChar>] = []
  allocations.reserveCapacity(strings.count)
  defer {
    for allocation in allocations {
      allocation.deallocate()
    }
  }

  for string in strings {
    let bytes = string.utf8CString
    let allocation = UnsafeMutablePointer<CChar>.allocate(
      capacity: bytes.count
    )
    bytes.withUnsafeBufferPointer { buffer in
      allocation.initialize(from: buffer.baseAddress!, count: bytes.count)
    }
    allocations.append(allocation)
  }

  let pointers = allocations.map { UnsafePointer<CChar>($0) }
  return try pointers.withUnsafeBufferPointer { buffer in
    try body(buffer.baseAddress!)
  }
}

func withOptionalCStringPointers<Result>(
  _ strings: [String],
  _ body: (UnsafePointer<UnsafePointer<CChar>>?) throws -> Result
) rethrows -> Result {
  if strings.isEmpty {
    return try body(nil)
  }
  return try withCStringPointers(strings, body)
}

func withOptionalCString<Result>(
  _ string: String?,
  _ body: (UnsafePointer<CChar>?) throws -> Result
) rethrows -> Result {
  guard let string else {
    return try body(nil)
  }
  return try string.withCString(body)
}

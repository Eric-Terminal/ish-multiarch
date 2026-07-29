struct TerminalLine: Identifiable, Equatable {
    let id: UInt64
    let text: String
}

struct TerminalDecoder {
    private enum EscapeState {
        case text
        case escape
        case controlSequence
        case operatingSystemCommand
        case operatingSystemCommandEscape
    }

    private var escapeState = EscapeState.text
    private var visibleBytes: [UInt8] = []
    private var firstLineID: UInt64 = 0
    private let outputLimit: Int

    init(outputLimit: Int = 32 * 1024) {
        self.outputLimit = outputLimit
    }

    var text: String {
        String(decoding: visibleBytes, as: UTF8.self)
    }

    var lines: [TerminalLine] {
        guard !visibleBytes.isEmpty else { return [] }
        return visibleBytes.split(
            separator: 0x0a,
            omittingEmptySubsequences: false
        ).enumerated().map { index, bytes in
            TerminalLine(
                id: firstLineID + UInt64(index),
                text: String(decoding: bytes, as: UTF8.self))
        }
    }

    mutating func append(_ bytes: [UInt8]) {
        for byte in bytes {
            switch escapeState {
            case .text:
                appendTextByte(byte)
            case .escape:
                if byte == 0x5b {
                    escapeState = .controlSequence
                } else if byte == 0x5d {
                    escapeState = .operatingSystemCommand
                } else {
                    escapeState = .text
                }
            case .controlSequence:
                if (0x40...0x7e).contains(byte) {
                    escapeState = .text
                }
            case .operatingSystemCommand:
                if byte == 0x07 {
                    escapeState = .text
                } else if byte == 0x1b {
                    escapeState = .operatingSystemCommandEscape
                }
            case .operatingSystemCommandEscape:
                escapeState = byte == 0x5c ? .text : .operatingSystemCommand
            }
        }
        trimIfNeeded()
    }

    mutating func reportDroppedBytes(_ count: UInt64) {
        guard count != 0 else { return }
        escapeState = .text
        if visibleBytes.last != 0x0a {
            appendTextByte(0x0a)
        }
        for byte in "[已省略 \(count) 字节输出]\n".utf8 {
            appendTextByte(byte)
        }
        trimIfNeeded()
    }

    private mutating func appendTextByte(_ byte: UInt8) {
        switch byte {
        case 0x1b:
            escapeState = .escape
        case 0x08, 0x7f:
            removeLastCharacter()
        case 0x0a:
            visibleBytes.append(byte)
        case 0x0d, 0x00...0x07, 0x0b...0x1a, 0x1c...0x1f:
            break
        default:
            visibleBytes.append(byte)
        }
    }

    private mutating func removeLastCharacter() {
        guard visibleBytes.last != 0x0a else { return }
        while let byte = visibleBytes.last, byte & 0xc0 == 0x80 {
            visibleBytes.removeLast()
        }
        if visibleBytes.last != nil {
            visibleBytes.removeLast()
        }
    }

    private mutating func trimIfNeeded() {
        guard visibleBytes.count > outputLimit else { return }
        let minimumRemoval = visibleBytes.count - outputLimit
        let removalCount: Int
        if let newline = visibleBytes[minimumRemoval...].firstIndex(of: 0x0a) {
            removalCount = newline + 1
        } else {
            removalCount = minimumRemoval
        }
        let removedLineCount = visibleBytes.prefix(removalCount).reduce(0) {
            $0 + ($1 == 0x0a ? 1 : 0)
        }
        visibleBytes.removeFirst(removalCount)
        firstLineID += UInt64(removedLineCount)
        while let byte = visibleBytes.first, byte & 0xc0 == 0x80 {
            visibleBytes.removeFirst()
        }
    }
}

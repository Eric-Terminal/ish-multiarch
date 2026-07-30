enum WatchControlInput {
    static func byte(for character: Character) -> UInt8? {
        let scalars = String(character).unicodeScalars
        guard scalars.count == 1,
              let scalar = scalars.first,
              scalar.value <= UInt8.max else {
            return nil
        }

        var value = UInt8(scalar.value)
        if (UInt8(ascii: "A")...UInt8(ascii: "Z")).contains(value) {
            value += UInt8(ascii: "a") - UInt8(ascii: "A")
        }

        let isLetter =
            (UInt8(ascii: "a")...UInt8(ascii: "z")).contains(value)
        let isSupportedSymbol: Bool
        switch value {
        case UInt8(ascii: "@"), UInt8(ascii: "^"),
             UInt8(ascii: "2"), UInt8(ascii: "6"),
             UInt8(ascii: "-"),
             UInt8(ascii: "["), UInt8(ascii: "]"),
             UInt8(ascii: "\\"), UInt8(ascii: " "):
            isSupportedSymbol = true
        default:
            isSupportedSymbol = false
        }
        guard isLetter || isSupportedSymbol else { return nil }

        if value == UInt8(ascii: " ") {
            return 0
        }
        if value == UInt8(ascii: "2") {
            value = UInt8(ascii: "@")
        } else if value == UInt8(ascii: "6") {
            value = UInt8(ascii: "^")
        } else if value == UInt8(ascii: "-") {
            value = UInt8(ascii: "_")
        } else if isLetter {
            value -= UInt8(ascii: "a") - UInt8(ascii: "A")
        }
        return value ^ 0x40
    }
}

enum WatchTerminalInput {
    // 物理终端的 Return 是 CR；canonical TTY 再按 termios 映射为换行。
    static let enter: UInt8 = 0x0d
    static let delete: UInt8 = 0x7f
    static let escape: UInt8 = 0x1b

    static func text(_ text: String) -> [UInt8] {
        Array(text.utf8)
    }

    static func sequence(
        _ bytes: [UInt8],
        after pendingText: String = ""
    ) -> [UInt8] {
        text(pendingText) + bytes
    }

    static func enter(after pendingText: String = "") -> [UInt8] {
        sequence([enter], after: pendingText)
    }

    static func delete(after pendingText: String = "") -> [UInt8] {
        sequence([delete], after: pendingText)
    }

    static func meta(
        _ text: String,
        after pendingText: String = ""
    ) -> [UInt8]? {
        guard !text.isEmpty else { return nil }
        return sequence(
            [escape] + self.text(text),
            after: pendingText)
    }
}

@main
struct WatchControlInputTest {
    private static var failures = 0

    static func main() {
        testLetters()
        testAliasesAndSymbols()
        testUnsupportedCharacters()
        testTerminalInputSequences()

        if failures == 0 {
            print("Watch Ctrl 输入映射回归通过")
        } else {
            fatalError("Watch Ctrl 输入映射回归失败：\(failures) 项")
        }
    }

    private static func expect(
        _ condition: @autoclosure () -> Bool,
        _ message: String
    ) {
        if !condition() {
            print("失败：\(message)")
            failures += 1
        }
    }

    private static func testLetters() {
        for (offset, character) in
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ".enumerated() {
            expect(
                WatchControlInput.byte(for: character) ==
                    UInt8(offset + 1),
                "Ctrl-\(character) 应映射为 \(offset + 1)")
            let lowercase = Character(
                String(character).lowercased())
            expect(
                WatchControlInput.byte(for: lowercase) ==
                    UInt8(offset + 1),
                "Ctrl 字母映射不应区分大小写")
        }
    }

    private static func testAliasesAndSymbols() {
        let expected: [(Character, UInt8)] = [
            ("@", 0x00),
            ("2", 0x00),
            (" ", 0x00),
            ("^", 0x1e),
            ("6", 0x1e),
            ("-", 0x1f),
            ("[", 0x1b),
            ("]", 0x1d),
            ("\\", 0x1c),
        ]
        for (character, byte) in expected {
            expect(
                WatchControlInput.byte(for: character) == byte,
                "Ctrl-\(character) 的主终端兼容映射不正确")
        }
    }

    private static func testUnsupportedCharacters() {
        for character in ["_", "=", "1", "/", "晚"] as [Character] {
            expect(
                WatchControlInput.byte(for: character) == nil,
                "不在控制键集合中的字符应被拒绝：\(character)")
        }
    }

    private static func testTerminalInputSequences() {
        let text = "  echo 晚\n"
        expect(
            WatchTerminalInput.text(text) == Array(text.utf8),
            "普通终端输入必须保留原文、空白和换行")
        expect(
            WatchTerminalInput.enter(after: "printf x") ==
                Array("printf x".utf8) + [0x0d],
            "Enter 必须以 CR 排在尚未发送的文字之后")
        expect(
            WatchTerminalInput.delete(after: "ab") ==
                Array("ab".utf8) + [0x7f],
            "DEL 必须排在尚未发送的文字之后")
        expect(
            WatchTerminalInput.sequence(
                [0x1b, 0x5b, 0x41],
                after: "draft"
            ) == Array("draft".utf8) + [0x1b, 0x5b, 0x41],
            "历史键必须先保留尚未发送的文字")
        expect(
            WatchTerminalInput.meta("x", after: "ab") ==
                Array("ab".utf8) + [0x1b, UInt8(ascii: "x")],
            "Meta 输入必须使用 ESC 前缀且保留尚未发送的文字")
        expect(
            WatchTerminalInput.meta("") == nil,
            "空 Meta 输入不应退化为一次普通 Escape")
    }
}

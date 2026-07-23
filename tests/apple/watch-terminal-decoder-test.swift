@main
struct WatchTerminalDecoderTest {
    private static var failures = 0

    static func main() {
        testControlsAndEscapeSequences()
        testSplitUTF8()
        testDroppedOutputRecovery()
        testBoundedScrollback()
        if failures == 0 {
            print("Watch 终端文本解码回归通过")
        } else {
            fatalError("Watch 终端文本解码回归失败：\(failures) 项")
        }
    }

    private static func expect(
            _ condition: @autoclosure () -> Bool, _ message: String) {
        if !condition() {
            print("失败：\(message)")
            failures += 1
        }
    }

    private static func testControlsAndEscapeSequences() {
        var decoder = TerminalDecoder()
        decoder.append(Array("abc".utf8) + [0x08] + Array("D\r\n".utf8))
        decoder.append([0x1b, 0x5b, 0x33])
        decoder.append(Array("1mred".utf8) + [0x1b, 0x5b, 0x30, 0x6d])
        expect(decoder.text == "abD\nred",
                "应处理退格、CRLF 和跨批次 ANSI CSI")

        decoder.append([0x1b, 0x5d] + Array("0;title".utf8) + [0x1b])
        decoder.append([0x5c] + Array("!".utf8))
        expect(decoder.text == "abD\nred!",
                "应过滤跨批次 OSC 标题序列")
    }

    private static func testSplitUTF8() {
        var decoder = TerminalDecoder()
        let bytes = Array("晚晖".utf8)
        decoder.append(Array(bytes.prefix(2)))
        decoder.append(Array(bytes.dropFirst(2)))
        expect(decoder.text == "晚晖",
                "分批到达的 UTF-8 应在完整后正确解码")
    }

    private static func testDroppedOutputRecovery() {
        var decoder = TerminalDecoder()
        decoder.append([0x1b, 0x5b, 0x33])
        decoder.reportDroppedBytes(17)
        decoder.append(Array("prompt".utf8))
        expect(decoder.text == "\n[已省略 17 字节输出]\nprompt",
                "丢失输出后应重置转义状态并保留后续文本")
    }

    private static func testBoundedScrollback() {
        var decoder = TerminalDecoder(outputLimit: 8)
        decoder.append(Array("line1\nline2\n".utf8))
        expect(decoder.text == "line2\n",
                "有界 scrollback 应优先在换行边界裁剪")
    }
}

@main
struct WatchTerminalDecoderTest {
    private static var failures = 0

    static func main() {
        testStreamingUnicodeAndControls()
        testBoundedCellText()
        testCursorMovement()
        testEraseAndStableRefresh()
        testStylesAndColors()
        testExtendedTextAttributes()
        testHiddenTextAccessibility()
        testCharacterEditing()
        testLineEditing()
        testDelayedWrapEditingRules()
        testScrollRegion()
        testBoundedScrollback()
        testAlternateScreenAndModes()
        testSaveRestoreAndOSCFiltering()
        testStringControlTermination()
        testOSC8Hyperlinks()
        testDeviceQueriesAndResponses()
        testDECCharacterSetsAndModes()
        testPrivateCSIIsolation()
        testCursorStyleAndInputModes()
        testBoundedCSIParameters()
        testResizeAndBoundaries()
        testDroppedOutputRecovery()

        if failures == 0 {
            print("Watch 原生终端核心回归通过")
        } else {
            fatalError("Watch 原生终端核心回归失败：\(failures) 项")
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

    private static func texts(
        _ screen: TerminalScreen
    ) -> [String] {
        screen.visibleLines.map(\.text)
    }

    private static func style(
        in screen: TerminalScreen,
        row: Int,
        column: Int
    ) -> TerminalStyle? {
        run(in: screen, row: row, column: column)?.style
    }

    private static func run(
        in screen: TerminalScreen,
        row: Int,
        column: Int
    ) -> TerminalRenderRun? {
        guard screen.visibleLines.indices.contains(row) else {
            return nil
        }
        return screen.visibleLines[row].runs.first {
            column >= $0.startColumn &&
                column < $0.startColumn + $0.columnCount
        }
    }

    private static func testStreamingUnicodeAndControls() {
        var screen = TerminalScreen(columns: 12, rows: 3)
        screen.append(Array("abc\rZ".utf8))
        screen.append([0x0a])
        screen.append(Array("Y".utf8) + [0x08] + Array("Q".utf8))

        expect(
            texts(screen) == ["Zbc", " Q", ""],
            "CR、LF 与退格应按终端光标语义工作")
        expect(
            screen.cursor == TerminalCursor(
                row: 1, column: 2, isVisible: true),
            "控制字符执行后应公开准确光标位置")

        var tabScreen = TerminalScreen(columns: 12, rows: 2)
        tabScreen.append(Array("A\tB".utf8))
        expect(
            texts(tabScreen)[0] == "A       B",
            "Tab 应移动到八列制表位")

        var unicodeScreen = TerminalScreen(columns: 12, rows: 2)
        let unicodeBytes = Array("晚e\u{301}".utf8)
        unicodeScreen.append(Array(unicodeBytes.prefix(2)))
        unicodeScreen.append(Array(unicodeBytes.dropFirst(2)))
        expect(
            texts(unicodeScreen)[0] == "晚e\u{301}",
            "跨批次 UTF-8、宽字符与组合字符应正确解码")
        expect(
            unicodeScreen.cursor.column == 3,
            "宽字符与组合字符应占用合理终端列宽")
    }

    private static func testBoundedCellText() {
        var screen = TerminalScreen(columns: 4, rows: 2)
        let combiningMarks = String(
            repeating: "\u{301}",
            count: TerminalScreen.maximumCellTextScalars + 4_096)
        screen.append(Array(("A" + combiningMarks + "B").utf8))

        expect(
            screen.buffers[0].lines[0].cells[0]
                .text.unicodeScalars.count ==
                TerminalScreen.maximumCellTextScalars,
            "单个 Cell 的组合序列应有固定上限")
        expect(
            screen.buffers[0].lines[0].cells[1].text == "B",
            "组合序列达到上限后仍应继续处理普通字符")
    }

    private static func testCursorMovement() {
        var screen = TerminalScreen(columns: 8, rows: 5)
        screen.append(Array("\u{1b}[3;4H".utf8))
        expect(
            screen.cursor.row == 2 && screen.cursor.column == 3,
            "CSI H 应定位行列")

        screen.append(Array(
            "\u{1b}[2A\u{1b}[2B\u{1b}[3C\u{1b}[4D".utf8))
        expect(
            screen.cursor.row == 2 && screen.cursor.column == 2,
            "CSI A/B/C/D 应相对移动光标")

        screen.append(Array(
            "\u{1b}[2E\u{1b}[3F\u{1b}[7G".utf8))
        expect(
            screen.cursor.row == 1 && screen.cursor.column == 6,
            "CSI E/F/G 应移动并按需回到首列")

        screen.append(Array("\u{1b}[4;8f".utf8))
        expect(
            screen.cursor.row == 3 && screen.cursor.column == 7,
            "CSI f 应与 CSI H 等价定位")

        screen.append(Array(
            "\u{1b}[2d\u{1b}[5`\u{1b}[2a\u{1b}[2e".utf8))
        expect(
            screen.cursor.row == 3 && screen.cursor.column == 6,
            "VPA、HPA、HPR 与 VPR 应支持 terminfo 常用定位序列")
    }

    private static func testEraseAndStableRefresh() {
        var screen = TerminalScreen(columns: 6, rows: 3)
        screen.append(Array(
            "abcdef\r\n123456\r\nuvwxyz".utf8))
        let initialIDs = screen.visibleLines.map(\.id)

        screen.append(Array("\u{1b}[2;3H\u{1b}[KXY".utf8))
        expect(
            texts(screen) == ["abcdef", "12XY", "uvwxyz"],
            "CSI K 默认模式应从光标擦除到行尾")
        expect(
            screen.visibleLines.map(\.id) == initialIDs,
            "局部刷新不应改变物理行身份")

        screen.append(Array("\u{1b}[1;3H\u{1b}[2X".utf8))
        expect(
            texts(screen)[0] == "ab  ef",
            "CSI X 应只擦除指定字符数")

        screen.append(Array("\u{1b}[3;3H\u{1b}[1K".utf8))
        expect(
            texts(screen)[2] == "   xyz",
            "CSI K 模式 1 应擦除行首到光标")

        screen.append(Array("\u{1b}[2;3H\u{1b}[J".utf8))
        expect(
            texts(screen) == ["ab  ef", "12", ""],
            "CSI J 默认模式应擦除光标后屏幕")

        screen.append(Array("\u{1b}[2J".utf8))
        expect(
            texts(screen) == ["", "", ""],
            "CSI J 模式 2 应擦除整个可见屏幕")
    }

    private static func testStylesAndColors() {
        var screen = TerminalScreen(columns: 10, rows: 2)
        screen.append(Array((
            "\u{1b}[1;2;4;7;31;48;5;200mR" +
            "\u{1b}[22;24;27;38;2;1;2;3;49mT" +
            "\u{1b}[94;103mB\u{1b}[0mN"
        ).utf8))

        let red = style(in: screen, row: 0, column: 0)
        expect(
            red == TerminalStyle(
                foreground: .indexed(1),
                background: .indexed(200),
                isBold: true,
                isDim: true,
                isUnderlined: true,
                isInverse: true),
            "SGR 应公开 16 色、256 色与文本属性")

        let trueColor = style(in: screen, row: 0, column: 1)
        expect(
            trueColor == TerminalStyle(
                foreground: .rgb(red: 1, green: 2, blue: 3),
                background: .default),
            "SGR 应解析真彩色并能独立复位文本属性")

        let bright = style(in: screen, row: 0, column: 2)
        expect(
            bright?.foreground == .indexed(12) &&
                bright?.background == .indexed(11),
            "SGR 应解析高亮 16 色")

        expect(
            style(in: screen, row: 0, column: 3) == TerminalStyle(),
            "SGR 0 应恢复默认样式")
        expect(
            screen.visibleLines[0].runs.count >= 4,
            "渲染接口应按样式变化公开样式片段")
    }

    private static func testExtendedTextAttributes() {
        var screen = TerminalScreen(columns: 12, rows: 2)
        screen.append(Array((
            "\u{1b}[3;5;8;9mA" +
            "\u{1b}[23mB" +
            "\u{1b}[25mC" +
            "\u{1b}[28mD" +
            "\u{1b}[29mE" +
            "\u{1b}[0mF"
        ).utf8))

        let all = style(in: screen, row: 0, column: 0)
        expect(
            all?.isItalic == true &&
                all?.isBlinking == true &&
                all?.isHidden == true &&
                all?.isStruckThrough == true,
            "SGR 3/5/8/9 应分别公开斜体、闪烁、隐藏和删除线")
        expect(
            style(in: screen, row: 0, column: 1)?.isItalic == false &&
                style(in: screen, row: 0, column: 1)?.isBlinking == true,
            "SGR 23 应只复位斜体")
        expect(
            style(in: screen, row: 0, column: 2)?.isBlinking == false &&
                style(in: screen, row: 0, column: 2)?.isHidden == true,
            "SGR 25 应只复位闪烁")
        expect(
            style(in: screen, row: 0, column: 3)?.isHidden == false &&
                style(in: screen, row: 0, column: 3)?
                    .isStruckThrough == true,
            "SGR 28 应只复位隐藏")
        expect(
            style(in: screen, row: 0, column: 4)?
                .isStruckThrough == false,
            "SGR 29 应复位删除线")
        expect(
            style(in: screen, row: 0, column: 5) == TerminalStyle(),
            "SGR 0 应同时复位扩展文本属性")
    }

    private static func testHiddenTextAccessibility() {
        var screen = TerminalScreen(columns: 24, rows: 2)
        screen.append(Array(
            "公开\u{1b}[8msecret\u{1b}[0m尾".utf8))

        let line = screen.visibleLines[0]
        expect(
            line.text.contains("secret") &&
                !line.accessibilityText.contains("secret") &&
                line.accessibilityText.contains("公开") &&
                line.accessibilityText.contains("尾"),
            "SGR 8 应保留终端单元格但不得向辅助功能公开隐藏文本")
    }

    private static func testCharacterEditing() {
        var screen = TerminalScreen(columns: 6, rows: 2)
        screen.append(Array("abcdef".utf8))
        screen.append(Array("\u{1b}[1;3H\u{1b}[2P".utf8))
        expect(
            texts(screen)[0] == "abef",
            "CSI P 应删除字符并左移剩余内容")

        screen.append(Array("\u{1b}[1;3H\u{1b}[2@XY".utf8))
        expect(
            texts(screen)[0] == "abXYef",
            "CSI @ 应插入空位并保留右侧内容")

        screen.append(Array("\u{1b}[2G\u{1b}[3X".utf8))
        expect(
            texts(screen)[0] == "a   ef",
            "字符擦除不应改变后方字符位置")

        var wideScreen = TerminalScreen(columns: 4, rows: 1)
        wideScreen.append(Array("晚X".utf8))
        wideScreen.append(Array("\u{1b}[2G\u{1b}[X".utf8))
        expect(
            texts(wideScreen)[0] == "  X",
            "擦除宽字符任一列时应清除完整字形")
    }

    private static func testLineEditing() {
        var screen = TerminalScreen(columns: 4, rows: 5)
        screen.append(Array("A\r\nB\r\nC\r\nD\r\nE".utf8))
        screen.append(Array("\u{1b}[2;4r\u{1b}[3;1H\u{1b}[L".utf8))
        expect(
            texts(screen) == ["A", "B", "", "C", "E"],
            "CSI L 应只在滚动区内插入行")

        screen.append(Array("\u{1b}[M".utf8))
        expect(
            texts(screen) == ["A", "B", "C", "", "E"],
            "CSI M 应只在滚动区内删除行")
    }

    private static func testDelayedWrapEditingRules() {
        let resetSequences = [
            "\u{1b}[J", "\u{1b}[1J", "\u{1b}[2J",
            "\u{1b}[K", "\u{1b}[1K", "\u{1b}[2K",
            "\u{1b}[@", "\u{1b}[P", "\u{1b}[X",
            "\u{1b}[L", "\u{1b}[M",
        ]
        for sequence in resetSequences {
            var screen = TerminalScreen(columns: 4, rows: 2)
            screen.append(Array(("ABCD" + sequence + "X").utf8))
            expect(
                screen.cursor.row == 0 &&
                    screen.buffers[0].lines[0].cells[3].text == "X",
                "编辑命令 \(sequence.debugDescription) 应清除延迟换行")
        }

        for sequence in ["\u{1b}[3J", "\u{1b}[S", "\u{1b}[T"] {
            var screen = TerminalScreen(columns: 4, rows: 2)
            screen.append(Array(("ABCD" + sequence + "X").utf8))
            expect(
                screen.cursor.row == 1,
                "滚动或 ED 3 \(sequence.debugDescription) 应保留延迟换行")
        }
    }

    private static func testScrollRegion() {
        var screen = TerminalScreen(columns: 4, rows: 5)
        screen.append(Array("A\r\nB\r\nC\r\nD\r\nE".utf8))
        screen.append(Array("\u{1b}[2;4r\u{1b}[S".utf8))
        expect(
            texts(screen) == ["A", "C", "D", "", "E"],
            "CSI S 应只向上滚动当前滚动区")
        expect(
            screen.scrollbackLines.isEmpty,
            "局部滚动区内容不应进入主屏历史")

        screen.append(Array("\u{1b}[T".utf8))
        expect(
            texts(screen) == ["A", "", "C", "D", "E"],
            "CSI T 应只向下滚动当前滚动区")
    }

    private static func testBoundedScrollback() {
        var screen = TerminalScreen(
            columns: 4, rows: 3, scrollbackLimit: 2)
        screen.append(Array("1\r\n2\r\n3".utf8))
        let initialIDs = screen.visibleLines.map(\.id)
        screen.append(Array("\r\n4\r\n5\r\n6".utf8))

        expect(
            screen.scrollbackLines.map(\.text) == ["2", "3"],
            "主屏历史应遵守有界 scrollback 限制")
        expect(
            screen.scrollbackLines.map(\.id) ==
                [initialIDs[1], initialIDs[2]],
            "滚入历史的行应保留稳定身份")
        expect(
            screen.visibleLines.map(\.text) == ["4", "5", "6"],
            "主屏滚动后应保留最新可见行")
        expect(
            screen.renderedLines.map(\.text) ==
                ["2", "3", "4", "5", "6"],
            "主屏渲染接口应按历史到可见屏顺序公开行")

        screen.resize(columns: 6, rows: 3)
        expect(
            screen.scrollbackLines.allSatisfy {
                $0.runs.reduce(0) { $0 + $1.columnCount } == 6
            },
            "调整列数时每条历史行应只被调整一次")

        screen.append(Array("\u{1b}[?2004h".utf8))
        let visibleBeforeClear = screen.visibleLines
        let cursorBeforeClear = screen.cursor
        let modesBeforeClear = screen.modes
        screen.clearScrollback()
        expect(
            screen.scrollbackLines.isEmpty &&
                screen.visibleLines == visibleBeforeClear &&
                screen.cursor == cursorBeforeClear &&
                screen.modes == modesBeforeClear,
            "clearScrollback 应只清历史并保留活动屏、光标和模式")

        screen.append(Array("\r\n7".utf8))
        expect(
            !screen.scrollbackLines.isEmpty,
            "CSI J 模式 3 测试前应重新产生主屏历史")
        screen.append(Array("\u{1b}[3J".utf8))
        expect(
            screen.scrollbackLines.isEmpty,
            "CSI J 模式 3 应清除主屏历史")

        var ringScreen = TerminalScreen(
            columns: 4, rows: 1, scrollbackLimit: 3)
        ringScreen.append(Array("0".utf8))
        for value in 1...8 {
            ringScreen.append(Array("\r\n\(value)".utf8))
        }
        expect(
            ringScreen.scrollback.count == 3 &&
                ringScreen.scrollbackHeadIndex != 0 &&
                ringScreen.scrollbackLines.map(\.text) ==
                    ["5", "6", "7"],
            "满历史持续输出应以固定容量环形顺序淘汰旧行")
        ringScreen.clearScrollback()
        expect(
            ringScreen.scrollback.isEmpty &&
                ringScreen.scrollbackHeadIndex == 0,
            "清空环形历史应同时复位逻辑首行")
    }

    private static func testAlternateScreenAndModes() {
        var screen = TerminalScreen(
            columns: 6, rows: 2, scrollbackLimit: 4)
        screen.append(Array("main".utf8))
        let mainID = screen.visibleLines[0].id
        let mainCursor = screen.cursor

        screen.append(Array("\u{1b}[?1h\u{1b}[?25l".utf8))
        screen.append(Array("\u{1b}[?104".utf8))
        screen.append(Array("9h".utf8))
        expect(
            screen.modes == TerminalModes(
                usesApplicationCursorKeys: true,
                isCursorVisible: false,
                usesAlternateScreen: true),
            "DEC 私有模式应公开应用光标、光标可见性和备用屏状态")
        expect(
            screen.cursor.isVisible == false,
            "渲染光标应反映 DEC ?25 模式")

        screen.append(Array("a\r\nb\r\nc".utf8))
        expect(
            screen.renderedLines.map(\.text) == ["b", "c"],
            "备用屏滚动时不应混入主屏历史")
        expect(
            screen.scrollbackLines.isEmpty,
            "备用屏内容不应写入 scrollback")

        screen.append(Array((
            "\u{1b}[31m" +
            "\u{1b}]8;;https://example.com/alternate\u{07}X"
        ).utf8))
        screen.append(Array("\u{1b}[?1049l".utf8))
        expect(
            screen.visibleLines[0].text == "main" &&
                screen.visibleLines[0].id == mainID,
            "离开备用屏应恢复主屏内容及物理行身份")
        expect(
            screen.cursor.row == mainCursor.row &&
                screen.cursor.column == mainCursor.column,
            "DEC ?1049 应恢复主屏光标")
        screen.append(Array("Z".utf8))
        expect(
            style(
                in: screen,
                row: mainCursor.row,
                column: mainCursor.column) == TerminalStyle() &&
                run(
                    in: screen,
                    row: mainCursor.row,
                    column: mainCursor.column)?.hyperlink == nil,
            "DEC ?1049 离开后不得把备用屏 SGR 与 OSC 8 元数据带回主屏")

        screen.append(Array("\u{1b}[?1l\u{1b}[?25h".utf8))
        expect(
            screen.modes == TerminalModes(),
            "DEC 私有模式关闭后应回到默认状态")

        var legacyScreen = TerminalScreen(columns: 8, rows: 2)
        legacyScreen.append(Array((
            "primary\u{1b}[?47h\u{1b}[Hlegacy\u{1b}[?47l" +
            "\u{1b}[?47h"
        ).utf8))
        expect(
            legacyScreen.visibleLines[0].text == "legacy",
            "DEC ?47 应切换并保留备用屏内容，而不是按 ?1049 清空")
        legacyScreen.append(Array("\u{1b}[?47l".utf8))
        expect(
            legacyScreen.visibleLines[0].text == "primary",
            "DEC ?47 离开后应返回原主屏内容")

        var clearOnExitScreen = TerminalScreen(columns: 8, rows: 2)
        clearOnExitScreen.append(Array((
            "main\u{1b}[?47h\u{1b}[Hcached\u{1b}[?47l" +
            "\u{1b}[?1047h"
        ).utf8))
        expect(
            clearOnExitScreen.visibleLines[0].text == "cached",
            "DEC ?1047 进入时应沿用尚未清除的备用屏")
        clearOnExitScreen.append(Array(
            "\u{1b}[?1047l\u{1b}[?47h".utf8))
        expect(
            clearOnExitScreen.visibleLines.allSatisfy(\.text.isEmpty),
            "DEC ?1047 离开时应清空备用屏，而 ?47 不应清空")
        clearOnExitScreen.append(Array("\u{1b}[?47l".utf8))

        var saveAndClearScreen = TerminalScreen(columns: 8, rows: 3)
        saveAndClearScreen.append(Array((
            "\u{1b}[?47h\u{1b}[Hold\u{1b}[?47l" +
            "\u{1b}[2;3H\u{1b}[31m\u{1b}[?1049h"
        ).utf8))
        expect(
            saveAndClearScreen.visibleLines.allSatisfy(\.text.isEmpty) &&
                saveAndClearScreen.cursor.row == 1 &&
                saveAndClearScreen.cursor.column == 2,
            "DEC ?1049 进入时应清屏但保留当前光标位置")
        saveAndClearScreen.append(Array("X\u{1b}[?1049lY".utf8))
        expect(
            saveAndClearScreen.cursor.row == 1 &&
                saveAndClearScreen.cursor.column == 3 &&
                style(
                    in: saveAndClearScreen,
                    row: 1,
                    column: 2)?.foreground == .indexed(1),
            "DEC ?1049 应恢复保存的光标与 SGR 后继续输出")

        var mixedExitScreen = TerminalScreen(columns: 8, rows: 2)
        mixedExitScreen.append(Array((
            "\u{1b}[1;4H\u{1b}[?1049h" +
            "\u{1b}[2;7H\u{1b}[?47l"
        ).utf8))
        expect(
            !mixedExitScreen.modes.usesAlternateScreen &&
                mixedExitScreen.cursor.row == 1 &&
                mixedExitScreen.cursor.column == 6,
            "DEC ?47l 只应切回主屏，不得执行 ?1049 光标恢复")
        mixedExitScreen.append(Array("\u{1b}[?1049l".utf8))
        expect(
            mixedExitScreen.cursor.row == 0 &&
                mixedExitScreen.cursor.column == 3,
            "DEC ?1049l 应独立执行对应的光标恢复")

        var queryScreen = TerminalScreen(columns: 8, rows: 2)
        queryScreen.append(Array((
            "\u{1b}[?1049h\u{1b}[?1049$p" +
            "\u{1b}[?1049l\u{1b}[?1049$p"
        ).utf8))
        expect(
            queryScreen.takePendingResponseBytes(
                maximumCount: Int.max) ==
                Array((
                    "\u{1b}[?1049;1$y" +
                    "\u{1b}[?1049;2$y"
                ).utf8),
            "DECRQM 应与 ?1049 实际切换状态一致")

        var resizedAlternate = TerminalScreen(columns: 8, rows: 3)
        resizedAlternate.append(Array(
            "\u{1b}[2;1H\u{1b}[?1049h".utf8))
        resizedAlternate.resize(columns: 8, rows: 2)
        resizedAlternate.append(Array("\u{1b}[?1049l".utf8))
        expect(
            resizedAlternate.cursor.row == 0,
            "备用屏期间缩小高度后应按主屏移除行数修正 1049 保存坐标")
    }

    private static func testSaveRestoreAndOSCFiltering() {
        var screen = TerminalScreen(columns: 12, rows: 3)
        screen.append(Array(
            "A\u{1b}[s\u{1b}[2;5HX\u{1b}[uB".utf8))
        expect(
            texts(screen)[0] == "AB" &&
                texts(screen)[1] == "    X",
            "CSI s/u 应保存并恢复光标")
        expect(
            screen.cursor.row == 0 && screen.cursor.column == 2,
            "恢复后继续输出应从保存位置开始")

        var oscScreen = TerminalScreen(columns: 20, rows: 2)
        oscScreen.append(Array("before\u{1b}]0;hidden\u{1b}".utf8))
        oscScreen.append(Array("\\after".utf8))
        oscScreen.append(Array("\u{1b}]2;ignored\u{07}!".utf8))
        oscScreen.append(Array("\u{1b}[>0c?".utf8))
        expect(
            texts(oscScreen)[0] == "beforeafter!?",
            "OSC 与不支持的 CSI 不应把控制序列内容泄漏到屏幕")
        expect(
            oscScreen.takePendingTitle() == "ignored",
            "OSC 0/2 应跨分片解析标题，并以最新完整标题为准")

        oscScreen.append(Array("\u{1b}]1;unsupported\u{07}".utf8))
        expect(
            oscScreen.takePendingTitle() == nil,
            "非 0/2 的 OSC 不应发布终端标题")

        oscScreen.append(Array("\u{1b}]2;".utf8))
        oscScreen.append([UInt8](
            repeating: UInt8(ascii: "x"),
            count: TerminalScreen.maximumOSCPayloadBytes + 4_096))
        expect(
            oscScreen.oscPayloadBytes.count <=
                TerminalScreen.maximumOSCPayloadBytes,
            "未结束的 OSC 负载不得无限增长")
        oscScreen.append([0x07])
        expect(
            oscScreen.takePendingTitle() == nil,
            "超限 OSC 标题应整段丢弃")

        oscScreen.append(Array("\u{1b}]2;  恢复标题  \u{07}".utf8))
        expect(
            oscScreen.takePendingTitle() == "恢复标题",
            "超限 OSC 结束后应恢复后续标题解析")

        var decSaveScreen = TerminalScreen(columns: 8, rows: 2)
        decSaveScreen.append(Array((
            "\u{1b}[32m\u{1b}(0\u{1b}7" +
            "\u{1b}[31m\u{1b}(B\u{1b}8q"
        ).utf8))
        expect(
            decSaveScreen.visibleLines[0].text == "─" &&
                style(in: decSaveScreen, row: 0, column: 0)?
                    .foreground == .indexed(2),
            "DECSC/DECRC 应恢复字符集与 SGR，而不只恢复坐标")
    }

    private static func testStringControlTermination() {
        var screen = TerminalScreen(columns: 16, rows: 2)
        screen.append(Array("A\u{1b}Pignored".utf8))
        screen.append([0x9c])
        screen.append(Array("B\u{1b}Pcancelled".utf8))
        screen.append([0x18])
        screen.append(Array("C\u{1b}^private\u{1b}\\".utf8))
        screen.append(Array("D\u{1b}_application".utf8))
        screen.append([0x1a])
        screen.append(Array("E\u{1b}Xsecret\u{1b}\\F".utf8))

        expect(
            screen.visibleLines[0].text == "ABCDEF",
            "DCS/SOS/PM/APC 应接受 ST、C1 ST、CAN 与 SUB 并恢复普通输出")
    }

    private static func testOSC8Hyperlinks() {
        let link = "https://example.com/docs?q=watch"
        var screen = TerminalScreen(columns: 32, rows: 2)
        screen.append(Array("\u{1b}]8;id=docs;\(link)\u{1b}".utf8))
        screen.append(Array("\\链接".utf8))
        screen.append(Array("\u{1b}]8;;\u{07}普通".utf8))

        expect(
            run(in: screen, row: 0, column: 0)?.hyperlink == link &&
                run(in: screen, row: 0, column: 4)?.hyperlink == nil,
            "OSC 8 应跨分片保存链接，关闭后不得污染后续字符")
        expect(
            screen.visibleLines[0].runs.contains {
                $0.text == "链接" &&
                    $0.columnCount == 4 &&
                    $0.hyperlink == link
            },
            "渲染 run 应公开 OSC 8 URI 并保留宽字符列数")

        screen.resize(columns: 40, rows: 3)
        expect(
            run(in: screen, row: 0, column: 0)?.hyperlink == link,
            "调整终端尺寸后应保留已有单元格的链接元数据")

        var history = TerminalScreen(columns: 8, rows: 1)
        history.append(Array((
            "\u{1b}]8;;https://example.com/history\u{07}old" +
            "\u{1b}]8;;\u{07}\r\n"
        ).utf8))
        expect(
            history.scrollbackLines.first?.runs.first?.hyperlink ==
                "https://example.com/history",
            "带链接的单元格滚入 scrollback 后应保留 URI 元数据")

        var filtered = TerminalScreen(columns: 24, rows: 2)
        filtered.append(Array((
            "\u{1b}]8;;https://safe.example/\u{07}A" +
            "\u{1b}]8;;javascript:alert(1)\u{07}B" +
            "\u{1b}]8;;https://example.com/a b\u{07}C"
        ).utf8))
        filtered.append(
            Array("\u{1b}]8;;https://example.com/".utf8) +
            [0x01] +
            Array("bad\u{07}D".utf8))
        filtered.append(
            Array("\u{1b}]8;;".utf8) +
            [0xff, 0x07] +
            Array("E".utf8))
        expect(
            run(in: filtered, row: 0, column: 0)?.hyperlink ==
                "https://safe.example/" &&
                (1...4).allSatisfy {
                    run(in: filtered, row: 0, column: $0)?
                        .hyperlink == nil
                },
            "OSC 8 应拒绝非允许 scheme、空白、控制字符及非法 UTF-8")

        filtered.append(Array(
            "\u{1b}]8;;https://safe.example/incomplete".utf8))
        filtered.append([0x18])
        filtered.append(Array("F".utf8))
        expect(
            run(in: filtered, row: 0, column: 5)?.hyperlink == nil,
            "被取消的截断 OSC 8 不得创建链接或泄漏控制串")

        filtered.append(Array(
            "\u{1b}]8;;https://safe.example/active\u{07}G".utf8))
        filtered.append(Array("\u{1b}]8;;".utf8))
        filtered.append([UInt8](
            repeating: UInt8(ascii: "x"),
            count: TerminalScreen.maximumOSCPayloadBytes + 1_024))
        expect(
            filtered.oscPayloadBytes.count <=
                TerminalScreen.maximumOSCPayloadBytes,
            "超长 OSC 8 URI 缓冲不得超过固定上限")
        filtered.append([0x07])
        filtered.append(Array("H".utf8))
        expect(
            run(in: filtered, row: 0, column: 6)?.hyperlink ==
                "https://safe.example/active" &&
                run(in: filtered, row: 0, column: 7)?.hyperlink == nil,
            "超限 OSC 8 应关闭旧链接作用域并避免污染后续字符")

        filtered.append(Array((
            "\u{1b}]8;;mailto:test@example.com\u{1b}\\M" +
            "\u{1b}]8;;\u{1b}\\N"
        ).utf8))
        expect(
            run(in: filtered, row: 0, column: 8)?.hyperlink ==
                "mailto:test@example.com" &&
                run(in: filtered, row: 0, column: 9)?.hyperlink == nil,
            "OSC 8 应支持有界 mailto 链接及 ST 关闭序列")

        let withinPayloadButOverURIBound =
            "https://example.com/" +
            String(
                repeating: "x",
                count: TerminalScreen.maximumHyperlinkCharacters)
        filtered.append(Array(
            "\u{1b}]8;;\(withinPayloadButOverURIBound)\u{07}I".utf8))
        expect(
            run(in: filtered, row: 0, column: 10)?.hyperlink == nil,
            "即使 OSC 总负载未超限，过长 URI 也不得进入单元格")

        var reset = TerminalScreen(columns: 8, rows: 2)
        reset.append(Array(
            "\u{1b}]8;;https://example.com/reset\u{07}A\u{1b}cB"
                .utf8))
        expect(
            texts(reset)[0] == "B" &&
                run(in: reset, row: 0, column: 0)?.hyperlink == nil,
            "RIS 应清除活动链接，复位后的字符不得继承旧 URI")
    }

    private static func testDeviceQueriesAndResponses() {
        var screen = TerminalScreen(columns: 12, rows: 5)
        screen.append(Array("\u{1b}[999;999H".utf8))
        screen.append(Array((
            "\u{1b}[c\u{1b}[5n\u{1b}[6n" +
            "\u{1b}[>0c\u{1b}[?6n\u{1b}Z"
        ).utf8))
        let responses = screen.takePendingResponseBytes(
            maximumCount: Int.max)
        expect(
            responses == Array((
                "\u{1b}[?1;2c\u{1b}[0n\u{1b}[5;12R" +
                "\u{1b}[>0;0;0c\u{1b}[?5;12R\u{1b}[?1;2c"
            ).utf8),
            "DA、DSR、CPR、DECXCPR 与 DECID 应按真实光标位置响应")
        expect(
            screen.visibleLines.allSatisfy(\.text.isEmpty),
            "终端查询及响应不得泄漏进可见屏幕")

        var splitScreen = TerminalScreen(columns: 8, rows: 3)
        splitScreen.append(Array("\u{1b}[2;4H\u{1b}[".utf8))
        splitScreen.append(Array("6n".utf8))
        expect(
            splitScreen.takePendingResponseBytes(
                maximumCount: Int.max) ==
                Array("\u{1b}[2;4R".utf8),
            "跨输出分片的 CPR 查询仍应生成完整响应")

        var boundedScreen = TerminalScreen(columns: 8, rows: 3)
        for _ in 0..<1_000 {
            boundedScreen.append(Array("\u{1b}[5n".utf8))
        }
        expect(
            boundedScreen.pendingResponseBytes.count <=
                TerminalScreen.maximumPendingResponseBytes &&
                boundedScreen.pendingResponseBytes.count % 4 == 0,
            "恶意重复查询不得让响应队列无界增长或留下半条响应")
    }

    private static func testDECCharacterSetsAndModes() {
        var graphicsScreen = TerminalScreen(columns: 16, rows: 2)
        graphicsScreen.append(Array(
            "\u{1b}(0lqkxmj\u{1b}(BA".utf8))
        graphicsScreen.carriageReturn()
        graphicsScreen.lineFeed()
        graphicsScreen.append(Array(
            "\u{1b})0\u{0e}tuwv\u{0f}B".utf8))
        expect(
            texts(graphicsScreen) ==
                ["┌─┐│└┘A", "├┤┬┴B"],
            "G0/G1、SI/SO 与 DEC 特殊图形集应正确绘制全屏程序边框")

        var utf8SelectionScreen = TerminalScreen(columns: 8, rows: 1)
        utf8SelectionScreen.append(Array(
            "A\u{1b}%GB".utf8))
        expect(
            texts(utf8SelectionScreen)[0] == "AB",
            "不需要处理的 ESC 中间序列也不得把结束字节显示出来")

        var originScreen = TerminalScreen(columns: 6, rows: 5)
        originScreen.append(Array(
            "\u{1b}[2;4r\u{1b}[?6h\u{1b}[2;3H\u{1b}[99B\u{1b}[6n"
                .utf8))
        expect(
            originScreen.cursor.row == 3 &&
                originScreen.cursor.column == 2 &&
                originScreen.takePendingResponseBytes(
                    maximumCount: Int.max) ==
                    Array("\u{1b}[3;3R".utf8),
            "DECOM 应限制滚动区坐标，CPR 行号应相对上边界")
        originScreen.append(Array("\u{1b}[?6l".utf8))
        expect(
            originScreen.cursor.row == 0 &&
                originScreen.cursor.column == 0,
            "关闭 DECOM 应把光标归位到屏幕原点")

        var wrapScreen = TerminalScreen(columns: 4, rows: 2)
        wrapScreen.append(Array("\u{1b}[?7labcdZ".utf8))
        expect(
            texts(wrapScreen) == ["abcZ", ""] &&
                wrapScreen.cursor.row == 0,
            "关闭 DECAWM 后末列应覆盖而不是换行")
        wrapScreen.append(Array("\u{1b}[?7hQR".utf8))
        expect(
            texts(wrapScreen) == ["abcQ", "R"],
            "重新开启 DECAWM 后应恢复延迟换行")

        var insertScreen = TerminalScreen(columns: 6, rows: 2)
        insertScreen.append(Array(
            "abcdef\r\u{1b}[3C\u{1b}[4hXY\u{1b}[4l".utf8))
        expect(
            texts(insertScreen)[0] == "abcXYd" &&
                !insertScreen.modes.usesInsertMode,
            "ANSI IRM 应插入字符并能恢复替换模式")

        var modeScreen = TerminalScreen(columns: 8, rows: 3)
        modeScreen.append(Array((
            "\u{1b}[?1h\u{1b}[?7l\u{1b}[4h" +
            "\u{1b}[?1$p\u{1b}[?7$p\u{1b}[4$p\u{1b}[?2004$p"
        ).utf8))
        expect(
            modeScreen.takePendingResponseBytes(
                maximumCount: Int.max) ==
                Array((
                    "\u{1b}[?1;1$y\u{1b}[?7;2$y" +
                    "\u{1b}[4;1$y\u{1b}[?2004;2$y"
                ).utf8),
            "DECRQM 应如实报告已实现模式的启用与关闭状态")
    }

    private static func testPrivateCSIIsolation() {
        var screen = TerminalScreen(columns: 12, rows: 2)
        screen.append(Array("keep\u{1b}[>4;2mX".utf8))
        let styleAfterXTMODKEYS =
            style(in: screen, row: 0, column: 4)
        screen.append(Array("\u{1b}[>2JY".utf8))
        screen.append(Array("\u{1b}[>4hZ".utf8))

        expect(
            screen.visibleLines[0].text == "keepXYZ" &&
                styleAfterXTMODKEYS == TerminalStyle() &&
                !screen.modes.usesInsertMode,
            "不支持的 CSI > 私有序列不得落入 SGR、ED 或 ANSI mode")

        screen.append(Array("\u{1b}[>0c".utf8))
        expect(
            screen.takePendingResponseBytes(maximumCount: Int.max) ==
                Array("\u{1b}[>0;0;0c".utf8),
            "私有分派收紧后仍应保留已实现的 secondary DA")

        var malformedScreen = TerminalScreen(columns: 12, rows: 2)
        malformedScreen.append(Array(
            "A\u{1b}[31?mB\u{1b}[??31mC".utf8))
        expect(
            malformedScreen.visibleLines[0].text == "ABC" &&
                style(
                    in: malformedScreen,
                    row: 0,
                    column: 1) == TerminalStyle() &&
                style(
                    in: malformedScreen,
                    row: 0,
                    column: 2) == TerminalStyle(),
            "参数后的私有标记或重复私有标记应丢弃整条 CSI")
    }

    private static func testCursorStyleAndInputModes() {
        var screen = TerminalScreen(columns: 12, rows: 3)
        screen.append(Array("\u{1b}[1 q".utf8))
        expect(
            screen.cursor.shapeOverride == .block &&
                screen.cursor.blinkOverride == true,
            "DECSCUSR 1 应选择闪烁块状光标")
        screen.append(Array("\u{1b}[2 q".utf8))
        expect(
            screen.cursor.shapeOverride == .block &&
                screen.cursor.blinkOverride == false,
            "DECSCUSR 2 应选择不闪烁块状光标")
        screen.append(Array("\u{1b}[3 q".utf8))
        expect(
            screen.cursor.shapeOverride == .underline &&
                screen.cursor.blinkOverride == true,
            "DECSCUSR 3 应选择闪烁下划线光标")

        screen.resize(columns: 16, rows: 4)
        screen.append(Array("\u{1b}[9 q".utf8))
        expect(
            screen.cursor.shapeOverride == .underline &&
                screen.cursor.blinkOverride == true,
            "resize 应保留动态光标，未知 DECSCUSR 参数不得改动状态")

        screen.append(Array("\u{1b}[6 q".utf8))
        expect(
            screen.cursor.shapeOverride == .beam &&
                screen.cursor.blinkOverride == false,
            "DECSCUSR 6 应选择不闪烁竖线光标")
        screen.append(Array("\u{1b}[ q".utf8))
        expect(
            screen.cursor.shapeOverride == nil &&
                screen.cursor.blinkOverride == nil,
            "DECSCUSR 0 应恢复用户外观中的默认光标设置")

        screen.append(Array((
            "\u{1b}[?1000h\u{1b}[?1002h" +
            "\u{1b}[?1006h\u{1b}[?1004h\u{1b}[?2004h"
        ).utf8))
        expect(
            screen.modes.mouseTrackingMode == .buttonEvent &&
                screen.modes.usesSGRMouseEncoding &&
                screen.modes.reportsFocusEvents &&
                screen.modes.usesBracketedPaste,
            "鼠标、焦点和 bracketed paste 控制串应维护终端模式")
        expect(
            screen.visibleLines.allSatisfy(\.text.isEmpty),
            "输入协议模式控制串不得显示在活动屏")

        screen.append(Array((
            "\u{1b}[?1000$p\u{1b}[?1002$p\u{1b}[?1006$p" +
            "\u{1b}[?1004$p\u{1b}[?2004$p"
        ).utf8))
        expect(
            screen.takePendingResponseBytes(maximumCount: Int.max) ==
                Array((
                    "\u{1b}[?1000;2$y\u{1b}[?1002;1$y" +
                    "\u{1b}[?1006;1$y\u{1b}[?1004;1$y" +
                    "\u{1b}[?2004;1$y"
                ).utf8),
            "DECRQM 应报告鼠标、焦点和 bracketed paste 当前状态")

        screen.append(Array((
            "\u{1b}[?1002l\u{1b}[?1006l" +
            "\u{1b}[?1004l\u{1b}[?2004l"
        ).utf8))
        expect(
            screen.modes.mouseTrackingMode == .disabled &&
                !screen.modes.usesSGRMouseEncoding &&
                !screen.modes.reportsFocusEvents &&
                !screen.modes.usesBracketedPaste,
            "关闭输入协议模式后应恢复默认状态")

        screen.append(Array("\u{1b}[1 q\u{1b}c".utf8))
        expect(
            screen.cursor.shapeOverride == nil &&
                screen.cursor.blinkOverride == nil &&
                screen.modes == TerminalModes(),
            "RIS 应复位动态光标和输入协议模式")
    }

    private static func testBoundedCSIParameters() {
        var screen = TerminalScreen(columns: 20, rows: 2)
        screen.append(Array("\u{1b}[".utf8))
        screen.append(
            [UInt8](
                repeating: 0x31,
                count: TerminalScreen.maximumCSIParameterBytes + 4_096))
        expect(
            screen.csiParameterBytes.count <=
                TerminalScreen.maximumCSIParameterBytes,
            "未结束的 CSI 参数不得无限增长")

        screen.append(Array("mvisible".utf8))
        expect(
            texts(screen)[0] == "visible",
            "超限 CSI 应整段丢弃，并在结束字节后恢复普通输出")

        var intermediateScreen = TerminalScreen(columns: 20, rows: 2)
        intermediateScreen.append(Array("\u{1b}[".utf8))
        intermediateScreen.append([UInt8](
            repeating: 0x24,
            count: TerminalScreen.maximumCSIIntermediateBytes + 4_096))
        expect(
            intermediateScreen.csiIntermediateBytes.count <=
                TerminalScreen.maximumCSIIntermediateBytes,
            "未结束的 CSI 中间字节不得无限增长")
        intermediateScreen.append(Array("mvisible".utf8))
        expect(
            texts(intermediateScreen)[0] == "visible",
            "超限 CSI 中间段结束后应恢复普通输出")
    }

    private static func testResizeAndBoundaries() {
        var screen = TerminalScreen(columns: 4, rows: 3)
        screen.append(Array("A\r\nB\r\nC".utf8))
        let initialIDs = screen.visibleLines.map(\.id)

        screen.resize(columns: 6, rows: 2)
        expect(
            screen.scrollbackLines.map(\.id) == [initialIDs[0]] &&
                screen.visibleLines.map(\.id) ==
                    [initialIDs[1], initialIDs[2]],
            "缩小行数应把光标上方内容移入历史")

        screen.resize(columns: 6, rows: 3)
        expect(
            screen.scrollbackLines.isEmpty &&
                screen.visibleLines.map(\.id) == initialIDs,
            "重新扩大行数应尽量恢复刚移入历史的内容与身份")

        var wideScreen = TerminalScreen(columns: 3, rows: 2)
        wideScreen.append(Array("ab晚".utf8))
        expect(
            wideScreen.visibleLines[0].isWrapped &&
                texts(wideScreen) == ["ab", "晚"],
            "双宽字符无法容纳在行尾时应整体换行")
        wideScreen.resize(columns: 1, rows: 2)
        expect(
            texts(wideScreen) == ["a", "晚"] &&
                wideScreen.visibleLines.allSatisfy {
                $0.runs.allSatisfy { $0.columnCount <= 1 }
            },
            "缩窄列数应保留内容且不留下越界的宽字符续格")
        wideScreen.resize(columns: 2, rows: 2)
        expect(
            run(in: wideScreen, row: 1, column: 0)?
                .columnCount == 2,
            "重新放宽列数后应恢复宽字符的双列语义")

        var historyScreen = TerminalScreen(columns: 8, rows: 2)
        historyScreen.append(Array(
            "abcdefgh\r\nijklmnop\r\nqrstuvwx".utf8))
        expect(
            historyScreen.scrollbackLines.first?.text == "abcdefgh",
            "缩列历史测试应先产生完整的宽行")
        historyScreen.resize(columns: 4, rows: 2)
        expect(
            historyScreen.columns == 4 &&
                historyScreen.scrollbackLines.first?.text == "abcdefgh" &&
                historyScreen.scrollbackLines.first?.columnCount == 8 &&
                historyScreen.renderedLines.first?.columnCount == 8,
            "缩列后既有 scrollback 应保留右侧内容和原始渲染宽度")
        expect(
            historyScreen.visibleLines.allSatisfy {
                $0.columnCount == 4
            },
            "缩列后活动屏仍应严格使用新的逻辑列数")
        historyScreen.resize(columns: 4, rows: 3)
        expect(
            historyScreen.scrollbackLines.first?.text == "abcdefgh" &&
                historyScreen.scrollbackLines.first?.columnCount == 8 &&
                historyScreen.visibleLines.allSatisfy {
                    $0.columnCount == 4
                },
            "增高行数时不应把宽历史塞回窄活动屏或丢失其右侧内容")
        historyScreen.resize(columns: 6, rows: 2)
        expect(
            historyScreen.scrollbackLines.first?.text == "abcdefgh" &&
                historyScreen.scrollbackLines.first?.columnCount == 8,
            "再次调整列数不得覆盖已保留的历史宽度")

        var linkScreen = TerminalScreen(columns: 4, rows: 2)
        linkScreen.append(Array((
            "\u{1b}]8;;https://example.com/resize\u{07}晚A" +
            "\u{1b}]8;;\u{07}"
        ).utf8))
        linkScreen.resize(columns: 8, rows: 3)
        expect(
            run(in: linkScreen, row: 0, column: 0)?.hyperlink ==
                "https://example.com/resize" &&
                run(in: linkScreen, row: 0, column: 2)?.hyperlink ==
                    "https://example.com/resize",
            "resize 应保留宽字符及相邻字符的 OSC 8 链接元数据")

        var linkRoundTrip = TerminalScreen(columns: 2, rows: 2)
        linkRoundTrip.append(Array((
            "\u{1b}]8;;https://example.com/round-trip\u{07}晚" +
            "\u{1b}]8;;\u{07}"
        ).utf8))
        linkRoundTrip.resize(columns: 1, rows: 2)
        linkRoundTrip.resize(columns: 2, rows: 2)
        expect(
            run(in: linkRoundTrip, row: 0, column: 0)?
                .columnCount == 2 &&
                run(in: linkRoundTrip, row: 0, column: 0)?
                    .hyperlink ==
                    "https://example.com/round-trip",
            "宽字符缩列再扩列后应同时恢复双列与 OSC 8 元数据")
        linkRoundTrip.append(Array("X".utf8))
        expect(
            texts(linkRoundTrip) == ["晚", "X"] &&
                linkRoundTrip.visibleLines[0].isWrapped,
            "缩列再扩列后应恢复逻辑光标列与延迟换行")

        var cursorRoundTrip = TerminalScreen(columns: 6, rows: 2)
        cursorRoundTrip.append(Array(
            "\u{1b}[6G\u{1b}[s".utf8))
        cursorRoundTrip.resize(columns: 2, rows: 2)
        cursorRoundTrip.carriageReturn()
        cursorRoundTrip.resize(columns: 6, rows: 2)
        expect(
            cursorRoundTrip.cursor.column == 0,
            "缩列后的显式水平移动应取代待恢复的逻辑列")
        cursorRoundTrip.append(Array("\u{1b}[u".utf8))
        expect(
            cursorRoundTrip.cursor.column == 5,
            "resize 不应永久截断 DECSC 保存的逻辑列")

        var boundaryScreen = TerminalScreen(
            columns: 1, rows: 1, scrollbackLimit: 0)
        boundaryScreen.append(Array(
            "\u{1b}[999;999H\u{1b}[999PZ\r\nQ".utf8))
        expect(
            boundaryScreen.cursor.row == 0 &&
                boundaryScreen.cursor.column == 0 &&
                boundaryScreen.visibleLines.count == 1 &&
                boundaryScreen.scrollbackLines.isEmpty,
            "极端参数与单格屏幕应被边界限制且不产生历史")
    }

    private static func testDroppedOutputRecovery() {
        var screen = TerminalScreen(columns: 40, rows: 3)
        screen.append(Array("\u{1b}[31".utf8))
        screen.reportDroppedBytes(17)
        screen.append(Array("prompt".utf8))

        expect(
            screen.renderedText.contains("[已省略 17 字节输出]") &&
                screen.renderedText.contains("prompt"),
            "丢失输出后应重置解析状态并显示可诊断标记")
    }

}

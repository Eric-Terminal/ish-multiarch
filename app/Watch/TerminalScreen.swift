enum TerminalColor: Equatable {
    case `default`
    case indexed(UInt8)
    case rgb(red: UInt8, green: UInt8, blue: UInt8)
}

struct TerminalStyle: Equatable {
    var foreground: TerminalColor = .default
    var background: TerminalColor = .default
    var isBold = false
    var isDim = false
    var isUnderlined = false
    var isInverse = false
    var isItalic = false
    var isStruckThrough = false
    var isHidden = false
    var isBlinking = false
}

struct TerminalRenderRun: Equatable {
    let startColumn: Int
    let columnCount: Int
    let text: String
    let style: TerminalStyle
    let hyperlink: String?
}

struct TerminalRenderLine: Identifiable, Equatable {
    let id: UInt64
    let runs: [TerminalRenderRun]
    let columnCount: Int
    let isWrapped: Bool

    var text: String {
        var value = runs.map(\.text).joined()
        while value.last == " " {
            value.removeLast()
        }
        return value
    }

    var accessibilityText: String {
        var value = ""
        for run in runs {
            if run.style.isHidden {
                value.append(String(
                    repeating: " ",
                    count: run.columnCount))
            } else {
                value.append(run.text)
            }
        }
        while value.last == " " {
            value.removeLast()
        }
        return value
    }
}

enum TerminalCursorShape: Equatable {
    case block
    case beam
    case underline
}

struct TerminalCursor: Equatable {
    let row: Int
    let column: Int
    let isVisible: Bool
    let shapeOverride: TerminalCursorShape?
    let blinkOverride: Bool?

    init(
        row: Int,
        column: Int,
        isVisible: Bool,
        shapeOverride: TerminalCursorShape? = nil,
        blinkOverride: Bool? = nil
    ) {
        self.row = row
        self.column = column
        self.isVisible = isVisible
        self.shapeOverride = shapeOverride
        self.blinkOverride = blinkOverride
    }
}

enum TerminalMouseTrackingMode: Equatable {
    case disabled
    case normal
    case buttonEvent
    case anyEvent
}

struct TerminalModes: Equatable {
    var usesApplicationCursorKeys = false
    var isCursorVisible = true
    var usesAlternateScreen = false
    var usesOriginMode = false
    var usesAutoWrap = true
    var usesInsertMode = false
    var usesBracketedPaste = false
    var reportsFocusEvents = false
    var mouseTrackingMode = TerminalMouseTrackingMode.disabled
    var usesSGRMouseEncoding = false
}

enum VTCharacterSet: Equatable {
    case ascii
    case decSpecialGraphics
}

struct TerminalScreen {
    static let maximumCSIParameterBytes = 256
    static let maximumCSIIntermediateBytes = 2
    static let maximumOSCPayloadBytes = 512
    static let maximumTerminalTitleCharacters = 128
    static let maximumHyperlinkCharacters = 384
    static let maximumPendingResponseBytes = 1_024
    static let maximumCellTextScalars = 32

    struct Cell: Equatable {
        var text: String
        var width: Int
        var naturalWidth: Int
        var style: TerminalStyle
        var hyperlink: String?

        static func blank(style: TerminalStyle = TerminalStyle()) -> Cell {
            Cell(
                text: "",
                width: 1,
                naturalWidth: 1,
                style: style,
                hyperlink: nil)
        }

        static func continuation(
            style: TerminalStyle,
            hyperlink: String?
        ) -> Cell {
            Cell(
                text: "",
                width: 0,
                naturalWidth: 0,
                style: style,
                hyperlink: hyperlink)
        }
    }

    struct Line: Equatable {
        let id: UInt64
        var cells: [Cell]
        var isWrapped = false
    }

    struct SavedCursorState: Equatable {
        var row = 0
        var column = 0
        var style = TerminalStyle()
        var hyperlink: String?
        var g0CharacterSet = VTCharacterSet.ascii
        var g1CharacterSet = VTCharacterSet.ascii
        var usesG1CharacterSet = false
        var usesOriginMode = false
        var usesAutoWrap = true
        var hasPendingWrap = false
    }

    struct Buffer: Equatable {
        var lines: [Line]
        var cursorRow = 0
        var cursorColumn = 0
        var savedCursorState = SavedCursorState()
        var scrollTop = 0
        var scrollBottom: Int
        var hasPendingWrap = false
        var resizeSavedCursorColumn: Int?
        var resizeSavedPendingWrap = false
        var currentStyle = TerminalStyle()
        var activeHyperlink: String?
        var g0CharacterSet = VTCharacterSet.ascii
        var g1CharacterSet = VTCharacterSet.ascii
        var usesG1CharacterSet = false
    }

    private(set) var columns: Int
    private(set) var rows: Int
    private(set) var modes = TerminalModes()
    let scrollbackLimit: Int
    var cursorShapeOverride: TerminalCursorShape?
    var cursorBlinkOverride: Bool?

    var cursor: TerminalCursor {
        let buffer = buffers[activeBufferIndex]
        return TerminalCursor(
            row: buffer.cursorRow,
            column: buffer.cursorColumn,
            isVisible: modes.isCursorVisible,
            shapeOverride: cursorShapeOverride,
            blinkOverride: cursorBlinkOverride)
    }

    var visibleLines: [TerminalRenderLine] {
        buffers[activeBufferIndex].lines.map(render)
    }

    var scrollbackLines: [TerminalRenderLine] {
        guard !scrollback.isEmpty else { return [] }
        return (0..<scrollback.count).map {
            render(scrollback[scrollbackPhysicalIndex(for: $0)])
        }
    }

    // 备用屏是临时工作区，不与主屏历史混合。
    var renderedLines: [TerminalRenderLine] {
        if modes.usesAlternateScreen {
            return visibleLines
        }
        return scrollbackLines + visibleLines
    }

    var visibleText: String {
        visibleLines.map(\.text).joined(separator: "\n")
    }

    var renderedText: String {
        renderedLines.map(\.text).joined(separator: "\n")
    }

    var buffers: [Buffer]
    var scrollback: [Line] = []
    var scrollbackHeadIndex = 0
    var nextLineID: UInt64

    var parserState = VTParserState.ground
    var csiParameterBytes: [UInt8] = []
    var csiIntermediateBytes: [UInt8] = []
    var csiPrivateMarker: UInt8?
    var escapeIntermediateByte: UInt8?
    var oscPayloadBytes: [UInt8] = []
    var oscPayloadOverflowed = false
    var utf8Decoder = VTUTF8Decoder()
    private(set) var pendingResponseBytes: [UInt8] = []
    private(set) var pendingTitle: String?

    var activeBufferIndex: Int {
        modes.usesAlternateScreen ? 1 : 0
    }

    var currentStyle: TerminalStyle {
        get { buffers[activeBufferIndex].currentStyle }
        set { buffers[activeBufferIndex].currentStyle = newValue }
    }

    var activeHyperlink: String? {
        get { buffers[activeBufferIndex].activeHyperlink }
        set { buffers[activeBufferIndex].activeHyperlink = newValue }
    }

    var g0CharacterSet: VTCharacterSet {
        get { buffers[activeBufferIndex].g0CharacterSet }
        set { buffers[activeBufferIndex].g0CharacterSet = newValue }
    }

    var g1CharacterSet: VTCharacterSet {
        get { buffers[activeBufferIndex].g1CharacterSet }
        set { buffers[activeBufferIndex].g1CharacterSet = newValue }
    }

    var usesG1CharacterSet: Bool {
        get { buffers[activeBufferIndex].usesG1CharacterSet }
        set { buffers[activeBufferIndex].usesG1CharacterSet = newValue }
    }

    init(
        columns: Int = 40,
        rows: Int = 18,
        scrollbackLimit: Int = 2_000
    ) {
        precondition(columns > 0 && rows > 0 && scrollbackLimit >= 0)
        self.columns = columns
        self.rows = rows
        self.scrollbackLimit = scrollbackLimit

        var identifier: UInt64 = 0
        func makeInitialLines() -> [Line] {
            (0..<rows).map { _ in
                defer { identifier &+= 1 }
                return Line(
                    id: identifier,
                    cells: Array(
                        repeating: Cell.blank(),
                        count: columns))
            }
        }
        buffers = [
            Buffer(lines: makeInitialLines(), scrollBottom: rows - 1),
            Buffer(lines: makeInitialLines(), scrollBottom: rows - 1),
        ]
        scrollback.reserveCapacity(scrollbackLimit)
        nextLineID = identifier
    }

    mutating func resize(columns newColumns: Int, rows newRows: Int) {
        precondition(newColumns > 0 && newRows > 0)
        guard newColumns != columns || newRows != rows else { return }

        for index in buffers.indices {
            resizeColumns(
                in: index,
                from: columns,
                to: newColumns)
        }
        columns = newColumns

        for index in buffers.indices {
            resizeRows(
                in: index,
                from: rows,
                to: newRows,
                preservesHistory: index == 0)
        }
        rows = newRows

        for index in buffers.indices {
            let logicalColumn =
                buffers[index].resizeSavedCursorColumn ??
                buffers[index].cursorColumn
            let logicalPendingWrap =
                buffers[index].resizeSavedCursorColumn == nil ?
                buffers[index].hasPendingWrap :
                buffers[index].resizeSavedPendingWrap
            buffers[index].cursorRow = min(
                buffers[index].cursorRow, newRows - 1)
            if logicalColumn >= newColumns {
                buffers[index].cursorColumn = newColumns - 1
                buffers[index].resizeSavedCursorColumn = logicalColumn
                buffers[index].resizeSavedPendingWrap =
                    logicalPendingWrap
            } else {
                buffers[index].cursorColumn = logicalColumn
                buffers[index].resizeSavedCursorColumn = nil
                buffers[index].resizeSavedPendingWrap = false
            }
            buffers[index].savedCursorState.row = min(
                buffers[index].savedCursorState.row, newRows - 1)
            buffers[index].scrollTop = 0
            buffers[index].scrollBottom = newRows - 1
            buffers[index].hasPendingWrap = logicalPendingWrap
        }
    }

    mutating func carriageReturn() {
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        buffers[index].cursorColumn = 0
        resetPendingWrap(in: index)
    }

    mutating func lineFeed() {
        let index = activeBufferIndex
        resetPendingWrap(in: index)
        indexDown()
    }

    mutating func backspace() {
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        buffers[index].cursorColumn = max(
            0, buffers[index].cursorColumn - 1)
        resetPendingWrap(in: index)
    }

    mutating func horizontalTab() {
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        let nextStop = (buffers[index].cursorColumn / 8 + 1) * 8
        buffers[index].cursorColumn = min(columns - 1, nextStop)
        resetPendingWrap(in: index)
    }

    mutating func horizontalBackTab() {
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        let column = buffers[index].cursorColumn
        buffers[index].cursorColumn = column == 0 ?
            0 : ((column - 1) / 8) * 8
        resetPendingWrap(in: index)
    }

    mutating func setApplicationCursorKeys(_ enabled: Bool) {
        modes.usesApplicationCursorKeys = enabled
    }

    mutating func setCursorVisibility(_ visible: Bool) {
        modes.isCursorVisible = visible
    }

    mutating func setOriginMode(_ enabled: Bool) {
        modes.usesOriginMode = enabled
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        buffers[index].cursorRow =
            enabled ? buffers[index].scrollTop : 0
        buffers[index].cursorColumn = 0
        resetPendingWrap(in: index)
    }

    mutating func setAutoWrap(_ enabled: Bool) {
        modes.usesAutoWrap = enabled
        if !enabled {
            resetPendingWrap(in: activeBufferIndex)
        }
    }

    mutating func setInsertMode(_ enabled: Bool) {
        modes.usesInsertMode = enabled
    }

    mutating func setCursorStyle(_ parameter: Int) {
        switch parameter {
        case 0:
            cursorShapeOverride = nil
            cursorBlinkOverride = nil
        case 1:
            cursorShapeOverride = .block
            cursorBlinkOverride = true
        case 2:
            cursorShapeOverride = .block
            cursorBlinkOverride = false
        case 3:
            cursorShapeOverride = .underline
            cursorBlinkOverride = true
        case 4:
            cursorShapeOverride = .underline
            cursorBlinkOverride = false
        case 5:
            cursorShapeOverride = .beam
            cursorBlinkOverride = true
        case 6:
            cursorShapeOverride = .beam
            cursorBlinkOverride = false
        default:
            break
        }
    }

    mutating func setBracketedPaste(_ enabled: Bool) {
        modes.usesBracketedPaste = enabled
    }

    mutating func setFocusReporting(_ enabled: Bool) {
        modes.reportsFocusEvents = enabled
    }

    mutating func setMouseTracking(
        _ mode: TerminalMouseTrackingMode,
        enabled: Bool
    ) {
        if enabled {
            modes.mouseTrackingMode = mode
        } else if modes.mouseTrackingMode == mode {
            modes.mouseTrackingMode = .disabled
        }
    }

    mutating func setSGRMouseEncoding(_ enabled: Bool) {
        modes.usesSGRMouseEncoding = enabled
    }

    mutating func write(_ scalar: Unicode.Scalar) {
        let scalarWidth = terminalColumnWidth(scalar)
        guard scalarWidth != 0 else {
            appendZeroWidthScalar(scalar)
            return
        }

        let width = min(scalarWidth, columns)
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        if modes.usesAutoWrap &&
                (buffers[index].hasPendingWrap ||
                 (width == 2 &&
                  buffers[index].cursorColumn == columns - 1)) {
            autoWrap()
        }
        guard width == 1 ||
                buffers[index].cursorColumn < columns - 1 else {
            return
        }

        let row = buffers[index].cursorRow
        let column = buffers[index].cursorColumn
        if modes.usesInsertMode {
            insertCharacters(width)
        }
        clearGlyph(in: index, row: row, column: column)
        if width == 2 {
            clearGlyph(in: index, row: row, column: column + 1)
        }

        buffers[index].lines[row].cells[column] = Cell(
            text: String(scalar),
            width: width,
            naturalWidth: scalarWidth,
            style: currentStyle,
            hyperlink: activeHyperlink)
        if width == 2 {
            buffers[index].lines[row].cells[column + 1] =
                Cell.continuation(
                    style: currentStyle,
                    hyperlink: activeHyperlink)
        }

        if column + width >= columns {
            buffers[index].cursorColumn = columns - 1
            buffers[index].hasPendingWrap = modes.usesAutoWrap
        } else {
            buffers[index].cursorColumn += width
        }
    }

    mutating func moveCursor(
        rowDelta: Int = 0,
        columnDelta: Int = 0,
        resetsColumn: Bool = false
    ) {
        let index = activeBufferIndex
        if resetsColumn || columnDelta != 0 {
            discardResizeSavedCursor(in: index)
        }
        let top = modes.usesOriginMode ?
            buffers[index].scrollTop : 0
        let bottom = modes.usesOriginMode ?
            buffers[index].scrollBottom : rows - 1
        buffers[index].cursorRow = min(
            bottom,
            max(top, buffers[index].cursorRow + rowDelta))
        if resetsColumn {
            buffers[index].cursorColumn = 0
        } else {
            buffers[index].cursorColumn = min(
                columns - 1,
                max(0, buffers[index].cursorColumn + columnDelta))
        }
        resetPendingWrap(in: index)
    }

    mutating func setCursor(row: Int? = nil, column: Int? = nil) {
        let index = activeBufferIndex
        if let row {
            if modes.usesOriginMode {
                buffers[index].cursorRow = min(
                    buffers[index].scrollBottom,
                    max(
                        buffers[index].scrollTop,
                        buffers[index].scrollTop + row))
            } else {
                buffers[index].cursorRow = min(rows - 1, max(0, row))
            }
        }
        if let column {
            discardResizeSavedCursor(in: index)
            buffers[index].cursorColumn = min(
                columns - 1, max(0, column))
        }
        resetPendingWrap(in: index)
    }

    mutating func saveCursor() {
        let index = activeBufferIndex
        buffers[index].savedCursorState = captureCursorState(in: index)
    }

    mutating func restoreCursor() {
        let index = activeBufferIndex
        restoreCursorState(
            buffers[index].savedCursorState,
            in: index)
    }

    func captureCursorState(in bufferIndex: Int) -> SavedCursorState {
        SavedCursorState(
            row: buffers[bufferIndex].cursorRow,
            column: buffers[bufferIndex].resizeSavedCursorColumn ??
                buffers[bufferIndex].cursorColumn,
            style: buffers[bufferIndex].currentStyle,
            hyperlink: buffers[bufferIndex].activeHyperlink,
            g0CharacterSet: buffers[bufferIndex].g0CharacterSet,
            g1CharacterSet: buffers[bufferIndex].g1CharacterSet,
            usesG1CharacterSet:
                buffers[bufferIndex].usesG1CharacterSet,
            usesOriginMode: modes.usesOriginMode,
            usesAutoWrap: modes.usesAutoWrap,
            hasPendingWrap:
                buffers[bufferIndex].resizeSavedCursorColumn == nil ?
                buffers[bufferIndex].hasPendingWrap :
                buffers[bufferIndex].resizeSavedPendingWrap)
    }

    mutating func restoreCursorState(
        _ state: SavedCursorState,
        in bufferIndex: Int
    ) {
        modes.usesOriginMode = state.usesOriginMode
        modes.usesAutoWrap = state.usesAutoWrap
        let top = state.usesOriginMode ?
            buffers[bufferIndex].scrollTop : 0
        let bottom = state.usesOriginMode ?
            buffers[bufferIndex].scrollBottom : rows - 1
        buffers[bufferIndex].cursorRow = min(
            bottom, max(top, state.row))
        let logicalColumn = max(0, state.column)
        buffers[bufferIndex].cursorColumn = min(
            columns - 1, logicalColumn)
        if logicalColumn >= columns {
            buffers[bufferIndex].resizeSavedCursorColumn =
                logicalColumn
            buffers[bufferIndex].resizeSavedPendingWrap =
                state.hasPendingWrap && state.usesAutoWrap
        } else {
            discardResizeSavedCursor(in: bufferIndex)
        }
        buffers[bufferIndex].currentStyle = state.style
        buffers[bufferIndex].activeHyperlink = state.hyperlink
        buffers[bufferIndex].g0CharacterSet = state.g0CharacterSet
        buffers[bufferIndex].g1CharacterSet = state.g1CharacterSet
        buffers[bufferIndex].usesG1CharacterSet =
            state.usesG1CharacterSet
        buffers[bufferIndex].hasPendingWrap =
            state.hasPendingWrap && state.usesAutoWrap
    }

    mutating func discardResizeSavedCursor(in bufferIndex: Int) {
        buffers[bufferIndex].resizeSavedCursorColumn = nil
        buffers[bufferIndex].resizeSavedPendingWrap = false
    }

    mutating func resetPendingWrap(in bufferIndex: Int) {
        buffers[bufferIndex].hasPendingWrap = false
        buffers[bufferIndex].resizeSavedPendingWrap = false
    }

    mutating func eraseDisplay(mode: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let column = buffers[index].cursorColumn
        switch mode {
        case 0:
            eraseCells(
                in: index, row: row,
                range: column..<columns)
            if row + 1 < rows {
                for followingRow in (row + 1)..<rows {
                    eraseCells(
                        in: index, row: followingRow,
                        range: 0..<columns)
                }
            }
            resetPendingWrap(in: index)
        case 1:
            if row > 0 {
                for previousRow in 0..<row {
                    eraseCells(
                        in: index, row: previousRow,
                        range: 0..<columns)
                }
            }
            eraseCells(
                in: index, row: row,
                range: 0..<min(columns, column + 1))
            resetPendingWrap(in: index)
        case 2:
            for screenRow in 0..<rows {
                eraseCells(
                    in: index, row: screenRow,
                    range: 0..<columns)
            }
            resetPendingWrap(in: index)
        case 3:
            if index == 0 {
                clearScrollback()
            }
        default:
            break
        }
    }

    mutating func eraseLine(mode: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let column = buffers[index].cursorColumn
        switch mode {
        case 0:
            eraseCells(
                in: index, row: row,
                range: column..<columns)
            resetPendingWrap(in: index)
        case 1:
            eraseCells(
                in: index, row: row,
                range: 0..<min(columns, column + 1))
            resetPendingWrap(in: index)
        case 2:
            eraseCells(
                in: index, row: row,
                range: 0..<columns)
            resetPendingWrap(in: index)
        default:
            break
        }
    }

    mutating func insertCharacters(_ requestedCount: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let column = buffers[index].cursorColumn
        let count = min(max(1, requestedCount), columns - column)
        guard count > 0 else { return }
        let blanks = Array(
            repeating: Cell.blank(style: currentStyle),
            count: count)
        buffers[index].lines[row].cells.insert(
            contentsOf: blanks, at: column)
        buffers[index].lines[row].cells.removeLast(count)
        normalizeLine(in: index, row: row)
        resetPendingWrap(in: index)
    }

    mutating func deleteCharacters(_ requestedCount: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let column = buffers[index].cursorColumn
        let count = min(max(1, requestedCount), columns - column)
        guard count > 0 else { return }
        buffers[index].lines[row].cells.removeSubrange(
            column..<(column + count))
        buffers[index].lines[row].cells.append(contentsOf: Array(
            repeating: Cell.blank(style: currentStyle),
            count: count))
        normalizeLine(in: index, row: row)
        resetPendingWrap(in: index)
    }

    mutating func eraseCharacters(_ requestedCount: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let column = buffers[index].cursorColumn
        let count = min(max(1, requestedCount), columns - column)
        eraseCells(
            in: index, row: row,
            range: column..<(column + count))
        resetPendingWrap(in: index)
    }

    mutating func insertLines(_ requestedCount: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let bottom = buffers[index].scrollBottom
        guard row >= buffers[index].scrollTop, row <= bottom else {
            return
        }
        let count = min(max(1, requestedCount), bottom - row + 1)
        for _ in 0..<count {
            let blank = makeBlankLine(style: currentStyle)
            buffers[index].lines.insert(blank, at: row)
            buffers[index].lines.remove(at: bottom + 1)
        }
        resetPendingWrap(in: index)
    }

    mutating func deleteLines(_ requestedCount: Int) {
        let index = activeBufferIndex
        let row = buffers[index].cursorRow
        let bottom = buffers[index].scrollBottom
        guard row >= buffers[index].scrollTop, row <= bottom else {
            return
        }
        let count = min(max(1, requestedCount), bottom - row + 1)
        for _ in 0..<count {
            buffers[index].lines.remove(at: row)
            let blank = makeBlankLine(style: currentStyle)
            buffers[index].lines.insert(blank, at: bottom)
        }
        resetPendingWrap(in: index)
    }

    mutating func scrollUp(_ requestedCount: Int) {
        let index = activeBufferIndex
        scrollUp(
            in: index,
            top: buffers[index].scrollTop,
            bottom: buffers[index].scrollBottom,
            count: requestedCount)
    }

    mutating func scrollDown(_ requestedCount: Int) {
        let index = activeBufferIndex
        scrollDown(
            in: index,
            top: buffers[index].scrollTop,
            bottom: buffers[index].scrollBottom,
            count: requestedCount)
    }

    mutating func setScrollRegion(top: Int?, bottom: Int?) {
        let requestedTop = min(rows - 1, max(0, top ?? 0))
        let requestedBottom = min(
            rows - 1, max(0, bottom ?? rows - 1))
        guard requestedTop < requestedBottom || rows == 1 else {
            return
        }
        let index = activeBufferIndex
        discardResizeSavedCursor(in: index)
        buffers[index].scrollTop = requestedTop
        buffers[index].scrollBottom = requestedBottom
        buffers[index].cursorRow =
            modes.usesOriginMode ? requestedTop : 0
        buffers[index].cursorColumn = 0
        resetPendingWrap(in: index)
    }

    mutating func reverseIndex() {
        let index = activeBufferIndex
        if buffers[index].cursorRow == buffers[index].scrollTop {
            scrollDown(
                in: index,
                top: buffers[index].scrollTop,
                bottom: buffers[index].scrollBottom,
                count: 1)
        } else {
            buffers[index].cursorRow = max(
                0, buffers[index].cursorRow - 1)
        }
        resetPendingWrap(in: index)
    }

    mutating func setAlternateScreen(_ enabled: Bool) {
        guard modes.usesAlternateScreen != enabled else { return }
        let sourceIndex = activeBufferIndex
        let targetIndex = enabled ? 1 : 0
        buffers[targetIndex].cursorRow =
            buffers[sourceIndex].cursorRow
        buffers[targetIndex].cursorColumn =
            buffers[sourceIndex].cursorColumn
        buffers[targetIndex].scrollTop =
            buffers[sourceIndex].scrollTop
        buffers[targetIndex].scrollBottom =
            buffers[sourceIndex].scrollBottom
        buffers[targetIndex].hasPendingWrap =
            buffers[sourceIndex].hasPendingWrap
        buffers[targetIndex].resizeSavedCursorColumn =
            buffers[sourceIndex].resizeSavedCursorColumn
        buffers[targetIndex].resizeSavedPendingWrap =
            buffers[sourceIndex].resizeSavedPendingWrap
        buffers[targetIndex].currentStyle =
            buffers[sourceIndex].currentStyle
        buffers[targetIndex].activeHyperlink =
            buffers[sourceIndex].activeHyperlink
        buffers[targetIndex].g0CharacterSet =
            buffers[sourceIndex].g0CharacterSet
        buffers[targetIndex].g1CharacterSet =
            buffers[sourceIndex].g1CharacterSet
        buffers[targetIndex].usesG1CharacterSet =
            buffers[sourceIndex].usesG1CharacterSet
        modes.usesAlternateScreen = enabled
    }

    mutating func setAlternateScreenClearingOnExit(_ enabled: Bool) {
        if enabled {
            setAlternateScreen(true)
            return
        }
        clearAlternateScreen()
        setAlternateScreen(false)
    }

    mutating func clearAlternateScreen() {
        guard modes.usesAlternateScreen else { return }
        for row in 0..<rows {
            eraseCells(
                in: 1,
                row: row,
                range: 0..<columns)
        }
        resetPendingWrap(in: 1)
    }

    mutating func saveAndEnterAlternateScreen() {
        saveCursor()
        setAlternateScreen(true)
        clearAlternateScreen()
    }

    mutating func leaveAlternateScreenAndRestore() {
        setAlternateScreen(false)
        restoreCursor()
    }

    mutating func resetTerminal() {
        modes = TerminalModes()
        currentStyle = TerminalStyle()
        cursorShapeOverride = nil
        cursorBlinkOverride = nil
        activeHyperlink = nil
        scrollback.removeAll(keepingCapacity: true)
        scrollbackHeadIndex = 0
        buffers[0] = makeBlankBuffer()
        buffers[1] = makeBlankBuffer()
        g0CharacterSet = .ascii
        g1CharacterSet = .ascii
        usesG1CharacterSet = false
    }

    mutating func clearScrollback() {
        scrollback.removeAll(keepingCapacity: true)
        scrollbackHeadIndex = 0
    }

    mutating func queueResponse(_ bytes: [UInt8]) {
        guard !bytes.isEmpty,
              pendingResponseBytes.count + bytes.count <=
                Self.maximumPendingResponseBytes else {
            return
        }
        pendingResponseBytes.append(contentsOf: bytes)
    }

    mutating func takePendingResponseBytes(
        maximumCount: Int
    ) -> [UInt8] {
        let count = min(max(0, maximumCount), pendingResponseBytes.count)
        guard count > 0 else { return [] }
        let bytes = Array(pendingResponseBytes.prefix(count))
        pendingResponseBytes.removeFirst(count)
        return bytes
    }

    mutating func discardPendingResponses() {
        pendingResponseBytes.removeAll(keepingCapacity: false)
    }

    mutating func takePendingTitle() -> String? {
        defer { pendingTitle = nil }
        return pendingTitle
    }

    mutating func publishTitle(_ title: String) {
        pendingTitle = title
    }

    mutating func autoWrap() {
        let index = activeBufferIndex
        buffers[index].lines[buffers[index].cursorRow].isWrapped = true
        discardResizeSavedCursor(in: index)
        buffers[index].cursorColumn = 0
        resetPendingWrap(in: index)
        indexDown()
    }

    mutating func indexDown() {
        let index = activeBufferIndex
        if buffers[index].cursorRow == buffers[index].scrollBottom {
            scrollUp(
                in: index,
                top: buffers[index].scrollTop,
                bottom: buffers[index].scrollBottom,
                count: 1)
        } else {
            buffers[index].cursorRow = min(
                rows - 1, buffers[index].cursorRow + 1)
        }
    }

    mutating func scrollUp(
        in bufferIndex: Int,
        top: Int,
        bottom: Int,
        count requestedCount: Int
    ) {
        let count = min(max(1, requestedCount), bottom - top + 1)
        for _ in 0..<count {
            let removed = buffers[bufferIndex].lines.remove(at: top)
            if bufferIndex == 0 && top == 0 && bottom == rows - 1 {
                appendToScrollback(removed)
            }
            let blank = makeBlankLine(style: currentStyle)
            buffers[bufferIndex].lines.insert(blank, at: bottom)
        }
    }

    mutating func scrollDown(
        in bufferIndex: Int,
        top: Int,
        bottom: Int,
        count requestedCount: Int
    ) {
        let count = min(max(1, requestedCount), bottom - top + 1)
        for _ in 0..<count {
            buffers[bufferIndex].lines.remove(at: bottom)
            let blank = makeBlankLine(style: currentStyle)
            buffers[bufferIndex].lines.insert(blank, at: top)
        }
    }

    mutating func appendZeroWidthScalar(_ scalar: Unicode.Scalar) {
        let index = activeBufferIndex
        var column = buffers[index].cursorColumn - 1
        if buffers[index].hasPendingWrap {
            column = columns - 1
        }
        guard column >= 0 else { return }
        while column > 0 &&
                buffers[index].lines[buffers[index].cursorRow]
                    .cells[column].width == 0 {
            column -= 1
        }
        guard buffers[index].lines[buffers[index].cursorRow]
                .cells[column].text != "" else {
            return
        }
        guard buffers[index].lines[buffers[index].cursorRow]
                .cells[column].text.unicodeScalars.count <
                Self.maximumCellTextScalars else {
            return
        }
        buffers[index].lines[buffers[index].cursorRow]
            .cells[column].text.append(contentsOf: String(scalar))
    }

    mutating func eraseCells(
        in bufferIndex: Int,
        row: Int,
        range: Range<Int>
    ) {
        guard !range.isEmpty else { return }
        var affectedColumns: Set<Int> = []
        for column in range {
            var glyphStart = column
            while glyphStart > 0 &&
                    buffers[bufferIndex].lines[row]
                        .cells[glyphStart].width == 0 {
                glyphStart -= 1
            }
            let glyphWidth = max(
                1,
                buffers[bufferIndex].lines[row]
                    .cells[glyphStart].width)
            affectedColumns.formUnion(
                glyphStart..<min(columns, glyphStart + glyphWidth))
        }
        for column in affectedColumns {
            buffers[bufferIndex].lines[row].cells[column] =
                Cell.blank(style: currentStyle)
        }
        normalizeLine(in: bufferIndex, row: row)
        buffers[bufferIndex].lines[row].isWrapped = false
    }

    mutating func clearGlyph(
        in bufferIndex: Int,
        row: Int,
        column: Int
    ) {
        guard column >= 0, column < columns else { return }
        var start = column
        if buffers[bufferIndex].lines[row].cells[start].width == 0 {
            while start > 0 &&
                    buffers[bufferIndex].lines[row].cells[start].width == 0 {
                start -= 1
            }
        }
        let width = max(
            1, buffers[bufferIndex].lines[row].cells[start].width)
        for target in start..<min(columns, start + width) {
            buffers[bufferIndex].lines[row].cells[target] = .blank()
        }
    }

    mutating func normalizeLine(in bufferIndex: Int, row: Int) {
        normalizeLine(
            &buffers[bufferIndex].lines[row],
            columns: columns)
    }

    mutating func appendToScrollback(_ line: Line) {
        guard scrollbackLimit > 0 else { return }
        if scrollback.count < scrollbackLimit {
            scrollback.append(line)
            return
        }
        scrollback[scrollbackHeadIndex] = line
        scrollbackHeadIndex =
            (scrollbackHeadIndex + 1) % scrollback.count
    }

    func scrollbackPhysicalIndex(for logicalIndex: Int) -> Int {
        precondition(scrollback.indices.contains(logicalIndex))
        return (scrollbackHeadIndex + logicalIndex) % scrollback.count
    }

    mutating func normalizeScrollbackStorage() {
        guard scrollbackHeadIndex != 0 else { return }
        let ordered =
            Array(scrollback[scrollbackHeadIndex...]) +
            Array(scrollback[..<scrollbackHeadIndex])
        scrollback = ordered
        scrollbackHeadIndex = 0
    }

    mutating func makeBlankLine(
        style: TerminalStyle = TerminalStyle()
    ) -> Line {
        defer { nextLineID &+= 1 }
        return Line(
            id: nextLineID,
            cells: Array(
                repeating: Cell.blank(style: style),
                count: columns))
    }

    mutating func makeBlankBuffer() -> Buffer {
        var lines: [Line] = []
        lines.reserveCapacity(rows)
        for _ in 0..<rows {
            lines.append(makeBlankLine())
        }
        return Buffer(lines: lines, scrollBottom: rows - 1)
    }

    mutating func resizeColumns(
        in bufferIndex: Int,
        from oldColumns: Int,
        to newColumns: Int
    ) {
        for row in buffers[bufferIndex].lines.indices {
            if newColumns > oldColumns {
                buffers[bufferIndex].lines[row].cells.append(
                    contentsOf: Array(
                        repeating: Cell.blank(),
                        count: newColumns - oldColumns))
            } else {
                buffers[bufferIndex].lines[row].cells.removeLast(
                    oldColumns - newColumns)
            }
            normalizeLineForColumnCount(
                in: bufferIndex, row: row, columns: newColumns)
        }
        guard bufferIndex == 0 else { return }
        for row in scrollback.indices {
            let storedColumns = scrollback[row].cells.count
            if newColumns > storedColumns {
                scrollback[row].cells.append(contentsOf: Array(
                    repeating: Cell.blank(),
                    count: newColumns - storedColumns))
            }
            // 已进入 scrollback 的行不再参与活动屏 VT 操作。
            // 缩列时保留旧宽度，使用户仍可横向查看先前输出。
            normalizeLine(
                &scrollback[row],
                columns: scrollback[row].cells.count)
        }
    }

    mutating func resizeRows(
        in bufferIndex: Int,
        from oldRows: Int,
        to newRows: Int,
        preservesHistory: Bool
    ) {
        if newRows < oldRows {
            let removalCount = oldRows - newRows
            let removeFromTop = min(
                removalCount, buffers[bufferIndex].cursorRow)
            for _ in 0..<removeFromTop {
                let removed = buffers[bufferIndex].lines.removeFirst()
                if preservesHistory {
                    appendToScrollback(removed)
                }
            }
            let removeFromBottom = removalCount - removeFromTop
            if removeFromBottom > 0 {
                buffers[bufferIndex].lines.removeLast(removeFromBottom)
            }
            buffers[bufferIndex].cursorRow -= removeFromTop
            buffers[bufferIndex].savedCursorState.row = max(
                0,
                buffers[bufferIndex].savedCursorState.row -
                    removeFromTop)
        } else if newRows > oldRows {
            var additionCount = newRows - oldRows
            if preservesHistory && !scrollback.isEmpty {
                normalizeScrollbackStorage()
                var restored: [Line] = []
                while additionCount > 0,
                      let newest = scrollback.last,
                      newest.cells.count == columns {
                    restored.insert(scrollback.removeLast(), at: 0)
                    additionCount -= 1
                }
                let restoredCount = restored.count
                buffers[bufferIndex].lines.insert(
                    contentsOf: restored, at: 0)
                buffers[bufferIndex].cursorRow += restoredCount
                buffers[bufferIndex].savedCursorState.row +=
                    restoredCount
            }
            for _ in 0..<additionCount {
                buffers[bufferIndex].lines.append(makeBlankLine(
                    style: buffers[bufferIndex].currentStyle))
            }
        }
    }

    mutating func normalizeLineForColumnCount(
        in bufferIndex: Int,
        row: Int,
        columns columnCount: Int
    ) {
        normalizeLine(
            &buffers[bufferIndex].lines[row],
            columns: columnCount)
    }

    func normalizeLine(_ line: inout Line, columns columnCount: Int) {
        var column = 0
        while column < columnCount {
            let width = line.cells[column].width
            if width == 1 &&
                    line.cells[column].naturalWidth == 2 {
                guard column + 1 < columnCount else {
                    column += 1
                    continue
                }
                line.cells[column].width = 2
                line.cells[column + 1] = .continuation(
                    style: line.cells[column].style,
                    hyperlink: line.cells[column].hyperlink)
                column += 2
            } else if width == 2 {
                guard column + 1 < columnCount else {
                    // 极窄窗口仍保留字符，避免调整尺寸时静默丢失内容。
                    line.cells[column].width = 1
                    column += 1
                    continue
                }
                line.cells[column + 1] = .continuation(
                    style: line.cells[column].style,
                    hyperlink: line.cells[column].hyperlink)
                column += 2
            } else if width == 0 {
                line.cells[column] = .blank()
                column += 1
            } else {
                column += 1
            }
        }
    }

    func render(_ line: Line) -> TerminalRenderLine {
        var runs: [TerminalRenderRun] = []
        var column = 0
        while column < line.cells.count {
            let first = line.cells[column]
            if first.width == 0 {
                column += 1
                continue
            }

            let start = column
            let style = first.style
            let hyperlink = first.hyperlink
            var text = ""
            var columnCount = 0
            while column < line.cells.count {
                let cell = line.cells[column]
                if cell.width == 0 {
                    column += 1
                    continue
                }
                if cell.style != style ||
                        cell.hyperlink != hyperlink {
                    break
                }
                text.append(contentsOf: cell.text.isEmpty ? " " : cell.text)
                let width = max(1, cell.width)
                columnCount += width
                column += width
            }
            runs.append(TerminalRenderRun(
                startColumn: start,
                columnCount: columnCount,
                text: text,
                style: style,
                hyperlink: hyperlink))
        }
        return TerminalRenderLine(
            id: line.id,
            runs: runs,
            columnCount: line.cells.count,
            isWrapped: line.isWrapped)
    }
}

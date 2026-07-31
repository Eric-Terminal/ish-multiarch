extension TerminalScreen {
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

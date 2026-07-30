import Foundation
import SwiftUI

struct WatchTerminalView: View {
    @Environment(\.accessibilityReduceMotion)
    private var reducesMotion
    @Environment(\.scenePhase)
    private var scenePhase

    @State private var scrollPosition: UInt64?
    @State private var programmaticScrollTarget: UInt64?
    @State private var hasEstablishedInitialPosition = false

    private let lines: [TerminalRenderLine]
    private let renderColumnCount: Int
    private let cursor: TerminalCursor
    private let cursorLineID: UInt64?
    private let appearance: WatchTerminalAppearance
    private let fontSize: CGFloat
    private let showsCursor: Bool
    private let followsOutput: Bool
    private let containsBlinkingText: Bool
    private let onManualScroll: () -> Void
    private let onViewportChange: (Int, Int) -> Void

    init(
        screen: TerminalScreen,
        appearance: WatchTerminalAppearance,
        fontSize: Double,
        showsCursor: Bool = true,
        followsOutput: Bool = true,
        onManualScroll: @escaping () -> Void = {},
        onViewportChange: @escaping (Int, Int) -> Void = { _, _ in }
    ) {
        let visibleLines = screen.visibleLines
        let renderedLines: [TerminalRenderLine]
        if screen.modes.usesAlternateScreen {
            renderedLines = visibleLines
        } else {
            renderedLines = screen.scrollbackLines + visibleLines
        }
        lines = renderedLines
        renderColumnCount = renderedLines.reduce(screen.columns) {
            max($0, $1.columnCount)
        }
        containsBlinkingText = renderedLines.contains { line in
            line.runs.contains {
                $0.style.isBlinking && !$0.style.isHidden
            }
        }
        cursor = screen.cursor
        cursorLineID = visibleLines.indices.contains(screen.cursor.row) ?
            visibleLines[screen.cursor.row].id : nil
        self.appearance = appearance
        self.fontSize = CGFloat(fontSize)
        self.showsCursor = showsCursor
        self.followsOutput = followsOutput
        self.onManualScroll = onManualScroll
        self.onViewportChange = onViewportChange
    }

    init(
        snapshot: WatchTerminalSessionSnapshot,
        appearance: WatchTerminalAppearance,
        fontSize: Double,
        showsCursor: Bool = true,
        followsOutput: Bool = true,
        onManualScroll: @escaping () -> Void = {},
        onViewportChange: @escaping (Int, Int) -> Void = { _, _ in }
    ) {
        lines = snapshot.renderedLines
        renderColumnCount = snapshot.renderedLines.reduce(
            snapshot.columns
        ) {
            max($0, $1.columnCount)
        }
        containsBlinkingText = snapshot.renderedLines.contains { line in
            line.runs.contains {
                $0.style.isBlinking && !$0.style.isHidden
            }
        }
        cursor = snapshot.cursor
        cursorLineID = snapshot.visibleLines.indices.contains(
            snapshot.cursor.row) ?
            snapshot.visibleLines[snapshot.cursor.row].id : nil
        self.appearance = appearance
        self.fontSize = CGFloat(fontSize)
        self.showsCursor = showsCursor
        self.followsOutput = followsOutput
        self.onManualScroll = onManualScroll
        self.onViewportChange = onViewportChange
    }

    var body: some View {
        Group {
            if animatesBlink {
                TimelineView(.periodic(from: .now, by: 0.55)) { context in
                    terminalViewport(
                        blinkVisible: blinkOpacity(at: context.date) != 0)
                }
            } else {
                terminalViewport(blinkVisible: true)
            }
        }
    }

    private var animatesBlink: Bool {
        guard scenePhase == .active, !reducesMotion else { return false }
        let cursorBlinks =
            cursor.blinkOverride ?? appearance.cursorBlinks
        return containsBlinkingText ||
            (showsCursor && cursor.isVisible && cursorBlinks)
    }

    private func terminalViewport(
        blinkVisible: Bool
    ) -> some View {
        GeometryReader { geometry in
            let metrics = WatchTerminalMetrics(fontSize: fontSize)
            let contentWidth = max(
                geometry.size.width,
                CGFloat(renderColumnCount) * metrics.cellWidth)
            let viewportColumns = max(
                1,
                Int(geometry.size.width / metrics.cellWidth))
            let viewportRows = max(
                1,
                Int(geometry.size.height / metrics.lineHeight))

            ScrollView([.horizontal, .vertical]) {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(lines) { line in
                        WatchTerminalLineView(
                            line: line,
                            columnCount: renderColumnCount,
                            cursorColumn:
                                line.id == cursorLineID &&
                                cursor.isVisible &&
                                showsCursor ?
                                cursor.column : nil,
                            cursorShape:
                                cursor.shapeOverride?.watchShape ??
                                appearance.cursorShape,
                            cursorBlinks:
                                cursor.blinkOverride ??
                                appearance.cursorBlinks,
                            blinkVisible: blinkVisible,
                            appearance: appearance,
                            metrics: metrics)
                            .id(line.id)
                    }
                }
                .frame(width: contentWidth, alignment: .leading)
                .scrollTargetLayout()
            }
            // 宽历史行存在时保持左边界，避免跟随末行把当前提示符横向居中。
            .scrollPosition(
                id: $scrollPosition,
                anchor: .bottomLeading)
            .background(terminalColor(appearance.palette.background))
            .accessibilityIdentifier("terminal-transcript")
            .onAppear {
                onViewportChange(viewportColumns, viewportRows)
                scrollToLatestLine()
            }
            .onChange(of: geometry.size) { _, _ in
                onViewportChange(viewportColumns, viewportRows)
                scrollToLatestLine()
            }
            .onChange(of: fontSize) { _, _ in
                onViewportChange(viewportColumns, viewportRows)
                scrollToLatestLine()
            }
            .onChange(of: lines.last?.id) { _, _ in
                scrollToLatestLine()
            }
            .onChange(of: followsOutput) { _, isFollowing in
                if isFollowing {
                    scrollToLatestLine()
                }
            }
            .onChange(of: scrollPosition) { _, position in
                guard let position else {
                    return
                }
                if let target = programmaticScrollTarget {
                    if position == target {
                        programmaticScrollTarget = nil
                        hasEstablishedInitialPosition = true
                    }
                    return
                }
                guard hasEstablishedInitialPosition,
                      followsOutput,
                      position != lines.last?.id else {
                    return
                }
                onManualScroll()
            }
        }
    }

    private func blinkOpacity(at date: Date) -> Double {
        let phase = Int(
            date.timeIntervalSinceReferenceDate / 0.55)
        return phase.isMultiple(of: 2) ? 1 : 0
    }

    private func scrollToLatestLine() {
        guard followsOutput, let latestLineID = lines.last?.id else {
            return
        }
        if scrollPosition == latestLineID {
            programmaticScrollTarget = nil
            hasEstablishedInitialPosition = true
            return
        }
        programmaticScrollTarget = latestLineID
        scrollPosition = latestLineID
    }
}

private struct WatchTerminalLineView: View {
    let line: TerminalRenderLine
    let columnCount: Int
    let cursorColumn: Int?
    let cursorShape: WatchTerminalCursorShape
    let cursorBlinks: Bool
    let blinkVisible: Bool
    let appearance: WatchTerminalAppearance
    let metrics: WatchTerminalMetrics

    @ViewBuilder
    var body: some View {
        if line.runs.contains(where: { $0.hyperlink != nil }) {
            lineContent
                .accessibilityElement(children: .contain)
                .accessibilityIdentifier("terminal-line-\(line.id)")
        } else {
            lineContent
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(
                    verbatim: line.accessibilityText))
                .accessibilityIdentifier("terminal-line-\(line.id)")
        }
    }

    private var lineContent: some View {
        ZStack(alignment: .topLeading) {
            terminalColor(appearance.palette.background)

            ForEach(line.runs.indices, id: \.self) { index in
                let run = line.runs[index]
                WatchTerminalRunView(
                    run: run,
                    blinkVisible: blinkVisible,
                    appearance: appearance,
                    metrics: metrics)
                    .offset(
                        x: CGFloat(run.startColumn) * metrics.cellWidth)
            }

            if let cursorColumn {
                WatchTerminalCursorView(
                    line: line,
                    column: cursorColumn,
                    shape: cursorShape,
                    blinks: cursorBlinks,
                    blinkVisible: blinkVisible,
                    appearance: appearance,
                    metrics: metrics)
                    .offset(
                        x: CGFloat(cursorColumn) * metrics.cellWidth)
            }
        }
        .frame(
            width: CGFloat(columnCount) * metrics.cellWidth,
            height: metrics.lineHeight,
            alignment: .topLeading)
        .clipped()
    }
}

private struct WatchTerminalRunView: View {
    @Environment(\.openURL) private var openURL

    let run: TerminalRenderRun
    let blinkVisible: Bool
    let appearance: WatchTerminalAppearance
    let metrics: WatchTerminalMetrics

    private var resolvedStyle: WatchTerminalResolvedStyle {
        appearance.palette.resolve(run.style)
    }

    @ViewBuilder
    var body: some View {
        if let hyperlink {
            Button {
                openURL(hyperlink)
            } label: {
                runContent
            }
            .buttonStyle(.plain)
            .contentShape(Rectangle())
            .accessibilityLabel(Text(verbatim: run.text))
            .accessibilityHint("打开链接")
            .accessibilityHidden(run.style.isHidden)
            .allowsHitTesting(!run.style.isHidden)
        } else {
            runContent
                .accessibilityHidden(run.style.isHidden)
        }
    }

    private var runContent: some View {
        ZStack(alignment: .leading) {
            terminalColor(resolvedStyle.background)

            WatchTerminalGlyphView(
                run: run,
                blinkVisible: blinkVisible,
                font: metrics.font(
                    choice: appearance.font,
                    weight: resolvedStyle.weight),
                foreground: terminalColor(resolvedStyle.foreground),
                isUnderlined:
                    resolvedStyle.decoration == .underline ||
                    hyperlink != nil)
        }
        .frame(
            width: CGFloat(run.columnCount) * metrics.cellWidth,
            height: metrics.lineHeight,
            alignment: .leading)
        .clipped()
    }

    private var hyperlink: URL? {
        run.hyperlink.flatMap(URL.init(string:))
    }
}

private struct WatchTerminalGlyphView: View {
    let run: TerminalRenderRun
    let blinkVisible: Bool
    let font: Font
    let foreground: Color
    let isUnderlined: Bool

    var body: some View {
        glyph
            .opacity(
                run.style.isHidden ||
                (run.style.isBlinking && !blinkVisible) ?
                0 : 1)
    }

    private var glyph: some View {
        Text(verbatim: run.text)
            .font(font)
            .foregroundStyle(foreground)
            .italic(run.style.isItalic)
            .underline(isUnderlined, color: foreground)
            .strikethrough(
                run.style.isStruckThrough,
                color: foreground)
            .fixedSize(horizontal: true, vertical: false)
    }
}

private struct WatchTerminalCursorView: View {
    let line: TerminalRenderLine
    let column: Int
    let shape: WatchTerminalCursorShape
    let blinks: Bool
    let blinkVisible: Bool
    let appearance: WatchTerminalAppearance
    let metrics: WatchTerminalMetrics

    var body: some View {
        cursor
            .opacity(blinks && !blinkVisible ? 0 : 1)
            .accessibilityHidden(true)
    }

    @ViewBuilder
    private var cursor: some View {
        switch shape {
        case .block:
            blockCursor
        case .beam:
            Rectangle()
                .fill(cursorColor)
                .frame(
                    width: max(1.5, metrics.cellWidth * 0.14),
                    height: metrics.lineHeight)
                .frame(
                    width: metrics.cellWidth,
                    height: metrics.lineHeight,
                    alignment: .leading)
        case .underline:
            Rectangle()
                .fill(cursorColor)
                .frame(
                    width: metrics.cellWidth,
                    height: max(1.5, metrics.lineHeight * 0.12))
                .frame(
                    width: metrics.cellWidth,
                    height: metrics.lineHeight,
                    alignment: .bottom)
        }
    }

    @ViewBuilder
    private var blockCursor: some View {
        if let run = runAtCursor {
            let resolvedStyle = appearance.palette.resolve(run.style)
            ZStack(alignment: .leading) {
                cursorColor

                // 重画整个 run 后裁出当前列，保留宽字符与组合字符的字形。
                WatchTerminalGlyphView(
                    run: run,
                    blinkVisible: blinkVisible,
                    font: metrics.font(
                        choice: appearance.font,
                        weight: resolvedStyle.weight),
                    foreground:
                        terminalColor(resolvedStyle.background),
                    isUnderlined:
                        resolvedStyle.decoration == .underline ||
                        run.hyperlink != nil)
                    .frame(
                        width:
                            CGFloat(run.columnCount) * metrics.cellWidth,
                        alignment: .leading)
                    .offset(
                        x: -CGFloat(column - run.startColumn) *
                            metrics.cellWidth)
            }
            .frame(
                width: metrics.cellWidth,
                height: metrics.lineHeight,
                alignment: .leading)
            .clipped()
        } else {
            Rectangle()
                .fill(cursorColor)
                .frame(
                    width: metrics.cellWidth,
                    height: metrics.lineHeight)
        }
    }

    private var runAtCursor: TerminalRenderRun? {
        line.runs.first {
            $0.startColumn <= column &&
                column < $0.startColumn + $0.columnCount
        }
    }

    private var cursorColor: Color {
        terminalColor(appearance.palette.cursor)
    }

}

private extension TerminalCursorShape {
    var watchShape: WatchTerminalCursorShape {
        switch self {
        case .block:
            return .block
        case .beam:
            return .beam
        case .underline:
            return .underline
        }
    }
}

struct WatchTerminalMetrics {
    let fontSize: CGFloat

    // SF Mono 的 advance 接近字号的 0.6；固定 advance 防止 ANSI run 改变列位置。
    var cellWidth: CGFloat {
        fontSize * 0.61
    }

    var lineHeight: CGFloat {
        fontSize * 1.16
    }

    func font(
        choice: WatchTerminalFontChoice,
        weight: WatchTerminalTextWeight
    ) -> Font {
        let swiftUIWeight: Font.Weight =
            weight == .bold ? .bold : .regular
        switch choice {
        case .systemMonospaced:
            return .system(
                size: fontSize,
                weight: swiftUIWeight,
                design: .monospaced)
        case .roundedMonospaced:
            return .system(
                size: fontSize,
                weight: swiftUIWeight,
                design: .rounded)
                .monospaced()
        }
    }
}

private func terminalColor(
    _ resolved: WatchTerminalResolvedColor
) -> Color {
    terminalColor(resolved.rgb, opacity: resolved.opacity)
}

private func terminalColor(
    _ rgb: WatchTerminalRGB,
    opacity: Double = 1
) -> Color {
    Color(
        .sRGB,
        red: rgb.redComponent,
        green: rgb.greenComponent,
        blue: rgb.blueComponent,
        opacity: opacity)
}

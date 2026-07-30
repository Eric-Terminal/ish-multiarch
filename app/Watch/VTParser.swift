import Foundation

enum VTParserState {
    case ground
    case escape
    case escapeIntermediate
    case csi
    case csiDiscard
    case osc
    case oscEscape
    case stringControl
    case stringControlEscape
}

struct VTUTF8Decoder {
    private var value: UInt32 = 0
    private var minimum: UInt32 = 0
    private var remaining = 0

    mutating func consume(_ byte: UInt8) -> [Unicode.Scalar] {
        var output: [Unicode.Scalar] = []
        var byteToConsume: UInt8? = byte
        while let candidate = byteToConsume {
            byteToConsume = nil
            if remaining == 0 {
                switch candidate {
                case 0x00...0x7f:
                    output.append(Unicode.Scalar(candidate))
                case 0xc2...0xdf:
                    value = UInt32(candidate & 0x1f)
                    minimum = 0x80
                    remaining = 1
                case 0xe0...0xef:
                    value = UInt32(candidate & 0x0f)
                    minimum = 0x800
                    remaining = 2
                case 0xf0...0xf4:
                    value = UInt32(candidate & 0x07)
                    minimum = 0x10000
                    remaining = 3
                default:
                    output.append(replacementScalar)
                }
                continue
            }

            guard (0x80...0xbf).contains(candidate) else {
                reset()
                output.append(replacementScalar)
                byteToConsume = candidate
                continue
            }

            value = (value << 6) | UInt32(candidate & 0x3f)
            remaining -= 1
            if remaining == 0 {
                let decoded = value
                let isValid = decoded >= minimum &&
                    decoded <= 0x10ffff &&
                    !(0xd800...0xdfff).contains(decoded)
                output.append(
                    isValid ? Unicode.Scalar(decoded)! : replacementScalar)
                reset()
            }
        }
        return output
    }

    mutating func flushIncomplete() -> Unicode.Scalar? {
        guard remaining != 0 else { return nil }
        reset()
        return replacementScalar
    }

    mutating func reset() {
        value = 0
        minimum = 0
        remaining = 0
    }

    private var replacementScalar: Unicode.Scalar {
        Unicode.Scalar(0xfffd)!
    }
}

extension TerminalScreen {
    mutating func append(_ bytes: [UInt8]) {
        for byte in bytes {
            consume(byte)
        }
    }

    mutating func reportDroppedBytes(_ count: UInt64) {
        guard count != 0 else { return }
        resetParser()
        carriageReturn()
        lineFeed()
        append(Array("[已省略 \(count) 字节输出]\r\n".utf8))
    }

    mutating func consume(_ byte: UInt8) {
        switch parserState {
        case .ground:
            consumeGround(byte)
        case .escape:
            consumeEscape(byte)
        case .escapeIntermediate:
            consumeEscapeIntermediate(byte)
        case .csi:
            consumeCSI(byte)
        case .csiDiscard:
            consumeDiscardedCSI(byte)
        case .osc:
            if byte == 0x07 {
                finishOSC()
            } else if byte == 0x1b {
                parserState = .oscEscape
            } else if byte == 0x18 || byte == 0x1a {
                cancelOSC()
            } else if byte == 0x9c {
                finishOSC()
            } else {
                appendOSC(byte)
            }
        case .oscEscape:
            if byte == 0x5c || byte == 0x07 || byte == 0x9c {
                finishOSC()
            } else if byte == 0x18 || byte == 0x1a {
                cancelOSC()
            } else {
                // OSC 内非 ST 的 ESC 序列视为畸形，继续有界丢弃到终止符。
                oscPayloadOverflowed = true
                parserState = byte == 0x1b ? .oscEscape : .osc
            }
        case .stringControl:
            if byte == 0x1b {
                parserState = .stringControlEscape
            } else if byte == 0x9c ||
                        byte == 0x18 ||
                        byte == 0x1a {
                parserState = .ground
            }
        case .stringControlEscape:
            if byte == 0x5c ||
                    byte == 0x9c ||
                    byte == 0x18 ||
                    byte == 0x1a {
                parserState = .ground
            } else if byte != 0x1b {
                parserState = .stringControl
            }
        }
    }

    mutating func consumeGround(_ byte: UInt8) {
        if byte == 0x1b {
            flushIncompleteUTF8()
            parserState = .escape
            return
        }
        if byte < 0x20 || byte == 0x7f {
            flushIncompleteUTF8()
            executeControl(byte)
            return
        }
        for scalar in utf8Decoder.consume(byte) {
            write(characterSetScalar(for: scalar))
        }
    }

    mutating func consumeEscape(_ byte: UInt8) {
        switch byte {
        case 0x1b:
            return
        case 0x5b:
            clearCSI()
            parserState = .csi
        case 0x5d:
            beginOSC()
        case 0x50, 0x58, 0x5e, 0x5f:
            parserState = .stringControl
        case 0x20...0x2f:
            escapeIntermediateByte = byte
            parserState = .escapeIntermediate
        case 0x37:
            saveCursor()
            parserState = .ground
        case 0x38:
            restoreCursor()
            parserState = .ground
        case 0x44:
            lineFeed()
            parserState = .ground
        case 0x45:
            carriageReturn()
            lineFeed()
            parserState = .ground
        case 0x4d:
            reverseIndex()
            parserState = .ground
        case 0x5a:
            reportDeviceAttributes([])
            parserState = .ground
        case 0x63:
            resetTerminal()
            parserState = .ground
        default:
            parserState = .ground
        }
    }

    mutating func consumeEscapeIntermediate(_ byte: UInt8) {
        if byte == 0x1b {
            escapeIntermediateByte = nil
            parserState = .escape
            return
        }
        if byte < 0x20 {
            executeControl(byte)
            return
        }
        if (0x20...0x2f).contains(byte) {
            escapeIntermediateByte = byte
            return
        }
        defer {
            escapeIntermediateByte = nil
            parserState = .ground
        }
        guard byte == 0x30 || byte == 0x42 else { return }
        let characterSet: VTCharacterSet =
            byte == 0x30 ? .decSpecialGraphics : .ascii
        switch escapeIntermediateByte {
        case 0x28:
            g0CharacterSet = characterSet
        case 0x29:
            g1CharacterSet = characterSet
        default:
            break
        }
    }

    mutating func consumeCSI(_ byte: UInt8) {
        if byte == 0x1b {
            clearCSI()
            parserState = .escape
            return
        }
        if byte == 0x18 || byte == 0x1a {
            clearCSI()
            parserState = .ground
            return
        }
        if byte < 0x20 {
            executeControl(byte)
            return
        }
        if (0x40...0x7e).contains(byte) {
            executeCSI(finalByte: byte)
            clearCSI()
            parserState = .ground
            return
        }
        if (0x3c...0x3f).contains(byte) &&
                csiParameterBytes.isEmpty &&
                csiIntermediateBytes.isEmpty &&
                csiPrivateMarker == nil {
            csiPrivateMarker = byte
            return
        }
        if (0x30...0x3b).contains(byte) || byte == 0x3a {
            guard csiIntermediateBytes.isEmpty else {
                discardCurrentCSI()
                return
            }
            guard csiParameterBytes.count <
                    TerminalScreen.maximumCSIParameterBytes else {
                discardCurrentCSI()
                return
            }
            csiParameterBytes.append(byte)
            return
        }
        if (0x3c...0x3f).contains(byte) {
            discardCurrentCSI()
            return
        }
        if (0x20...0x2f).contains(byte) {
            guard csiIntermediateBytes.count <
                    TerminalScreen.maximumCSIIntermediateBytes else {
                discardCurrentCSI()
                return
            }
            csiIntermediateBytes.append(byte)
            return
        }
        parserState = .ground
    }

    mutating func discardCurrentCSI() {
        clearCSI()
        parserState = .csiDiscard
    }

    mutating func clearCSI() {
        csiParameterBytes.removeAll(keepingCapacity: true)
        csiIntermediateBytes.removeAll(keepingCapacity: true)
        csiPrivateMarker = nil
    }

    mutating func beginOSC() {
        oscPayloadBytes.removeAll(keepingCapacity: true)
        oscPayloadOverflowed = false
        parserState = .osc
    }

    mutating func appendOSC(_ byte: UInt8) {
        guard !oscPayloadOverflowed else { return }
        guard oscPayloadBytes.count <
                TerminalScreen.maximumOSCPayloadBytes else {
            oscPayloadOverflowed = true
            return
        }
        oscPayloadBytes.append(byte)
    }

    mutating func finishOSC() {
        defer {
            oscPayloadBytes.removeAll(keepingCapacity: true)
            oscPayloadOverflowed = false
            parserState = .ground
        }
        if oscPayloadOverflowed {
            // 超限 OSC 8 无法确认闭合语义，主动结束当前链接作用域。
            if oscPayloadBytes.starts(with: [0x38, 0x3b]) {
                activeHyperlink = nil
            }
            return
        }
        guard let separator = oscPayloadBytes.firstIndex(of: 0x3b) else {
            return
        }
        let selector = String(
            decoding: oscPayloadBytes[..<separator],
            as: UTF8.self)
        if selector == "8" {
            applyOSC8(
                oscPayloadBytes[oscPayloadBytes.index(
                    after: separator)...])
            return
        }
        guard selector == "0" || selector == "2" else { return }

        let titleBytes = oscPayloadBytes[oscPayloadBytes.index(
            after: separator)...]
        let decoded = String(decoding: titleBytes, as: UTF8.self)
        let sanitized = String(decoded.unicodeScalars.filter {
            !CharacterSet.controlCharacters.contains($0)
        })
        let normalized = sanitized.trimmingCharacters(
            in: .whitespacesAndNewlines)
        guard !normalized.isEmpty else { return }
        publishTitle(String(
            normalized.prefix(
                TerminalScreen.maximumTerminalTitleCharacters)))
    }

    mutating func applyOSC8(_ payload: ArraySlice<UInt8>) {
        guard let separator = payload.firstIndex(of: 0x3b) else {
            activeHyperlink = nil
            return
        }
        let uriBytes = payload[payload.index(after: separator)...]
        guard !uriBytes.isEmpty else {
            activeHyperlink = nil
            return
        }
        guard let uri = String(bytes: uriBytes, encoding: .utf8),
              uri.unicodeScalars.count <=
                TerminalScreen.maximumHyperlinkCharacters,
              !uri.unicodeScalars.contains(where: {
                  CharacterSet.controlCharacters.contains($0) ||
                      CharacterSet.whitespacesAndNewlines.contains($0)
              }),
              let components = URLComponents(string: uri),
              let scheme = components.scheme?.lowercased() else {
            activeHyperlink = nil
            return
        }

        switch scheme {
        case "http", "https":
            guard let host = components.host,
                  !host.isEmpty,
                  let url = components.url else {
                activeHyperlink = nil
                return
            }
            activeHyperlink = url.absoluteString
        case "mailto":
            guard uri.count > "mailto:".count,
                  let url = components.url else {
                activeHyperlink = nil
                return
            }
            activeHyperlink = url.absoluteString
        default:
            activeHyperlink = nil
        }
    }

    mutating func cancelOSC() {
        oscPayloadBytes.removeAll(keepingCapacity: true)
        oscPayloadOverflowed = false
        parserState = .ground
    }

    mutating func consumeDiscardedCSI(_ byte: UInt8) {
        if byte == 0x1b {
            parserState = .escape
        } else if byte == 0x18 || byte == 0x1a ||
                    (0x40...0x7e).contains(byte) {
            parserState = .ground
        } else if byte < 0x20 {
            executeControl(byte)
        }
    }

    mutating func executeControl(_ byte: UInt8) {
        switch byte {
        case 0x08:
            backspace()
        case 0x09:
            horizontalTab()
        case 0x0a, 0x0b, 0x0c:
            lineFeed()
        case 0x0d:
            carriageReturn()
        case 0x0e:
            usesG1CharacterSet = true
        case 0x0f:
            usesG1CharacterSet = false
        default:
            break
        }
    }

    mutating func executeCSI(finalByte: UInt8) {
        let parameters = parsedCSIParameters()
        if !csiIntermediateBytes.isEmpty {
            if finalByte == 0x70 &&
                    csiIntermediateBytes == [0x24] {
                reportMode(parameters)
            } else if finalByte == 0x71 &&
                    csiIntermediateBytes == [0x20] &&
                    csiPrivateMarker == nil &&
                    parameters.count <= 1 {
                setCursorStyle(parameters.first.flatMap { $0 } ?? 0)
            }
            return
        }
        if let marker = csiPrivateMarker {
            executePrivateCSI(
                marker: marker,
                finalByte: finalByte,
                parameters: parameters)
            return
        }
        let count = csiCount(parameters)
        switch finalByte {
        case 0x40:
            insertCharacters(count)
        case 0x41:
            moveCursor(rowDelta: -count)
        case 0x42:
            moveCursor(rowDelta: count)
        case 0x43:
            moveCursor(columnDelta: count)
        case 0x44:
            moveCursor(columnDelta: -count)
        case 0x45:
            moveCursor(rowDelta: count, resetsColumn: true)
        case 0x46:
            moveCursor(rowDelta: -count, resetsColumn: true)
        case 0x47:
            setCursor(column: csiPosition(parameters, at: 0) - 1)
        case 0x49:
            for _ in 0..<min(count, columns) {
                horizontalTab()
            }
        case 0x48, 0x66:
            setCursor(
                row: csiPosition(parameters, at: 0) - 1,
                column: csiPosition(parameters, at: 1) - 1)
        case 0x4a:
            eraseDisplay(mode: parameters.first.flatMap { $0 } ?? 0)
        case 0x4b:
            eraseLine(mode: parameters.first.flatMap { $0 } ?? 0)
        case 0x4c:
            insertLines(count)
        case 0x4d:
            deleteLines(count)
        case 0x50:
            deleteCharacters(count)
        case 0x53:
            scrollUp(count)
        case 0x54:
            scrollDown(count)
        case 0x58:
            eraseCharacters(count)
        case 0x5a:
            for _ in 0..<min(count, columns) {
                horizontalBackTab()
            }
        case 0x60:
            setCursor(column: csiPosition(parameters, at: 0) - 1)
        case 0x61:
            moveCursor(columnDelta: count)
        case 0x63:
            reportDeviceAttributes(parameters)
        case 0x64:
            setCursor(row: csiPosition(parameters, at: 0) - 1)
        case 0x65:
            moveCursor(rowDelta: count)
        case 0x68, 0x6c:
            setModes(
                parameters,
                enabled: finalByte == 0x68,
                isPrivate: false)
        case 0x6d:
            applySGR(parameters)
        case 0x6e:
            reportDeviceStatus(parameters)
        case 0x72:
            let top = parameters.indices.contains(0) &&
                    parameters[0] != nil ?
                max(1, parameters[0]!) - 1 : nil
            let bottom = parameters.indices.contains(1) &&
                    parameters[1] != nil ?
                max(1, parameters[1]!) - 1 : nil
            setScrollRegion(top: top, bottom: bottom)
        case 0x73:
            saveCursor()
        case 0x75:
            restoreCursor()
        default:
            break
        }
    }

    mutating func executePrivateCSI(
        marker: UInt8,
        finalByte: UInt8,
        parameters: [Int?]
    ) {
        switch (marker, finalByte) {
        case (0x3f, 0x68), (0x3f, 0x6c):
            setModes(
                parameters,
                enabled: finalByte == 0x68,
                isPrivate: true)
        case (0x3f, 0x6e):
            reportDeviceStatus(parameters)
        case (0x3e, 0x63):
            reportDeviceAttributes(parameters)
        default:
            break
        }
    }

    mutating func setModes(
        _ parameters: [Int?],
        enabled: Bool,
        isPrivate: Bool
    ) {
        for parameter in parameters.compactMap({ $0 }) {
            if !isPrivate {
                if parameter == 4 {
                    setInsertMode(enabled)
                }
                continue
            }
            switch parameter {
            case 1:
                setApplicationCursorKeys(enabled)
            case 6:
                setOriginMode(enabled)
            case 7:
                setAutoWrap(enabled)
            case 25:
                setCursorVisibility(enabled)
            case 1000:
                setMouseTracking(.normal, enabled: enabled)
            case 1002:
                setMouseTracking(.buttonEvent, enabled: enabled)
            case 1003:
                setMouseTracking(.anyEvent, enabled: enabled)
            case 1004:
                setFocusReporting(enabled)
            case 1006:
                setSGRMouseEncoding(enabled)
            case 47:
                setAlternateScreen(enabled)
            case 1047:
                setAlternateScreenClearingOnExit(enabled)
            case 1048:
                if enabled {
                    saveCursor()
                } else {
                    restoreCursor()
                }
            case 1049:
                if enabled {
                    saveAndEnterAlternateScreen()
                } else {
                    leaveAlternateScreenAndRestore()
                }
            case 2004:
                setBracketedPaste(enabled)
            default:
                break
            }
        }
    }

    mutating func reportDeviceAttributes(_ parameters: [Int?]) {
        guard csiIntermediateBytes.isEmpty,
              parameters.count <= 1 else {
            return
        }
        let parameter = parameters.first.flatMap { $0 } ?? 0
        guard parameter == 0 else { return }
        switch csiPrivateMarker {
        case nil:
            // 只声明当前实现实际具备的 VT100 高级视频能力。
            queueResponse(Array("\u{1b}[?1;2c".utf8))
        case 0x3e:
            queueResponse(Array("\u{1b}[>0;0;0c".utf8))
        default:
            break
        }
    }

    mutating func reportDeviceStatus(_ parameters: [Int?]) {
        guard csiIntermediateBytes.isEmpty,
              parameters.count == 1,
              let parameter = parameters.first.flatMap({ $0 }) else {
            return
        }
        let isPrivate = csiPrivateMarker == 0x3f
        guard csiPrivateMarker == nil || isPrivate else { return }
        switch parameter {
        case 5:
            let prefix = isPrivate ? "?" : ""
            queueResponse(Array("\u{1b}[\(prefix)0n".utf8))
        case 6:
            let index = activeBufferIndex
            let origin = modes.usesOriginMode ?
                buffers[index].scrollTop : 0
            let row = buffers[index].cursorRow - origin + 1
            let column = buffers[index].cursorColumn + 1
            let prefix = isPrivate ? "?" : ""
            queueResponse(
                Array("\u{1b}[\(prefix)\(row);\(column)R".utf8))
        default:
            break
        }
    }

    mutating func reportMode(_ parameters: [Int?]) {
        guard let parameter = parameters.first.flatMap({ $0 }),
              parameters.count == 1 else {
            return
        }
        let isPrivate = csiPrivateMarker == 0x3f
        guard csiPrivateMarker == nil || isPrivate else { return }
        let status = modeStatus(parameter, isPrivate: isPrivate)
        let prefix = isPrivate ? "?" : ""
        queueResponse(
            Array("\u{1b}[\(prefix)\(parameter);\(status)$y".utf8))
    }

    func modeStatus(_ parameter: Int, isPrivate: Bool) -> Int {
        if !isPrivate {
            return parameter == 4 ? (modes.usesInsertMode ? 1 : 2) : 0
        }
        switch parameter {
        case 1:
            return modes.usesApplicationCursorKeys ? 1 : 2
        case 6:
            return modes.usesOriginMode ? 1 : 2
        case 7:
            return modes.usesAutoWrap ? 1 : 2
        case 25:
            return modes.isCursorVisible ? 1 : 2
        case 1000:
            return modes.mouseTrackingMode == .normal ? 1 : 2
        case 1002:
            return modes.mouseTrackingMode == .buttonEvent ? 1 : 2
        case 1003:
            return modes.mouseTrackingMode == .anyEvent ? 1 : 2
        case 1004:
            return modes.reportsFocusEvents ? 1 : 2
        case 1006:
            return modes.usesSGRMouseEncoding ? 1 : 2
        case 47, 1047, 1049:
            return modes.usesAlternateScreen ? 1 : 2
        case 2004:
            return modes.usesBracketedPaste ? 1 : 2
        default:
            return 0
        }
    }

    mutating func applySGR(_ suppliedParameters: [Int?]) {
        let parameters = suppliedParameters.isEmpty ?
            [Optional(0)] : suppliedParameters
        var index = 0
        while index < parameters.count {
            let code = parameters[index] ?? 0
            switch code {
            case 0:
                currentStyle = TerminalStyle()
            case 1:
                currentStyle.isBold = true
            case 2:
                currentStyle.isDim = true
            case 3:
                currentStyle.isItalic = true
            case 4:
                currentStyle.isUnderlined = true
            case 5, 6:
                currentStyle.isBlinking = true
            case 7:
                currentStyle.isInverse = true
            case 8:
                currentStyle.isHidden = true
            case 9:
                currentStyle.isStruckThrough = true
            case 21, 22:
                currentStyle.isBold = false
                currentStyle.isDim = false
            case 23:
                currentStyle.isItalic = false
            case 24:
                currentStyle.isUnderlined = false
            case 25:
                currentStyle.isBlinking = false
            case 27:
                currentStyle.isInverse = false
            case 28:
                currentStyle.isHidden = false
            case 29:
                currentStyle.isStruckThrough = false
            case 30...37:
                currentStyle.foreground =
                    .indexed(UInt8(code - 30))
            case 39:
                currentStyle.foreground = .default
            case 40...47:
                currentStyle.background =
                    .indexed(UInt8(code - 40))
            case 49:
                currentStyle.background = .default
            case 90...97:
                currentStyle.foreground =
                    .indexed(UInt8(code - 90 + 8))
            case 100...107:
                currentStyle.background =
                    .indexed(UInt8(code - 100 + 8))
            case 38, 48:
                if let result = extendedColor(
                        parameters, startingAt: index + 1) {
                    if code == 38 {
                        currentStyle.foreground = result.color
                    } else {
                        currentStyle.background = result.color
                    }
                    index += result.consumed
                }
            default:
                break
            }
            index += 1
        }
    }

    func extendedColor(
        _ parameters: [Int?],
        startingAt index: Int
    ) -> (color: TerminalColor, consumed: Int)? {
        guard index < parameters.count,
              let kind = parameters[index] else {
            return nil
        }
        if kind == 5,
                index + 1 < parameters.count,
                let color = byteValue(parameters[index + 1]) {
            return (.indexed(color), 2)
        }
        if kind == 2,
                index + 3 < parameters.count,
                let red = byteValue(parameters[index + 1]),
                let green = byteValue(parameters[index + 2]),
                let blue = byteValue(parameters[index + 3]) {
            return (.rgb(red: red, green: green, blue: blue), 4)
        }
        return nil
    }

    func byteValue(_ value: Int?) -> UInt8? {
        guard let value, (0...255).contains(value) else {
            return nil
        }
        return UInt8(value)
    }

    func parsedCSIParameters() -> [Int?] {
        guard !csiParameterBytes.isEmpty else { return [] }
        var parameters: [Int?] = []
        var value: Int?
        for byte in csiParameterBytes {
            if byte == 0x3b || byte == 0x3a {
                parameters.append(value)
                value = nil
            } else if (0x30...0x39).contains(byte) {
                let digit = Int(byte - 0x30)
                value = min(
                    1_000_000,
                    (value ?? 0) * 10 + digit)
            }
        }
        parameters.append(value)
        return parameters
    }

    func csiCount(_ parameters: [Int?]) -> Int {
        guard let supplied = parameters.first ?? nil, supplied > 0 else {
            return 1
        }
        return supplied
    }

    func csiPosition(_ parameters: [Int?], at index: Int) -> Int {
        guard parameters.indices.contains(index),
              let supplied = parameters[index],
              supplied > 0 else {
            return 1
        }
        return supplied
    }

    mutating func flushIncompleteUTF8() {
        if let replacement = utf8Decoder.flushIncomplete() {
            write(replacement)
        }
    }

    mutating func resetParser() {
        parserState = .ground
        clearCSI()
        escapeIntermediateByte = nil
        oscPayloadBytes.removeAll(keepingCapacity: true)
        oscPayloadOverflowed = false
        activeHyperlink = nil
        utf8Decoder.reset()
    }

    func characterSetScalar(
        for scalar: Unicode.Scalar
    ) -> Unicode.Scalar {
        let characterSet = usesG1CharacterSet ?
            g1CharacterSet : g0CharacterSet
        guard characterSet == .decSpecialGraphics else {
            return scalar
        }
        let mappedValue: UInt32
        switch scalar.value {
        case 0x60: mappedValue = 0x25c6
        case 0x61: mappedValue = 0x2592
        case 0x62: mappedValue = 0x2409
        case 0x63: mappedValue = 0x240c
        case 0x64: mappedValue = 0x240d
        case 0x65: mappedValue = 0x240a
        case 0x66: mappedValue = 0x00b0
        case 0x67: mappedValue = 0x00b1
        case 0x68: mappedValue = 0x2424
        case 0x69: mappedValue = 0x240b
        case 0x6a: mappedValue = 0x2518
        case 0x6b: mappedValue = 0x2510
        case 0x6c: mappedValue = 0x250c
        case 0x6d: mappedValue = 0x2514
        case 0x6e: mappedValue = 0x253c
        case 0x6f: mappedValue = 0x23ba
        case 0x70: mappedValue = 0x23bb
        case 0x71: mappedValue = 0x2500
        case 0x72: mappedValue = 0x23bc
        case 0x73: mappedValue = 0x23bd
        case 0x74: mappedValue = 0x251c
        case 0x75: mappedValue = 0x2524
        case 0x76: mappedValue = 0x2534
        case 0x77: mappedValue = 0x252c
        case 0x78: mappedValue = 0x2502
        case 0x79: mappedValue = 0x2264
        case 0x7a: mappedValue = 0x2265
        case 0x7b: mappedValue = 0x03c0
        case 0x7c: mappedValue = 0x2260
        case 0x7d: mappedValue = 0x00a3
        case 0x7e: mappedValue = 0x00b7
        default: return scalar
        }
        return Unicode.Scalar(mappedValue)!
    }
}

func terminalColumnWidth(_ scalar: Unicode.Scalar) -> Int {
    let value = scalar.value
    if value == 0x200d ||
            (0x0300...0x036f).contains(value) ||
            (0x1ab0...0x1aff).contains(value) ||
            (0x1dc0...0x1dff).contains(value) ||
            (0x20d0...0x20ff).contains(value) ||
            (0xfe00...0xfe0f).contains(value) ||
            (0xfe20...0xfe2f).contains(value) ||
            (0x1f3fb...0x1f3ff).contains(value) {
        return 0
    }

    if (0x1100...0x115f).contains(value) ||
            value == 0x2329 || value == 0x232a ||
            (0x2e80...0xa4cf).contains(value) ||
            (0xac00...0xd7a3).contains(value) ||
            (0xf900...0xfaff).contains(value) ||
            (0xfe10...0xfe19).contains(value) ||
            (0xfe30...0xfe6f).contains(value) ||
            (0xff00...0xff60).contains(value) ||
            (0xffe0...0xffe6).contains(value) ||
            (0x1f300...0x1faff).contains(value) ||
            (0x20000...0x3fffd).contains(value) {
        return 2
    }
    return 1
}

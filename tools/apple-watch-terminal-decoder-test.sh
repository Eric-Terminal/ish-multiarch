#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-decoder-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$ROOT/app/Watch/TerminalScreen.swift" \
    "$ROOT/app/Watch/VTParser.swift" \
    "$ROOT/app/Watch/WatchTerminalSession.swift" \
    "$ROOT/app/Watch/WatchRuntime.swift" \
    "$ROOT/tests/apple/watch-terminal-decoder-test.swift" \
    -o "$TEMPORARY/watch-terminal-decoder-test"
"$TEMPORARY/watch-terminal-decoder-test"

# UI 渲染依赖 SwiftUI，仅做 Watch Simulator SDK 类型检查，不启动构建或模拟器。
xcrun --sdk watchsimulator swiftc \
    -target "$(uname -m)-apple-watchos10.0-simulator" \
    -warnings-as-errors \
    -typecheck \
    "$ROOT/app/Watch/TerminalScreen.swift" \
    "$ROOT/app/Watch/VTParser.swift" \
    "$ROOT/app/Watch/WatchTerminalAppearance.swift" \
    "$ROOT/app/Watch/WatchTerminalThemes.swift" \
    "$ROOT/app/Watch/WatchTerminalSession.swift" \
    "$ROOT/app/Watch/WatchTerminalView.swift"

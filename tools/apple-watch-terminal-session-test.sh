#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-session-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$ROOT/app/Watch/TerminalScreen.swift" \
    "$ROOT/app/Watch/TerminalScreenStorage.swift" \
    "$ROOT/app/Watch/VTParser.swift" \
    "$ROOT/app/Watch/WatchTerminalSession.swift" \
    "$ROOT/app/Watch/WatchRuntime.swift" \
    "$ROOT/tests/apple/watch-terminal-session-test.swift" \
    -o "$TEMPORARY/watch-terminal-session-test"
"$TEMPORARY/watch-terminal-session-test"

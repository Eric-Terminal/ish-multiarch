#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-appearance-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$ROOT/app/Watch/TerminalScreen.swift" \
    "$ROOT/app/Watch/VTParser.swift" \
    "$ROOT/app/Watch/WatchTerminalAppearance.swift" \
    "$ROOT/app/Watch/WatchTerminalThemes.swift" \
    "$ROOT/app/Watch/WatchSharedFiles.swift" \
    "$ROOT/tests/apple/watch-terminal-appearance-test.swift" \
    -o "$TEMPORARY/watch-terminal-appearance-test"
"$TEMPORARY/watch-terminal-appearance-test"

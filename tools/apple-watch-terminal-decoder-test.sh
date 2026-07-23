#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-decoder-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$ROOT/app/Watch/TerminalDecoder.swift" \
    "$ROOT/tests/apple/watch-terminal-decoder-test.swift" \
    -o "$TEMPORARY/watch-terminal-decoder-test"
"$TEMPORARY/watch-terminal-decoder-test"

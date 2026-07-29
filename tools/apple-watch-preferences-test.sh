#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-preferences-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$ROOT/app/Watch/WatchPreferences.swift" \
    "$ROOT/tests/apple/watch-preferences-test.swift" \
    -o "$TEMPORARY/watch-preferences-test"
"$TEMPORARY/watch-preferences-test"

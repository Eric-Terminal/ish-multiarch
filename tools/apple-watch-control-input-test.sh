#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-control-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$ROOT/app/Watch/WatchControlInput.swift" \
    "$ROOT/tests/apple/watch-control-input-test.swift" \
    -o "$TEMPORARY/watch-control-input-test"
"$TEMPORARY/watch-control-input-test"

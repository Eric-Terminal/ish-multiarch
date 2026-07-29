#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMPORARY=$(mktemp -d "${TMPDIR:-/tmp}/ish-terminal-tabs-test.XXXXXX")
trap 'rm -rf "$TEMPORARY"' EXIT

xcrun --sdk macosx clang \
    -fobjc-arc \
    -Wall \
    -Wextra \
    -Werror \
    -framework Foundation \
    "$ROOT/tests/apple/terminal-tabs-state-test.m" \
    -o "$TEMPORARY/terminal-tabs-state-test"
"$TEMPORARY/terminal-tabs-state-test"

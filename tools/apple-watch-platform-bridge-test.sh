#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d \
    "${TMPDIR:-/tmp}/ish-watch-platform-bridge.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

set -- \
    "$repo_root/app/Watch/TerminalScreen.swift" \
    "$repo_root/app/Watch/TerminalScreenStorage.swift" \
    "$repo_root/app/Watch/VTParser.swift" \
    "$repo_root/app/Watch/WatchTerminalAppearance.swift" \
    "$repo_root/app/Watch/WatchTerminalThemes.swift" \
    "$repo_root/app/Watch/WatchPreferences.swift" \
    "$repo_root/app/Watch/WatchPlatformBridge.swift"

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$@" \
    "$repo_root/tests/apple/watch-platform-bridge-test.swift" \
    -o "$build_dir/watch-platform-bridge-test"

"$build_dir/watch-platform-bridge-test"

watch_sdk=$(xcrun --sdk watchsimulator --show-sdk-path)
xcrun swiftc \
    -warnings-as-errors \
    -typecheck \
    -swift-version 5 \
    -enable-upcoming-feature StrictConcurrency \
    -target "$(uname -m)-apple-watchos10.0-simulator" \
    -sdk "$watch_sdk" \
    -I "$repo_root" \
    -import-objc-header \
    "$repo_root/app/Watch/iSHWatch-Bridging-Header.h" \
    "$@"

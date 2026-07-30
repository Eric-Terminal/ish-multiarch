#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d \
    "${TMPDIR:-/tmp}/ish-watch-shared-files.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$repo_root/app/Watch/WatchSharedFiles.swift" \
    "$repo_root/tests/apple/watch-shared-files-test.swift" \
    -o "$build_dir/watch-shared-files-test"

"$build_dir/watch-shared-files-test"

watch_sdk=$(xcrun --sdk watchsimulator --show-sdk-path)
xcrun swiftc \
    -warnings-as-errors \
    -typecheck \
    -target "$(uname -m)-apple-watchos10.0-simulator" \
    -sdk "$watch_sdk" \
    "$repo_root/app/Watch/WatchSharedFiles.swift"

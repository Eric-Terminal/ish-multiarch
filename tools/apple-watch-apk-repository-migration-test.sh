#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d \
    "${TMPDIR:-/tmp}/ish-watch-apk-repository-migration.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

xcrun --sdk macosx swiftc \
    -warnings-as-errors \
    -parse-as-library \
    "$repo_root/app/Watch/WatchAPKRepositoryMigration.swift" \
    "$repo_root/tests/apple/watch-apk-repository-migration-test.swift" \
    -o "$build_dir/watch-apk-repository-migration-test"

"$build_dir/watch-apk-repository-migration-test"

watch_sdk=$(xcrun --sdk watchsimulator --show-sdk-path)
xcrun swiftc \
    -warnings-as-errors \
    -typecheck \
    -target arm64-apple-watchos10.0-simulator \
    -sdk "$watch_sdk" \
    -I "$repo_root" \
    -import-objc-header \
    "$repo_root/app/Watch/iSHWatch-Bridging-Header.h" \
    "$repo_root/app/Watch/WatchAPKRepositoryMigration.swift"

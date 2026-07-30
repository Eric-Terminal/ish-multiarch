#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-host-archive.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

xcrun swiftc \
    "$repo_root/app/Watch/WatchHostRootArchive.swift" \
    "$repo_root/tests/apple/watch-host-root-archive-test.swift" \
    -o "$build_dir/watch-host-root-archive-test"

"$build_dir/watch-host-root-archive-test"

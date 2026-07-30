#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-root-metadata.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

xcrun swiftc \
    "$repo_root/app/Watch/WatchRootMetadata.swift" \
    "$repo_root/tests/apple/watch-root-metadata-test.swift" \
    -o "$build_dir/watch-root-metadata-test"

"$build_dir/watch-root-metadata-test"

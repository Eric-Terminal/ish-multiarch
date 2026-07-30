#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-root-archive.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

xcrun clang \
    -std=gnu11 -Wall -Wextra -Werror \
    -I"$repo_root" \
    "$repo_root/platform/apple-watch-root-archive.c" \
    "$repo_root/platform/apple-watch-root-archive-test.c" \
    -o "$build_dir/apple-watch-root-archive-test"

"$build_dir/apple-watch-root-archive-test"

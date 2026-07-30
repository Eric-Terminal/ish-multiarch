#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ish-watch-root-roundtrip.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

if command -v brew >/dev/null 2>&1; then
    libarchive_prefix=$(brew --prefix libarchive)
elif [ -d /opt/homebrew/opt/libarchive ]; then
    libarchive_prefix=/opt/homebrew/opt/libarchive
elif [ -d /usr/local/opt/libarchive ]; then
    libarchive_prefix=/usr/local/opt/libarchive
else
    echo "错误：真实 roundtrip 需要可用的 libarchive 开发文件。" >&2
    exit 1
fi

xcrun clang \
    -std=gnu11 -Wall -Wextra -Werror \
    -DISH_FAKEFS_TESTING \
    -I"$repo_root" \
    -I"$libarchive_prefix/include" \
    "$repo_root/tools/fakefs.c" \
    "$repo_root/util/fchdir.c" \
    "$repo_root/platform/apple-watch-root-archive-roundtrip-test.c" \
    -L"$libarchive_prefix/lib" \
    -Wl,-rpath,"$libarchive_prefix/lib" \
    -larchive \
    -lsqlite3 \
    -o "$build_dir/apple-watch-root-roundtrip-test"

"$build_dir/apple-watch-root-roundtrip-test"

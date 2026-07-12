#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_ROOT=${1:-"$ROOT/build-apple-core"}

if [[ -z "${MESON:-}" ]]; then
    MESON=$(command -v meson || true)
fi
if [[ -z "${NINJA:-}" ]]; then
    NINJA=$(command -v ninja || true)
fi
if [[ -z "$MESON" ]]; then
    echo "错误：找不到 meson，请通过 MESON 环境变量指定可执行文件。" >&2
    exit 1
fi
if [[ -z "$NINJA" ]]; then
    echo "错误：找不到 ninja，请通过 NINJA 环境变量指定可执行文件。" >&2
    exit 1
fi

CLANG=$(xcrun --find clang)
AR=$(xcrun --find ar)
STRIP=$(xcrun --find strip)
mkdir -p "$BUILD_ROOT"

write_cross_file() {
    local cross_file=$1
    local sdk=$2
    local arch=$3
    local target=$4
    local sysroot
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)

    cat > "$cross_file" <<EOF
[binaries]
c = '$CLANG'
ar = '$AR'
strip = '$STRIP'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = '$arch'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-target', '$target', '-isysroot', '$sysroot', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-Wshorten-64-to-32', '-Wpointer-to-int-cast', '-Wint-to-pointer-cast', '-Wcast-align']
c_link_args = ['-target', '$target', '-isysroot', '$sysroot']
EOF
}

build_slice() {
    local name=$1
    local sdk=$2
    local arch=$3
    local target=$4
    local platform=$5
    local cross_file="$BUILD_ROOT/cross-$name.ini"
    local build_dir="$BUILD_ROOT/$name"

    echo "==> 构建 ${name}（${target}）"
    write_cross_file "$cross_file" "$sdk" "$arch" "$target"
    if [[ -d "$build_dir/meson-private" ]]; then
        "$MESON" setup --wipe "$build_dir" "$ROOT" \
            --cross-file "$cross_file" -Dcore_only=true --buildtype=release
    else
        "$MESON" setup "$build_dir" "$ROOT" \
            --cross-file "$cross_file" -Dcore_only=true --buildtype=release
    fi
    "$NINJA" -C "$build_dir" libish_aarch64_core.a aarch64_core_link_smoke

    local sysroot
    local float80_object="$build_dir/float80-compile.o"
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
    echo "==> 编译 ${name} 的 float80 可移植性证据"
    "$CLANG" -target "$target" -isysroot "$sysroot" -I"$ROOT" \
        -std=gnu11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
        -Wshorten-64-to-32 -Wpointer-to-int-cast -Wint-to-pointer-cast \
        -Wcast-align -c "$ROOT/emu/float80.c" -o "$float80_object"
    file "$build_dir/libish_aarch64_core.a" "$build_dir/aarch64_core_link_smoke"
    file "$float80_object"
    if ! xcrun vtool -show-build "$build_dir/aarch64_core_link_smoke" | \
            grep -q "platform $platform"; then
        echo "错误：${name} 的 Mach-O 平台不是 ${platform}。" >&2
        exit 1
    fi
}

build_slice iphoneos-arm64 iphoneos arm64 arm64-apple-ios15.0 IOS
build_slice watchos-arm64_32 watchos arm64_32 arm64_32-apple-watchos10.0 WATCHOS
build_slice watchos-arm64 watchos arm64 arm64-apple-watchos26.0 WATCHOS

WATCH_LIBRARY="$BUILD_ROOT/libish_aarch64_core-watchos.a"
xcrun lipo -create \
    "$BUILD_ROOT/watchos-arm64_32/libish_aarch64_core.a" \
    "$BUILD_ROOT/watchos-arm64/libish_aarch64_core.a" \
    -output "$WATCH_LIBRARY"
xcrun lipo "$BUILD_ROOT/iphoneos-arm64/libish_aarch64_core.a" \
    -verify_arch arm64
xcrun lipo "$WATCH_LIBRARY" -verify_arch arm64_32 arm64

echo "==> Apple core 架构验证完成"
xcrun lipo -info "$BUILD_ROOT/iphoneos-arm64/libish_aarch64_core.a"
xcrun lipo -info "$WATCH_LIBRARY"

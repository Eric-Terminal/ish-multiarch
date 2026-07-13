#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_ROOT=${1:-"$ROOT/build-apple-core"}
source "$ROOT/tools/apple-meson-arch.sh"

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
    local mode=$5
    local sysroot
    local c_args
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
    apple_meson_architecture "$arch"

    c_args="'-target', '$target', '-isysroot', '$sysroot', '-Wall', '-Wextra'"
    if [[ "$mode" == strict ]]; then
        c_args+=", '-Werror', '-Wconversion', '-Wsign-conversion', '-Wshorten-64-to-32', '-Wpointer-to-int-cast', '-Wint-to-pointer-cast', '-Wcast-align'"
    else
        c_args+=", '-Werror=shift-count-overflow'"
    fi

    cat > "$cross_file" <<EOF
[binaries]
c = '$CLANG'
ar = '$AR'
strip = '$STRIP'

[host_machine]
system = 'darwin'
cpu_family = '$APPLE_MESON_CPU_FAMILY'
cpu = '$APPLE_MESON_CPU'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = [$c_args]
c_link_args = ['-target', '$target', '-isysroot', '$sysroot']
EOF
}

build_slice() {
    local name=$1
    local sdk=$2
    local arch=$3
    local target=$4
    local platform=$5
    local word_bytes=$6
    local core_cross_file="$BUILD_ROOT/cross-core-$name.ini"
    local core_build_dir="$BUILD_ROOT/core/$name"
    local full_cross_file="$BUILD_ROOT/cross-full-$name.ini"
    local full_build_dir="$BUILD_ROOT/full/$name"

    echo "==> 严格构建 ${name} 的 AArch64 core（${target}）"
    write_cross_file "$core_cross_file" "$sdk" "$arch" "$target" strict
    if [[ -d "$core_build_dir/meson-private" ]]; then
        "$MESON" setup --wipe "$core_build_dir" "$ROOT" \
            --cross-file "$core_cross_file" -Dcore_only=true --buildtype=release
    else
        "$MESON" setup "$core_build_dir" "$ROOT" \
            --cross-file "$core_cross_file" -Dcore_only=true --buildtype=release
    fi
    "$NINJA" -C "$core_build_dir" \
        libish_aarch64_core.a aarch64_core_link_smoke

    local sysroot
    local float80_object="$core_build_dir/float80-compile.o"
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
    echo "==> 编译 ${name} 的 float80 可移植性证据"
    "$CLANG" -target "$target" -isysroot "$sysroot" -I"$ROOT" \
        -std=gnu11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
        -Wshorten-64-to-32 -Wpointer-to-int-cast -Wint-to-pointer-cast \
        -Wcast-align -c "$ROOT/emu/float80.c" -o "$float80_object"
    file "$core_build_dir/libish_aarch64_core.a" \
        "$core_build_dir/aarch64_core_link_smoke" "$float80_object"

    echo "==> 完整构建 ${name} 的 kernel/fs/platform"
    write_cross_file "$full_cross_file" "$sdk" "$arch" "$target" full
    if [[ -d "$full_build_dir/meson-private" ]]; then
        "$MESON" setup --wipe "$full_build_dir" "$ROOT" \
            --cross-file "$full_cross_file" -Dcore_only=false \
            -Dkernel=ish -Dengine=asbestos --buildtype=release
    else
        "$MESON" setup "$full_build_dir" "$ROOT" \
            --cross-file "$full_cross_file" -Dcore_only=false \
            -Dkernel=ish -Dengine=asbestos --buildtype=release
    fi
    "$NINJA" -C "$full_build_dir" \
        libish.a libish_emu.a libfakefs.a darwin_platform_link_smoke

    local abi_probe="$full_build_dir/apple-abi-probe.o"
    "$CLANG" -target "$target" -isysroot "$sysroot" -I"$ROOT" \
        -std=gnu11 -Wall -Wextra -Werror \
        -DEXPECTED_APPLE_WORD_BYTES="$word_bytes" \
        -c "$ROOT/tools/apple-abi-probe.c" -o "$abi_probe"

    local library
    for library in libish.a libish_emu.a libfakefs.a; do
        file "$full_build_dir/$library"
        xcrun lipo "$full_build_dir/$library" -verify_arch "$arch"
    done
    file "$full_build_dir/darwin_platform_link_smoke" "$abi_probe"

    local archive_members
    archive_members=$("$AR" -t "$full_build_dir/libish.a")
    local required_member
    for required_member in kernel_init.c.o fs_fd.c.o platform_darwin.c.o; do
        if ! grep -Fqx "$required_member" <<< "$archive_members"; then
            echo "错误：${name} 的 libish.a 缺少 ${required_member}。" >&2
            exit 1
        fi
    done

    local undefined_symbols
    undefined_symbols=$(xcrun nm -u "$full_build_dir/libish.a" \
        "$full_build_dir/darwin_platform_link_smoke")
    if grep -Eq '(^|[[:space:]])_host_info$' <<< "$undefined_symbols"; then
        echo "错误：${name} 仍引用 watchOS 禁用的 host_info。" >&2
        exit 1
    fi

    local build_info
    build_info=$(xcrun vtool -show-build \
        "$full_build_dir/darwin_platform_link_smoke")
    if ! grep -q "platform $platform" <<< "$build_info"; then
        echo "错误：${name} 的 Mach-O 平台不是 ${platform}。" >&2
        exit 1
    fi
}

build_slice iphoneos-arm64 iphoneos arm64 arm64-apple-ios15.0 IOS 8
build_slice watchos-arm64_32 watchos arm64_32 \
    arm64_32-apple-watchos10.0 WATCHOS 4
build_slice watchos-arm64 watchos arm64 arm64-apple-watchos26.0 WATCHOS 8

WATCH_LIBRARY="$BUILD_ROOT/libish_aarch64_core-watchos.a"
xcrun lipo -create \
    "$BUILD_ROOT/core/watchos-arm64_32/libish_aarch64_core.a" \
    "$BUILD_ROOT/core/watchos-arm64/libish_aarch64_core.a" \
    -output "$WATCH_LIBRARY"
xcrun lipo "$BUILD_ROOT/core/iphoneos-arm64/libish_aarch64_core.a" \
    -verify_arch arm64
xcrun lipo "$WATCH_LIBRARY" -verify_arch arm64_32 arm64

for library in libish.a libish_emu.a libfakefs.a; do
    watch_library="$BUILD_ROOT/${library%.a}-watchos.a"
    xcrun lipo -create \
        "$BUILD_ROOT/full/watchos-arm64_32/$library" \
        "$BUILD_ROOT/full/watchos-arm64/$library" \
        -output "$watch_library"
    xcrun lipo "$BUILD_ROOT/full/iphoneos-arm64/$library" \
        -verify_arch arm64
    xcrun lipo "$watch_library" -verify_arch arm64_32 arm64
    xcrun lipo -info "$watch_library"
done

echo "==> Apple core 与完整库架构验证完成"
xcrun lipo -info "$BUILD_ROOT/core/iphoneos-arm64/libish_aarch64_core.a"
xcrun lipo -info "$WATCH_LIBRARY"

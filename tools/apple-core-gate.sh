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

verify_backend_config() {
    local name=$1
    local build_dir=$2
    local expected_value=$3
    local config_header="$build_dir/aarch64-backend-config.h"
    local configured_backend

    configured_backend=$("$MESON" configure "$build_dir" |
        awk '$1 == "aarch64_backend" { print $2 }')
    if [[ "$configured_backend" != auto ]]; then
        echo "错误：${name} 的 Meson AArch64 后端选项不是 auto。" >&2
        exit 1
    fi

    if [[ ! -f "$config_header" ]]; then
        echo "错误：${name} 未生成 AArch64 后端配置头。" >&2
        exit 1
    fi
    if ! grep -Fqx \
            "#define ISH_AARCH64_BACKEND_THREADED_DEFAULT $expected_value" \
            "$config_header"; then
        echo "错误：${name} 的 AArch64 默认后端与 auto 选择不一致。" >&2
        exit 1
    fi
}

verify_backend_archive() {
    local name=$1
    local archive=$2
    local archive_members
    local archive_symbols
    local required_member
    local required_symbol

    archive_members=$("$AR" -t "$archive")
    for required_member in \
            guest_aarch64_execute.c.o \
            guest_aarch64_backend.c.o \
            guest_aarch64_runner.c.o \
            guest_aarch64_threaded.c.o; do
        if ! grep -Fqx "$required_member" <<< "$archive_members"; then
            echo "错误：${name} 缺少 AArch64 后端对象 ${required_member}。" >&2
            exit 1
        fi
    done

    archive_symbols=$(xcrun nm -g "$archive")
    for required_symbol in \
            aarch64_execute \
            aarch64_backend_default \
            aarch64_runner_init_backend \
            aarch64_threaded_execute; do
        if ! grep -Eq \
                "[[:space:]]T[[:space:]]+_${required_symbol}$" \
                <<< "$archive_symbols"; then
            echo "错误：${name} 未导出 ${required_symbol}。" >&2
            exit 1
        fi
    done
}

build_slice() {
    local name=$1
    local sdk=$2
    local arch=$3
    local target=$4
    local platform=$5
    local word_bytes=$6
    local expected_minos=$7
    local expected_backend=$8
    local core_cross_file="$BUILD_ROOT/cross-core-$name.ini"
    local core_build_dir="$BUILD_ROOT/core/$name"
    local full_cross_file="$BUILD_ROOT/cross-full-$name.ini"
    local full_build_dir="$BUILD_ROOT/full/$name"
    local expected_backend_value

    case "$expected_backend" in
        threaded)
            expected_backend_value=1
            ;;
        c)
            expected_backend_value=0
            ;;
        *)
            echo "错误：${name} 声明了未知的 AArch64 后端 ${expected_backend}。" >&2
            exit 1
            ;;
    esac

    echo "==> 严格构建 ${name} 的 AArch64 core（${target}）"
    write_cross_file "$core_cross_file" "$sdk" "$arch" "$target" strict
    if [[ -d "$core_build_dir/meson-private" ]]; then
        "$MESON" setup --reconfigure "$core_build_dir" "$ROOT" \
            --cross-file "$core_cross_file" -Dcore_only=true \
            -Daarch64_backend=auto --buildtype=release
    else
        "$MESON" setup "$core_build_dir" "$ROOT" \
            --cross-file "$core_cross_file" -Dcore_only=true \
            -Daarch64_backend=auto --buildtype=release
    fi
    verify_backend_config "${name} core" "$core_build_dir" \
        "$expected_backend_value"
    "$NINJA" -C "$core_build_dir" \
        libish_aarch64_core.a aarch64_core_link_smoke
    verify_backend_archive "${name} core 归档" \
        "$core_build_dir/libish_aarch64_core.a"

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
        "$MESON" setup --reconfigure "$full_build_dir" "$ROOT" \
            --cross-file "$full_cross_file" -Dcore_only=false \
            -Dkernel=ish -Dengine=asbestos -Daarch64_backend=auto \
            --buildtype=release
    else
        "$MESON" setup "$full_build_dir" "$ROOT" \
            --cross-file "$full_cross_file" -Dcore_only=false \
            -Dkernel=ish -Dengine=asbestos -Daarch64_backend=auto \
            --buildtype=release
    fi
    verify_backend_config "${name} 完整构建" "$full_build_dir" \
        "$expected_backend_value"
    "$NINJA" -C "$full_build_dir" \
        libish.a libish_emu.a libfakefs.a \
        darwin_platform_link_smoke apple_runtime_link_smoke \
        apple_runtime_force_link_smoke
    verify_backend_archive "${name} libish.a" "$full_build_dir/libish.a"

    local abi_probe="$full_build_dir/apple-abi-probe.o"
    "$CLANG" -target "$target" -isysroot "$sysroot" -I"$ROOT" \
        -std=gnu11 -Wall -Wextra -Werror \
        -DEXPECTED_APPLE_WORD_BYTES="$word_bytes" \
        -c "$ROOT/tools/apple-abi-probe.c" -o "$abi_probe"

    local backend_probe="$full_build_dir/apple-aarch64-backend-probe.o"
    "$CLANG" -target "$target" -isysroot "$sysroot" \
        -I"$full_build_dir" -I"$ROOT" -DISH_GUEST_AARCH64=1 \
        -std=gnu11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
        -Wshorten-64-to-32 -Wpointer-to-int-cast -Wint-to-pointer-cast \
        -Wcast-align -DEXPECTED_APPLE_WORD_BYTES="$word_bytes" \
        -DEXPECTED_AARCH64_THREADED_DEFAULT="$expected_backend_value" \
        -c "$ROOT/tools/apple-aarch64-backend-probe.c" -o "$backend_probe"

    local rootfs_seed_object="$full_build_dir/apple-rootfs-seed-strict.o"
    "$CLANG" -target "$target" -isysroot "$sysroot" -I"$ROOT" \
        -std=gnu11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
        -Wshorten-64-to-32 -Wpointer-to-int-cast -Wint-to-pointer-cast \
        -Wcast-align -c "$ROOT/platform/apple-rootfs-seed.c" \
        -o "$rootfs_seed_object"

    local library
    for library in libish.a libish_emu.a libfakefs.a; do
        file "$full_build_dir/$library"
        xcrun lipo "$full_build_dir/$library" -verify_arch "$arch"
    done
    file "$full_build_dir/darwin_platform_link_smoke" \
        "$full_build_dir/apple_runtime_link_smoke" \
        "$full_build_dir/apple_runtime_force_link_smoke" \
        "$abi_probe" "$backend_probe" "$rootfs_seed_object"

    local fakefs_members
    local fakefs_symbols
    local runtime_symbols
    fakefs_members=$("$AR" -t "$full_build_dir/libfakefs.a")
    if ! grep -Fqx 'platform_apple-rootfs-seed.c.o' \
            <<< "$fakefs_members"; then
        echo "错误：${name} 的 libfakefs.a 缺少 Apple rootfs 安装器对象。" >&2
        exit 1
    fi
    fakefs_symbols=$(xcrun nm -g "$full_build_dir/libfakefs.a")
    if ! grep -Eq \
            '[[:space:]]T[[:space:]]+_ish_apple_rootfs_seed_install$' \
            <<< "$fakefs_symbols"; then
        echo "错误：${name} 的 libfakefs.a 未导出 Apple rootfs 安装器。" >&2
        exit 1
    fi
    runtime_symbols=$(xcrun nm -g \
        "$full_build_dir/apple_runtime_link_smoke")
    if ! grep -Eq \
            '[[:space:]]T[[:space:]]+_ish_apple_rootfs_seed_install$' \
            <<< "$runtime_symbols"; then
        echo "错误：${name} 的普通 runtime consumer 未抽取 rootfs 安装器。" >&2
        exit 1
    fi

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
    undefined_symbols=$(xcrun nm -u \
        "$full_build_dir/libish.a" \
        "$full_build_dir/libish_emu.a" \
        "$full_build_dir/libfakefs.a" \
        "$full_build_dir/darwin_platform_link_smoke" \
        "$full_build_dir/apple_runtime_link_smoke" \
        "$full_build_dir/apple_runtime_force_link_smoke")
    if grep -Eq '(^|[[:space:]])_host_info$' <<< "$undefined_symbols"; then
        echo "错误：${name} 仍引用 watchOS 禁用的 host_info。" >&2
        exit 1
    fi

    local executable
    local build_info
    for executable in darwin_platform_link_smoke apple_runtime_link_smoke \
            apple_runtime_force_link_smoke; do
        build_info=$(xcrun vtool -show-build "$full_build_dir/$executable")
        if ! grep -q "platform $platform" <<< "$build_info"; then
            echo "错误：${name} 的 ${executable} 平台不是 ${platform}。" >&2
            exit 1
        fi
        if ! grep -q "minos $expected_minos" <<< "$build_info"; then
            echo "错误：${name} 的 ${executable} 最低系统版本不是 ${expected_minos}。" >&2
            exit 1
        fi
    done

    if [[ "$arch" == arm64_32 || "$arch" == arm64 ]]; then
        local entry_object="$full_build_dir/libish_emu.a.p/asbestos_gadgets-aarch64_entry.S.o"
        local control_object="$full_build_dir/libish_emu.a.p/asbestos_gadgets-aarch64_control.S.o"
        local pointer_register=x8
        if [[ "$arch" == arm64_32 ]]; then
            pointer_register=w8
        fi

        local poked_offset
        local last_block_offset
        local entry_disassembly
        local control_disassembly
        poked_offset=$(awk '$2 == "CPU_poked_ptr" { print $3 }' \
            "$full_build_dir/cpu-offsets.h")
        last_block_offset=$(awk '$2 == "LOCAL_last_block" { print $3 }' \
            "$full_build_dir/cpu-offsets.h")
        entry_disassembly=$(xcrun otool -tvV "$entry_object")
        control_disassembly=$(xcrun otool -tvV "$control_object")
        local poked_hex
        local last_block_hex
        printf -v poked_hex '0x%x' "$poked_offset"
        printf -v last_block_hex '0x%x' "$last_block_offset"
        if ! grep -Eq "ldr[[:space:]]+${pointer_register}, \\[x1, #${poked_hex}\\]" \
                <<< "$entry_disassembly"; then
            echo "错误：${name} 未按宿主指针宽度读取 poked_ptr。" >&2
            exit 1
        fi
        if ! grep -Eq "str[[:space:]]+${pointer_register}, \\[x1, #${last_block_hex}\\]" \
                <<< "$entry_disassembly$control_disassembly"; then
            echo "错误：${name} 未按宿主指针宽度写入 last_block。" >&2
            exit 1
        fi
        if ! grep -Eq 'str[[:space:]]+x28, \[x13, x12, lsl #3\]' \
                <<< "$control_disassembly" || \
                ! grep -Eq 'ldr[[:space:]]+x28, \[x13, x12, lsl #3\]' \
                <<< "$control_disassembly"; then
            echo "错误：${name} 的 fiber 返回缓存不再保持 64 位单元。" >&2
            exit 1
        fi
    fi

    if [[ "$arch" == arm64_32 ]]; then
        local relocation_dump
        relocation_dump=$(xcrun otool -rv \
            "$full_build_dir"/libish_emu.a.p/asbestos_gadgets-aarch64_*.S.o)
        local gadget_relocations
        gadget_relocations=$(grep -E \
            'False.*UNSIGND.*_gadget_' \
            <<< "$relocation_dump" || true)
        if [[ -z "$gadget_relocations" ]]; then
            echo "错误：${name} 没有可验证的 gadget 指针 relocation。" >&2
            exit 1
        fi
        if grep -Ev \
                'False[[:space:]]+long[[:space:]]+True[[:space:]]+UNSIGND' \
                <<< "$gadget_relocations"; then
            echo "错误：${name} 仍存在非 4 字节 gadget 指针 relocation。" >&2
            exit 1
        fi
    fi

    if [[ "$name" == iphoneos-arm64 ]]; then
        local arm64e_object="$core_build_dir/aarch64-threaded-arm64e.o"
        local arm64e_file
        local arm64e_disassembly

        echo "==> 编译 iOS arm64e 的 threaded 指针认证证据"
        "$CLANG" -target arm64e-apple-ios15.0 -isysroot "$sysroot" \
            -I"$ROOT" -DISH_GUEST_AARCH64=1 -std=gnu11 -O2 \
            -Wall -Wextra -Werror -Wconversion -Wsign-conversion \
            -Wshorten-64-to-32 -Wpointer-to-int-cast \
            -Wint-to-pointer-cast -Wcast-align \
            -c "$ROOT/guest/aarch64/threaded.c" -o "$arm64e_object"
        arm64e_file=$(file "$arm64e_object")
        if ! grep -Eq 'Mach-O 64-bit.*arm64e' <<< "$arm64e_file"; then
            echo "错误：iOS threaded 严格编译产物不是 arm64e 对象。" >&2
            exit 1
        fi
        arm64e_disassembly=$(xcrun otool -tvV "$arm64e_object")
        if ! grep -Eq '[[:space:]]paciza[[:space:]]' \
                <<< "$arm64e_disassembly" || \
                ! grep -Eq '[[:space:]]blraaz[[:space:]]' \
                <<< "$arm64e_disassembly"; then
            echo "错误：iOS arm64e threaded 间接调用缺少预期的指针认证指令。" >&2
            exit 1
        fi
        echo "$arm64e_file"
    fi
}

if [[ "${APPLE_SKIP_IOS:-0}" != 1 ]]; then
    build_slice iphoneos-arm64 iphoneos arm64 \
        arm64-apple-ios15.0 IOS 8 15.0 threaded
fi
build_slice watchos-arm64_32 watchos arm64_32 \
    arm64_32-apple-watchos10.0 WATCHOS 4 10.0 threaded
build_slice watchos-arm64 watchos arm64 \
    arm64-apple-watchos26.0 WATCHOS 8 26.0 threaded
build_slice watchsimulator-arm64 watchsimulator arm64 \
    arm64-apple-watchos10.0-simulator WATCHOSSIMULATOR 8 10.0 threaded
build_slice watchsimulator-x86_64 watchsimulator x86_64 \
    x86_64-apple-watchos10.0-simulator WATCHOSSIMULATOR 8 10.0 c

UNIVERSAL_ROOT="$BUILD_ROOT/universal"
XCFRAMEWORK_ROOT="$BUILD_ROOT/xcframeworks"
mkdir -p "$UNIVERSAL_ROOT/watchos" "$UNIVERSAL_ROOT/watchsimulator" \
    "$XCFRAMEWORK_ROOT"

for library in libish_aarch64_core.a; do
    xcrun lipo -create \
        "$BUILD_ROOT/core/watchos-arm64_32/$library" \
        "$BUILD_ROOT/core/watchos-arm64/$library" \
        -output "$UNIVERSAL_ROOT/watchos/$library"
    xcrun lipo -create \
        "$BUILD_ROOT/core/watchsimulator-arm64/$library" \
        "$BUILD_ROOT/core/watchsimulator-x86_64/$library" \
        -output "$UNIVERSAL_ROOT/watchsimulator/$library"
    xcrun lipo "$UNIVERSAL_ROOT/watchos/$library" \
        -verify_arch arm64_32 arm64
    xcrun lipo "$UNIVERSAL_ROOT/watchsimulator/$library" \
        -verify_arch arm64 x86_64
done

for library in libish.a libish_emu.a libfakefs.a; do
    xcrun lipo -create \
        "$BUILD_ROOT/full/watchos-arm64_32/$library" \
        "$BUILD_ROOT/full/watchos-arm64/$library" \
        -output "$UNIVERSAL_ROOT/watchos/$library"
    xcrun lipo -create \
        "$BUILD_ROOT/full/watchsimulator-arm64/$library" \
        "$BUILD_ROOT/full/watchsimulator-x86_64/$library" \
        -output "$UNIVERSAL_ROOT/watchsimulator/$library"
    xcrun lipo "$UNIVERSAL_ROOT/watchos/$library" \
        -verify_arch arm64_32 arm64
    xcrun lipo "$UNIVERSAL_ROOT/watchsimulator/$library" \
        -verify_arch arm64 x86_64

    xcframework="$XCFRAMEWORK_ROOT/${library%.a}.xcframework"
    rm -rf "$xcframework"
    xcodebuild -create-xcframework \
        -library "$UNIVERSAL_ROOT/watchos/$library" \
        -library "$UNIVERSAL_ROOT/watchsimulator/$library" \
        -output "$xcframework"
    plutil -lint "$xcframework/Info.plist"
    device_xc_library="$xcframework/watchos-arm64_arm64_32/$library"
    simulator_xc_library="$xcframework/watchos-arm64_x86_64-simulator/$library"
    if [[ ! -f "$device_xc_library" || ! -f "$simulator_xc_library" ]]; then
        echo "错误：${library} XCFramework 缺少 device 或 simulator 变体。" >&2
        exit 1
    fi
    xcrun lipo "$device_xc_library" -verify_arch arm64_32 arm64
    xcrun lipo "$simulator_xc_library" -verify_arch arm64 x86_64
done

cp "$UNIVERSAL_ROOT/watchos/libish_aarch64_core.a" \
    "$BUILD_ROOT/libish_aarch64_core-watchos.a"
for library in libish.a libish_emu.a libfakefs.a; do
    cp "$UNIVERSAL_ROOT/watchos/$library" \
        "$BUILD_ROOT/${library%.a}-watchos.a"
done

if [[ "${APPLE_SKIP_IOS:-0}" != 1 ]]; then
    xcrun lipo "$BUILD_ROOT/core/iphoneos-arm64/libish_aarch64_core.a" \
        -verify_arch arm64
    for library in libish.a libish_emu.a libfakefs.a; do
        xcrun lipo "$BUILD_ROOT/full/iphoneos-arm64/$library" \
            -verify_arch arm64
    done
fi

echo "==> Apple core、完整消费者与 XCFramework 验证完成"
xcrun lipo -info "$UNIVERSAL_ROOT/watchos/libish.a"
xcrun lipo -info "$UNIVERSAL_ROOT/watchsimulator/libish.a"

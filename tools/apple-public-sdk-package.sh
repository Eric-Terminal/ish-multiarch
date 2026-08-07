#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
BUILD_ROOT=${1:-"$ROOT/build-apple-core"}
PUBLIC_ROOT="$BUILD_ROOT/public-sdk"
XCFRAMEWORK="$BUILD_ROOT/xcframeworks/iSHApple.xcframework"
HEADERS="$ROOT/sdk/iSHApple/Headers"
CONSUMER="$ROOT/sdk/iSHApple/Tests/PublicModuleConsumer.m"
PUBLIC_SYMBOLS="$ROOT/sdk/iSHApple/PublicSymbols.txt"

CLANG=$(xcrun --find clang)
LD_CLASSIC=$(xcrun --find ld-classic)
LIBTOOL=$(xcrun --find libtool)
NMEDIT=$(xcrun --find nmedit)

merge_thin_slice() {
    local name=$1
    local sdk=$2
    local arch=$3
    local platform=$4
    local minos=$5
    local source="$BUILD_ROOT/full/$name"
    local destination="$PUBLIC_ROOT/thin/$name"
    mkdir -p "$destination"
    local library
    for library in libish.a libish_emu.a libfakefs.a; do
        if [[ ! -f "$source/$library" ]]; then
            echo "错误：公共 SDK 缺少 ${name}/${library}。" >&2
            exit 1
        fi
    done
    local unscoped="$destination/libiSHApple-unscoped.a"
    "$LIBTOOL" -static -D \
        -o "$unscoped" \
        "$source/libish.a" \
        "$source/libish_emu.a" \
        "$source/libfakefs.a"

    # 先解析内部跨成员引用，再局部化非公共定义；-d 同时收敛 tentative
    # definitions，避免它们作为 common symbol 留在外部链接命名空间。
    local sysroot
    local sdk_version
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
    sdk_version=$(xcrun --sdk "$sdk" --show-sdk-version)
    "$LD_CLASSIC" \
        -r -d \
        -arch "$arch" \
        -platform_version "$platform" "$minos" "$sdk_version" \
        -syslibroot "$sysroot" \
        -all_load "$unscoped" \
        -o "$destination/libiSHApple-combined.o"
    "$NMEDIT" \
        -s "$PUBLIC_SYMBOLS" \
        -o "$destination/libiSHApple-public.o" \
        "$destination/libiSHApple-combined.o"
    "$LIBTOOL" -static -D \
        -o "$destination/libiSHApple.a" \
        "$destination/libiSHApple-public.o"
}

verify_public_symbols() {
    local name=$1
    local archive=$2
    local symbols
    symbols=$(xcrun nm -g "$archive")
    local required
    for required in \
            ish_apple_runtime_start \
            ish_apple_runtime_start_v2 \
            ish_apple_runtime_current_phase \
            ish_apple_runtime_last_error \
            ish_apple_diagnostics_drain \
            ish_apple_diagnostics_clear \
            ish_apple_guest_file_stat \
            ish_apple_guest_file_list \
            ish_apple_guest_file_read \
            ish_apple_guest_file_write \
            ish_apple_guest_file_edit \
            ish_apple_guest_file_remove \
            ish_apple_guest_file_rename \
            ish_apple_guest_file_mkdir \
            ish_apple_mount_add \
            ish_apple_mount_remove \
            ish_apple_mount_list \
            ish_apple_mount_copy_guest_directory \
            ish_apple_mount_lease_acquire \
            ish_apple_mount_lease_retain \
            ish_apple_mount_lease_release \
            ish_apple_rootfs_install_seed \
            ish_apple_command_session_start \
            ish_apple_command_session_retain \
            ish_apple_command_session_release \
            ish_apple_command_session_write_stdin \
            ish_apple_command_session_close_stdin \
            ish_apple_command_session_interrupt \
            ish_apple_command_session_cancel \
            ish_apple_command_session_wait; do
        if ! grep -Eq \
                "[[:space:]]T[[:space:]]+_${required}$" \
                <<< "$symbols"; then
            echo "错误：${name} 未导出公共入口 ${required}。" >&2
            exit 1
        fi
    done
    if grep -Eq \
            '[[:space:]]T[[:space:]]+_ish_apple_command_session_test_' \
            <<< "$symbols"; then
        echo "错误：${name} 泄漏了命令会话测试入口。" >&2
        exit 1
    fi
    if ! diff -u \
            <(LC_ALL=C sort "$PUBLIC_SYMBOLS") \
            <(xcrun nm -g "$archive" |
                awk 'NF >= 3 && $2 != "U" { print $3 }' |
                LC_ALL=C sort -u); then
        echo "错误：${name} 含有公共清单之外的全局定义。" >&2
        exit 1
    fi
}

link_module_consumer() {
    local name=$1
    local sdk=$2
    local target=$3
    local archive=$4
    local headers=$5
    local sysroot
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
    mkdir -p "$PUBLIC_ROOT/consumers" "$PUBLIC_ROOT/module-cache/$name"
    "$CLANG" \
        -target "$target" \
        -isysroot "$sysroot" \
        -fmodules \
        -fmodules-cache-path="$PUBLIC_ROOT/module-cache/$name" \
        -fmodule-map-file="$headers/module.modulemap" \
        -I"$headers" \
        -Wall -Wextra -Werror \
        "$CONSUMER" "$archive" \
        -lm -ldl -lresolv -lsqlite3 \
        -o "$PUBLIC_ROOT/consumers/$name"
}

rm -rf "$PUBLIC_ROOT" "$XCFRAMEWORK"
mkdir -p \
    "$PUBLIC_ROOT/thin" \
    "$PUBLIC_ROOT/universal/iphoneos" \
    "$PUBLIC_ROOT/universal/iphonesimulator" \
    "$PUBLIC_ROOT/universal/watchos" \
    "$PUBLIC_ROOT/universal/watchsimulator" \
    "$(dirname "$XCFRAMEWORK")"

merge_thin_slice \
    iphoneos-arm64 iphoneos arm64 ios 15.0
merge_thin_slice \
    iphonesimulator-arm64 iphonesimulator arm64 ios-simulator 15.0
merge_thin_slice \
    iphonesimulator-x86_64 iphonesimulator x86_64 ios-simulator 15.0
merge_thin_slice \
    watchos-arm64_32 watchos arm64_32 watchos 10.0
merge_thin_slice \
    watchos-arm64 watchos arm64 watchos 26.0
merge_thin_slice \
    watchsimulator-arm64 watchsimulator arm64 watchos-simulator 10.0
merge_thin_slice \
    watchsimulator-x86_64 watchsimulator x86_64 watchos-simulator 10.0

cp "$PUBLIC_ROOT/thin/iphoneos-arm64/libiSHApple.a" \
    "$PUBLIC_ROOT/universal/iphoneos/libiSHApple.a"
xcrun lipo -create \
    "$PUBLIC_ROOT/thin/iphonesimulator-arm64/libiSHApple.a" \
    "$PUBLIC_ROOT/thin/iphonesimulator-x86_64/libiSHApple.a" \
    -output "$PUBLIC_ROOT/universal/iphonesimulator/libiSHApple.a"
xcrun lipo -create \
    "$PUBLIC_ROOT/thin/watchos-arm64_32/libiSHApple.a" \
    "$PUBLIC_ROOT/thin/watchos-arm64/libiSHApple.a" \
    -output "$PUBLIC_ROOT/universal/watchos/libiSHApple.a"
xcrun lipo -create \
    "$PUBLIC_ROOT/thin/watchsimulator-arm64/libiSHApple.a" \
    "$PUBLIC_ROOT/thin/watchsimulator-x86_64/libiSHApple.a" \
    -output "$PUBLIC_ROOT/universal/watchsimulator/libiSHApple.a"

xcrun lipo "$PUBLIC_ROOT/universal/iphoneos/libiSHApple.a" \
    -verify_arch arm64
xcrun lipo "$PUBLIC_ROOT/universal/iphonesimulator/libiSHApple.a" \
    -verify_arch arm64 x86_64
xcrun lipo "$PUBLIC_ROOT/universal/watchos/libiSHApple.a" \
    -verify_arch arm64_32 arm64
xcrun lipo "$PUBLIC_ROOT/universal/watchsimulator/libiSHApple.a" \
    -verify_arch arm64 x86_64

xcodebuild -create-xcframework \
    -library "$PUBLIC_ROOT/universal/iphoneos/libiSHApple.a" \
    -headers "$HEADERS" \
    -library "$PUBLIC_ROOT/universal/iphonesimulator/libiSHApple.a" \
    -headers "$HEADERS" \
    -library "$PUBLIC_ROOT/universal/watchos/libiSHApple.a" \
    -headers "$HEADERS" \
    -library "$PUBLIC_ROOT/universal/watchsimulator/libiSHApple.a" \
    -headers "$HEADERS" \
    -output "$XCFRAMEWORK"
plutil -lint "$XCFRAMEWORK/Info.plist"

IOS_DEVICE="$XCFRAMEWORK/ios-arm64"
IOS_SIMULATOR="$XCFRAMEWORK/ios-arm64_x86_64-simulator"
WATCH_DEVICE="$XCFRAMEWORK/watchos-arm64_arm64_32"
WATCH_SIMULATOR="$XCFRAMEWORK/watchos-arm64_x86_64-simulator"

for variant in \
        "$IOS_DEVICE" \
        "$IOS_SIMULATOR" \
        "$WATCH_DEVICE" \
        "$WATCH_SIMULATOR"; do
    if [[ ! -f "$variant/libiSHApple.a" || \
            ! -f "$variant/Headers/iSHApple.h" || \
            ! -f "$variant/Headers/module.modulemap" ]]; then
        echo "错误：公共 XCFramework 变体 ${variant##*/} 不完整。" >&2
        exit 1
    fi
    verify_public_symbols \
        "公共 XCFramework 变体 ${variant##*/}" \
        "$variant/libiSHApple.a"
done

xcrun lipo "$IOS_DEVICE/libiSHApple.a" -verify_arch arm64
xcrun lipo "$IOS_SIMULATOR/libiSHApple.a" \
    -verify_arch arm64 x86_64
xcrun lipo "$WATCH_DEVICE/libiSHApple.a" \
    -verify_arch arm64_32 arm64
xcrun lipo "$WATCH_SIMULATOR/libiSHApple.a" \
    -verify_arch arm64 x86_64

link_module_consumer \
    iphoneos-arm64 iphoneos arm64-apple-ios15.0 \
    "$IOS_DEVICE/libiSHApple.a" "$IOS_DEVICE/Headers"
link_module_consumer \
    iphonesimulator-arm64 iphonesimulator \
    arm64-apple-ios15.0-simulator \
    "$IOS_SIMULATOR/libiSHApple.a" "$IOS_SIMULATOR/Headers"
link_module_consumer \
    iphonesimulator-x86_64 iphonesimulator \
    x86_64-apple-ios15.0-simulator \
    "$IOS_SIMULATOR/libiSHApple.a" "$IOS_SIMULATOR/Headers"
link_module_consumer \
    watchos-arm64_32 watchos arm64_32-apple-watchos10.0 \
    "$WATCH_DEVICE/libiSHApple.a" "$WATCH_DEVICE/Headers"
link_module_consumer \
    watchos-arm64 watchos arm64-apple-watchos26.0 \
    "$WATCH_DEVICE/libiSHApple.a" "$WATCH_DEVICE/Headers"
link_module_consumer \
    watchsimulator-arm64 watchsimulator \
    arm64-apple-watchos10.0-simulator \
    "$WATCH_SIMULATOR/libiSHApple.a" "$WATCH_SIMULATOR/Headers"
link_module_consumer \
    watchsimulator-x86_64 watchsimulator \
    x86_64-apple-watchos10.0-simulator \
    "$WATCH_SIMULATOR/libiSHApple.a" "$WATCH_SIMULATOR/Headers"

"$ROOT/sdk/iSHApple/Tests/typecheck.sh" "$IOS_DEVICE/Headers"

echo "==> iSHApple 公共 XCFramework 与七切片模块消费者验证完成"

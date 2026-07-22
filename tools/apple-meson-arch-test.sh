#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/tools/apple-meson-arch.sh"

check_architecture() {
    local arch=$1
    local expected_family=$2
    local expected_cpu=$3

    apple_meson_architecture "$arch"
    if [[ "$APPLE_MESON_CPU_FAMILY" != "$expected_family" ||
            "$APPLE_MESON_CPU" != "$expected_cpu" ]]; then
        echo "Apple Meson 架构映射测试失败：$arch -> $APPLE_MESON_CPU_FAMILY/$APPLE_MESON_CPU" >&2
        exit 1
    fi
}

check_architecture arm64 aarch64 aarch64
check_architecture arm64_32 aarch64 arm64_32
check_architecture x86_64 x86_64 x86_64

actual=$(CURRENT_ARCH=arm64_32 ARCHS="arm64 x86_64" \
    apple_xcode_architectures)
if [[ "$actual" != arm64_32 ]]; then
    echo "Apple Xcode 当前架构优先级测试失败：$actual" >&2
    exit 1
fi

actual=$(CURRENT_ARCH=undefined_arch ARCHS="arm64 x86_64" \
    apple_xcode_architectures)
if [[ "$actual" != $'arm64\nx86_64' ]]; then
    echo "Apple Xcode 架构列表回退测试失败：$actual" >&2
    exit 1
fi

if (unset CURRENT_ARCH ARCHS; apple_xcode_architectures \
        >/dev/null 2>&1); then
    echo "Apple Xcode 缺失架构输入时应失败" >&2
    exit 1
fi

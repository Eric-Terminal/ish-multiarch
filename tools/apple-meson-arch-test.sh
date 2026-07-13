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

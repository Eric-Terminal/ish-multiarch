#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_ROOT=${ISH_WATCH_ARTIFACT_ROOT:-${1:-"$ROOT/build-apple-watch"}}
PACKAGE_MODE=${ISH_WATCH_PACKAGE_MODE:-build}

find_tool() {
    local name=$1
    local configured=$2
    local variable=$3
    if [[ -n "$configured" ]]; then
        printf '%s\n' "$configured"
        return
    fi

    local executable
    executable=$(command -v "$name" || true)
    if [[ -n "$executable" ]]; then
        printf '%s\n' "$executable"
        return
    fi

    local candidate
    for candidate in "$HOME"/.cache/uv/archive-v0/*/bin/"$name"; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    echo "错误：找不到 ${name}，请先安装，或通过 ${variable} 环境变量指定可执行文件。" >&2
    exit 1
}

case "$PACKAGE_MODE" in
    build)
        for tool_dir in "$HOME/.local/bin" /opt/homebrew/bin \
                /opt/homebrew/opt/llvm/bin /usr/local/bin; do
            if [[ -d "$tool_dir" ]]; then
                PATH="$tool_dir:$PATH"
            fi
        done
        export PATH
        export MESON
        export NINJA
        MESON=$(find_tool meson "${MESON:-}" MESON)
        NINJA=$(find_tool ninja "${NINJA:-}" NINJA)
        APPLE_SKIP_IOS=1 "$ROOT/tools/apple-core-gate.sh" "$BUILD_ROOT"
        ;;
    prebuilt)
        ;;
    *)
        echo "错误：不支持 Watch 核心打包模式 ${PACKAGE_MODE}。" >&2
        exit 1
        ;;
esac

case "${PLATFORM_NAME:-}" in
    watchos)
        platform=watchos
        ;;
    watchsimulator)
        platform=watchsimulator
        ;;
    "")
        if [[ "$PACKAGE_MODE" == prebuilt ]]; then
            echo "错误：预构建 Watch 核心时缺少 PLATFORM_NAME。" >&2
            exit 1
        fi
        exit 0
        ;;
    *)
        echo "错误：不支持从 ${PLATFORM_NAME} 打包 Watch 核心。" >&2
        exit 1
        ;;
esac

PRODUCT_DIR=${ISH_WATCH_PRODUCT_DIR:-${BUILT_PRODUCTS_DIR:-}}
if [[ -z "$PRODUCT_DIR" ]]; then
    if [[ "$PACKAGE_MODE" == prebuilt ]]; then
        echo "错误：预构建 Watch 核心时缺少产品目录。" >&2
        exit 1
    fi
    exit 0
fi

for library in libish.a libish_emu.a libfakefs.a; do
    if [[ ! -s "$BUILD_ROOT/universal/$platform/$library" ]]; then
        echo "错误：Watch 核心产物缺失：$BUILD_ROOT/universal/$platform/$library" >&2
        exit 1
    fi
done

mkdir -p "$PRODUCT_DIR"
for library in libish.a libish_emu.a libfakefs.a; do
    cp "$BUILD_ROOT/universal/$platform/$library" "$PRODUCT_DIR/$library"
done

echo "==> 已将 ${platform} 静态库复制到 ${PRODUCT_DIR}"

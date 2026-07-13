#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_ROOT=${ISH_WATCH_ARTIFACT_ROOT:-${1:-"$ROOT/build-apple-watch"}}

for tool_dir in "$HOME/.local/bin" /opt/homebrew/bin \
        /opt/homebrew/opt/llvm/bin /usr/local/bin; do
    if [[ -d "$tool_dir" ]]; then
        PATH="$tool_dir:$PATH"
    fi
done
export PATH

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

export MESON
export NINJA
MESON=$(find_tool meson "${MESON:-}" MESON)
NINJA=$(find_tool ninja "${NINJA:-}" NINJA)

APPLE_SKIP_IOS=1 "$ROOT/tools/apple-core-gate.sh" "$BUILD_ROOT"

case "${PLATFORM_NAME:-}" in
    watchos)
        platform=watchos
        ;;
    watchsimulator)
        platform=watchsimulator
        ;;
    "")
        exit 0
        ;;
    *)
        echo "错误：不支持从 ${PLATFORM_NAME} 打包 Watch 核心。" >&2
        exit 1
        ;;
esac

PRODUCT_DIR=${ISH_WATCH_PRODUCT_DIR:-${BUILT_PRODUCTS_DIR:-}}
if [[ -z "$PRODUCT_DIR" ]]; then
    exit 0
fi

mkdir -p "$PRODUCT_DIR"
for library in libish.a libish_emu.a libfakefs.a; do
    cp "$BUILD_ROOT/universal/$platform/$library" "$PRODUCT_DIR/$library"
done

echo "==> 已将 ${platform} 静态库复制到 ${PRODUCT_DIR}"

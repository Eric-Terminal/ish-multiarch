#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TEMP_ROOT=${TMPDIR:-/tmp}
FIXTURE=$(mktemp -d "${TEMP_ROOT%/}/ish-watch-package.XXXXXX")
trap 'rm -rf "$FIXTURE"' EXIT
unset PLATFORM_NAME BUILT_PRODUCTS_DIR ISH_WATCH_PRODUCT_DIR \
    ISH_WATCH_PACKAGE_MODE

PROJECT="$FIXTURE/project"
ARTIFACTS="$FIXTURE/artifacts"
PRODUCTS="$FIXTURE/products"
SOURCE="$ARTIFACTS/universal/watchsimulator"
GATE_MARKER="$FIXTURE/gate-called"
mkdir -p "$PROJECT/tools" "$SOURCE"
ln -s "$ROOT/tools/apple-watch-package.sh" \
    "$PROJECT/tools/apple-watch-package.sh"
printf '%s\n' \
    '#!/bin/bash' \
    ': > "$ISH_WATCH_GATE_MARKER"' \
    'exit 97' > "$PROJECT/tools/apple-core-gate.sh"
chmod +x "$PROJECT/tools/apple-core-gate.sh"

for library in libish.a libish_emu.a libfakefs.a; do
    printf 'fixture:%s\n' "$library" > "$SOURCE/$library"
done

PLATFORM_NAME=watchsimulator \
ISH_WATCH_ARTIFACT_ROOT="$ARTIFACTS" \
ISH_WATCH_PRODUCT_DIR="$PRODUCTS" \
ISH_WATCH_PACKAGE_MODE=prebuilt \
ISH_WATCH_GATE_MARKER="$GATE_MARKER" \
MESON="$FIXTURE/不得调用-meson" \
NINJA="$FIXTURE/不得调用-ninja" \
    "$PROJECT/tools/apple-watch-package.sh"

for library in libish.a libish_emu.a libfakefs.a; do
    cmp "$SOURCE/$library" "$PRODUCTS/$library"
done
if [[ -e "$GATE_MARKER" ]]; then
    echo "错误：预构建模式不应再次运行 Watch 核心门禁。" >&2
    exit 1
fi

rm -rf "$PRODUCTS"
rm "$SOURCE/libish_emu.a"
if output=$(
    PLATFORM_NAME=watchsimulator \
    ISH_WATCH_ARTIFACT_ROOT="$ARTIFACTS" \
    ISH_WATCH_PRODUCT_DIR="$PRODUCTS" \
    ISH_WATCH_PACKAGE_MODE=prebuilt \
    ISH_WATCH_GATE_MARKER="$GATE_MARKER" \
    MESON="$FIXTURE/不得调用-meson" \
    NINJA="$FIXTURE/不得调用-ninja" \
        "$PROJECT/tools/apple-watch-package.sh" 2>&1
); then
    echo "错误：缺少预构建 Watch 核心时仍然打包成功。" >&2
    exit 1
fi

grep -Fq "错误：Watch 核心产物缺失：$SOURCE/libish_emu.a" <<< "$output"
if [[ -e "$PRODUCTS" ]]; then
    echo "错误：产物不完整时不应创建 Watch 产品目录。" >&2
    exit 1
fi
if [[ -e "$GATE_MARKER" ]]; then
    echo "错误：预构建产物缺失时不应回退到 Watch 核心门禁。" >&2
    exit 1
fi
printf 'fixture:%s\n' libish_emu.a > "$SOURCE/libish_emu.a"

if output=$(
    PLATFORM_NAME=watchsimulator \
    ISH_WATCH_ARTIFACT_ROOT="$ARTIFACTS" \
    ISH_WATCH_PACKAGE_MODE=prebuilt \
    ISH_WATCH_GATE_MARKER="$GATE_MARKER" \
        "$PROJECT/tools/apple-watch-package.sh" 2>&1
); then
    echo "错误：预构建模式缺少产品目录时仍然成功。" >&2
    exit 1
fi
grep -Fq "错误：预构建 Watch 核心时缺少产品目录。" <<< "$output"

if output=$(
    ISH_WATCH_ARTIFACT_ROOT="$ARTIFACTS" \
    ISH_WATCH_PRODUCT_DIR="$PRODUCTS" \
    ISH_WATCH_PACKAGE_MODE=prebuilt \
    ISH_WATCH_GATE_MARKER="$GATE_MARKER" \
        "$PROJECT/tools/apple-watch-package.sh" 2>&1
); then
    echo "错误：预构建模式缺少平台时仍然成功。" >&2
    exit 1
fi
grep -Fq "错误：预构建 Watch 核心时缺少 PLATFORM_NAME。" <<< "$output"

if [[ -e "$GATE_MARKER" ]]; then
    echo "错误：预构建参数错误时不应运行 Watch 核心门禁。" >&2
    exit 1
fi

if output=$(
    PLATFORM_NAME=watchsimulator \
    ISH_WATCH_ARTIFACT_ROOT="$ARTIFACTS" \
    ISH_WATCH_PRODUCT_DIR="$PRODUCTS" \
    ISH_WATCH_PACKAGE_MODE=invalid \
    ISH_WATCH_GATE_MARKER="$GATE_MARKER" \
        "$PROJECT/tools/apple-watch-package.sh" 2>&1
); then
    echo "错误：未知 Watch 核心打包模式仍然成功。" >&2
    exit 1
fi
grep -Fq "错误：不支持 Watch 核心打包模式 invalid。" <<< "$output"
if [[ -e "$GATE_MARKER" ]]; then
    echo "错误：未知打包模式不应运行 Watch 核心门禁。" >&2
    exit 1
fi

if ISH_WATCH_ARTIFACT_ROOT="$ARTIFACTS" \
    ISH_WATCH_GATE_MARKER="$GATE_MARKER" \
    MESON="$FIXTURE/不得调用-meson" \
    NINJA="$FIXTURE/不得调用-ninja" \
        "$PROJECT/tools/apple-watch-package.sh" >/dev/null 2>&1; then
    echo "错误：默认构建模式的 fixture gate 应返回失败。" >&2
    exit 1
fi
if [[ ! -e "$GATE_MARKER" ]]; then
    echo "错误：默认构建模式没有运行 Watch 核心门禁。" >&2
    exit 1
fi

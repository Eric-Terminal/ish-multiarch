#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/tools/apple-lipo.sh"

CHECKED_ARCHS=()
EXPECTED_ARCHIVE='/构建路径 含空格/静态库/libiSHApple.a'

# 模拟只接受一个架构参数的工具链，同时核对含空格路径不会被拆成多个输入。
xcrun() {
    if [[ "$#" != 4 || "$1" != lipo || "$2" != "$EXPECTED_ARCHIVE" ||
            "$3" != -verify_arch ]]; then
        echo "错误：架构检查必须只传入一个文件和一个架构。" >&2
        return 97
    fi
    CHECKED_ARCHS+=("$4")
    case "$4" in
        arm64|arm64_32|x86_64) return 0 ;;
        *) return 1 ;;
    esac
}

apple_verify_archive_architectures "$EXPECTED_ARCHIVE" arm64_32 arm64 x86_64
if [[ "${CHECKED_ARCHS[*]}" != 'arm64_32 arm64 x86_64' ]]; then
    echo "错误：没有检查全部请求架构。" >&2
    exit 1
fi

CHECKED_ARCHS=()
if apple_verify_archive_architectures "$EXPECTED_ARCHIVE" arm64 missing x86_64; then
    echo "错误：缺少中间架构时仍然校验成功。" >&2
    exit 1
fi
if [[ "${CHECKED_ARCHS[*]}" != 'arm64 missing' ]]; then
    echo "错误：缺少架构后没有立即停止校验。" >&2
    exit 1
fi

echo "Apple 架构逐项校验测试通过。"

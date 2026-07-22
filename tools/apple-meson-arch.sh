#!/bin/bash

# 将 Xcode 架构名拆成 Meson 的架构家族与具体 CPU。
apple_meson_architecture() {
    case "$1" in
        arm64)
            APPLE_MESON_CPU_FAMILY=aarch64
            APPLE_MESON_CPU=aarch64
            ;;
        arm64_32)
            APPLE_MESON_CPU_FAMILY=aarch64
            APPLE_MESON_CPU=arm64_32
            ;;
        *)
            APPLE_MESON_CPU_FAMILY=$1
            APPLE_MESON_CPU=$1
            ;;
    esac
}

# PBXLegacyTarget 通常只提供 ARCHS；逐架构构建阶段则优先提供 CURRENT_ARCH。
apple_xcode_architectures() {
    if [[ -n ${CURRENT_ARCH:-} && ${CURRENT_ARCH} != undefined_arch ]]; then
        printf '%s\n' "$CURRENT_ARCH"
        return
    fi

    if [[ -z ${ARCHS:-} ]]; then
        echo "错误：Xcode 没有提供 CURRENT_ARCH 或 ARCHS。" >&2
        return 1
    fi

    local architecture
    for architecture in $ARCHS; do
        printf '%s\n' "$architecture"
    done
}

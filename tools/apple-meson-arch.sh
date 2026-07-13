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

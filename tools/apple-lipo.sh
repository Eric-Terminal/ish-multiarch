#!/bin/bash

# 每次只传入一个架构，避免云端 lipo 对多架构参数的解析差异，仍要求所有切片齐全。
apple_verify_archive_architectures() {
    local archive=$1
    shift
    local arch
    for arch in "$@"; do
        xcrun lipo "$archive" -verify_arch "$arch" || return "$?"
    done
}

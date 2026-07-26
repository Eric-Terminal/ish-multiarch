#!/bin/bash

# 为 Apple 静态归档与 C 编译时间宏建立稳定的构建环境。
ish_reproducible_build_environment() {
    local source_root=$1
    local commit_epoch
    local git_executable
    local git_root
    local physical_git_root
    local physical_source_root
    local -a git_command

    if ! physical_source_root=$(cd "$source_root" && pwd -P); then
        echo "错误：无法解析可复现构建的源码根目录。" >&2
        return 1
    fi

    ZERO_AR_DATE=1
    if [[ -z ${SOURCE_DATE_EPOCH:-} ]]; then
        git_executable=$(command -v git || true)
        git_command=(env -i
            "GIT_CONFIG_NOSYSTEM=1"
            "GIT_CONFIG_GLOBAL=/dev/null"
            "HOME=/"
            "XDG_CONFIG_HOME=/nonexistent"
            "PATH=${PATH:-/usr/bin:/bin}"
            "LC_ALL=C")
        if [[ -n ${DEVELOPER_DIR:-} ]]; then
            git_command+=("DEVELOPER_DIR=$DEVELOPER_DIR")
        fi
        git_command+=("$git_executable" --no-replace-objects)

        if [[ -n "$git_executable" ]] &&
                git_root=$("${git_command[@]}" -C "$physical_source_root" \
                    rev-parse \
                    --show-toplevel 2>/dev/null) &&
                physical_git_root=$(cd "$git_root" && pwd -P) &&
                [[ "$physical_git_root" == "$physical_source_root" ]] &&
                commit_epoch=$("${git_command[@]}" \
                    -C "$physical_source_root" log -1 \
                    --format=%ct HEAD 2>/dev/null) &&
                [[ -n "$commit_epoch" ]]; then
            SOURCE_DATE_EPOCH=$commit_epoch
        else
            SOURCE_DATE_EPOCH=0
        fi
    fi

    case "$SOURCE_DATE_EPOCH" in
        ""|*[!0-9]*)
            echo "错误：SOURCE_DATE_EPOCH 必须是非负十进制整数。" >&2
            return 1
            ;;
    esac

    export SOURCE_DATE_EPOCH ZERO_AR_DATE
}

#!/bin/bash

# 旧 builddir 不会在携带未知 -D 参数时自动加载新加入的项目选项。
apple_meson_refresh_project_options() {
    local meson=$1
    local build_dir=$2
    local source_dir=$3
    shift 3

    local build_options
    if ! build_options=$("$meson" introspect --buildoptions "$build_dir"); then
        echo "错误：无法读取 Meson 构建目录 ${build_dir}，拒绝破坏性重建。" >&2
        return 1
    fi

    local option
    local missing_option=
    for option in "$@"; do
        if ! /usr/bin/grep -Eq \
                "\"name\"[[:space:]]*:[[:space:]]*\"${option}\"" \
                <<< "$build_options"; then
            missing_option=$option
            break
        fi
    done
    if [[ -z "$missing_option" ]]; then
        return 0
    fi

    echo "==> 刷新旧 Meson 构建目录的项目选项（缺少 ${missing_option}）"
    if ! "$meson" setup --reconfigure "$build_dir" "$source_dir"; then
        echo "错误：Meson 无法刷新构建目录 ${build_dir} 的项目选项。" >&2
        return 1
    fi

    if ! build_options=$(
            "$meson" introspect --buildoptions "$build_dir"
        ); then
        echo "错误：无法核验刷新后的 Meson 构建目录 ${build_dir}。" >&2
        return 1
    fi
    for option in "$@"; do
        if ! /usr/bin/grep -Eq \
                "\"name\"[[:space:]]*:[[:space:]]*\"${option}\"" \
                <<< "$build_options"; then
            echo "错误：Meson 刷新后仍缺少项目选项 ${option}。" >&2
            return 1
        fi
    done
    return 0
}

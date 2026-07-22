#!/bin/bash
set -euo pipefail

# Try to figure out the user's PATH to pick up their installed utilities.
export PATH="$PATH:$(sudo -u "$USER" -i printenv PATH)"
source "$SRCROOT/tools/apple-meson-arch.sh"

architectures=()
while IFS= read -r architecture; do
    [[ -n $architecture ]] && architectures+=("$architecture")
done < <(apple_xcode_architectures)
if (( ${#architectures[@]} == 0 )); then
    echo "错误：没有可供 Ninja 构建的 Xcode 架构。" >&2
    exit 1
fi

for architecture in "${architectures[@]}"; do
    ninja -C "$MESON_BUILD_DIR/arch-$architecture" "$@"
done

publication_outputs=()
publication_temporaries=()
cleanup() {
    local temporary
    for temporary in "${publication_temporaries[@]}"; do
        [[ -n $temporary ]] && rm -f "$temporary"
    done
}
trap cleanup EXIT

# 所有切片成功后再生成临时归档，任何失败都不会破坏上一次的顶层产物。
for target in "$@"; do
    [[ $target == *.a ]] || continue

    output="$MESON_BUILD_DIR/$target"
    mkdir -p "$(dirname "$output")"
    temporary="${output}.publishing.$$"
    rm -f "$temporary"

    slices=()
    for architecture in "${architectures[@]}"; do
        slice="$MESON_BUILD_DIR/arch-$architecture/$target"
        if [[ ! -f $slice ]]; then
            echo "错误：$architecture 构建没有生成 $target。" >&2
            exit 1
        fi
        slices+=("$slice")
    done

    if (( ${#slices[@]} == 1 )); then
        cp "${slices[0]}" "$temporary"
    else
        xcrun lipo -create "${slices[@]}" -output "$temporary"
    fi
    publication_outputs+=("$output")
    publication_temporaries+=("$temporary")
done

for index in "${!publication_outputs[@]}"; do
    output=${publication_outputs[$index]}
    temporary=${publication_temporaries[$index]}
    if [[ -f $output ]] && cmp -s "$temporary" "$output"; then
        rm -f "$temporary"
    else
        mv -f "$temporary" "$output"
    fi
    publication_temporaries[$index]=
done

# Linux Xcode target 仍从顶层 MESON_BUILD_DIR 查找生成头；复用首个切片的完整生成树。
for target in "$@"; do
    if [[ $target == deps/liblinux.a ]]; then
        linux_headers_source="$MESON_BUILD_DIR/arch-${architectures[0]}/deps/linux"
        linux_headers="$MESON_BUILD_DIR/deps/linux"
        if [[ ! -d $linux_headers_source ]]; then
            echo "错误：Linux 构建没有生成头文件树。" >&2
            exit 1
        fi
        temporary_link="${linux_headers}.publishing.$$"
        rm -f "$temporary_link"
        ln -s "$linux_headers_source" "$temporary_link"
        rm -rf "$linux_headers"
        mv "$temporary_link" "$linux_headers"
        break
    fi
done

trap - EXIT

#!/bin/bash
set -euo pipefail

# Try to figure out the user's PATH to pick up their installed utilities.
export PATH="$PATH:$(sudo -u "$USER" -i printenv PATH)"
source "$SRCROOT/tools/apple-meson-arch.sh"

mkdir -p "$MESON_BUILD_DIR"

architectures=()
while IFS= read -r architecture; do
    [[ -n $architecture ]] && architectures+=("$architecture")
done < <(apple_xcode_architectures)
if (( ${#architectures[@]} == 0 )); then
    echo "错误：没有可供 Meson 配置的 Xcode 架构。" >&2
    exit 1
fi

export CC_FOR_BUILD="env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun clang"
export CC="$CC_FOR_BUILD" # compatibility with meson < 0.54.0

buildtype=debug
b_ndebug=false
if [[ ${CONFIGURATION:-} == Release ]]; then
    buildtype=debugoptimized
fi
b_sanitize=none
if [[ -n ${ENABLE_ADDRESS_SANITIZER:-} ]]; then
    b_sanitize=address
fi
log=${ISH_LOG:-}
log_handler=${ISH_LOGGER:-}
kernel=ish
if [[ -n ${ISH_KERNEL:-} ]]; then
    kernel=$ISH_KERNEL
fi
kconfig=""

write_cross_file() {
    local cross_file=$1
    local architecture=$2
    local temporary="${cross_file}.tmp.$$"

    apple_meson_architecture "$architecture"
    cat > "$temporary" <<-EOF
    [binaries]
    c = 'clang'
    ar = 'ar'

    [host_machine]
    system = 'darwin'
    cpu_family = '$APPLE_MESON_CPU_FAMILY'
    cpu = '$APPLE_MESON_CPU'
    endian = 'little'

    [built-in options]
    c_args = ['-arch', '$architecture']
    c_link_args = ['-arch', '$architecture']

    [properties]
    needs_exe_wrapper = true
EOF
    if [[ -f $cross_file ]] && cmp -s "$temporary" "$cross_file"; then
        rm -f "$temporary"
    else
        mv -f "$temporary" "$cross_file"
    fi
}

configure_architecture() {
    local architecture=$1
    local build_dir="$MESON_BUILD_DIR/arch-$architecture"
    local cross_file="$build_dir/cross.txt"
    local config
    local old_value
    local new_value
    local var

    mkdir -p "$build_dir"
    write_cross_file "$cross_file" "$architecture"

    if ! config=$(cd "$build_dir" && meson introspect --buildoptions 2>/dev/null); then
        set -x
        meson setup "$build_dir" "$SRCROOT" --cross-file "$cross_file"
        set +x
        config=$(cd "$build_dir" && meson introspect --buildoptions)
    fi

    for var in buildtype log b_ndebug b_sanitize log_handler kernel kconfig; do
        old_value=$(python3 -c "import sys, json; v = next(x['value'] for x in json.load(sys.stdin) if x['name'] == '$var'); print(str(v).lower() if isinstance(v, bool) else ','.join(v) if isinstance(v, list) else v)" <<< "$config")
        new_value=${!var}
        if [[ $old_value != "$new_value" ]]; then
            set -x
            meson configure "$build_dir" "-D$var=$new_value"
            set +x
        fi
    done
}

for architecture in "${architectures[@]}"; do
    configure_architecture "$architecture"
done

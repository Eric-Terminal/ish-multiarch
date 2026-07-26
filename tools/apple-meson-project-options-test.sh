#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
source "$ROOT/tools/apple-meson-project-options.sh"

if (( $# > 1 )); then
    echo "用法：$0 [meson]" >&2
    exit 2
fi
if (( $# == 1 )); then
    MESON=$1
fi
if [[ -z "${MESON:-}" ]]; then
    MESON=$(command -v meson || true)
fi
if [[ -z "$MESON" ]]; then
    echo "错误：找不到 meson，无法测试旧项目选项刷新。" >&2
    exit 1
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/ish-meson-options.XXXXXX")
cleanup() {
    /usr/bin/find "$test_root" -depth -delete
}
trap cleanup EXIT

source_dir="$test_root/source"
build_dir="$test_root/build"
broken_dir="$test_root/broken"
mkdir -p "$source_dir" "$broken_dir/meson-private"

cat > "$source_dir/meson.build" <<'EOF'
project('project-option-refresh-fixture', meson_version: '>=1.3')
EOF
"$MESON" setup "$build_dir" "$source_dir" >/dev/null

before=$("$MESON" configure "$build_dir")
if /usr/bin/awk '$1 == "fixture_option" { found = 1 }
        END { exit !found }' <<< "$before"; then
    echo "错误：初始 fixture 意外包含待新增项目选项。" >&2
    exit 1
fi

cat > "$source_dir/meson_options.txt" <<'EOF'
option('fixture_option', type: 'boolean', value: false)
EOF
touch "$build_dir/保留缓存"

apple_meson_refresh_project_options \
    "$MESON" "$build_dir" "$source_dir" fixture_option
if ! "$MESON" setup --reconfigure "$build_dir" "$source_dir" \
        -Dfixture_option=true >"$test_root/reconfigure.log" 2>&1; then
    /bin/cat "$test_root/reconfigure.log" >&2
    exit 1
fi

configured=$("$MESON" configure "$build_dir" |
    /usr/bin/awk '$1 == "fixture_option" { print $2 }')
if [[ "$configured" != true ]]; then
    echo "错误：新增项目选项没有采用请求值。" >&2
    exit 1
fi
if [[ ! -f "$build_dir/保留缓存" ]]; then
    echo "错误：刷新项目选项破坏了既有构建缓存。" >&2
    exit 1
fi

touch "$broken_dir/保留损坏现场"
if apple_meson_refresh_project_options \
        "$MESON" "$broken_dir" "$source_dir" fixture_option \
        >/dev/null 2>&1; then
    echo "错误：损坏的 Meson 构建目录应被拒绝。" >&2
    exit 1
fi
if [[ ! -f "$broken_dir/保留损坏现场" ]]; then
    echo "错误：拒绝损坏缓存时删除了原始现场。" >&2
    exit 1
fi

echo "Apple Meson 项目选项刷新测试通过"

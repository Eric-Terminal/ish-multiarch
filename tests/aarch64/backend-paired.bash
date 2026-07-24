#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "用法：$0 <只读规范 fakefs 种子>" >&2
    exit 2
fi

script_dir=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$script_dir/../.." && pwd)
workload=$script_dir/backend-paired-workload.sh
expected=$script_dir/backend-paired-expected.txt
rootfs_arg=$1

if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo "双后端差分门禁只能在原生 arm64 macOS 主机运行。" >&2
    exit 2
fi
if [[ ! -d $rootfs_arg || -L $rootfs_arg ]]; then
    echo "fakefs 种子不存在、不是目录或是符号链接：$rootfs_arg" >&2
    exit 2
fi
if [[ ! -f $workload || ! -f $expected ]]; then
    echo "双后端差分门禁缺少受跟踪的工作负载或预期输出。" >&2
    exit 2
fi

rootfs=$(cd "$rootfs_arg" && pwd -P)
meson=${MESON:-$(command -v meson || true)}
ninja=${NINJA:-$(command -v ninja || true)}
sqlite3_bin=${SQLITE3:-$(command -v sqlite3 || true)}
timeout_bin=$(command -v timeout || command -v gtimeout || true)

if [[ ! -x $meson ]]; then
    echo "找不到可执行的 Meson；可通过 MESON 指定路径。" >&2
    exit 2
fi
if [[ ! -x $ninja ]]; then
    echo "找不到可执行的 Ninja；可通过 NINJA 指定路径。" >&2
    exit 2
fi
if [[ ! -x $sqlite3_bin ]]; then
    echo "找不到可执行的 sqlite3；可通过 SQLITE3 指定路径。" >&2
    exit 2
fi
if [[ -z $timeout_bin || ! -x $timeout_bin ]]; then
    echo "需要 timeout 或 gtimeout 提供 guest 硬超时。" >&2
    exit 2
fi
if [[ ! -x /usr/bin/python3 ]]; then
    echo "需要 macOS Python 3 建立受控进程组。" >&2
    exit 2
fi

for path in \
    "$rootfs/meta.db" \
    "$rootfs/rootfs-manifest.txt" \
    "$rootfs/rootfs-hardlinks.tsv"; do
    if [[ ! -f $path || -L $path ]]; then
        echo "规范 fakefs 种子缺少普通文件：$path" >&2
        exit 2
    fi
done
if [[ ! -d $rootfs/data || -L $rootfs/data ]]; then
    echo "规范 fakefs 种子缺少普通 data 目录。" >&2
    exit 2
fi

top_level_count=$(/usr/bin/find "$rootfs" -mindepth 1 -maxdepth 1 \
    -print | /usr/bin/wc -l | /usr/bin/tr -d '[:space:]')
if [[ $top_level_count != 4 ]]; then
    echo "规范 fakefs 种子顶层必须严格包含打包器生成的四项。" >&2
    exit 2
fi
special_node=$(/usr/bin/find "$rootfs/data" ! -type d ! -type f \
    -print -quit)
if [[ -n $special_node ]]; then
    echo "规范 fakefs data 树只能包含普通文件和目录：$special_node" >&2
    exit 2
fi
if [[ ! -f $rootfs/data/bin/busybox ]]; then
    echo "规范 fakefs 种子缺少 /bin/busybox。" >&2
    exit 2
fi

if ! /usr/bin/cmp -s "$rootfs/rootfs-manifest.txt" <(
        printf '%s\n' \
            'format=ish-fakefs-v3' \
            'packager=apple-aarch64-rootfs-v1' \
            'guest_arch=aarch64' \
            'source_kind=official' \
            'alpine_version=3.24.1' \
            'archive_sha256=f55a90f69052c5bd6f92cb09a8f47065970830b194c917a006fb94028e721259' \
            'source_url=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/alpine-minirootfs-3.24.1-aarch64.tar.gz' \
            'hardlinks=rootfs-hardlinks.tsv'
    ); then
    echo "规范 fakefs manifest 不符合固定 Alpine 3.24.1 合同。" >&2
    exit 2
fi

temporary_parent=/private/tmp
if [[ ! -d $temporary_parent || -L $temporary_parent ]]; then
    echo "缺少可信的 macOS 临时目录：$temporary_parent" >&2
    exit 2
fi
case "$temporary_parent/" in
    "$rootfs/"*)
        echo "临时目录不能位于输入 fakefs 种子内部。" >&2
        exit 2
        ;;
esac

lock_file=$temporary_parent/ish-aarch64-backend-paired.lock
if [[ -L $lock_file ||
        ( -e $lock_file && ( ! -f $lock_file || ! -O $lock_file ) ) ]]; then
    echo "双后端差分锁路径不是普通文件：$lock_file" >&2
    exit 2
fi
exec 9>>"$lock_file"
if ! /usr/bin/lockf -s -t 0 9; then
    echo "已有另一份双后端差分门禁正在运行。" >&2
    exit 3
fi

run_dir=
sqlite_home=
current_pid=
current_pgid=
owned_status=0
group_launcher='import os, sys; os.setsid(); os.execvp(sys.argv[1], sys.argv[1:])'

group_has_live_process() {
    /bin/ps -axo pgid=,state= |
        /usr/bin/awk -v group="$1" '
            $1 == group && $2 !~ /^Z/ { found = 1 }
            END { exit found ? 0 : 1 }
        '
}

stop_owned_process() {
    if [[ -z $current_pid ]]; then
        return
    fi
    if ! kill -TERM -- "-$current_pgid" >/dev/null 2>&1; then
        kill -TERM "$current_pid" >/dev/null 2>&1 || true
    fi
    for _ in {1..40}; do
        if ! group_has_live_process "$current_pgid"; then
            break
        fi
        sleep 0.05
    done
    if group_has_live_process "$current_pgid"; then
        kill -KILL -- "-$current_pgid" >/dev/null 2>&1 || true
    fi
    wait "$current_pid" >/dev/null 2>&1 || true
    current_pid=
    current_pgid=
}

cleanup() {
    local status=$?
    trap - EXIT
    trap '' HUP INT TERM
    stop_owned_process
    if [[ -n $run_dir ]]; then
        rm -rf "$run_dir"
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_dir=$(mktemp -d "$temporary_parent/ish-aarch64-backend-paired.XXXXXX")
sqlite_home=$run_dir/sqlite-home
mkdir "$sqlite_home"

run_owned() {
    # 独立进程组让异常清理可以覆盖 Ninja、timeout 与它们的后代。
    /usr/bin/python3 -c "$group_launcher" "$@" <&0 &
    current_pid=$!
    current_pgid=$current_pid
    if wait "$current_pid"; then
        owned_status=0
    else
        owned_status=$?
    fi
    if group_has_live_process "$current_pgid"; then
        echo "受控命令退出后仍留有同组后代进程。" >&2
        stop_owned_process
        owned_status=125
    else
        current_pid=
        current_pgid=
    fi
}

check_database() {
    local database=$1
    local result
    result=$(HOME="$sqlite_home" "$sqlite3_bin" \
        -readonly -batch -noheader -list -bail -init /dev/null \
        "$database" \
        'pragma quick_check; pragma user_version; select count(*) from paths where length(path) = 0;')
    if [[ $result != $'ok\n3\n1' ]]; then
        echo "fakefs 元数据完整性、schema 或根路径检查失败：$database" >&2
        exit 1
    fi
}

check_seed_database() {
    local database=$1
    local database_inode
    check_database "$database"
    database_inode=$(HOME="$sqlite_home" "$sqlite3_bin" \
        -readonly -batch -noheader -list -bail -init /dev/null \
        "$database" 'select db_inode from meta where id = 0;')
    if [[ $database_inode != 0 ]]; then
        echo "fakefs 种子已经被 mount，不能再作为规范输入：$database" >&2
        exit 1
    fi
}

build_backend() {
    local backend=$1
    local expected_default=$2
    local build_dir=$run_dir/build-$backend
    local binary_arch
    local configure_output
    local configured_backend
    local configured_profile
    local nm_symbols

    echo "配置 AArch64 $backend Release 后端"
    run_owned "$meson" setup "$build_dir" "$repo" \
        --buildtype=release \
        --wrap-mode=nodownload \
        -Dkernel=ish \
        -Dengine=asbestos \
        -Daarch64_backend="$backend" \
        -Daarch64_threaded_profile=false
    if [[ $owned_status -ne 0 ]]; then
        echo "Meson 配置 $backend 后端失败：$owned_status" >&2
        exit 1
    fi

    echo "低并发构建 AArch64 $backend Release 后端"
    run_owned /usr/bin/nice -n 10 "$ninja" -C "$build_dir" -j2 ish
    if [[ $owned_status -ne 0 ]]; then
        echo "Ninja 构建 $backend 后端失败：$owned_status" >&2
        exit 1
    fi

    configure_output=$("$meson" configure "$build_dir")
    configured_backend=$(printf '%s\n' "$configure_output" |
        /usr/bin/awk '$1 == "aarch64_backend" { print $2 }')
    configured_profile=$(printf '%s\n' "$configure_output" |
        /usr/bin/awk '$1 == "aarch64_threaded_profile" { print $2 }')
    if [[ $configured_backend != "$backend" ||
            $configured_profile != false ]]; then
        echo "$backend 构建没有保留请求的后端或关闭画像。" >&2
        exit 1
    fi
    if ! /usr/bin/grep -Fx \
            "#define ISH_AARCH64_BACKEND_THREADED_DEFAULT $expected_default" \
            "$build_dir/aarch64-backend-config.h" >/dev/null; then
        echo "$backend 构建生成了错误的默认后端宏。" >&2
        exit 1
    fi
    binary_arch=$(/usr/bin/xcrun lipo -archs "$build_dir/ish")
    if [[ $binary_arch != arm64 ]]; then
        echo "$backend Release 产物不是单一 arm64 Mach-O。" >&2
        exit 1
    fi
    nm_symbols=$(/usr/bin/nm "$build_dir/ish")
    if /usr/bin/grep -q 'aarch64_threaded_profile_' \
            <<< "$nm_symbols"; then
        echo "$backend Release 产物意外包含 threaded 画像符号。" >&2
        exit 1
    fi
}

run_backend() {
    local backend=$1
    local ish=$run_dir/build-$backend/ish
    local root=$run_dir/root-$backend
    local stdout=$run_dir/$backend.stdout
    local stderr=$run_dir/$backend.stderr

    echo "串行运行 AArch64 $backend 工作负载"
    run_owned /usr/bin/nice -n 10 /usr/bin/env -i TERM=dumb \
        "$timeout_bin" --foreground -k 5s 300s \
        "$ish" -f "$root" \
        /bin/busybox env -i \
        PATH=/bin:/usr/bin HOME=/root LC_ALL=C TZ=UTC \
        /bin/sh -s \
        < "$workload" > "$stdout" 2> "$stderr"

    if [[ $owned_status -ne 0 ]]; then
        echo "$backend 工作负载退出码不是 0：$owned_status" >&2
        [[ ! -s $stdout ]] || cat "$stdout" >&2
        [[ ! -s $stderr ]] || cat "$stderr" >&2
        exit 1
    fi
    if [[ -s $stderr ]]; then
        echo "$backend 工作负载产生了非空标准错误：" >&2
        cat "$stderr" >&2
        exit 1
    fi
    if ! /usr/bin/cmp -s "$expected" "$stdout"; then
        echo "$backend 工作负载输出不符合固定合同：" >&2
        /usr/bin/diff -u "$expected" "$stdout" >&2 || true
        exit 1
    fi
    check_database "$root/meta.db"
}

frozen_seed=$run_dir/seed
check_seed_database "$rootfs/meta.db"
run_owned /usr/bin/nice -n 10 /usr/bin/ditto "$rootfs" "$frozen_seed"
if [[ $owned_status -ne 0 ]]; then
    echo "冻结输入 fakefs 快照失败：$owned_status" >&2
    exit 1
fi
if ! /usr/bin/diff -qr "$rootfs" "$frozen_seed" >/dev/null; then
    echo "输入 fakefs 在冻结快照期间发生了变化。" >&2
    exit 1
fi
check_seed_database "$frozen_seed/meta.db"

build_backend c 0
build_backend threaded 1

root_c=$run_dir/root-c
root_threaded=$run_dir/root-threaded
inode_c=
inode_threaded=
run_owned /usr/bin/nice -n 10 /usr/bin/ditto "$frozen_seed" "$root_c"
if [[ $owned_status -ne 0 ]]; then
    echo "复制 C 后端隔离 fakefs 失败：$owned_status" >&2
    exit 1
fi
run_owned /usr/bin/nice -n 10 /usr/bin/ditto \
    "$frozen_seed" "$root_threaded"
if [[ $owned_status -ne 0 ]]; then
    echo "复制 threaded 后端隔离 fakefs 失败：$owned_status" >&2
    exit 1
fi

if ! /usr/bin/diff -qr "$root_c" "$root_threaded" >/dev/null; then
    echo "两个隔离 fakefs 在运行前并不一致。" >&2
    exit 1
fi
inode_c=$(/usr/bin/stat -f '%i' "$root_c/meta.db")
inode_threaded=$(/usr/bin/stat -f '%i' "$root_threaded/meta.db")
if [[ $inode_c == "$inode_threaded" ]]; then
    echo "两个隔离 fakefs 没有获得独立的元数据文件。" >&2
    exit 1
fi
check_seed_database "$root_c/meta.db"
check_seed_database "$root_threaded/meta.db"

run_backend c
run_backend threaded

if ! /usr/bin/cmp -s "$run_dir/c.stdout" \
        "$run_dir/threaded.stdout"; then
    echo "C 与 threaded 后端的标准输出不一致。" >&2
    exit 1
fi
if ! /usr/bin/cmp -s "$run_dir/c.stderr" \
        "$run_dir/threaded.stderr"; then
    echo "C 与 threaded 后端的标准错误不一致。" >&2
    exit 1
fi

artifact=tmp/ish-aarch64-paired-v1
if [[ ! -d $root_c/data/$artifact ||
        ! -d $root_threaded/data/$artifact ]]; then
    echo "双后端运行没有生成完整的工作负载 artifact。" >&2
    exit 1
fi
if ! /usr/bin/diff -ru "$root_c/data/$artifact" \
        "$root_threaded/data/$artifact"; then
    echo "C 与 threaded 后端的工作负载 artifact 不一致。" >&2
    exit 1
fi

echo "AArch64 C/threaded 真实工作负载差分通过"

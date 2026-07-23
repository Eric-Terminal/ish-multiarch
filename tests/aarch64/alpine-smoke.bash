#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "用法：$0 <ish 可执行文件> <fakefs 根目录> <DNS 端口重定向动态库>" >&2
    exit 2
fi

ish=$1
rootfs=$2
redirect_library=$3
script_dir=$(cd "$(dirname "$0")" && pwd)
network_fixture=$script_dir/local-network-fixture.py
timeout_bin=$(command -v timeout || command -v gtimeout || true)
fixture_dir=
fixture_pid=
injected_library=
ready_file=
query_log=
fixture_log=
smoke_rootfs=

if [[ ! -x $ish ]]; then
    echo "ish 可执行文件不存在或不可执行：$ish" >&2
    exit 2
fi
if [[ ! -f $rootfs/meta.db || ! -d $rootfs/data ]]; then
    echo "fakefs 根目录缺少 meta.db 或 data：$rootfs" >&2
    exit 2
fi
rootfs=$(cd "$rootfs" && pwd -P)
for fakefs_path in "$rootfs"/meta.db* "$rootfs/data"; do
    if [[ -L $fakefs_path ]]; then
        echo "fakefs 顶层存储不能是符号链接：$fakefs_path" >&2
        exit 2
    fi
done
fakefs_host_link=$(/usr/bin/find "$rootfs/data" -type l -print -quit)
if [[ -n $fakefs_host_link ]]; then
    echo "fakefs data 树不能包含宿主符号链接：$fakefs_host_link" >&2
    exit 2
fi
if [[ -z $timeout_bin ]]; then
    echo "需要 timeout 或 gtimeout 提供硬超时" >&2
    exit 2
fi
if [[ $(uname -s) != Darwin || ! -f $redirect_library ]]; then
    echo "DNS 冒烟需要 macOS 构建生成的端口重定向动态库：$redirect_library" >&2
    exit 2
fi
if [[ ! -f $network_fixture ]]; then
    echo "缺少本地 DNS/HTTP 夹具：$network_fixture" >&2
    exit 2
fi
exec 9>"$rootfs/.ish-aarch64-smoke.lock"
if ! /usr/bin/lockf -s -t 0 9; then
    echo "同一 fakefs 根目录已有 AArch64 冒烟验收" >&2
    exit 3
fi
if pgrep -x ish >/dev/null 2>&1; then
    echo "检测到已有 ish 进程，拒绝启动重叠验收" >&2
    exit 3
fi

cleanup() {
    local status=$?
    local fixture_state=
    trap - EXIT
    trap '' HUP INT TERM
    if [[ -n $fixture_pid ]]; then
        kill "$fixture_pid" >/dev/null 2>&1 || true
        for _ in {1..40}; do
            fixture_state=$(ps -o state= -p "$fixture_pid" 2>/dev/null | \
                tr -d '[:space:]' || true)
            if [[ -z $fixture_state || $fixture_state == Z ]]; then
                break
            fi
            sleep 0.05
        done
        if [[ -n $fixture_state && $fixture_state != Z ]]; then
            kill -KILL "$fixture_pid" >/dev/null 2>&1 || true
        fi
        wait "$fixture_pid" >/dev/null 2>&1 || true
    fi
    if [[ -n $fixture_dir ]]; then
        if [[ $status -ne 0 ]]; then
            [[ -f $fixture_log ]] && cat "$fixture_log" >&2
            [[ -f $query_log ]] && cat "$query_log" >&2
        fi
        rm -rf "$fixture_dir"
    fi
    exit "$status"
}

run_guest_with_dns() {
    env DYLD_INSERT_LIBRARIES="$injected_library" \
        ISH_AARCH64_E2E_DNS_PORT="$DNS_PORT" \
        "$timeout_bin" -k 2s 30s "$ish" -f "$smoke_rootfs" "$@"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_guest() {
    "$timeout_bin" -k 2s 30s "$ish" -f "$smoke_rootfs" "$@"
}

fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/ish-aarch64-network.XXXXXX")
smoke_rootfs=$fixture_dir/rootfs
/usr/bin/ditto "$rootfs" "$smoke_rootfs"
for fakefs_path in "$smoke_rootfs"/meta.db* "$smoke_rootfs/data"; do
    if [[ -L $fakefs_path ]]; then
        echo "隔离 fakefs 顶层存储不能是符号链接：$fakefs_path" >&2
        exit 1
    fi
done
fakefs_host_link=$(/usr/bin/find "$smoke_rootfs/data" \
    -type l -print -quit)
if [[ -n $fakefs_host_link ]]; then
    echo "隔离 fakefs data 树不能包含宿主符号链接：$fakefs_host_link" >&2
    exit 1
fi
injected_library=$fixture_dir/dns-port-redirect.dylib
cp "$redirect_library" "$injected_library"

run_guest /bin/true

run_guest /bin/sh -c '
    set -e
    test "$(uname -m)" = aarch64
    rm -rf /tmp/ish-aarch64-smoke
    mkdir -p /tmp/ish-aarch64-smoke
    printf "multiarch-ok\n" > /tmp/ish-aarch64-smoke/data
    test "$(cat /tmp/ish-aarch64-smoke/data)" = multiarch-ok
    chmod 0640 /tmp/ish-aarch64-smoke/data
    test "$(stat -c %a /tmp/ish-aarch64-smoke/data)" = 640
    cp /tmp/ish-aarch64-smoke/data /tmp/ish-aarch64-smoke/copy
    mv /tmp/ish-aarch64-smoke/copy /tmp/ish-aarch64-smoke/moved
    test -f /tmp/ish-aarch64-smoke/moved
    rm -rf /tmp/ish-aarch64-smoke
'

run_guest /bin/sh -c '
    set +e
    /bin/sh -c "exit 7" &
    child=$!
    wait "$child"
    test $? -eq 7
'

run_guest /bin/sh -c '
    set +e
    /bin/sh -c "while :; do :; done" &
    child=$!
    kill -TERM "$child"
    sent=$?
    wait "$child"
    status=$?
    test "$sent" -eq 0 && test "$status" -eq 143
'

ready_file=$fixture_dir/ready
query_log=$fixture_dir/queries
fixture_log=$fixture_dir/fixture.log
/usr/bin/python3 -u "$network_fixture" \
    --ready-file "$ready_file" \
    --query-log "$query_log" \
    --parent-pid "$$" >"$fixture_log" 2>&1 9>&- &
fixture_pid=$!
for _ in {1..100}; do
    if [[ -f $ready_file ]]; then
        break
    fi
    if ! kill -0 "$fixture_pid" >/dev/null 2>&1; then
        cat "$fixture_log" >&2
        echo "本地 DNS/HTTP 验收夹具提前退出" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ ! -f $ready_file ]]; then
    cat "$fixture_log" >&2
    echo "本地 DNS/HTTP 验收夹具未能启动" >&2
    exit 1
fi

# 文件只含夹具生成的十进制端口与十六进制 proof。
source "$ready_file"
if [[ ! $DNS_PORT =~ ^[0-9]+$ ||
        ! $HTTP_PORT =~ ^[0-9]+$ ||
        ! $PROOF =~ ^[0-9a-z-]+$ ]]; then
    echo "本地网络夹具返回了无效参数" >&2
    exit 1
fi

numeric_http=$(run_guest /usr/bin/wget -qO- \
    "http://127.0.0.1:$HTTP_PORT/proof")
[[ $numeric_http == "$PROOF" ]]

run_guest_with_dns /bin/sh -c '
    set -eu
    http_port=$1
    proof=$2
    resolver_tag=$3
    resolver_proof=/tmp/ish-aarch64-dns-proof
    printf "# %s\nnameserver 127.0.0.53\noptions attempts:1 timeout:1\n" \
        "$resolver_tag" > /etc/resolv.conf

    getent_output=$(getent ahostsv4 getent.ish-dns.test)
    printf "%s\n" "$getent_output" | grep -F "127.0.0.1" >/dev/null

    nslookup_output=$(nslookup nslookup.ish-dns.test)
    printf "%s\n" "$nslookup_output" | grep -F "127.0.0.1" >/dev/null

    wget -qO "$resolver_proof" \
        "http://http.ish-dns.test:$http_port/proof"
    test "$(cat "$resolver_proof")" = "$proof"
' alpine-dns-smoke "$HTTP_PORT" "$PROOF" \
    "ish-aarch64-dns-smoke-$PROOF"

grep -Fx 'getent.ish-dns.test. A' "$query_log" >/dev/null
grep -Fx 'nslookup.ish-dns.test. A' "$query_log" >/dev/null
grep -Fx 'http.ish-dns.test. A' "$query_log" >/dev/null
grep -Fx 'http.ish-dns.test. AAAA' "$query_log" >/dev/null

echo "AArch64 Alpine 冒烟验收通过"

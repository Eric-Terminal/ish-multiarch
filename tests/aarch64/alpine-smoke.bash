#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "用法：$0 <ish 可执行文件> <fakefs 根目录>" >&2
    exit 2
fi

ish=$1
rootfs=$2
timeout_bin=$(command -v timeout || command -v gtimeout || true)
server_pid=

if [[ ! -x $ish ]]; then
    echo "ish 可执行文件不存在或不可执行：$ish" >&2
    exit 2
fi
if [[ ! -f $rootfs/meta.db || ! -d $rootfs/data ]]; then
    echo "fakefs 根目录缺少 meta.db 或 data：$rootfs" >&2
    exit 2
fi
if [[ -z $timeout_bin ]]; then
    echo "需要 timeout 或 gtimeout 提供硬超时" >&2
    exit 2
fi
if pgrep -x ish >/dev/null 2>&1; then
    echo "检测到已有 ish 进程，拒绝启动重叠验收" >&2
    exit 3
fi

cleanup() {
    if [[ -n $server_pid ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" >/dev/null 2>&1 || true
    fi
    if pgrep -x ish >/dev/null 2>&1; then
        pkill -TERM -x ish >/dev/null 2>&1 || true
        pkill -KILL -x ish >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

run_guest() {
    "$timeout_bin" 30s "$ish" -f "$rootfs" "$@"
    if pgrep -x ish >/dev/null 2>&1; then
        echo "guest 命令结束后仍有 ish 进程：$*" >&2
        return 1
    fi
}

run_guest /bin/true

run_guest /bin/sh -c '
    set -e
    test "$(uname -m)" = aarch64
    rm -rf /tmp/ish-aarch64-smoke
    mkdir -p /tmp/ish-aarch64-smoke
    printf "multiarch-ok\n" > /tmp/ish-aarch64-smoke/data
    test "$(cat /tmp/ish-aarch64-smoke/data)" = multiarch-ok
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

port=${ISH_AARCH64_SMOKE_PORT:-18080}
/usr/bin/python3 -m http.server "$port" \
    --bind 127.0.0.1 --directory /tmp >/dev/null 2>&1 &
server_pid=$!
for _ in {1..50}; do
    if ! kill -0 "$server_pid" >/dev/null 2>&1; then
        echo "本地 HTTP 验收服务器提前退出" >&2
        exit 1
    fi
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        exec 3>&-
        exec 3<&-
        break
    fi
    sleep 0.1
done
if ! (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
    echo "本地 HTTP 验收服务器未能启动" >&2
    exit 1
fi
exec 3>&-
exec 3<&-
run_guest /usr/bin/wget -qO- "http://127.0.0.1:$port/" >/dev/null

echo "AArch64 Alpine 冒烟验收通过"

#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "用法：$0 <端口重定向动态库> <探针可执行文件> <本地网络夹具>" >&2
    exit 2
fi

redirect_library=$1
probe=$2
fixture=$3
workdir=$(mktemp -d "${TMPDIR:-/tmp}/ish-dns-redirect.XXXXXX")
ready_file=$workdir/ready
query_log=$workdir/queries
fixture_log=$workdir/fixture.log
fixture_pid=
injected_library=$workdir/dns-port-redirect.dylib

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
    if [[ $status -ne 0 && -f $fixture_log ]]; then
        cat "$fixture_log" >&2
        [[ -f $query_log ]] && cat "$query_log" >&2
    fi
    rm -rf "$workdir"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

cp "$redirect_library" "$injected_library"
python3 -u "$fixture" \
    --ready-file "$ready_file" \
    --query-log "$query_log" \
    --parent-pid "$$" >"$fixture_log" 2>&1 &
fixture_pid=$!
for _ in {1..100}; do
    if [[ -f $ready_file ]]; then
        break
    fi
    if ! kill -0 "$fixture_pid" >/dev/null 2>&1; then
        echo "本地网络夹具在就绪前退出" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ ! -f $ready_file ]]; then
    echo "本地网络夹具未在时限内就绪" >&2
    exit 1
fi

# 文件只含夹具生成的十进制端口与十六进制 proof。
source "$ready_file"
if [[ ! $DNS_PORT =~ ^[0-9]+$ ]]; then
    echo "本地网络夹具返回了无效 DNS 端口" >&2
    exit 1
fi

# 夹具空闲超过一次 socket timeout 后仍必须继续服务。
sleep 0.3
env DYLD_INSERT_LIBRARIES="$injected_library" \
    ISH_AARCH64_E2E_DNS_PORT="$DNS_PORT" \
    "$probe"
query_count=$(grep -Fxc 'probe.ish-dns.test. A' "$query_log" || true)
if [[ $query_count -ne 7 ]]; then
    echo "DNS 端口重定向探针查询数异常：$query_count" >&2
    exit 1
fi

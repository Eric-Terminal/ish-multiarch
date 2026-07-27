#!/bin/bash
set -euo pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [[ $# -ne 4 ]]; then
    echo "用法：$0 <Alpine minirootfs 归档> <包来源清单> <二进制参照清单|--fixture> <LGPL payload 清单|--fixture>" >&2
    exit 2
fi

ARCHIVE=$1
LOCK=$2
REFERENCE=$3
LGPL_PAYLOADS=$4
if [[ ! -f "$ARCHIVE" ]]; then
    echo "错误：rootfs 归档不存在：$ARCHIVE" >&2
    exit 1
fi
if [[ ! -f "$LOCK" ]]; then
    echo "错误：包来源清单不存在：$LOCK" >&2
    exit 1
fi
if [[ "$REFERENCE" == --fixture ]]; then
    REFERENCE=
elif [[ ! -f "$REFERENCE" ]]; then
    echo "错误：二进制参照清单不存在：$REFERENCE" >&2
    exit 1
fi
if [[ "$LGPL_PAYLOADS" == --fixture ]]; then
    LGPL_PAYLOADS=
elif [[ ! -f "$LGPL_PAYLOADS" ]]; then
    echo "错误：LGPL payload 清单不存在：$LGPL_PAYLOADS" >&2
    exit 1
fi
if [[ -n "$LGPL_PAYLOADS" && -z "$REFERENCE" ]]; then
    echo "错误：生产 LGPL payload 门禁不能跳过二进制参照。" >&2
    exit 1
fi

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        echo "错误：找不到 shasum 或 sha256sum。" >&2
        return 1
    fi
}

file_size() {
    wc -c < "$1" | tr -d ' '
}

EXPECTED_INSTALLED_SIZE=
EXPECTED_INSTALLED_SHA256=
EXPECTED_PACKAGES_SIZE=
EXPECTED_PACKAGES_SHA256=
EXPECTED_PACKAGE_COUNT=
EXPECTED_ORIGIN_COUNT=
if [[ -n "$REFERENCE" ]]; then
    REFERENCE_HEADER=$'alpine_version\tarchive_name\tarchive_size\tarchive_sha256\tsource_url\tinstalled_size\tinstalled_sha256\tpackages_size\tpackages_sha256\tpackage_count\torigin_count'
    if ! awk -F '\t' -v header="$REFERENCE_HEADER" '
            NR == 1 && $0 != header { exit 1 }
            NR == 2 && NF != 11 { exit 1 }
            NR > 2 { exit 1 }
            END { if (NR != 2) exit 1 }
        ' "$REFERENCE"; then
        echo "错误：AArch64 rootfs 二进制参照清单格式非法。" >&2
        exit 1
    fi
    IFS=$'\t' read -r VERSION ARCHIVE_NAME ARCHIVE_SIZE ARCHIVE_SHA256 \
        SOURCE_URL EXPECTED_INSTALLED_SIZE EXPECTED_INSTALLED_SHA256 \
        EXPECTED_PACKAGES_SIZE EXPECTED_PACKAGES_SHA256 \
        EXPECTED_PACKAGE_COUNT EXPECTED_ORIGIN_COUNT \
        < <(sed -n '2p' "$REFERENCE")
    EXPECTED_SOURCE_URL="https://dl-cdn.alpinelinux.org/alpine/v${VERSION%.*}/releases/aarch64/$ARCHIVE_NAME"
    if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ||
            "$ARCHIVE_NAME" != "alpine-minirootfs-${VERSION}-aarch64.tar.gz" ||
            ! "$ARCHIVE_SIZE" =~ ^[1-9][0-9]*$ ||
            ! "$ARCHIVE_SHA256" =~ ^[0-9a-f]{64}$ ||
            "$SOURCE_URL" != "$EXPECTED_SOURCE_URL" ||
            ! "$EXPECTED_INSTALLED_SIZE" =~ ^[1-9][0-9]*$ ||
            ! "$EXPECTED_INSTALLED_SHA256" =~ ^[0-9a-f]{64}$ ||
            ! "$EXPECTED_PACKAGES_SIZE" =~ ^[1-9][0-9]*$ ||
            ! "$EXPECTED_PACKAGES_SHA256" =~ ^[0-9a-f]{64}$ ||
            ! "$EXPECTED_PACKAGE_COUNT" =~ ^[1-9][0-9]*$ ||
            ! "$EXPECTED_ORIGIN_COUNT" =~ ^[1-9][0-9]*$ ]]; then
        echo "错误：AArch64 rootfs 二进制参照字段非法。" >&2
        exit 1
    fi
    ACTUAL_ARCHIVE_SIZE=$(file_size "$ARCHIVE")
    ACTUAL_ARCHIVE_SHA256=$(sha256_file "$ARCHIVE")
    if [[ "$ACTUAL_ARCHIVE_SIZE" != "$ARCHIVE_SIZE" ||
            "$ACTUAL_ARCHIVE_SHA256" != "$ARCHIVE_SHA256" ]]; then
        echo "错误：rootfs 归档大小或 SHA-256 与二进制参照不一致。" >&2
        exit 1
    fi
    ACTUAL_PACKAGES_SIZE=$(file_size "$LOCK")
    ACTUAL_PACKAGES_SHA256=$(sha256_file "$LOCK")
    if [[ "$ACTUAL_PACKAGES_SIZE" != "$EXPECTED_PACKAGES_SIZE" ||
            "$ACTUAL_PACKAGES_SHA256" != "$EXPECTED_PACKAGES_SHA256" ]]; then
        echo "错误：包来源清单大小或 SHA-256 与二进制参照不一致。" >&2
        exit 1
    fi
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ish-rootfs-packages.XXXXXX")
cleanup() {
    local exit_code=$?
    trap - EXIT HUP INT TERM
    rm -rf "$TMP"
    exit "$exit_code"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

MEMBERS="$TMP/members"
if ! tar -tzf "$ARCHIVE" > "$MEMBERS"; then
    echo "错误：无法读取 rootfs 归档目录。" >&2
    exit 1
fi
INSTALLED_MEMBERS="$TMP/installed-members"
awk '$0 == "lib/apk/db/installed" ||
        $0 == "./lib/apk/db/installed" { print }' \
    "$MEMBERS" > "$INSTALLED_MEMBERS"
if [[ $(wc -l < "$INSTALLED_MEMBERS" | tr -d ' ') != 1 ]]; then
    echo "错误：rootfs 必须包含唯一的 apk installed 数据库。" >&2
    exit 1
fi
INSTALLED_MEMBER=$(sed -n '1p' "$INSTALLED_MEMBERS")
INSTALLED="$TMP/installed"
if ! tar -xOzf "$ARCHIVE" "$INSTALLED_MEMBER" > "$INSTALLED"; then
    echo "错误：无法读取 apk installed 数据库。" >&2
    exit 1
fi
if [[ -n "$REFERENCE" ]]; then
    ACTUAL_INSTALLED_SIZE=$(file_size "$INSTALLED")
    ACTUAL_INSTALLED_SHA256=$(sha256_file "$INSTALLED")
    if [[ "$ACTUAL_INSTALLED_SIZE" != "$EXPECTED_INSTALLED_SIZE" ||
            "$ACTUAL_INSTALLED_SHA256" != "$EXPECTED_INSTALLED_SHA256" ]]; then
        echo "错误：apk installed 数据库大小或 SHA-256 与二进制参照不一致。" >&2
        exit 1
    fi
fi

PARSED="$TMP/packages.unsorted.tsv"
if ! awk '
    BEGIN {
        RS = ""
        FS = "\n"
        OFS = "\t"
    }
    {
        seen_package = seen_version = seen_origin = 0
        seen_license = seen_commit = 0
        package = version = origin = license = commit = ""
        for (field_number = 1; field_number <= NF; field_number++) {
            prefix = substr($field_number, 1, 2)
            value = substr($field_number, 3)
            if (prefix == "P:") {
                seen_package++
                package = value
            } else if (prefix == "V:") {
                seen_version++
                version = value
            } else if (prefix == "o:") {
                seen_origin++
                origin = value
            } else if (prefix == "L:") {
                seen_license++
                license = value
            } else if (prefix == "c:") {
                seen_commit++
                commit = value
            }
        }
        if (seen_package != 1 || seen_version != 1 ||
                seen_origin != 1 || seen_license != 1 ||
                seen_commit != 1 ||
                package == "" || version == "" ||
                origin == "" || license == "" ||
                index(package, "\t") || index(version, "\t") ||
                index(origin, "\t") || index(license, "\t") ||
                commit !~ /^[0-9a-f][0-9a-f]*$/ ||
                length(commit) != 40) {
            print "错误：apk installed 记录字段缺失、重复或格式非法。" > "/dev/stderr"
            exit 1
        }
        print package, version, origin, license, commit
    }
' "$INSTALLED" > "$PARSED"; then
    exit 1
fi

if [[ ! -s "$PARSED" ]]; then
    echo "错误：apk installed 数据库没有包记录。" >&2
    exit 1
fi
DUPLICATES="$TMP/duplicate-packages"
LC_ALL=C sort -t $'\t' -k1,1 "$PARSED" |
    cut -f1 | uniq -d > "$DUPLICATES"
if [[ -s "$DUPLICATES" ]]; then
    echo "错误：apk installed 数据库包含重复包名。" >&2
    exit 1
fi

ACTUAL="$TMP/packages.tsv"
{
    printf '%s\n' $'package\tversion\torigin\tlicense\taports_commit'
    LC_ALL=C sort -t $'\t' -k1,1 "$PARSED"
} > "$ACTUAL"
if ! cmp -s "$LOCK" "$ACTUAL"; then
    echo "错误：rootfs 包版本、来源、许可证或 aports 提交与锁定清单不一致。" >&2
    diff -u "$LOCK" "$ACTUAL" >&2 || true
    exit 1
fi
if [[ -n "$REFERENCE" ]]; then
    ACTUAL_PACKAGE_COUNT=$(wc -l < "$PARSED" | tr -d ' ')
    ACTUAL_ORIGIN_COUNT=$(
        cut -f3 "$PARSED" | LC_ALL=C sort -u | wc -l | tr -d ' ')
    if [[ "$ACTUAL_PACKAGE_COUNT" != "$EXPECTED_PACKAGE_COUNT" ||
            "$ACTUAL_ORIGIN_COUNT" != "$EXPECTED_ORIGIN_COUNT" ]]; then
        echo "错误：rootfs 包数量或来源数量与二进制参照不一致。" >&2
        exit 1
    fi
fi

if [[ -n "$LGPL_PAYLOADS" ]]; then
    PYTHON3=$(command -v python3 || true)
    if [[ -z "$PYTHON3" ]]; then
        echo "错误：找不到 Python 3，无法验证 LGPL payload。" >&2
        exit 1
    fi
    "$PYTHON3" -B "$ROOT/tools/apple-aarch64-lgpl-surface.py" \
        validate-rootfs "$ARCHIVE" \
        --packages "$LOCK" \
        --payloads "$LGPL_PAYLOADS" \
        --binary-reference "$REFERENCE"
fi

echo "Alpine rootfs apk 包元数据清单验证通过"

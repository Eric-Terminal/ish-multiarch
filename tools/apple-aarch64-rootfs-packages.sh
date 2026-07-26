#!/bin/bash
set -euo pipefail
export LC_ALL=C

if [[ $# -ne 2 ]]; then
    echo "用法：$0 <Alpine minirootfs 归档> <包来源清单>" >&2
    exit 2
fi

ARCHIVE=$1
LOCK=$2
if [[ ! -f "$ARCHIVE" ]]; then
    echo "错误：rootfs 归档不存在：$ARCHIVE" >&2
    exit 1
fi
if [[ ! -f "$LOCK" ]]; then
    echo "错误：包来源清单不存在：$LOCK" >&2
    exit 1
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

echo "Alpine rootfs apk 包元数据清单验证通过"

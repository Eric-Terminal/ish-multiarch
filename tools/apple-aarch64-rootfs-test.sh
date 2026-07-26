#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PACKAGER="$ROOT/tools/apple-aarch64-rootfs.sh"
PACKAGE_VERIFIER="$ROOT/tools/apple-aarch64-rootfs-packages.sh"
FAKEFSIFY=${1:-}
SQLITE3_BIN=${2:-}

if [[ ! -x "$FAKEFSIFY" || ! -x "$SQLITE3_BIN" ]]; then
    echo "用法：$0 <fakefsify> <sqlite3>" >&2
    exit 2
fi
export SQLITE3=$SQLITE3_BIN

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

sha256_text() {
    if command -v shasum >/dev/null 2>&1; then
        printf '%s' "$1" | shasum -a 256 | awk '{print $1}'
    else
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    fi
}

stat_inode() {
    local inode
    inode=$(stat -f '%i' "$1" 2>/dev/null || true)
    if [[ "$inode" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$inode"
    else
        stat -c '%i' "$1"
    fi
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ish-aarch64-rootfs-test.XXXXXX")
child_pids=
cleanup() {
    local status=$?
    trap - EXIT HUP INT TERM
    local pid
    for pid in $child_pids; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    for pid in $child_pids; do
        wait "$pid" >/dev/null 2>&1 || true
    done
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

make_archive() {
    local machine_low=$1
    local destination=$2
    local elf_class=${3:-2}
    local elf_data=${4:-1}
    local installed_fixture=${5:-}
    local tree="$TMP/tree"
    rm -rf "$tree"
    mkdir -p "$tree/bin" "$tree/etc" "$tree/usr/lib" \
        "$tree/lib/apk/db"

    # 只需构造足够识别 class/data/e_machine 的 ELF 头，转换器不会执行它。
    printf '\177ELF' > "$tree/bin/busybox"
    printf '%b' "\\$(printf '%03o' "$elf_class")" \
        >> "$tree/bin/busybox"
    printf '%b' "\\$(printf '%03o' "$elf_data")" \
        >> "$tree/bin/busybox"
    printf '\001\000\000\000\000\000\000\000\000\000' \
        >> "$tree/bin/busybox"
    printf '\002\000' >> "$tree/bin/busybox"
    printf '%b' "\\$(printf '%03o' "$machine_low")\000" \
        >> "$tree/bin/busybox"
    dd if=/dev/zero bs=1 count=44 >> "$tree/bin/busybox" 2>/dev/null
    chmod 0755 "$tree/bin/busybox"
    printf 'test\n' > "$tree/etc/alpine-release"
    printf 'hardlink-proof\n' > "$tree/usr/lib/hardlink-source"
    ln "$tree/usr/lib/hardlink-source" "$tree/usr/lib/hardlink-alias"
    if [[ -n "$installed_fixture" ]]; then
        cp "$installed_fixture" "$tree/lib/apk/db/installed"
    else
        printf '%s\n' \
            'P:fixture-extra' \
            'V:2-r0' \
            'L:BSD-2-Clause' \
            'o:fixture-origin' \
            'c:fedcba9876543210fedcba9876543210fedcba98' \
            '' \
            'P:fixture-base' \
            'V:1-r0' \
            'L:MIT' \
            'o:fixture-origin' \
            'c:0123456789abcdef0123456789abcdef01234567' \
            > "$tree/lib/apk/db/installed"
    fi
    (cd "$tree" && tar -czf "$destination" .)
}

run_packager() {
    local archive=$1
    local output=$2
    local digest
    digest=$(sha256_file "$archive")
    env ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
        ISH_AARCH64_ROOTFS_TEST_SHA256="$digest" \
        ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
        "$PACKAGER" "$output" "$archive" "$FAKEFSIFY"
}

run_packager_with_digest() {
    local archive=$1
    local output=$2
    local digest=$3
    env ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
        ISH_AARCH64_ROOTFS_TEST_SHA256="$digest" \
        ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
        "$PACKAGER" "$output" "$archive" "$FAKEFSIFY"
}

run_packager_with_packages() {
    local archive=$1
    local output=$2
    local packages=$3
    env ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
        ISH_AARCH64_ROOTFS_TEST_SHA256="$(sha256_file "$archive")" \
        ISH_AARCH64_ROOTFS_TEST_PACKAGES="$packages" \
        "$PACKAGER" "$output" "$archive" "$FAKEFSIFY"
}

FIXTURE_PACKAGES="$TMP/packages.tsv"
EMPTY_PACKAGES="$TMP/packages-empty.tsv"
BASE_PACKAGES="$TMP/packages-base.tsv"
DUPLICATE_ACTUAL_PACKAGES="$TMP/packages-actual-duplicate.tsv"
printf '%s\n' \
    $'package\tversion\torigin\tlicense\taports_commit' \
    $'fixture-base\t1-r0\tfixture-origin\tMIT\t0123456789abcdef0123456789abcdef01234567' \
    $'fixture-extra\t2-r0\tfixture-origin\tBSD-2-Clause\tfedcba9876543210fedcba9876543210fedcba98' \
    > "$FIXTURE_PACKAGES"
sed -n '1p' "$FIXTURE_PACKAGES" > "$EMPTY_PACKAGES"
sed -n '1p;2p' "$FIXTURE_PACKAGES" > "$BASE_PACKAGES"
sed -n '1p;2p;2p' "$FIXTURE_PACKAGES" \
    > "$DUPLICATE_ACTUAL_PACKAGES"

EMPTY_INSTALLED="$TMP/installed-empty"
REPEATED_FIELD_INSTALLED="$TMP/installed-repeated-field"
DUPLICATE_PACKAGE_INSTALLED="$TMP/installed-duplicate-package"
: > "$EMPTY_INSTALLED"
printf '%s\n' \
    'P:' \
    'P:fixture-base' \
    'V:1-r0' \
    'L:MIT' \
    'o:fixture-origin' \
    'c:0123456789abcdef0123456789abcdef01234567' \
    > "$REPEATED_FIELD_INSTALLED"
printf '%s\n' \
    'P:fixture-base' \
    'V:1-r0' \
    'L:MIT' \
    'o:fixture-origin' \
    'c:0123456789abcdef0123456789abcdef01234567' \
    '' \
    'P:fixture-base' \
    'V:1-r0' \
    'L:MIT' \
    'o:fixture-origin' \
    'c:0123456789abcdef0123456789abcdef01234567' \
    > "$DUPLICATE_PACKAGE_INSTALLED"

AARCH64_ARCHIVE="$TMP/aarch64.tar.gz"
WRONG_ARCHIVE="$TMP/wrong-arch.tar.gz"
WRONG_CLASS_ARCHIVE="$TMP/wrong-class.tar.gz"
WRONG_ENDIAN_ARCHIVE="$TMP/wrong-endian.tar.gz"
EMPTY_INSTALLED_ARCHIVE="$TMP/empty-installed.tar.gz"
REPEATED_FIELD_ARCHIVE="$TMP/repeated-field.tar.gz"
DUPLICATE_PACKAGE_ARCHIVE="$TMP/duplicate-package.tar.gz"
OUTPUT="$TMP/result"
make_archive 183 "$AARCH64_ARCHIVE"
make_archive 3 "$WRONG_ARCHIVE"
make_archive 183 "$WRONG_CLASS_ARCHIVE" 1 1
make_archive 183 "$WRONG_ENDIAN_ARCHIVE" 2 2
make_archive 183 "$EMPTY_INSTALLED_ARCHIVE" 2 1 "$EMPTY_INSTALLED"
make_archive 183 "$REPEATED_FIELD_ARCHIVE" 2 1 \
    "$REPEATED_FIELD_INSTALLED"
make_archive 183 "$DUPLICATE_PACKAGE_ARCHIVE" 2 1 \
    "$DUPLICATE_PACKAGE_INSTALLED"

run_packager "$AARCH64_ARCHIVE" "$OUTPUT"
[[ -f "$OUTPUT/meta.db" && -f "$OUTPUT/data/bin/busybox" ]]
[[ ! -e "$OUTPUT/meta.db-wal" && ! -e "$OUTPUT/meta.db-shm" ]]
grep -Fx 'guest_arch=aarch64' "$OUTPUT/rootfs-manifest.txt" >/dev/null
grep -Fx 'source_kind=test-fixture' "$OUTPUT/rootfs-manifest.txt" >/dev/null
grep -Fx 'alpine_version=test-fixture' "$OUTPUT/rootfs-manifest.txt" >/dev/null
grep -Fx 'source_url=test-fixture://synthetic' \
    "$OUTPUT/rootfs-manifest.txt" >/dev/null
grep -F $'usr/lib/hardlink-alias\tusr/lib/hardlink-alias' \
    "$OUTPUT/rootfs-hardlinks.tsv" >/dev/null
grep -F $'usr/lib/hardlink-alias\tusr/lib/hardlink-source' \
    "$OUTPUT/rootfs-hardlinks.tsv" >/dev/null
[[ $(stat_inode "$OUTPUT/data/usr/lib/hardlink-source") == \
    $(stat_inode "$OUTPUT/data/usr/lib/hardlink-alias") ]]

VERSION_DRIFT_PACKAGES="$TMP/packages-version-drift.tsv"
ORIGIN_DRIFT_PACKAGES="$TMP/packages-origin-drift.tsv"
LICENSE_DRIFT_PACKAGES="$TMP/packages-license-drift.tsv"
COMMIT_DRIFT_PACKAGES="$TMP/packages-commit-drift.tsv"
DUPLICATE_PACKAGES="$TMP/packages-duplicate.tsv"
UNSORTED_PACKAGES="$TMP/packages-unsorted.tsv"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 { $2 = "9-r9" } { print }' \
    "$FIXTURE_PACKAGES" > "$VERSION_DRIFT_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 { $3 = "other-origin" } { print }' \
    "$FIXTURE_PACKAGES" > "$ORIGIN_DRIFT_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 { $4 = "GPL-2.0-only" } { print }' \
    "$FIXTURE_PACKAGES" > "$LICENSE_DRIFT_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $5 = "1123456789abcdef0123456789abcdef01234567"
    } { print }' "$FIXTURE_PACKAGES" > "$COMMIT_DRIFT_PACKAGES"
sed -n '1p;2p;2p;3p' "$FIXTURE_PACKAGES" > "$DUPLICATE_PACKAGES"
{
    sed -n '1p' "$FIXTURE_PACKAGES"
    sed -n '3p' "$FIXTURE_PACKAGES"
    sed -n '2p' "$FIXTURE_PACKAGES"
} > "$UNSORTED_PACKAGES"

if run_packager_with_packages "$AARCH64_ARCHIVE" \
        "$TMP/package-drift-output" "$VERSION_DRIFT_PACKAGES" \
        >/dev/null 2>&1; then
    echo "错误：打包器不能接受版本漂移的 rootfs 包清单。" >&2
    exit 1
fi
[[ ! -e "$TMP/package-drift-output" ]]
for invalid_packages in "$ORIGIN_DRIFT_PACKAGES" \
        "$LICENSE_DRIFT_PACKAGES" "$COMMIT_DRIFT_PACKAGES" \
        "$DUPLICATE_PACKAGES" "$UNSORTED_PACKAGES"; do
    if "$PACKAGE_VERIFIER" "$AARCH64_ARCHIVE" "$invalid_packages" \
            >/dev/null 2>&1; then
        echo "错误：包来源校验器接受了漂移、重复或乱序清单。" >&2
        exit 1
    fi
done
if "$PACKAGE_VERIFIER" "$EMPTY_INSTALLED_ARCHIVE" "$EMPTY_PACKAGES" \
        >/dev/null 2>&1; then
    echo "错误：包来源校验器接受了空的 apk installed 数据库。" >&2
    exit 1
fi
if "$PACKAGE_VERIFIER" "$REPEATED_FIELD_ARCHIVE" "$BASE_PACKAGES" \
        >/dev/null 2>&1; then
    echo "错误：包来源校验器接受了重复的 apk installed 字段。" >&2
    exit 1
fi
if "$PACKAGE_VERIFIER" "$DUPLICATE_PACKAGE_ARCHIVE" \
        "$DUPLICATE_ACTUAL_PACKAGES" >/dev/null 2>&1; then
    echo "错误：包来源校验器接受了重复的 apk installed 包名。" >&2
    exit 1
fi

touch "$OUTPUT/previous-output"
if run_packager_with_digest "$AARCH64_ARCHIVE" "$OUTPUT" \
        0000000000000000000000000000000000000000000000000000000000000000 \
        >/dev/null 2>&1; then
    echo "错误：错误摘要不应生成 rootfs。" >&2
    exit 1
fi
[[ -f "$OUTPUT/previous-output" ]]

if "$PACKAGER" "$TMP/forbidden-fourth-argument" "$AARCH64_ARCHIVE" \
        "$FAKEFSIFY" "$(sha256_file "$AARCH64_ARCHIVE")" \
        >/dev/null 2>&1; then
    echo "错误：生产命令行不能接受可变 rootfs 摘要。" >&2
    exit 1
fi
if env ISH_AARCH64_ROOTFS_TEST_SHA256="$(sha256_file "$AARCH64_ARCHIVE")" \
        "$PACKAGER" "$TMP/implicit-fixture" "$AARCH64_ARCHIVE" \
        "$FAKEFSIFY" >/dev/null 2>&1; then
    echo "错误：未显式启用 fixture 模式时不能接受测试摘要。" >&2
    exit 1
fi
if env ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
        "$PACKAGER" "$TMP/implicit-package-fixture" "$AARCH64_ARCHIVE" \
        "$FAKEFSIFY" >/dev/null 2>&1; then
    echo "错误：未显式启用 fixture 模式时不能接受测试包清单。" >&2
    exit 1
fi

if run_packager "$WRONG_ARCHIVE" "$OUTPUT" >/dev/null 2>&1; then
    echo "错误：非 AArch64 ELF 不应生成 rootfs。" >&2
    exit 1
fi
[[ -f "$OUTPUT/previous-output" ]]
if run_packager "$WRONG_CLASS_ARCHIVE" "$OUTPUT" >/dev/null 2>&1 ||
        run_packager "$WRONG_ENDIAN_ARCHIVE" "$OUTPUT" \
            >/dev/null 2>&1; then
    echo "错误：非 ELF64 little-endian 标记不应生成 rootfs。" >&2
    exit 1
fi
[[ -f "$OUTPUT/previous-output" ]]

run_packager "$AARCH64_ARCHIVE" "$OUTPUT" >/dev/null
[[ ! -e "$OUTPUT/previous-output" ]]

OUTPUT_PARENT=$(cd "$(dirname "$OUTPUT")" && pwd -P)
OUTPUT_CANONICAL="$OUTPUT_PARENT/$(basename "$OUTPUT")"
RECOVERY="$OUTPUT_PARENT/.ish-rootfs-recovery-$(sha256_text "$OUTPUT_CANONICAL")"
printf '%s\n' "$OUTPUT_CANONICAL" > "$RECOVERY.target"
mv "$OUTPUT" "$RECOVERY"
run_packager "$AARCH64_ARCHIVE" "$OUTPUT" >/dev/null
[[ -d "$OUTPUT" && ! -e "$RECOVERY" && ! -e "$RECOVERY.target" ]]

UNOWNED="$TMP/unowned"
mkdir "$UNOWNED"
printf 'keep\n' > "$UNOWNED/user-file"
if run_packager "$AARCH64_ARCHIVE" "$UNOWNED" >/dev/null 2>&1; then
    echo "错误：不能替换无打包器标记的既有目录。" >&2
    exit 1
fi
[[ -f "$UNOWNED/user-file" ]]

SYMLINK_OUTPUT="$TMP/symlink-output"
ln -s "$UNOWNED" "$SYMLINK_OUTPUT"
if run_packager "$AARCH64_ARCHIVE" "$SYMLINK_OUTPUT" \
        >/dev/null 2>&1; then
    echo "错误：不能替换符号链接输出。" >&2
    exit 1
fi
[[ -L "$SYMLINK_OUTPUT" && -f "$UNOWNED/user-file" ]]

INVALID_ARCHIVE="$TMP/not-a-tar.gz"
printf 'not-an-archive\n' > "$INVALID_ARCHIVE"
if run_packager "$INVALID_ARCHIVE" "$OUTPUT" >/dev/null 2>&1; then
    echo "错误：无法读取的归档不应替换既有 rootfs。" >&2
    exit 1
fi
[[ -f "$OUTPUT/meta.db" ]]

FAILING_FAKEFSIFY="$TMP/failing-fakefsify"
FAILING_FAKEFSIFY_LOG="$TMP/failing-fakefsify.log"
printf '%s\n' \
    '#!/bin/bash' \
    'set -euo pipefail' \
    'printf "called\n" > "$FAILING_FAKEFSIFY_LOG"' \
    'exit 1' > "$FAILING_FAKEFSIFY"
chmod +x "$FAILING_FAKEFSIFY"
if env FAILING_FAKEFSIFY_LOG="$FAILING_FAKEFSIFY_LOG" \
        ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
        ISH_AARCH64_ROOTFS_TEST_SHA256="$(sha256_file "$AARCH64_ARCHIVE")" \
        ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
        "$PACKAGER" "$OUTPUT" "$AARCH64_ARCHIVE" "$FAILING_FAKEFSIFY" \
        >/dev/null 2>&1; then
    echo "错误：fakefsify 转换失败不应替换既有 rootfs。" >&2
    exit 1
fi
[[ $(sed -n '1p' "$FAILING_FAKEFSIFY_LOG") == called ]]
[[ -f "$OUTPUT/meta.db" ]]

CACHE="$TMP/cache"
mkdir "$CACHE"
printf 'corrupt-cache\n' > \
    "$CACHE/alpine-minirootfs-3.24.1-aarch64.tar.gz"
if env ISH_AARCH64_ROOTFS_CACHE="$CACHE" \
        ISH_AARCH64_ROOTFS_OFFLINE=1 \
        "$PACKAGER" "$TMP/cache-output" "" "$FAKEFSIFY" \
        >/dev/null 2>&1; then
    echo "错误：离线模式不能接受损坏的缓存。" >&2
    exit 1
fi
[[ ! -e "$TMP/cache-output" ]]

SLOW_FAKEFSIFY="$TMP/slow-fakefsify"
SLOW_LOG="$TMP/slow-fakefsify.log"
printf '%s\n' \
    '#!/bin/bash' \
    'set -euo pipefail' \
    'printf "start %s\n" "$$" >> "$SLOW_FAKEFSIFY_LOG"' \
    'sleep 0.25' \
    '"$REAL_FAKEFSIFY" "$@"' \
    'printf "end %s\n" "$$" >> "$SLOW_FAKEFSIFY_LOG"' \
    > "$SLOW_FAKEFSIFY"
chmod +x "$SLOW_FAKEFSIFY"
digest=$(sha256_file "$AARCH64_ARCHIVE")
env REAL_FAKEFSIFY="$FAKEFSIFY" SLOW_FAKEFSIFY_LOG="$SLOW_LOG" \
    ISH_AARCH64_ROOTFS_LOCK_TIMEOUT=10 \
    ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
    ISH_AARCH64_ROOTFS_TEST_SHA256="$digest" \
    ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
    "$PACKAGER" "$OUTPUT" "$AARCH64_ARCHIVE" "$SLOW_FAKEFSIFY" \
        >/dev/null &
first_pid=$!
child_pids=$first_pid
env REAL_FAKEFSIFY="$FAKEFSIFY" SLOW_FAKEFSIFY_LOG="$SLOW_LOG" \
    ISH_AARCH64_ROOTFS_LOCK_TIMEOUT=10 \
    ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
    ISH_AARCH64_ROOTFS_TEST_SHA256="$digest" \
    ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
    "$PACKAGER" "$OUTPUT" "$AARCH64_ARCHIVE" "$SLOW_FAKEFSIFY" \
        >/dev/null &
second_pid=$!
child_pids="$child_pids $second_pid"
wait "$first_pid"
wait "$second_pid"
child_pids=
[[ -f "$OUTPUT/meta.db" && -f "$OUTPUT/rootfs-manifest.txt" ]]
if [[ $(awk '{print $1}' "$SLOW_LOG" | paste -sd, -) != \
        'start,end,start,end' ]]; then
    echo "错误：同一输出的 fakefs 转换没有被锁串行化。" >&2
    exit 1
fi

if env ISH_AARCH64_ROOTFS_TEST_MODE=fixture \
        ISH_AARCH64_ROOTFS_TEST_SHA256="$(sha256_file "$AARCH64_ARCHIVE")" \
        ISH_AARCH64_ROOTFS_TEST_PACKAGES="$FIXTURE_PACKAGES" \
        "$PACKAGER" "$TMP/nested/.." "$AARCH64_ARCHIVE" "$FAKEFSIFY" \
        >/dev/null 2>&1; then
    echo "错误：输出目录不能接受 ..。" >&2
    exit 1
fi
NEWLINE_OUTPUT="$TMP/"$'line\nbreak'
TRAILING_NEWLINE_OUTPUT="$TMP/rootfs"$'\n'
PARENT_NEWLINE_OUTPUT="$TMP/parent"$'\n'"/rootfs"
for invalid_output in "$NEWLINE_OUTPUT" "$TRAILING_NEWLINE_OUTPUT" \
        "$PARENT_NEWLINE_OUTPUT"; do
    if run_packager "$AARCH64_ARCHIVE" "$invalid_output" \
            >/dev/null 2>&1; then
        echo "错误：输出路径不能包含换行符。" >&2
        exit 1
    fi
done

HARDLINK_GROUP_INPUT="$TMP/hardlink-group-input"
HARDLINK_GROUP_ACTUAL="$TMP/hardlink-group-actual"
HARDLINK_GROUP_EXPECTED="$TMP/hardlink-group-expected"
printf '%s\n' \
    $'9007199254740992\ta' \
    $'9007199254740992\tc' \
    $'9007199254740993\tb' \
    $'9007199254740993\td' > "$HARDLINK_GROUP_INPUT"
awk -F '\t' -f "$ROOT/tools/apple-aarch64-hardlinks.awk" \
    "$HARDLINK_GROUP_INPUT" > "$HARDLINK_GROUP_ACTUAL"
printf '%s\n' $'a\ta' $'a\tc' $'b\tb' $'b\td' \
    > "$HARDLINK_GROUP_EXPECTED"
if ! cmp -s "$HARDLINK_GROUP_EXPECTED" "$HARDLINK_GROUP_ACTUAL"; then
    echo "错误：大 inode 必须按完整十进制字符串分组。" >&2
    exit 1
fi
if find "$TMP" -maxdepth 1 -name '.result.*' -print -quit | grep -q .; then
    echo "错误：事务替换留下了临时目录。" >&2
    exit 1
fi

echo "AArch64 rootfs 打包测试通过"

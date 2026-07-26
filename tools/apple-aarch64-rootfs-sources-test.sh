#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/tools/apple-aarch64-rootfs-sources.py"
PYTHON=${1:-}

if [[ ! -x "$PYTHON" ]]; then
    echo "用法：$0 <python3>" >&2
    exit 2
fi

"$PYTHON" "$TOOL" check-locks >/dev/null

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

sha512_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 512 "$1" | awk '{print $1}'
    else
        sha512sum "$1" | awk '{print $1}'
    fi
}

size_file() {
    wc -c < "$1" | tr -d ' '
}

expect_failure() {
    local expected=$1
    local error_file=$2
    shift 2
    if "$@" >/dev/null 2>"$error_file"; then
        echo "错误：无效的对应源码输入未被拒绝。" >&2
        exit 1
    fi
    if ! grep -q '^错误：' "$error_file" \
            || ! grep -Fq "$expected" "$error_file" \
            || grep -Fq 'Traceback (most recent call last)' "$error_file"; then
        echo "错误：无效输入没有命中预期校验门禁。" >&2
        cat "$error_file" >&2
        exit 1
    fi
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ish-aarch64-sources-test.XXXXXX")
cleanup() {
    local exit_code=$?
    trap - EXIT HUP INT TERM
    find "$TMP" -depth -delete
    exit "$exit_code"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

PRODUCTION_STATIC="$ROOT/third_party/alpine/3.24.1-aarch64/static-link-sources.tsv"
PRODUCTION_STATIC_DRIFT="$TMP/static-link-sources-production-drift.tsv"
sed 's/skalibs-static	2\.14\.5\.0-r0/skalibs-static	9-r0/' \
    "$PRODUCTION_STATIC" > "$PRODUCTION_STATIC_DRIFT"
expect_failure "固定 Alpine 静态链接来源语义集合不一致" \
    "$TMP/production-static.stderr" \
    "$PYTHON" "$TOOL" check-locks \
    --static-link-sources "$PRODUCTION_STATIC_DRIFT"

ALPHA_COMMIT=1111111111111111111111111111111111111111
BETA_COMMIT=2222222222222222222222222222222222222222
SOURCE="$TMP/source"
APORTS_BUILD="$TMP/aports-build"
mkdir -p "$SOURCE" "$APORTS_BUILD"
git -C "$APORTS_BUILD" init -q

ALPHA_DISTFILE="$SOURCE/alpha-source.tar.gz"
BETA_DISTFILE="$SOURCE/beta-source.txt"
GAMMA_DISTFILE="$SOURCE/gamma-source.tar.gz"
printf 'alpha upstream\n' > "$ALPHA_DISTFILE"
printf 'beta upstream\n' > "$BETA_DISTFILE"
printf 'gamma static upstream\n' > "$GAMMA_DISTFILE"

make_aports_archive() {
    local origin=$1
    local commit=$2
    local destination=$3
    local upstream=$4
    local upstream_name=$5
    local with_symlink=${6:-0}
    local wrapper="aports-$commit-$commit-main-$origin"
    local tree="$APORTS_BUILD/$wrapper/main/$origin"
    mkdir -p "$tree"
    printf 'pkgname=%s\n' "$origin" > "$tree/APKBUILD"
    if [[ "$origin" == gamma ]]; then
        printf 'pkgver=3\npkgrel=0\n' >> "$tree/APKBUILD"
    fi
    if [[ "$origin" == alpha ]]; then
        printf 'fixture patch\n' > "$tree/fix.patch"
    fi
    if [[ "$with_symlink" == 1 ]]; then
        ln -s fix.patch "$tree/current.patch"
    fi
    printf 'sha512sums="\n' >> "$tree/APKBUILD"
    if [[ "$origin" == alpha ]]; then
        printf '%s  fix.patch\n' "$(sha512_file "$tree/fix.patch")" \
            >> "$tree/APKBUILD"
    fi
    printf '%s  %s\n' "$(sha512_file "$upstream")" "$upstream_name" \
        >> "$tree/APKBUILD"
    printf '"\n' >> "$tree/APKBUILD"
    git -C "$APORTS_BUILD" add -f -- "$wrapper"
    (cd "$APORTS_BUILD" && tar -czf "$destination" "$wrapper")
}

ALPHA_APORTS="$SOURCE/alpha-aports.tar.gz"
BETA_APORTS="$SOURCE/beta-aports.tar.gz"
GAMMA_APORTS="$SOURCE/gamma-aports.tar.gz"
make_aports_archive alpha "$ALPHA_COMMIT" "$ALPHA_APORTS" \
    "$ALPHA_DISTFILE" "$(basename "$ALPHA_DISTFILE")" 1
make_aports_archive beta "$BETA_COMMIT" "$BETA_APORTS" \
    "$BETA_DISTFILE" "$(basename "$BETA_DISTFILE")"
make_aports_archive gamma "$ALPHA_COMMIT" "$GAMMA_APORTS" \
    "$GAMMA_DISTFILE" "$(basename "$GAMMA_DISTFILE")"
ALPHA_WRAPPER="aports-$ALPHA_COMMIT-$ALPHA_COMMIT-main-alpha"
BETA_WRAPPER="aports-$BETA_COMMIT-$BETA_COMMIT-main-beta"
GAMMA_WRAPPER="aports-$ALPHA_COMMIT-$ALPHA_COMMIT-main-gamma"
INDEX_TREE=$(git -C "$APORTS_BUILD" write-tree)
ALPHA_TREE=$(git -C "$APORTS_BUILD" rev-parse \
    "$INDEX_TREE:$ALPHA_WRAPPER/main/alpha")
BETA_TREE=$(git -C "$APORTS_BUILD" rev-parse \
    "$INDEX_TREE:$BETA_WRAPPER/main/beta")
GAMMA_TREE=$(git -C "$APORTS_BUILD" rev-parse \
    "$INDEX_TREE:$GAMMA_WRAPPER/main/gamma")
ALPHA_COUNT=$(find "$APORTS_BUILD/$ALPHA_WRAPPER/main/alpha" \
    -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')
BETA_COUNT=$(find "$APORTS_BUILD/$BETA_WRAPPER/main/beta" \
    -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')
GAMMA_COUNT=$(find "$APORTS_BUILD/$GAMMA_WRAPPER/main/gamma" \
    -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')

PACKAGES="$TMP/packages.tsv"
STATIC_LINK_SOURCES="$TMP/static-link-sources.tsv"
ORIGINS="$TMP/origins.tsv"
ASSETS="$TMP/source-assets.tsv"
BINARY_REFERENCE="$TMP/binary-reference.tsv"
README="$TMP/SOURCE-BUNDLE.md"
printf '%s\n' \
    $'package\tversion\torigin\tlicense\taports_commit' \
    $'alpha-bin\t1-r0\talpha\tMIT\t1111111111111111111111111111111111111111' \
    $'alpha-utils\t1-r0\talpha\tMIT\t1111111111111111111111111111111111111111' \
    $'beta-bin\t2-r0\tbeta\tBSD-2-Clause\t2222222222222222222222222222222222222222' \
    > "$PACKAGES"
printf '%s\n' \
    $'binary_package\tsource_package\tsource_version\tsource_origin\tsource_license\taports_snapshot_commit' \
    "alpha-bin@1-r0"$'\tgamma-static\t3-r0\tgamma\tISC\t'"$ALPHA_COMMIT" \
    > "$STATIC_LINK_SOURCES"
printf '%s\n' \
    $'origin\taports_commit\taports_path\torigin_tree_sha1\tentry_count' \
    "alpha"$'\t'"$ALPHA_COMMIT"$'\tmain/alpha\t'"$ALPHA_TREE"$'\t'"$ALPHA_COUNT" \
    "beta"$'\t'"$BETA_COMMIT"$'\tmain/beta\t'"$BETA_TREE"$'\t'"$BETA_COUNT" \
    "gamma"$'\t'"$ALPHA_COMMIT"$'\tmain/gamma\t'"$GAMMA_TREE"$'\t'"$GAMMA_COUNT" \
    > "$ORIGINS"
printf '%s\n' \
    $'kind\torigin\taports_commit\tbundle_path\tsize\tsha512\tsource_url' \
    "aports"$'\talpha\t'"$ALPHA_COMMIT"$'\taports/alpha.tar.gz\t'"$(size_file "$ALPHA_APORTS")"$'\t'"$(sha512_file "$ALPHA_APORTS")"$'\tfile://'"$ALPHA_APORTS" \
    "aports"$'\tbeta\t'"$BETA_COMMIT"$'\taports/beta.tar.gz\t'"$(size_file "$BETA_APORTS")"$'\t'"$(sha512_file "$BETA_APORTS")"$'\tfile://'"$BETA_APORTS" \
    "aports"$'\tgamma\t'"$ALPHA_COMMIT"$'\taports/gamma.tar.gz\t'"$(size_file "$GAMMA_APORTS")"$'\t'"$(sha512_file "$GAMMA_APORTS")"$'\tfile://'"$GAMMA_APORTS" \
    "upstream"$'\talpha\t'"$ALPHA_COMMIT"$'\tdistfiles/alpha/alpha-source.tar.gz\t'"$(size_file "$ALPHA_DISTFILE")"$'\t'"$(sha512_file "$ALPHA_DISTFILE")"$'\tfile://'"$ALPHA_DISTFILE" \
    "upstream"$'\tbeta\t'"$BETA_COMMIT"$'\tdistfiles/beta/beta-source.txt\t'"$(size_file "$BETA_DISTFILE")"$'\t'"$(sha512_file "$BETA_DISTFILE")"$'\tfile://'"$BETA_DISTFILE" \
    "upstream"$'\tgamma\t'"$ALPHA_COMMIT"$'\tdistfiles/gamma/gamma-source.tar.gz\t'"$(size_file "$GAMMA_DISTFILE")"$'\t'"$(sha512_file "$GAMMA_DISTFILE")"$'\tfile://'"$GAMMA_DISTFILE" \
    > "$ASSETS"
printf 'fixture source bundle\n' > "$README"
printf '%s\n' \
    $'alpine_version\tarchive_name\tarchive_size\tarchive_sha256\tsource_url\tinstalled_size\tinstalled_sha256\tpackages_size\tpackages_sha256\tpackage_count\torigin_count' \
    "1.2.3"$'\talpine-minirootfs-1.2.3-aarch64.tar.gz\t123\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\thttps://dl-cdn.alpinelinux.org/alpine/v1.2/releases/aarch64/alpine-minirootfs-1.2.3-aarch64.tar.gz\t45\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t'"$(size_file "$PACKAGES")"$'\t'"$(sha256_file "$PACKAGES")"$'\t3\t2' \
    > "$BINARY_REFERENCE"

COMMON=(
    --packages "$PACKAGES"
    --static-link-sources "$STATIC_LINK_SOURCES"
    --origins "$ORIGINS"
    --assets "$ASSETS"
    --binary-reference "$BINARY_REFERENCE"
    --readme "$README"
    --allow-local-sources
)
CACHE="$TMP/cache"
"$PYTHON" "$TOOL" fetch "$CACHE" "${COMMON[@]}" >/dev/null

ATOMIC_CACHE_LEAF="$CACHE/distfiles/alpha/alpha-source.tar.gz"
ALPHA_DISTFILE_GOOD="$TMP/alpha-source.good"
cp "$ALPHA_DISTFILE" "$ALPHA_DISTFILE_GOOD"
printf 'bad cache sentinel\n' > "$ATOMIC_CACHE_LEAF"
ATOMIC_SENTINEL_SHA=$(sha256_file "$ATOMIC_CACHE_LEAF")
printf 'source drift\n' >> "$ALPHA_DISTFILE"
expect_failure "下载的源码资产超过锁定大小" \
    "$TMP/download-drift.stderr" \
    "$PYTHON" "$TOOL" fetch "$CACHE" "${COMMON[@]}"
if [[ $(sha256_file "$ATOMIC_CACHE_LEAF") != "$ATOMIC_SENTINEL_SHA" ]]; then
    echo "错误：下载失败时替换了既有缓存叶节点。" >&2
    exit 1
fi
if find "$TMP" -name '.source-download.*' -print -quit | grep -q .; then
    echo "错误：下载失败后留下了源码临时文件。" >&2
    exit 1
fi
cp "$ALPHA_DISTFILE_GOOD" "$ALPHA_DISTFILE"
"$PYTHON" "$TOOL" fetch "$CACHE" "${COMMON[@]}" >/dev/null
"$PYTHON" "$TOOL" fetch "$CACHE" --offline \
    "${COMMON[@]}" >/dev/null
expect_failure "源码缓存不存在" "$TMP/missing-cache.stderr" \
    "$PYTHON" "$TOOL" fetch "$TMP/missing-cache" --offline \
    "${COMMON[@]}"

mv "$PACKAGES" "$TMP/packages.saved"
ln -s packages.saved "$PACKAGES"
expect_failure "二进制包清单不存在或是符号链接" \
    "$TMP/package-lock-symlink.stderr" \
    "$PYTHON" "$TOOL" check-locks "${COMMON[@]}"
rm "$PACKAGES"
mv "$TMP/packages.saved" "$PACKAGES"

BETA_CACHE_LEAF="$CACHE/distfiles/beta/beta-source.txt"
mv "$BETA_CACHE_LEAF" "$CACHE/distfiles/beta/beta-source.saved"
ln -s beta-source.saved "$BETA_CACHE_LEAF"
expect_failure "源码资产不存在或是符号链接" \
    "$TMP/cache-leaf-symlink.stderr" \
    "$PYTHON" "$TOOL" fetch "$CACHE" --offline "${COMMON[@]}"
rm "$BETA_CACHE_LEAF"
mv "$CACHE/distfiles/beta/beta-source.saved" "$BETA_CACHE_LEAF"

mv "$CACHE/distfiles/beta" "$CACHE/distfiles/beta.saved"
ln -s beta.saved "$CACHE/distfiles/beta"
expect_failure "源码缓存父路径不存在、含符号链接或非实体目录" \
    "$TMP/cache-parent-symlink.stderr" \
    "$PYTHON" "$TOOL" fetch "$CACHE" --offline "${COMMON[@]}"
rm "$CACHE/distfiles/beta"
mv "$CACHE/distfiles/beta.saved" "$CACHE/distfiles/beta"

mv "$BETA_CACHE_LEAF" "$CACHE/distfiles/beta/beta-source.saved"
mkfifo "$BETA_CACHE_LEAF"
expect_failure "源码资产必须是普通文件" \
    "$TMP/cache-leaf-fifo.stderr" \
    "$PYTHON" "$TOOL" fetch "$CACHE" --offline "${COMMON[@]}"
rm "$BETA_CACHE_LEAF"
mv "$CACHE/distfiles/beta/beta-source.saved" "$BETA_CACHE_LEAF"

FIRST="$TMP/first.tar"
SECOND="$TMP/second.tar"
"$PYTHON" "$TOOL" bundle "$CACHE" "$FIRST" \
    "${COMMON[@]}" >/dev/null
"$PYTHON" "$TOOL" bundle "$CACHE" "$SECOND" \
    "${COMMON[@]}" >/dev/null
if ! cmp -s "$FIRST" "$SECOND"; then
    echo "错误：相同源码输入没有生成逐字节一致的 tar。" >&2
    exit 1
fi
STATIC_MEMBER="alpine-minirootfs-3.24.1-aarch64-corresponding-source/manifest/static-link-sources.tsv"
if ! tar -tf "$FIRST" | grep -Fxq "$STATIC_MEMBER"; then
    echo "错误：对应源码包缺少静态链接来源清单。" >&2
    exit 1
fi

CHECKSUM="$TMP/first.sha256"
printf '%s  %s\n' "$(sha256_file "$FIRST")" "$(basename "$FIRST")" \
    > "$CHECKSUM"
"$PYTHON" "$TOOL" verify "$FIRST" --checksum "$CHECKSUM" \
    "${COMMON[@]}" >/dev/null
VERIFY_FIFO="$TMP/verify-fifo.tar"
VERIFY_FIFO_CHECKSUM="$TMP/verify-fifo.sha256"
mkfifo "$VERIFY_FIFO"
printf '%064d  %s\n' 0 "$(basename "$VERIFY_FIFO")" \
    > "$VERIFY_FIFO_CHECKSUM"
expect_failure "对应源码包必须是普通文件" \
    "$TMP/verify-fifo.stderr" \
    "$PYTHON" "$TOOL" verify "$VERIFY_FIFO" \
    --checksum "$VERIFY_FIFO_CHECKSUM" "${COMMON[@]}"
rm "$VERIFY_FIFO"

MISSING_ORIGIN_PACKAGES="$TMP/packages-missing-origin.tsv"
COMMIT_DRIFT_PACKAGES="$TMP/packages-commit-drift.tsv"
URL_DRIFT_BINARY_REFERENCE="$TMP/binary-reference-url-drift.tsv"
MALFORMED_URL_ASSETS="$TMP/assets-malformed-url.tsv"
BAD_PORT_ASSETS="$TMP/assets-bad-port.tsv"
CONTROL_URL_ASSETS="$TMP/assets-control-url.tsv"
C0_URL_ASSETS="$TMP/assets-c0-url.tsv"
DEL_URL_ASSETS="$TMP/assets-del-url.tsv"
EMPTY_FRAGMENT_URL_ASSETS="$TMP/assets-empty-fragment-url.tsv"
CONTROL_PATH_ASSETS="$TMP/assets-control-path.tsv"
MISSING_APORTS_ASSETS="$TMP/assets-missing-aports.tsv"
MISSING_UPSTREAM_ASSETS="$TMP/assets-missing-upstream.tsv"
EXTRA_UPSTREAM_ASSETS="$TMP/assets-extra-upstream.tsv"
TRAVERSAL_ASSETS="$TMP/assets-traversal.tsv"
DUPLICATE_ASSETS="$TMP/assets-duplicate.tsv"
MISSING_STATIC_SOURCES="$TMP/static-link-sources-missing.tsv"
DUPLICATE_STATIC_SOURCES="$TMP/static-link-sources-duplicate.tsv"
BAD_CONSUMER_STATIC_SOURCES="$TMP/static-link-sources-bad-consumer.tsv"
BAD_LICENSE_STATIC_SOURCES="$TMP/static-link-sources-bad-license.tsv"
BAD_VERSION_STATIC_SOURCES="$TMP/static-link-sources-bad-version.tsv"
BAD_COMMIT_STATIC_SOURCES="$TMP/static-link-sources-bad-commit.tsv"
BAD_ORIGIN_STATIC_SOURCES="$TMP/static-link-sources-bad-origin.tsv"
BAD_SOURCE_PACKAGE_STATIC_SOURCES="$TMP/static-link-sources-bad-package.tsv"
MISSING_STATIC_ASSETS="$TMP/assets-missing-static.tsv"
DUPLICATE_VERSION_APORTS="$SOURCE/gamma-duplicate-version-aports.tar.gz"
DUPLICATE_VERSION_ORIGINS="$TMP/origins-duplicate-version.tsv"
DUPLICATE_VERSION_ASSETS="$TMP/assets-duplicate-version.tsv"
DUPLICATE_VERSION_CACHE="$TMP/cache-duplicate-version"
UNREFERENCED_DISTFILE="$SOURCE/unreferenced-source.txt"
sed '/^beta-bin	/d' "$PACKAGES" > "$MISSING_ORIGIN_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 4 {
        $5 = "3333333333333333333333333333333333333333"
    } { print }' "$PACKAGES" > "$COMMIT_DRIFT_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $5 = "https://example.invalid/alpine-minirootfs-1.2.3-aarch64.tar.gz"
    } { print }' "$BINARY_REFERENCE" > "$URL_DRIFT_BINARY_REFERENCE"
"$PYTHON" - "$ASSETS" "$MALFORMED_URL_ASSETS" \
    "$BAD_PORT_ASSETS" "$CONTROL_URL_ASSETS" "$C0_URL_ASSETS" \
    "$DEL_URL_ASSETS" "$EMPTY_FRAGMENT_URL_ASSETS" \
    "$CONTROL_PATH_ASSETS" <<'PY'
from pathlib import Path
import sys

(
    source,
    malformed,
    bad_port,
    control,
    c0,
    delete,
    empty_fragment,
    control_path,
) = map(Path, sys.argv[1:])
lines = source.read_text(encoding="utf-8").splitlines()
rows = [line.split("\t") for line in lines[1:]]
for destination, url in (
    (malformed, "https://[::1"),
    (bad_port, "https://example.invalid:bad/source"),
    (control, "https://example.invalid/source\u0000"),
    (c0, "https://example.invalid/source\u0001"),
    (delete, "https://example.invalid/source\u007f"),
    (empty_fragment, "https://example.invalid/source#"),
):
    changed = [row[:] for row in rows]
    changed[0][6] = url
    destination.write_text(
        lines[0]
        + "\n"
        + "\n".join("\t".join(fields) for fields in sorted(changed))
        + "\n",
        encoding="utf-8",
    )
changed = [row[:] for row in rows]
changed[0][3] = "aports/\u0000alpha.tar.gz"
control_path.write_text(
    lines[0]
    + "\n"
    + "\n".join("\t".join(fields) for fields in sorted(changed))
    + "\n",
    encoding="utf-8",
)
PY
awk -F '\t' '$1 != "aports" || $2 != "beta"' "$ASSETS" \
    > "$MISSING_APORTS_ASSETS"
awk -F '\t' '$1 != "upstream" || $2 != "alpha"' "$ASSETS" \
    > "$MISSING_UPSTREAM_ASSETS"
printf 'unreferenced upstream\n' > "$UNREFERENCED_DISTFILE"
cp "$ASSETS" "$EXTRA_UPSTREAM_ASSETS"
printf '%s\n' \
    "upstream"$'\tgamma\t'"$ALPHA_COMMIT"$'\tdistfiles/gamma/unreferenced-source.txt\t'"$(size_file "$UNREFERENCED_DISTFILE")"$'\t'"$(sha512_file "$UNREFERENCED_DISTFILE")"$'\tfile://'"$UNREFERENCED_DISTFILE" \
    >> "$EXTRA_UPSTREAM_ASSETS"
cp "$UNREFERENCED_DISTFILE" \
    "$CACHE/distfiles/gamma/unreferenced-source.txt"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $4 = "../escape.tar.gz"
    } { print }' "$ASSETS" > "$TRAVERSAL_ASSETS"
sed -n '1p;2p;2p;3,$p' "$ASSETS" > "$DUPLICATE_ASSETS"
sed -n '1p' "$STATIC_LINK_SOURCES" > "$MISSING_STATIC_SOURCES"
sed -n '1p;2p;2p' "$STATIC_LINK_SOURCES" \
    > "$DUPLICATE_STATIC_SOURCES"
sed 's/^alpha-bin@1-r0	/unknown@1-r0	/' "$STATIC_LINK_SOURCES" \
    > "$BAD_CONSUMER_STATIC_SOURCES"
sed 's/	ISC	/	MIT	/' "$STATIC_LINK_SOURCES" \
    > "$BAD_LICENSE_STATIC_SOURCES"
sed 's/	gamma-static	3-r0	/	gamma-static	4-r0	/' \
    "$STATIC_LINK_SOURCES" > "$BAD_VERSION_STATIC_SOURCES"
sed "s/$ALPHA_COMMIT/$BETA_COMMIT/" "$STATIC_LINK_SOURCES" \
    > "$BAD_COMMIT_STATIC_SOURCES"
sed 's/	gamma-static	3-r0	gamma	/	alpha-static	3-r0	alpha	/' \
    "$STATIC_LINK_SOURCES" > "$BAD_ORIGIN_STATIC_SOURCES"
sed 's/	gamma-static	3-r0	gamma	/	other-static	3-r0	gamma	/' \
    "$STATIC_LINK_SOURCES" > "$BAD_SOURCE_PACKAGE_STATIC_SOURCES"
awk -F '\t' '$1 != "aports" || $2 != "gamma"' "$ASSETS" \
    > "$MISSING_STATIC_ASSETS"

GAMMA_APKBUILD="$APORTS_BUILD/$GAMMA_WRAPPER/main/gamma/APKBUILD"
awk '{
        print
        if ($0 == "pkgver=3")
            print "pkgver=9"
    }' "$GAMMA_APKBUILD" > "$TMP/APKBUILD-duplicate-version"
mv "$TMP/APKBUILD-duplicate-version" "$GAMMA_APKBUILD"
git -C "$APORTS_BUILD" add -f -- "$GAMMA_WRAPPER"
DUPLICATE_VERSION_INDEX_TREE=$(git -C "$APORTS_BUILD" write-tree)
DUPLICATE_VERSION_GAMMA_TREE=$(git -C "$APORTS_BUILD" rev-parse \
    "$DUPLICATE_VERSION_INDEX_TREE:$GAMMA_WRAPPER/main/gamma")
(cd "$APORTS_BUILD" \
    && tar -czf "$DUPLICATE_VERSION_APORTS" "$GAMMA_WRAPPER")
awk -v tree="$DUPLICATE_VERSION_GAMMA_TREE" \
    'BEGIN { FS = OFS = "\t" } $1 == "gamma" {
        $4 = tree
    } { print }' "$ORIGINS" > "$DUPLICATE_VERSION_ORIGINS"
awk -v size="$(size_file "$DUPLICATE_VERSION_APORTS")" \
    -v digest="$(sha512_file "$DUPLICATE_VERSION_APORTS")" \
    -v url="file://$DUPLICATE_VERSION_APORTS" \
    'BEGIN { FS = OFS = "\t" } $1 == "aports" && $2 == "gamma" {
        $5 = size
        $6 = digest
        $7 = url
    } { print }' "$ASSETS" > "$DUPLICATE_VERSION_ASSETS"
cp -R "$CACHE" "$DUPLICATE_VERSION_CACHE"
rm "$DUPLICATE_VERSION_CACHE/distfiles/gamma/unreferenced-source.txt"
cp "$DUPLICATE_VERSION_APORTS" \
    "$DUPLICATE_VERSION_CACHE/aports/gamma.tar.gz"

expect_failure "aports origin 清单与二进制包及静态链接源码集合不一致" \
    "$TMP/missing-origin.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$MISSING_ORIGIN_PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "aports origin 的锁定提交与包提交或静态链接源码快照不一致" \
    "$TMP/commit-drift.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$COMMIT_DRIFT_PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "二进制参照清单与受跟踪包清单不一致" \
    "$TMP/binary-reference-url.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$URL_DRIFT_BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码 URL 格式非法" "$TMP/malformed-url.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$MALFORMED_URL_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码 URL 格式非法" "$TMP/bad-port.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$BAD_PORT_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码 URL 含片段、空白或控制字符" \
    "$TMP/control-url.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$CONTROL_URL_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码 URL 含片段、空白或控制字符" \
    "$TMP/c0-url.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$C0_URL_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码 URL 含片段、空白或控制字符" \
    "$TMP/del-url.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$DEL_URL_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码 URL 含片段、空白或控制字符" \
    "$TMP/empty-fragment-url.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$EMPTY_FRAGMENT_URL_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码包相对路径非法" "$TMP/control-path.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$CONTROL_PATH_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "未闭合 APKBUILD sha512sums" \
    "$TMP/missing-upstream.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$MISSING_UPSTREAM_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "未闭合 APKBUILD sha512sums" \
    "$TMP/extra-upstream.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$EXTRA_UPSTREAM_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "每个锁定 origin 必须恰有一份 aports 资产" \
    "$TMP/missing-aports.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$MISSING_APORTS_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码包相对路径非法" "$TMP/traversal.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$TRAVERSAL_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "源码资产清单排序、来源、大小或摘要格式非法" \
    "$TMP/duplicate-assets.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --origins "$ORIGINS" \
    --assets "$DUPLICATE_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接来源清单没有数据行" \
    "$TMP/missing-static.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$MISSING_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接来源清单未按完整记录唯一排序" \
    "$TMP/duplicate-static.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$DUPLICATE_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接来源声明了未知二进制包" \
    "$TMP/bad-static-consumer.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_CONSUMER_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接来源的包、版本、origin、许可或快照格式非法" \
    "$TMP/bad-static-license.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_LICENSE_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "gamma 的静态链接来源版本与 APKBUILD 不一致" \
    "$TMP/bad-static-version.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_VERSION_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "gamma 的静态链接来源版本与 APKBUILD 不一致" \
    "$TMP/duplicate-apkbuild-version.stderr" \
    "$PYTHON" "$TOOL" bundle "$DUPLICATE_VERSION_CACHE" \
    "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --origins "$DUPLICATE_VERSION_ORIGINS" \
    --assets "$DUPLICATE_VERSION_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接来源的 aports snapshot 与目标二进制包不一致" \
    "$TMP/bad-static-commit.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_COMMIT_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接源码 origin 不能冒充二进制包 origin" \
    "$TMP/bad-static-origin.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_ORIGIN_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "静态链接来源的包、版本、origin、许可或快照格式非法" \
    "$TMP/bad-static-package.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_SOURCE_PACKAGE_STATIC_SOURCES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "每个锁定 origin 必须恰有一份 aports 资产" \
    "$TMP/missing-static-assets.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --origins "$ORIGINS" --assets "$MISSING_STATIC_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources

PROTECTED="$TMP/protected.tar"
printf 'keep existing output\n' > "$PROTECTED"
PROTECTED_SHA=$(sha256_file "$PROTECTED")
printf 'drift\n' >> "$CACHE/distfiles/alpha/alpha-source.tar.gz"
expect_failure "源码资产大小或 SHA-512 不匹配" \
    "$TMP/protected-output.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$PROTECTED" "${COMMON[@]}"
if [[ $(sha256_file "$PROTECTED") != "$PROTECTED_SHA" ]]; then
    echo "错误：源码资产漂移时替换了既有输出。" >&2
    exit 1
fi
"$PYTHON" "$TOOL" fetch "$CACHE" "${COMMON[@]}" >/dev/null
"$PYTHON" "$TOOL" fetch "$CACHE" --offline \
    "${COMMON[@]}" >/dev/null

TAMPERED="$TMP/tampered.tar"
cp "$FIRST" "$TAMPERED"
printf 'tamper\n' >> "$TAMPERED"
TAMPERED_CHECKSUM="$TMP/tampered.sha256"
printf '%s  %s\n' "$(sha256_file "$FIRST")" "$(basename "$TAMPERED")" \
    > "$TAMPERED_CHECKSUM"
expect_failure "对应源码包 SHA-256 不匹配" "$TMP/tampered.stderr" \
    "$PYTHON" "$TOOL" verify "$TAMPERED" \
    --checksum "$TAMPERED_CHECKSUM" "${COMMON[@]}"

MALICIOUS="$TMP/malicious.tar"
README_MEMBER="alpine-minirootfs-3.24.1-aarch64-corresponding-source/README.md"
"$PYTHON" - "$FIRST" "$MALICIOUS" "$README_MEMBER" <<'PY'
import sys
import tarfile

source_name, destination_name, readme_name = sys.argv[1:]
with tarfile.open(source_name, mode="r:") as source:
    with tarfile.open(
        destination_name, mode="w", format=tarfile.USTAR_FORMAT
    ) as destination:
        for member in source.getmembers():
            if member.name == readme_name:
                member.type = tarfile.SYMTYPE
                member.linkname = "manifest/packages.tsv"
                member.size = 0
                destination.addfile(member)
            else:
                extracted = source.extractfile(member)
                if extracted is None:
                    raise RuntimeError("无法读取合法源码包成员")
                destination.addfile(member, extracted)
PY
MALICIOUS_CHECKSUM="$TMP/malicious.sha256"
printf '%s  %s\n' "$(sha256_file "$MALICIOUS")" \
    "$(basename "$MALICIOUS")" > "$MALICIOUS_CHECKSUM"
MALICIOUS_ERROR="$TMP/malicious.stderr"
if "$PYTHON" "$TOOL" verify "$MALICIOUS" \
        --checksum "$MALICIOUS_CHECKSUM" \
        "${COMMON[@]}" >/dev/null 2>"$MALICIOUS_ERROR"; then
    echo "错误：源码包内的符号链接节点未被拒绝。" >&2
    exit 1
fi
if ! grep -q '^错误：' "$MALICIOUS_ERROR" \
        || ! grep -Fq '源码包成员元数据不规范' "$MALICIOUS_ERROR" \
        || grep -Fq 'Traceback (most recent call last)' "$MALICIOUS_ERROR"; then
    echo "错误：恶意源码包未命中节点类型门禁。" >&2
    exit 1
fi

DUPLICATE_MEMBER="$TMP/duplicate-member.tar"
"$PYTHON" - "$FIRST" "$DUPLICATE_MEMBER" "$README_MEMBER" <<'PY'
import io
import sys
import tarfile

source_name, destination_name, readme_name = sys.argv[1:]
with tarfile.open(source_name, mode="r:") as source:
    with tarfile.open(
        destination_name, mode="w", format=tarfile.USTAR_FORMAT
    ) as destination:
        duplicate = None
        for member in source.getmembers():
            extracted = source.extractfile(member)
            if extracted is None:
                raise RuntimeError("无法读取合法源码包成员")
            data = extracted.read()
            destination.addfile(member, fileobj=io.BytesIO(data))
            if member.name == readme_name:
                duplicate = (member, data)
        if duplicate is None:
            raise RuntimeError("合法源码包缺少 README 成员")
        member, data = duplicate
        destination.addfile(member, fileobj=io.BytesIO(data))
PY
DUPLICATE_MEMBER_CHECKSUM="$TMP/duplicate-member.sha256"
printf '%s  %s\n' "$(sha256_file "$DUPLICATE_MEMBER")" \
    "$(basename "$DUPLICATE_MEMBER")" > "$DUPLICATE_MEMBER_CHECKSUM"
DUPLICATE_MEMBER_ERROR="$TMP/duplicate-member.stderr"
if "$PYTHON" "$TOOL" verify "$DUPLICATE_MEMBER" \
        --checksum "$DUPLICATE_MEMBER_CHECKSUM" \
        "${COMMON[@]}" >/dev/null 2>"$DUPLICATE_MEMBER_ERROR"; then
    echo "错误：源码包内的重复成员未被拒绝。" >&2
    exit 1
fi
if ! grep -q '^错误：' "$DUPLICATE_MEMBER_ERROR" \
        || ! grep -Fq '源码包成员集合缺失、重复或含额外文件' \
        "$DUPLICATE_MEMBER_ERROR" \
        || grep -Fq 'Traceback (most recent call last)' \
        "$DUPLICATE_MEMBER_ERROR"; then
    echo "错误：恶意源码包未命中重复成员门禁。" >&2
    exit 1
fi

if find "$TMP" -name '.source-download.*' -print -quit | grep -q .; then
    echo "错误：源码资产测试留下了下载临时文件。" >&2
    exit 1
fi
if find "$TMP" -name '.*.tar.*' -print -quit | grep -q .; then
    echo "错误：源码资产测试留下了 bundle 临时文件。" >&2
    exit 1
fi

echo "Alpine AArch64 对应源码包测试通过"

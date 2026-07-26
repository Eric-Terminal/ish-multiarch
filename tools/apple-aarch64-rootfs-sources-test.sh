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
    if "$@" >/dev/null 2>&1; then
        echo "错误：无效的对应源码输入未被拒绝。" >&2
        exit 1
    fi
}

expect_failure_containing() {
    local expected=$1
    local error_file=$2
    shift 2
    if "$@" >/dev/null 2>"$error_file"; then
        echo "错误：无效的对应源码输入未被拒绝。" >&2
        exit 1
    fi
    if ! grep -F "$expected" "$error_file" >/dev/null; then
        echo "错误：无效输入没有命中预期校验门禁。" >&2
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

ALPHA_COMMIT=1111111111111111111111111111111111111111
BETA_COMMIT=2222222222222222222222222222222222222222
SOURCE="$TMP/source"
APORTS_BUILD="$TMP/aports-build"
mkdir -p "$SOURCE" "$APORTS_BUILD"
git -C "$APORTS_BUILD" init -q

ALPHA_DISTFILE="$SOURCE/alpha-source.tar.gz"
BETA_DISTFILE="$SOURCE/beta-source.txt"
printf 'alpha upstream\n' > "$ALPHA_DISTFILE"
printf 'beta upstream\n' > "$BETA_DISTFILE"

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
make_aports_archive alpha "$ALPHA_COMMIT" "$ALPHA_APORTS" \
    "$ALPHA_DISTFILE" "$(basename "$ALPHA_DISTFILE")" 1
make_aports_archive beta "$BETA_COMMIT" "$BETA_APORTS" \
    "$BETA_DISTFILE" "$(basename "$BETA_DISTFILE")"
ALPHA_WRAPPER="aports-$ALPHA_COMMIT-$ALPHA_COMMIT-main-alpha"
BETA_WRAPPER="aports-$BETA_COMMIT-$BETA_COMMIT-main-beta"
INDEX_TREE=$(git -C "$APORTS_BUILD" write-tree)
ALPHA_TREE=$(git -C "$APORTS_BUILD" rev-parse \
    "$INDEX_TREE:$ALPHA_WRAPPER/main/alpha")
BETA_TREE=$(git -C "$APORTS_BUILD" rev-parse \
    "$INDEX_TREE:$BETA_WRAPPER/main/beta")
ALPHA_COUNT=$(find "$APORTS_BUILD/$ALPHA_WRAPPER/main/alpha" \
    -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')
BETA_COUNT=$(find "$APORTS_BUILD/$BETA_WRAPPER/main/beta" \
    -mindepth 1 -maxdepth 1 | wc -l | tr -d ' ')

PACKAGES="$TMP/packages.tsv"
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
    $'origin\taports_commit\taports_path\torigin_tree_sha1\tentry_count' \
    "alpha"$'\t'"$ALPHA_COMMIT"$'\tmain/alpha\t'"$ALPHA_TREE"$'\t'"$ALPHA_COUNT" \
    "beta"$'\t'"$BETA_COMMIT"$'\tmain/beta\t'"$BETA_TREE"$'\t'"$BETA_COUNT" \
    > "$ORIGINS"
printf '%s\n' \
    $'kind\torigin\taports_commit\tbundle_path\tsize\tsha512\tsource_url' \
    "aports"$'\talpha\t'"$ALPHA_COMMIT"$'\taports/alpha.tar.gz\t'"$(size_file "$ALPHA_APORTS")"$'\t'"$(sha512_file "$ALPHA_APORTS")"$'\tfile://'"$ALPHA_APORTS" \
    "aports"$'\tbeta\t'"$BETA_COMMIT"$'\taports/beta.tar.gz\t'"$(size_file "$BETA_APORTS")"$'\t'"$(sha512_file "$BETA_APORTS")"$'\tfile://'"$BETA_APORTS" \
    "upstream"$'\talpha\t'"$ALPHA_COMMIT"$'\tdistfiles/alpha/alpha-source.tar.gz\t'"$(size_file "$ALPHA_DISTFILE")"$'\t'"$(sha512_file "$ALPHA_DISTFILE")"$'\tfile://'"$ALPHA_DISTFILE" \
    "upstream"$'\tbeta\t'"$BETA_COMMIT"$'\tdistfiles/beta/beta-source.txt\t'"$(size_file "$BETA_DISTFILE")"$'\t'"$(sha512_file "$BETA_DISTFILE")"$'\tfile://'"$BETA_DISTFILE" \
    > "$ASSETS"
printf 'fixture source bundle\n' > "$README"
printf '%s\n' \
    $'alpine_version\tarchive_name\tarchive_size\tarchive_sha256\tsource_url\tinstalled_size\tinstalled_sha256\tpackages_size\tpackages_sha256\tpackage_count\torigin_count' \
    "1.2.3"$'\talpine-minirootfs-1.2.3-aarch64.tar.gz\t123\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\thttps://dl-cdn.alpinelinux.org/alpine/v1.2/releases/aarch64/alpine-minirootfs-1.2.3-aarch64.tar.gz\t45\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\t'"$(size_file "$PACKAGES")"$'\t'"$(sha256_file "$PACKAGES")"$'\t3\t2' \
    > "$BINARY_REFERENCE"

COMMON=(
    --packages "$PACKAGES"
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
expect_failure "$PYTHON" "$TOOL" fetch "$CACHE" "${COMMON[@]}"
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
expect_failure "$PYTHON" "$TOOL" fetch "$TMP/missing-cache" --offline \
    "${COMMON[@]}"

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

CHECKSUM="$TMP/first.sha256"
printf '%s  %s\n' "$(sha256_file "$FIRST")" "$(basename "$FIRST")" \
    > "$CHECKSUM"
"$PYTHON" "$TOOL" verify "$FIRST" --checksum "$CHECKSUM" \
    "${COMMON[@]}" >/dev/null

MISSING_ORIGIN_PACKAGES="$TMP/packages-missing-origin.tsv"
COMMIT_DRIFT_PACKAGES="$TMP/packages-commit-drift.tsv"
URL_DRIFT_BINARY_REFERENCE="$TMP/binary-reference-url-drift.tsv"
MISSING_APORTS_ASSETS="$TMP/assets-missing-aports.tsv"
MISSING_UPSTREAM_ASSETS="$TMP/assets-missing-upstream.tsv"
EXTRA_UPSTREAM_ASSETS="$TMP/assets-extra-upstream.tsv"
TRAVERSAL_ASSETS="$TMP/assets-traversal.tsv"
DUPLICATE_ASSETS="$TMP/assets-duplicate.tsv"
UNREFERENCED_DISTFILE="$SOURCE/unreferenced-source.txt"
sed '/^beta-bin	/d' "$PACKAGES" > "$MISSING_ORIGIN_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 4 {
        $5 = "3333333333333333333333333333333333333333"
    } { print }' "$PACKAGES" > "$COMMIT_DRIFT_PACKAGES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $5 = "https://example.invalid/alpine-minirootfs-1.2.3-aarch64.tar.gz"
    } { print }' "$BINARY_REFERENCE" > "$URL_DRIFT_BINARY_REFERENCE"
awk -F '\t' '$1 != "aports" || $2 != "beta"' "$ASSETS" \
    > "$MISSING_APORTS_ASSETS"
awk -F '\t' '$1 != "upstream" || $2 != "alpha"' "$ASSETS" \
    > "$MISSING_UPSTREAM_ASSETS"
printf 'unreferenced upstream\n' > "$UNREFERENCED_DISTFILE"
cp "$ASSETS" "$EXTRA_UPSTREAM_ASSETS"
printf '%s\n' \
    "upstream"$'\tbeta\t'"$BETA_COMMIT"$'\tdistfiles/beta/unreferenced-source.txt\t'"$(size_file "$UNREFERENCED_DISTFILE")"$'\t'"$(sha512_file "$UNREFERENCED_DISTFILE")"$'\tfile://'"$UNREFERENCED_DISTFILE" \
    >> "$EXTRA_UPSTREAM_ASSETS"
cp "$UNREFERENCED_DISTFILE" \
    "$CACHE/distfiles/beta/unreferenced-source.txt"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $4 = "../escape.tar.gz"
    } { print }' "$ASSETS" > "$TRAVERSAL_ASSETS"
sed -n '1p;2p;2p;3,$p' "$ASSETS" > "$DUPLICATE_ASSETS"

expect_failure "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$MISSING_ORIGIN_PACKAGES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$COMMIT_DRIFT_PACKAGES" \
    --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" --origins "$ORIGINS" --assets "$ASSETS" \
    --binary-reference "$URL_DRIFT_BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure_containing '未闭合 APKBUILD sha512sums' \
    "$TMP/missing-upstream.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" --origins "$ORIGINS" \
    --assets "$MISSING_UPSTREAM_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
expect_failure_containing '未闭合 APKBUILD sha512sums' \
    "$TMP/extra-upstream.stderr" \
    "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
    --packages "$PACKAGES" --origins "$ORIGINS" \
    --assets "$EXTRA_UPSTREAM_ASSETS" \
    --binary-reference "$BINARY_REFERENCE" --readme "$README" \
    --allow-local-sources
for invalid_assets in "$MISSING_APORTS_ASSETS" \
        "$TRAVERSAL_ASSETS" "$DUPLICATE_ASSETS"; do
    expect_failure "$PYTHON" "$TOOL" bundle "$CACHE" "$TMP/invalid.tar" \
        --packages "$PACKAGES" --origins "$ORIGINS" \
        --assets "$invalid_assets" \
        --binary-reference "$BINARY_REFERENCE" --readme "$README" \
        --allow-local-sources
done

PROTECTED="$TMP/protected.tar"
printf 'keep existing output\n' > "$PROTECTED"
PROTECTED_SHA=$(sha256_file "$PROTECTED")
printf 'drift\n' >> "$CACHE/distfiles/alpha/alpha-source.tar.gz"
expect_failure "$PYTHON" "$TOOL" bundle "$CACHE" "$PROTECTED" \
    "${COMMON[@]}"
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
expect_failure "$PYTHON" "$TOOL" verify "$TAMPERED" \
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
if ! grep -F '源码包成员元数据不规范' "$MALICIOUS_ERROR" \
        >/dev/null; then
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
if ! grep -F '源码包成员集合缺失、重复或含额外文件' \
        "$DUPLICATE_MEMBER_ERROR" >/dev/null; then
    echo "错误：恶意源码包未命中重复成员门禁。" >&2
    exit 1
fi

if find "$TMP" -name '.source-download.*' -print -quit | grep -q .; then
    echo "错误：源码资产测试留下了下载临时文件。" >&2
    exit 1
fi

echo "Alpine AArch64 对应源码包测试通过"

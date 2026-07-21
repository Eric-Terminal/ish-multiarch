#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

VERSION=3.24.1
ARCHIVE_NAME="alpine-minirootfs-${VERSION}-aarch64.tar.gz"
URL="https://dl-cdn.alpinelinux.org/alpine/v${VERSION%.*}/releases/aarch64/${ARCHIVE_NAME}"
EXPECTED_SHA256=f55a90f69052c5bd6f92cb09a8f47065970830b194c917a006fb94028e721259

usage() {
    echo "用法：$0 <输出 fakefs 目录> [本地 rootfs 归档] [fakefsify]" >&2
    exit 2
}

if [[ $# -lt 1 || $# -gt 3 ]]; then
    usage
fi

OUTPUT=$1
if [[ "$OUTPUT" == *$'\n'* ]]; then
    echo "错误：输出路径不能包含换行符。" >&2
    exit 1
fi
ARCHIVE=${2:-}
FAKEFSIFY=${3:-}
SOURCE_KIND=official
MANIFEST_VERSION=$VERSION
MANIFEST_URL=$URL
TEST_MODE=${ISH_AARCH64_ROOTFS_TEST_MODE:-}
TEST_SHA256=${ISH_AARCH64_ROOTFS_TEST_SHA256:-}
case "$TEST_MODE" in
    '')
        if [[ -n "$TEST_SHA256" ]]; then
            echo "错误：测试摘要只能用于显式 fixture 模式。" >&2
            exit 2
        fi
        ;;
    fixture)
        if [[ -z "$TEST_SHA256" || -z "$ARCHIVE" || -z "$FAKEFSIFY" ]]; then
            echo "错误：fixture 模式必须显式提供摘要、本地归档和 fakefsify。" >&2
            exit 2
        fi
        EXPECTED_SHA256=$TEST_SHA256
        SOURCE_KIND=test-fixture
        MANIFEST_VERSION=test-fixture
        MANIFEST_URL=test-fixture://synthetic
        ;;
    *)
        echo "错误：未知的 AArch64 rootfs 测试模式：$TEST_MODE" >&2
        exit 2
        ;;
esac
if [[ ! "$EXPECTED_SHA256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "错误：rootfs SHA-256 pin 必须是 64 位小写十六进制。" >&2
    exit 2
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

sha256_text() {
    if command -v shasum >/dev/null 2>&1; then
        printf '%s' "$1" | shasum -a 256 | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    else
        echo "错误：找不到 shasum 或 sha256sum。" >&2
        return 1
    fi
}

find_tool() {
    local name=$1
    local configured=$2
    if [[ -n "$configured" ]]; then
        printf '%s\n' "$configured"
        return
    fi

    local candidate
    candidate=$(command -v "$name" || true)
    if [[ -n "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return
    fi
    for candidate in "$HOME/.local/bin/$name" \
            /opt/homebrew/bin/"$name" /usr/local/bin/"$name" \
            "$HOME"/.cache/uv/archive-v0/*/bin/"$name"; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    return 1
}

build_fakefsify() {
    local build_dir
    if [[ -n ${ISH_AARCH64_ROOTFS_HOST_BUILD:-} ]]; then
        build_dir=$ISH_AARCH64_ROOTFS_HOST_BUILD
    elif [[ -n ${DERIVED_FILE_DIR:-} ]]; then
        build_dir="$DERIVED_FILE_DIR/ish-aarch64-rootfs-host"
    else
        build_dir="$ROOT/build/apple-rootfs-host"
    fi
    local meson
    local ninja
    meson=$(find_tool meson "${MESON:-}") || {
        echo "错误：找不到 Meson，无法构建宿主 fakefsify。" >&2
        return 1
    }
    ninja=$(find_tool ninja "${NINJA:-}") || {
        echo "错误：找不到 Ninja，无法构建宿主 fakefsify。" >&2
        return 1
    }

    local -a clean_env=(env
        -u ARCHS -u CURRENT_ARCH -u SDKROOT -u PLATFORM_NAME
        -u EFFECTIVE_PLATFORM_NAME -u CC -u CXX -u CPP
        -u AR -u AS -u LD -u NM -u RANLIB -u STRIP -u LIBTOOL
        -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS
        -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LIBRARY_PATH
        -u PKG_CONFIG -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR
        -u PKG_CONFIG_SYSROOT_DIR -u ARCHFLAGS
        -u IPHONEOS_DEPLOYMENT_TARGET -u WATCHOS_DEPLOYMENT_TARGET
        -u MACOSX_DEPLOYMENT_TARGET)
    if [[ ! -f "$build_dir/build.ninja" ]]; then
        if ! "${clean_env[@]}" "$meson" setup "$build_dir" "$ROOT" \
                --buildtype=release -Dkernel=ish -Dengine=asbestos >&2; then
            echo "错误：无法配置宿主 fakefsify 构建。" >&2
            return 1
        fi
    else
        if ! "${clean_env[@]}" "$meson" setup --reconfigure \
                "$build_dir" "$ROOT" >&2; then
            echo "错误：无法刷新宿主 fakefsify 构建。" >&2
            return 1
        fi
    fi
    if ! "${clean_env[@]}" "$ninja" -C "$build_dir" \
            tools/fakefsify >&2; then
        echo "错误：无法构建宿主 fakefsify；请安装 libarchive 开发文件，或显式传入可执行文件。" >&2
        return 1
    fi
    if [[ ! -x "$build_dir/tools/fakefsify" ]]; then
        echo "错误：宿主构建没有生成 fakefsify，请检查 libarchive 依赖。" >&2
        return 1
    fi
    printf '%s\n' "$build_dir/tools/fakefsify"
}

if [[ -z "$ARCHIVE" ]]; then
    if [[ -n ${ISH_AARCH64_ROOTFS_CACHE:-} ]]; then
        CACHE=$ISH_AARCH64_ROOTFS_CACHE
    elif [[ -n ${DERIVED_FILE_DIR:-} ]]; then
        CACHE="$DERIVED_FILE_DIR/ish-aarch64-rootfs-cache"
    else
        CACHE="$ROOT/build/apple-rootfs-cache"
    fi
    mkdir -p "$CACHE"
    ARCHIVE="$CACHE/$ARCHIVE_NAME"
    CACHE_VALID=false
    if [[ -f "$ARCHIVE" && $(sha256_file "$ARCHIVE") == \
            "$EXPECTED_SHA256" ]]; then
        CACHE_VALID=true
    fi
    if ! $CACHE_VALID; then
        if [[ ${ISH_AARCH64_ROOTFS_OFFLINE:-0} == 1 ]]; then
            echo "错误：离线模式下没有通过摘要校验的 AArch64 rootfs 缓存。" >&2
            exit 1
        fi
        if ! command -v curl >/dev/null 2>&1; then
            echo "错误：找不到 curl，无法取得固定 AArch64 rootfs。" >&2
            exit 1
        fi
        download=$(mktemp "$CACHE/.${ARCHIVE_NAME}.download.XXXXXX")
        trap 'rm -f "${download:-}"' EXIT
        curl --fail --location --retry 3 --output "$download" "$URL"
        DOWNLOAD_SHA256=$(sha256_file "$download")
        if [[ "$DOWNLOAD_SHA256" != "$EXPECTED_SHA256" ]]; then
            echo "错误：下载的 AArch64 rootfs SHA-256 不匹配。" >&2
            echo "期望：$EXPECTED_SHA256" >&2
            echo "实际：$DOWNLOAD_SHA256" >&2
            exit 1
        fi
        mv "$download" "$ARCHIVE"
        download=
        trap - EXIT
    fi
fi

if [[ ! -f "$ARCHIVE" ]]; then
    echo "错误：rootfs 归档不存在：$ARCHIVE" >&2
    exit 1
fi

ACTUAL_SHA256=$(sha256_file "$ARCHIVE")
if [[ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]]; then
    echo "错误：AArch64 rootfs SHA-256 不匹配。" >&2
    echo "期望：$EXPECTED_SHA256" >&2
    echo "实际：$ACTUAL_SHA256" >&2
    exit 1
fi

if [[ -z "$FAKEFSIFY" ]]; then
    FAKEFSIFY=$(build_fakefsify)
fi
if [[ ! -x "$FAKEFSIFY" ]]; then
    echo "错误：fakefsify 不存在或不可执行：$FAKEFSIFY" >&2
    exit 1
fi

OUTPUT_PARENT=$(dirname "$OUTPUT")
OUTPUT_NAME=$(basename "$OUTPUT")
if [[ -z "$OUTPUT_NAME" || "$OUTPUT_NAME" == "." ||
        "$OUTPUT_NAME" == ".." || "$OUTPUT_NAME" == "/" ]]; then
    echo "错误：输出目录名称无效：$OUTPUT" >&2
    exit 1
fi
mkdir -p "$OUTPUT_PARENT"
OUTPUT_PARENT=$(cd "$OUTPUT_PARENT" && pwd -P)
OUTPUT="$OUTPUT_PARENT/$OUTPUT_NAME"
case "$ROOT/" in
    "$OUTPUT/"*)
        echo "错误：输出目录不能是源码树或其祖先：$OUTPUT" >&2
        exit 1
        ;;
esac

LOCK_ROOT=${ISH_AARCH64_ROOTFS_LOCK_ROOT:-"${TMPDIR:-/tmp}/ish-aarch64-rootfs-locks-$(id -u)"}
if [[ -L "$LOCK_ROOT" ]]; then
    echo "错误：rootfs 锁目录不能是符号链接：$LOCK_ROOT" >&2
    exit 1
fi
(
    umask 077
    mkdir -p "$LOCK_ROOT"
)
LOCK_OWNER=$(stat -f '%u' "$LOCK_ROOT" 2>/dev/null || true)
if [[ ! "$LOCK_OWNER" =~ ^[0-9]+$ ]]; then
    LOCK_OWNER=$(stat -c '%u' "$LOCK_ROOT")
fi
if [[ "$LOCK_OWNER" != "$(id -u)" ]]; then
    echo "错误：rootfs 锁目录不属于当前用户：$LOCK_ROOT" >&2
    exit 1
fi
chmod 0700 "$LOCK_ROOT"
LOCK_KEY=$(sha256_text "$OUTPUT")
exec 9> "$LOCK_ROOT/$LOCK_KEY.lock"
LOCK_TIMEOUT=${ISH_AARCH64_ROOTFS_LOCK_TIMEOUT:-60}
if [[ ! "$LOCK_TIMEOUT" =~ ^[0-9]+$ ]]; then
    echo "错误：rootfs 输出锁超时必须是非负整数秒。" >&2
    exit 1
fi
if command -v flock >/dev/null 2>&1; then
    if ! flock -w "$LOCK_TIMEOUT" 9; then
        echo "错误：等待同一 rootfs 输出锁超时：$OUTPUT" >&2
        exit 1
    fi
elif command -v lockf >/dev/null 2>&1; then
    if ! lockf -s -t "$LOCK_TIMEOUT" 9; then
        echo "错误：等待同一 rootfs 输出锁超时：$OUTPUT" >&2
        exit 1
    fi
else
    echo "错误：找不到 flock 或 lockf，不能安全串行化 rootfs 输出。" >&2
    exit 1
fi

is_owned_rootfs() {
    [[ ! -L "$1" && -d "$1" &&
        -f "$1/rootfs-manifest.txt" &&
        -f "$1/meta.db" && -d "$1/data" &&
        $(sed -n '1p' "$1/rootfs-manifest.txt") == \
            'format=ish-fakefs-v3' &&
        $(sed -n '2p' "$1/rootfs-manifest.txt") == \
            'packager=apple-aarch64-rootfs-v1' ]]
}

RECOVERY="$OUTPUT_PARENT/.ish-rootfs-recovery-$LOCK_KEY"
RECOVERY_MARKER="$RECOVERY.target"
if [[ -e "$RECOVERY_MARKER" || -L "$RECOVERY_MARKER" ]]; then
    if [[ -L "$RECOVERY_MARKER" || ! -f "$RECOVERY_MARKER" ||
            $(sed -n '1p' "$RECOVERY_MARKER") != "$OUTPUT" ]]; then
        echo "错误：rootfs 恢复标记不属于当前输出：$RECOVERY_MARKER" >&2
        exit 1
    fi
elif [[ -e "$RECOVERY" || -L "$RECOVERY" ]]; then
    echo "错误：rootfs 恢复目录缺少目标标记：$RECOVERY" >&2
    exit 1
fi
if [[ -e "$RECOVERY" || -L "$RECOVERY" ]]; then
    if ! is_owned_rootfs "$RECOVERY"; then
        echo "错误：拒绝处理不属于本打包器的恢复路径：$RECOVERY" >&2
        exit 1
    fi
    if [[ -e "$OUTPUT" || -L "$OUTPUT" ]]; then
        if ! is_owned_rootfs "$OUTPUT"; then
            echo "错误：恢复路径与未知输出同时存在：$OUTPUT" >&2
            exit 1
        fi
        rm -rf "$RECOVERY"
    else
        mv "$RECOVERY" "$OUTPUT"
    fi
fi
if [[ -e "$RECOVERY_MARKER" ]]; then
    rm -f "$RECOVERY_MARKER"
fi

if [[ -e "$OUTPUT" || -L "$OUTPUT" ]]; then
    if ! is_owned_rootfs "$OUTPUT"; then
        echo "错误：拒绝替换不属于本打包器的既有路径：$OUTPUT" >&2
        exit 1
    fi
fi

STAGING=$(mktemp -d "$OUTPUT_PARENT/.${OUTPUT_NAME}.building.XXXXXX")
BACKUP=
cleanup() {
    local status=$?
    trap - EXIT HUP INT TERM
    rm -rf "$STAGING"
    if [[ -n "$BACKUP" && -e "$BACKUP" ]]; then
        if [[ -e "$OUTPUT" ]]; then
            rm -rf "$BACKUP"
        else
            mv "$BACKUP" "$OUTPUT"
        fi
    fi
    if [[ -n "$BACKUP" && -e "$RECOVERY_MARKER" ]]; then
        rm -f "$RECOVERY_MARKER"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

ARCHIVE_SNAPSHOT="$STAGING/$ARCHIVE_NAME"
cp "$ARCHIVE" "$ARCHIVE_SNAPSHOT"
SNAPSHOT_SHA256=$(sha256_file "$ARCHIVE_SNAPSHOT")
if [[ "$SNAPSHOT_SHA256" != "$ACTUAL_SHA256" ]]; then
    echo "错误：rootfs 归档在摘要校验后发生了变化。" >&2
    exit 1
fi

"$FAKEFSIFY" "$ARCHIVE_SNAPSHOT" "$STAGING/rootfs"

BUSYBOX="$STAGING/rootfs/data/bin/busybox"
if [[ ! -f "$STAGING/rootfs/meta.db" || ! -d "$STAGING/rootfs/data" ||
        ! -f "$BUSYBOX" ]]; then
    echo "错误：转换结果缺少 meta.db、data 或 /bin/busybox。" >&2
    exit 1
fi

SQLITE3_BIN=${SQLITE3:-}
if [[ -z "$SQLITE3_BIN" ]]; then
    SQLITE3_BIN=$(command -v sqlite3 || true)
fi
if [[ ! -x "$SQLITE3_BIN" ]]; then
    echo "错误：找不到 sqlite3，无法验证 fakefs 元数据。" >&2
    exit 1
fi
META_DB="$STAGING/rootfs/meta.db"
SQLITE_HOME="$STAGING/sqlite-home"
mkdir "$SQLITE_HOME"
run_sqlite() {
    HOME="$SQLITE_HOME" "$SQLITE3_BIN" -batch -noheader -list -bail \
        -init /dev/null "$@"
}
JOURNAL_RESULT=$(run_sqlite "$META_DB" \
    'pragma wal_checkpoint(truncate); pragma journal_mode=delete;')
if [[ ${JOURNAL_RESULT##*$'\n'} != "delete" ||
        -s "$STAGING/rootfs/meta.db-wal" ]]; then
    echo "错误：无法把 fakefs 元数据收敛成单一 SQLite 文件。" >&2
    exit 1
fi
rm -f "$STAGING/rootfs/meta.db-wal" "$STAGING/rootfs/meta.db-shm"
QUICK_CHECK=$(run_sqlite "$META_DB" 'pragma quick_check;')
SCHEMA_VERSION=$(run_sqlite "$META_DB" 'pragma user_version;')
ROOT_ROWS=$(run_sqlite "$META_DB" \
    'select count(*) from paths where length(path) = 0;')
if [[ "$QUICK_CHECK" != "ok" || "$SCHEMA_VERSION" != "3" ||
        "$ROOT_ROWS" != "1" ]]; then
    echo "错误：fakefs 元数据完整性、schema 或根路径验证失败。" >&2
    exit 1
fi
if find "$STAGING/rootfs" -maxdepth 1 \
        \( -name 'meta.db-wal' -o -name 'meta.db-shm' \) \
        -print -quit | grep -q .; then
    echo "错误：fakefs 资源不能携带 SQLite WAL/SHM 临时文件。" >&2
    exit 1
fi
SPECIAL_NODE=$(find "$STAGING/rootfs/data" ! -type d ! -type f \
    -print -quit)
if [[ -n "$SPECIAL_NODE" ]]; then
    echo "错误：fakefs data 含不能安全打包的宿主特殊节点：$SPECIAL_NODE" >&2
    exit 1
fi

# ELF64 little-endian 的 e_machine 必须是 EM_AARCH64(183)。
read -r -a elf_header <<< "$(od -An -t u1 -N 20 "$BUSYBOX" | tr '\n' ' ')"
if [[ ${#elf_header[@]} -ne 20 ||
        ${elf_header[0]} -ne 127 || ${elf_header[1]} -ne 69 ||
        ${elf_header[2]} -ne 76 || ${elf_header[3]} -ne 70 ||
        ${elf_header[4]} -ne 2 || ${elf_header[5]} -ne 1 ||
        ${elf_header[18]} -ne 183 || ${elf_header[19]} -ne 0 ]]; then
    echo "错误：rootfs 的 /bin/busybox 不是 AArch64 ELF64 little-endian。" >&2
    exit 1
fi

stat_inode() {
    local inode
    inode=$(stat -f '%i' "$1" 2>/dev/null || true)
    if [[ "$inode" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$inode"
        return
    fi
    stat -c '%i' "$1"
}

HARDLINK_RAW="$STAGING/hardlinks.raw"
: > "$HARDLINK_RAW"
while IFS= read -r -d '' hardlink_path; do
    hardlink_relative=${hardlink_path#"$STAGING/rootfs/data/"}
    if [[ "$hardlink_relative" == *$'\t'* ||
            "$hardlink_relative" == *$'\n'* ]]; then
        echo "错误：hardlink 路径含清单无法表示的制表符或换行。" >&2
        exit 1
    fi
    printf '%s\t%s\n' "$(stat_inode "$hardlink_path")" \
        "$hardlink_relative" >> "$HARDLINK_RAW"
done < <(find "$STAGING/rootfs/data" -type f -links +1 -print0)
LC_ALL=C sort -t $'\t' -k1,1 -k2,2 "$HARDLINK_RAW" | \
    awk -F '\t' -f "$ROOT/tools/apple-aarch64-hardlinks.awk" | \
    LC_ALL=C sort > "$STAGING/rootfs/rootfs-hardlinks.tsv"
rm -f "$HARDLINK_RAW"

printf '%s\n' \
    'format=ish-fakefs-v3' \
    'packager=apple-aarch64-rootfs-v1' \
    'guest_arch=aarch64' \
    "source_kind=$SOURCE_KIND" \
    "alpine_version=$MANIFEST_VERSION" \
    "archive_sha256=$ACTUAL_SHA256" \
    "source_url=$MANIFEST_URL" \
    'hardlinks=rootfs-hardlinks.tsv' \
    > "$STAGING/rootfs/rootfs-manifest.txt"

if [[ -e "$OUTPUT" ]]; then
    BACKUP=$RECOVERY
    if [[ -e "$RECOVERY_MARKER" || -L "$RECOVERY_MARKER" ]]; then
        echo "错误：rootfs 恢复标记已存在：$RECOVERY_MARKER" >&2
        exit 1
    fi
    printf '%s\n' "$OUTPUT" > "$RECOVERY_MARKER"
    mv "$OUTPUT" "$BACKUP"
fi
mv "$STAGING/rootfs" "$OUTPUT"
rm -rf "$STAGING"
STAGING=
if [[ -n "$BACKUP" ]]; then
    rm -rf "$BACKUP"
    rm -f "$RECOVERY_MARKER"
    BACKUP=
fi
trap - EXIT HUP INT TERM

echo "已生成 AArch64 fakefs：$OUTPUT"

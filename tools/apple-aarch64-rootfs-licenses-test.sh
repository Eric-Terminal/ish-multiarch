#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/tools/apple-aarch64-rootfs-licenses.py"
PYTHON=${1:-}

if [[ ! -x "$PYTHON" ]]; then
    echo "用法：$0 <python3>" >&2
    exit 2
fi

"$PYTHON" "$TOOL" check-locks >/dev/null

expect_failure() {
    local expected=$1
    shift
    if "$@" >"$FAILURE_OUTPUT" 2>&1; then
        echo "错误：无效的许可输入未被拒绝。" >&2
        exit 1
    fi
    if ! grep -q '^错误：' "$FAILURE_OUTPUT" \
            || ! grep -Fq "$expected" "$FAILURE_OUTPUT"; then
        echo "错误：无效输入没有到达预期拒绝路径：$expected" >&2
        cat "$FAILURE_OUTPUT" >&2
        exit 1
    fi
    if grep -Fq 'Traceback (most recent call last)' "$FAILURE_OUTPUT"; then
        echo "错误：无效输入输出了 Python traceback：$expected" >&2
        cat "$FAILURE_OUTPUT" >&2
        exit 1
    fi
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ish-aarch64-licenses-test.XXXXXX")
FAILURE_OUTPUT="$TMP/expected-failure.txt"
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

PACKAGES="$TMP/packages.tsv"
STATIC_LINK_SOURCES="$TMP/static-link-sources.tsv"
ASSETS="$TMP/source-assets.tsv"
INPUTS="$TMP/license-inputs.tsv"
NOTICES="$TMP/THIRD-PARTY-NOTICES.txt"
CACHE="$TMP/cache"
AUTHORITY="$TMP/license-inputs/mit.txt"

"$PYTHON" - "$TMP" <<'PY'
import hashlib
import io
from pathlib import Path
import sys
import tarfile


root = Path(sys.argv[1])
cache = root / "cache"
(root / "license-inputs").mkdir()
(root / "license-inputs" / "mit.txt").write_bytes(b"MIT authority fixture\n")


def archive_bytes(name, data):
    output = io.BytesIO()
    with tarfile.open(
        fileobj=output, mode="w:gz", format=tarfile.USTAR_FORMAT
    ) as archive:
        member = tarfile.TarInfo(name)
        member.size = len(data)
        member.mode = 0o644
        archive.addfile(member, io.BytesIO(data))
    return output.getvalue()


aports_body = b"aports license fixture\n"
upstream_body = b"upstream license fixture\n"
raw_body = b"raw license fixture\n"
assets = []
aports_sources = {}
upstream_sources = {}
origin_commits = {}
for index in range(10):
    origin = f"origin{index:02d}"
    origin_commits[origin] = f"{index + 1:040x}"
    body = (
        aports_body
        if index == 0
        else f"aports license fixture {index:02d}\n".encode()
    )
    path = f"aports/{origin}.tar.gz"
    url = f"https://example.invalid/aports-{origin}.tar.gz"
    assets.append(
        ("aports", origin, path, archive_bytes("root/LICENSE", body), url)
    )
    aports_sources[index] = (path, "root/LICENSE", url, body)
for index in range(1, 10):
    origin = f"origin{index:02d}"
    if index == 1:
        body = upstream_body
        path = f"distfiles/{origin}/upstream.tar.gz"
        member = "upstream/COPYING"
        data = archive_bytes(member, body)
    else:
        body = raw_body if index == 2 else f"raw license fixture {index:02d}\n".encode()
        path = f"distfiles/{origin}/raw.txt"
        member = "-"
        data = body
    url = f"https://example.invalid/upstream-{origin}"
    assets.append(("upstream", origin, path, data, url))
    upstream_sources[index] = (path, member, url, body)
static_sources = {}
for index in range(10, 12):
    origin = f"origin{index:02d}"
    origin_commits[origin] = f"{1:040x}"
    aports_body = (
        f"pkgname={origin}\n"
        "pkgver=1\n"
        "pkgrel=0\n"
        f"static aports license fixture {index:02d}\n"
    ).encode()
    upstream_body = f"static upstream license fixture {index:02d}\n".encode()
    aports_path = f"aports/{origin}.tar.gz"
    aports_url = f"https://example.invalid/aports-{origin}.tar.gz"
    assets.append(
        (
            "aports",
            origin,
            aports_path,
            archive_bytes("root/APKBUILD", aports_body),
            aports_url,
        )
    )
    upstream_path = f"distfiles/{origin}/source.tar.gz"
    upstream_member = "source/COPYING"
    upstream_url = f"https://example.invalid/upstream-{origin}.tar.gz"
    assets.append(
        (
            "upstream",
            origin,
            upstream_path,
            archive_bytes(upstream_member, upstream_body),
            upstream_url,
        )
    )
    static_sources[index] = (
        aports_path,
        "root/APKBUILD",
        aports_url,
        aports_body,
        upstream_path,
        upstream_member,
        upstream_url,
        upstream_body,
    )
for _kind, _origin, path, data, _url in assets:
    destination = cache / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)

package_lines = ["package\tversion\torigin\tlicense\taports_commit"]
for index in range(16):
    origin_index = index if index < 10 else index - 10
    commit = f"{origin_index + 1:040x}"
    package_lines.append(
        f"pkg{index:02d}\t1-r0\torigin{origin_index:02d}\tMIT\t{commit}"
    )
(root / "packages.tsv").write_text(
    "\n".join(package_lines) + "\n", encoding="utf-8"
)
(root / "static-link-sources.tsv").write_text(
    (
        "binary_package\tsource_package\tsource_version\tsource_origin"
        "\tsource_license\taports_snapshot_commit\n"
        f"pkg00@1-r0\torigin10-static\t1-r0\torigin10\tISC\t{1:040x}\n"
        f"pkg00@1-r0\torigin11-static\t1-r0\torigin11\tISC\t{1:040x}\n"
    ),
    encoding="utf-8",
)

asset_lines = [
    "kind\torigin\taports_commit\tbundle_path\tsize\tsha512\tsource_url"
]
for kind, origin, path, data, url in sorted(assets, key=lambda item: item[2]):
    asset_lines.append(
        "\t".join(
            (
                kind,
                origin,
                origin_commits[origin],
                path,
                str(len(data)),
                hashlib.sha512(data).hexdigest(),
                url,
            )
        )
    )
(root / "source-assets.tsv").write_text(
    "\n".join(asset_lines) + "\n", encoding="utf-8"
)

authority = (root / "license-inputs" / "mit.txt").read_bytes()
source_rows = []
notice_parts = []
for index in range(10):
    packages = [f"pkg{index:02d}@1-r0"]
    if index < 6:
        packages.append(f"pkg{index + 10:02d}@1-r0")
    section = f"notice-{index:02d}"
    notice_body = f"Notice body {index:02d}\n".encode()
    if index >= 3:
        notice_body += authority
    notice_parts.extend(
        (
            f"===== BEGIN NOTICE: {section} =====\n",
            notice_body.decode(),
            f"===== END NOTICE: {section} =====\n\n",
        )
    )
    notice_hash = hashlib.sha256(notice_body).hexdigest()
    package_text = ",".join(packages)
    asset, member, source_url, source_body = aports_sources[index]
    source_rows.append(
        (
            f"origin{index:02d}",
            package_text,
            "MIT",
            "aports",
            asset,
            member,
            source_url,
            str(len(source_body)),
            hashlib.sha256(source_body).hexdigest(),
            section,
            notice_hash,
        )
    )
    if index in upstream_sources:
        asset, member, source_url, source_body = upstream_sources[index]
        source_rows.append(
            (
                f"origin{index:02d}",
                package_text,
                "MIT",
                "upstream",
                asset,
                member,
                source_url,
                str(len(source_body)),
                hashlib.sha256(source_body).hexdigest(),
                section,
                notice_hash,
            )
        )
    if index >= 3:
        source_rows.append(
            (
                f"origin{index:02d}",
                package_text,
                "MIT",
                "authority",
                "license-inputs/mit.txt",
                "-",
                "https://example.invalid/mit-license",
                str(len(authority)),
                hashlib.sha256(authority).hexdigest(),
                section,
                notice_hash,
            )
        )
alternate_body = b"Notice body 00 alternate\n"
notice_parts.extend(
    (
        "===== BEGIN NOTICE: notice-00-alt =====\n",
        alternate_body.decode(),
        "===== END NOTICE: notice-00-alt =====\n\n",
    )
)
source_rows.append(
    (*source_rows[0][:9], "notice-00-alt", hashlib.sha256(alternate_body).hexdigest())
)
for index in range(10, 12):
    section = f"notice-static-{index:02d}"
    notice_body = f"Static notice body {index:02d}\n".encode()
    notice_parts.extend(
        (
            f"===== BEGIN NOTICE: {section} =====\n",
            notice_body.decode(),
            f"===== END NOTICE: {section} =====\n\n",
        )
    )
    notice_hash = hashlib.sha256(notice_body).hexdigest()
    (
        aports_asset,
        aports_member,
        aports_url,
        aports_body,
        upstream_asset,
        upstream_member,
        upstream_url,
        upstream_body,
    ) = static_sources[index]
    for kind, asset, member, url, body in (
        ("aports", aports_asset, aports_member, aports_url, aports_body),
        (
            "upstream",
            upstream_asset,
            upstream_member,
            upstream_url,
            upstream_body,
        ),
    ):
        source_rows.append(
            (
                f"origin{index:02d}",
                "pkg00@1-r0",
                "ISC",
                kind,
                asset,
                member,
                url,
                str(len(body)),
                hashlib.sha256(body).hexdigest(),
                section,
                notice_hash,
            )
        )
input_lines = [
    "origin\tpackages\tlicense\tsource_kind\tsource_asset\tsource_member"
    "\tsource_url\tsource_size\tsource_sha256\tnotice_section"
    "\tnotice_sha256"
]
input_lines.extend("\t".join(row) for row in sorted(source_rows))
(root / "license-inputs.tsv").write_text(
    "\n".join(input_lines) + "\n", encoding="utf-8"
)
(root / "THIRD-PARTY-NOTICES.txt").write_text(
    "".join(notice_parts), encoding="utf-8"
)
PY

COMMON=(
    --packages "$PACKAGES"
    --static-link-sources "$STATIC_LINK_SOURCES"
    --assets "$ASSETS"
    --inputs "$INPUTS"
    --notices "$NOTICES"
)

"$PYTHON" "$TOOL" check-locks "${COMMON[@]}" >/dev/null
"$PYTHON" "$TOOL" validate-sources "$CACHE" "${COMMON[@]}" >/dev/null

MISSING_PACKAGE_INPUTS="$TMP/license-inputs-missing-package.tsv"
DRIFT_PACKAGES="$TMP/packages-drift.tsv"
MISSING_STATIC_SOURCES="$TMP/static-link-sources-missing.tsv"
DUPLICATE_STATIC_SOURCES="$TMP/static-link-sources-duplicate.tsv"
BAD_CONSUMER_STATIC_SOURCES="$TMP/static-link-sources-bad-consumer.tsv"
BAD_LICENSE_STATIC_SOURCES="$TMP/static-link-sources-bad-license.tsv"
BAD_COMMIT_STATIC_SOURCES="$TMP/static-link-sources-bad-commit.tsv"
BAD_ORIGIN_STATIC_SOURCES="$TMP/static-link-sources-bad-origin.tsv"
DRIFT_VERSION_STATIC_SOURCES="$TMP/static-link-sources-version-drift.tsv"
CONFLICT_VERSION_ASSETS="$TMP/source-assets-version-conflict.tsv"
CONFLICT_VERSION_INPUTS="$TMP/license-inputs-version-conflict.tsv"
CONFLICT_VERSION_CACHE="$TMP/cache-version-conflict"
BAD_STATIC_TARGET_INPUTS="$TMP/license-inputs-bad-static-target.tsv"
BAD_STATIC_LICENSE_INPUTS="$TMP/license-inputs-bad-static-license.tsv"
MISSING_STATIC_ASSETS="$TMP/source-assets-missing-static.tsv"
MALFORMED_URL_INPUTS="$TMP/license-inputs-malformed-url.tsv"
MALFORMED_PORT_URL_INPUTS="$TMP/license-inputs-malformed-port-url.tsv"
NUL_URL_INPUTS="$TMP/license-inputs-nul-url.tsv"
C0_URL_INPUTS="$TMP/license-inputs-c0-url.tsv"
DEL_URL_INPUTS="$TMP/license-inputs-del-url.tsv"
EMPTY_FRAGMENT_URL_INPUTS="$TMP/license-inputs-empty-fragment-url.tsv"
NUL_PATH_INPUTS="$TMP/license-inputs-nul-path.tsv"
MISSING_SECTION="$TMP/notices-missing-section.txt"
DUPLICATE_SECTION="$TMP/notices-duplicate-section.txt"
TAMPERED_SECTION="$TMP/notices-tampered-section.txt"
UNSCOPED_TEXT="$TMP/notices-unscoped-text.txt"
AUTHORITY_MISSING_NOTICES="$TMP/notices-authority-missing.txt"
AUTHORITY_DUPLICATE_NOTICES="$TMP/notices-authority-duplicate.txt"
AUTHORITY_MISSING_INPUTS="$TMP/inputs-authority-missing.tsv"
AUTHORITY_DUPLICATE_INPUTS="$TMP/inputs-authority-duplicate.tsv"
MISSING_MEMBER_INPUTS="$TMP/license-inputs-missing-member.tsv"
DRIFT_MEMBER_INPUTS="$TMP/license-inputs-member-drift.tsv"
DRIFT_DUPLICATE_INPUTS="$TMP/license-inputs-duplicate-drift.tsv"
UNREFERENCED_ASSETS="$TMP/source-assets-unreferenced.tsv"
UNREFERENCED_INPUTS="$TMP/license-inputs-unreferenced.tsv"

sed -n '1p;2p' "$STATIC_LINK_SOURCES" > "$MISSING_STATIC_SOURCES"
sed -n '1p;2p;2p;3p' "$STATIC_LINK_SOURCES" \
    > "$DUPLICATE_STATIC_SOURCES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $1 = "unknown@1-r0"
    } { print }' "$STATIC_LINK_SOURCES" > "$BAD_CONSUMER_STATIC_SOURCES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $5 = "MIT"
    } { print }' "$STATIC_LINK_SOURCES" > "$BAD_LICENSE_STATIC_SOURCES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $6 = "2222222222222222222222222222222222222222"
    } { print }' "$STATIC_LINK_SOURCES" > "$BAD_COMMIT_STATIC_SOURCES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $2 = "origin00-static"
        $4 = "origin00"
    } { print }' "$STATIC_LINK_SOURCES" > "$BAD_ORIGIN_STATIC_SOURCES"
awk 'BEGIN { FS = OFS = "\t" } NR == 2 {
        $3 = "2-r0"
    } { print }' "$STATIC_LINK_SOURCES" > "$DRIFT_VERSION_STATIC_SOURCES"
"$PYTHON" - "$INPUTS" "$BAD_STATIC_TARGET_INPUTS" \
    "$BAD_STATIC_LICENSE_INPUTS" <<'PY'
from pathlib import Path
import sys

source, bad_target, bad_license = map(Path, sys.argv[1:])
lines = source.read_text(encoding="utf-8").splitlines()
rows = [line.split("\t") for line in lines[1:]]
target_rows = [row[:] for row in rows]
license_rows = [row[:] for row in rows]
for row in target_rows:
    if row[0] == "origin10":
        row[1] = "pkg01@1-r0"
for row in license_rows:
    if row[0] == "origin10":
        row[2] = "MIT"
for destination, changed_rows in (
    (bad_target, target_rows),
    (bad_license, license_rows),
):
    destination.write_text(
        lines[0]
        + "\n"
        + "\n".join("\t".join(row) for row in sorted(changed_rows))
        + "\n",
        encoding="utf-8",
    )
PY
awk -F '\t' '$4 != "distfiles/origin11/source.tar.gz"' "$ASSETS" \
    > "$MISSING_STATIC_ASSETS"

expect_failure "源码资产清单排序、来源、大小或摘要格式非法" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$MISSING_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"
expect_failure "静态链接来源清单未按完整记录唯一排序" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$DUPLICATE_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"
expect_failure "静态链接来源声明了未知二进制包" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_CONSUMER_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"
expect_failure "静态链接来源的包、版本、origin、许可或快照格式非法" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_LICENSE_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"
expect_failure "静态链接来源的 aports snapshot 与目标二进制包不一致" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_COMMIT_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"
expect_failure "静态链接源码 origin 不能冒充二进制包 origin" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$BAD_ORIGIN_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"
expect_failure "许可输入与包清单的 origin 或 license 漂移" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --assets "$ASSETS" --inputs "$BAD_STATIC_TARGET_INPUTS" \
    --notices "$NOTICES"
expect_failure "许可输入与包清单的 origin 或 license 漂移" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --assets "$ASSETS" --inputs "$BAD_STATIC_LICENSE_INPUTS" \
    --notices "$NOTICES"
expect_failure "源码资产清单必须恰含 23 项" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --assets "$MISSING_STATIC_ASSETS" --inputs "$INPUTS" \
    --notices "$NOTICES"
expect_failure "静态链接来源版本与 APKBUILD 的 pkgname/pkgver/pkgrel 不一致" \
    "$PYTHON" "$TOOL" validate-sources "$CACHE" \
    --packages "$PACKAGES" \
    --static-link-sources "$DRIFT_VERSION_STATIC_SOURCES" \
    --assets "$ASSETS" --inputs "$INPUTS" --notices "$NOTICES"

cp -R "$CACHE" "$CONFLICT_VERSION_CACHE"
"$PYTHON" - "$CONFLICT_VERSION_CACHE/aports/origin10.tar.gz" \
    "$ASSETS" "$CONFLICT_VERSION_ASSETS" \
    "$INPUTS" "$CONFLICT_VERSION_INPUTS" <<'PY'
import hashlib
import io
from pathlib import Path
import sys
import tarfile

(
    archive_path,
    source_assets,
    destination_assets,
    source_inputs,
    destination_inputs,
) = map(Path, sys.argv[1:])
body = (
    b"pkgname=origin10\n"
    b"pkgver=1\n"
    b"pkgrel=0\n"
    b"static aports license fixture 10\n"
    b"pkgver=9\n"
)
output = io.BytesIO()
with tarfile.open(
    fileobj=output, mode="w:gz", format=tarfile.USTAR_FORMAT
) as archive:
    member = tarfile.TarInfo("root/APKBUILD")
    member.size = len(body)
    member.mode = 0o644
    archive.addfile(member, io.BytesIO(body))
archive_data = output.getvalue()
archive_path.write_bytes(archive_data)

asset_lines = source_assets.read_text(encoding="utf-8").splitlines()
asset_updated = False
for index, line in enumerate(asset_lines[1:], start=1):
    fields = line.split("\t")
    if fields[3] == "aports/origin10.tar.gz":
        fields[4] = str(len(archive_data))
        fields[5] = hashlib.sha512(archive_data).hexdigest()
        asset_lines[index] = "\t".join(fields)
        asset_updated = True
        break
if not asset_updated:
    raise RuntimeError("未找到静态来源的 aports 资产夹具。")
destination_assets.write_text(
    "\n".join(asset_lines) + "\n", encoding="utf-8"
)

input_lines = source_inputs.read_text(encoding="utf-8").splitlines()
input_updated = False
for index, line in enumerate(input_lines[1:], start=1):
    fields = line.split("\t")
    if (
        fields[0] == "origin10"
        and fields[3] == "aports"
        and fields[4] == "aports/origin10.tar.gz"
        and fields[5] == "root/APKBUILD"
    ):
        fields[7] = str(len(body))
        fields[8] = hashlib.sha256(body).hexdigest()
        input_lines[index] = "\t".join(fields)
        input_updated = True
        break
if not input_updated:
    raise RuntimeError("未找到静态来源的 APKBUILD 许可输入夹具。")
destination_inputs.write_text(
    "\n".join(input_lines) + "\n", encoding="utf-8"
)
PY
expect_failure "静态链接来源版本与 APKBUILD 的 pkgname/pkgver/pkgrel 不一致" \
    "$PYTHON" "$TOOL" validate-sources "$CONFLICT_VERSION_CACHE" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --assets "$CONFLICT_VERSION_ASSETS" \
    --inputs "$CONFLICT_VERSION_INPUTS" --notices "$NOTICES"

"$PYTHON" - "$INPUTS" "$MISSING_PACKAGE_INPUTS" <<'PY'
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
data = source.read_text(encoding="utf-8")
destination.write_text(
    data.replace("pkg00@1-r0,pkg10@1-r0", "pkg00@1-r0"),
    encoding="utf-8",
)
PY
expect_failure "许可输入未闭合包清单" "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$MISSING_PACKAGE_INPUTS" --notices "$NOTICES"

"$PYTHON" - "$PACKAGES" "$DRIFT_PACKAGES" <<'PY'
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
data = source.read_text(encoding="utf-8")
destination.write_text(
    data.replace("pkg10\t1-r0\t", "pkg10\t2-r0\t", 1),
    encoding="utf-8",
)
PY
expect_failure "许可输入声明了未知包版本" "$PYTHON" "$TOOL" check-locks \
    --packages "$DRIFT_PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$INPUTS" --notices "$NOTICES"

"$PYTHON" - "$INPUTS" "$MALFORMED_URL_INPUTS" \
    "$MALFORMED_PORT_URL_INPUTS" "$NUL_URL_INPUTS" \
    "$C0_URL_INPUTS" "$DEL_URL_INPUTS" \
    "$EMPTY_FRAGMENT_URL_INPUTS" "$NUL_PATH_INPUTS" <<'PY'
from pathlib import Path
import sys

source, *destinations = map(Path, sys.argv[1:])
url_destinations = destinations[:-1]
nul_path_destination = destinations[-1]
lines = source.read_text(encoding="utf-8").splitlines()
invalid_urls = (
    "https://[::1",
    "https://example.invalid:not-a-port/source",
    "https://example.invalid/\x00source",
    "https://example.invalid/\x01source",
    "https://example.invalid/\x7fsource",
    "https://example.invalid/source#",
)
for destination, invalid_url in zip(url_destinations, invalid_urls):
    rows = [line.split("\t") for line in lines[1:]]
    rows[0][6] = invalid_url
    destination.write_text(
        lines[0]
        + "\n"
        + "\n".join("\t".join(fields) for fields in sorted(rows))
        + "\n",
        encoding="utf-8",
    )
rows = [line.split("\t") for line in lines[1:]]
rows[0][4] = "license-inputs/\x00authority.txt"
nul_path_destination.write_text(
    lines[0]
    + "\n"
    + "\n".join("\t".join(fields) for fields in sorted(rows))
    + "\n",
    encoding="utf-8",
)
PY
for invalid_url_inputs in "$MALFORMED_URL_INPUTS" \
        "$MALFORMED_PORT_URL_INPUTS" "$NUL_URL_INPUTS" \
        "$C0_URL_INPUTS" "$DEL_URL_INPUTS" \
        "$EMPTY_FRAGMENT_URL_INPUTS"; do
    expect_failure "必须是无凭据、无片段的 HTTPS URL" \
        "$PYTHON" "$TOOL" check-locks \
        --packages "$PACKAGES" \
        --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
        --inputs "$invalid_url_inputs" --notices "$NOTICES"
done
expect_failure "不是安全的相对路径" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$NUL_PATH_INPUTS" --notices "$NOTICES"

"$PYTHON" - "$INPUTS" "$DRIFT_DUPLICATE_INPUTS" <<'PY'
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
lines = source.read_text(encoding="utf-8").splitlines()
rows = [line.split("\t") for line in lines[1:]]
for fields in rows:
    if fields[9] == "notice-00-alt":
        fields[8] = "a" * 64
        break
else:
    raise SystemExit("缺少重复源码输入夹具")
destination.write_text(
    lines[0] + "\n" + "\n".join("\t".join(row) for row in sorted(rows)) + "\n",
    encoding="utf-8",
)
PY
expect_failure "同一许可源码输入对应了不一致的锁定值" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$DRIFT_DUPLICATE_INPUTS" --notices "$NOTICES"

"$PYTHON" - "$NOTICES" "$MISSING_SECTION" \
    "$DUPLICATE_SECTION" "$TAMPERED_SECTION" "$UNSCOPED_TEXT" \
    "$AUTHORITY" <<'PY'
from pathlib import Path
import sys

source, missing, duplicate, tampered, unscoped, authority_path = map(
    Path, sys.argv[1:]
)
data = source.read_text(encoding="utf-8")
authority = authority_path.read_text(encoding="utf-8")
block = (
    "===== BEGIN NOTICE: notice-09 =====\n"
    "Notice body 09\n"
    f"{authority}"
    "===== END NOTICE: notice-09 =====\n\n"
)
missing.write_text(data.replace(block, "", 1), encoding="utf-8")
duplicate.write_text(data + block, encoding="utf-8")
tampered.write_text(
    data.replace("Notice body 09\n", "Tampered body 09\n", 1),
    encoding="utf-8",
)
unscoped.write_text("Unscoped notice text\n" + data, encoding="utf-8")
PY
for invalid_notices in "$MISSING_SECTION" "$DUPLICATE_SECTION" \
        "$TAMPERED_SECTION" "$UNSCOPED_TEXT"; do
    expected_error="第三方声明 section 未闭合"
    if [[ "$invalid_notices" == "$DUPLICATE_SECTION" ]]; then
        expected_error="第三方声明含重复的 section"
    elif [[ "$invalid_notices" == "$TAMPERED_SECTION" ]]; then
        expected_error="第三方声明 section 摘要不匹配"
    elif [[ "$invalid_notices" == "$UNSCOPED_TEXT" ]]; then
        expected_error="第三方声明含未受 section 摘要保护的正文"
    fi
    expect_failure "$expected_error" "$PYTHON" "$TOOL" check-locks \
        --packages "$PACKAGES" \
        --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
        --inputs "$INPUTS" --notices "$invalid_notices"
done

"$PYTHON" - "$NOTICES" "$INPUTS" "$AUTHORITY" \
    "$AUTHORITY_MISSING_NOTICES" "$AUTHORITY_MISSING_INPUTS" \
    "$AUTHORITY_DUPLICATE_NOTICES" "$AUTHORITY_DUPLICATE_INPUTS" <<'PY'
import hashlib
from pathlib import Path
import sys

(
    notices,
    inputs,
    authority_path,
    missing_notices,
    missing_inputs,
    duplicate_notices,
    duplicate_inputs,
) = map(Path, sys.argv[1:])
authority = authority_path.read_text(encoding="utf-8")
original_body = "Notice body 09\n" + authority
begin = "===== BEGIN NOTICE: notice-09 =====\n"
end = "===== END NOTICE: notice-09 =====\n"
original_block = begin + original_body + end
notice_data = notices.read_text(encoding="utf-8")
input_lines = inputs.read_text(encoding="utf-8").splitlines()


def write_fixture(body, notices_path, inputs_path):
    notices_path.write_text(
        notice_data.replace(original_block, begin + body + end, 1),
        encoding="utf-8",
    )
    digest = hashlib.sha256(body.encode()).hexdigest()
    rows = [line.split("\t") for line in input_lines[1:]]
    for fields in rows:
        if fields[9] == "notice-09":
            fields[10] = digest
    inputs_path.write_text(
        input_lines[0]
        + "\n"
        + "\n".join("\t".join(fields) for fields in rows)
        + "\n",
        encoding="utf-8",
    )


write_fixture("Notice body 09\n", missing_notices, missing_inputs)
write_fixture(
    original_body + authority,
    duplicate_notices,
    duplicate_inputs,
)
PY
expect_failure "权威许可输入必须逐字且唯一出现在声明 section" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$AUTHORITY_MISSING_INPUTS" \
    --notices "$AUTHORITY_MISSING_NOTICES"
expect_failure "权威许可输入必须逐字且唯一出现在声明 section" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$AUTHORITY_DUPLICATE_INPUTS" \
    --notices "$AUTHORITY_DUPLICATE_NOTICES"

mv "$AUTHORITY" "$TMP/mit-authority.saved"
ln -s ../mit-authority.saved "$AUTHORITY"
expect_failure "权威许可输入不存在或是符号链接" \
    "$PYTHON" "$TOOL" check-locks "${COMMON[@]}"
rm "$AUTHORITY"
mv "$TMP/mit-authority.saved" "$AUTHORITY"

mv "$TMP/license-inputs" "$TMP/license-inputs.saved"
ln -s license-inputs.saved "$TMP/license-inputs"
expect_failure "权威许可输入路径不存在、含符号链接或非实体目录" \
    "$PYTHON" "$TOOL" check-locks "${COMMON[@]}"
rm "$TMP/license-inputs"
mv "$TMP/license-inputs.saved" "$TMP/license-inputs"

mv "$AUTHORITY" "$TMP/mit-authority.saved"
mkfifo "$AUTHORITY"
expect_failure "权威许可输入必须是普通文件" \
    "$PYTHON" "$TOOL" check-locks "${COMMON[@]}"
rm "$AUTHORITY"
mv "$TMP/mit-authority.saved" "$AUTHORITY"

mv "$CACHE/aports" "$CACHE/aports.saved"
ln -s aports.saved "$CACHE/aports"
expect_failure "源码缓存资产路径不存在、含符号链接或非实体目录" \
    "$PYTHON" "$TOOL" validate-sources "$CACHE" "${COMMON[@]}"
rm "$CACHE/aports"
mv "$CACHE/aports.saved" "$CACHE/aports"

"$PYTHON" - "$INPUTS" "$MISSING_MEMBER_INPUTS" \
    "$DRIFT_MEMBER_INPUTS" <<'PY'
from pathlib import Path
import sys

source, missing, drift = map(Path, sys.argv[1:])
lines = source.read_text(encoding="utf-8").splitlines()
rows = [line.split("\t") for line in lines[1:]]
target = next(
    fields
    for fields in rows
    if fields[4] == "distfiles/origin01/upstream.tar.gz"
    and fields[5] == "upstream/COPYING"
)
missing_rows = [fields[:] for fields in rows]
missing_target = missing_rows[rows.index(target)]
missing_target[5] = "upstream/MISSING"
drift_rows = [fields[:] for fields in rows]
drift_target = drift_rows[rows.index(target)]
drift_target[8] = "a" * 64
for destination, changed_rows in (
    (missing, missing_rows),
    (drift, drift_rows),
):
    destination.write_text(
        lines[0]
        + "\n"
        + "\n".join("\t".join(fields) for fields in sorted(changed_rows))
        + "\n",
        encoding="utf-8",
    )
PY
expect_failure "许可源码归档缺少成员" \
    "$PYTHON" "$TOOL" validate-sources "$CACHE" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$MISSING_MEMBER_INPUTS" --notices "$NOTICES"
expect_failure "许可源码成员大小或 SHA-256 不匹配" \
    "$PYTHON" "$TOOL" validate-sources "$CACHE" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" --assets "$ASSETS" \
    --inputs "$DRIFT_MEMBER_INPUTS" --notices "$NOTICES"

DUPLICATE_ASSETS="$TMP/source-assets-duplicate-member.tsv"
DUPLICATE_CACHE="$TMP/cache-duplicate-member"
cp -R "$CACHE" "$DUPLICATE_CACHE"
"$PYTHON" - "$DUPLICATE_CACHE/aports/origin00.tar.gz" \
    "$ASSETS" "$DUPLICATE_ASSETS" <<'PY'
import hashlib
import io
from pathlib import Path
import sys
import tarfile

archive_path, source_manifest, destination_manifest = map(Path, sys.argv[1:])
body = b"aports license fixture\n"
output = io.BytesIO()
with tarfile.open(
    fileobj=output, mode="w:gz", format=tarfile.USTAR_FORMAT
) as archive:
    for _ in range(2):
        member = tarfile.TarInfo("root/LICENSE")
        member.size = len(body)
        member.mode = 0o644
        archive.addfile(member, io.BytesIO(body))
data = output.getvalue()
archive_path.write_bytes(data)
lines = source_manifest.read_text(encoding="utf-8").splitlines()
fields = lines[1].split("\t")
fields[4] = str(len(data))
fields[5] = hashlib.sha512(data).hexdigest()
lines[1] = "\t".join(fields)
destination_manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY
expect_failure "许可源码归档含重复成员" \
    "$PYTHON" "$TOOL" validate-sources "$DUPLICATE_CACHE" \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --assets "$DUPLICATE_ASSETS" \
    --inputs "$INPUTS" --notices "$NOTICES"

"$PYTHON" - "$ASSETS" "$INPUTS" \
    "$UNREFERENCED_ASSETS" "$UNREFERENCED_INPUTS" <<'PY'
import hashlib
from pathlib import Path
import sys

source_assets, source_inputs, destination_assets, destination_inputs = map(
    Path, sys.argv[1:]
)
data = b"unreferenced source\n"
asset_lines = source_assets.read_text(encoding="utf-8").splitlines()
asset_rows = [line.split("\t") for line in asset_lines[1:]]
old_path = "distfiles/origin09/raw.txt"
for fields in asset_rows:
    if fields[3] == old_path:
        fields[3] = "distfiles/origin09/zz-extra.txt"
        fields[4] = str(len(data))
        fields[5] = hashlib.sha512(data).hexdigest()
        fields[6] = "https://example.invalid/zz-extra.txt"
        break
else:
    raise SystemExit("缺少待替换的上游资产夹具")
destination_assets.write_text(
    asset_lines[0]
    + "\n"
    + "\n".join("\t".join(fields) for fields in sorted(asset_rows))
    + "\n",
    encoding="utf-8",
)

input_lines = source_inputs.read_text(encoding="utf-8").splitlines()
input_rows = [
    line.split("\t")
    for line in input_lines[1:]
    if line.split("\t")[4] != old_path
]
destination_inputs.write_text(
    input_lines[0]
    + "\n"
    + "\n".join("\t".join(fields) for fields in input_rows)
    + "\n",
    encoding="utf-8",
)
PY
expect_failure "许可输入未闭合源码资产清单" \
    "$PYTHON" "$TOOL" check-locks \
    --packages "$PACKAGES" \
    --static-link-sources "$STATIC_LINK_SOURCES" \
    --assets "$UNREFERENCED_ASSETS" \
    --inputs "$UNREFERENCED_INPUTS" --notices "$NOTICES"

echo "Alpine AArch64 许可输入测试通过"

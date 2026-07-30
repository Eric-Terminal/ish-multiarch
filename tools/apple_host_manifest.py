from __future__ import annotations

from dataclasses import dataclass
from pathlib import PurePosixPath
import re
import stat
import urllib.parse


LOCK_RELATIVE = PurePosixPath("third_party/apple-host")
DEPENDENCIES_HEADER = (
    "component\tversion\tversion_source\tgitlink_path\tgitlink_commit"
    "\tsource_url\tdelivery_unit\tdelivery_kind\tdelivery_name\tinput_count\tinput_sha256"
)
TARGETS_HEADER = "target\trelease_scope\tinput_scope\tcomponent\tdelivery_kind\tdelivery_name"
LICENSES_HEADER = "delivery_unit\tcomponent\trole\tpath\tsize\tsha256"
NOTICE_FRAGMENTS_HEADER = "path\tstart_line\tend_line\tsize\tsha256"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
NAME = re.compile(r"^[a-z0-9][a-z0-9+._-]*$")
VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._-]*$")
EXPECTED_SCOPES = {
    "iSH": "product",
    "iSH+Linux": "undecided",
    "iSHFileProvider": "embedded",
    "iSHWatch": "product",
}
EXPECTED_VERSION_SOURCES = {
    "hterm": "deps/libapps/hterm/package.json",
    "intl-segmenter": "gitlink",
    "libarchive": "deps/libarchive/configure.ac",
    "libdot": "deps/libapps/libdot/package.json",
    "linux": "gitlink",
    "wcwidth": "deps/libapps/libdot/third_party/wcwidth/METADATA",
}
EXPECTED_COMPONENTS = set(EXPECTED_VERSION_SOURCES)
EXPECTED_GITLINK_PATHS = {
    "hterm": "deps/libapps", "intl-segmenter": "deps/libapps",
    "libarchive": "deps/libarchive", "libdot": "deps/libapps",
    "linux": "deps/linux", "wcwidth": "deps/libapps",
}
EXPECTED_DELIVERY_CONTRACTS = {
    "hterm": ("hterm-bundle", "generated-resource", "hterm_all.js"),
    "intl-segmenter": ("hterm-bundle", "generated-resource", "hterm_all.js"),
    "libarchive": ("libarchive", "static-library", "libarchive.a"),
    "libdot": ("hterm-bundle", "generated-resource", "hterm_all.js"),
    "linux": ("linux-kernel", "static-library", "liblinux.a"),
    "wcwidth": ("hterm-bundle", "generated-resource", "hterm_all.js"),
}
TARGET_DELIVERY_ALIASES = {
    ("iSHWatch", "libarchive"): "libarchive-watchOS.a",
}
MATERIAL_ICON_REVISION = "3d4a32b327272c458e12586437c3ca0696b28a69"
MATERIAL_ICON_SNAPSHOT_BASE = (
    "third_party/apple-host/material-design-icons/"
    f"{MATERIAL_ICON_REVISION}"
)
MATERIAL_ICON_LICENSE_PATH = f"{MATERIAL_ICON_SNAPSHOT_BASE}/LICENSE"
MATERIAL_ICON_README_PATH = f"{MATERIAL_ICON_SNAPSHOT_BASE}/README.md"
MATERIAL_ICON_UPSTREAM_PATHS = (
    "hardware/svg/production/ic_keyboard_arrow_down_24px.svg",
    "hardware/svg/production/ic_keyboard_arrow_up_24px.svg",
    "navigation/svg/production/ic_close_24px.svg",
)
MATERIAL_ICON_SNAPSHOT_PATHS = tuple(
    f"{MATERIAL_ICON_SNAPSHOT_BASE}/{path}"
    for path in MATERIAL_ICON_UPSTREAM_PATHS
)
LIB_COLORS_CSSWG_REVISION = (
    "7b4ea6de873df70dd4c79f3efe86bf2ea5964019"
)
LIB_COLORS_CSSWG_SOURCE_URL = (
    "https://github.com/w3c/csswg-drafts"
)
LIB_COLORS_CSSWG_SNAPSHOT_BASE = (
    "third_party/apple-host/csswg-drafts/"
    f"{LIB_COLORS_CSSWG_REVISION}"
)
LIB_COLORS_CSSWG_LICENSE_PATH = (
    f"{LIB_COLORS_CSSWG_SNAPSHOT_BASE}/LICENSE.md"
)
LIB_COLORS_CSSWG_OVERVIEW_PATH = (
    f"{LIB_COLORS_CSSWG_SNAPSHOT_BASE}/css-color-4/Overview.bs"
)
LIB_COLORS_W3C_LICENSE_VERSION = "2015"
LIB_COLORS_W3C_LICENSE_URL = (
    "https://www.w3.org/copyright/software-license-2015/"
)
LIB_COLORS_W3C_LICENSE_BASE = (
    "third_party/apple-host/w3c-software-and-document-license/"
    f"{LIB_COLORS_W3C_LICENSE_VERSION}"
)
LIB_COLORS_W3C_LICENSE_HTML_PATH = (
    f"{LIB_COLORS_W3C_LICENSE_BASE}/software-and-document-license.html"
)
LIB_COLORS_W3C_NOTICE_PATH = (
    f"{LIB_COLORS_W3C_LICENSE_BASE}/NOTICE.txt"
)
LIB_COLORS_XORG_REVISION = (
    "d96f362956d9e58cbb46740f825d5bad50f0fbf1"
)
LIB_COLORS_XORG_SOURCE_URL = (
    "https://gitlab.freedesktop.org/xorg/app/rgb"
)
LIB_COLORS_XORG_SNAPSHOT_BASE = (
    "third_party/apple-host/xorg-rgb/"
    f"{LIB_COLORS_XORG_REVISION}"
)
LIB_COLORS_XORG_LICENSE_PATH = (
    f"{LIB_COLORS_XORG_SNAPSHOT_BASE}/COPYING"
)
LIB_COLORS_XORG_RGB_PATH = (
    f"{LIB_COLORS_XORG_SNAPSHOT_BASE}/rgb.txt"
)
LIB_COLORS_DEBIAN_TAG = "xorg-1_7.6+12"
LIB_COLORS_DEBIAN_TAG_OBJECT = (
    "12c57230373f9f51f4af46b6312de742e2b402aa"
)
LIB_COLORS_DEBIAN_REVISION = (
    "75d568a94a7ccfb37a51711c9f1ac42f584ec140"
)
LIB_COLORS_DEBIAN_SOURCE_URL = (
    "https://salsa.debian.org/xorg-team/debian/xorg"
)
LIB_COLORS_DEBIAN_SNAPSHOT_BASE = (
    "third_party/apple-host/debian-xorg/"
    f"{LIB_COLORS_DEBIAN_REVISION}"
)
LIB_COLORS_DEBIAN_LICENSE_PATH = (
    f"{LIB_COLORS_DEBIAN_SNAPSHOT_BASE}/debian/copyright"
)
LIB_COLORS_DEBIAN_RGB_PATH = (
    f"{LIB_COLORS_DEBIAN_SNAPSHOT_BASE}/debian/local/rgb.txt"
)
WCWIDTH_UCD_VERSION = "13.0.0"
WCWIDTH_UCD_ARCHIVE_URL = (
    "https://www.unicode.org/Public/zipped/13.0.0/UCD.zip"
)
WCWIDTH_UCD_ARCHIVE_SIZE = 7_537_310
WCWIDTH_UCD_ARCHIVE_SHA256 = (
    "2f76973b4d36ae45584f5a45ec65b47138932d777dd23a5669c89535ef3da951"
)
WCWIDTH_UCD_SNAPSHOT_BASE = (
    "third_party/apple-host/unicode-ucd/13.0.0"
)
WCWIDTH_UCD_README_PATH = f"{WCWIDTH_UCD_SNAPSHOT_BASE}/ReadMe.txt"
WCWIDTH_UCD_DATA_FILES = (
    (
        "PropList.txt",
        "7d2f44c56fab8d8d787a0f70bc1518866d6f567c",
    ),
    (
        "UnicodeData.txt",
        "e22f967bbab8f2477a43533a334e21ebc0728eda",
    ),
    (
        "EastAsianWidth.txt",
        "b43aec92738c51a231709632a12998ef64fe7f34",
    ),
)
WCWIDTH_UCD_DATA_PATHS = tuple(
    f"{WCWIDTH_UCD_SNAPSHOT_BASE}/{name}"
    for name, _blob in WCWIDTH_UCD_DATA_FILES
)
WCWIDTH_UNICODETOOLS_REVISION = (
    "a87ae283358bf1858e7cbf6520c6bba0d3b58710"
)
WCWIDTH_UNICODETOOLS_TAG = "release-2020-09-15"
WCWIDTH_UNICODETOOLS_SOURCE_URL = (
    "https://github.com/unicode-org/unicodetools"
)
WCWIDTH_UNICODETOOLS_DATA_BASE = (
    "unicodetools/data/ucd/13.0.0-Update"
)
UNICODE_DATA_LICENSE_PATH = (
    "third_party/apple-host/unicodetools/"
    f"{WCWIDTH_UNICODETOOLS_REVISION}/LICENSE"
)
UNICODE_DATA_LICENSE_GIT_BLOB = (
    "500dbd5463e43403fa163a8095828e7f6c1539c6"
)
WCWIDTH_UCD_LICENSE_PATH = UNICODE_DATA_LICENSE_PATH
WCWIDTH_UCD_LICENSE_GIT_BLOB = UNICODE_DATA_LICENSE_GIT_BLOB
LIBARCHIVE_UCD_VERSION = "6.0.0"
LIBARCHIVE_UCD_ARCHIVE_URL = (
    "https://www.unicode.org/Public/zipped/6.0.0/UCD.zip"
)
LIBARCHIVE_UCD_ARCHIVE_SIZE = 2_581_878
LIBARCHIVE_UCD_ARCHIVE_SHA256 = (
    "f4c32d5d3f2ba8e73c156b13e8fb7742a1e0cab88b6a0bf765dc3b42109e915c"
)
LIBARCHIVE_UCD_SNAPSHOT_BASE = (
    "third_party/apple-host/unicode-ucd/6.0.0"
)
LIBARCHIVE_UCD_README_PATH = (
    f"{LIBARCHIVE_UCD_SNAPSHOT_BASE}/ReadMe.txt"
)
LIBARCHIVE_UCD_DATA_FILES = (
    (
        "CompositionExclusions.txt",
        "e39c651811014a593ed14dea9a868e8bb02cd3c3",
    ),
    (
        "UnicodeData.txt",
        "8d7222b13789c43e5ed36ddf0d3eb7ebe8d72c7b",
    ),
)
LIBARCHIVE_UCD_DATA_PATHS = tuple(
    f"{LIBARCHIVE_UCD_SNAPSHOT_BASE}/{name}"
    for name, _blob in LIBARCHIVE_UCD_DATA_FILES
)
LIBARCHIVE_UCD_LICENSE_PATH = UNICODE_DATA_LICENSE_PATH
LIBARCHIVE_UCD_LICENSE_GIT_BLOB = UNICODE_DATA_LICENSE_GIT_BLOB
LIBARCHIVE_UNICODETOOLS_REVISION = WCWIDTH_UNICODETOOLS_REVISION
LIBARCHIVE_UNICODETOOLS_SOURCE_URL = WCWIDTH_UNICODETOOLS_SOURCE_URL
LIBARCHIVE_UNICODETOOLS_DATA_BASE = (
    "unicodetools/data/ucd/6.0.0-Update"
)
LIBARCHIVE_UNICODE_GENERATOR_PATH = (
    "deps/libarchive/build/utils/gen_archive_string_composition_h.sh"
)
LIBARCHIVE_INLINE_NOTICE_PATHS = {
    "deps/libarchive/libarchive/archive_blake2.h",
    "deps/libarchive/libarchive/archive_blake2_impl.h",
    "deps/libarchive/libarchive/archive_blake2s_ref.c",
    "deps/libarchive/libarchive/archive_blake2sp_ref.c",
    "deps/libarchive/libarchive/archive_entry.c",
    "deps/libarchive/libarchive/archive_getdate.c",
    "deps/libarchive/libarchive/archive_pack_dev.c",
    "deps/libarchive/libarchive/archive_pack_dev.h",
    "deps/libarchive/libarchive/archive_ppmd7.c",
    "deps/libarchive/libarchive/archive_ppmd7_private.h",
    "deps/libarchive/libarchive/archive_ppmd8.c",
    "deps/libarchive/libarchive/archive_ppmd8_private.h",
    "deps/libarchive/libarchive/archive_ppmd_private.h",
    "deps/libarchive/libarchive/archive_random.c",
    "deps/libarchive/libarchive/archive_rb.c",
    "deps/libarchive/libarchive/archive_rb.h",
    "deps/libarchive/libarchive/archive_read_support_filter_compress.c",
    "deps/libarchive/libarchive/archive_read_support_format_7zip.c",
    "deps/libarchive/libarchive/archive_read_support_format_mtree.c",
    "deps/libarchive/libarchive/archive_string.c",
    "deps/libarchive/libarchive/archive_string_composition.h",
    "deps/libarchive/libarchive/archive_windows.c",
    "deps/libarchive/libarchive/archive_write_add_filter_compress.c",
    "deps/libarchive/libarchive/archive_xxhash.h",
    "deps/libarchive/libarchive/xxhash.c",
}
REQUIRED_NOTICE_FRAGMENT_RANGES = {
    ("deps/libapps/libdot/doc/ChangeLog.md", 134, 136),
    ("deps/libapps/libdot/js/lib_colors.js", 322, 323),
    ("deps/libapps/libdot/js/lib_colors.js", 629, 640),
    ("deps/libapps/libdot/js/lib_colors.js", 755, 757),
    (
        "deps/libapps/libdot/third_party/wcwidth/lib_wc.js",
        7,
        77,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/lib_wc.js",
        117,
        118,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/lib_wc.js",
        235,
        236,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/lib_wc.js",
        326,
        327,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/ranges.py",
        15,
        17,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/ranges.py",
        28,
        32,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/ranges.py",
        54,
        60,
    ),
    (
        "deps/libapps/libdot/third_party/wcwidth/ranges.py",
        114,
        130,
    ),
    ("deps/libarchive/libarchive/archive_entry.c", 1618, 1650),
    ("deps/libarchive/libarchive/archive_ppmd8.c", 1118, 1122),
    ("deps/libarchive/libarchive/archive_random.c", 103, 131),
    (
        "deps/libarchive/libarchive/archive_read_support_format_7zip.c",
        3605,
        3611,
    ),
    (
        "deps/libarchive/libarchive/archive_read_support_format_7zip.c",
        3703,
        3709,
    ),
    (
        "deps/libarchive/libarchive/archive_read_support_format_mtree.c",
        1413,
        1413,
    ),
    ("deps/libarchive/libarchive/archive_string.c", 2797, 2800),
    ("deps/libarchive/libarchive/archive_string.c", 3043, 3046),
    ("deps/libarchive/libarchive/archive_windows.c", 789, 829),
    (MATERIAL_ICON_README_PATH, 37, 40),
}
REQUIRED_LICENSE_KEYS = {
    (
        "hterm-bundle",
        "hterm",
        "provenance",
        "deps/libapps/hterm/concat/hterm_resources.concat",
    ),
    (
        "hterm-bundle",
        "hterm",
        "provenance",
        "deps/libapps/hterm/images/close.svg",
    ),
    (
        "hterm-bundle",
        "hterm",
        "provenance",
        "deps/libapps/hterm/images/keyboard_arrow_down.svg",
    ),
    (
        "hterm-bundle",
        "hterm",
        "provenance",
        "deps/libapps/hterm/images/keyboard_arrow_up.svg",
    ),
    ("hterm-bundle", "hterm", "license", "deps/libapps/hterm/LICENSE"),
    (
        "hterm-bundle",
        "libdot",
        "inline-notice",
        "deps/libapps/libdot/js/lib_colors.js",
    ),
    ("hterm-bundle", "libdot", "license", "deps/libapps/libdot/LICENSE"),
    (
        "hterm-bundle",
        "libdot",
        "license",
        LIB_COLORS_DEBIAN_LICENSE_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "license",
        LIB_COLORS_W3C_NOTICE_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "license",
        LIB_COLORS_XORG_LICENSE_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "provenance",
        LIB_COLORS_CSSWG_LICENSE_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "provenance",
        LIB_COLORS_CSSWG_OVERVIEW_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "provenance",
        LIB_COLORS_DEBIAN_RGB_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "provenance",
        LIB_COLORS_W3C_LICENSE_HTML_PATH,
    ),
    (
        "hterm-bundle",
        "libdot",
        "provenance",
        LIB_COLORS_XORG_RGB_PATH,
    ),
    ("hterm-bundle", "intl-segmenter", "license",
     "deps/libapps/libdot/third_party/intl-segmenter/LICENSE.md"),
    ("hterm-bundle", "wcwidth", "license",
     "deps/libapps/libdot/third_party/wcwidth/LICENSE.md"),
    (
        "hterm-bundle",
        "wcwidth",
        "inline-notice",
        "deps/libapps/libdot/third_party/wcwidth/lib_wc.js",
    ),
    (
        "hterm-bundle",
        "wcwidth",
        "provenance",
        "deps/libapps/libdot/doc/ChangeLog.md",
    ),
    (
        "hterm-bundle",
        "wcwidth",
        "provenance",
        "deps/libapps/libdot/third_party/wcwidth/ranges.py",
    ),
    ("libarchive", "libarchive", "license", "deps/libarchive/COPYING"),
    ("linux-kernel", "linux", "license", "deps/linux/COPYING"),
    ("hterm-bundle", "hterm", "license", MATERIAL_ICON_LICENSE_PATH),
    ("hterm-bundle", "hterm", "provenance", MATERIAL_ICON_README_PATH),
    (
        "hterm-bundle",
        "wcwidth",
        "license",
        WCWIDTH_UCD_LICENSE_PATH,
    ),
    (
        "hterm-bundle",
        "wcwidth",
        "provenance",
        WCWIDTH_UCD_README_PATH,
    ),
    (
        "libarchive",
        "libarchive",
        "license",
        LIBARCHIVE_UCD_LICENSE_PATH,
    ),
    (
        "libarchive",
        "libarchive",
        "provenance",
        LIBARCHIVE_UCD_README_PATH,
    ),
    (
        "libarchive",
        "libarchive",
        "provenance",
        LIBARCHIVE_UNICODE_GENERATOR_PATH,
    ),
} | {
    ("libarchive", "libarchive", "inline-notice", path)
    for path in LIBARCHIVE_INLINE_NOTICE_PATHS
} | {
    ("hterm-bundle", "hterm", "provenance", path)
    for path in MATERIAL_ICON_SNAPSHOT_PATHS
} | {
    ("hterm-bundle", "wcwidth", "provenance", path)
    for path in WCWIDTH_UCD_DATA_PATHS
} | {
    ("libarchive", "libarchive", "provenance", path)
    for path in LIBARCHIVE_UCD_DATA_PATHS
}
APPLE_HOST_RAW_INPUT_PATHS = tuple(
    sorted(
        {
            path
            for _unit, _component, _role, path in REQUIRED_LICENSE_KEYS
            if PurePosixPath(path).is_relative_to(LOCK_RELATIVE)
        }
    )
)


class HostInputError(Exception):
    pass


@dataclass(frozen=True)
class Dependency:
    component: str
    version: str
    version_source: str
    gitlink_path: str
    gitlink_commit: str
    source_url: str
    delivery_unit: str
    delivery_kind: str
    delivery_name: str
    input_count: int
    input_sha256: str


@dataclass(frozen=True)
class TargetInput:
    target: str
    release_scope: str
    input_scope: str
    component: str
    delivery_kind: str
    delivery_name: str


@dataclass(frozen=True)
class LicenseInput:
    delivery_unit: str
    component: str
    role: str
    path: str
    size: int
    sha256: str


@dataclass(frozen=True)
class NoticeFragment:
    path: str
    start_line: int
    end_line: int
    size: int
    sha256: str


def fail(message):
    raise HostInputError(message)


def validate_relative(value, description):
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or str(path) != value
        or any(part in ("", ".", "..") for part in path.parts)
        or "\\" in value
        or any(
            ord(character) < 0x20 or ord(character) == 0x7F
            for character in value
        )
    ):
        fail(f"{description}不是安全的仓库相对路径：{value}")
    return path


def path_from_root(root, value, description):
    relative = validate_relative(value, description)
    current = root
    # 锁定输入不能借符号链接跳出仓库或替换中间目录。
    for part in relative.parts:
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"{description}不存在：{value}（{error}）")
        if stat.S_ISLNK(metadata.st_mode):
            fail(f"{description}不能经过符号链接：{value}")
    try:
        metadata = current.lstat()
    except OSError as error:
        fail(f"{description}不存在：{value}（{error}）")
    if not stat.S_ISREG(metadata.st_mode):
        fail(f"{description}必须是常规文件：{value}")
    return current


def read_regular(root, value, description):
    path = path_from_root(root, value, description)
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"无法读取{description} {value}：{error}")


def decode_utf8(data, description):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        fail(f"{description}不是 UTF-8 文本")


def read_lock(root, name, header, description):
    relative = str(LOCK_RELATIVE / name)
    data = read_regular(root, relative, description)
    if not data or not data.endswith(b"\n") or b"\r" in data:
        fail(f"{description}必须非空、只使用 LF 且以换行结束")
    lines = decode_utf8(data, description).splitlines()
    if lines[0] != header:
        fail(f"{description}表头不符合固定格式")
    rows = [line.split("\t") for line in lines[1:]]
    if not rows:
        fail(f"{description}没有数据行")
    tuples = [tuple(row) for row in rows]
    if tuples != sorted(tuples) or len(tuples) != len(set(tuples)):
        fail(f"{description}必须按完整记录唯一排序")
    return rows


def validate_url(value, description):
    try:
        parsed = urllib.parse.urlsplit(value)
        _port = parsed.port
    except ValueError:
        fail(f"{description}不是有效的 HTTPS URL")
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        fail(f"{description}必须是无凭据、查询和片段的 HTTPS URL")


def parse_dependencies(root):
    rows = read_lock(root, "dependencies.tsv", DEPENDENCIES_HEADER, "宿主组件锁")
    dependencies = {}
    for row in rows:
        if len(row) != 11 or any(not field for field in row):
            fail("宿主组件锁含空字段或列数错误")
        (
            component,
            version,
            version_source,
            gitlink_path,
            gitlink_commit,
            source_url,
            delivery_unit,
            delivery_kind,
            delivery_name,
            count_text,
            input_sha256,
        ) = row
        if (
            not NAME.fullmatch(component)
            or not VERSION.fullmatch(version)
            or component in dependencies
            or not NAME.fullmatch(delivery_unit)
            or delivery_kind not in {"generated-resource", "static-library"}
            or not HEX40.fullmatch(gitlink_commit)
            or not HEX64.fullmatch(input_sha256)
        ):
            fail(f"宿主组件锁字段格式非法：{component}")
        validate_relative(gitlink_path, "gitlink 路径")
        if version_source != "gitlink":
            validate_relative(version_source, "版本来源路径")
        validate_url(source_url, "子模块来源")
        try:
            input_count = int(count_text)
        except ValueError:
            fail(f"宿主组件输入数量非法：{component}")
        if input_count <= 0:
            fail(f"宿主组件输入数量必须为正数：{component}")
        dependencies[component] = Dependency(
            component,
            version,
            version_source,
            gitlink_path,
            gitlink_commit,
            source_url,
            delivery_unit,
            delivery_kind,
            delivery_name,
            input_count,
            input_sha256,
        )
    if set(dependencies) != EXPECTED_COMPONENTS:
        fail("宿主组件集合漂移")
    for component, expected in EXPECTED_VERSION_SOURCES.items():
        if dependencies[component].version_source != expected:
            fail(f"{component} 的版本来源路径漂移")
    for component, expected in EXPECTED_GITLINK_PATHS.items():
        if dependencies[component].gitlink_path != expected:
            fail(f"{component} 的 gitlink 路径漂移")
    for component, expected in EXPECTED_DELIVERY_CONTRACTS.items():
        dependency = dependencies[component]
        actual = (
            dependency.delivery_unit,
            dependency.delivery_kind,
            dependency.delivery_name,
        )
        if actual != expected:
            fail(f"{component} 的交付合同漂移")
    return dependencies


def parse_targets(root, dependencies):
    rows = read_lock(root, "target-inputs.tsv", TARGETS_HEADER, "宿主 target 路由锁")
    targets = []
    unit_contracts = {}
    for dependency in dependencies.values():
        contract = (dependency.delivery_kind, dependency.delivery_name)
        previous = unit_contracts.setdefault(dependency.delivery_unit, contract)
        if previous != contract:
            fail(f"{dependency.delivery_unit} 对应多个交付合同")
    for row in rows:
        if len(row) != 6 or any(not field for field in row):
            fail("宿主 target 路由锁含空字段或列数错误")
        target = TargetInput(*row)
        if (
            target.release_scope not in {"product", "embedded", "undecided"}
            or target.input_scope not in {"boundary", "platform", "vendored"}
            or target.delivery_kind not in {
                "generated-resource",
                "no-vendored",
                "platform-link",
                "static-library",
            }
            or any(
                ord(character) < 0x20 or ord(character) == 0x7F
                for character in target.delivery_name
            )
        ):
            fail(f"宿主 target 路由字段非法：{target.target}")
        # component 必须绑定组件锁中的唯一合同，不能只靠同名叶子文件。
        if target.input_scope == "platform":
            if (
                target.component != "apple-sdk"
                or target.delivery_kind != "platform-link"
            ):
                fail(f"{target.target} 的平台输入合同漂移")
            if target.target == "iSHWatch":
                if not re.fullmatch(r"-l[A-Za-z0-9+._-]+", target.delivery_name):
                    fail("Watch 平台输入必须是显式 -l 链接项")
            elif target.target in {"iSH", "iSH+Linux"}:
                if target.delivery_name.startswith("-"):
                    fail("App 平台输入必须是 PBX 路径")
                validate_relative(target.delivery_name, "App 平台输入路径")
            else:
                fail(f"{target.target} 不能声明平台输入")
        elif target.input_scope == "vendored":
            contract = unit_contracts.get(target.component)
            delivery_name = target.delivery_name
            if target.delivery_kind == "generated-resource":
                validate_relative(delivery_name, "生成资源交付路径")
                delivery_name = PurePosixPath(delivery_name).name
            expected_name = TARGET_DELIVERY_ALIASES.get(
                (target.target, target.component),
                contract[1] if contract is not None else None,
            )
            if (
                contract is None
                or contract[0] != target.delivery_kind
                or delivery_name != expected_name
            ):
                fail(f"{target.target} 的 vendored 输入合同漂移")
        elif (
            target.component,
            target.delivery_kind,
            target.delivery_name,
        ) != ("project", "no-vendored", "-"):
            fail(f"{target.target} 的边界输入合同漂移")
        targets.append(target)
    route_keys = [(target.target, target.delivery_name) for target in targets]
    if len(route_keys) != len(set(route_keys)):
        fail("同一 target 不能重复声明同一交付名称")
    actual_scopes = {}
    for target in targets:
        previous = actual_scopes.setdefault(target.target, target.release_scope)
        if previous != target.release_scope:
            fail(f"{target.target} 同时声明了多个发行范围")
    if actual_scopes != EXPECTED_SCOPES:
        fail("Apple 交付 target 或发行范围漂移")
    boundary = [target for target in targets if target.input_scope == "boundary"]
    if boundary != [
        TargetInput(
            "iSHFileProvider",
            "embedded",
            "boundary",
            "project",
            "no-vendored",
            "-",
        )
    ]:
        fail("iSHFileProvider 的无 vendored 边界漂移")
    if any(
        target.input_scope == "vendored"
        for target in targets
        if target.target == "iSHFileProvider"
    ):
        fail("FileProvider 不能声明宿主 vendored 输入")
    watch_vendored = [
        target
        for target in targets
        if target.target == "iSHWatch" and
        target.input_scope == "vendored"
    ]
    if watch_vendored != [
        TargetInput(
            "iSHWatch",
            "product",
            "vendored",
            "libarchive",
            "static-library",
            "libarchive-watchOS.a",
        )
    ]:
        fail("Watch 必须只声明 libarchive vendored 输入")
    return targets


def parse_license_inputs(root, dependencies):
    rows = read_lock(
        root, "license-inputs.tsv", LICENSES_HEADER, "宿主许可复核输入锁"
    )
    inputs = []
    for row in rows:
        if len(row) != 6 or any(not field for field in row):
            fail("宿主许可复核输入锁含空字段或列数错误")
        delivery_unit, component, role, path, size_text, sha256 = row
        if (
            component not in dependencies
            or dependencies[component].delivery_unit != delivery_unit
            or role not in {"inline-notice", "license", "provenance"}
            or not HEX64.fullmatch(sha256)
        ):
            fail(f"宿主许可复核输入字段非法：{path}")
        validate_relative(path, "许可复核输入路径")
        try:
            size = int(size_text)
        except ValueError:
            fail(f"宿主许可复核输入大小非法：{path}")
        if size <= 0:
            fail(f"宿主许可复核输入不能为空：{path}")
        inputs.append(LicenseInput(delivery_unit, component, role, path, size, sha256))
    keys = {
        (item.delivery_unit, item.component, item.role, item.path)
        for item in inputs
    }
    if keys != REQUIRED_LICENSE_KEYS:
        fail("宿主许可复核输入路径集合漂移")
    return inputs


def parse_notice_fragments(root, license_inputs):
    rows = read_lock(
        root,
        "notice-fragments.tsv",
        NOTICE_FRAGMENTS_HEADER,
        "宿主声明中段片段锁",
    )
    licensed_paths = {item.path for item in license_inputs}
    fragments = []
    for row in rows:
        if len(row) != 5 or any(not field for field in row):
            fail("宿主声明中段片段锁含空字段或列数错误")
        path, start_text, end_text, size_text, sha256 = row
        validate_relative(path, "宿主声明片段路径")
        if path not in licensed_paths or not HEX64.fullmatch(sha256):
            fail(f"宿主声明中段片段字段非法：{path}")
        try:
            start_line = int(start_text)
            end_line = int(end_text)
            size = int(size_text)
        except ValueError:
            fail(f"宿主声明中段片段数字字段非法：{path}")
        if start_line <= 0 or end_line < start_line or size <= 0:
            fail(f"宿主声明中段片段范围非法：{path}")
        fragments.append(
            NoticeFragment(path, start_line, end_line, size, sha256)
        )
    ranges = {
        (fragment.path, fragment.start_line, fragment.end_line)
        for fragment in fragments
    }
    if ranges != REQUIRED_NOTICE_FRAGMENT_RANGES:
        fail("宿主声明中段片段路径或范围集合漂移")
    return fragments

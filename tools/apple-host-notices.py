#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import html
import importlib.util
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile

from apple_host_manifest import (
    HostInputError,
    LIBARCHIVE_UCD_ARCHIVE_SHA256,
    LIBARCHIVE_UCD_ARCHIVE_SIZE,
    LIBARCHIVE_UCD_ARCHIVE_URL,
    LIBARCHIVE_UCD_DATA_FILES,
    LIBARCHIVE_UCD_DATA_PATHS,
    LIBARCHIVE_UCD_LICENSE_GIT_BLOB,
    LIBARCHIVE_UCD_LICENSE_PATH,
    LIBARCHIVE_UCD_README_PATH,
    LIBARCHIVE_UCD_SNAPSHOT_BASE,
    LIBARCHIVE_UCD_VERSION,
    LIBARCHIVE_UNICODE_GENERATOR_PATH,
    LIBARCHIVE_UNICODETOOLS_DATA_BASE,
    LIBARCHIVE_UNICODETOOLS_REVISION,
    LIBARCHIVE_UNICODETOOLS_SOURCE_URL,
    LIB_COLORS_CSSWG_LICENSE_PATH,
    LIB_COLORS_CSSWG_OVERVIEW_PATH,
    LIB_COLORS_CSSWG_REVISION,
    LIB_COLORS_CSSWG_SOURCE_URL,
    LIB_COLORS_DEBIAN_LICENSE_PATH,
    LIB_COLORS_DEBIAN_REVISION,
    LIB_COLORS_DEBIAN_RGB_PATH,
    LIB_COLORS_DEBIAN_SOURCE_URL,
    LIB_COLORS_DEBIAN_TAG,
    LIB_COLORS_DEBIAN_TAG_OBJECT,
    LIB_COLORS_W3C_LICENSE_HTML_PATH,
    LIB_COLORS_W3C_LICENSE_URL,
    LIB_COLORS_W3C_LICENSE_VERSION,
    LIB_COLORS_W3C_NOTICE_PATH,
    LIB_COLORS_XORG_LICENSE_PATH,
    LIB_COLORS_XORG_REVISION,
    LIB_COLORS_XORG_RGB_PATH,
    LIB_COLORS_XORG_SOURCE_URL,
    MATERIAL_ICON_LICENSE_PATH,
    MATERIAL_ICON_README_PATH,
    MATERIAL_ICON_REVISION,
    MATERIAL_ICON_SNAPSHOT_BASE,
    WCWIDTH_UCD_ARCHIVE_SHA256,
    WCWIDTH_UCD_ARCHIVE_SIZE,
    WCWIDTH_UCD_ARCHIVE_URL,
    WCWIDTH_UCD_DATA_FILES,
    WCWIDTH_UCD_DATA_PATHS,
    WCWIDTH_UCD_LICENSE_GIT_BLOB,
    WCWIDTH_UCD_LICENSE_PATH,
    WCWIDTH_UCD_README_PATH,
    WCWIDTH_UCD_SNAPSHOT_BASE,
    WCWIDTH_UCD_VERSION,
    WCWIDTH_UNICODETOOLS_DATA_BASE,
    WCWIDTH_UNICODETOOLS_REVISION,
    WCWIDTH_UNICODETOOLS_SOURCE_URL,
    WCWIDTH_UNICODETOOLS_TAG,
    fail,
    read_regular,
)


ROOT = Path(__file__).resolve().parent.parent
VALIDATOR_PATH = ROOT / "tools/apple-host-delivery-inputs.py"
OUTPUT_RELATIVE = PurePosixPath(
    "third_party/apple-host/APPLE-HOST-NOTICES.txt"
)
WATCH_OUTPUT_RELATIVE = PurePosixPath(
    "third_party/apple-host/WATCH-LIBARCHIVE-NOTICES.txt"
)
FULL_LICENSE_COMPONENTS = {
    "hterm",
    "intl-segmenter",
    "libarchive",
    "libdot",
    "wcwidth",
}
EXPECTED_LIBARCHIVE_SOURCE_COUNT = 128
EXPECTED_LIBARCHIVE_CLOSURE_COUNT = 165
EXPECTED_LEADING_TEXT_COUNT = 97
EXPECTED_NOTICE_TEXT_COUNT = 128
MATERIAL_ICON_IMPORT_COMMIT = "de5387e902ef285c2d2c6909a53d37d826843551"
MATERIAL_ICON_SOURCE_URL = (
    "https://github.com/google/material-design-icons"
)
MATERIAL_ICON_SOURCES = (
    (
        "deps/libapps/hterm/images/close.svg",
        "navigation/svg/production/ic_close_24px.svg",
    ),
    (
        "deps/libapps/hterm/images/keyboard_arrow_down.svg",
        "hardware/svg/production/ic_keyboard_arrow_down_24px.svg",
    ),
    (
        "deps/libapps/hterm/images/keyboard_arrow_up.svg",
        "hardware/svg/production/ic_keyboard_arrow_up_24px.svg",
    ),
)
MATERIAL_ICON_PATHS = tuple(item[0] for item in MATERIAL_ICON_SOURCES)
MATERIAL_ICON_README_LICENSE_HEADING = b"## License\n\n"
MATERIAL_ICON_README_APPLICABILITY = (
    b"We have made these icons available for you to incorporate into your "
    b"products under the [Apache License Version 2.0]"
    b"(https://www.apache.org/licenses/LICENSE-2.0.txt). Feel free to remix "
    b"and re-share these icons and documentation in your products.\n"
)
MATERIAL_ICON_LICENSE_MARKERS = (
    b"Apache License\n",
    b"Version 2.0, January 2004\n",
    b"http://www.apache.org/licenses/\n",
    b"TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION\n",
)
LIB_COLORS_CURRENT_PATH = "deps/libapps/libdot/js/lib_colors.js"
LIB_COLORS_CURRENT_GIT_BLOB = "3716a647e4e7baa95d904a10f56e5bd68c90d1d8"
LIB_COLORS_X11_IMPORT_COMMIT = "f522ce0a191e24a2fc7549962ec10338b1069b3a"
LIB_COLORS_X11_IMPORT_TREE = "6ad91bc53c5a81389664fe24f2d6d081dcfdf653"
LIB_COLORS_X11_IMPORT_DATE = "2012-04-17T17:49:17-07:00"
LIB_COLORS_X11_IMPORT_PATH = "hterm/js/colors.js"
LIB_COLORS_X11_IMPORT_GIT_BLOB = (
    "780c6ae13420fd3d525bcaa0179ef77bbd7d05e9"
)
LIB_COLORS_HSL_IMPORT_COMMIT = "ead51de5339938850e5fc249b0d92482869479e8"
LIB_COLORS_HSL_IMPORT_TREE = "aa59a126df3e61f34b9ef2776a51dfdff380adf0"
LIB_COLORS_HSL_IMPORT_DATE = "2019-10-30T10:21:29+11:00"
LIB_COLORS_HSL_IMPORT_PATH = "libdot/js/lib_colors.js"
LIB_COLORS_HSL_IMPORT_GIT_BLOB = (
    "0a17b6add818c2c2caab5e719d6cc8112b17a155"
)
LIB_COLORS_CSSWG_TREE = "c25eb296a3c508f52b5b89a96486b346771c5ba4"
LIB_COLORS_CSSWG_LICENSE_GIT_BLOB = (
    "0f7c218c64691e29b618b5c1fed5cd9db7651727"
)
LIB_COLORS_CSSWG_OVERVIEW_GIT_BLOB = (
    "e83ae5f51715d3961573758d5e5190efa22eea01"
)
LIB_COLORS_W3C_NOTICE_GIT_BLOB = (
    "60d9c6df3b5e6fdda4b9e90a40fb598835b29f03"
)
LIB_COLORS_XORG_TREE = "2b0b6921c3185bc58b7203fe63f1f5600b3ca3d0"
LIB_COLORS_XORG_LICENSE_GIT_BLOB = (
    "ffaa287aaa08864f00e68f9254756f13b52a3bea"
)
LIB_COLORS_XORG_RGB_GIT_BLOB = "62eb8961ecd193922e17c8e318007f4bdf85383e"
LIB_COLORS_DEBIAN_TREE = "4dd21fe20e7cb24b20cf36828ebe1e4715a49a72"
LIB_COLORS_DEBIAN_LICENSE_GIT_BLOB = (
    "54fc777195d6df457f49a7ae7bbe55540bd07c5d"
)
LIB_COLORS_DEBIAN_RGB_GIT_BLOB = (
    "b9e56c60236bcb02a5e1aac9f7fa2c98f408e977"
)
LIB_COLORS_DEBIAN_EXTENSION_COMMIT = (
    "84aa5f582d5d9cf36e57ea5434b20ec5fac76c59"
)
LIB_COLORS_DEBIAN_EXTENSION_LINE = b"215   7  81\t\tDebianRed\n"
LIB_COLORS_XORG_RAW_RECORD_COUNT = 752
LIB_COLORS_XORG_COLOR_COUNT = 657
LIB_COLORS_DEBIAN_RAW_RECORD_COUNT = 753
LIB_COLORS_COLOR_COUNT = 658
LIB_COLORS_TABLE_SIZE = 23_232
LIB_COLORS_TABLE_SHA256 = (
    "ee247a9e2ce8e254d4bd2d4fa7041a3d8cf86a69a569ef18205014dc3f492ec8"
)
LIB_COLORS_HSL_SOURCE_SIZE = 534
LIB_COLORS_HSL_SOURCE_SHA256 = (
    "acd43b04b073c9b67814922f8d6622c8c274fce5bcb341bd3ef96a34bc93a738"
)
LIB_COLORS_HSL_CURRENT_SIZE = 892
LIB_COLORS_HSL_CURRENT_SHA256 = (
    "cfccdc3437d0c5f3fed3b3cb1f4770ef758e472c6ffe7d52f7e9f835f85ae649"
)
LIB_COLORS_W3C_TR_URL = (
    "https://www.w3.org/TR/2016/WD-css-color-4-20160705/"
)
LIB_COLORS_W3C_TR_SIZE = 411_314
LIB_COLORS_W3C_TR_SHA256 = (
    "10c04c171032d7d8cb58f8e73b9fd7a20003ee9af8330a19b03036cfe2d6f722"
)
LIB_COLORS_W3C_CHANGE_NOTICE = (
    "This software includes material copied from or derived from CSS Color "
    "Module Level 4, "
    f"{LIB_COLORS_W3C_TR_URL}. Copyright © 2016 W3C® "
    "(MIT, ERCIM, Keio, Beihang)."
)
LIB_COLORS_RGB_RECORD = re.compile(
    rb"^[ \t]*([0-9]{1,3})[ \t]+([0-9]{1,3})[ \t]+"
    rb"([0-9]{1,3})[ \t]+([A-Za-z0-9 \t]+?)[ \t]*$"
)
LIB_COLORS_CURRENT_RECORD = re.compile(
    rb"^  '([a-z][a-z0-9]*)': 'rgb\(([0-9]+), ([0-9]+), ([0-9]+)\)',$"
)
LIB_COLORS_HISTORIC_RECORD = re.compile(
    rb'^  "([a-z][a-z0-9]*)": "rgb\(([0-9]+), ([0-9]+), ([0-9]+)\)"[,]?$'
)
LIB_COLORS_W3C_LICENSE_MARKERS = (
    b"W3C Software and Document Notice and License\n",
    b"Permission to copy, modify, and distribute this work, with or without\n",
    b"The full text of this NOTICE in a location viewable to users of\n",
    b"Notice of any changes or modifications, through a copyright\n",
    b'THIS WORK IS PROVIDED "AS IS," AND COPYRIGHT HOLDERS MAKE NO\n',
    b"COPYRIGHT HOLDERS WILL NOT BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL\n",
    b"The name and trademarks of copyright holders may NOT be used in\n",
    b"[1] https://www.w3.org/Consortium/Legal/copyright-software-short-notice\n",
)
LIB_COLORS_XORG_LICENSE_MARKERS = (
    b"Copyright 1985, 1989, 1998  The Open Group\n",
    b"Permission to use, copy, modify, distribute, and sell this software and its\n",
    b'THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS\n',
)
LIB_COLORS_DEBIAN_LICENSE_MARKERS = (
    b"Copyright 2004-2005 Canonical Ltd.\n",
    b"Copyright 1996-2002 Software in the Public Interest, Inc.\n",
    b"Except as contained in this notice, the name of Canoncial Ltd. shall not be\n",
)
WCWIDTH_UCD_EVIDENCE_COMMIT = "6b9f6ee9b9c94cfa4e3adf049c906610d1623ee8"
WCWIDTH_RANGES_GIT_BLOB = "087b50e4bdd49bf053383fc58939786149c8246c"
WCWIDTH_RANGES_SHA256 = (
    "c172743c16062a0fd74b1b176e4a538f557d87be686bcadb53e18b077db86cdf"
)
WCWIDTH_TABLE_NAMES = {
    b"lib.wc.ambiguous",
    b"lib.wc.combining",
    b"lib.wc.unambiguous",
}
WCWIDTH_TABLE_ASSIGNMENT = re.compile(
    rb"^(lib\.wc\.(?:ambiguous|combining|unambiguous)) = .*?^\];\n",
    re.MULTILINE | re.DOTALL,
)
LIBARCHIVE_UCD_README_GIT_BLOB = (
    "89d5cb39ef3a1ee6ec04edda391d90102dbbe890"
)
LIBARCHIVE_UNICODE_GENERATOR_GIT_BLOB = (
    "925de5c85e784c35d2291b86d4d9550042a15a02"
)
LIBARCHIVE_UNICODE_GENERATOR_SHA256 = (
    "a523081eb14b692457b8c9f6c6896030d0d3b8f313fdf37154b70ab00bf107fa"
)
LIBARCHIVE_COMPOSITION_HEADER_PATH = (
    "deps/libarchive/libarchive/archive_string_composition.h"
)
LIBARCHIVE_COMPOSITION_HEADER_GIT_BLOB = (
    "d0ac340961a0a892fb9271ab0f48100515aeb6a9"
)
LIBARCHIVE_COMPOSITION_HEADER_SHA256 = (
    "da5a7bf624dc2316ca9d73537f16d749de65df52bfb211f61ed7052c61cbf46a"
)
LIBARCHIVE_GENERATED_BASELINE_GIT_BLOB = (
    "be41e3365124f630e361bd29726d1df26688e64f"
)
LIBARCHIVE_GENERATED_BASELINE_SHA256 = (
    "1cb44be8d597f87efca8bf6fb33696635ebdf231a2b63a4b420b1c0b5bcec015"
)
LIBARCHIVE_STRING_SOURCE_PATH = (
    "deps/libarchive/libarchive/archive_string.c"
)
LIBARCHIVE_COMPOSITION_RECORD = re.compile(
    rb"\{ 0x([0-9A-F]+) , 0x([0-9A-F]+) , 0x([0-9A-F]+) \},"
)
LIBARCHIVE_EXPLICIT_EXCLUSION_COUNT = 81
LIBARCHIVE_COMPOSITION_RECORD_COUNT = 931
LIBARCHIVE_UAX15_REVISION = 33
LIBARCHIVE_UAX15_VERSION = "Unicode 6.0.0"
LIBARCHIVE_UAX15_DATE = "2010-09-17"
LIBARCHIVE_UAX15_URL = (
    "https://www.unicode.org/reports/tr15/tr15-33.html"
)
LIBARCHIVE_UAX15_SIZE = 143_604
LIBARCHIVE_UAX15_SHA256 = (
    "d7deff6d0a4651bb7249d22cbfc1e5e752eb722bdde3982c8e5c54c9a7d664f4"
)
LIBARCHIVE_HANGUL_CONSTANTS = {
    b"SBASE": b"0xAC00",
    b"LBASE": b"0x1100",
    b"VBASE": b"0x1161",
    b"TBASE": b"0x11A7",
    b"LCOUNT": b"19",
    b"VCOUNT": b"21",
    b"TCOUNT": b"28",
}
UNICODE_LICENSE_BOM = b"\xef\xbb\xbf"
UNICODE_LICENSE_MARKERS = (
    b"UNICODE, INC. LICENSE AGREEMENT - DATA FILES AND SOFTWARE\n",
    b"COPYRIGHT AND PERMISSION NOTICE\n",
    b"Copyright \xc2\xa9 1991-2020 Unicode, Inc. All rights reserved.\n",
    b"a copy of the Unicode data files and any associated documentation\n",
    b"THE DATA FILES AND SOFTWARE ARE PROVIDED \"AS IS\", WITHOUT WARRANTY OF\n",
    b"written authorization of the copyright holder.\n",
)
EXPECTED_UNRESOLVED_INCLUDES = {
    ("deps/libarchive/libarchive/archive.h", "android_lf.h"),
    ("deps/libarchive/libarchive/archive_blake2s_ref.c", "blake2-kat.h"),
    ("deps/libarchive/libarchive/archive_blake2sp_ref.c", "blake2-kat.h"),
    ("deps/libarchive/libarchive/archive_entry.h", "android_lf.h"),
    ("deps/libarchive/libarchive/archive_platform.h", "config.h"),
}
INCLUDE = re.compile(
    rb'^[ \t]*#[ \t]*include[ \t]*"([^"\r\n]+)"',
    re.MULTILINE,
)
CATEGORY_ORDER = {
    "完整许可": 0,
    "hterm 来源与生成证据": 1,
    "libarchive 前导注释": 2,
    "libarchive 中段片段": 3,
}


@dataclass(frozen=True)
class NoticeSource:
    category: str
    component: str
    source: str
    content: bytes


@dataclass(frozen=True)
class NoticeGroup:
    categories: tuple[str, ...]
    components: tuple[str, ...]
    sources: tuple[str, ...]
    content: bytes


def load_validator():
    module_name = "_apple_host_delivery_inputs_for_notices"
    existing = sys.modules.get(module_name)
    if existing is not None:
        return existing
    spec = importlib.util.spec_from_file_location(module_name, VALIDATOR_PATH)
    if spec is None or spec.loader is None:
        fail("无法加载 Apple 宿主交付输入校验器")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def local_include_path(current, include):
    try:
        text = include.decode("utf-8")
    except UnicodeDecodeError:
        fail(f"libarchive 双引号 include 不是 UTF-8：{current}")
    candidate = PurePosixPath(current).parent / PurePosixPath(text)
    if (
        not text
        or PurePosixPath(text).is_absolute()
        or "\\" in text
        or any(part in ("", ".", "..") for part in PurePosixPath(text).parts)
        or not candidate.is_relative_to(PurePosixPath("deps/libarchive"))
    ):
        fail(f"libarchive 双引号 include 路径非法：{current} -> {text}")
    return str(candidate), text


def collect_include_closure(
    root, sources, ensure_tracked, expected_unresolved
):
    pending = sorted(sources, reverse=True)
    closure = set()
    unresolved = set()
    while pending:
        relative = pending.pop()
        if relative in closure:
            continue
        data = read_regular(root, relative, "libarchive 声明来源")
        ensure_tracked(relative)
        closure.add(relative)
        for match in INCLUDE.finditer(data):
            candidate, include = local_include_path(relative, match.group(1))
            path = root / candidate
            try:
                metadata = path.lstat()
            except FileNotFoundError:
                unresolved.add((relative, include))
                continue
            except OSError as error:
                fail(f"无法检查 libarchive 双引号 include：{candidate}（{error}）")
            if stat.S_ISLNK(metadata.st_mode):
                fail(f"libarchive 双引号 include 不能是符号链接：{candidate}")
            if not stat.S_ISREG(metadata.st_mode):
                fail(f"libarchive 双引号 include 必须是常规文件：{candidate}")
            if candidate not in closure:
                pending.append(candidate)
    if unresolved != set(expected_unresolved):
        missing = sorted(set(expected_unresolved) - unresolved)
        unexpected = sorted(unresolved - set(expected_unresolved))
        detail = []
        if missing:
            detail.append(f"不再缺失={missing}")
        if unexpected:
            detail.append(f"新增缺失={unexpected}")
        fail(f"libarchive 双引号 include 未解析集合漂移：{'; '.join(detail)}")
    return closure


def extract_leading_comments(data, source):
    if not data.startswith(b"/*"):
        fail(f"libarchive 声明来源缺少文件开头 C 块注释：{source}")
    position = 0
    last_end = 0
    while data.startswith(b"/*", position):
        end = data.find(b"*/", position + 2)
        if end < 0:
            fail(f"libarchive 文件开头 C 块注释未闭合：{source}")
        last_end = end + 2
        cursor = last_end
        while cursor < len(data) and data[cursor] in b" \t\r\n":
            cursor += 1
        if not data.startswith(b"/*", cursor):
            break
        position = cursor
    # 源码中的首个 LF 属于注释的原始行边界；更多空白不进入正文。
    if data[last_end : last_end + 1] == b"\n":
        last_end += 1
    return data[:last_end]


def extract_fragment(root, fragment):
    data = read_regular(root, fragment.path, "宿主声明片段来源")
    if b"\r" in data:
        fail(f"宿主声明片段来源必须只使用 LF：{fragment.path}")
    lines = data.splitlines(keepends=True)
    if fragment.end_line > len(lines):
        fail(f"宿主声明片段行号超出来源文件：{fragment.path}")
    content = b"".join(lines[fragment.start_line - 1 : fragment.end_line])
    if (
        len(content) != fragment.size
        or hashlib.sha256(content).hexdigest() != fragment.sha256
    ):
        fail(f"宿主声明片段锁与来源字节不一致：{fragment.path}")
    if data.count(content) != 1:
        fail(f"宿主声明片段在来源文件中不是唯一字节序列：{fragment.path}")
    return content


def group_exact_sources(sources):
    grouped = {}
    for source in sources:
        entry = grouped.setdefault(
            source.content,
            {"categories": set(), "components": set(), "sources": set()},
        )
        entry["categories"].add(source.category)
        entry["components"].add(source.component)
        entry["sources"].add(source.source)
    groups = [
        NoticeGroup(
            tuple(sorted(entry["categories"], key=CATEGORY_ORDER.__getitem__)),
            tuple(sorted(entry["components"])),
            tuple(sorted(entry["sources"])),
            content,
        )
        for content, entry in grouped.items()
    ]
    return sorted(
        groups,
        key=lambda group: (
            min(CATEGORY_ORDER[item] for item in group.categories),
            group.sources,
            hashlib.sha256(group.content).hexdigest(),
        ),
    )


def require_closure_coverage(paths, closure, description):
    missing = set(paths) - set(closure)
    if missing:
        fail(f"{description}没有进入交付闭包：{', '.join(sorted(missing))}")


def verify_history_evidence(root, validator):
    repository = root / "deps/libapps"
    checks = (
        (
            MATERIAL_ICON_IMPORT_COMMIT,
            b"Find bar icons were taken from "
            b"github.com/google/material-design-icons/.",
            MATERIAL_ICON_PATHS,
            "Material 图标",
        ),
        (
            WCWIDTH_UCD_EVIDENCE_COMMIT,
            b"update to Unicode 13.0.0 release",
            ("deps/libapps/libdot/third_party/wcwidth/lib_wc.js",),
            "wcwidth Unicode 13.0.0",
        ),
    )
    for commit, message_marker, paths, description in checks:
        base = validator.run_git(
            repository, "merge-base", commit, "HEAD"
        ).decode("ascii", errors="strict").strip()
        if base != commit:
            fail(f"{description}证据提交不是锁定 libapps HEAD 的祖先")
        message = validator.run_git(
            repository, "show", "-s", "--format=%B", commit
        )
        if message_marker not in message:
            fail(f"{description}证据提交说明漂移")
        for relative in paths:
            child = str(
                PurePosixPath(relative).relative_to("deps/libapps")
            )
            historic = validator.run_git(
                repository, "show", f"{commit}:{child}"
            )
            if historic != read_regular(root, relative, f"{description}当前输入"):
                fail(f"{description}证据提交与当前输入字节不一致：{relative}")


def format_material_icon(data, source):
    if (
        b"\n" in data
        or data.count(b"><path") != 1
        or data.count(b"/></svg>") != 1
        or not data.endswith(b"/></svg>")
    ):
        fail(f"Material 上游 SVG 结构漂移：{source}")
    return data.replace(b"><path", b">\n  <path", 1).replace(
        b"/></svg>", b"/>\n</svg>\n", 1
    )


def verify_material_icon_evidence(root, inputs_by_path):
    expected_roles = {
        MATERIAL_ICON_LICENSE_PATH: "license",
        MATERIAL_ICON_README_PATH: "provenance",
    }
    for current, upstream in MATERIAL_ICON_SOURCES:
        expected_roles[current] = "provenance"
        expected_roles[f"{MATERIAL_ICON_SNAPSHOT_BASE}/{upstream}"] = (
            "provenance"
        )
    for relative, role in expected_roles.items():
        item = inputs_by_path.get(relative)
        if (
            item is None
            or item.delivery_unit != "hterm-bundle"
            or item.component != "hterm"
            or item.role != role
        ):
            fail(f"Material 来源或许可输入缺失：{relative}")

    readme = read_regular(
        root, MATERIAL_ICON_README_PATH, "Material 上游 README"
    )
    for marker in (
        MATERIAL_ICON_README_LICENSE_HEADING,
        MATERIAL_ICON_README_APPLICABILITY,
    ):
        if readme.count(marker) != 1:
            fail("Material README 的 Apache-2.0 适用声明漂移")

    license_text = read_regular(
        root, MATERIAL_ICON_LICENSE_PATH, "Material 上游 LICENSE"
    )
    for marker in MATERIAL_ICON_LICENSE_MARKERS:
        if license_text.count(marker) != 1:
            fail("Material LICENSE 的 Apache-2.0 正文标记漂移")

    for current, upstream in MATERIAL_ICON_SOURCES:
        snapshot = f"{MATERIAL_ICON_SNAPSHOT_BASE}/{upstream}"
        formatted = format_material_icon(
            read_regular(root, snapshot, "Material 上游 SVG"),
            snapshot,
        )
        if formatted != read_regular(root, current, "Material 当前 SVG"):
            fail(f"Material SVG 固定格式化关系漂移：{current}")


def git_blob_oid(data):
    header = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


def extract_lib_colors_table(data, assignment, description):
    marker = assignment + b" = {\n"
    if data.count(marker) != 1:
        fail(f"{description}颜色表起始边界漂移")
    start = data.index(marker)
    end = data.find(b"};\n", start + len(marker))
    if end < 0:
        fail(f"{description}颜色表结束边界漂移")
    return data[start : end + 3]


def parse_lib_colors_js_table(block, assignment, record, description):
    if not block.endswith(b"\n") or b"\r" in block:
        fail(f"{description}颜色表必须只使用 LF 且以换行结束")
    lines = block.splitlines()
    if lines[0] != assignment + b" = {" or lines[-1] != b"};":
        fail(f"{description}颜色表赋值边界漂移")
    colors = {}
    for line_number, line in enumerate(lines[1:-1], 1):
        match = record.fullmatch(line)
        if match is None:
            fail(f"{description}颜色表第 {line_number} 项格式漂移")
        name = match.group(1).decode("ascii")
        rgb = tuple(int(value, 10) for value in match.groups()[1:])
        if name in colors:
            fail(f"{description}颜色表含重复键：{name}")
        if any(value > 255 for value in rgb):
            fail(f"{description}颜色表含越界 RGB：{name}")
        colors[name] = rgb
    return colors


def parse_lib_colors_rgb(data, description):
    if not data.endswith(b"\n") or b"\r" in data:
        fail(f"{description}必须只使用 LF 且以换行结束")
    colors = {}
    record_count = 0
    for line_number, line in enumerate(data.splitlines(), 1):
        if not line or line.lstrip().startswith(b"!"):
            continue
        match = LIB_COLORS_RGB_RECORD.fullmatch(line)
        if match is None:
            fail(f"{description}第 {line_number} 行格式漂移")
        rgb = tuple(int(value, 10) for value in match.groups()[:3])
        if any(value > 255 for value in rgb):
            fail(f"{description}第 {line_number} 行 RGB 越界")
        normalized = re.sub(rb"[ \t]+", b"", match.group(4)).lower()
        if re.fullmatch(rb"[a-z][a-z0-9]*", normalized) is None:
            fail(f"{description}第 {line_number} 行颜色名称非法")
        name = normalized.decode("ascii")
        # 与原表生成规则一致：同名后项覆盖前项。
        colors[name] = rgb
        record_count += 1
    return record_count, colors


def render_lib_colors_table(colors):
    output = bytearray(b"lib.colors.colorNames = {\n")
    for name, rgb in sorted(colors.items()):
        output.extend(
            (
                f"  '{name}': 'rgb({rgb[0]}, {rgb[1]}, {rgb[2]})',\n"
            ).encode("ascii")
        )
    output.extend(b"};\n")
    return bytes(output)


def extract_lib_colors_hsl(data):
    start_marker = (
        b"lib.colors.hslxArrayToRgbaArray = function(hslx) {\n"
    )
    end_marker = b"\n\n/**\n * Converts a hsvx array to a hsla array."
    if data.count(start_marker) != 1:
        fail("lib_colors 当前 HSL 函数起始边界漂移")
    start = data.index(start_marker)
    end = data.find(end_marker, start + len(start_marker))
    if end < 0:
        fail("lib_colors 当前 HSL 函数结束边界漂移")
    return data[start : end + 1]


def extract_csswg_hsl_source(data):
    start_marker = b"\tfunction hslToRgb(hue, sat, light) {"
    end_marker = b"\n\t</pre>"
    if data.count(start_marker) != 1:
        fail("CSS Color 4 HSL 算法起始边界漂移")
    start = data.index(start_marker)
    end = data.find(end_marker, start + len(start_marker))
    if end < 0:
        fail("CSS Color 4 HSL 算法结束边界漂移")
    source = html.unescape(
        data[start:end].decode("utf-8", errors="strict")
    ).encode("utf-8")
    if (
        len(source) != LIB_COLORS_HSL_SOURCE_SIZE
        or hashlib.sha256(source).hexdigest()
        != LIB_COLORS_HSL_SOURCE_SHA256
    ):
        fail("CSS Color 4 HSL 算法原始字节漂移")
    return source


def verify_lib_colors_hsl_adaptation(overview, current):
    source = extract_csswg_hsl_source(overview)
    adapted = extract_lib_colors_hsl(current)
    if (
        len(adapted) != LIB_COLORS_HSL_CURRENT_SIZE
        or hashlib.sha256(adapted).hexdigest()
        != LIB_COLORS_HSL_CURRENT_SHA256
    ):
        fail("lib_colors 当前 HSL 改编函数字节漂移")

    source_markers = (
        b"var t2 = light * (sat + 1);",
        b"var t2 = light + sat - (light * sat);",
        b"var t1 = light * 2 - t2;",
        b"var r = hueToRgb(t1, t2, hue + 2);",
        b"var g = hueToRgb(t1, t2, hue);",
        b"var b = hueToRgb(t1, t2, hue - 2);",
        b"if(hue < 0) hue += 6;",
        b"if(hue >= 6) hue -= 6;",
        b"if(hue < 1) return (t2 - t1) * hue + t1;",
        b"else if(hue < 3) return t2;",
        b"else if(hue < 4) return (t2 - t1) * (4 - hue) + t1;",
        b"else return t1;",
    )
    for marker in source_markers:
        if source.count(marker) != 1:
            fail("CSS Color 4 HSL 公式边界漂移")

    adapted_markers = (
        b"const hue = parseInt(hslx[0], 10) / 60;",
        b"const sat = parseInt(hslx[1], 10) / 100;",
        b"const light = parseInt(hslx[2], 10) / 100;",
        b"https://www.w3.org/TR/css-color-4/#hsl-to-rgb",
        (
            b"const t2 = light <= 0.5 ? light * (sat + 1) : "
            b"light + sat - (light * sat);"
        ),
        b"const t1 = light * 2 - t2;",
        b"255 * hueToRgb(t1, t2, hue + 2),",
        b"255 * hueToRgb(t1, t2, hue),",
        b"255 * hueToRgb(t1, t2, hue - 2),",
        b"hslx[3] !== undefined ? +hslx[3] : 1,",
        b"if (hue < 0) {",
        b"if (hue >= 6) {",
        b"return (t2 - t1) * hue + t1;",
        b"} else if (hue < 3) {",
        b"} else if (hue < 4) {",
        b"return (t2 - t1) * (4 - hue) + t1;",
    )
    for marker in adapted_markers:
        if adapted.count(marker) != 1:
            fail("lib_colors HSL 固定改编关系漂移")


def verify_lib_colors_history(
    root, validator, current, current_colors
):
    repository = root / "deps/libapps"
    checks = (
        (
            LIB_COLORS_X11_IMPORT_COMMIT,
            LIB_COLORS_X11_IMPORT_TREE,
            LIB_COLORS_X11_IMPORT_DATE,
            LIB_COLORS_X11_IMPORT_PATH,
            LIB_COLORS_X11_IMPORT_GIT_BLOB,
            b"Added colors.js file to contain color utilities and palettes.",
            "X11 颜色表",
        ),
        (
            LIB_COLORS_HSL_IMPORT_COMMIT,
            LIB_COLORS_HSL_IMPORT_TREE,
            LIB_COLORS_HSL_IMPORT_DATE,
            LIB_COLORS_HSL_IMPORT_PATH,
            LIB_COLORS_HSL_IMPORT_GIT_BLOB,
            b"libdot: support hsla? css strings",
            "W3C HSL 改编",
        ),
    )
    historic_inputs = {}
    for (
        commit,
        tree,
        author_date,
        path,
        blob,
        message_marker,
        description,
    ) in checks:
        base = validator.run_git(
            repository, "merge-base", commit, "HEAD"
        ).decode("ascii", errors="strict").strip()
        if base != commit:
            fail(f"{description}历史提交不是锁定 libapps HEAD 的祖先")
        actual_tree = validator.run_git(
            repository, "show", "-s", "--format=%T", commit
        ).decode("ascii", errors="strict").strip()
        actual_date = validator.run_git(
            repository, "show", "-s", "--format=%aI", commit
        ).decode("ascii", errors="strict").strip()
        message = validator.run_git(
            repository, "show", "-s", "--format=%B", commit
        )
        if (
            actual_tree != tree
            or actual_date != author_date
            or message.count(message_marker) != 1
        ):
            fail(f"{description}历史提交身份漂移")
        historic = validator.run_git(
            repository, "show", f"{commit}:{path}"
        )
        if git_blob_oid(historic) != blob:
            fail(f"{description}历史文件 Git blob 漂移")
        historic_inputs[commit] = historic

    if git_blob_oid(current) != LIB_COLORS_CURRENT_GIT_BLOB:
        fail("lib_colors 当前文件 Git blob 漂移")
    historic_table = extract_lib_colors_table(
        historic_inputs[LIB_COLORS_X11_IMPORT_COMMIT],
        b"hterm.colors.colorNames",
        "libapps 历史",
    )
    historic_colors = parse_lib_colors_js_table(
        historic_table,
        b"hterm.colors.colorNames",
        LIB_COLORS_HISTORIC_RECORD,
        "libapps 历史",
    )
    if historic_colors != current_colors:
        fail("libapps 历史 X11 颜色表与当前交付表语义不一致")

    historic_hsl = historic_inputs[LIB_COLORS_HSL_IMPORT_COMMIT]
    for marker in (
        b"https://www.w3.org/TR/css-color-4/#hsl-to-rgb",
        b"return (t2 - t1) * hue + t1;",
        b"return (t2 - t1) * (4 - hue) + t1;",
        b"255 * hueToRgb(t1, t2, hue + 2),",
        b"255 * hueToRgb(t1, t2, hue - 2),",
    ):
        if historic_hsl.count(marker) != 1:
            fail("libapps 历史 HSL 改编公式漂移")


def verify_lib_colors_licenses(
    csswg_license,
    notice,
    license_html,
    xorg_license,
    debian_license,
):
    if csswg_license.count(
        b"[W3C Software and Document License]"
    ) != 1 or csswg_license.count(
        b"https://www.w3.org/Consortium/Legal/copyright-software"
    ) != 1:
        fail("CSSWG 根 LICENSE 的 W3C 许可指针漂移")

    for marker in LIB_COLORS_W3C_LICENSE_MARKERS:
        if notice.count(marker) != 1:
            fail("W3C Software and Document NOTICE 正文漂移")
    for marker in (
        b"Software and Document license - 2015 version",
        b"This license was in effect between 13 May 2015 and 31 December 2022.",
        b"Permission to copy, modify, and distribute this work, with or without",
        b"The full text of this NOTICE in a location viewable to users",
        b"Notice of any changes or modifications",
        b"COPYRIGHT HOLDERS WILL NOT BE LIABLE FOR ANY DIRECT, INDIRECT, SPECIAL",
        b"The name and trademarks of copyright holders may NOT be used",
        b"https://www.w3.org/copyright/software-license-2015/",
    ):
        if marker not in license_html:
            fail("W3C 2015 官方许可页面标记漂移")

    for marker in LIB_COLORS_XORG_LICENSE_MARKERS:
        if xorg_license.count(marker) != 1:
            fail("X.Org rgb COPYING 正文漂移")
    for marker in LIB_COLORS_DEBIAN_LICENSE_MARKERS:
        if debian_license.count(marker) != 1:
            fail("Debian X.Org copyright 正文漂移")
    for marker in (
        b"Permission is hereby granted, free of charge, to any person obtaining a\n",
        b'THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n',
    ):
        if debian_license.count(marker) != 2:
            fail("Debian X.Org copyright 授权条款漂移")


def verify_lib_colors_rgb_replay(xorg_rgb, debian_rgb, current):
    if (
        debian_rgb.count(LIB_COLORS_DEBIAN_EXTENSION_LINE) != 1
        or debian_rgb.replace(
            LIB_COLORS_DEBIAN_EXTENSION_LINE, b"", 1
        ) != xorg_rgb
    ):
        fail("DebianRed 唯一扩展行与 X.Org rgb.txt 的字节关系漂移")

    xorg_records, xorg_colors = parse_lib_colors_rgb(
        xorg_rgb, "X.Org rgb.txt"
    )
    debian_records, debian_colors = parse_lib_colors_rgb(
        debian_rgb, "Debian rgb.txt"
    )
    if (
        xorg_records != LIB_COLORS_XORG_RAW_RECORD_COUNT
        or len(xorg_colors) != LIB_COLORS_XORG_COLOR_COUNT
        or debian_records != LIB_COLORS_DEBIAN_RAW_RECORD_COUNT
        or len(debian_colors) != LIB_COLORS_COLOR_COUNT
    ):
        fail("lib_colors X11 原始记录或归一化键数量漂移")
    if (
        set(debian_colors) - set(xorg_colors) != {"debianred"}
        or debian_colors["debianred"] != (215, 7, 81)
        or any(debian_colors[name] != rgb for name, rgb in xorg_colors.items())
    ):
        fail("Debian rgb.txt 相对 X.Org 的唯一语义扩展漂移")

    table = extract_lib_colors_table(
        current, b"lib.colors.colorNames", "lib_colors 当前"
    )
    current_colors = parse_lib_colors_js_table(
        table,
        b"lib.colors.colorNames",
        LIB_COLORS_CURRENT_RECORD,
        "lib_colors 当前",
    )
    rendered = render_lib_colors_table(debian_colors)
    if (
        len(table) != LIB_COLORS_TABLE_SIZE
        or hashlib.sha256(table).hexdigest()
        != LIB_COLORS_TABLE_SHA256
        or rendered != table
        or current_colors != debian_colors
    ):
        fail("Debian rgb.txt 不能逐字重建当前 lib_colors 颜色表")
    return current_colors


def verify_lib_colors_evidence(root, inputs_by_path, validator):
    expected_roles = {
        LIB_COLORS_CSSWG_LICENSE_PATH: "provenance",
        LIB_COLORS_CSSWG_OVERVIEW_PATH: "provenance",
        LIB_COLORS_DEBIAN_LICENSE_PATH: "license",
        LIB_COLORS_DEBIAN_RGB_PATH: "provenance",
        LIB_COLORS_W3C_LICENSE_HTML_PATH: "provenance",
        LIB_COLORS_W3C_NOTICE_PATH: "license",
        LIB_COLORS_XORG_LICENSE_PATH: "license",
        LIB_COLORS_XORG_RGB_PATH: "provenance",
    }
    for relative, role in expected_roles.items():
        item = inputs_by_path.get(relative)
        if (
            item is None
            or item.delivery_unit != "hterm-bundle"
            or item.component != "libdot"
            or item.role != role
        ):
            fail(f"lib_colors 来源或许可输入缺失：{relative}")
    current_item = inputs_by_path.get(LIB_COLORS_CURRENT_PATH)
    if (
        current_item is None
        or current_item.delivery_unit != "hterm-bundle"
        or current_item.component != "libdot"
        or current_item.role != "inline-notice"
    ):
        fail("lib_colors 当前交付输入归属漂移")

    blob_expectations = {
        LIB_COLORS_CSSWG_LICENSE_PATH:
            LIB_COLORS_CSSWG_LICENSE_GIT_BLOB,
        LIB_COLORS_CSSWG_OVERVIEW_PATH:
            LIB_COLORS_CSSWG_OVERVIEW_GIT_BLOB,
        LIB_COLORS_DEBIAN_LICENSE_PATH:
            LIB_COLORS_DEBIAN_LICENSE_GIT_BLOB,
        LIB_COLORS_DEBIAN_RGB_PATH:
            LIB_COLORS_DEBIAN_RGB_GIT_BLOB,
        LIB_COLORS_W3C_NOTICE_PATH:
            LIB_COLORS_W3C_NOTICE_GIT_BLOB,
        LIB_COLORS_XORG_LICENSE_PATH:
            LIB_COLORS_XORG_LICENSE_GIT_BLOB,
        LIB_COLORS_XORG_RGB_PATH:
            LIB_COLORS_XORG_RGB_GIT_BLOB,
    }
    evidence = {}
    for relative in expected_roles:
        evidence[relative] = read_regular(
            root, relative, "lib_colors 来源或许可"
        )
    for relative, expected_blob in blob_expectations.items():
        if git_blob_oid(evidence[relative]) != expected_blob:
            fail(f"lib_colors 固定 Git blob 漂移：{relative}")

    verify_lib_colors_licenses(
        evidence[LIB_COLORS_CSSWG_LICENSE_PATH],
        evidence[LIB_COLORS_W3C_NOTICE_PATH],
        evidence[LIB_COLORS_W3C_LICENSE_HTML_PATH],
        evidence[LIB_COLORS_XORG_LICENSE_PATH],
        evidence[LIB_COLORS_DEBIAN_LICENSE_PATH],
    )

    xorg_rgb = evidence[LIB_COLORS_XORG_RGB_PATH]
    debian_rgb = evidence[LIB_COLORS_DEBIAN_RGB_PATH]

    current = read_regular(root, LIB_COLORS_CURRENT_PATH, "lib_colors 当前输入")
    current_colors = verify_lib_colors_rgb_replay(
        xorg_rgb, debian_rgb, current
    )

    overview = evidence[LIB_COLORS_CSSWG_OVERVIEW_PATH]
    verify_lib_colors_hsl_adaptation(overview, current)
    verify_lib_colors_history(root, validator, current, current_colors)


def unicode_license_text(data):
    if not data.startswith(UNICODE_LICENSE_BOM):
        fail("Unicode Data Files 许可缺少锁定的 UTF-8 BOM")
    content = data[len(UNICODE_LICENSE_BOM) :]
    if UNICODE_LICENSE_BOM in content:
        fail("Unicode Data Files 许可含额外 UTF-8 BOM")
    for marker in UNICODE_LICENSE_MARKERS:
        if content.count(marker) != 1:
            fail("Unicode Data Files 许可正文标记漂移")
    return content


def replay_wcwidth_tables(root, validator, data_by_name):
    repository = root / "deps/libapps"
    script = validator.run_git(
        repository,
        "show",
        (
            f"{WCWIDTH_UCD_EVIDENCE_COMMIT}:"
            "libdot/third_party/wcwidth/ranges.py"
        ),
    )
    if (
        hashlib.sha256(script).hexdigest() != WCWIDTH_RANGES_SHA256
        or git_blob_oid(script) != WCWIDTH_RANGES_GIT_BLOB
    ):
        fail("wcwidth 历史生成脚本字节漂移")

    current_path = (
        "deps/libapps/libdot/third_party/wcwidth/lib_wc.js"
    )
    current = read_regular(root, current_path, "wcwidth 当前 JavaScript")
    matches = list(WCWIDTH_TABLE_ASSIGNMENT.finditer(current))
    if (
        len(matches) != len(WCWIDTH_TABLE_NAMES)
        or {match.group(1) for match in matches} != WCWIDTH_TABLE_NAMES
    ):
        fail("wcwidth 当前 JavaScript 三张表边界漂移")

    sentinel = WCWIDTH_TABLE_ASSIGNMENT.sub(
        lambda match: (
            match.group(1)
            + b" = [\n  [0x0000, 0x0000],\n];\n"
        ),
        current,
    )
    if sentinel == current:
        fail("wcwidth 三张表哨兵替换没有生效")

    with tempfile.TemporaryDirectory(
        prefix="ish-wcwidth-ucd13."
    ) as temporary:
        directory = Path(temporary)
        script_path = directory / "ranges.py"
        javascript_path = directory / "lib_wc.js"
        script_path.write_bytes(script)
        javascript_path.write_bytes(sentinel)
        for name, data in data_by_name.items():
            (directory / name).write_bytes(data)
        try:
            result = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(script_path),
                    "--js",
                    str(javascript_path),
                    "update",
                ],
                cwd=directory,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
            )
        except subprocess.TimeoutExpired:
            fail("wcwidth Unicode 13.0.0 离线重放超时")
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).decode(
                "utf-8", errors="replace"
            ).strip()
            fail(f"wcwidth Unicode 13.0.0 离线重放失败：{detail}")
        if javascript_path.read_bytes() != current:
            fail("wcwidth Unicode 13.0.0 输入不能逐字重建当前三张表")


def verify_wcwidth_unicode_evidence(root, inputs_by_path, validator):
    expected_roles = {
        WCWIDTH_UCD_LICENSE_PATH: "license",
        WCWIDTH_UCD_README_PATH: "provenance",
        **{path: "provenance" for path in WCWIDTH_UCD_DATA_PATHS},
    }
    for relative, role in expected_roles.items():
        item = inputs_by_path.get(relative)
        if (
            item is None
            or item.delivery_unit != "hterm-bundle"
            or item.component != "wcwidth"
            or item.role != role
        ):
            fail(f"wcwidth Unicode 来源或许可输入缺失：{relative}")

    readme = read_regular(
        root, WCWIDTH_UCD_README_PATH, "Unicode 13.0.0 final ReadMe"
    )
    for marker in (
        b"# Unicode Character Database\n",
        b"This directory contains the final data files \n",
        b"for the Unicode Character Database, for Version 13.0.0 "
        b"of the Unicode Standard.\n",
        b"# For terms of use, see https://www.unicode.org/terms_of_use.html\n",
    ):
        if readme.count(marker) != 1:
            fail("Unicode 13.0.0 final ReadMe 标记漂移")

    data_by_name = {}
    for name, expected_blob in WCWIDTH_UCD_DATA_FILES:
        relative = f"{WCWIDTH_UCD_SNAPSHOT_BASE}/{name}"
        data = read_regular(root, relative, "Unicode 13.0.0 生成输入")
        if git_blob_oid(data) != expected_blob:
            fail(f"UnicodeTools 固定 Git blob 漂移：{relative}")
        data_by_name[name] = data
    if not data_by_name["PropList.txt"].startswith(
        b"# PropList-13.0.0.txt\n"
    ):
        fail("Unicode 13.0.0 PropList 版本标记漂移")
    if not data_by_name["EastAsianWidth.txt"].startswith(
        b"# EastAsianWidth-13.0.0.txt\n"
    ):
        fail("Unicode 13.0.0 EastAsianWidth 版本标记漂移")
    if not data_by_name["UnicodeData.txt"].startswith(
        b"0000;<control>;Cc;0;BN;;;;;N;NULL;;;;\n"
    ):
        fail("Unicode 13.0.0 UnicodeData 起始记录漂移")

    license_data = read_regular(
        root, WCWIDTH_UCD_LICENSE_PATH, "Unicode Data Files 许可"
    )
    if git_blob_oid(license_data) != WCWIDTH_UCD_LICENSE_GIT_BLOB:
        fail("Unicode Data Files 许可 Git blob 漂移")
    unicode_license_text(license_data)
    replay_wcwidth_tables(root, validator, data_by_name)


def normalize_libarchive_generated_header(data):
    old_guard = (
        b"\n#ifndef __LIBARCHIVE_BUILD\n"
        b"#error This header is only to be used internally to libarchive.\n"
        b"#endif\n"
        b"\n"
        b"#ifndef ARCHIVE_STRING_COMPOSITION_H_INCLUDED\n"
        b"#define ARCHIVE_STRING_COMPOSITION_H_INCLUDED\n"
    )
    new_guard = (
        b"\n#ifndef ARCHIVE_STRING_COMPOSITION_H_INCLUDED\n"
        b"#define ARCHIVE_STRING_COMPOSITION_H_INCLUDED\n"
        b"\n"
        b"#ifndef __LIBARCHIVE_BUILD\n"
        b"#error This header is only to be used internally to libarchive.\n"
        b"#endif\n"
    )
    old_spelling = b"Canonical Cimbining Class"
    new_spelling = b"Canonical Combining Class"
    if data.count(old_guard) != 1:
        fail("libarchive Unicode 生成基线的 header guard 边界漂移")
    if data.count(old_spelling) != 1 or data.count(new_spelling) != 1:
        fail("libarchive Unicode 生成基线的拼写桥接边界漂移")
    return data.replace(old_guard, new_guard, 1).replace(
        old_spelling, new_spelling, 1
    )


def replay_libarchive_composition_header(
    generator, unicode_data, current_header
):
    if (
        git_blob_oid(generator) != LIBARCHIVE_UNICODE_GENERATOR_GIT_BLOB
        or hashlib.sha256(generator).hexdigest()
        != LIBARCHIVE_UNICODE_GENERATOR_SHA256
    ):
        fail("libarchive Unicode 生成脚本字节漂移")
    if (
        git_blob_oid(current_header)
        != LIBARCHIVE_COMPOSITION_HEADER_GIT_BLOB
        or hashlib.sha256(current_header).hexdigest()
        != LIBARCHIVE_COMPOSITION_HEADER_SHA256
    ):
        fail("libarchive Unicode 当前生成头文件字节漂移")

    with tempfile.TemporaryDirectory(
        prefix="ish-libarchive-ucd6."
    ) as temporary:
        directory = Path(temporary)
        script_path = directory / "gen_archive_string_composition_h.sh"
        input_path = directory / "UnicodeData.txt"
        output_path = directory / "archive_string_composition.h"
        script_path.write_bytes(generator)
        input_path.write_bytes(unicode_data)
        environment = os.environ.copy()
        environment.update({"LANG": "C", "LC_ALL": "C"})
        try:
            result = subprocess.run(
                ["sh", str(script_path), str(input_path)],
                cwd=directory,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=60,
                env=environment,
            )
        except subprocess.TimeoutExpired:
            fail("libarchive Unicode 6.0.0 离线重放超时")
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).decode(
                "utf-8", errors="replace"
            ).strip()
            fail(f"libarchive Unicode 6.0.0 离线重放失败：{detail}")
        try:
            generated = output_path.read_bytes()
        except OSError as error:
            fail(f"libarchive Unicode 生成器没有产生头文件：{error}")

    if (
        git_blob_oid(generated) != LIBARCHIVE_GENERATED_BASELINE_GIT_BLOB
        or hashlib.sha256(generated).hexdigest()
        != LIBARCHIVE_GENERATED_BASELINE_SHA256
    ):
        fail("libarchive Unicode 6.0.0 生成基线字节漂移")
    if normalize_libarchive_generated_header(generated) != current_header:
        fail("libarchive Unicode 生成基线经两处固定文本变换后不能恢复当前头文件")


def parse_libarchive_unicode_data(data):
    if not data.endswith(b"\n") or b"\r" in data:
        fail("libarchive UnicodeData 必须只使用 LF 且以换行结束")
    combining_classes = {}
    decompositions = []
    for line_number, line in enumerate(data.splitlines(), 1):
        fields = line.split(b";")
        if len(fields) != 15:
            fail(f"libarchive UnicodeData 第 {line_number} 行字段数量漂移")
        try:
            codepoint = int(fields[0], 16)
            combining_class = int(fields[3], 10)
        except ValueError:
            fail(f"libarchive UnicodeData 第 {line_number} 行数字字段非法")
        if codepoint in combining_classes:
            fail(f"libarchive UnicodeData 重复码点：U+{codepoint:04X}")
        combining_classes[codepoint] = combining_class
        decompositions.append((codepoint, fields[5]))
    return combining_classes, decompositions


def parse_libarchive_composition_exclusions(data):
    if not data.endswith(b"\n") or b"\r" in data:
        fail("libarchive CompositionExclusions 必须只使用 LF 且以换行结束")
    exclusions = set()
    for line_number, line in enumerate(data.splitlines(), 1):
        value = line.split(b"#", 1)[0].strip()
        if not value:
            continue
        if re.fullmatch(rb"[0-9A-F]{4,6}", value) is None:
            fail(
                "libarchive CompositionExclusions "
                f"第 {line_number} 行码点非法"
            )
        codepoint = int(value, 16)
        if codepoint in exclusions:
            fail(
                "libarchive CompositionExclusions "
                f"重复码点：U+{codepoint:04X}"
            )
        exclusions.add(codepoint)
    if len(exclusions) != LIBARCHIVE_EXPLICIT_EXCLUSION_COUNT:
        fail("libarchive CompositionExclusions 显式排除项数量漂移")
    return exclusions


def extract_libarchive_table(header, declaration, description):
    marker = declaration + b" {\n"
    if header.count(marker) != 1:
        fail(f"libarchive {description}声明边界漂移")
    block = header.split(marker, 1)[1].split(b"};\n", 1)[0]
    records = [
        tuple(int(value, 16) for value in match)
        for match in LIBARCHIVE_COMPOSITION_RECORD.findall(block)
    ]
    if len(records) != LIBARCHIVE_COMPOSITION_RECORD_COUNT:
        fail(f"libarchive {description}记录数量漂移")
    return records


def verify_libarchive_composition_semantics(
    unicode_data, composition_exclusions, header
):
    combining_classes, decompositions = parse_libarchive_unicode_data(
        unicode_data
    )
    exclusions = parse_libarchive_composition_exclusions(
        composition_exclusions
    )
    expected = []
    for codepoint, mapping in decompositions:
        parts = mapping.split()
        if len(parts) != 2 or parts[0].startswith(b"<"):
            continue
        first, second = (int(part, 16) for part in parts)
        if first not in combining_classes:
            fail(
                "libarchive UnicodeData 分解首码点缺少组合类别："
                f"U+{first:04X}"
            )
        # 单码点分解不会进入二元表；非 starter 分解由首码点 CCC 排除。
        if codepoint in exclusions or combining_classes[first] != 0:
            continue
        expected.append((first, second, codepoint))
    expected.sort()
    if len(expected) != LIBARCHIVE_COMPOSITION_RECORD_COUNT:
        fail("Unicode 6.0.0 输入推导出的 libarchive 组合记录数量漂移")

    composition = extract_libarchive_table(
        header,
        (
            b"static const struct unicode_composition_table "
            b"u_composition_table[] ="
        ),
        "正向组合表",
    )
    if composition != expected:
        fail("Unicode 6.0.0 输入不能逐项恢复 libarchive 正向组合表")

    decomposition = extract_libarchive_table(
        header,
        (
            b"static const struct unicode_decomposition_table "
            b"u_decomposition_table[] ="
        ),
        "反向分解表",
    )
    reverse = sorted((first, second, nfc) for nfc, first, second in decomposition)
    if reverse != expected:
        fail("Unicode 6.0.0 输入不能逐项恢复 libarchive 反向分解表")


def verify_libarchive_hangul_constants(source):
    for name, value in LIBARCHIVE_HANGUL_CONSTANTS.items():
        pattern = (
            rb"^#define HC_" + name + rb"[ \t]+" + re.escape(value) + rb"$"
        )
        if len(re.findall(pattern, source, re.MULTILINE)) != 1:
            fail(f"libarchive UAX #15 Hangul 常量漂移：HC_{name.decode()}")
    for line in (
        b"#define HC_NCOUNT\t(HC_VCOUNT * HC_TCOUNT)\n",
        b"#define HC_SCOUNT\t(HC_LCOUNT * HC_NCOUNT)\n",
    ):
        if source.count(line) != 1:
            fail("libarchive UAX #15 Hangul 派生常量漂移")


def verify_libarchive_unicode_evidence(
    root, inputs_by_path, input_keys
):
    expected_roles = {
        LIBARCHIVE_UCD_README_PATH: "provenance",
        LIBARCHIVE_UNICODE_GENERATOR_PATH: "provenance",
        **{path: "provenance" for path in LIBARCHIVE_UCD_DATA_PATHS},
    }
    for relative, role in expected_roles.items():
        item = inputs_by_path.get(relative)
        if (
            item is None
            or item.delivery_unit != "libarchive"
            or item.component != "libarchive"
            or item.role != role
        ):
            fail(f"libarchive Unicode 来源输入缺失：{relative}")
    license_key = (
        "libarchive",
        "libarchive",
        "license",
        LIBARCHIVE_UCD_LICENSE_PATH,
    )
    if license_key not in input_keys:
        fail("libarchive Unicode Data Files 许可输入缺失")

    readme = read_regular(
        root, LIBARCHIVE_UCD_README_PATH, "Unicode 6.0.0 final ReadMe"
    )
    for marker in (
        b"# Date: 2010-10-05, 16:26:38 PDT [KW]\n",
        b"# Unicode Character Database\n",
        b"# Copyright (c) 1991-2010 Unicode, Inc.\n",
        b"# For terms of use, see http://www.unicode.org/terms_of_use.html\n",
        b"for the Unicode Character Database (UCD) for Unicode 6.0.0.\n",
    ):
        if readme.count(marker) != 1:
            fail("Unicode 6.0.0 final ReadMe 标记漂移")
    if git_blob_oid(readme) != LIBARCHIVE_UCD_README_GIT_BLOB:
        fail("Unicode 6.0.0 final ReadMe Git blob 漂移")

    data_by_name = {}
    for name, expected_blob in LIBARCHIVE_UCD_DATA_FILES:
        relative = f"{LIBARCHIVE_UCD_SNAPSHOT_BASE}/{name}"
        data = read_regular(root, relative, "Unicode 6.0.0 生成来源")
        if git_blob_oid(data) != expected_blob:
            fail(f"UnicodeTools 固定 Git blob 漂移：{relative}")
        data_by_name[name] = data
    if not data_by_name["CompositionExclusions.txt"].startswith(
        b"# CompositionExclusions-6.0.0.txt\n"
    ):
        fail("Unicode 6.0.0 CompositionExclusions 版本标记漂移")
    if not data_by_name["UnicodeData.txt"].startswith(
        b"0000;<control>;Cc;0;BN;;;;;N;NULL;;;;\n"
    ):
        fail("Unicode 6.0.0 UnicodeData 起始记录漂移")

    license_data = read_regular(
        root, LIBARCHIVE_UCD_LICENSE_PATH, "Unicode Data Files 许可"
    )
    if git_blob_oid(license_data) != LIBARCHIVE_UCD_LICENSE_GIT_BLOB:
        fail("Unicode Data Files 许可 Git blob 漂移")
    unicode_license_text(license_data)

    generator = read_regular(
        root, LIBARCHIVE_UNICODE_GENERATOR_PATH, "libarchive Unicode 生成器"
    )
    header = read_regular(
        root, LIBARCHIVE_COMPOSITION_HEADER_PATH, "libarchive Unicode 当前头文件"
    )
    replay_libarchive_composition_header(
        generator, data_by_name["UnicodeData.txt"], header
    )
    verify_libarchive_composition_semantics(
        data_by_name["UnicodeData.txt"],
        data_by_name["CompositionExclusions.txt"],
        header,
    )
    verify_libarchive_hangul_constants(
        read_regular(
            root, LIBARCHIVE_STRING_SOURCE_PATH, "libarchive Unicode 运行时源码"
        )
    )


def overview():
    return (
        "===== BEGIN APPLE HOST NOTICE: overview =====\n"
        "Apple 宿主第三方声明\n"
        "\n"
        "覆盖范围：本文件汇集普通 iSH 与可选 iSH+Linux 共同使用的 "
        "hterm 生成资源及 libarchive 静态链接输入的锁定原始文本，并在"
        "后续专节记录已闭合的来源证据与仍由发行门禁处理的边界。\n"
        "\n"
        "交付与重审边界：\n"
        "- Linux kernel、在线 rootfs 及其许可与对应源码不在本文件内；"
        "本文件不能代表 iSH+Linux 的完整发行义务已经闭合。\n"
        "- Alpine guest seed、项目自身许可、LGPL 与对应源码交付继续由"
        "各自的发行门禁处理。\n"
        "- libarchive 的 BLAKE2 编译对象当前只进入未单独交付的 "
        "libarchive.a 中间产物；普通 iSH 与 iSH+Linux 的最终 Mach-O "
        "不拉入对应对象；Watch 使用独立 libarchive-watchOS.a，其 tar "
        "读取、gzip 读写与 pax 写入入口产生的完整传递对象闭包由 LinkMap "
        "固定，并使用独立的 WATCH-LIBARCHIVE-NOTICES.txt。未来启用 "
        "RAR5/format-all、改变强制加载或单独交付静态库时，必须重新评估"
        "并明确许可分支。\n"
        "- 三个已交付 Material 图标的来源、格式化关系与适用许可证据见"
        "后续专节；wcwidth Unicode 13.0.0 的数据、重放关系与许可也已"
        "闭合；libarchive Unicode 6.0.0 的产品表、生成关系、规范版本与"
        "许可路径也已闭合；lib_colors 的 W3C HSL 算法、X11 颜色表、"
        "改编关系与适用许可证据同样已闭合。\n"
        "- 来源与许可边界是工程审计记录，不是法律结论。\n"
        "- public-domain 字样仅转述锁定上游源码，不是本工具作出的法律"
        "判断；libarchive/COPYING 也只是上游汇总，具体源码文本仍有控制力。\n"
        "===== END APPLE HOST NOTICE: overview =====\n"
        "\n"
    ).encode("utf-8")


def material_provenance(inputs_by_path):
    icon_lines = []
    for current, upstream in MATERIAL_ICON_SOURCES:
        current_item = inputs_by_path[current]
        snapshot = f"{MATERIAL_ICON_SNAPSHOT_BASE}/{upstream}"
        upstream_item = inputs_by_path[snapshot]
        icon_lines.append(
            f"- 当前 {current}；{current_item.size} 字节；SHA-256 "
            f"{current_item.sha256}\n"
            f"  上游 {upstream}；{upstream_item.size} 字节；SHA-256 "
            f"{upstream_item.sha256}"
        )

    return (
        "===== BEGIN APPLE HOST NOTICE: material-provenance =====\n"
        "已闭合的 Material 图标来源与许可\n"
        "\n"
        "libapps 历史提交 "
        f"{MATERIAL_ICON_IMPORT_COMMIT} 只记录三个 hterm find bar SVG "
        "取自 google/material-design-icons，没有记录精确上游 revision。"
        "本锁选取作者时间点的官方 master tip "
        f"{MATERIAL_ICON_REVISION} 作为同时期不可变快照；这不表示导入者"
        "明确记录或选择了该 revision。\n"
        "\n"
        f"权威仓库：{MATERIAL_ICON_SOURCE_URL}\n"
        + "\n".join(icon_lines)
        + "\n\n"
        "三份当前 SVG 都由对应上游单行原字节执行同一确定性格式化得到："
        "在 svg 与 path 元素之间插入 LF 和两个空格，在 path 与结束 svg "
        "之间插入 LF，并补文件尾 LF；图形数据没有改写。校验器会离线逐字"
        "重放该关系。\n"
        "\n"
        f"同一快照的 {MATERIAL_ICON_README_PATH} 第 37–40 行明确说明这些 icons "
        "在 Apache License Version 2.0 下提供；根 LICENSE 原字节也已固定"
        "并作为完整许可收入本文件。该证据只闭合上述三个已交付 SVG，"
        "不表示其他宿主来源或整个发行已经闭合。\n"
        "===== END APPLE HOST NOTICE: material-provenance =====\n"
        "\n"
    ).encode("utf-8")


def wcwidth_unicode_provenance(inputs_by_path):
    data_lines = []
    for name, blob in WCWIDTH_UCD_DATA_FILES:
        relative = f"{WCWIDTH_UCD_SNAPSHOT_BASE}/{name}"
        item = inputs_by_path[relative]
        upstream = f"{WCWIDTH_UNICODETOOLS_DATA_BASE}/{name}"
        data_lines.append(
            f"- {relative}；{item.size} 字节；SHA-256 {item.sha256}\n"
            f"  官方 Git 路径 {upstream}；blob {blob}"
        )
    readme = inputs_by_path[WCWIDTH_UCD_README_PATH]
    license_item = inputs_by_path[WCWIDTH_UCD_LICENSE_PATH]

    return (
        "===== BEGIN APPLE HOST NOTICE: wcwidth-unicode-provenance =====\n"
        "已闭合的 wcwidth Unicode 13.0.0 生成来源与许可\n"
        "\n"
        "libapps 历史提交 "
        f"{WCWIDTH_UCD_EVIDENCE_COMMIT} 明确说明当前三张 wcwidth 表由 "
        "Unicode 13.0.0 重新生成，且该提交的 lib_wc.js 与当前交付字节"
        "一致。本工程选择 Unicode 官方 unicodetools tag "
        f"{WCWIDTH_UNICODETOOLS_TAG} 对应的固定提交 "
        f"{WCWIDTH_UNICODETOOLS_REVISION} 作为不可变输入证据；这不表示 "
        "libapps 作者明确记录或使用了该 Git 提交。\n"
        "\n"
        f"权威仓库：{WCWIDTH_UNICODETOOLS_SOURCE_URL}\n"
        + "\n".join(data_lines)
        + "\n\n"
        "上述三个官方 Git blob 与 Unicode 13.0.0 发布归档中的同名成员"
        "逐字节一致。发布归档锁为：\n"
        f"- {WCWIDTH_UCD_ARCHIVE_URL}\n"
        f"- {WCWIDTH_UCD_ARCHIVE_SIZE} 字节；SHA-256 "
        f"{WCWIDTH_UCD_ARCHIVE_SHA256}\n"
        f"- final ReadMe：{WCWIDTH_UCD_README_PATH}；{readme.size} 字节；"
        f"SHA-256 {readme.sha256}\n"
        "\n"
        "校验器在临时目录中使用历史提交里的 ranges.py，先把当前三张表"
        "替换为哨兵，再以三份锁定数据离线执行 update；结果必须逐字恢复"
        "当前 lib_wc.js。历史生成脚本固定为 Git blob "
        f"{WCWIDTH_RANGES_GIT_BLOB}、SHA-256 {WCWIDTH_RANGES_SHA256}。"
        "运行时表和子模块字节没有改写。\n"
        "\n"
        f"适用许可来自同一 unicodetools 固定提交的根 LICENSE："
        f"{WCWIDTH_UCD_LICENSE_PATH}；{license_item.size} 字节；SHA-256 "
        f"{license_item.sha256}。原文件的 UTF-8 BOM 只在聚合显示时移除，"
        "其余许可正文逐字收入后续完整许可 section。UCD.zip 本身不含 "
        "LICENSE；final ReadMe 只用于确认最终 Unicode 13.0.0 发布身份。\n"
        "===== END APPLE HOST NOTICE: wcwidth-unicode-provenance =====\n"
        "\n"
    ).encode("utf-8")


def libarchive_unicode_provenance(inputs_by_path):
    data_lines = []
    for name, blob in LIBARCHIVE_UCD_DATA_FILES:
        relative = f"{LIBARCHIVE_UCD_SNAPSHOT_BASE}/{name}"
        item = inputs_by_path[relative]
        upstream = f"{LIBARCHIVE_UNICODETOOLS_DATA_BASE}/{name}"
        data_lines.append(
            f"- {relative}；{item.size} 字节；SHA-256 {item.sha256}\n"
            f"  官方 Git 路径 {upstream}；blob {blob}"
        )
    readme = inputs_by_path[LIBARCHIVE_UCD_README_PATH]
    generator = inputs_by_path[LIBARCHIVE_UNICODE_GENERATOR_PATH]
    license_item = inputs_by_path[LIBARCHIVE_UCD_LICENSE_PATH]

    return (
        "===== BEGIN APPLE HOST NOTICE: libarchive-unicode-provenance =====\n"
        "已闭合的 libarchive Unicode 6.0.0 规范化表来源与许可\n"
        "\n"
        "锁定的 archive_string_composition.h 明确标记由 Unicode 6.0.0 "
        "UnicodeData.txt 生成；当前生成脚本与头文件均来自 libarchive "
        "3.4.3 gitlink。本工程选择 Unicode 官方 unicodetools 固定提交 "
        f"{LIBARCHIVE_UNICODETOOLS_REVISION} 中逐字匹配的数据作为不可变"
        "输入证据；这不表示 libarchive 作者明确记录或使用了该 Git 提交。\n"
        "\n"
        f"权威仓库：{LIBARCHIVE_UNICODETOOLS_SOURCE_URL}\n"
        + "\n".join(data_lines)
        + "\n\n"
        "上述两个官方 Git blob 与 Unicode 6.0.0 发布归档中的同名成员"
        "逐字节一致。发布归档锁为：\n"
        f"- {LIBARCHIVE_UCD_ARCHIVE_URL}\n"
        f"- {LIBARCHIVE_UCD_ARCHIVE_SIZE} 字节；SHA-256 "
        f"{LIBARCHIVE_UCD_ARCHIVE_SHA256}\n"
        f"- final ReadMe：{LIBARCHIVE_UCD_README_PATH}；{readme.size} 字节；"
        f"SHA-256 {readme.sha256}；官方 Git blob "
        f"{LIBARCHIVE_UCD_README_GIT_BLOB}\n"
        "\n"
        f"生成脚本：{LIBARCHIVE_UNICODE_GENERATOR_PATH}；{generator.size} "
        f"字节；SHA-256 {generator.sha256}；Git blob "
        f"{LIBARCHIVE_UNICODE_GENERATOR_GIT_BLOB}。校验器在隔离临时目录、"
        "C locale 下以锁定 UnicodeData 离线运行脚本，先得到 Git blob "
        f"{LIBARCHIVE_GENERATED_BASELINE_GIT_BLOB}，再且仅再执行拼写修正"
        "与 header guard 前移两处固定文本变换；结果必须逐字恢复当前头文件 "
        f"Git blob {LIBARCHIVE_COMPOSITION_HEADER_GIT_BLOB}。"
        "CompositionExclusions 的 81 个显式项与 UnicodeData 中单码点、"
        "非 starter 分解规则还会独立推导并逐项核对正反两张 931 项表。\n"
        "\n"
        "本工程记录的许可依据来自同一 Unicode 官方固定 Git 树的根 "
        f"LICENSE："
        f"{LIBARCHIVE_UCD_LICENSE_PATH}；{license_item.size} 字节；SHA-256 "
        f"{license_item.sha256}。该许可与 wcwidth UCD 证据共享同一原始"
        "文件，聚合正文按精确字节只收入一次。Unicode 6.0.0 UCD.zip 本身"
        "不含 LICENSE，final ReadMe 与 CompositionExclusions 只链接当时"
        "的可变 Terms of Use；固定 Git 树同时包含逐字相同的数据和根 "
        "LICENSE，因此这里只记录本工程采用的许可证据路径，不冒充 2010 "
        "归档内许可原件，也不声称 Unicode 在该提交中专门重新许可旧数据。\n"
        "\n"
        f"archive_string.c 的 Hangul 常量对应 {LIBARCHIVE_UAX15_VERSION}、"
        f"UAX #15 Revision {LIBARCHIVE_UAX15_REVISION}（"
        f"{LIBARCHIVE_UAX15_DATE}）：{LIBARCHIVE_UAX15_URL}；"
        f"{LIBARCHIVE_UAX15_SIZE} 字节；SHA-256 "
        f"{LIBARCHIVE_UAX15_SHA256}。上游只记录未版本化链接，因此该版本"
        "是本工程按 Unicode 6.0.0 选择的规范证据，不表示作者明确选择了 "
        "UAX #15 Revision 33；技术报告全文不作为数据输入复制进仓库或产品。\n"
        "\n"
        "本节只闭合 Apple 产品使用的 libarchive 规范化运行时与生成表。"
        "子模块中的 libarchive 测试源码、压缩夹具及其 NormalizationTest "
        "来源未进入 Apple App 的 128 个编译源、由其形成的 165 文件"
        "编译/include 闭包或产品资源闭包，也不在本节范围。\n"
        "===== END APPLE HOST NOTICE: libarchive-unicode-provenance =====\n"
        "\n"
    ).encode("utf-8")


def lib_colors_provenance(inputs_by_path):
    current = inputs_by_path[LIB_COLORS_CURRENT_PATH]
    csswg_license = inputs_by_path[LIB_COLORS_CSSWG_LICENSE_PATH]
    csswg_overview = inputs_by_path[LIB_COLORS_CSSWG_OVERVIEW_PATH]
    w3c_html = inputs_by_path[LIB_COLORS_W3C_LICENSE_HTML_PATH]
    w3c_notice = inputs_by_path[LIB_COLORS_W3C_NOTICE_PATH]
    xorg_rgb = inputs_by_path[LIB_COLORS_XORG_RGB_PATH]
    xorg_license = inputs_by_path[LIB_COLORS_XORG_LICENSE_PATH]
    debian_rgb = inputs_by_path[LIB_COLORS_DEBIAN_RGB_PATH]
    debian_license = inputs_by_path[LIB_COLORS_DEBIAN_LICENSE_PATH]

    return (
        "===== BEGIN APPLE HOST NOTICE: lib-colors-provenance =====\n"
        "已闭合的 lib_colors W3C HSL 与 X11 颜色表来源及许可\n"
        "\n"
        f"当前交付文件：{LIB_COLORS_CURRENT_PATH}；{current.size} 字节；"
        f"SHA-256 {current.sha256}；Git blob "
        f"{LIB_COLORS_CURRENT_GIT_BLOB}。运行时代码与子模块字节没有改写；"
        "八份原始输入不作为独立资源进入 App bundle，三份适用许可正文"
        "仅在本聚合声明中逐字收入。\n"
        "\n"
        "X11 颜色表历史与重放：\n"
        f"- libapps 引入提交 {LIB_COLORS_X11_IMPORT_COMMIT}；tree "
        f"{LIB_COLORS_X11_IMPORT_TREE}；作者时间 "
        f"{LIB_COLORS_X11_IMPORT_DATE}；历史文件 blob "
        f"{LIB_COLORS_X11_IMPORT_GIT_BLOB}。\n"
        f"- X.Org 官方仓库 {LIB_COLORS_XORG_SOURCE_URL}；固定提交 "
        f"{LIB_COLORS_XORG_REVISION}；tree {LIB_COLORS_XORG_TREE}。\n"
        f"- {LIB_COLORS_XORG_RGB_PATH}；{xorg_rgb.size} 字节；SHA-256 "
        f"{xorg_rgb.sha256}；blob {LIB_COLORS_XORG_RGB_GIT_BLOB}。\n"
        f"- {LIB_COLORS_XORG_LICENSE_PATH}；{xorg_license.size} 字节；"
        f"SHA-256 {xorg_license.sha256}；blob "
        f"{LIB_COLORS_XORG_LICENSE_GIT_BLOB}。\n"
        f"- Debian 官方仓库 {LIB_COLORS_DEBIAN_SOURCE_URL}；签名 tag "
        f"{LIB_COLORS_DEBIAN_TAG}；tag object "
        f"{LIB_COLORS_DEBIAN_TAG_OBJECT}；固定提交 "
        f"{LIB_COLORS_DEBIAN_REVISION}；tree {LIB_COLORS_DEBIAN_TREE}。\n"
        f"- {LIB_COLORS_DEBIAN_RGB_PATH}；{debian_rgb.size} 字节；"
        f"SHA-256 {debian_rgb.sha256}；blob "
        f"{LIB_COLORS_DEBIAN_RGB_GIT_BLOB}。\n"
        f"- {LIB_COLORS_DEBIAN_LICENSE_PATH}；{debian_license.size} 字节；"
        f"SHA-256 {debian_license.sha256}；blob "
        f"{LIB_COLORS_DEBIAN_LICENSE_GIT_BLOB}。\n"
        "\n"
        "libapps 没有记录精确 X.Org revision 或 Debian 发行包。本工程"
        "选择上述两份快照，是因为 Debian 表能够精确重放当前对象，且"
        "删除 DebianRed 后逐字等于该 X.Org 表；这不表示 libapps 作者"
        "明确选择了这两个 revision。\n"
        "\n"
        "Debian 原表删除唯一 23 字节 DebianRed 扩展行后必须逐字恢复 "
        "X.Org 原表；该扩展的历史提交为 "
        f"{LIB_COLORS_DEBIAN_EXTENSION_COMMIT}。X.Org 的 "
        f"{LIB_COLORS_XORG_RAW_RECORD_COUNT} 条颜色记录归一化为 "
        f"{LIB_COLORS_XORG_COLOR_COUNT} 个键；Debian 的 "
        f"{LIB_COLORS_DEBIAN_RAW_RECORD_COUNT} 条颜色记录归一化为 "
        f"{LIB_COLORS_COLOR_COUNT} 个键。校验器按名称小写、移除名称空白、"
        "同名后项覆盖、键排序及固定 JavaScript 格式离线重放，必须逐字"
        f"恢复 {LIB_COLORS_TABLE_SIZE} 字节、SHA-256 "
        f"{LIB_COLORS_TABLE_SHA256} 的当前对象块；2012 历史对象的 "
        "658 个键和值也必须与当前表一致。\n"
        "\n"
        "W3C HSL 算法历史与改编核对：\n"
        f"- libapps 引入提交 {LIB_COLORS_HSL_IMPORT_COMMIT}；tree "
        f"{LIB_COLORS_HSL_IMPORT_TREE}；作者时间 "
        f"{LIB_COLORS_HSL_IMPORT_DATE}；历史文件 blob "
        f"{LIB_COLORS_HSL_IMPORT_GIT_BLOB}。\n"
        f"- CSSWG 官方仓库 {LIB_COLORS_CSSWG_SOURCE_URL}；本工程以该"
        "作者时间为截止点选取固定提交 "
        f"{LIB_COLORS_CSSWG_REVISION}；tree {LIB_COLORS_CSSWG_TREE}。"
        "这不表示 libapps 作者明确记录或使用了该 Git 提交。\n"
        f"- {LIB_COLORS_CSSWG_OVERVIEW_PATH}；{csswg_overview.size} 字节；"
        f"SHA-256 {csswg_overview.sha256}；blob "
        f"{LIB_COLORS_CSSWG_OVERVIEW_GIT_BLOB}。\n"
        f"- {LIB_COLORS_CSSWG_LICENSE_PATH}；{csswg_license.size} 字节；"
        f"SHA-256 {csswg_license.sha256}；blob "
        f"{LIB_COLORS_CSSWG_LICENSE_GIT_BLOB}。\n"
        "\n"
        f"从 CSS Color 4 固定源提取并解码的算法为 "
        f"{LIB_COLORS_HSL_SOURCE_SIZE} 字节、SHA-256 "
        f"{LIB_COLORS_HSL_SOURCE_SHA256}；当前改编函数为 "
        f"{LIB_COLORS_HSL_CURRENT_SIZE} 字节、SHA-256 "
        f"{LIB_COLORS_HSL_CURRENT_SHA256}。固定关系保留原始分支与公式，"
        "并将 hue 从角度归一化为六段、将饱和度和亮度百分比归一化为 "
        "0–1、把 RGB 缩放到 0–255、返回 alpha，再按项目风格改写语法。"
        "校验器会锁定两端字节并逐项核对这些公式与改动。\n"
        "\n"
        f"不可变公开工作草案交叉证据：{LIB_COLORS_W3C_TR_URL}；当前取得"
        f"页面为 {LIB_COLORS_W3C_TR_SIZE} 字节、SHA-256 "
        f"{LIB_COLORS_W3C_TR_SHA256}；该页面不复制进仓库或产品。\n"
        "\n"
        "CSSWG 根 LICENSE 指向 W3C Software and Document License。"
        f"{LIB_COLORS_W3C_LICENSE_HTML_PATH} 是 2026-07-27 从版本化官方"
        f"入口 {LIB_COLORS_W3C_LICENSE_URL} 取得的 2015 版页面；"
        f"{w3c_html.size} 字节；SHA-256 {w3c_html.sha256}。它用于锁定"
        "当前官方页面和条款标记，不能冒充 2019 年网页原件。完整、可见的"
        f"许可正文来自 {LIB_COLORS_W3C_NOTICE_PATH}；{w3c_notice.size} "
        f"字节；SHA-256 {w3c_notice.sha256}；Git blob "
        f"{LIB_COLORS_W3C_NOTICE_GIT_BLOB}，并在后续完整许可 section "
        "逐字收入。X.Org COPYING 与 Debian copyright 也分别逐字收入。\n"
        "\n"
        f"{LIB_COLORS_W3C_CHANGE_NOTICE}\n"
        "\n"
        "Changes and modifications: hue, saturation, and lightness inputs are "
        "normalized for the libdot API; RGB channels are scaled to 0–255; an "
        "alpha channel is returned; syntax and formatting follow the project "
        "style.\n"
        "\n"
        "本节记录可重复的工程来源、改编与许可证据，不替代发行者的法律"
        "审查，也不表示其他发行边界已经闭合。\n"
        "===== END APPLE HOST NOTICE: lib-colors-provenance =====\n"
        "\n"
    ).encode("utf-8")


def audit_boundaries(inputs_by_path):
    return (
        material_provenance(inputs_by_path)
        + wcwidth_unicode_provenance(inputs_by_path)
        + libarchive_unicode_provenance(inputs_by_path)
        + lib_colors_provenance(inputs_by_path)
    )


def render_groups(groups, boundaries):
    output = bytearray(overview())
    output.extend(boundaries)
    for index, group in enumerate(groups, 1):
        identifier = f"text-{index:03d}"
        output.extend(
            f"===== BEGIN APPLE HOST NOTICE: {identifier} =====\n".encode(
                "utf-8"
            )
        )
        output.extend(
            f"类别：{'、'.join(group.categories)}\n".encode("utf-8")
        )
        output.extend(
            f"组件：{'、'.join(group.components)}\n".encode("utf-8")
        )
        output.extend("来源：\n".encode("utf-8"))
        for source in group.sources:
            output.extend(f"- {source}\n".encode("utf-8"))
        output.extend(
            (
                "原始文本 SHA-256："
                f"{hashlib.sha256(group.content).hexdigest()}\n\n"
            ).encode("utf-8")
        )
        output.extend(group.content)
        if not group.content.endswith(b"\n"):
            output.extend(b"\n")
        output.extend(
            f"===== END APPLE HOST NOTICE: {identifier} =====\n\n".encode(
                "utf-8"
            )
        )
    # 最后一个结束标记只保留一个换行，避免生成无意义的文件尾空行。
    return bytes(output[:-1])


def build_notice(root):
    validator = load_validator()
    state = validator.check_locks(root)
    sources = set(state.libarchive_sources)
    if len(sources) != EXPECTED_LIBARCHIVE_SOURCE_COUNT:
        fail("libarchive Xcode 编译源数量漂移")

    def ensure_tracked(relative):
        validator.ensure_tracked_file(root, relative, state.gitlinks)

    closure = collect_include_closure(
        root, sources, ensure_tracked, EXPECTED_UNRESOLVED_INCLUDES
    )
    if len(closure) != EXPECTED_LIBARCHIVE_CLOSURE_COUNT:
        fail("libarchive 双引号 include 闭包数量漂移")
    inline_paths = {
        item.path
        for item in state.license_inputs
        if item.delivery_unit == "libarchive"
        and item.role == "inline-notice"
    }
    require_closure_coverage(
        inline_paths, closure, "libarchive 许可复核输入"
    )

    inputs_by_path = {}
    input_keys = {}
    for item in state.license_inputs:
        key = (
            item.delivery_unit,
            item.component,
            item.role,
            item.path,
        )
        if key in input_keys:
            fail(f"宿主许可与来源输入记录重复：{item.path}")
        input_keys[key] = item
        inputs_by_path.setdefault(item.path, item)
    shared_unicode_license = {
        (
            item.delivery_unit,
            item.component,
            item.role,
        )
        for item in state.license_inputs
        if item.path == LIBARCHIVE_UCD_LICENSE_PATH
    }
    if shared_unicode_license != {
        ("hterm-bundle", "wcwidth", "license"),
        ("libarchive", "libarchive", "license"),
    }:
        fail("Unicode Data Files 共享许可归属漂移")
    duplicate_paths = {
        item.path
        for item in state.license_inputs
        if sum(
            other.path == item.path
            for other in state.license_inputs
        )
        > 1
    }
    if duplicate_paths != {LIBARCHIVE_UCD_LICENSE_PATH}:
        fail("除共享 Unicode Data Files 许可外，宿主输入路径不得重复")
    verify_history_evidence(root, validator)
    verify_material_icon_evidence(root, inputs_by_path)
    verify_lib_colors_evidence(root, inputs_by_path, validator)
    verify_wcwidth_unicode_evidence(root, inputs_by_path, validator)
    verify_libarchive_unicode_evidence(root, inputs_by_path, input_keys)

    raw_sources = []
    full_license_components = set()
    for item in state.license_inputs:
        if item.role != "license" or item.component not in FULL_LICENSE_COMPONENTS:
            continue
        full_license_components.add(item.component)
        if item.path == MATERIAL_ICON_LICENSE_PATH:
            component = f"Material Design Icons {MATERIAL_ICON_REVISION}"
            content = read_regular(root, item.path, "宿主完整许可文本")
        elif item.path == LIB_COLORS_W3C_NOTICE_PATH:
            component = (
                "W3C Software and Document License "
                f"{LIB_COLORS_W3C_LICENSE_VERSION}"
            )
            content = read_regular(root, item.path, "宿主完整许可文本")
        elif item.path == LIB_COLORS_XORG_LICENSE_PATH:
            component = f"X.Org rgb {LIB_COLORS_XORG_REVISION}"
            content = read_regular(root, item.path, "宿主完整许可文本")
        elif item.path == LIB_COLORS_DEBIAN_LICENSE_PATH:
            component = f"Debian X.Org {LIB_COLORS_DEBIAN_TAG}"
            content = read_regular(root, item.path, "宿主完整许可文本")
        elif (
            item.path == WCWIDTH_UCD_LICENSE_PATH
            and item.component == "wcwidth"
        ):
            component = (
                f"Unicode Character Database {WCWIDTH_UCD_VERSION}"
            )
            content = unicode_license_text(
                read_regular(root, item.path, "Unicode Data Files 许可")
            )
        elif (
            item.path == LIBARCHIVE_UCD_LICENSE_PATH
            and item.component == "libarchive"
        ):
            component = (
                f"Unicode Character Database {LIBARCHIVE_UCD_VERSION}"
            )
            content = unicode_license_text(
                read_regular(root, item.path, "Unicode Data Files 许可")
            )
        else:
            component = (
                f"{item.component} "
                f"{state.dependencies[item.component].version}"
            )
            content = read_regular(root, item.path, "宿主完整许可文本")
        raw_sources.append(
            NoticeSource(
                "完整许可",
                component,
                item.path,
                content,
            )
        )
    if full_license_components != FULL_LICENSE_COMPONENTS:
        fail("宿主完整许可组件集合漂移")

    leading_sources = []
    libarchive_label = (
        "libarchive " + state.dependencies["libarchive"].version
    )
    for relative in sorted(closure):
        leading_sources.append(
            NoticeSource(
                "libarchive 前导注释",
                libarchive_label,
                relative,
                extract_leading_comments(
                    read_regular(root, relative, "libarchive 声明来源"),
                    relative,
                ),
            )
        )
    leading_groups = group_exact_sources(leading_sources)
    if len(leading_groups) != EXPECTED_LEADING_TEXT_COUNT:
        fail("libarchive 唯一前导注释数量漂移")
    raw_sources.extend(leading_sources)

    fragment_items = {}
    for fragment in state.notice_fragments:
        item = inputs_by_path.get(fragment.path)
        if item is None or item.role not in {"inline-notice", "provenance"}:
            fail(f"宿主声明片段没有来源复核输入：{fragment.path}")
        fragment_items[fragment] = item

    fragment_paths = {
        fragment.path
        for fragment, item in fragment_items.items()
        if item.delivery_unit == "libarchive"
    }
    require_closure_coverage(
        fragment_paths, closure, "libarchive 宿主声明片段"
    )
    hterm_fragment_paths = {
        fragment.path
        for fragment, item in fragment_items.items()
        if item.delivery_unit == "hterm-bundle"
        and item.role == "inline-notice"
    }
    require_closure_coverage(
        hterm_fragment_paths,
        state.hterm_inputs,
        "hterm 宿主声明片段",
    )

    for fragment, item in fragment_items.items():
        if fragment.path == MATERIAL_ICON_README_PATH:
            label = f"Material Design Icons {MATERIAL_ICON_REVISION}"
        else:
            dependency = state.dependencies[item.component]
            label = f"{item.component} {dependency.version}"
        is_original_wcwidth_notice = (
            fragment.path.endswith("/wcwidth/lib_wc.js")
            and fragment.start_line == 7
            and fragment.end_line == 77
        )
        if item.component == "wcwidth" and not is_original_wcwidth_notice:
            label += "（UCD 13.0.0 生成证据）"
        category = (
            "libarchive 中段片段"
            if item.delivery_unit == "libarchive"
            else "hterm 来源与生成证据"
        )
        raw_sources.append(
            NoticeSource(
                category,
                label,
                (
                    f"{fragment.path}:"
                    f"{fragment.start_line}-{fragment.end_line}"
                ),
                extract_fragment(root, fragment),
            )
        )

    groups = group_exact_sources(raw_sources)
    if len(groups) != EXPECTED_NOTICE_TEXT_COUNT:
        fail("Apple 宿主声明唯一正文数量漂移")
    return render_groups(groups, audit_boundaries(inputs_by_path))


def build_watch_notice(root):
    validator = load_validator()
    state = validator.check_locks(root)
    version = state.dependencies["libarchive"].version
    copying = read_regular(root, "deps/libarchive/COPYING",
            "libarchive 完整许可文本")
    archive_entry_fragments = [
        fragment
        for fragment in state.notice_fragments
        if (
            fragment.path
            == "deps/libarchive/libarchive/archive_entry.c"
            and fragment.start_line == 1618
            and fragment.end_line == 1650
        )
    ]
    if len(archive_entry_fragments) != 1:
        fail("Watch libarchive 缺少唯一 archive_entry UC Regents 许可锁")
    archive_entry_license = extract_fragment(
        root, archive_entry_fragments[0])
    unicode_license = unicode_license_text(read_regular(
            root, LIBARCHIVE_UCD_LICENSE_PATH,
            "libarchive Unicode Data Files 许可"))
    header = (
        "Watch libarchive notices\n"
        "========================\n\n"
        "This Watch app links the complete fixed object closure required "
        "by its tar read, gzip read/write, and pax write entry points from "
        f"libarchive {version}.\n"
        "The following text is reproduced from the upstream libarchive "
        "COPYING file.\n\n"
    ).encode("utf-8")
    archive_entry_header = (
        "\n\narchive_entry UC Regents notice\n"
        "================================\n\n"
        "The linked archive_entry object contains code governed by the "
        "following source-level notice.\n\n"
    ).encode("utf-8")
    unicode_header = (
        "\n\nUnicode Character Database 6.0.0 notice\n"
        "=======================================\n\n"
        "libarchive's archive_string composition table is generated from "
        "Unicode 6.0.0 data. The applicable locked Unicode notice follows.\n\n"
    ).encode("utf-8")
    return (
        header
        + copying
        + archive_entry_header
        + archive_entry_license
        + unicode_header
        + unicode_license
    )


def output_path(root, relative=OUTPUT_RELATIVE):
    parent = root
    for part in relative.parent.parts:
        parent = parent / part
        try:
            metadata = parent.lstat()
        except OSError as error:
            fail(f"宿主声明输出目录不存在：{parent}（{error}）")
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            fail(f"宿主声明输出目录必须是非符号链接目录：{parent}")
    path = root / relative
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return path
    except OSError as error:
        fail(f"无法检查宿主声明输出：{error}")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        fail("宿主声明输出必须不存在或为常规文件")
    return path


def atomic_replace(path, content):
    descriptor = None
    temporary = None
    try:
        descriptor, temporary = tempfile.mkstemp(
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=path.parent,
        )
        os.fchmod(descriptor, 0o644)
        with os.fdopen(descriptor, "wb") as output:
            descriptor = None
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass


def check_output(path, expected):
    try:
        actual = path.read_bytes()
    except OSError as error:
        fail(f"无法读取宿主声明生成物：{error}")
    if actual != expected:
        fail("APPLE-HOST-NOTICES.txt 与锁定输入重建结果不一致；请显式 render")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="生成或只读校验 Apple 宿主第三方声明。"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command, help_text in (
        ("check-locks", "只读重建并比对宿主声明"),
        ("render", "显式原子写入宿主声明"),
    ):
        command_parser = subparsers.add_parser(command, help=help_text)
        command_parser.add_argument(
            "--root",
            type=Path,
            default=ROOT,
            help="仓库根目录，默认使用脚本所在仓库",
        )
    arguments = parser.parse_args(argv)
    root = arguments.root.resolve()
    content = build_notice(root)
    watch_content = build_watch_notice(root)
    path = output_path(root)
    watch_path = output_path(root, WATCH_OUTPUT_RELATIVE)
    if arguments.command == "check-locks":
        check_output(path, content)
        check_output(watch_path, watch_content)
        print("Apple 宿主第三方声明校验通过")
    elif arguments.command == "render":
        atomic_replace(path, content)
        atomic_replace(watch_path, watch_content)
        print(f"已生成 Apple 宿主第三方声明：{path}")
        print(f"已生成 Watch libarchive 声明：{watch_path}")
    else:
        fail("未知命令")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (HostInputError, OSError, UnicodeError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

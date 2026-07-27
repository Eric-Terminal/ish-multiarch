#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
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
EXPECTED_NOTICE_TEXT_COUNT = 125
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
        "覆盖范围：本文件汇集普通 iSH 与可选 iSH+Linux 共同交付的 "
        "hterm 生成资源及 libarchive 静态库的锁定原始文本，并在后续"
        "专节记录已闭合的来源证据和仍未闭合的外部来源。\n"
        "\n"
        "未决边界：\n"
        "- Linux kernel、在线 rootfs 及其许可与对应源码不在本文件内；"
        "本文件不能代表 iSH+Linux 的完整发行义务已经闭合。\n"
        "- Alpine guest seed、项目自身许可、LGPL 与对应源码交付继续由"
        "各自的发行门禁处理。\n"
        "- libarchive 的 BLAKE2 声明列出 CC0 1.0 Universal、OpenSSL "
        "与 Apache 2.0 三种选项；这里只逐字收录，不替发行者选择分支。\n"
        "- 三个已交付 Material 图标的来源、格式化关系与适用许可证据见"
        "后续专节；wcwidth Unicode 13.0.0 的数据、重放关系与许可也已"
        "闭合；libarchive Unicode 6.0.0 的产品表、生成关系、规范版本与"
        "许可路径也已闭合。W3C/X11 缺口继续列在未闭合一节。\n"
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


def unresolved_provenance():
    return (
        "===== BEGIN APPLE HOST NOTICE: unresolved-provenance =====\n"
        "未闭合的外部来源与许可\n"
        "\n"
        "libdot 外部来源：lib_colors.js 的锁定注释说明 HSL 算法改编自 "
        "W3C CSS Color 4，颜色表派生自 stock X11 rgb.txt；仓内没有所用"
        "上游版本、X11 原始数据或对应权威条款。收录这些注释不表示外部"
        "许可已经确定。\n"
        "===== END APPLE HOST NOTICE: unresolved-provenance =====\n"
        "\n"
    ).encode("utf-8")


def audit_boundaries(inputs_by_path):
    return (
        material_provenance(inputs_by_path)
        + wcwidth_unicode_provenance(inputs_by_path)
        + libarchive_unicode_provenance(inputs_by_path)
        + unresolved_provenance()
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


def output_path(root):
    parent = root
    for part in OUTPUT_RELATIVE.parent.parts:
        parent = parent / part
        try:
            metadata = parent.lstat()
        except OSError as error:
            fail(f"宿主声明输出目录不存在：{parent}（{error}）")
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            fail(f"宿主声明输出目录必须是非符号链接目录：{parent}")
    path = root / OUTPUT_RELATIVE
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
    path = output_path(root)
    if arguments.command == "check-locks":
        check_output(path, content)
        print("Apple 宿主第三方声明校验通过")
    elif arguments.command == "render":
        atomic_replace(path, content)
        print(f"已生成 Apple 宿主第三方声明：{path}")
    else:
        fail("未知命令")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (HostInputError, OSError, UnicodeError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

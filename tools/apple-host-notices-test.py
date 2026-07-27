#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from apple_host_manifest import APPLE_HOST_RAW_INPUT_PATHS, HostInputError


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
GENERATOR = TOOLS / "apple-host-notices.py"
SPEC = importlib.util.spec_from_file_location("apple_host_notices", GENERATOR)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 Apple 宿主声明生成器")
NOTICES = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = NOTICES
SPEC.loader.exec_module(NOTICES)


class TestFailure(Exception):
    pass


def fail(message):
    raise TestFailure(message)


def expect_failure(action, expected):
    try:
        action()
    except HostInputError as error:
        if expected not in str(error):
            fail(f"负例没有命中预期错误“{expected}”：{error}")
    else:
        fail(f"负例意外成功：{expected}")


def test_production_output():
    first = NOTICES.build_notice(ROOT)
    second = NOTICES.build_notice(ROOT)
    if first != second:
        fail("相同宿主输入没有生成逐字节一致的声明")
    if first.splitlines()[0] != (
        b"===== BEGIN APPLE HOST NOTICE: overview ====="
    ):
        fail("宿主声明首行 marker 漂移")
    if NOTICES.EXPECTED_NOTICE_TEXT_COUNT != 128:
        fail("宿主声明唯一正文组数量合同漂移")
    begin_count = first.count(b"===== BEGIN APPLE HOST NOTICE:")
    if begin_count != 133 or begin_count != first.count(
        b"===== END APPLE HOST NOTICE:"
    ):
        fail("宿主声明 section marker 不平衡")
    if first.count(b"===== BEGIN APPLE HOST NOTICE: text-") != 128:
        fail("宿主声明正文 section 数量漂移")
    for identifier in (
        "overview",
        "material-provenance",
        "wcwidth-unicode-provenance",
        "libarchive-unicode-provenance",
        "lib-colors-provenance",
    ):
        begin = (
            f"===== BEGIN APPLE HOST NOTICE: {identifier} =====\n"
        ).encode("utf-8")
        end = (
            f"===== END APPLE HOST NOTICE: {identifier} =====\n"
        ).encode("utf-8")
        if first.count(begin) != 1 or first.count(end) != 1:
            fail(f"宿主声明缺少唯一边界 section：{identifier}")

    for metadata in (
        "组件：hterm 1.91.0、libdot 8.0.0\n",
        "组件：intl-segmenter snapshot\n",
        "组件：wcwidth 0.0.3\n",
        (
            "组件：Unicode Character Database "
            f"{NOTICES.WCWIDTH_UCD_VERSION}、"
            "Unicode Character Database "
            f"{NOTICES.LIBARCHIVE_UCD_VERSION}\n"
        ),
        "组件：libarchive 3.4.3\n",
        f"组件：Material Design Icons {NOTICES.MATERIAL_ICON_REVISION}\n",
        (
            "组件：W3C Software and Document License "
            f"{NOTICES.LIB_COLORS_W3C_LICENSE_VERSION}\n"
        ),
        f"组件：X.Org rgb {NOTICES.LIB_COLORS_XORG_REVISION}\n",
        f"组件：Debian X.Org {NOTICES.LIB_COLORS_DEBIAN_TAG}\n",
    ):
        if metadata.encode("utf-8") not in first:
            fail(f"宿主声明缺少锁定版本元数据：{metadata.strip()}")
    for relative in (
        "deps/libapps/hterm/LICENSE",
        "deps/libapps/libdot/LICENSE",
        "deps/libapps/libdot/third_party/intl-segmenter/LICENSE.md",
        "deps/libapps/libdot/third_party/wcwidth/LICENSE.md",
        "deps/libarchive/COPYING",
        NOTICES.MATERIAL_ICON_LICENSE_PATH,
        NOTICES.LIB_COLORS_W3C_NOTICE_PATH,
        NOTICES.LIB_COLORS_XORG_LICENSE_PATH,
        NOTICES.LIB_COLORS_DEBIAN_LICENSE_PATH,
    ):
        content = (ROOT / relative).read_bytes()
        if first.count(content) != 1:
            fail(f"宿主声明缺少完整许可文本：{relative}")
    unicode_license = NOTICES.unicode_license_text(
        (ROOT / NOTICES.WCWIDTH_UCD_LICENSE_PATH).read_bytes()
    )
    if first.count(unicode_license) != 1:
        fail("宿主声明缺少完整 Unicode Data Files 许可文本")
    if (ROOT / NOTICES.WCWIDTH_UCD_LICENSE_PATH).read_bytes() in first:
        fail("宿主声明不应把 Unicode 许可文件的 UTF-8 BOM 放在正文中部")
    if (ROOT / "deps/linux/COPYING").read_bytes() in first:
        fail("宿主声明错误收入 Linux COPYING 正文")

    license_rows = (
        ROOT / "third_party/apple-host/license-inputs.tsv"
    ).read_text(encoding="utf-8").splitlines()[1:]
    lib_colors_raw_inputs = {
        NOTICES.LIB_COLORS_CSSWG_LICENSE_PATH,
        NOTICES.LIB_COLORS_CSSWG_OVERVIEW_PATH,
        NOTICES.LIB_COLORS_DEBIAN_LICENSE_PATH,
        NOTICES.LIB_COLORS_DEBIAN_RGB_PATH,
        NOTICES.LIB_COLORS_W3C_LICENSE_HTML_PATH,
        NOTICES.LIB_COLORS_W3C_NOTICE_PATH,
        NOTICES.LIB_COLORS_XORG_LICENSE_PATH,
        NOTICES.LIB_COLORS_XORG_RGB_PATH,
    }
    if (
        len(APPLE_HOST_RAW_INPUT_PATHS) != 21
        or not lib_colors_raw_inputs.issubset(APPLE_HOST_RAW_INPUT_PATHS)
    ):
        fail("Apple 宿主原始输入路径合同漂移")
    hterm_provenance = {
        fields[3]
        for fields in (row.split("\t") for row in license_rows)
        if fields[1:3] == ["hterm", "provenance"]
    }
    expected_hterm_provenance = {
        "deps/libapps/hterm/concat/hterm_resources.concat",
        *NOTICES.MATERIAL_ICON_PATHS,
        NOTICES.MATERIAL_ICON_README_PATH,
        *(
            f"{NOTICES.MATERIAL_ICON_SNAPSHOT_BASE}/{upstream}"
            for _current, upstream in NOTICES.MATERIAL_ICON_SOURCES
        ),
    }
    if hterm_provenance != expected_hterm_provenance:
        fail("hterm Material 图标 provenance 输入集合漂移")

    libdot_provenance = {
        fields[3]
        for fields in (row.split("\t") for row in license_rows)
        if fields[1:3] == ["libdot", "provenance"]
    }
    if libdot_provenance != {
        NOTICES.LIB_COLORS_CSSWG_LICENSE_PATH,
        NOTICES.LIB_COLORS_CSSWG_OVERVIEW_PATH,
        NOTICES.LIB_COLORS_DEBIAN_RGB_PATH,
        NOTICES.LIB_COLORS_W3C_LICENSE_HTML_PATH,
        NOTICES.LIB_COLORS_XORG_RGB_PATH,
    }:
        fail("libdot W3C/X11 provenance 输入集合漂移")

    for current, upstream in NOTICES.MATERIAL_ICON_SOURCES:
        current_item = next(
            fields
            for fields in (row.split("\t") for row in license_rows)
            if fields[3] == current
        )
        snapshot = f"{NOTICES.MATERIAL_ICON_SNAPSHOT_BASE}/{upstream}"
        upstream_item = next(
            fields
            for fields in (row.split("\t") for row in license_rows)
            if fields[3] == snapshot
        )
        for marker in (
            f"- 当前 {current}；{current_item[4]} 字节；SHA-256 "
            f"{current_item[5]}",
            f"  上游 {upstream}；{upstream_item[4]} 字节；SHA-256 "
            f"{upstream_item[5]}",
        ):
            if first.count(marker.encode("utf-8")) != 1:
                fail(f"宿主声明缺少唯一 Material 映射证据：{marker}")
    for marker in (
        NOTICES.MATERIAL_ICON_IMPORT_COMMIT,
        NOTICES.MATERIAL_ICON_REVISION,
        NOTICES.MATERIAL_ICON_SOURCE_URL,
        "作为同时期不可变快照",
        "这不表示导入者明确记录或选择了该 revision",
        "校验器会离线逐字重放该关系",
        f"{NOTICES.MATERIAL_ICON_README_PATH}:37-40",
        "在 Apache License Version 2.0 下提供",
    ):
        if marker.encode("utf-8") not in first:
            fail(f"宿主声明缺少 Material 来源或许可证据：{marker}")
    readme_lines = (
        ROOT / NOTICES.MATERIAL_ICON_README_PATH
    ).read_bytes().splitlines(keepends=True)
    applicability = b"".join(readme_lines[36:40])
    if first.count(applicability) != 1:
        fail("宿主声明缺少唯一 Material Apache-2.0 适用范围原文")
    for stale in (
        "当前锁定仓内没有对应上游 revision",
        "不能据此推定 Apache 2.0",
        "Material Design 图标：锁定 libapps 历史提交",
    ):
        if stale.encode("utf-8") in first:
            fail(f"宿主声明仍保留已闭合的 Material 未决文案：{stale}")

    fragments = (
        ROOT / "third_party/apple-host/notice-fragments.tsv"
    ).read_text(encoding="utf-8").splitlines()[1:]
    if len(fragments) != 22:
        fail("宿主声明片段数量合同漂移")
    for row in fragments:
        path, start, end, _size, _sha256 = row.split("\t")
        marker = f"{path}:{start}-{end}".encode("utf-8")
        if first.count(marker) != 1:
            fail(f"宿主声明缺少唯一锁定片段来源：{marker!r}")

    wcwidth_source = (
        b"- deps/libapps/libdot/third_party/wcwidth/lib_wc.js:7-77\n"
    )
    section_start = first.rfind(
        b"===== BEGIN APPLE HOST NOTICE:", 0, first.index(wcwidth_source)
    )
    section_end = first.index(b"===== END APPLE HOST NOTICE:", section_start)
    wcwidth_source_section = first[section_start:section_end]
    for marker in (
        b"ported from the wcwidth.js module of node.js",
        b"https://npmjs.org/package/wcwidth.js",
    ):
        if marker not in wcwidth_source_section:
            fail(f"wcwidth 原始移植片段缺少来源证据：{marker!r}")
    if "UCD 13.0.0 生成证据".encode("utf-8") in wcwidth_source_section:
        fail("wcwidth 原始移植片段被错误标记为 UCD 生成证据")

    for marker in (
        "已闭合的 wcwidth Unicode 13.0.0 生成来源与许可",
        NOTICES.WCWIDTH_UCD_EVIDENCE_COMMIT,
        NOTICES.WCWIDTH_UNICODETOOLS_TAG,
        NOTICES.WCWIDTH_UNICODETOOLS_REVISION,
        NOTICES.WCWIDTH_UNICODETOOLS_SOURCE_URL,
        NOTICES.WCWIDTH_UCD_ARCHIVE_URL,
        NOTICES.WCWIDTH_UCD_ARCHIVE_SHA256,
        NOTICES.WCWIDTH_RANGES_GIT_BLOB,
        NOTICES.WCWIDTH_RANGES_SHA256,
        "已闭合的 libarchive Unicode 6.0.0 规范化表来源与许可",
        NOTICES.LIBARCHIVE_UNICODETOOLS_REVISION,
        NOTICES.LIBARCHIVE_UNICODETOOLS_SOURCE_URL,
        NOTICES.LIBARCHIVE_UCD_ARCHIVE_URL,
        NOTICES.LIBARCHIVE_UCD_ARCHIVE_SHA256,
        NOTICES.LIBARCHIVE_UNICODE_GENERATOR_GIT_BLOB,
        NOTICES.LIBARCHIVE_GENERATED_BASELINE_GIT_BLOB,
        NOTICES.LIBARCHIVE_COMPOSITION_HEADER_GIT_BLOB,
        NOTICES.LIBARCHIVE_UAX15_URL,
        NOTICES.LIBARCHIVE_UAX15_SHA256,
        "这不表示 libarchive 作者明确记录或使用了该 Git 提交",
        "这里只记录本工程采用的许可证据路径",
        "不冒充 2010 归档内许可原件",
        "不声称 Unicode 在该提交中专门重新许可旧数据",
        "技术报告全文不作为数据输入复制进仓库或产品",
        "NormalizationTest 来源未进入 Apple App 的 128 个编译源",
        "这不表示 libapps 作者明确记录或使用了该 Git 提交",
        "先把当前三张表替换为哨兵",
        "UCD.zip 本身不含 LICENSE",
        "组件：wcwidth 0.0.3（UCD 13.0.0 生成证据）",
        "deps/libapps/libdot/js/lib_colors.js:322-323",
        "deps/libapps/libdot/js/lib_colors.js:629-640",
        "deps/libapps/libdot/js/lib_colors.js:755-757",
        "The following algorithm has been adapted from:",
        "This list of color name to RGB mapping is derived from the stock X11",
        "已闭合的 lib_colors W3C HSL 与 X11 颜色表来源及许可",
        NOTICES.LIB_COLORS_X11_IMPORT_COMMIT,
        NOTICES.LIB_COLORS_X11_IMPORT_TREE,
        NOTICES.LIB_COLORS_X11_IMPORT_GIT_BLOB,
        NOTICES.LIB_COLORS_HSL_IMPORT_COMMIT,
        NOTICES.LIB_COLORS_HSL_IMPORT_TREE,
        NOTICES.LIB_COLORS_HSL_IMPORT_GIT_BLOB,
        NOTICES.LIB_COLORS_XORG_SOURCE_URL,
        NOTICES.LIB_COLORS_XORG_REVISION,
        NOTICES.LIB_COLORS_XORG_TREE,
        NOTICES.LIB_COLORS_DEBIAN_SOURCE_URL,
        NOTICES.LIB_COLORS_DEBIAN_TAG,
        NOTICES.LIB_COLORS_DEBIAN_TAG_OBJECT,
        NOTICES.LIB_COLORS_DEBIAN_REVISION,
        NOTICES.LIB_COLORS_CSSWG_SOURCE_URL,
        NOTICES.LIB_COLORS_CSSWG_REVISION,
        NOTICES.LIB_COLORS_CSSWG_TREE,
        NOTICES.LIB_COLORS_TABLE_SHA256,
        NOTICES.LIB_COLORS_HSL_SOURCE_SHA256,
        NOTICES.LIB_COLORS_HSL_CURRENT_SHA256,
        NOTICES.LIB_COLORS_W3C_TR_URL,
        NOTICES.LIB_COLORS_W3C_TR_SHA256,
        NOTICES.LIB_COLORS_W3C_LICENSE_URL,
        NOTICES.LIB_COLORS_W3C_CHANGE_NOTICE,
        "同名后项覆盖",
        "不能冒充 2019 年网页原件",
        "这不表示 libapps 作者明确选择了这两个 revision",
        "八份原始输入不作为独立资源进入 App bundle",
        "deps/libarchive/libarchive/archive_string.c:2797-2800",
        "deps/libarchive/libarchive/archive_string.c:3043-3046",
        "Unicode Standard Annex #15",
    ):
        if marker.encode("utf-8") not in first:
            fail(f"宿主声明缺少外部来源或生成证据：{marker}")
    expected_unicode_path_counts = {
        NOTICES.WCWIDTH_UCD_README_PATH: 1,
        NOTICES.WCWIDTH_UCD_LICENSE_PATH: 3,
        **{path: 1 for path in NOTICES.WCWIDTH_UCD_DATA_PATHS},
        NOTICES.LIBARCHIVE_UCD_README_PATH: 1,
        **{path: 1 for path in NOTICES.LIBARCHIVE_UCD_DATA_PATHS},
    }
    for relative, count in expected_unicode_path_counts.items():
        if first.count(relative.encode("utf-8")) != count:
            fail(f"宿主声明 Unicode 来源路径数量漂移：{relative}")
    expected_lib_colors_path_counts = {
        NOTICES.LIB_COLORS_CSSWG_LICENSE_PATH: 1,
        NOTICES.LIB_COLORS_CSSWG_OVERVIEW_PATH: 1,
        NOTICES.LIB_COLORS_W3C_LICENSE_HTML_PATH: 1,
        NOTICES.LIB_COLORS_DEBIAN_RGB_PATH: 1,
        NOTICES.LIB_COLORS_XORG_RGB_PATH: 1,
        NOTICES.LIB_COLORS_W3C_NOTICE_PATH: 2,
        NOTICES.LIB_COLORS_DEBIAN_LICENSE_PATH: 2,
        NOTICES.LIB_COLORS_XORG_LICENSE_PATH: 2,
    }
    for relative, count in expected_lib_colors_path_counts.items():
        if first.count(relative.encode("utf-8")) != count:
            fail(f"宿主声明 lib_colors 来源路径数量漂移：{relative}")
    for stale in (
        "wcwidth Unicode 数据：完整 lib_wc.js 摘要与原始来源注释已锁定",
        "生成脚本读取 PropList.txt、UnicodeData.txt 与 EastAsianWidth.txt",
        "仓内没有这三份 13.0.0 原始字节或相应 Unicode 许可原文",
        "libarchive Unicode 来源：archive_string_composition.h",
        "来源数据、标准文本版本与适用许可仍须在公共发行前单独闭合",
        "unresolved-provenance",
        "未闭合的外部来源与许可",
        "仓内没有所用上游版本",
        "W3C/X11 缺口继续列在未闭合一节",
    ):
        if stale.encode("utf-8") in first:
            fail(f"宿主声明仍保留已闭合的 wcwidth 未决文案：{stale}")

    output = ROOT / NOTICES.OUTPUT_RELATIVE
    NOTICES.check_output(output, first)


def test_material_icon_formatting(temporary_root):
    upstream = b'<svg width="24"><path d="M0 0z"/></svg>'
    current = NOTICES.format_material_icon(upstream, "合成上游 SVG")
    if current != (
        b'<svg width="24">\n  <path d="M0 0z"/>\n</svg>\n'
    ):
        fail("Material SVG 固定格式化没有生成预期字节")
    expect_failure(
        lambda: NOTICES.format_material_icon(
            upstream + b"\n", "带文件尾换行的合成上游 SVG"
        ),
        "结构漂移",
    )

    inputs = {}
    evidence = {
        NOTICES.MATERIAL_ICON_LICENSE_PATH: (
            b"Apache License\n"
            b"Version 2.0, January 2004\n"
            b"http://www.apache.org/licenses/\n"
            b"TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION\n"
        ),
        NOTICES.MATERIAL_ICON_README_PATH: (
            NOTICES.MATERIAL_ICON_README_LICENSE_HEADING
            + NOTICES.MATERIAL_ICON_README_APPLICABILITY
        ),
    }
    for relative, role in (
        (NOTICES.MATERIAL_ICON_LICENSE_PATH, "license"),
        (NOTICES.MATERIAL_ICON_README_PATH, "provenance"),
    ):
        path = temporary_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(evidence[relative])
        inputs[relative] = SimpleNamespace(
            delivery_unit="hterm-bundle",
            component="hterm",
            role=role,
        )
    for local, upstream_relative in NOTICES.MATERIAL_ICON_SOURCES:
        snapshot = (
            f"{NOTICES.MATERIAL_ICON_SNAPSHOT_BASE}/{upstream_relative}"
        )
        for relative, content in (
            (snapshot, upstream),
            (local, current),
        ):
            path = temporary_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            inputs[relative] = SimpleNamespace(
                delivery_unit="hterm-bundle",
                component="hterm",
                role="provenance",
            )
    NOTICES.verify_material_icon_evidence(temporary_root, inputs)

    readme_path = temporary_root / NOTICES.MATERIAL_ICON_README_PATH
    readme_path.write_bytes(
        evidence[NOTICES.MATERIAL_ICON_README_PATH].replace(
            b"Apache License Version 2.0",
            b"Apache License Version 1.0",
        )
    )
    expect_failure(
        lambda: NOTICES.verify_material_icon_evidence(
            temporary_root, inputs
        ),
        "README 的 Apache-2.0 适用声明漂移",
    )
    readme_path.write_bytes(evidence[NOTICES.MATERIAL_ICON_README_PATH])

    license_path = temporary_root / NOTICES.MATERIAL_ICON_LICENSE_PATH
    license_path.write_bytes(
        evidence[NOTICES.MATERIAL_ICON_LICENSE_PATH].replace(
            b"Version 2.0", b"Version 1.0"
        )
    )
    expect_failure(
        lambda: NOTICES.verify_material_icon_evidence(
            temporary_root, inputs
        ),
        "LICENSE 的 Apache-2.0 正文标记漂移",
    )
    license_path.write_bytes(evidence[NOTICES.MATERIAL_ICON_LICENSE_PATH])

    first_local = NOTICES.MATERIAL_ICON_SOURCES[0][0]
    (temporary_root / first_local).write_bytes(current.replace(b"M0", b"M1"))
    expect_failure(
        lambda: NOTICES.verify_material_icon_evidence(
            temporary_root, inputs
        ),
        "固定格式化关系漂移",
    )


def test_lib_colors_replay():
    xorg_rgb = (ROOT / NOTICES.LIB_COLORS_XORG_RGB_PATH).read_bytes()
    debian_rgb = (ROOT / NOTICES.LIB_COLORS_DEBIAN_RGB_PATH).read_bytes()
    current = (ROOT / NOTICES.LIB_COLORS_CURRENT_PATH).read_bytes()
    colors = NOTICES.verify_lib_colors_rgb_replay(
        xorg_rgb, debian_rgb, current
    )
    if len(colors) != NOTICES.LIB_COLORS_COLOR_COUNT:
        fail("lib_colors 颜色表离线重放数量漂移")

    records, synthetic = NOTICES.parse_lib_colors_rgb(
        b"1 2 3 Foo Bar\n4 5 6 FooBar\n",
        "合成 X11 表",
    )
    if records != 2 or synthetic != {"foobar": (4, 5, 6)}:
        fail("X11 归一化重复键没有执行后项覆盖")
    expect_failure(
        lambda: NOTICES.parse_lib_colors_rgb(
            b"256 2 3 Invalid\n", "合成 X11 表"
        ),
        "RGB 越界",
    )

    expect_failure(
        lambda: NOTICES.verify_lib_colors_rgb_replay(
            xorg_rgb,
            debian_rgb.replace(
                NOTICES.LIB_COLORS_DEBIAN_EXTENSION_LINE,
                b"214   7  81\t\tDebianRed\n",
                1,
            ),
            current,
        ),
        "DebianRed 唯一扩展行",
    )

    common_tail = b"144 238 144\t\tLightGreen\n"
    if xorg_rgb.count(common_tail) != 1 or debian_rgb.count(common_tail) != 1:
        fail("X11 颜色表负例缺少唯一共同变更点")
    changed_tail = b"143 238 144\t\tLightGreen\n"
    expect_failure(
        lambda: NOTICES.verify_lib_colors_rgb_replay(
            xorg_rgb.replace(common_tail, changed_tail, 1),
            debian_rgb.replace(common_tail, changed_tail, 1),
            current,
        ),
        "不能逐字重建当前 lib_colors 颜色表",
    )

    current_debian_red = b"'debianred': 'rgb(215, 7, 81)'"
    if current.count(current_debian_red) != 1:
        fail("当前颜色表负例缺少唯一 DebianRed 变更点")
    expect_failure(
        lambda: NOTICES.verify_lib_colors_rgb_replay(
            xorg_rgb,
            debian_rgb,
            current.replace(
                current_debian_red,
                b"'debianred': 'rgb(214, 7, 81)'",
                1,
            ),
        ),
        "不能逐字重建当前 lib_colors 颜色表",
    )

    overview = (
        ROOT / NOTICES.LIB_COLORS_CSSWG_OVERVIEW_PATH
    ).read_bytes()
    NOTICES.verify_lib_colors_hsl_adaptation(overview, current)
    expect_failure(
        lambda: NOTICES.verify_lib_colors_hsl_adaptation(
            overview.replace(
                b"(t2 - t1) * (4 - hue)",
                b"(t2 - t1) * (5 - hue)",
                1,
            ),
            current,
        ),
        "CSS Color 4 HSL 算法原始字节漂移",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_hsl_adaptation(
            overview,
            current.replace(
                b"light <= 0.5 ? light * (sat + 1)",
                b"light <= 0.4 ? light * (sat + 1)",
                1,
            ),
        ),
        "lib_colors 当前 HSL 改编函数字节漂移",
    )


def test_lib_colors_licenses():
    csswg_license = (
        ROOT / NOTICES.LIB_COLORS_CSSWG_LICENSE_PATH
    ).read_bytes()
    notice = (ROOT / NOTICES.LIB_COLORS_W3C_NOTICE_PATH).read_bytes()
    license_html = (
        ROOT / NOTICES.LIB_COLORS_W3C_LICENSE_HTML_PATH
    ).read_bytes()
    xorg_license = (
        ROOT / NOTICES.LIB_COLORS_XORG_LICENSE_PATH
    ).read_bytes()
    debian_license = (
        ROOT / NOTICES.LIB_COLORS_DEBIAN_LICENSE_PATH
    ).read_bytes()
    NOTICES.verify_lib_colors_licenses(
        csswg_license,
        notice,
        license_html,
        xorg_license,
        debian_license,
    )

    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license.replace(
                b"[W3C Software and Document License]",
                b"[W3C Document License]",
                1,
            ),
            notice,
            license_html,
            xorg_license,
            debian_license,
        ),
        "CSSWG 根 LICENSE",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license,
            notice.replace(
                b"Permission to copy, modify, and distribute this work",
                b"Permission to copy and distribute this work",
                1,
            ),
            license_html,
            xorg_license,
            debian_license,
        ),
        "W3C Software and Document NOTICE",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license,
            notice.replace(
                b"COPYRIGHT HOLDERS WILL NOT BE LIABLE",
                b"COPYRIGHT HOLDERS MAY BE LIABLE",
                1,
            ),
            license_html,
            xorg_license,
            debian_license,
        ),
        "W3C Software and Document NOTICE",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license,
            notice,
            license_html.replace(
                b"13 May 2015 and 31 December 2022",
                b"13 May 2014 and 31 December 2022",
                1,
            ),
            xorg_license,
            debian_license,
        ),
        "W3C 2015 官方许可页面",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license,
            notice,
            license_html,
            xorg_license.replace(
                b"Copyright 1985, 1989, 1998  The Open Group",
                b"Copyright 1985, 1989  The Open Group",
                1,
            ),
            debian_license,
        ),
        "X.Org rgb COPYING",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license,
            notice,
            license_html,
            xorg_license,
            debian_license.replace(
                b"Copyright 2004-2005 Canonical Ltd.",
                b"Copyright 2004 Canonical Ltd.",
                1,
            ),
        ),
        "Debian X.Org copyright",
    )
    expect_failure(
        lambda: NOTICES.verify_lib_colors_licenses(
            csswg_license,
            notice,
            license_html,
            xorg_license,
            debian_license.replace(
                b"Permission is hereby granted, free of charge",
                b"Permission is granted, free of charge",
                1,
            ),
        ),
        "Debian X.Org copyright 授权条款",
    )


def test_wcwidth_unicode_replay():
    license_data = (ROOT / NOTICES.WCWIDTH_UCD_LICENSE_PATH).read_bytes()
    normalized = NOTICES.unicode_license_text(license_data)
    if normalized != license_data[len(NOTICES.UNICODE_LICENSE_BOM) :]:
        fail("Unicode Data Files 许可只应移除唯一 UTF-8 BOM")
    expect_failure(
        lambda: NOTICES.unicode_license_text(
            license_data.replace(b"1991-2020", b"1991-2019", 1)
        ),
        "许可正文标记漂移",
    )

    data_by_name = {
        name: (
            ROOT / NOTICES.WCWIDTH_UCD_SNAPSHOT_BASE / name
        ).read_bytes()
        for name, _blob in NOTICES.WCWIDTH_UCD_DATA_FILES
    }
    unicode_data = data_by_name["UnicodeData.txt"]
    marker = b"0300;COMBINING GRAVE ACCENT;Mn;"
    if unicode_data.count(marker) != 1:
        fail("UnicodeData 负例缺少唯一组合字符变更点")
    mutated = dict(data_by_name)
    mutated["UnicodeData.txt"] = unicode_data.replace(
        marker,
        b"0300;COMBINING GRAVE ACCENT;Lu;",
        1,
    )
    expect_failure(
        lambda: NOTICES.replay_wcwidth_tables(
            ROOT, NOTICES.load_validator(), mutated
        ),
        "不能逐字重建当前三张表",
    )


def test_libarchive_unicode_replay():
    data_by_name = {
        name: (
            ROOT / NOTICES.LIBARCHIVE_UCD_SNAPSHOT_BASE / name
        ).read_bytes()
        for name, _blob in NOTICES.LIBARCHIVE_UCD_DATA_FILES
    }
    header = (
        ROOT / NOTICES.LIBARCHIVE_COMPOSITION_HEADER_PATH
    ).read_bytes()
    NOTICES.verify_libarchive_composition_semantics(
        data_by_name["UnicodeData.txt"],
        data_by_name["CompositionExclusions.txt"],
        header,
    )

    unicode_data = data_by_name["UnicodeData.txt"]
    combining_marker = b"0308;COMBINING DIAERESIS;Mn;230;"
    if unicode_data.count(combining_marker) != 1:
        fail("libarchive UnicodeData 负例缺少唯一 CCC 变更点")
    mutated_unicode_data = unicode_data.replace(
        combining_marker,
        b"0308;COMBINING DIAERESIS;Mn;0;",
        1,
    )
    expect_failure(
        lambda: NOTICES.verify_libarchive_composition_semantics(
            mutated_unicode_data,
            data_by_name["CompositionExclusions.txt"],
            header,
        ),
        "组合记录数量漂移",
    )

    exclusions = data_by_name["CompositionExclusions.txt"]
    exclusion_marker = b"0958    #  DEVANAGARI LETTER QA\n"
    if exclusions.count(exclusion_marker) != 1:
        fail("libarchive CompositionExclusions 负例缺少唯一变更点")
    mutated_exclusions = exclusions.replace(
        exclusion_marker,
        b"0957    #  DEVANAGARI LETTER QA\n",
        1,
    )
    expect_failure(
        lambda: NOTICES.verify_libarchive_composition_semantics(
            unicode_data,
            mutated_exclusions,
            header,
        ),
        "组合记录数量漂移",
    )

    generator = (
        ROOT / NOTICES.LIBARCHIVE_UNICODE_GENERATOR_PATH
    ).read_bytes()
    expect_failure(
        lambda: NOTICES.replay_libarchive_composition_header(
            generator.replace(b"Expect UnicodeData.txt", b"Expect UCD input", 1),
            unicode_data,
            header,
        ),
        "生成脚本字节漂移",
    )
    expect_failure(
        lambda: NOTICES.replay_libarchive_composition_header(
            generator,
            unicode_data,
            header.replace(b"Canonical Combining", b"Canonical Changed", 1),
        ),
        "当前生成头文件字节漂移",
    )

    source = (ROOT / NOTICES.LIBARCHIVE_STRING_SOURCE_PATH).read_bytes()
    NOTICES.verify_libarchive_hangul_constants(source)
    expect_failure(
        lambda: NOTICES.verify_libarchive_hangul_constants(
            source.replace(b"#define HC_TCOUNT\t28", b"#define HC_TCOUNT\t27", 1)
        ),
        "HC_TCOUNT",
    )


def test_leading_comments_and_include_closure(temporary_root):
    source_root = temporary_root / "deps/libarchive/libarchive"
    source_root.mkdir(parents=True)
    (source_root / "main.c").write_bytes(
        b"/* same */\n#include \"a.h\"\nint main(void) { return 0; }\n"
    )
    (source_root / "a.h").write_bytes(
        b"/* same */\n#include \"b.h\"\n"
    )
    (source_root / "b.h").write_bytes(
        b"/* first */\n\n/* second */\n#define VALUE 1\n"
    )
    seen = []
    closure = NOTICES.collect_include_closure(
        temporary_root,
        {"deps/libarchive/libarchive/main.c"},
        seen.append,
        set(),
    )
    if closure != {
        "deps/libarchive/libarchive/main.c",
        "deps/libarchive/libarchive/a.h",
        "deps/libarchive/libarchive/b.h",
    } or set(seen) != closure:
        fail("双引号 include 没有形成精确本地递归闭包")
    if NOTICES.extract_leading_comments(
        (source_root / "b.h").read_bytes(), "b.h"
    ) != b"/* first */\n\n/* second */\n":
        fail("连续文件前导 C 块注释提取边界错误")
    groups = NOTICES.group_exact_sources(
        [
            NOTICES.NoticeSource(
                "libarchive 前导注释", "libarchive", relative,
                NOTICES.extract_leading_comments(
                    (temporary_root / relative).read_bytes(), relative
                ),
            )
            for relative in sorted(closure)
        ]
    )
    if len(groups) != 2 or not any(len(group.sources) == 2 for group in groups):
        fail("相同原始前导注释没有精确去重并保留全部来源")
    expect_failure(
        lambda: NOTICES.extract_leading_comments(
            b"/* never closes", "broken.c"
        ),
        "未闭合",
    )
    (source_root / "missing.c").write_bytes(
        b"/* missing */\n#include \"absent.h\"\n"
    )
    expect_failure(
        lambda: NOTICES.collect_include_closure(
            temporary_root,
            {"deps/libarchive/libarchive/missing.c"},
            lambda _relative: None,
            set(),
        ),
        "新增缺失",
    )
    if NOTICES.collect_include_closure(
        temporary_root,
        {"deps/libarchive/libarchive/missing.c"},
        lambda _relative: None,
        {("deps/libarchive/libarchive/missing.c", "absent.h")},
    ) != {"deps/libarchive/libarchive/missing.c"}:
        fail("固定未解析 include 合同没有得到稳定闭包")


def test_fragment_uniqueness(temporary_root):
    path = temporary_root / "fragment.c"
    content = b"head\n/* locked */\ntail\n"
    path.write_bytes(content)
    fragment_bytes = b"/* locked */\n"
    fragment = SimpleNamespace(
        path="fragment.c",
        start_line=2,
        end_line=2,
        size=len(fragment_bytes),
        sha256=hashlib.sha256(fragment_bytes).hexdigest(),
    )
    if NOTICES.extract_fragment(temporary_root, fragment) != fragment_bytes:
        fail("固定闭区间没有返回原始片段字节")
    path.write_bytes(content + fragment_bytes)
    expect_failure(
        lambda: NOTICES.extract_fragment(temporary_root, fragment),
        "不是唯一字节序列",
    )
    expect_failure(
        lambda: NOTICES.require_closure_coverage(
            {"locked.c", "missing.h"}, {"locked.c"}, "合成许可输入"
        ),
        "missing.h",
    )


def test_output_drift_and_atomicity(temporary_root):
    output = temporary_root / "APPLE-HOST-NOTICES.txt"
    original = b"old\n"
    expected = b"new\n"
    output.write_bytes(original)
    expect_failure(
        lambda: NOTICES.check_output(output, expected),
        "重建结果不一致",
    )
    if output.read_bytes() != original:
        fail("只读输出漂移检查改写了旧文件")

    replace = NOTICES.os.replace

    def injected_failure(_source, _destination):
        raise OSError("注入替换失败")

    NOTICES.os.replace = injected_failure
    try:
        try:
            NOTICES.atomic_replace(output, expected)
        except OSError:
            pass
        else:
            fail("原子替换失败注入意外成功")
    finally:
        NOTICES.os.replace = replace
    if output.read_bytes() != original:
        fail("原子替换失败改写了旧宿主声明")
    if list(temporary_root.glob(".APPLE-HOST-NOTICES.txt.*.tmp")):
        fail("原子替换失败遗留临时文件")

    NOTICES.atomic_replace(output, expected)
    if output.read_bytes() != expected:
        fail("原子替换成功后输出字节错误")
    if list(temporary_root.glob(".APPLE-HOST-NOTICES.txt.*.tmp")):
        fail("原子替换成功后遗留临时文件")


def test_cli_check():
    result = subprocess.run(
        [
            sys.executable,
            "-B",
            str(GENERATOR),
            "check-locks",
            "--root",
            str(ROOT),
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        output = (result.stdout + result.stderr).decode(
            "utf-8", errors="replace"
        )
        fail(f"宿主声明 check-locks 命令失败：\n{output}")


def main():
    test_production_output()
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-host-notices."
    ) as temporary:
        temporary_root = Path(temporary)
        test_material_icon_formatting(temporary_root)
        test_lib_colors_replay()
        test_lib_colors_licenses()
        test_wcwidth_unicode_replay()
        test_libarchive_unicode_replay()
        test_leading_comments_and_include_closure(temporary_root)
        test_fragment_uniqueness(temporary_root)
        test_output_drift_and_atomicity(temporary_root)
    test_cli_check()
    print("Apple 宿主第三方声明生成回归通过")


if __name__ == "__main__":
    try:
        main()
    except (TestFailure, HostInputError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

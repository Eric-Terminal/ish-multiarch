#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from apple_host_manifest import HostInputError


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
    if NOTICES.EXPECTED_NOTICE_TEXT_COUNT != 122:
        fail("宿主声明唯一正文组数量合同漂移")
    begin_count = first.count(b"===== BEGIN APPLE HOST NOTICE:")
    if begin_count != 124 or begin_count != first.count(
        b"===== END APPLE HOST NOTICE:"
    ):
        fail("宿主声明 section marker 不平衡")
    if first.count(b"===== BEGIN APPLE HOST NOTICE: text-") != 122:
        fail("宿主声明正文 section 数量漂移")
    for identifier in ("overview", "unresolved-provenance"):
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
        "组件：libarchive 3.4.3\n",
    ):
        if metadata.encode("utf-8") not in first:
            fail(f"宿主声明缺少锁定版本元数据：{metadata.strip()}")
    for relative in (
        "deps/libapps/hterm/LICENSE",
        "deps/libapps/libdot/LICENSE",
        "deps/libapps/libdot/third_party/intl-segmenter/LICENSE.md",
        "deps/libapps/libdot/third_party/wcwidth/LICENSE.md",
        "deps/libarchive/COPYING",
    ):
        if (ROOT / relative).read_bytes() not in first:
            fail(f"宿主声明缺少完整许可文本：{relative}")
    if (ROOT / "deps/linux/COPYING").read_bytes() in first:
        fail("宿主声明错误收入 Linux COPYING 正文")

    license_rows = (
        ROOT / "third_party/apple-host/license-inputs.tsv"
    ).read_text(encoding="utf-8").splitlines()[1:]
    hterm_provenance = {
        fields[3]
        for fields in (row.split("\t") for row in license_rows)
        if fields[1:3] == ["hterm", "provenance"]
    }
    expected_hterm_provenance = {
        "deps/libapps/hterm/concat/hterm_resources.concat",
        *NOTICES.MATERIAL_ICON_PATHS,
    }
    if hterm_provenance != expected_hterm_provenance:
        fail("hterm Material 图标 provenance 输入集合漂移")

    for marker in (
        *NOTICES.MATERIAL_ICON_PATHS,
        NOTICES.MATERIAL_ICON_IMPORT_COMMIT,
        "google/material-design-icons",
    ):
        encoded = marker.encode("utf-8")
        if first.count(encoded) != 1:
            fail(f"宿主声明缺少唯一 Material 来源证据：{marker}")
    for marker in (
        "适用许可版本或权威许可原文",
        "不能据此推定 Apache 2.0",
        "公共发行前必须补齐这些证据",
    ):
        if marker.encode("utf-8") not in first:
            fail(f"宿主声明没有明确 Material 许可边界：{marker}")

    fragments = (
        ROOT / "third_party/apple-host/notice-fragments.tsv"
    ).read_text(encoding="utf-8").splitlines()[1:]
    if len(fragments) != 21:
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
        "将当前三张表标识为 Unicode 13.0.0",
        "生成脚本读取 PropList.txt、UnicodeData.txt 与 EastAsianWidth.txt",
        "仓内没有这三份 13.0.0 原始字节或相应 Unicode 许可原文",
        "组件：wcwidth 0.0.3（UCD 13.0.0 生成证据）",
        "deps/libapps/libdot/js/lib_colors.js:322-323",
        "deps/libapps/libdot/js/lib_colors.js:629-640",
        "deps/libapps/libdot/js/lib_colors.js:755-757",
        "HSL 算法改编自 W3C CSS Color 4",
        "颜色表派生自 stock X11 rgb.txt",
        "deps/libarchive/libarchive/archive_string.c:2797-2800",
        "deps/libarchive/libarchive/archive_string.c:3043-3046",
        "Unicode Standard Annex #15",
    ):
        if marker.encode("utf-8") not in first:
            fail(f"宿主声明缺少外部来源或生成证据：{marker}")

    output = ROOT / NOTICES.OUTPUT_RELATIVE
    NOTICES.check_output(output, first)


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

#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
GENERATOR = TOOLS / "apple-project-license-notices.py"
SPEC = importlib.util.spec_from_file_location(
    "apple_project_license_notices", GENERATOR
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载项目许可声明生成器")
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
    except NOTICES.ProjectLicenseError as error:
        if expected not in str(error):
            fail(f"负例没有命中预期错误“{expected}”：{error}")
    else:
        fail(f"负例意外成功：{expected}")


def section_raw(content, identifier):
    begin = (
        f"===== BEGIN PROJECT LICENSE NOTICE: {identifier} =====\n"
    ).encode("utf-8")
    end = (
        f"===== END PROJECT LICENSE NOTICE: {identifier} =====\n"
    ).encode("utf-8")
    if content.count(begin) != 1 or content.count(end) != 1:
        fail(f"项目许可声明 section marker 漂移：{identifier}")
    start = content.index(begin) + len(begin)
    finish = content.index(end, start)
    metadata, separator, raw = content[start:finish].partition(b"\n\n")
    if not separator or not metadata or not raw:
        fail(f"项目许可声明原文 section 边界错误：{identifier}")
    return raw


def test_production_output():
    first = NOTICES.build_notice(ROOT)
    second = NOTICES.build_notice(ROOT)
    if first != second:
        fail("相同项目许可输入没有生成逐字节一致的声明")
    if not first.startswith(
        b"===== BEGIN PROJECT LICENSE NOTICE: overview =====\n"
    ):
        fail("项目许可声明首行 marker 漂移")
    if (
        first.count(b"===== BEGIN PROJECT LICENSE NOTICE:") != 6
        or first.count(b"===== END PROJECT LICENSE NOTICE:") != 6
    ):
        fail("项目许可声明 section marker 数量或平衡关系漂移")
    if b"\r" in first or not first.endswith(b"\n") or first.endswith(b"\n\n"):
        fail("项目许可声明换行合同漂移")

    for identifier, relative in (
        ("license-md", "LICENSE.md"),
        ("license-ios", "LICENSE.IOS"),
        (
            "gpl-2.0",
            "distribution/apple/project-license/"
            "license-inputs/GPL-2.0.txt",
        ),
        (
            "gpl-3.0",
            "distribution/apple/project-license/"
            "license-inputs/GPL-3.0.txt",
        ),
    ):
        expected = (ROOT / relative).read_bytes()
        if section_raw(first, identifier) != expected:
            fail(f"项目许可声明没有逐字保留原文：{relative}")
        if first.count(expected) != 1:
            fail(f"项目许可原文没有唯一进入生成物：{relative}")

    for marker in (
        NOTICES.SOURCE_URL,
        "https://www.gnu.org/licenses/gpl-2.0.txt",
        "https://www.gnu.org/licenses/gpl-3.0.txt",
        "精确 release revision、主仓库与子模块源码资产及校验清单必须",
        "某个发布构建对应的精确 commit、tag 与源码资产仍须",
        "不替发行者选择许可路径，也不构成法律结论",
    ):
        encoded = marker.encode("utf-8")
        if first.count(encoded) != 1:
            fail(f"项目许可声明缺少唯一固定边界文本：{marker}")

    for row in NOTICES.EXPECTED_ROWS[1:]:
        digest = row[5].encode("ascii")
        if first.count(digest) != 1:
            fail(f"项目许可声明缺少唯一输入摘要：{row[0]}")

    output = NOTICES.output_path(ROOT)
    NOTICES.check_output(output, first)
    if stat.S_IMODE(output.stat().st_mode) != 0o644:
        fail("项目许可声明生成物模式不是 100644")


def copy_fixture(destination):
    for relative in (
        NOTICES.INPUTS_RELATIVE,
        Path("LICENSE.md"),
        Path("LICENSE.IOS"),
        Path(
            "distribution/apple/project-license/"
            "license-inputs/GPL-2.0.txt"
        ),
        Path(
            "distribution/apple/project-license/"
            "license-inputs/GPL-3.0.txt"
        ),
    ):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / relative).read_bytes())
    output = destination / NOTICES.OUTPUT_RELATIVE
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes((ROOT / NOTICES.OUTPUT_RELATIVE).read_bytes())
    output.chmod(0o644)
    return output


def replace_once(path, old, new):
    content = path.read_bytes()
    if content.count(old) != 1:
        fail(f"合成夹具无法唯一替换：{old!r}")
    path.write_bytes(content.replace(old, new))


def fixture_root(parent, name):
    root = parent / name
    root.mkdir()
    copy_fixture(root)
    return root


def test_input_failures(temporary_root):
    url_root = fixture_root(temporary_root, "url-drift")
    replace_once(
        url_root / NOTICES.INPUTS_RELATIVE,
        NOTICES.SOURCE_URL.encode("utf-8"),
        b"https://example.invalid/source",
    )
    expect_failure(
        lambda: NOTICES.build_notice(url_root),
        "公开源码仓库 URL 漂移",
    )

    authority_root = fixture_root(temporary_root, "authority-drift")
    replace_once(
        authority_root / NOTICES.INPUTS_RELATIVE,
        b"https://www.gnu.org/licenses/gpl-3.0.txt",
        b"https://example.invalid/gpl-3.0.txt",
    )
    expect_failure(
        lambda: NOTICES.build_notice(authority_root),
        "GNU 许可权威 URL 漂移：gpl-3.0",
    )

    hash_root = fixture_root(temporary_root, "hash-drift")
    replace_once(
        hash_root / NOTICES.INPUTS_RELATIVE,
        NOTICES.EXPECTED_ROWS[3][5].encode("ascii"),
        b"0" * 64,
    )
    expect_failure(
        lambda: NOTICES.build_notice(hash_root),
        "项目许可输入锁漂移：gpl-2.0",
    )

    missing_root = fixture_root(temporary_root, "missing-input")
    (missing_root / NOTICES.EXPECTED_ROWS[4][2]).unlink()
    expect_failure(
        lambda: NOTICES.build_notice(missing_root),
        "项目许可输入 gpl-3.0不存在",
    )

    content_root = fixture_root(temporary_root, "content-drift")
    license_ios = content_root / "LICENSE.IOS"
    license_ios.write_bytes(license_ios.read_bytes() + b"drift\n")
    expect_failure(
        lambda: NOTICES.build_notice(content_root),
        "项目许可输入大小或 SHA-256 不匹配：license-ios",
    )


def test_output_drift_and_atomicity(temporary_root):
    drift_root = fixture_root(temporary_root, "output-drift")
    drift_output = drift_root / NOTICES.OUTPUT_RELATIVE
    original = "保留旧项目许可声明\n".encode("utf-8")
    drift_output.write_bytes(original)
    expected = NOTICES.build_notice(drift_root)
    expect_failure(
        lambda: NOTICES.check_output(drift_output, expected),
        "重建结果不一致",
    )
    if drift_output.read_bytes() != original:
        fail("只读输出漂移检查改写了旧项目许可声明")

    validation_root = fixture_root(temporary_root, "validation-failure")
    validation_output = validation_root / NOTICES.OUTPUT_RELATIVE
    validation_output.write_bytes(original)
    replace_once(
        validation_root / NOTICES.INPUTS_RELATIVE,
        NOTICES.SOURCE_URL.encode("utf-8"),
        b"https://example.invalid/source",
    )
    expect_failure(
        lambda: NOTICES.main(
            ["render", "--root", str(validation_root)]
        ),
        "公开源码仓库 URL 漂移",
    )
    if validation_output.read_bytes() != original:
        fail("输入校验失败仍改写了旧项目许可声明")
    if list(validation_output.parent.glob(
        ".PROJECT-LICENSES.txt.*.tmp"
    )):
        fail("输入校验失败遗留项目许可声明临时文件")

    replace_root = fixture_root(temporary_root, "replace-failure")
    replace_output = replace_root / NOTICES.OUTPUT_RELATIVE
    replace_output.write_bytes(original)
    replacement = NOTICES.os.replace

    def injected_failure(_source, _destination):
        raise OSError("注入替换失败")

    NOTICES.os.replace = injected_failure
    try:
        try:
            NOTICES.atomic_replace(replace_output, expected)
        except OSError:
            pass
        else:
            fail("原子替换失败注入意外成功")
    finally:
        NOTICES.os.replace = replacement
    if replace_output.read_bytes() != original:
        fail("原子替换失败改写了旧项目许可声明")
    if list(replace_output.parent.glob(
        ".PROJECT-LICENSES.txt.*.tmp"
    )):
        fail("原子替换失败遗留项目许可声明临时文件")

    success_root = fixture_root(temporary_root, "render-success")
    success_output = success_root / NOTICES.OUTPUT_RELATIVE
    success_output.write_bytes(original)
    expected = NOTICES.build_notice(success_root)
    if NOTICES.main(["render", "--root", str(success_root)]) != 0:
        fail("项目许可声明显式 render 返回失败")
    if success_output.read_bytes() != expected:
        fail("项目许可声明显式 render 输出字节错误")
    if stat.S_IMODE(success_output.stat().st_mode) != 0o644:
        fail("项目许可声明显式 render 没有固定 100644 模式")
    if list(success_output.parent.glob(
        ".PROJECT-LICENSES.txt.*.tmp"
    )):
        fail("项目许可声明显式 render 成功后遗留临时文件")


def test_cli_check_is_read_only():
    output = ROOT / NOTICES.OUTPUT_RELATIVE
    before = output.read_bytes()
    before_mtime = output.stat().st_mtime_ns
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
        detail = (result.stdout + result.stderr).decode(
            "utf-8", errors="replace"
        )
        fail(f"项目许可声明 check-locks 命令失败：\n{detail}")
    if output.read_bytes() != before or output.stat().st_mtime_ns != before_mtime:
        fail("项目许可声明 check-locks 改写了生成物")


def main():
    test_production_output()
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-project-license."
    ) as temporary:
        temporary_root = Path(temporary)
        test_input_failures(temporary_root)
        test_output_drift_and_atomicity(temporary_root)
    test_cli_check_is_read_only()
    print("Apple 产品项目许可与源码入口声明生成回归通过")


if __name__ == "__main__":
    try:
        main()
    except (
        TestFailure,
        NOTICES.ProjectLicenseError,
        OSError,
        UnicodeError,
    ) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

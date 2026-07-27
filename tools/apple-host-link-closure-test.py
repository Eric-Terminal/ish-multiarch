#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys


TOOLS = Path(__file__).resolve().parent
VALIDATOR = TOOLS / "apple-host-link-closure.py"


def fail(message):
    raise ValueError(message)


def load_validator():
    spec = importlib.util.spec_from_file_location(
        "apple_host_link_closure", VALIDATOR
    )
    if spec is None or spec.loader is None:
        fail("无法加载 Apple 最终链接闭包校验器")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expect_failure(callback, expected):
    try:
        callback()
    except ValueError as error:
        if expected not in str(error):
            fail(f"负例命中错误诊断：{error}")
    else:
        fail(f"负例意外通过，期望错误：{expected}")


def map_with_members(members):
    records = [
        "# Path: /tmp/iSH.app/iSH",
        "# Arch: arm64",
        "# Object files:",
        "[  0] linker synthesized",
        "[  1] /tmp/libfakefs.a(fakefs.o)",
    ]
    records.extend(
        f"[ {index:2d}] /tmp/libarchive.a({member})"
        for index, member in enumerate(sorted(members), start=2)
    )
    records.extend(("# Sections:", "# Address\tSize\tSegment\tSection"))
    return "\n".join(records) + "\n"


def main():
    validator = load_validator()
    expected_members = set(validator.EXPECTED_ARCHIVE_MEMBERS)
    binary = Path("/tmp/iSH.app/iSH")
    valid_map = map_with_members(expected_members)
    validator.verify_link_map(valid_map, binary, {"arm64"})

    for member in sorted(expected_members):
        expect_failure(
            lambda member=member: validator.verify_link_map(
                map_with_members(expected_members - {member})
            ),
            "最终链接没有拉入 fakefs 必需的 libarchive 对象",
        )
    for member in (
        "archive_blake2s_ref.o",
        "archive_blake2sp_ref.o",
        "archive_blake2_future.o",
        "archive_blake2-neon.c.o",
        "archive_read_support_format_all.o",
        "archive_read_support_format_rar5.o",
    ):
        expect_failure(
            lambda member=member: validator.verify_link_map(
                map_with_members(expected_members | {member})
            ),
            "最终链接意外拉入 BLAKE2 或 RAR5 对象",
        )
    expect_failure(
        lambda: validator.verify_link_map("# Object files:\n"),
        "LinkMap 必须包含唯一产品路径",
    )
    expect_failure(
        lambda: validator.verify_link_map(
            valid_map, Path("/tmp/Other.app/Other"), {"arm64"}
        ),
        "LinkMap 产品路径与待检 Mach-O 不一致",
    )
    expect_failure(
        lambda: validator.verify_link_map(
            valid_map, binary, {"x86_64"}
        ),
        "LinkMap 架构与待检 Mach-O 不一致",
    )

    valid_symbols = "\n".join(sorted(validator.EXPECTED_SYMBOLS)) + "\n"
    validator.verify_symbols(valid_symbols)
    for symbol in sorted(validator.EXPECTED_SYMBOLS):
        expect_failure(
            lambda symbol=symbol: validator.verify_symbols(
                "\n".join(sorted(validator.EXPECTED_SYMBOLS - {symbol})) + "\n"
            ),
            "最终 Mach-O 缺少 fakefs 必需的 libarchive 符号",
        )
    for symbol in (
        "blake2sp_init",
        "_blake2sp_init",
        "_archive_read_support_format_all",
        "_archive_read_support_format_rar5",
    ):
        expect_failure(
            lambda symbol=symbol: validator.verify_symbols(
                valid_symbols + symbol + "\n"
            ),
            "最终 Mach-O 意外包含 BLAKE2 或 RAR5 符号",
        )
    print("Apple 成品 libarchive 最终链接闭包合成回归通过")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

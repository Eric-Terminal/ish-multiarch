#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


EXPECTED_ARCHIVE_MEMBERS = {
    "archive_read_support_filter_gzip.o",
    "archive_read_support_format_tar.o",
    "archive_write_add_filter_gzip.o",
    "archive_write_set_format_pax.o",
}
FORBIDDEN_ARCHIVE_MEMBER_PATTERNS = (
    re.compile(r"^archive_blake2.*\.o$"),
    re.compile(r"^archive_read_support_format_(?:all|rar5)\.o$"),
)
EXPECTED_SYMBOLS = {
    "_archive_read_support_filter_gzip",
    "_archive_read_support_format_tar",
    "_archive_write_add_filter_gzip",
    "_archive_write_set_format_pax",
}
FORBIDDEN_SYMBOL_PATTERNS = (
    re.compile(r"^_?blake2[A-Za-z0-9_]*$"),
    re.compile(r"^_archive_read_support_format_(?:all|rar5)$"),
)
OBJECT_ENTRY = re.compile(r"^\[\s*\d+\]\s+(.+?)\s*$")
LIBARCHIVE_MEMBER = re.compile(r"(?:^|/)libarchive\.a\(([^()]+)\)$")


def fail(message):
    raise ValueError(message)


def read_regular(path, description, *, binary=False):
    try:
        metadata = path.lstat()
    except OSError as error:
        fail(f"无法读取{description}：{error}")
    if not stat.S_ISREG(metadata.st_mode):
        fail(f"{description}必须是非符号链接常规文件：{path}")
    if metadata.st_size > 64 * 1024 * 1024:
        fail(f"{description}超过 64 MiB 上限：{path}")
    data = path.read_bytes()
    if binary:
        return data
    # LinkMap 的符号区会原样写入二进制常量；对象列表仍是 ASCII。
    return data.decode("utf-8", errors="surrogateescape")


def link_map_members(content):
    lines = content.splitlines()
    paths = [
        line[len("# Path: "):]
        for line in lines
        if line.startswith("# Path: ")
    ]
    architectures = [
        line[len("# Arch: "):]
        for line in lines
        if line.startswith("# Arch: ")
    ]
    if len(paths) != 1 or not paths[0]:
        fail("LinkMap 必须包含唯一产品路径")
    if len(architectures) != 1 or not architectures[0]:
        fail("LinkMap 必须包含唯一产品架构")
    starts = [
        index for index, line in enumerate(lines)
        if line.strip() == "# Object files:"
    ]
    if len(starts) != 1:
        fail("LinkMap 必须包含唯一 Object files 段")
    start = starts[0] + 1
    end = next(
        (
            index for index in range(start, len(lines))
            if lines[index].startswith("# Sections:")
        ),
        None,
    )
    if end is None:
        fail("LinkMap 缺少紧随对象列表的 Sections 段")
    members = set()
    for line in lines[start:end]:
        match = OBJECT_ENTRY.fullmatch(line)
        if match is None:
            if line.strip():
                fail(f"LinkMap 对象记录格式非法：{line}")
            continue
        archive_match = LIBARCHIVE_MEMBER.search(match.group(1))
        if archive_match is not None:
            members.add(Path(archive_match.group(1)).name)
    return Path(paths[0]), architectures[0], members


def verify_link_map(content, binary=None, architectures=None):
    mapped_binary, mapped_architecture, members = link_map_members(content)
    if binary is not None and (
        mapped_binary.resolve(strict=False) != binary.resolve(strict=False)
    ):
        fail("LinkMap 产品路径与待检 Mach-O 不一致")
    if architectures is not None and {mapped_architecture} != set(architectures):
        fail("LinkMap 架构与待检 Mach-O 不一致")
    missing = EXPECTED_ARCHIVE_MEMBERS - members
    if missing:
        fail(
            "最终链接没有拉入 fakefs 必需的 libarchive 对象："
            + ", ".join(sorted(missing))
        )
    forbidden = sorted(
        member
        for member in members
        if any(
            pattern.fullmatch(member)
            for pattern in FORBIDDEN_ARCHIVE_MEMBER_PATTERNS
        )
    )
    if forbidden:
        fail(
            "最终链接意外拉入 BLAKE2 或 RAR5 对象："
            + ", ".join(forbidden)
        )


def verify_symbols(content):
    symbols = {
        line.strip()
        for line in content.splitlines()
        if line.strip() and not line.strip().endswith(":")
    }
    missing = EXPECTED_SYMBOLS - symbols
    if missing:
        fail(
            "最终 Mach-O 缺少 fakefs 必需的 libarchive 符号："
            + ", ".join(sorted(missing))
        )
    forbidden = sorted(
        symbol
        for symbol in symbols
        if any(pattern.fullmatch(symbol) for pattern in FORBIDDEN_SYMBOL_PATTERNS)
    )
    if forbidden:
        fail(
            "最终 Mach-O 意外包含 BLAKE2 或 RAR5 符号："
            + ", ".join(forbidden)
        )


def read_symbols(binary):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    result = subprocess.run(
        ["xcrun", "nm", "-gj", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        fail(f"无法读取最终 Mach-O 符号：{detail or '未知错误'}")
    try:
        return result.stdout.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        fail(f"最终 Mach-O 符号不是 UTF-8：{error}")


def read_architectures(binary):
    result = subprocess.run(
        ["xcrun", "lipo", "-archs", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        fail(f"无法读取最终 Mach-O 架构：{detail or '未知错误'}")
    architectures = result.stdout.decode(
        "ascii", errors="strict"
    ).strip().split()
    if not architectures or len(architectures) != len(set(architectures)):
        fail("最终 Mach-O 架构列表为空或重复")
    return set(architectures)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="校验 Apple 成品只拉入 fakefs 所需的 libarchive 链接闭包。"
    )
    parser.add_argument("binary", type=Path, help="最终 App Mach-O")
    parser.add_argument("link_map", type=Path, help="最终链接生成的 LinkMap")
    arguments = parser.parse_args(argv)
    link_map = read_regular(arguments.link_map, "Apple 产品 LinkMap")
    read_regular(arguments.binary, "Apple 产品 Mach-O", binary=True)
    architectures = read_architectures(arguments.binary)
    verify_link_map(link_map, arguments.binary, architectures)
    verify_symbols(read_symbols(arguments.binary))
    print("Apple 成品 libarchive 最终链接闭包校验通过")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

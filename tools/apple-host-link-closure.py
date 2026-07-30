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
    "archive_acl.o",
    "archive_check_magic.o",
    "archive_cryptor.o",
    "archive_digest.o",
    "archive_entry.o",
    "archive_entry_link_resolver.o",
    "archive_entry_sparse.o",
    "archive_entry_xattr.o",
    "archive_hmac.o",
    "archive_ppmd7.o",
    "archive_random.o",
    "archive_rb.o",
    "archive_read.o",
    "archive_read_open_filename.o",
    "archive_read_support_filter_gzip.o",
    "archive_read_support_format_tar.o",
    "archive_string.o",
    "archive_string_sprintf.o",
    "archive_util.o",
    "archive_virtual.o",
    "archive_write.o",
    "archive_write_add_filter_gzip.o",
    "archive_write_open_fd.o",
    "archive_write_open_filename.o",
    "archive_write_set_format.o",
    "archive_write_set_format_7zip.o",
    "archive_write_set_format_cpio.o",
    "archive_write_set_format_cpio_newc.o",
    "archive_write_set_format_gnutar.o",
    "archive_write_set_format_iso9660.o",
    "archive_write_set_format_mtree.o",
    "archive_write_set_format_pax.o",
    "archive_write_set_format_raw.o",
    "archive_write_set_format_shar.o",
    "archive_write_set_format_ustar.o",
    "archive_write_set_format_warc.o",
    "archive_write_set_format_xar.o",
    "archive_write_set_format_zip.o",
    "archive_write_set_passphrase.o",
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
LIBARCHIVE_MEMBER = re.compile(
    r"(?:^|/)(?:libarchive|libarchive-watchOS)\.a\(([^()]+)\)$"
)


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


def is_xcode_architecture_binary(mapped_binary, binary, architecture):
    return (
        mapped_binary.name == binary.name
        and mapped_binary.parent.name == "Binary"
        and mapped_binary.parent.parent.name == architecture
        and mapped_binary.parent.parent.parent.name == "Objects-normal"
        and mapped_binary.parent.parent.parent.parent.name.endswith(".build")
    )


def verify_link_map(
    content,
    binary=None,
    architecture=None,
    *,
    allow_xcode_architecture_binary=False,
):
    mapped_binary, mapped_architecture, members = link_map_members(content)
    if binary is not None:
        same_binary = (
            mapped_binary.resolve(strict=False) == binary.resolve(strict=False)
        )
        if (
            not same_binary
            and not (
                allow_xcode_architecture_binary
                and architecture is not None
                and is_xcode_architecture_binary(
                    mapped_binary.resolve(strict=False),
                    binary.resolve(strict=False),
                    architecture,
                )
            )
        ):
            fail("LinkMap 产品路径与待检 Mach-O 不一致")
    if architecture is not None and mapped_architecture != architecture:
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
    unexpected = members - EXPECTED_ARCHIVE_MEMBERS
    if unexpected:
        fail(
            "最终链接的 libarchive 传递闭包发生漂移："
            + ", ".join(sorted(unexpected))
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


def read_symbols(binary, architecture, *, runner=subprocess.run):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    result = runner(
        ["xcrun", "nm", "-arch", architecture, "-gj", str(binary)],
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


def read_architectures(binary, *, runner=subprocess.run):
    result = runner(
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


def main(argv=None, *, runner=subprocess.run):
    parser = argparse.ArgumentParser(
        description="校验 Apple 成品只拉入 fakefs 所需的 libarchive 链接闭包。"
    )
    parser.add_argument(
        "--arch",
        help="仅校验胖 Mach-O 的指定架构切片；架构必须存在于产品中",
    )
    parser.add_argument("binary", type=Path, help="最终 App Mach-O")
    parser.add_argument("link_map", type=Path, help="最终链接生成的 LinkMap")
    arguments = parser.parse_args(argv)
    link_map = read_regular(arguments.link_map, "Apple 产品 LinkMap")
    read_regular(arguments.binary, "Apple 产品 Mach-O", binary=True)
    architectures = read_architectures(arguments.binary, runner=runner)
    if arguments.arch is None:
        if len(architectures) != 1:
            fail("胖 Mach-O 必须通过 --arch 指定与 LinkMap 对应的架构")
        architecture = next(iter(architectures))
    else:
        architecture = arguments.arch
        if architecture not in architectures:
            fail(f"所选架构不属于待检 Mach-O：{architecture}")
    verify_link_map(
        link_map,
        arguments.binary,
        architecture,
        allow_xcode_architecture_binary=(
            arguments.arch is not None and len(architectures) > 1
        ),
    )
    verify_symbols(
        read_symbols(arguments.binary, architecture, runner=runner)
    )
    print("Apple 成品 libarchive 最终链接闭包校验通过")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

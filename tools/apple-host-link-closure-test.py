#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


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


def map_with_members(
    members,
    archive_name="libarchive.a",
    *,
    binary=Path("/tmp/iSH.app/iSH"),
    architecture="arm64",
):
    records = [
        f"# Path: {binary}",
        f"# Arch: {architecture}",
        "# Object files:",
        "[  0] linker synthesized",
        "[  1] /tmp/libfakefs.a(fakefs.o)",
    ]
    records.extend(
        f"[ {index:2d}] /tmp/{archive_name}({member})"
        for index, member in enumerate(sorted(members), start=2)
    )
    records.extend(("# Sections:", "# Address\tSize\tSegment\tSection"))
    return "\n".join(records) + "\n"


class FakeAppleTools:
    def __init__(self, architectures, symbols_by_architecture):
        self.architectures = tuple(architectures)
        self.symbols_by_architecture = symbols_by_architecture
        self.commands = []

    def run(self, command, **_kwargs):
        self.commands.append(command)
        if command[:3] == ["xcrun", "lipo", "-archs"]:
            output = " ".join(self.architectures) + "\n"
            return subprocess.CompletedProcess(
                command, 0, stdout=output.encode("ascii"), stderr=b""
            )
        if command[:2] == ["xcrun", "nm"]:
            if command[2:3] != ["-arch"] or len(command) < 6:
                return subprocess.CompletedProcess(
                    command,
                    1,
                    stdout=b"",
                    stderr="nm 缺少显式 -arch".encode("utf-8"),
                )
            architecture = command[3]
            symbols = self.symbols_by_architecture.get(architecture)
            if symbols is None:
                return subprocess.CompletedProcess(
                    command,
                    1,
                    stdout=b"",
                    stderr=f"未知架构：{architecture}".encode("utf-8"),
                )
            return subprocess.CompletedProcess(
                command, 0, stdout=symbols.encode("utf-8"), stderr=b""
            )
        return subprocess.CompletedProcess(
            command,
            1,
            stdout=b"",
            stderr="未预期的工具调用".encode("utf-8"),
        )


def verify_architecture_selection(validator, expected_members, valid_symbols):
    with tempfile.TemporaryDirectory() as temporary_directory:
        directory = Path(temporary_directory)
        binary = directory / "iSHWatch"
        link_map = directory / "iSHWatch.map"
        binary.write_bytes(b"synthetic Mach-O")

        thin_tools = FakeAppleTools(("arm64",), {"arm64": valid_symbols})
        link_map.write_text(
            map_with_members(expected_members, binary=binary),
            encoding="utf-8",
        )
        validator.main(
            [str(binary), str(link_map)],
            runner=thin_tools.run,
        )
        if thin_tools.commands[-1] != [
            "xcrun",
            "nm",
            "-arch",
            "arm64",
            "-gj",
            str(binary),
        ]:
            fail("薄产品的 nm 没有显式选择唯一架构")

        fat_symbols = {
            "arm64": valid_symbols,
            "arm64_32": valid_symbols,
        }
        for architecture in ("arm64", "arm64_32"):
            tools = FakeAppleTools(("arm64", "arm64_32"), fat_symbols)
            architecture_binary = (
                directory
                / "iSHWatch.build"
                / "Objects-normal"
                / architecture
                / "Binary"
                / binary.name
            )
            link_map.write_text(
                map_with_members(
                    expected_members,
                    binary=architecture_binary,
                    architecture=architecture,
                ),
                encoding="utf-8",
            )
            validator.main(
                [
                    "--arch",
                    architecture,
                    str(binary),
                    str(link_map),
                ],
                runner=tools.run,
            )
            if tools.commands[-1][2:4] != ["-arch", architecture]:
                fail(f"胖产品的 nm 没有选择 {architecture} 切片")

        invalid_xcode_paths = (
            (
                directory
                / "iSHWatch.build"
                / "Objects-normal"
                / "x86_64"
                / "Binary"
                / binary.name
            ),
            (
                directory
                / "iSHWatch.build"
                / "Objects-normal"
                / "arm64"
                / "Binary"
                / "Other"
            ),
            directory / "arbitrary" / "arm64" / "Binary" / binary.name,
        )
        for mapped_binary in invalid_xcode_paths:
            tools = FakeAppleTools(("arm64", "arm64_32"), fat_symbols)
            link_map.write_text(
                map_with_members(
                    expected_members,
                    binary=mapped_binary,
                    architecture="arm64",
                ),
                encoding="utf-8",
            )
            expect_failure(
                lambda tools=tools: validator.main(
                    [
                        "--arch",
                        "arm64",
                        str(binary),
                        str(link_map),
                    ],
                    runner=tools.run,
                ),
                "LinkMap 产品路径与待检 Mach-O 不一致",
            )
            if any(command[1:2] == ["nm"] for command in tools.commands):
                fail("LinkMap 切片路径不匹配时不应继续读取符号")

        thin_slice_tools = FakeAppleTools(
            ("arm64",), {"arm64": valid_symbols}
        )
        thin_slice_binary = (
            directory
            / "iSHWatch.build"
            / "Objects-normal"
            / "arm64"
            / "Binary"
            / binary.name
        )
        link_map.write_text(
            map_with_members(
                expected_members,
                binary=thin_slice_binary,
                architecture="arm64",
            ),
            encoding="utf-8",
        )
        expect_failure(
            lambda: validator.main(
                [
                    "--arch",
                    "arm64",
                    str(binary),
                    str(link_map),
                ],
                runner=thin_slice_tools.run,
            ),
            "LinkMap 产品路径与待检 Mach-O 不一致",
        )

        fat_tools = FakeAppleTools(("arm64", "arm64_32"), fat_symbols)
        expect_failure(
            lambda: validator.main(
                [str(binary), str(link_map)],
                runner=fat_tools.run,
            ),
            "胖 Mach-O 必须通过 --arch",
        )
        if any(command[1:2] == ["nm"] for command in fat_tools.commands):
            fail("未选择胖产品架构时不应继续读取符号")

        absent_tools = FakeAppleTools(("arm64", "arm64_32"), fat_symbols)
        expect_failure(
            lambda: validator.main(
                [
                    "--arch",
                    "x86_64",
                    str(binary),
                    str(link_map),
                ],
                runner=absent_tools.run,
            ),
            "所选架构不属于待检 Mach-O",
        )
        if any(command[1:2] == ["nm"] for command in absent_tools.commands):
            fail("所选架构不存在时不应继续读取符号")

        mismatch_tools = FakeAppleTools(
            ("arm64", "arm64_32"), fat_symbols
        )
        link_map.write_text(
            map_with_members(
                expected_members,
                binary=binary,
                architecture="arm64",
            ),
            encoding="utf-8",
        )
        expect_failure(
            lambda: validator.main(
                [
                    "--arch",
                    "arm64_32",
                    str(binary),
                    str(link_map),
                ],
                runner=mismatch_tools.run,
            ),
            "LinkMap 架构与待检 Mach-O 不一致",
        )
        if any(command[1:2] == ["nm"] for command in mismatch_tools.commands):
            fail("LinkMap 架构不匹配时不应继续读取符号")


def main():
    validator = load_validator()
    expected_members = set(validator.EXPECTED_ARCHIVE_MEMBERS)
    binary = Path("/tmp/iSH.app/iSH")
    valid_map = map_with_members(expected_members)
    validator.verify_link_map(valid_map, binary, "arm64")
    validator.verify_link_map(
        map_with_members(expected_members, "libarchive-watchOS.a"),
        binary,
        "arm64",
    )

    for archive_name in (
        "libarchive-watchos.a",
        "libarchive-watchOS-debug.a",
        "libarchive-watchOS.a.backup",
        "not-libarchive-watchOS.a",
    ):
        expect_failure(
            lambda archive_name=archive_name: validator.verify_link_map(
                map_with_members(expected_members, archive_name)
            ),
            "最终链接没有拉入 fakefs 必需的 libarchive 对象",
        )

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
        lambda: validator.verify_link_map(
            map_with_members(expected_members | {"archive_future.o"})
        ),
        "最终链接的 libarchive 传递闭包发生漂移",
    )
    expect_failure(
        lambda: validator.verify_link_map("# Object files:\n"),
        "LinkMap 必须包含唯一产品路径",
    )
    expect_failure(
        lambda: validator.verify_link_map(
            valid_map, Path("/tmp/Other.app/Other"), "arm64"
        ),
        "LinkMap 产品路径与待检 Mach-O 不一致",
    )
    expect_failure(
        lambda: validator.verify_link_map(
            valid_map, binary, "x86_64"
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
    verify_architecture_selection(
        validator,
        expected_members,
        valid_symbols,
    )
    print("Apple 成品 libarchive 最终链接闭包合成回归通过")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

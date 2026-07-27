#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import tempfile
from typing import Optional


ROOT = Path(__file__).resolve().parent.parent
INPUTS_RELATIVE = PurePosixPath(
    "distribution/apple/project-license/inputs.tsv"
)
OUTPUT_RELATIVE = PurePosixPath(
    "distribution/apple/project-license/PROJECT-LICENSES.txt"
)
SOURCE_URL = "https://github.com/Eric-Terminal/ish-multiarch"
INPUT_HEADER = "section\trole\tpath\tsource_url\tsize\tsha256"
EXPECTED_ROWS = (
    (
        "source",
        "source",
        "-",
        SOURCE_URL,
        "-",
        "-",
    ),
    (
        "license-md",
        "terms",
        "LICENSE.md",
        "-",
        "1731",
        "300e62ce6fafbee38b072ab7daf8d64bd57042d81e09d3b4c58af26bcabddab1",
    ),
    (
        "license-ios",
        "additional-terms",
        "LICENSE.IOS",
        "-",
        "782",
        "37708e19d5ea72c1491926ce6d33ae33ab746609b772a4f1b1657a809f835958",
    ),
    (
        "gpl-2.0",
        "license-text",
        "distribution/apple/project-license/license-inputs/GPL-2.0.txt",
        "https://www.gnu.org/licenses/gpl-2.0.txt",
        "17984",
        "edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6",
    ),
    (
        "gpl-3.0",
        "license-text",
        "distribution/apple/project-license/license-inputs/GPL-3.0.txt",
        "https://www.gnu.org/licenses/gpl-3.0.txt",
        "35149",
        "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986",
    ),
)


class ProjectLicenseError(ValueError):
    pass


@dataclass(frozen=True)
class InputLock:
    section: str
    role: str
    path: Optional[str]
    source_url: Optional[str]
    size: Optional[int]
    sha256: Optional[str]


def fail(message):
    raise ProjectLicenseError(message)


def validate_relative_path(value, description):
    parts = value.split("/")
    if (
        not value
        or value.startswith("/")
        or "\\" in value
        or any(part in {"", ".", ".."} for part in parts)
    ):
        fail(f"{description}不是规范仓库相对路径：{value}")
    return PurePosixPath(value)


def regular_path(root, relative, description):
    current = root
    parts = relative.parts
    for index, part in enumerate(parts):
        current = current / part
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            fail(f"{description}不存在：{relative}")
        except OSError as error:
            fail(f"无法检查{description}：{relative}（{error}）")
        if stat.S_ISLNK(metadata.st_mode):
            fail(f"{description}不能经过符号链接：{relative}")
        if index + 1 < len(parts):
            if not stat.S_ISDIR(metadata.st_mode):
                fail(f"{description}的父路径不是目录：{relative}")
        elif not stat.S_ISREG(metadata.st_mode):
            fail(f"{description}必须是常规文件：{relative}")
    return current


def read_regular(root, relative, description):
    path = regular_path(root, relative, description)
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"无法读取{description}：{relative}（{error}）")


def parse_inputs(root):
    raw = read_regular(root, INPUTS_RELATIVE, "项目许可输入锁")
    if not raw.endswith(b"\n") or b"\r" in raw:
        fail("项目许可输入锁必须只使用 LF，并以单个 LF 结尾")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"项目许可输入锁不是 UTF-8：{error}")
    lines = text[:-1].split("\n")
    if not lines or lines[0] != INPUT_HEADER:
        fail("项目许可输入锁表头漂移")
    if any(not line for line in lines[1:]):
        fail("项目许可输入锁含空行")

    raw_rows = []
    seen = set()
    for line in lines[1:]:
        fields = tuple(line.split("\t"))
        if len(fields) != 6 or any(not field for field in fields):
            fail("项目许可输入锁含空字段或列数错误")
        section, role, path, source_url, size, digest = fields
        if section in seen:
            fail(f"项目许可输入锁 section 重复：{section}")
        seen.add(section)
        raw_rows.append(fields)

        if role == "source":
            if path != "-" or size != "-" or digest != "-":
                fail("公开源码入口不得冒充文件输入")
        else:
            validate_relative_path(path, f"{section} 路径")
            if (
                not size.isdigit()
                or size.startswith("0")
                or len(digest) != 64
                or any(character not in "0123456789abcdef"
                       for character in digest)
            ):
                fail(f"项目许可输入大小或 SHA-256 格式错误：{section}")
        if source_url != "-" and not source_url.startswith("https://"):
            fail(f"项目许可输入 URL 不是 HTTPS：{section}")

    expected_sections = [row[0] for row in EXPECTED_ROWS]
    if [row[0] for row in raw_rows] != expected_sections:
        fail("项目许可输入 section 集合或顺序漂移")
    for actual, expected in zip(raw_rows, EXPECTED_ROWS):
        section = expected[0]
        if actual[3] != expected[3]:
            if section == "source":
                fail("公开源码仓库 URL 漂移")
            if section in {"gpl-2.0", "gpl-3.0"}:
                fail(f"GNU 许可权威 URL 漂移：{section}")
        if actual != expected:
            fail(f"项目许可输入锁漂移：{section}")

    locks = {}
    for section, role, path, source_url, size, digest in raw_rows:
        locks[section] = InputLock(
            section,
            role,
            None if path == "-" else path,
            None if source_url == "-" else source_url,
            None if size == "-" else int(size),
            None if digest == "-" else digest,
        )
    return locks


def read_locked_inputs(root, locks):
    contents = {}
    for section in ("license-md", "license-ios", "gpl-2.0", "gpl-3.0"):
        item = locks[section]
        assert item.path is not None
        assert item.size is not None
        assert item.sha256 is not None
        content = read_regular(
            root,
            PurePosixPath(item.path),
            f"项目许可输入 {section}",
        )
        if (
            len(content) != item.size
            or hashlib.sha256(content).hexdigest() != item.sha256
        ):
            fail(f"项目许可输入大小或 SHA-256 不匹配：{section}")
        if not content.endswith(b"\n") or b"\r" in content:
            fail(f"项目许可输入必须只使用 LF，并以 LF 结尾：{section}")
        contents[section] = content
    return contents


def render_section(identifier, metadata, raw=None):
    begin = (
        f"===== BEGIN PROJECT LICENSE NOTICE: {identifier} =====\n"
    ).encode("utf-8")
    end = (
        f"===== END PROJECT LICENSE NOTICE: {identifier} =====\n"
    ).encode("utf-8")
    body = "\n".join(metadata).encode("utf-8") + b"\n"
    if raw is not None:
        body += b"\n" + raw
    return begin + body + end


def build_notice(root):
    locks = parse_inputs(root)
    contents = read_locked_inputs(root, locks)
    sections = [
        render_section(
            "overview",
            (
                "项目许可与公开源码入口",
                "",
                "覆盖范围：本文件逐字收入仓库根 LICENSE.md、LICENSE.IOS，"
                "以及两份根许可说明明确引用的 GNU GPLv2、GPLv3 完整原文。",
                "",
                "工程边界：收入两种 GPL 原文不替发行者选择许可路径，也不构成"
                "法律结论。Linux kernel、Alpine guest 与 Apple 宿主第三方组件"
                "仍由各自的发行门禁处理。",
                "",
                "精确 release revision、主仓库与子模块源码资产及校验清单必须"
                "由后续独立 Release 门禁绑定；本文件不把可变分支当成某个"
                "二进制的精确对应源码证据。",
            ),
        ),
        render_section(
            "source",
            (
                "公开源码仓库",
                "",
                f"规范入口：{SOURCE_URL}",
                "",
                "本节只记录项目源码入口。某个发布构建对应的精确 commit、tag "
                "与源码资产仍须由该次 Release 单独绑定和验证。",
            ),
        ),
        render_section(
            "license-md",
            (
                "项目根许可说明",
                "来源：LICENSE.md",
                f"原始文本 SHA-256：{locks['license-md'].sha256}",
            ),
            contents["license-md"],
        ),
        render_section(
            "license-ios",
            (
                "Apple 分发附加条款",
                "来源：LICENSE.IOS",
                f"原始文本 SHA-256：{locks['license-ios'].sha256}",
            ),
            contents["license-ios"],
        ),
        render_section(
            "gpl-2.0",
            (
                "GNU General Public License version 2 原文",
                f"权威 URL：{locks['gpl-2.0'].source_url}",
                f"原始文本 SHA-256：{locks['gpl-2.0'].sha256}",
            ),
            contents["gpl-2.0"],
        ),
        render_section(
            "gpl-3.0",
            (
                "GNU General Public License version 3 原文",
                f"权威 URL：{locks['gpl-3.0'].source_url}",
                f"原始文本 SHA-256：{locks['gpl-3.0'].sha256}",
            ),
            contents["gpl-3.0"],
        ),
    ]
    return b"\n".join(sections)


def output_path(root):
    parent = root
    for part in OUTPUT_RELATIVE.parent.parts:
        parent = parent / part
        try:
            metadata = parent.lstat()
        except OSError as error:
            fail(f"项目许可声明输出目录不存在：{parent}（{error}）")
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            fail(f"项目许可声明输出目录必须是非符号链接目录：{parent}")
    path = root / OUTPUT_RELATIVE
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return path
    except OSError as error:
        fail(f"无法检查项目许可声明输出：{error}")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        fail("项目许可声明输出必须不存在或为常规文件")
    if stat.S_IMODE(metadata.st_mode) != 0o644:
        fail("项目许可声明输出必须使用 100644 模式")
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
        fail(f"无法读取项目许可声明生成物：{error}")
    if actual != expected:
        fail("PROJECT-LICENSES.txt 与锁定输入重建结果不一致；请显式 render")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="生成或只读校验 Apple 产品项目许可与源码入口声明。"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command, help_text in (
        ("check-locks", "只读重建并比对项目许可声明"),
        ("render", "显式原子写入项目许可声明"),
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
        print("Apple 产品项目许可与源码入口声明校验通过")
    elif arguments.command == "render":
        atomic_replace(path, content)
        print(f"已生成 Apple 产品项目许可与源码入口声明：{path}")
    else:
        fail("未知命令")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ProjectLicenseError, OSError, UnicodeError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)

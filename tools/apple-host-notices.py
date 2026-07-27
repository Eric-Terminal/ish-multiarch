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
import sys
import tempfile

from apple_host_manifest import HostInputError, fail, read_regular


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
EXPECTED_NOTICE_TEXT_COUNT = 122
MATERIAL_ICON_IMPORT_COMMIT = "de5387e902ef285c2d2c6909a53d37d826843551"
MATERIAL_ICON_PATHS = (
    "deps/libapps/hterm/images/close.svg",
    "deps/libapps/hterm/images/keyboard_arrow_down.svg",
    "deps/libapps/hterm/images/keyboard_arrow_up.svg",
)
WCWIDTH_UCD_EVIDENCE_COMMIT = "6b9f6ee9b9c94cfa4e3adf049c906610d1623ee8"
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


def overview():
    return (
        "===== BEGIN APPLE HOST NOTICE: overview =====\n"
        "Apple 宿主第三方声明\n"
        "\n"
        "覆盖范围：本文件汇集普通 iSH 与可选 iSH+Linux 共同交付的 "
        "hterm 生成资源及 libarchive 静态库的锁定原始文本，并在下一节"
        "列出尚未闭合的外部来源。\n"
        "\n"
        "未决边界：\n"
        "- Linux kernel、在线 rootfs 及其许可与对应源码不在本文件内；"
        "本文件不能代表 iSH+Linux 的完整发行义务已经闭合。\n"
        "- Alpine guest seed、项目自身许可、LGPL 与对应源码交付继续由"
        "各自的发行门禁处理。\n"
        "- libarchive 的 BLAKE2 声明列出 CC0 1.0 Universal、OpenSSL "
        "与 Apache 2.0 三种选项；这里只逐字收录，不替发行者选择分支。\n"
        "- Material 图标、Unicode 数据、W3C 与 X11 来源的具体缺口见"
        "“未闭合的外部来源与许可”一节；该节是工程审计记录，不是法律结论。\n"
        "- public-domain 字样仅转述锁定上游源码，不是本工具作出的法律"
        "判断；libarchive/COPYING 也只是上游汇总，具体源码文本仍有控制力。\n"
        "===== END APPLE HOST NOTICE: overview =====\n"
        "\n"
    ).encode("utf-8")


def audit_boundaries(inputs_by_path):
    icon_lines = []
    for relative in MATERIAL_ICON_PATHS:
        item = inputs_by_path.get(relative)
        if (
            item is None
            or item.component != "hterm"
            or item.role != "provenance"
        ):
            fail(f"Material 图标来源输入缺失：{relative}")
        icon_lines.append(
            f"- {relative}；{item.size} 字节；SHA-256 {item.sha256}"
        )

    return (
        "===== BEGIN APPLE HOST NOTICE: unresolved-provenance =====\n"
        "未闭合的外部来源与许可\n"
        "\n"
        "Material Design 图标：锁定 libapps 历史提交 "
        f"{MATERIAL_ICON_IMPORT_COMMIT} 记录下列三个 hterm find bar SVG "
        "取自 google/material-design-icons；当前锁定仓内没有对应上游 "
        "revision、上游路径、适用许可版本或权威许可原文，不能据此推定 "
        "Apache 2.0。公共发行前必须补齐这些证据：\n"
        + "\n".join(icon_lines)
        + "\n\n"
        "wcwidth Unicode 数据：完整 lib_wc.js 摘要与原始来源注释已锁定；"
        "本地历史提交 "
        f"{WCWIDTH_UCD_EVIDENCE_COMMIT} 将当前三张表标识为 Unicode "
        "13.0.0。生成脚本读取 PropList.txt、UnicodeData.txt 与 "
        "EastAsianWidth.txt，但仓内没有这三份 13.0.0 原始字节或相应 "
        "Unicode 许可原文，因此生成数据闭包尚未完成。\n"
        "\n"
        "libdot 外部来源：lib_colors.js 的锁定注释说明 HSL 算法改编自 "
        "W3C CSS Color 4，颜色表派生自 stock X11 rgb.txt；仓内没有所用"
        "上游版本、X11 原始数据或对应权威条款。收录这些注释不表示外部"
        "许可已经确定。\n"
        "\n"
        "libarchive Unicode 来源：archive_string_composition.h、"
        "archive_string.c 的锁定文本分别指向 UnicodeData 与 Unicode "
        "Standard Annex #15；来源数据、标准文本版本与适用许可仍须在"
        "公共发行前单独闭合。\n"
        "===== END APPLE HOST NOTICE: unresolved-provenance =====\n"
        "\n"
    ).encode("utf-8")


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
    for item in state.license_inputs:
        if item.path in inputs_by_path:
            fail(f"宿主许可与来源输入路径重复：{item.path}")
        inputs_by_path[item.path] = item
    verify_history_evidence(root, validator)

    raw_sources = []
    full_license_components = set()
    for item in state.license_inputs:
        if item.role != "license" or item.component not in FULL_LICENSE_COMPONENTS:
            continue
        full_license_components.add(item.component)
        raw_sources.append(
            NoticeSource(
                "完整许可",
                (
                    f"{item.component} "
                    f"{state.dependencies[item.component].version}"
                ),
                item.path,
                read_regular(root, item.path, "宿主完整许可文本"),
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

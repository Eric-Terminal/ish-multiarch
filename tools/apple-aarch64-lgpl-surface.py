#!/usr/bin/env python3

from __future__ import annotations

import argparse
import base64
import binascii
from dataclasses import dataclass
import hashlib
import importlib.util
import io
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys
import tarfile


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parent.parent
LOCK_ROOT = ROOT / "third_party" / "alpine" / "3.24.1-aarch64"
DEFAULT_PACKAGES = LOCK_ROOT / "packages.tsv"
DEFAULT_STATIC_LINK_SOURCES = LOCK_ROOT / "static-link-sources.tsv"
DEFAULT_ASSETS = LOCK_ROOT / "source-assets.tsv"
DEFAULT_INPUTS = LOCK_ROOT / "license-inputs.tsv"
DEFAULT_NOTICES = LOCK_ROOT / "THIRD-PARTY-NOTICES.txt"
DEFAULT_PAYLOADS = LOCK_ROOT / "lgpl-payloads.tsv"
DEFAULT_BINARY_REFERENCE = LOCK_ROOT / "binary-reference.tsv"

BUSYBOX_ROOT = "busybox-1.37.0"
BUSYBOX_VOLUME_PREFIX = f"{BUSYBOX_ROOT}/util-linux/volume_id/"
BUSYBOX_PUBLIC_HEADER = f"{BUSYBOX_ROOT}/include/volume_id.h"
BUSYBOX_INTERNAL_HEADER = (
    f"{BUSYBOX_VOLUME_PREFIX}volume_id_internal.h"
)
BUSYBOX_KBUILD_GENERATOR = f"{BUSYBOX_ROOT}/scripts/gen_build_files.sh"
PAX_ROOT = "pax-utils-1.3.9"
PAX_ELF_HEADER = f"{PAX_ROOT}/elf.h"
PAYLOAD_HEADER = "source_origin\tpayload_package\tpayload_path"
REFERENCE_HEADER = (
    "alpine_version\tarchive_name\tarchive_size\tarchive_sha256"
    "\tsource_url\tinstalled_size\tinstalled_sha256\tpackages_size"
    "\tpackages_sha256\tpackage_count\torigin_count"
)
EXPECTED_PAYLOADS = {
    "busybox": ("busybox@1.37.0-r31", "bin/busybox"),
    "pax-utils": ("scanelf@1.3.9-r1", "usr/bin/scanelf"),
}
EXPECTED_SOURCE_SNAPSHOT_SHA512 = {
    (
        "busybox",
        "aports",
    ): (
        "08c7e202226bda2285244ed607c1f390ae0a623781d9019d5f87e91113b514b3a"
        "236dc9a6ac0a31b31e03649420e265ad221867518d479e6689ab43830a4f09a"
    ),
    (
        "busybox",
        "upstream",
    ): (
        "ad8fd06f082699774f990a53d7a73b189ed404fe0a2166aff13eae4d9d8ee5c9"
        "239493befe949c98801fe7897520dbff3ed0224faa7205854ce4fa975e18467e"
    ),
    (
        "pax-utils",
        "aports",
    ): (
        "9bf4ffa973c56531b804cd29c837211543c88d266186924f6a165f262fd1332bf"
        "8d8d6bc7c10083f02d92ea253616791e0bbf7e09400019cfc0923c6fe7696d4"
    ),
    (
        "pax-utils",
        "upstream",
    ): (
        "96caf7f2087bedc7949db828d966835f7d691e29ef8177dee00605c10d433d4c5"
        "b5770a3d7f2e25e3c0218f5b9a6c6967669a3dea18cf138d6f0111d2bb5288b"
    ),
}
EXPECTED_BUSYBOX_NONPATCH_SOURCES = {
    "$_extras_openrc_files",
    "$_mdev_openrc_files",
    "$_openrc_files",
    "acpid-poweroff.sh",
    "acpid.logrotate",
    "bbsuid.c",
    "busyboxconfig",
    "busyboxconfig-extras",
    "dad.if-up",
    "default.script",
    "https://busybox.net/downloads/busybox-$pkgver.tar.bz2",
    "securetty",
    "ssl_client.c",
    "udhcpc.conf",
}


def load_license_tool():
    path = ROOT / "tools" / "apple-aarch64-rootfs-licenses.py"
    spec = importlib.util.spec_from_file_location(
        "apple_aarch64_rootfs_licenses", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("无法载入现有 Alpine 许可锁校验器。")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


LICENSE_TOOL = load_license_tool()


class SurfaceError(Exception):
    pass


@dataclass(frozen=True)
class Payload:
    source_origin: str
    package: str
    path: str


@dataclass(frozen=True)
class BinaryReference:
    archive_size: int
    archive_sha256: str
    installed_size: int
    installed_sha256: str
    packages_size: int
    packages_sha256: str
    package_count: int
    origin_count: int


def read_regular(path: Path, description: str) -> bytes:
    flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NONBLOCK", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise SurfaceError(f"无法读取{description}：{error}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise SurfaceError(f"{description}必须是普通文件。")
        chunks = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
    except SurfaceError:
        raise
    except OSError as error:
        raise SurfaceError(f"无法读取{description}：{error}") from error
    finally:
        os.close(descriptor)


def decode_text(data: bytes, description: str) -> str:
    if not data or not data.endswith(b"\n") or b"\r" in data:
        raise SurfaceError(f"{description}必须是非空、仅使用 LF 的文本。")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SurfaceError(f"{description}不是 UTF-8 文本。") from error


def parse_payloads(
    path: Path, packages: dict[str, object]
) -> dict[str, Payload]:
    lines = decode_text(
        read_regular(path, "LGPL payload 清单"), "LGPL payload 清单"
    ).splitlines()
    if lines[0] != PAYLOAD_HEADER or len(lines) != 3:
        raise SurfaceError("LGPL payload 清单必须使用固定表头且恰含两项。")
    payloads: dict[str, Payload] = {}
    previous: tuple[str, str, str] | None = None
    for line in lines[1:]:
        fields = tuple(line.split("\t"))
        if len(fields) != 3 or any(not field for field in fields):
            raise SurfaceError("LGPL payload 清单含空字段或列数错误。")
        origin, package_key, payload_path = fields
        if previous is not None and fields <= previous:
            raise SurfaceError("LGPL payload 清单必须按完整记录唯一排序。")
        try:
            LICENSE_TOOL.validate_relative_path(
                payload_path, "LGPL payload 路径"
            )
        except LICENSE_TOOL.LicenseLockError as error:
            raise SurfaceError(str(error)) from error
        package = packages.get(package_key)
        if package is None or package.origin != origin:
            raise SurfaceError("LGPL payload 与固定包版本或 origin 不一致。")
        if origin in payloads:
            raise SurfaceError("LGPL payload 清单含重复 source_origin。")
        payloads[origin] = Payload(origin, package_key, payload_path)
        previous = fields
    actual = {
        origin: (payload.package, payload.path)
        for origin, payload in payloads.items()
    }
    if actual != EXPECTED_PAYLOADS:
        raise SurfaceError("LGPL payload 的包版本或规范路径发生漂移。")
    return payloads


def select_lgpl_facts(locks) -> tuple[list[object], list[object]]:
    busybox = [
        item
        for item in locks.inputs
        if item.origin == "busybox"
        and item.source_kind == "upstream"
        and (
            item.source_member == BUSYBOX_PUBLIC_HEADER
            or item.source_member.startswith(BUSYBOX_VOLUME_PREFIX)
        )
    ]
    pax = [
        item
        for item in locks.inputs
        if item.origin == "pax-utils"
        and item.source_kind == "upstream"
        and item.source_member == PAX_ELF_HEADER
    ]
    busybox_c = [
        item for item in busybox if item.source_member.endswith(".c")
    ]
    busybox_headers = {
        item.source_member
        for item in busybox
        if item.source_member.endswith(".h")
    }
    if (
        len(busybox) != 22
        or len(busybox_c) != 20
        or busybox_headers
        != {BUSYBOX_PUBLIC_HEADER, BUSYBOX_INTERNAL_HEADER}
        or len(pax) != 1
    ):
        raise SurfaceError(
            "许可输入没有闭合 BusyBox 20 个 C、两个头和 pax elf.h。"
        )
    return busybox, pax


def validate_fact_locks(locks, payloads: dict[str, Payload]) -> None:
    busybox, pax = select_lgpl_facts(locks)
    expectations = (
        (
            busybox,
            "busybox",
            "distfiles/busybox/busybox-1.37.0.tar.bz2",
            "busybox-embedded-notices",
        ),
        (
            pax,
            "pax-utils",
            "distfiles/pax-utils/pax-utils-1.3.9.tar.xz",
            "pax-utils",
        ),
    )
    for facts, origin, asset, section in expectations:
        payload = payloads[origin]
        section_data = locks.sections.get(section)
        if section_data is None:
            raise SurfaceError("LGPL 源码事实引用了未知声明 section。")
        section_lines = section_data.splitlines()
        for item in facts:
            member = item.source_member.encode()
            bullet_count = section_lines.count(b"- " + member)
            verbatim_count = sum(
                line.startswith(
                    b"Verbatim source: " + member + b","
                )
                for line in section_lines
            )
            structured_count_is_invalid = (
                bullet_count > 1
                or verbatim_count > 1
                or bullet_count + verbatim_count == 0
            )
            if (
                item.source_asset != asset
                or item.notice_section != section
                or payload.package not in item.packages
                or structured_count_is_invalid
            ):
                raise SurfaceError(
                    f"{origin} 的 LGPL 源码事实、payload 或声明范围发生漂移。"
                )
        authority = [
            item
            for item in locks.inputs
            if item.origin == origin
            and item.source_kind == "authority"
            and item.source_asset == "license-inputs/LGPL-2.1.txt"
            and item.source_member == "-"
            and item.notice_section == "lgpl-2.1"
            and payload.package in item.packages
        ]
        if len(authority) != 1:
            raise SurfaceError(
                f"{origin} 必须恰有一项 LGPL 2.1 权威正文映射。"
            )


def read_asset_snapshot(cache: Path, locks, origin: str, kind: str) -> bytes:
    assets = [
        asset
        for asset in locks.assets.values()
        if asset.origin == origin and asset.kind == kind
    ]
    if len(assets) != 1:
        raise SurfaceError(f"{origin} 的 {kind} 源码资产不唯一。")
    asset = assets[0]
    try:
        descriptor = LICENSE_TOOL.open_safe_relative(
            cache, asset.bundle_path, "源码缓存资产"
        )
        try:
            data = LICENSE_TOOL.read_descriptor(descriptor, asset.size)
        finally:
            os.close(descriptor)
    except LICENSE_TOOL.LicenseLockError as error:
        raise SurfaceError(str(error)) from error
    if (
        len(data) != asset.size
        or hashlib.sha512(data).hexdigest() != asset.sha512
    ):
        raise SurfaceError(f"源码缓存资产摘要不匹配：{asset.bundle_path}")
    return data


def validate_reviewed_source_snapshots(locks) -> None:
    actual: dict[tuple[str, str], str] = {}
    for asset in locks.assets.values():
        key = (asset.origin, asset.kind)
        if key not in EXPECTED_SOURCE_SNAPSHOT_SHA512:
            continue
        if key in actual:
            raise SurfaceError("LGPL 构建表面的源码快照不唯一。")
        actual[key] = asset.sha512
    if actual != EXPECTED_SOURCE_SNAPSHOT_SHA512:
        raise SurfaceError(
            "LGPL 构建表面的已审阅源码快照指纹发生漂移。"
        )


def archive_files(
    data: bytes, predicate, description: str
) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
            for member in archive:
                try:
                    LICENSE_TOOL.validate_relative_path(
                        member.name, f"{description}成员路径"
                    )
                except LICENSE_TOOL.LicenseLockError as error:
                    raise SurfaceError(str(error)) from error
                if not predicate(member.name):
                    continue
                if member.name in files:
                    raise SurfaceError(f"{description}含重复成员：{member.name}")
                if not member.isfile():
                    raise SurfaceError(
                        f"{description}的目标成员不是普通文件：{member.name}"
                    )
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise SurfaceError(
                        f"无法读取{description}成员：{member.name}"
                    )
                files[member.name] = extracted.read()
    except SurfaceError:
        raise
    except (tarfile.TarError, OSError) as error:
        raise SurfaceError(f"{description}不是有效归档。") from error
    return files


def unique_basename(
    files: dict[str, bytes], name: str, description: str
) -> bytes:
    matches = [
        data for path, data in files.items() if PurePosixPath(path).name == name
    ]
    if len(matches) != 1:
        raise SurfaceError(f"{description}必须恰含一个 {name}。")
    return matches[0]


def license_comment(data: bytes, description: str) -> str:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SurfaceError(f"{description}不是 UTF-8 源码。") from error
    position = 0
    while position < len(text) and text[position].isspace():
        position += 1
    while text.startswith("/*", position):
        match = re.match(r"/\*.*?\*/", text[position:8192], re.DOTALL)
        if match is None:
            break
        comment = match.group(0)
        if "License" in comment or "Licensed" in comment:
            return comment
        position += match.end()
        while position < len(text) and text[position].isspace():
            position += 1
    raise SurfaceError(f"{description}开头缺少许可块注释。")


def require_versioned_lgpl(data: bytes, description: str) -> None:
    notice = normalized_comment(license_comment(data, description))
    grant = re.compile(
        r"(?:This library|The GNU C Library) is free software; "
        r"you can redistribute it and/or modify it under the terms "
        r"of the GNU Lesser General Public License as published by "
        r"the Free Software Foundation; either version 2\.1 of the "
        r"License, or \(at your option\) any later version\."
    )
    versions = re.findall(
        r"\bversion\s+([0-9]+(?:\.[0-9]+)?)\b",
        notice,
        flags=re.IGNORECASE,
    )
    if grant.search(notice) is None or versions != ["2.1"]:
        raise SurfaceError(f"{description}的 LGPL-2.1-or-later 原文发生漂移。")


def normalized_comment(comment: str) -> str:
    body = comment.removeprefix("/*").removesuffix("*/")
    lines = [
        re.sub(r"^\s*\*?\s?", "", line)
        for line in body.splitlines()
    ]
    return " ".join(" ".join(lines).split())


def has_local_include(data: bytes, header: str) -> bool:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return False
    without_comments = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    without_comments = re.sub(r"//[^\n]*", "", without_comments)
    conditions: list[bool | None] = []
    found = False
    meaningful_lines = 0
    for line in without_comments.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        conditional = re.fullmatch(r"#\s*if\s+(.+)", stripped)
        if conditional is not None:
            expression = conditional.group(1).strip()
            if re.fullmatch(
                r"\(?0[uUlL]*\)?",
                expression,
            ):
                conditions.append(False)
            elif re.fullmatch(
                r"\(?1[uUlL]*\)?",
                expression,
            ):
                conditions.append(True)
            else:
                conditions.append(None)
            meaningful_lines += 1
            continue
        named_conditional = re.fullmatch(
            r"#\s*(ifdef|ifndef)\b.*",
            stripped,
        )
        if named_conditional is not None:
            is_outer_header_guard = (
                not conditions
                and meaningful_lines == 0
                and named_conditional.group(1) == "ifndef"
            )
            conditions.append(True if is_outer_header_guard else None)
            meaningful_lines += 1
            continue
        alternate = re.fullmatch(r"#\s*(?:else|elif\b(.*))", stripped)
        if alternate is not None:
            if not conditions:
                return False
            current = conditions[-1]
            expression = alternate.group(1)
            if expression is None:
                conditions[-1] = (
                    None if current is None else not current
                )
            elif current is True:
                conditions[-1] = False
            elif current is None:
                conditions[-1] = None
            else:
                expression = expression.strip()
                conditions[-1] = (
                    re.fullmatch(r"\(?0[uUlL]*\)?", expression)
                    is None
                )
            meaningful_lines += 1
            continue
        if re.fullmatch(r"#\s*endif\b.*", stripped):
            if not conditions:
                return False
            conditions.pop()
            meaningful_lines += 1
            continue
        if (
            all(condition is True for condition in conditions)
            and re.fullmatch(
                rf'#\s*include\s*"{re.escape(header)}"',
                stripped,
            )
        ):
            found = True
        meaningful_lines += 1
    return found and not conditions


def bcache_notice_is_unversioned(notice: str) -> bool:
    normalized = normalized_comment(notice)
    grant = (
        "This file may be redistributed under the terms of the "
        "GNU Lesser General Public License."
    )
    return (
        grant in normalized
        and re.search(r"\bversion\b", normalized, re.IGNORECASE) is None
        and re.search(
            r"\bLGPL\s*(?:v(?:ersion)?\s*)?[0-9]",
            normalized,
            re.IGNORECASE,
        )
        is None
        and re.search(
            r"General Public License\s*[0-9]",
            normalized,
            re.IGNORECASE,
        )
        is None
        and re.search(
            r"\b(?:first|second|third|fourth|fifth)\s+edition\b",
            normalized,
            re.IGNORECASE,
        )
        is None
    )


def normalize_patch_path(path: bytes) -> bytes | None:
    if path == b"/dev/null":
        return None
    if (
        not path
        or path.startswith((b"/", b'"'))
        or b'"' in path
        or b"\\" in path
        or any(byte < 0x20 or byte == 0x7F for byte in path)
    ):
        raise SurfaceError("patch 含不支持或不安全的成员路径。")
    if path.startswith((b"a/", b"b/")):
        path = path[2:]
    parts = path.split(b"/")
    if any(part in (b"", b".", b"..") for part in parts):
        raise SurfaceError("patch 成员路径不是规范相对路径。")
    return b"/".join(parts)


def patch_changed_paths(data: bytes) -> set[bytes]:
    paths: set[bytes] = set()
    for line in data.splitlines():
        candidates: tuple[bytes, ...] = ()
        if line.startswith((b"--- ", b"+++ ")):
            field = line[4:].split(b"\t", 1)[0]
            if b"\t" not in line[4:]:
                field = field.split(b" ", 1)[0]
            candidates = (field,)
        elif line.startswith(b"diff --git "):
            fields = line.split()
            if len(fields) != 4:
                raise SurfaceError("patch 的 diff --git 路径格式不受支持。")
            candidates = (fields[2], fields[3])
        elif line.startswith((b"rename from ", b"rename to ")):
            field = line.split(b" ", 2)[2]
            if b" " in field:
                raise SurfaceError("patch 的 rename 路径格式不受支持。")
            candidates = (field,)
        for path in candidates:
            normalized = normalize_patch_path(path)
            if normalized is not None:
                paths.add(normalized)
    return paths


def patch_touches_busybox_surface(data: bytes) -> bool:
    for path in patch_changed_paths(data):
        if path.startswith(b"busybox-1.37.0/"):
            path = path[len(b"busybox-1.37.0/"):]
        if (
            path in {
                b"Makefile",
                b"Makefile.flags",
                b"Makefile.custom",
                b"Config.in",
                b"Kbuild",
                b"include/volume_id.h",
                b"util-linux/Kbuild.src",
                b"util-linux/Config.src",
                b"util-linux/blkid.c",
            }
            or path.startswith(b"scripts/")
            or path == b"util-linux/volume_id"
            or path.startswith(b"util-linux/volume_id/")
        ):
            return True
    return False


def patch_touches_pax_surface(data: bytes) -> bool:
    protected = {
        b"elf.h",
        b"meson.build",
        b"meson_options.txt",
        b"paxinc.h",
        b"scanelf.c",
    }
    for path in patch_changed_paths(data):
        if path.startswith(b"pax-utils-1.3.9/"):
            path = path[len(b"pax-utils-1.3.9/"):]
        if path in protected or path.endswith(b"/meson.build"):
            return True
    return False


def strip_shell_comment(raw_line: str) -> str:
    single_quoted = False
    double_quoted = False
    escaped = False
    for index, character in enumerate(raw_line):
        if escaped:
            escaped = False
            continue
        if character == "\\" and not single_quoted:
            escaped = True
            continue
        if character == "'" and not double_quoted:
            single_quoted = not single_quoted
            continue
        if character == '"' and not single_quoted:
            double_quoted = not double_quoted
            continue
        if (
            character == "#"
            and not single_quoted
            and not double_quoted
            and (
                index == 0
                or raw_line[index - 1].isspace()
                or raw_line[index - 1] in ";&|()<>"
            )
        ):
            return raw_line[:index].rstrip()
    return raw_line.rstrip()


def shell_control_depth(text: str, description: str) -> int:
    depth = 0
    for raw_line in text.splitlines():
        line = strip_shell_comment(raw_line).strip()
        if not line or line.startswith("#"):
            continue
        if re.fullmatch(r"(?:fi|done|esac)", line):
            depth -= 1
            if depth < 0:
                raise SurfaceError(f"{description}的 shell 控制块不平衡。")
            continue
        if re.match(
            r"^(?:if|for|while|until|select|case)\b",
            line,
        ) and re.search(
            r";\s*(?:fi|done|esac)\s*$",
            line,
        ) is None:
            depth += 1
    return depth


def top_level_shell_commands(body: str, description: str) -> list[str]:
    commands: list[str] = []
    depth = 0
    for raw_line in body.splitlines():
        line = strip_shell_comment(raw_line).strip()
        if not line or line.startswith("#"):
            continue
        if re.search(r"\b(?:exec|exit|return)\b", line):
            raise SurfaceError(f"{description}含提前终止命令。")
        if re.fullmatch(r"(?:fi|done|esac)", line):
            depth -= 1
            if depth < 0:
                raise SurfaceError(f"{description}的 shell 控制块不平衡。")
            continue
        if depth == 0:
            commands.append(line)
        if re.match(
            r"^(?:if|for|while|until|select|case)\b",
            line,
        ) and re.search(
            r";\s*(?:fi|done|esac)\s*$",
            line,
        ) is None:
            depth += 1
    if depth != 0:
        raise SurfaceError(f"{description}的 shell 控制块不平衡。")
    return commands


def reject_dynamic_apkbuild_constructs(
    text: str, description: str
) -> None:
    previous_continues = False
    for raw_line in text.splitlines():
        line = strip_shell_comment(raw_line).strip()
        if not line:
            continue
        if (
            re.search(r"\b(?:eval|unset)\b", line)
            or re.search(
                r"(?:^|[;&|])\s*source(?:\s|$)",
                line,
            )
            or (
                not previous_continues
                and re.search(
                    r"(?:^|[;&|])\s*\.(?:\s|$)",
                    line,
                )
            )
        ):
            raise SurfaceError(
                f"{description}含不支持的动态 shell 构造。"
            )
        previous_continues = line.endswith("\\")


def require_command_sequence(
    commands: list[str],
    expected: tuple[str, ...],
    description: str,
) -> list[int]:
    position = -1
    positions = []
    for command in expected:
        try:
            position = commands.index(command, position + 1)
        except ValueError as error:
            raise SurfaceError(f"{description}的可执行命令链发生漂移。") from error
        positions.append(position)
    relevant = commands[:position + 1]
    if any(
        re.match(
            r"^(?:exec|exit|return)(?:\s|$)",
            command,
        )
        for command in relevant
    ):
        raise SurfaceError(f"{description}含提前终止命令。")
    if any(
        line in {"{", "}", "(", ")"}
        or re.search(r"(?:^|&&|\|\|)\s*[\{\(]\s*$", line)
        or re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*\s*\(\)\s*\{",
            line,
        )
        or re.search(r"(?:&&|\|\||\|)\s*\\?\s*$", line)
        for line in relevant
    ):
        raise SurfaceError(f"{description}含不支持的 shell 控制结构。")
    return positions


def shell_group_depths(
    commands: list[str], description: str
) -> list[int]:
    depth = 0
    result = []
    for line in commands:
        closes = line == "}" or line.startswith("} ")
        if closes:
            depth -= 1
            if depth < 0:
                raise SurfaceError(f"{description}的 shell 分组不平衡。")
        result.append(depth)
        opens = (
            line == "{"
            or re.search(r"(?:^|&&|\|\|)\s*\{\s*$", line)
            is not None
        )
        if opens:
            depth += 1
    if depth != 0:
        raise SurfaceError(f"{description}的 shell 分组不平衡。")
    return result


def meson_control_depth(text: str, description: str) -> int:
    depth = 0
    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if line in {"endif", "endforeach"}:
            depth -= 1
            if depth < 0:
                raise SurfaceError(f"{description}的控制块不平衡。")
            continue
        if re.match(r"^(?:if|foreach)\b", line):
            depth += 1
    return depth


def apkbuild_function(text: str, name: str, description: str) -> str:
    if "<<" in text:
        raise SurfaceError(f"{description}含不支持的 here-document。")
    declarations = re.findall(
        rf"(?m)^{re.escape(name)}\s*\(\s*\)\s*\{{",
        text,
    )
    keyword_declarations = re.findall(
        rf"(?m)^function\s+{re.escape(name)}\b",
        text,
    )
    if len(declarations) != 1 or keyword_declarations:
        raise SurfaceError(f"{description}的 {name} 函数定义不唯一。")
    matches = re.findall(
        rf"(?ms)^{re.escape(name)}\(\) \{{\n(.*?)^\}}$",
        text,
    )
    if len(matches) != 1:
        raise SurfaceError(f"{description}缺少唯一可解析的 {name} 函数。")
    match = re.search(
        rf"(?ms)^{re.escape(name)}\(\) \{{\n.*?^\}}$",
        text,
    )
    if (
        match is None
        or shell_control_depth(
            text[:match.start()], description
        )
        != 0
    ):
        raise SurfaceError(f"{description}的 {name} 不是顶层函数。")
    return matches[0]


def require_top_level_patterns(
    text: str,
    patterns: tuple[str, ...],
    description: str,
) -> None:
    for pattern in patterns:
        matches = list(re.finditer(pattern, text))
        variable = re.match(
            r"\(\?m\)\^([A-Za-z_][A-Za-z0-9_]*)=",
            pattern,
        )
        if (
            len(matches) != 1
            or (
                variable is not None
                and len(
                    re.findall(
                        rf"(?m)^{re.escape(variable.group(1))}=",
                        text,
                    )
                )
                != 1
            )
            or shell_control_depth(
                text[:matches[0].start()], description
            )
            != 0
        ):
            raise SurfaceError(f"{description}的固定顶层字段发生漂移。")


def apkbuild_source_entries(text: str, description: str) -> list[str]:
    matches = list(re.finditer(
        r'(?ms)^source="(?P<body>.*?)^[ \t]*"$',
        text,
    ))
    if (
        len(matches) != 1
        or len(re.findall(r"(?m)^source=", text)) != 1
        or shell_control_depth(
            text[:matches[0].start()], description
        )
        != 0
    ):
        raise SurfaceError(f"{description}缺少唯一可解析的 source 清单。")
    entries = matches[0].group("body").split()
    if len(entries) != len(set(entries)):
        raise SurfaceError(f"{description}的 source 清单含重复成员。")
    return entries


def validate_busybox_sources(
    aports_files: dict[str, bytes],
    upstream_files: dict[str, bytes],
    expected_members: set[str],
) -> None:
    apkbuild = unique_basename(aports_files, "APKBUILD", "BusyBox aports 资产")
    config = unique_basename(
        aports_files, "busyboxconfig", "BusyBox aports 资产"
    )
    try:
        apkbuild_text = apkbuild.decode("utf-8")
        config_text = config.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SurfaceError("BusyBox aports 构建输入不是 UTF-8。") from error
    reject_dynamic_apkbuild_constructs(
        apkbuild_text, "BusyBox APKBUILD"
    )
    required_apkbuild = (
        r"(?m)^pkgname=busybox$",
        r"(?m)^pkgver=1\.37\.0$",
        r"(?m)^pkgrel=31$",
        r'(?m)^_config="\$srcdir"/busyboxconfig$',
    )
    require_top_level_patterns(
        apkbuild_text,
        required_apkbuild,
        "BusyBox APKBUILD",
    )
    source_entries = apkbuild_source_entries(
        apkbuild_text, "BusyBox APKBUILD"
    )
    if (
        source_entries.count(
            "https://busybox.net/downloads/busybox-$pkgver.tar.bz2"
        )
        != 1
        or source_entries.count("busyboxconfig") != 1
        or {
            entry
            for entry in source_entries
            if not entry.endswith(".patch")
        }
        != EXPECTED_BUSYBOX_NONPATCH_SOURCES
    ):
        raise SurfaceError(
            "BusyBox APKBUILD 的固定上游 source 或 busyboxconfig 发生漂移。"
        )
    declared_patch_names = [
        PurePosixPath(entry).name
        for entry in source_entries
        if entry.endswith(".patch")
    ]
    archived_patch_names = [
        PurePosixPath(path).name
        for path in aports_files
        if path.endswith(".patch")
    ]
    if (
        len(declared_patch_names) != len(set(declared_patch_names))
        or len(archived_patch_names) != len(set(archived_patch_names))
    ):
        raise SurfaceError("BusyBox aports 资产含重复 patch 名称。")
    declared_patches = set(declared_patch_names)
    archived_patches = set(archived_patch_names)
    if declared_patches != archived_patches:
        raise SurfaceError("BusyBox APKBUILD 的 patch 集合未与 aports 资产闭合。")
    prepare = apkbuild_function(apkbuild_text, "prepare", "BusyBox APKBUILD")
    build = apkbuild_function(apkbuild_text, "build", "BusyBox APKBUILD")
    package = apkbuild_function(apkbuild_text, "package", "BusyBox APKBUILD")
    require_command_sequence(
        top_level_shell_commands(prepare, "BusyBox prepare"),
        ("default_prepare",),
        "BusyBox prepare",
    )
    require_command_sequence(
        top_level_shell_commands(build, "BusyBox build"),
        (
            'cd "$_dyndir"',
            'cp "$_config" .config',
            'make -C "$builddir" O="$PWD" silentoldconfig',
            'make CONFIG_EXTRA_CFLAGS="$_extra_cflags" '
            'CONFIG_EXTRA_LDLIBS="$_extra_libs"',
        ),
        "BusyBox 动态配置与 make 构建链",
    )
    require_command_sequence(
        top_level_shell_commands(package, "BusyBox package"),
        (
            'cd "$_dyndir"',
            'install -Dm755 busybox "$pkgdir"/bin/busybox',
        ),
        "BusyBox 动态 bin/busybox 安装链",
    )

    enabled: set[str] = set()
    seen: set[str] = set()
    for line in config_text.splitlines():
        match = re.fullmatch(r"CONFIG_([A-Z0-9_]+)=y", line)
        disabled = re.fullmatch(r"# CONFIG_([A-Z0-9_]+) is not set", line)
        if match is not None:
            name = match.group(1)
            if name in seen:
                raise SurfaceError("BusyBox 配置含重复 selector。")
            enabled.add(name)
            seen.add(name)
        elif disabled is not None:
            name = disabled.group(1)
            if name in seen:
                raise SurfaceError("BusyBox 配置含重复 selector。")
            seen.add(name)

    source_by_name = {
        PurePosixPath(path).name: data
        for path, data in upstream_files.items()
        if path.startswith(BUSYBOX_VOLUME_PREFIX) and path.endswith(".c")
    }
    if len(source_by_name) != len(
        [
            path
            for path in upstream_files
            if path.startswith(BUSYBOX_VOLUME_PREFIX) and path.endswith(".c")
        ]
    ):
        raise SurfaceError("BusyBox volume_id 源码含重复文件名。")
    generator = upstream_files.get(BUSYBOX_KBUILD_GENERATOR)
    try:
        generator_text = (
            generator.decode("utf-8") if generator is not None else ""
        )
    except UnicodeDecodeError as error:
        raise SurfaceError("BusyBox Kbuild 生成器不是 UTF-8。") from error
    generator_commands = [
        line.strip()
        for line in generator_text.splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]
    generator_sequence = (
        'src="$srctree/$d/Kbuild.src"',
        'dst="$d/Kbuild"',
        'if test -f "$src"; then',
        "sed -n 's@^//kbuild:@@p' \"$srctree/$d\"/*.c \\",
        "| generate \\",
        '"${src}" "${dst}" \\',
        '"# DO NOT EDIT. This file is generated from Kbuild.src"',
        "fi",
    )
    if (
        "<<" in generator_text
        or generator_commands.count("exit 0") != 1
        or not generator_commands
        or generator_commands[-1] != "exit 0"
    ):
        raise SurfaceError("BusyBox 不再从 C 源码生成 Kbuild 输入。")
    if generator_commands.count(generator_sequence[0]) != 1:
        raise SurfaceError("BusyBox Kbuild 生成器入口发生漂移。")
    sequence_start = generator_commands.index(generator_sequence[0])
    require_command_sequence(
        generator_commands[sequence_start:-1],
        generator_sequence,
        "BusyBox Kbuild 生成器",
    )
    generator_depths = shell_group_depths(
        generator_commands, "BusyBox Kbuild 生成器"
    )
    if (
        generator_depths[sequence_start] != 0
        or any(
            generator_depths[index] == 0
            and re.match(
                r"^(?:exec|exit|return)(?:\s|$)",
                command,
            )
            for index, command in enumerate(
                generator_commands[:sequence_start]
            )
        )
        or (
            sequence_start > 0
            and re.search(
                r"(?:&&|\|\||\|)\s*\\?\s*$",
                generator_commands[sequence_start - 1],
            )
            is not None
        )
    ):
        raise SurfaceError(
            "BusyBox Kbuild 生成链位于不执行的 shell 分组中。"
        )
    active_objects: set[str] = set()
    kbuild = re.compile(
        r"^//kbuild:lib-\$\(CONFIG_([A-Z0-9_]+)\)\s*\+=\s*(.+)$",
        re.MULTILINE,
    )
    for name, data in source_by_name.items():
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as error:
            raise SurfaceError(f"BusyBox 源码不是 UTF-8：{name}") from error
        for selector, object_text in kbuild.findall(text):
            objects = object_text.split()
            if (
                not objects
                or any(
                    re.fullmatch(r"[A-Za-z0-9_+.-]+\.o", item) is None
                    or PurePosixPath(item).name != item
                    for item in objects
                )
            ):
                raise SurfaceError(f"BusyBox Kbuild 对象格式非法：{name}")
            if selector in enabled:
                active_objects.update(objects)
    active_names = {f"{PurePosixPath(item).stem}.c" for item in active_objects}
    if any(name not in source_by_name for name in active_names):
        raise SurfaceError("BusyBox Kbuild 引用了不存在的 volume_id C 文件。")
    active = {name: source_by_name[name] for name in active_names}
    if len(active) != 26:
        raise SurfaceError("BusyBox 固定配置不再产生 26 个 active volume_id C。")

    lgpl_members: set[str] = set()
    gplv2_count = 0
    gplv2_later: set[str] = set()
    for name, data in active.items():
        notice = license_comment(data, f"BusyBox {name}")
        if "GNU Lesser General Public" in notice:
            member = f"{BUSYBOX_VOLUME_PREFIX}{name}"
            if name == "bcache.c":
                if not bcache_notice_is_unversioned(notice):
                    raise SurfaceError(
                        "bcache.c 的无版本 LGPL 事实被擅自解释。"
                    )
            else:
                require_versioned_lgpl(data, f"BusyBox {name}")
            lgpl_members.add(member)
        elif "Licensed under GPLv2 or later" in notice:
            gplv2_later.add(name)
        elif "Licensed under GPLv2" in notice:
            gplv2_count += 1
        else:
            raise SurfaceError(f"BusyBox {name} 的许可开头无法归类。")
        if not has_local_include(data, "volume_id_internal.h"):
            raise SurfaceError(
                f"active BusyBox {name} 不再消费 volume_id_internal.h。"
            )
    if (
        len(lgpl_members) != 20
        or gplv2_count != 5
        or gplv2_later != {"get_devname.c"}
    ):
        raise SurfaceError(
            "BusyBox active C 的 LGPL/GPLv2 边界发生漂移。"
        )

    public_header = upstream_files.get(BUSYBOX_PUBLIC_HEADER)
    internal_header = upstream_files.get(BUSYBOX_INTERNAL_HEADER)
    if public_header is None or internal_header is None:
        raise SurfaceError("BusyBox volume_id 头文件闭包不完整。")
    require_versioned_lgpl(public_header, "BusyBox volume_id.h")
    require_versioned_lgpl(internal_header, "BusyBox volume_id_internal.h")
    if not has_local_include(internal_header, "volume_id.h"):
        raise SurfaceError(
            "BusyBox volume_id_internal.h 不再消费 volume_id.h。"
        )
    lgpl_members.update({BUSYBOX_PUBLIC_HEADER, BUSYBOX_INTERNAL_HEADER})
    if lgpl_members != expected_members:
        raise SurfaceError(
            "BusyBox 构建推导的 LGPL 输入与许可输入清单不一致。"
        )

    for path, patch in aports_files.items():
        if path.endswith(".patch"):
            changed_paths = patch_changed_paths(patch)
            if not changed_paths:
                raise SurfaceError(f"Alpine BusyBox patch 没有可解析路径：{path}")
            if patch_touches_busybox_surface(patch):
                raise SurfaceError(
                    f"Alpine BusyBox patch 触及已审计 LGPL 路径：{path}"
                )


def validate_pax_sources(
    aports_files: dict[str, bytes],
    upstream_files: dict[str, bytes],
) -> None:
    apkbuild = unique_basename(aports_files, "APKBUILD", "pax-utils aports 资产")
    required = {
        f"{PAX_ROOT}/meson.build",
        f"{PAX_ROOT}/scanelf.c",
        f"{PAX_ROOT}/paxinc.h",
        PAX_ELF_HEADER,
    }
    if set(upstream_files) != required:
        raise SurfaceError("pax-utils scanelf 源码闭包不完整。")
    try:
        apkbuild_text = apkbuild.decode("utf-8")
        meson = upstream_files[f"{PAX_ROOT}/meson.build"].decode("utf-8")
    except UnicodeDecodeError as error:
        raise SurfaceError("pax-utils 构建输入不是 UTF-8。") from error
    reject_dynamic_apkbuild_constructs(
        apkbuild_text, "pax-utils APKBUILD"
    )
    required_apkbuild = (
        r"(?m)^pkgname=pax-utils$",
        r"(?m)^pkgver=1\.3\.9$",
        r"(?m)^pkgrel=1$",
        r'(?m)^subpackages=.*\bscanelf:_scanelf\b',
    )
    require_top_level_patterns(
        apkbuild_text,
        required_apkbuild,
        "pax-utils APKBUILD",
    )
    source_entries = apkbuild_source_entries(
        apkbuild_text, "pax-utils APKBUILD"
    )
    upstream_source = (
        "https://dev.gentoo.org/~sam/distfiles/app-misc/"
        "pax-utils/pax-utils-$pkgver.tar.xz"
    )
    if (
        not source_entries
        or source_entries[0] != upstream_source
        or any(
            not entry.endswith(".patch")
            for entry in source_entries[1:]
        )
    ):
        raise SurfaceError("pax-utils APKBUILD 的固定上游 source 发生漂移。")
    declared_patch_names = [
        PurePosixPath(entry).name
        for entry in source_entries
        if entry.endswith(".patch")
    ]
    archived_patch_names = [
        PurePosixPath(path).name
        for path in aports_files
        if path.endswith(".patch")
    ]
    if (
        len(declared_patch_names) != len(set(declared_patch_names))
        or len(archived_patch_names) != len(set(archived_patch_names))
    ):
        raise SurfaceError("pax-utils aports 资产含重复 patch 名称。")
    declared_patches = set(declared_patch_names)
    archived_patches = set(archived_patch_names)
    if declared_patches != archived_patches:
        raise SurfaceError("pax-utils APKBUILD 的 patch 集合未与 aports 资产闭合。")
    build = apkbuild_function(apkbuild_text, "build", "pax-utils APKBUILD")
    package = apkbuild_function(
        apkbuild_text, "package", "pax-utils APKBUILD"
    )
    scanelf_package = apkbuild_function(
        apkbuild_text, "_scanelf", "pax-utils APKBUILD"
    )
    require_command_sequence(
        top_level_shell_commands(build, "pax-utils build"),
        (
            "abuild-meson \\",
            ". output",
            "meson compile -C output",
        ),
        "pax-utils Meson 构建链",
    )
    require_command_sequence(
        top_level_shell_commands(package, "pax-utils package"),
        ('DESTDIR="$pkgdir" meson install --no-rebuild -C output',),
        "pax-utils package 安装链",
    )
    require_command_sequence(
        top_level_shell_commands(scanelf_package, "pax-utils _scanelf"),
        ("amove usr/bin/scanelf",),
        "pax-utils scanelf 子包安装链",
    )
    targets = list(re.finditer(
        r"^executable\(\s*'scanelf',(?P<body>.*?)^\)",
        meson,
        re.MULTILINE | re.DOTALL,
    ))
    if (
        len(targets) != 1
        or meson_control_depth(
            meson[:targets[0].start()], "pax-utils meson.build"
        )
        != 0
    ):
        raise SurfaceError("pax-utils Meson scanelf target 发生漂移。")
    target_lines = {
        line.strip()
        for line in targets[0].group("body").splitlines()
        if line.strip() and not line.strip().startswith("#")
    }
    if target_lines != {
        "'paxelf.c',",
        "'paxldso.c',",
        "'scanelf.c',",
        "version_h,",
        "dependencies : [libcap],",
        "link_with : common,",
        "install : true",
    }:
        raise SurfaceError("pax-utils Meson scanelf target 发生漂移。")
    if not has_local_include(
        upstream_files[f"{PAX_ROOT}/scanelf.c"], "paxinc.h"
    ):
        raise SurfaceError("scanelf.c 不再消费 paxinc.h。")
    if not has_local_include(
        upstream_files[f"{PAX_ROOT}/paxinc.h"], "elf.h"
    ):
        raise SurfaceError("paxinc.h 不再消费本地 elf.h。")
    require_versioned_lgpl(
        upstream_files[PAX_ELF_HEADER], "pax-utils elf.h"
    )
    for path, patch in aports_files.items():
        if path.endswith(".patch"):
            changed_paths = patch_changed_paths(patch)
            if not changed_paths:
                raise SurfaceError(
                    f"Alpine pax-utils patch 没有可解析路径：{path}"
                )
            if patch_touches_pax_surface(patch):
                raise SurfaceError(
                    f"Alpine pax-utils patch 触及 scanelf LGPL 链：{path}"
                )


def validate_source_surface(
    cache: Path,
    locks,
    require_reviewed_snapshots: bool = True,
) -> None:
    if require_reviewed_snapshots:
        validate_reviewed_source_snapshots(locks)
    busybox_aports = archive_files(
        read_asset_snapshot(cache, locks, "busybox", "aports"),
        lambda name: PurePosixPath(name).name in {"APKBUILD", "busyboxconfig"}
        or name.endswith(".patch"),
        "BusyBox aports 资产",
    )
    busybox_upstream = archive_files(
        read_asset_snapshot(cache, locks, "busybox", "upstream"),
        lambda name: name == BUSYBOX_PUBLIC_HEADER
        or name == BUSYBOX_KBUILD_GENERATOR
        or name.startswith(BUSYBOX_VOLUME_PREFIX),
        "BusyBox 上游资产",
    )
    busybox_facts, _ = select_lgpl_facts(locks)
    validate_busybox_sources(
        busybox_aports,
        busybox_upstream,
        {item.source_member for item in busybox_facts},
    )

    pax_aports = archive_files(
        read_asset_snapshot(cache, locks, "pax-utils", "aports"),
        lambda name: PurePosixPath(name).name == "APKBUILD"
        or name.endswith(".patch"),
        "pax-utils aports 资产",
    )
    pax_members = {
        f"{PAX_ROOT}/meson.build",
        f"{PAX_ROOT}/scanelf.c",
        f"{PAX_ROOT}/paxinc.h",
        PAX_ELF_HEADER,
    }
    pax_upstream = archive_files(
        read_asset_snapshot(cache, locks, "pax-utils", "upstream"),
        lambda name: name in pax_members,
        "pax-utils 上游资产",
    )
    validate_pax_sources(pax_aports, pax_upstream)


def parse_binary_reference(path: Path) -> BinaryReference:
    lines = decode_text(
        read_regular(path, "rootfs 二进制参照清单"),
        "rootfs 二进制参照清单",
    ).splitlines()
    if lines[0] != REFERENCE_HEADER or len(lines) != 2:
        raise SurfaceError("rootfs 二进制参照清单格式非法。")
    fields = lines[1].split("\t")
    if len(fields) != 11 or any(not field for field in fields):
        raise SurfaceError("rootfs 二进制参照清单字段非法。")
    (
        version,
        archive_name,
        archive_size,
        archive_sha256,
        source_url,
        installed_size,
        installed_sha256,
        packages_size,
        packages_sha256,
        package_count,
        origin_count,
    ) = fields
    expected_url = (
        "https://dl-cdn.alpinelinux.org/alpine/"
        f"v{version.rpartition('.')[0]}/releases/aarch64/{archive_name}"
    )
    decimal = (
        archive_size,
        installed_size,
        packages_size,
        package_count,
        origin_count,
    )
    if (
        re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None
        or archive_name
        != f"alpine-minirootfs-{version}-aarch64.tar.gz"
        or source_url != expected_url
        or any(
            not value.isdecimal()
            or str(int(value)) != value
            or int(value) < 1
            for value in decimal
        )
        or re.fullmatch(r"[0-9a-f]{64}", archive_sha256) is None
        or re.fullmatch(r"[0-9a-f]{64}", installed_sha256) is None
        or re.fullmatch(r"[0-9a-f]{64}", packages_sha256) is None
    ):
        raise SurfaceError("rootfs 二进制参照清单字段非法。")
    return BinaryReference(
        int(archive_size),
        archive_sha256,
        int(installed_size),
        installed_sha256,
        int(packages_size),
        packages_sha256,
        int(package_count),
        int(origin_count),
    )


def normalize_archive_path(name: str) -> str | None:
    while name.startswith("./"):
        name = name[2:]
    path = PurePosixPath(name)
    if (
        not name
        or path.is_absolute()
        or str(path) != name
        or any(part in ("", ".", "..") for part in path.parts)
        or "\\" in name
    ):
        return None
    return name


def parse_installed_records(data: bytes) -> list[list[str]]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SurfaceError("apk installed 数据库不是 UTF-8。") from error
    records = [
        paragraph.splitlines()
        for paragraph in text.strip().split("\n\n")
        if paragraph
    ]
    if not records or any(not record for record in records):
        raise SurfaceError("apk installed 数据库没有有效记录。")
    return records


def unique_field(record: list[str], prefix: str) -> str:
    values = [line[len(prefix):] for line in record if line.startswith(prefix)]
    if len(values) != 1 or not values[0]:
        raise SurfaceError(f"apk installed 记录缺少或重复字段 {prefix}")
    return values[0]


def validate_installed_packages(
    records: list[list[str]], packages: dict[str, object], reference: BinaryReference
) -> dict[str, list[str]]:
    actual: dict[str, list[str]] = {}
    origins: set[str] = set()
    for record in records:
        name = unique_field(record, "P:")
        version = unique_field(record, "V:")
        origin = unique_field(record, "o:")
        license_name = unique_field(record, "L:")
        commit = unique_field(record, "c:")
        architecture = unique_field(record, "A:")
        key = f"{name}@{version}"
        package = packages.get(key)
        if (
            package is None
            or package.origin != origin
            or package.license != license_name
            or package.commit != commit
            or architecture != "aarch64"
            or key in actual
        ):
            raise SurfaceError(
                "apk installed 包版本、架构、来源或许可证与锁定清单不一致。"
            )
        actual[key] = record
        origins.add(origin)
    if (
        set(actual) != set(packages)
        or len(actual) != reference.package_count
        or len(origins) != reference.origin_count
    ):
        raise SurfaceError("apk installed 包或 origin 数量未闭合。")
    return actual


def payload_digests(record: list[str]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    directory: str | None = None
    pending: str | None = None
    for line in record:
        if line.startswith("F:"):
            if pending is not None:
                raise SurfaceError("apk installed 的 R/Z 摘要链不完整。")
            directory = line[2:]
            try:
                LICENSE_TOOL.validate_relative_path(
                    directory, "apk installed 的 F 目录"
                )
            except LICENSE_TOOL.LicenseLockError as error:
                raise SurfaceError(str(error)) from error
        elif line.startswith("R:"):
            filename = line[2:]
            if (
                directory is None
                or not filename
                or filename in (".", "..")
                or "/" in filename
                or "\\" in filename
                or pending is not None
            ):
                raise SurfaceError("apk installed 的 F/R 路径链非法。")
            pending = f"{directory}/{filename}"
        elif line.startswith("Z:"):
            if pending is None or not line[2:]:
                raise SurfaceError("apk installed 的 R/Z 摘要链非法。")
            result.setdefault(pending, []).append(line[2:])
            pending = None
    if pending is not None:
        raise SurfaceError("apk installed 的 R/Z 摘要链不完整。")
    return result


def validate_elf(payload: bytes, path: str) -> None:
    if len(payload) < 64 or payload[:4] != b"\x7fELF":
        raise SurfaceError(f"LGPL payload 不是 ELF：{path}")
    if payload[4] != 2 or payload[5] != 1 or payload[6] != 1:
        raise SurfaceError(f"LGPL payload 不是 little-endian ELF64：{path}")
    elf_type, machine, elf_version = struct.unpack_from("<HHI", payload, 16)
    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    header_size, program_entry_size, program_count = struct.unpack_from(
        "<HHH", payload, 52
    )
    if (
        elf_type != 3
        or machine != 183
        or elf_version != 1
        or header_size != 64
        or program_offset < header_size
        or program_entry_size != 56
        or program_count < 1
        or program_offset + program_entry_size * program_count
        > len(payload)
    ):
        raise SurfaceError(f"LGPL payload 不是 AArch64 ET_DYN：{path}")
    executable_load = False
    for index in range(program_count):
        offset = program_offset + index * program_entry_size
        (
            program_type,
            flags,
            file_offset,
            _virtual_address,
            _physical_address,
            file_size,
            memory_size,
            _alignment,
        ) = struct.unpack_from("<IIQQQQQQ", payload, offset)
        if (
            file_size > memory_size
            or file_offset > len(payload)
            or file_size > len(payload) - file_offset
        ):
            raise SurfaceError(f"LGPL payload 的程序段越界：{path}")
        if program_type == 1 and flags & 1 and file_size > 0:
            executable_load = True
    if not executable_load:
        raise SurfaceError(f"LGPL payload 缺少可执行 PT_LOAD：{path}")


def validate_payload_binary(
    payload: Payload, binary: bytes, records: dict[str, list[str]]
) -> None:
    ownership: list[tuple[str, str]] = []
    for package_key, record in records.items():
        ownership.extend(
            (package_key, digest)
            for digest in payload_digests(record).get(payload.path, [])
        )
    if (
        len(ownership) != 1
        or ownership[0][0] != payload.package
        or not ownership[0][1].startswith("Q1")
    ):
        raise SurfaceError(
            f"apk installed 没有唯一拥有 LGPL payload：{payload.path}"
        )
    try:
        expected_sha1 = base64.b64decode(
            ownership[0][1][2:], validate=True
        )
    except (ValueError, binascii.Error) as error:
        raise SurfaceError(
            f"apk installed 的 payload 摘要非法：{payload.path}"
        ) from error
    if (
        len(expected_sha1) != hashlib.sha1().digest_size
        or hashlib.sha1(binary).digest() != expected_sha1
    ):
        raise SurfaceError(
            f"apk installed 的 payload SHA-1 不匹配：{payload.path}"
        )
    validate_elf(binary, payload.path)


def validate_rootfs(
    archive_path: Path,
    packages_path: Path,
    payloads_path: Path,
    reference_path: Path,
) -> None:
    reference = parse_binary_reference(reference_path)
    package_data = read_regular(packages_path, "二进制包清单")
    if (
        len(package_data) != reference.packages_size
        or hashlib.sha256(package_data).hexdigest()
        != reference.packages_sha256
    ):
        raise SurfaceError("二进制包清单大小或 SHA-256 与参照不一致。")
    try:
        packages = LICENSE_TOOL.parse_packages(package_data)
    except LICENSE_TOOL.LicenseLockError as error:
        raise SurfaceError(str(error)) from error
    payloads = parse_payloads(payloads_path, packages)
    archive_data = read_regular(archive_path, "AArch64 rootfs 归档")
    if (
        len(archive_data) != reference.archive_size
        or hashlib.sha256(archive_data).hexdigest()
        != reference.archive_sha256
    ):
        raise SurfaceError("rootfs 归档大小或 SHA-256 与参照不一致。")

    wanted = {payload.path for payload in payloads.values()}
    payload_data: dict[str, list[bytes]] = {path: [] for path in wanted}
    installed: list[bytes] = []
    try:
        with tarfile.open(fileobj=io.BytesIO(archive_data), mode="r:gz") as archive:
            for member in archive:
                normalized = normalize_archive_path(member.name)
                if normalized is None:
                    raise SurfaceError(
                        "rootfs 含不安全或非规范的归档成员路径。"
                    )
                if normalized == "lib/apk/db/installed":
                    if not member.isfile():
                        raise SurfaceError(
                            "apk installed 数据库必须是普通文件。"
                        )
                    extracted = archive.extractfile(member)
                    if extracted is None:
                        raise SurfaceError("无法读取 apk installed 数据库。")
                    installed.append(extracted.read())
                if normalized not in wanted:
                    continue
                if not member.isfile() or member.mode & 0o111 == 0:
                    raise SurfaceError(
                        f"LGPL payload 必须是可执行普通文件：{normalized}"
                    )
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise SurfaceError(f"无法读取 LGPL payload：{normalized}")
                payload_data[normalized].append(extracted.read())
    except SurfaceError:
        raise
    except (tarfile.TarError, OSError) as error:
        raise SurfaceError("AArch64 rootfs 不是有效 gzip tar。") from error
    if len(installed) != 1:
        raise SurfaceError("rootfs 必须恰含一个 apk installed 数据库。")
    if (
        len(installed[0]) != reference.installed_size
        or hashlib.sha256(installed[0]).hexdigest()
        != reference.installed_sha256
    ):
        raise SurfaceError("apk installed 数据库大小或 SHA-256 与参照不一致。")
    records = validate_installed_packages(
        parse_installed_records(installed[0]), packages, reference
    )
    for payload in payloads.values():
        candidates = payload_data[payload.path]
        if len(candidates) != 1:
            raise SurfaceError(
                f"rootfs 必须恰含一个规范 LGPL payload：{payload.path}"
            )
        validate_payload_binary(payload, candidates[0], records)


def add_license_arguments(parser: argparse.ArgumentParser) -> None:
    LICENSE_TOOL.add_lock_arguments(parser)
    parser.add_argument("--payloads", type=Path, default=DEFAULT_PAYLOADS)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="验证 Alpine AArch64 LGPL 源码构建表面与二进制 payload"
    )
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser(
        "check-locks", help="验证 LGPL 源码事实与 payload 映射"
    )
    add_license_arguments(check)
    sources = commands.add_parser(
        "validate-sources", help="验证固定源码 cache 的真实构建表面"
    )
    sources.add_argument("cache", type=Path)
    add_license_arguments(sources)
    rootfs = commands.add_parser(
        "validate-rootfs", help="验证固定 rootfs 中的规范 AArch64 payload"
    )
    rootfs.add_argument("archive", type=Path)
    rootfs.add_argument("--packages", type=Path, default=DEFAULT_PACKAGES)
    rootfs.add_argument("--payloads", type=Path, default=DEFAULT_PAYLOADS)
    rootfs.add_argument(
        "--binary-reference", type=Path, default=DEFAULT_BINARY_REFERENCE
    )
    return parser


def main() -> int:
    args = make_parser().parse_args()
    try:
        if args.command == "validate-rootfs":
            validate_rootfs(
                args.archive,
                args.packages,
                args.payloads,
                args.binary_reference,
            )
            print("AArch64 rootfs 的 LGPL payload 验证通过")
            return 0
        try:
            locks = LICENSE_TOOL.load_locks(args)
        except LICENSE_TOOL.LicenseLockError as error:
            raise SurfaceError(str(error)) from error
        payloads = parse_payloads(args.payloads, locks.packages)
        validate_fact_locks(locks, payloads)
        if args.command == "check-locks":
            print("LGPL 源码事实与 payload 映射验证通过")
            return 0
        try:
            LICENSE_TOOL.validate_sources(args.cache, locks)
        except LICENSE_TOOL.LicenseLockError as error:
            raise SurfaceError(str(error)) from error
        validate_source_surface(args.cache, locks)
        print("LGPL 源码构建表面验证通过")
        return 0
    except SurfaceError as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

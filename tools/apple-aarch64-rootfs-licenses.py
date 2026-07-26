#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import errno
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tarfile
import urllib.parse


ROOT = Path(__file__).resolve().parent.parent
LOCK_ROOT = ROOT / "third_party" / "alpine" / "3.24.1-aarch64"
DEFAULT_PACKAGES = LOCK_ROOT / "packages.tsv"
DEFAULT_STATIC_LINK_SOURCES = LOCK_ROOT / "static-link-sources.tsv"
DEFAULT_ASSETS = LOCK_ROOT / "source-assets.tsv"
DEFAULT_INPUTS = LOCK_ROOT / "license-inputs.tsv"
DEFAULT_NOTICES = LOCK_ROOT / "THIRD-PARTY-NOTICES.txt"
INPUT_HEADER = (
    "origin\tpackages\tlicense\tsource_kind\tsource_asset\tsource_member"
    "\tsource_url\tsource_size\tsource_sha256\tnotice_section"
    "\tnotice_sha256"
)
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX128 = re.compile(r"^[0-9a-f]{128}$")
NAME = re.compile(r"^[a-z0-9][a-z0-9+._-]*$")
VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._~-]*$")
SECTION = re.compile(r"^[a-z0-9][a-z0-9.-]*$")
BEGIN = re.compile(
    rb"^===== BEGIN NOTICE: ([a-z0-9][a-z0-9.-]*) =====\n$"
)
END = re.compile(
    rb"^===== END NOTICE: ([a-z0-9][a-z0-9.-]*) =====\n$"
)


class LicenseLockError(Exception):
    pass


@dataclass(frozen=True)
class Package:
    name: str
    version: str
    origin: str
    license: str
    commit: str

    @property
    def key(self) -> str:
        return f"{self.name}@{self.version}"


@dataclass(frozen=True)
class Asset:
    kind: str
    origin: str
    bundle_path: str
    size: int
    sha512: str
    source_url: str


@dataclass(frozen=True)
class LicenseInput:
    origin: str
    packages: tuple[str, ...]
    license: str
    source_kind: str
    source_asset: str
    source_member: str
    source_url: str
    source_size: int
    source_sha256: str
    notice_section: str
    notice_sha256: str


@dataclass(frozen=True)
class StaticLinkSource:
    binary_package: str
    source_package: str
    source_version: str
    source_origin: str
    source_license: str
    snapshot_commit: str


@dataclass(frozen=True)
class Locks:
    root: Path
    packages: dict[str, Package]
    assets: dict[str, Asset]
    inputs: tuple[LicenseInput, ...]
    sections: dict[str, bytes]
    static_link_sources: dict[str, StaticLinkSource]


def read_text_lock(path: Path, description: str) -> bytes:
    descriptor = open_safe_relative(path.parent, path.name, description)
    try:
        data = read_descriptor(descriptor)
    finally:
        os.close(descriptor)
    if not data or not data.endswith(b"\n") or b"\r" in data:
        raise LicenseLockError(f"{description}必须是非空、仅使用 LF 的文本。")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise LicenseLockError(f"{description}不是 UTF-8 文本。") from error
    return data


def split_tsv(data: bytes, header: str, description: str) -> list[list[str]]:
    lines = data.decode("utf-8").splitlines()
    if lines[0] != header:
        raise LicenseLockError(f"{description}表头不符合固定格式。")
    rows = [line.split("\t") for line in lines[1:]]
    if not rows:
        raise LicenseLockError(f"{description}没有数据行。")
    return rows


def validate_relative_path(
    value: str, description: str
) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or str(path) != value
        or any(part in ("", ".", "..") for part in path.parts)
        or "\\" in value
        or any(
            ord(character) < 0x20 or ord(character) == 0x7F
            for character in value
        )
    ):
        raise LicenseLockError(f"{description}不是安全的相对路径。")
    return path


def validate_https_url(value: str, description: str) -> None:
    try:
        parsed = urllib.parse.urlsplit(value)
        hostname = parsed.hostname
        _port = parsed.port
    except ValueError as error:
        raise LicenseLockError(
            f"{description}必须是无凭据、无片段的 HTTPS URL。"
        ) from error
    if (
        parsed.scheme != "https"
        or not hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
        or "#" in value
        or any(
            ord(character) < 0x20
            or ord(character) == 0x7F
            or character.isspace()
            for character in value
        )
    ):
        raise LicenseLockError(f"{description}必须是无凭据、无片段的 HTTPS URL。")


def parse_packages(data: bytes) -> dict[str, Package]:
    rows = split_tsv(
        data,
        "package\tversion\torigin\tlicense\taports_commit",
        "二进制包清单",
    )
    packages: dict[str, Package] = {}
    commits: dict[str, str] = {}
    previous_name = ""
    for row in rows:
        if len(row) != 5 or any(not field for field in row):
            raise LicenseLockError("二进制包清单含空字段或列数错误。")
        name, version, origin, license_name, commit = row
        if (
            not NAME.fullmatch(name)
            or not VERSION.fullmatch(version)
            or not NAME.fullmatch(origin)
            or license_name != license_name.strip()
            or not HEX40.fullmatch(commit)
            or name <= previous_name
        ):
            raise LicenseLockError("二进制包清单排序、名称或版本格式非法。")
        package = Package(name, version, origin, license_name, commit)
        if package.key in packages:
            raise LicenseLockError("二进制包清单含重复的包版本元组。")
        known_commit = commits.setdefault(origin, commit)
        if known_commit != commit:
            raise LicenseLockError("同一 origin 对应了多个 aports 提交。")
        packages[package.key] = package
        previous_name = name
    if len(packages) != 16 or len(commits) != 10:
        raise LicenseLockError("二进制包清单必须恰含 16 个包和 10 个 origin。")
    return packages


def parse_static_link_sources(
    data: bytes, packages: dict[str, Package]
) -> dict[str, StaticLinkSource]:
    rows = split_tsv(
        data,
        (
            "binary_package\tsource_package\tsource_version\tsource_origin"
            "\tsource_license\taports_snapshot_commit"
        ),
        "静态链接来源清单",
    )
    binary_origins = {package.origin for package in packages.values()}
    sources: dict[str, StaticLinkSource] = {}
    previous_row: tuple[str, ...] | None = None
    for row in rows:
        if len(row) != 6 or any(not field for field in row):
            raise LicenseLockError("静态链接来源清单含空字段或列数错误。")
        row_tuple = tuple(row)
        (
            binary_package,
            source_package,
            source_version,
            source_origin,
            source_license,
            snapshot_commit,
        ) = row
        if previous_row is not None and row_tuple <= previous_row:
            raise LicenseLockError("静态链接来源清单未按完整记录唯一排序。")
        consumer = packages.get(binary_package)
        if consumer is None:
            raise LicenseLockError(
                f"静态链接来源声明了未知二进制包：{binary_package}"
            )
        if (
            not NAME.fullmatch(source_package)
            or not VERSION.fullmatch(source_version)
            or not NAME.fullmatch(source_origin)
            or source_package != f"{source_origin}-static"
            or source_license != "ISC"
            or not HEX40.fullmatch(snapshot_commit)
        ):
            raise LicenseLockError(
                "静态链接来源的包、版本、origin、许可或快照格式非法。"
            )
        if source_origin in binary_origins:
            raise LicenseLockError(
                "静态链接源码 origin 不能冒充二进制包 origin。"
            )
        if snapshot_commit != consumer.commit:
            raise LicenseLockError(
                "静态链接来源的 aports snapshot 与目标二进制包不一致。"
            )
        if source_origin in sources:
            raise LicenseLockError(
                "固定静态链接模型中每个源码 origin 必须恰有一条记录。"
            )
        sources[source_origin] = StaticLinkSource(
            binary_package,
            source_package,
            source_version,
            source_origin,
            source_license,
            snapshot_commit,
        )
        previous_row = row_tuple
    return sources


def parse_assets(
    data: bytes,
    packages: dict[str, Package],
    static_sources: dict[str, StaticLinkSource],
) -> dict[str, Asset]:
    rows = split_tsv(
        data,
        (
            "kind\torigin\taports_commit\tbundle_path\tsize\tsha512"
            "\tsource_url"
        ),
        "源码资产清单",
    )
    origin_commits = {
        package.origin: package.commit for package in packages.values()
    }
    # 静态源码 origin 使用消费方的 aports 快照，
    # 不把它解释成依赖 APK 的构建提交。
    origin_commits.update(
        {
            source.source_origin: source.snapshot_commit
            for source in static_sources.values()
        }
    )
    assets: dict[str, Asset] = {}
    aports_origins: set[str] = set()
    previous_path = ""
    for row in rows:
        if len(row) != 7 or any(not field for field in row):
            raise LicenseLockError("源码资产清单含空字段或列数错误。")
        kind, origin, commit, bundle_path, size_text, digest, source_url = row
        bundle = validate_relative_path(bundle_path, "源码资产路径")
        validate_https_url(source_url, "源码资产 URL")
        if (
            kind not in ("aports", "upstream")
            or origin not in origin_commits
            or commit != origin_commits[origin]
            or bundle_path <= previous_path
            or bundle_path in assets
            or not size_text.isdecimal()
            or str(int(size_text)) != size_text
            or int(size_text) < 1
            or not HEX128.fullmatch(digest)
        ):
            raise LicenseLockError("源码资产清单排序、来源、大小或摘要格式非法。")
        if kind == "aports":
            if (
                bundle_path != f"aports/{origin}.tar.gz"
                or origin in aports_origins
            ):
                raise LicenseLockError("aports 资产路径与 origin 不一致或重复。")
            aports_origins.add(origin)
        elif (
            len(bundle.parts) < 3
            or bundle.parts[0] != "distfiles"
            or bundle.parts[1] != origin
        ):
            raise LicenseLockError("上游资产必须位于对应 origin 的 distfiles。")
        assets[bundle_path] = Asset(
            kind,
            origin,
            bundle_path,
            int(size_text),
            digest,
            source_url,
        )
        previous_path = bundle_path
    if (
        len(assets) != 23
        or len(origin_commits) != 12
        or aports_origins != set(origin_commits)
    ):
        raise LicenseLockError(
            "源码资产清单必须恰含 23 项、覆盖 12 个 origin，"
            "且每个 origin 恰有一份 aports 资产。"
        )
    return assets


def parse_notice_sections(data: bytes) -> dict[str, bytes]:
    sections: dict[str, bytes] = {}
    current: str | None = None
    body: list[bytes] = []
    for line in data.splitlines(keepends=True):
        begin = BEGIN.fullmatch(line)
        end = END.fullmatch(line)
        if line.startswith(b"===== BEGIN NOTICE:") and begin is None:
            raise LicenseLockError("第三方声明含格式非法的 BEGIN marker。")
        if line.startswith(b"===== END NOTICE:") and end is None:
            raise LicenseLockError("第三方声明含格式非法的 END marker。")
        if begin is not None:
            if current is not None:
                raise LicenseLockError("第三方声明的 section marker 发生嵌套。")
            name = begin.group(1).decode("ascii")
            if name in sections:
                raise LicenseLockError("第三方声明含重复的 section。")
            current = name
            body = []
            continue
        if end is not None:
            name = end.group(1).decode("ascii")
            if current is None or name != current:
                raise LicenseLockError("第三方声明的 BEGIN/END marker 不匹配。")
            section_body = b"".join(body)
            if not section_body or not section_body.endswith(b"\n"):
                raise LicenseLockError("第三方声明的 section body 必须非空并以换行结束。")
            sections[current] = section_body
            current = None
            body = []
            continue
        if current is not None:
            body.append(line)
        elif line != b"\n":
            raise LicenseLockError("第三方声明含未受 section 摘要保护的正文。")
    if current is not None:
        raise LicenseLockError("第三方声明含未闭合的 section。")
    if not sections:
        raise LicenseLockError("第三方声明没有可验证的 section。")
    return sections


def parse_package_list(
    value: str,
    origin: str,
    license_name: str,
    packages: dict[str, Package],
    static_sources: dict[str, StaticLinkSource],
) -> tuple[str, ...]:
    declared = value.split(",")
    if (
        any(not item for item in declared)
        or declared != sorted(set(declared))
    ):
        raise LicenseLockError("许可输入的 packages 必须按唯一包版本元组排序。")
    static_source = static_sources.get(origin)
    for key in declared:
        package = packages.get(key)
        if package is None:
            raise LicenseLockError(f"许可输入声明了未知包版本：{key}")
        if static_source is not None:
            matches_source = (
                key == static_source.binary_package
                and license_name == static_source.source_license
            )
        else:
            matches_source = (
                package.origin == origin and package.license == license_name
            )
        if not matches_source:
            raise LicenseLockError(
                f"许可输入与包清单的 origin 或 license 漂移：{key}"
            )
    return tuple(declared)


def parse_inputs(
    data: bytes,
    packages: dict[str, Package],
    assets: dict[str, Asset],
    static_sources: dict[str, StaticLinkSource],
) -> tuple[tuple[LicenseInput, ...], dict[str, str]]:
    rows = split_tsv(data, INPUT_HEADER, "许可输入清单")
    inputs: list[LicenseInput] = []
    package_coverage: set[str] = set()
    asset_coverage: set[str] = set()
    section_hashes: dict[str, str] = {}
    source_locks: dict[tuple[str, str, str], tuple[int, str, str]] = {}
    previous_row: tuple[str, ...] | None = None
    for row in rows:
        if len(row) != 11 or any(not field for field in row):
            raise LicenseLockError("许可输入清单含空字段或列数错误。")
        row_tuple = tuple(row)
        if previous_row is not None and row_tuple <= previous_row:
            raise LicenseLockError("许可输入清单未按完整记录唯一排序。")
        (
            origin,
            package_text,
            license_name,
            source_kind,
            source_asset,
            source_member,
            source_url,
            source_size_text,
            source_sha256,
            notice_section,
            notice_sha256,
        ) = row
        if not NAME.fullmatch(origin):
            raise LicenseLockError("许可输入含非法 origin。")
        declared_packages = parse_package_list(
            package_text,
            origin,
            license_name,
            packages,
            static_sources,
        )
        if source_kind not in ("aports", "upstream", "authority"):
            raise LicenseLockError("许可输入的 source_kind 非法。")
        validate_relative_path(source_asset, "许可输入的 source_asset")
        validate_https_url(source_url, "许可输入的 source_url")
        if (
            not source_size_text.isdecimal()
            or str(int(source_size_text)) != source_size_text
            or int(source_size_text) < 1
            or not HEX64.fullmatch(source_sha256)
            or not SECTION.fullmatch(notice_section)
            or not HEX64.fullmatch(notice_sha256)
        ):
            raise LicenseLockError("许可输入的大小、摘要或 section 格式非法。")
        source_key = (source_kind, source_asset, source_member)
        source_lock = (
            int(source_size_text),
            source_sha256,
            source_url,
        )
        known_source_lock = source_locks.setdefault(source_key, source_lock)
        if known_source_lock != source_lock:
            raise LicenseLockError("同一许可源码输入对应了不一致的锁定值。")
        if source_kind in ("aports", "upstream"):
            asset = assets.get(source_asset)
            if (
                asset is None
                or asset.kind != source_kind
                or asset.origin != origin
                or asset.source_url != source_url
            ):
                raise LicenseLockError("许可输入与源码资产清单不一致。")
            if source_member != "-":
                validate_relative_path(source_member, "许可输入的归档成员")
            elif source_kind == "aports":
                raise LicenseLockError("aports 许可输入必须引用归档成员。")
            asset_coverage.add(source_asset)
        else:
            asset_path = validate_relative_path(
                source_asset, "权威许可输入路径"
            )
            if len(asset_path.parts) < 2 or asset_path.parts[0] != "license-inputs":
                raise LicenseLockError("权威许可输入必须位于 license-inputs/。")
            if source_member != "-":
                raise LicenseLockError("权威许可输入的 source_member 必须为 -。")
        known_section_hash = section_hashes.setdefault(
            notice_section, notice_sha256
        )
        if known_section_hash != notice_sha256:
            raise LicenseLockError("同一声明 section 对应了多个摘要。")
        package_coverage.update(declared_packages)
        inputs.append(
            LicenseInput(
                origin,
                declared_packages,
                license_name,
                source_kind,
                source_asset,
                source_member,
                source_url,
                int(source_size_text),
                source_sha256,
                notice_section,
                notice_sha256,
            )
        )
        previous_row = row_tuple
    if package_coverage != set(packages):
        missing = len(set(packages) - package_coverage)
        extra = len(package_coverage - set(packages))
        raise LicenseLockError(
            f"许可输入未闭合包清单（缺少 {missing} 项，多余 {extra} 项）。"
        )
    if asset_coverage != set(assets):
        missing = len(set(assets) - asset_coverage)
        extra = len(asset_coverage - set(assets))
        raise LicenseLockError(
            f"许可输入未闭合源码资产清单（缺少 {missing} 项，多余 {extra} 项）。"
        )
    return tuple(inputs), section_hashes


def open_safe_relative(root: Path, relative: str, description: str) -> int:
    path = validate_relative_path(relative, description)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        raise LicenseLockError(
            "当前平台不支持安全的逐级目录打开，无法验证许可输入。"
        )
    common_flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NONBLOCK", 0)
        | nofollow
    )
    directory_flags = common_flags | directory
    # root 是调用者选择的信任锚；清单提供的相对路径再逐级固定到 dirfd。
    try:
        current_descriptor = os.open(root, directory_flags)
    except OSError as error:
        raise LicenseLockError(f"无法打开{description}的根目录：{error}") from error
    try:
        for part in path.parts[:-1]:
            try:
                next_descriptor = os.open(
                    part, directory_flags, dir_fd=current_descriptor
                )
            except OSError as error:
                if error.errno in (errno.ELOOP, errno.ENOENT, errno.ENOTDIR):
                    raise LicenseLockError(
                        f"{description}路径不存在、含符号链接或非实体目录。"
                    ) from error
                raise LicenseLockError(
                    f"无法打开{description}的父目录：{error}"
                ) from error
            os.close(current_descriptor)
            current_descriptor = next_descriptor
        try:
            descriptor = os.open(
                path.parts[-1], common_flags, dir_fd=current_descriptor
            )
        except OSError as error:
            if error.errno in (errno.ELOOP, errno.ENOENT, errno.ENOTDIR):
                raise LicenseLockError(
                    f"{description}不存在或是符号链接。"
                ) from error
            raise LicenseLockError(f"无法打开{description}：{error}") from error
    finally:
        os.close(current_descriptor)
    try:
        metadata = os.fstat(descriptor)
    except OSError as error:
        try:
            os.close(descriptor)
        except OSError:
            pass
        raise LicenseLockError(
            f"无法检查{description}的文件类型：{error}"
        ) from error
    if not stat.S_ISREG(metadata.st_mode):
        os.close(descriptor)
        raise LicenseLockError(f"{description}必须是普通文件。")
    return descriptor


def read_descriptor(descriptor: int, maximum_size: int | None = None) -> bytes:
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks = []
    size = 0
    while True:
        read_size = 1024 * 1024
        if maximum_size is not None:
            read_size = min(read_size, maximum_size + 1 - size)
            if read_size <= 0:
                break
        chunk = os.read(descriptor, read_size)
        if not chunk:
            break
        chunks.append(chunk)
        size += len(chunk)
    os.lseek(descriptor, 0, os.SEEK_SET)
    return b"".join(chunks)


def validate_authority_inputs(
    root: Path,
    inputs: tuple[LicenseInput, ...],
    sections: dict[str, bytes],
) -> None:
    authority_files: dict[tuple[str, int, str], bytes] = {}
    checked_sections: set[tuple[str, str]] = set()
    for item in inputs:
        if item.source_kind != "authority":
            continue
        file_key = (item.source_asset, item.source_size, item.source_sha256)
        if file_key not in authority_files:
            descriptor = open_safe_relative(
                root, item.source_asset, "权威许可输入"
            )
            try:
                if os.fstat(descriptor).st_size != item.source_size:
                    raise LicenseLockError(
                        "权威许可输入大小或 SHA-256 不匹配："
                        f"{item.source_asset}"
                    )
                authority = read_descriptor(descriptor, item.source_size)
            finally:
                os.close(descriptor)
            if (
                len(authority) != item.source_size
                or hashlib.sha256(authority).hexdigest() != item.source_sha256
            ):
                raise LicenseLockError(
                    f"权威许可输入大小或 SHA-256 不匹配：{item.source_asset}"
                )
            authority_files[file_key] = authority
        section_key = (item.source_asset, item.notice_section)
        if section_key in checked_sections:
            continue
        authority = authority_files[file_key]
        if sections[item.notice_section].count(authority) != 1:
            raise LicenseLockError(
                "权威许可输入必须逐字且唯一出现在声明 section："
                f"{item.source_asset}"
            )
        checked_sections.add(section_key)


def validate_notices(
    sections: dict[str, bytes], expected_hashes: dict[str, str]
) -> None:
    if set(sections) != set(expected_hashes):
        missing = len(set(expected_hashes) - set(sections))
        extra = len(set(sections) - set(expected_hashes))
        raise LicenseLockError(
            f"第三方声明 section 未闭合（缺少 {missing} 项，多余 {extra} 项）。"
        )
    for name, expected in expected_hashes.items():
        actual = hashlib.sha256(sections[name]).hexdigest()
        if actual != expected:
            raise LicenseLockError(f"第三方声明 section 摘要不匹配：{name}")


def load_locks(args: argparse.Namespace) -> Locks:
    packages = parse_packages(read_text_lock(args.packages, "二进制包清单"))
    static_sources = parse_static_link_sources(
        read_text_lock(args.static_link_sources, "静态链接来源清单"),
        packages,
    )
    assets = parse_assets(
        read_text_lock(args.assets, "源码资产清单"),
        packages,
        static_sources,
    )
    inputs, section_hashes = parse_inputs(
        read_text_lock(args.inputs, "许可输入清单"),
        packages,
        assets,
        static_sources,
    )
    notices = read_text_lock(args.notices, "第三方声明")
    sections = parse_notice_sections(notices)
    validate_notices(sections, section_hashes)
    lock_root = args.inputs.parent
    validate_authority_inputs(lock_root, inputs, sections)
    return Locks(
        lock_root,
        packages,
        assets,
        inputs,
        sections,
        static_sources,
    )


def validate_member(
    archive: tarfile.TarFile,
    member: tarfile.TarInfo,
    item: LicenseInput,
) -> None:
    if not member.isfile():
        raise LicenseLockError(
            f"许可源码成员必须是普通文件：{item.source_member}"
        )
    extracted = archive.extractfile(member)
    if extracted is None:
        raise LicenseLockError(f"无法读取许可源码成员：{item.source_member}")
    digest = hashlib.sha256()
    size = 0
    while True:
        chunk = extracted.read(1024 * 1024)
        if not chunk:
            break
        size += len(chunk)
        digest.update(chunk)
    if size != item.source_size or digest.hexdigest() != item.source_sha256:
        raise LicenseLockError(
            f"许可源码成员大小或 SHA-256 不匹配：{item.source_member}"
        )


def validate_asset_sources(
    cache: Path,
    asset: Asset,
    inputs: list[LicenseInput],
    static_source: StaticLinkSource | None,
) -> None:
    descriptor = open_safe_relative(cache, asset.bundle_path, "源码缓存资产")
    try:
        if os.fstat(descriptor).st_size != asset.size:
            raise LicenseLockError(
                f"源码缓存资产大小或 SHA-512 不匹配：{asset.bundle_path}"
            )
        # 摘要与归档解析共用一次读取快照，避免可写缓存两次读取间漂移。
        asset_data = read_descriptor(descriptor, asset.size)
    finally:
        os.close(descriptor)
    size = len(asset_data)
    if size != asset.size or hashlib.sha512(asset_data).hexdigest() != asset.sha512:
        raise LicenseLockError(
            f"源码缓存资产大小或 SHA-512 不匹配：{asset.bundle_path}"
        )
    raw_inputs = [item for item in inputs if item.source_member == "-"]
    archive_inputs = [item for item in inputs if item.source_member != "-"]
    for item in raw_inputs:
        if (
            size != item.source_size
            or hashlib.sha256(asset_data).hexdigest() != item.source_sha256
        ):
            raise LicenseLockError(
                f"原始许可源码大小或 SHA-256 不匹配：{asset.bundle_path}"
            )
    if not archive_inputs:
        return
    requested = {item.source_member: item for item in archive_inputs}
    found: dict[str, int] = {name: 0 for name in requested}
    apkbuild_count = 0
    apkbuild_data: bytes | None = None
    try:
        with tarfile.open(fileobj=io.BytesIO(asset_data), mode="r:*") as archive:
            for member in archive:
                if (
                    static_source is not None
                    and PurePosixPath(member.name).name == "APKBUILD"
                ):
                    apkbuild_count += 1
                    if apkbuild_count > 1 or not member.isfile():
                        raise LicenseLockError(
                            "静态源码 aports 归档必须恰含一个普通 APKBUILD。"
                        )
                    extracted = archive.extractfile(member)
                    if extracted is None:
                        raise LicenseLockError(
                            "静态源码 aports 归档无法读取 APKBUILD。"
                        )
                    apkbuild_data = extracted.read()
                item = requested.get(member.name)
                if item is None:
                    continue
                found[member.name] += 1
                if found[member.name] > 1:
                    raise LicenseLockError(
                        f"许可源码归档含重复成员：{member.name}"
                    )
                validate_member(archive, member, item)
    except LicenseLockError:
        raise
    except (tarfile.TarError, OSError, UnicodeError) as error:
        raise LicenseLockError(
            f"许可源码资产不是有效归档：{asset.bundle_path}"
        ) from error
    if static_source is not None:
        if apkbuild_count != 1 or apkbuild_data is None:
            raise LicenseLockError(
                "静态源码 aports 归档必须恰含一个普通 APKBUILD。"
            )
        version, separator, release = static_source.source_version.rpartition(
            "-r"
        )
        expected_assignments = (
            (
                b"pkgname=",
                f"pkgname={static_source.source_origin}\n".encode(),
            ),
            (b"pkgver=", f"pkgver={version}\n".encode()),
            (b"pkgrel=", f"pkgrel={release}\n".encode()),
        )
        apkbuild_lines = apkbuild_data.splitlines(keepends=True)
        if (
            not separator
            or not version
            or not release.isdecimal()
            or any(
                [
                    line
                    for line in apkbuild_lines
                    if line.startswith(prefix)
                ]
                != [expected]
                for prefix, expected in expected_assignments
            )
        ):
            raise LicenseLockError(
                "静态链接来源版本与 APKBUILD 的 "
                f"pkgname/pkgver/pkgrel 不一致：{static_source.source_origin}"
            )
    missing = [name for name, count in found.items() if count == 0]
    if missing:
        raise LicenseLockError(f"许可源码归档缺少成员：{missing[0]}")


def validate_sources(cache: Path, locks: Locks) -> None:
    grouped: dict[str, list[LicenseInput]] = {
        path: [] for path in locks.assets
    }
    for item in locks.inputs:
        if item.source_kind in ("aports", "upstream"):
            grouped[item.source_asset].append(item)
    for path, asset in locks.assets.items():
        static_source = (
            locks.static_link_sources.get(asset.origin)
            if asset.kind == "aports"
            else None
        )
        validate_asset_sources(
            cache, asset, grouped[path], static_source
        )


def add_lock_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--packages", type=Path, default=DEFAULT_PACKAGES)
    parser.add_argument(
        "--static-link-sources",
        type=Path,
        default=DEFAULT_STATIC_LINK_SOURCES,
    )
    parser.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--inputs", type=Path, default=DEFAULT_INPUTS)
    parser.add_argument("--notices", type=Path, default=DEFAULT_NOTICES)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="验证 Alpine AArch64 rootfs 的许可输入与第三方声明"
    )
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check-locks", help="纯离线验证受跟踪锁定文件")
    add_lock_arguments(check)
    validate = commands.add_parser(
        "validate-sources", help="验证源码缓存中的许可输入"
    )
    validate.add_argument("cache", type=Path)
    add_lock_arguments(validate)
    return parser


def main() -> int:
    args = make_parser().parse_args()
    try:
        locks = load_locks(args)
        if args.command == "check-locks":
            print(
                "许可锁定文件验证通过："
                f"{len(locks.packages)} 个包，{len(locks.assets)} 份源码资产，"
                f"{len(locks.sections)} 个声明 section"
            )
            return 0
        validate_sources(args.cache, locks)
        print("许可源码输入验证通过")
        return 0
    except LicenseLockError as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

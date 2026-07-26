#!/usr/bin/env python3

from __future__ import annotations

import argparse
import errno
import hashlib
import io
import os
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import secrets
import stat
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = ROOT / "third_party" / "alpine" / "3.24.1-aarch64"
DEFAULT_PACKAGES = SOURCE_ROOT / "packages.tsv"
DEFAULT_STATIC_LINK_SOURCES = SOURCE_ROOT / "static-link-sources.tsv"
DEFAULT_ORIGINS = SOURCE_ROOT / "origins.tsv"
DEFAULT_ASSETS = SOURCE_ROOT / "source-assets.tsv"
DEFAULT_BINARY_REFERENCE = SOURCE_ROOT / "binary-reference.tsv"
DEFAULT_README = SOURCE_ROOT / "SOURCE-BUNDLE.md"
DEFAULT_CHECKSUM = SOURCE_ROOT / "corresponding-source.sha256"
BUNDLE_FILENAME = (
    "alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar"
)
BUNDLE_ROOT = BUNDLE_FILENAME[:-4]
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX128 = re.compile(r"^[0-9a-f]{128}$")
ORIGIN_NAME = re.compile(r"^[a-z0-9][a-z0-9+._-]*$")
VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._~-]*$")
PRODUCTION_STATIC_LINK_SOURCES = frozenset(
    {
        (
            "busybox@1.37.0-r31",
            "skalibs-static",
            "2.14.5.0-r0",
            "skalibs",
            "ISC",
        ),
        (
            "busybox@1.37.0-r31",
            "utmps-static",
            "0.1.3.2-r0",
            "utmps",
            "ISC",
        ),
    }
)


class SourceBundleError(Exception):
    pass


@dataclass(frozen=True)
class Origin:
    name: str
    commit: str
    aports_path: str
    tree_sha1: str
    entry_count: int


@dataclass(frozen=True)
class Asset:
    kind: str
    origin: str
    commit: str
    bundle_path: str
    size: int
    sha512: str
    source_url: str


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
    packages_bytes: bytes
    static_link_sources_bytes: bytes
    origins_bytes: bytes
    assets_bytes: bytes
    binary_reference_bytes: bytes
    readme_bytes: bytes
    origins: dict[str, Origin]
    assets: tuple[Asset, ...]
    static_link_sources: tuple[StaticLinkSource, ...]


def open_safe_file(path: Path, description: str) -> int:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        raise SourceBundleError(
            "当前平台不支持安全的目录相对打开，无法验证源码输入。"
        )
    common_flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NONBLOCK", 0)
        | nofollow
    )
    try:
        parent_descriptor = os.open(path.parent, common_flags | directory)
    except OSError as error:
        raise SourceBundleError(f"无法打开{description}的父目录：{error}") from error
    try:
        try:
            descriptor = os.open(
                path.name, common_flags, dir_fd=parent_descriptor
            )
        except OSError as error:
            if error.errno in (errno.ELOOP, errno.ENOENT, errno.ENOTDIR):
                raise SourceBundleError(
                    f"{description}不存在或是符号链接：{path}"
                ) from error
            raise SourceBundleError(f"无法打开{description}：{error}") from error
    finally:
        os.close(parent_descriptor)
    try:
        metadata = os.fstat(descriptor)
    except OSError as error:
        os.close(descriptor)
        raise SourceBundleError(f"无法读取{description}元数据：{error}") from error
    if not stat.S_ISREG(metadata.st_mode):
        os.close(descriptor)
        raise SourceBundleError(f"{description}必须是普通文件：{path}")
    return descriptor


def read_descriptor(descriptor: int, maximum_size: int | None = None) -> bytes:
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks: list[bytes] = []
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


def read_lock(path: Path, description: str) -> bytes:
    descriptor = open_safe_file(path, description)
    try:
        data = read_descriptor(descriptor)
    finally:
        os.close(descriptor)
    if not data or not data.endswith(b"\n") or b"\r" in data:
        raise SourceBundleError(f"{description}必须使用非空的 LF 结尾格式。")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SourceBundleError(f"{description}不是 UTF-8 文本。") from error
    return data


def split_tsv(data: bytes, header: str, description: str) -> list[list[str]]:
    lines = data.decode("utf-8").splitlines()
    if lines[0] != header:
        raise SourceBundleError(f"{description}表头不符合固定格式。")
    rows = [line.split("\t") for line in lines[1:]]
    if not rows:
        raise SourceBundleError(f"{description}没有数据行。")
    return rows


def parse_packages(
    data: bytes,
) -> tuple[int, dict[str, str], dict[str, str]]:
    rows = split_tsv(
        data,
        "package\tversion\torigin\tlicense\taports_commit",
        "二进制包清单",
    )
    packages: set[str] = set()
    package_keys: dict[str, str] = {}
    origins: dict[str, str] = {}
    previous_package = ""
    for row in rows:
        if len(row) != 5 or any(not field for field in row):
            raise SourceBundleError("二进制包清单含空字段或列数错误。")
        package, version, origin, _license, commit = row
        if (
            not ORIGIN_NAME.fullmatch(package)
            or not VERSION.fullmatch(version)
            or not ORIGIN_NAME.fullmatch(origin)
            or not HEX40.fullmatch(commit)
            or package in packages
            or package <= previous_package
        ):
            raise SourceBundleError("二进制包清单未按唯一包名排序或来源格式非法。")
        packages.add(package)
        package_keys[f"{package}@{version}"] = commit
        previous_package = package
        known_commit = origins.setdefault(origin, commit)
        if known_commit != commit:
            raise SourceBundleError("同一 apk origin 不能对应多个 aports 提交。")
    return len(packages), origins, package_keys


def parse_static_link_sources(
    data: bytes,
    package_origins: dict[str, str],
    package_keys: dict[str, str],
    enforce_production_model: bool,
) -> tuple[tuple[StaticLinkSource, ...], dict[str, str]]:
    rows = split_tsv(
        data,
        (
            "binary_package\tsource_package\tsource_version\tsource_origin"
            "\tsource_license\taports_snapshot_commit"
        ),
        "静态链接来源清单",
    )
    sources: list[StaticLinkSource] = []
    origin_commits: dict[str, str] = {}
    source_origins: set[str] = set()
    previous_row: tuple[str, ...] | None = None
    for row in rows:
        if len(row) != 6 or any(not field for field in row):
            raise SourceBundleError("静态链接来源清单含空字段或列数错误。")
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
            raise SourceBundleError("静态链接来源清单未按完整记录唯一排序。")
        if binary_package not in package_keys:
            raise SourceBundleError(
                f"静态链接来源声明了未知二进制包：{binary_package}"
            )
        if (
            not ORIGIN_NAME.fullmatch(source_package)
            or not VERSION.fullmatch(source_version)
            or not ORIGIN_NAME.fullmatch(source_origin)
            or source_package != f"{source_origin}-static"
            or source_license != "ISC"
            or not HEX40.fullmatch(snapshot_commit)
        ):
            raise SourceBundleError(
                "静态链接来源的包、版本、origin、许可或快照格式非法。"
            )
        if source_origin in package_origins:
            raise SourceBundleError(
                "静态链接源码 origin 不能冒充二进制包 origin。"
            )
        if snapshot_commit != package_keys[binary_package]:
            raise SourceBundleError(
                "静态链接来源的 aports snapshot 与目标二进制包不一致。"
            )
        if source_origin in source_origins:
            raise SourceBundleError(
                "固定静态链接模型中每个源码 origin 必须恰有一条记录。"
            )
        source_origins.add(source_origin)
        origin_commits[source_origin] = snapshot_commit
        sources.append(
            StaticLinkSource(
                binary_package,
                source_package,
                source_version,
                source_origin,
                source_license,
                snapshot_commit,
            )
        )
        previous_row = row_tuple
    if enforce_production_model:
        actual_model = frozenset(
            (
                source.binary_package,
                source.source_package,
                source.source_version,
                source.source_origin,
                source.source_license,
            )
            for source in sources
        )
        if actual_model != PRODUCTION_STATIC_LINK_SOURCES:
            raise SourceBundleError("固定 Alpine 静态链接来源语义集合不一致。")
    return tuple(sources), origin_commits


def parse_origins(
    data: bytes,
    package_origins: dict[str, str],
    static_origin_commits: dict[str, str],
) -> dict[str, Origin]:
    rows = split_tsv(
        data,
        "origin\taports_commit\taports_path\torigin_tree_sha1\tentry_count",
        "aports origin 清单",
    )
    origins: dict[str, Origin] = {}
    previous_origin = ""
    for row in rows:
        if len(row) != 5 or any(not field for field in row):
            raise SourceBundleError("aports origin 清单含空字段或列数错误。")
        name, commit, aports_path, tree_sha1, count_text = row
        if (
            not ORIGIN_NAME.fullmatch(name)
            or name <= previous_origin
            or not HEX40.fullmatch(commit)
            or aports_path != f"main/{name}"
            or not HEX40.fullmatch(tree_sha1)
            or not count_text.isdecimal()
            or str(int(count_text)) != count_text
            or int(count_text) < 1
        ):
            raise SourceBundleError("aports origin 清单排序、路径或摘要格式非法。")
        origins[name] = Origin(
            name, commit, aports_path, tree_sha1, int(count_text)
        )
        previous_origin = name
    # 静态源码 origin 锁定消费方构建时的 aports 全树快照，
    # 不代表依赖 APK 的构建提交。
    expected_commits = {**package_origins, **static_origin_commits}
    if set(origins) != set(expected_commits):
        raise SourceBundleError(
            "aports origin 清单与二进制包及静态链接源码集合不一致。"
        )
    for name, commit in expected_commits.items():
        if origins[name].commit != commit:
            raise SourceBundleError(
                "aports origin 的锁定提交与包提交或静态链接源码快照不一致。"
            )
    return origins


def validate_url(url: str, allow_local_sources: bool) -> None:
    try:
        parsed = urllib.parse.urlsplit(url)
        hostname = parsed.hostname
        parsed.port
    except ValueError as error:
        raise SourceBundleError("源码 URL 格式非法。") from error
    if (
        parsed.fragment
        or "#" in url
        or any(character.isspace() for character in url)
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in url)
    ):
        raise SourceBundleError("源码 URL 含片段、空白或控制字符。")
    if parsed.scheme == "https":
        if (
            not hostname
            or parsed.username is not None
            or parsed.password is not None
        ):
            raise SourceBundleError("HTTPS 源码 URL 格式非法。")
        return
    if (
        allow_local_sources
        and parsed.scheme == "file"
        and not parsed.netloc
        and parsed.path.startswith("/")
        and not parsed.query
    ):
        return
    raise SourceBundleError("源码 URL 必须使用 HTTPS。")


def validate_bundle_path(path_text: str) -> PurePosixPath:
    path = PurePosixPath(path_text)
    if (
        path.is_absolute()
        or str(path) != path_text
        or any(part in ("", ".", "..") for part in path.parts)
        or "\\" in path_text
        or any(
            ord(character) < 0x20 or ord(character) == 0x7F
            for character in path_text
        )
    ):
        raise SourceBundleError("源码包相对路径非法。")
    return path


def parse_assets(
    data: bytes,
    origins: dict[str, Origin],
    allow_local_sources: bool,
) -> tuple[Asset, ...]:
    rows = split_tsv(
        data,
        (
            "kind\torigin\taports_commit\tbundle_path\tsize\tsha512"
            "\tsource_url"
        ),
        "源码资产清单",
    )
    assets: list[Asset] = []
    paths: set[str] = set()
    aports_origins: set[str] = set()
    previous_path = ""
    for row in rows:
        if len(row) != 7 or any(not field for field in row):
            raise SourceBundleError("源码资产清单含空字段或列数错误。")
        kind, origin, commit, path_text, size_text, digest, source_url = row
        path = validate_bundle_path(path_text)
        if (
            kind not in ("aports", "upstream")
            or origin not in origins
            or commit != origins[origin].commit
            or path_text <= previous_path
            or path_text in paths
            or not size_text.isdecimal()
            or str(int(size_text)) != size_text
            or int(size_text) < 1
            or not HEX128.fullmatch(digest)
        ):
            raise SourceBundleError("源码资产清单排序、来源、大小或摘要格式非法。")
        if kind == "aports":
            if path_text != f"aports/{origin}.tar.gz":
                raise SourceBundleError("aports 资产路径与 origin 不一致。")
            if origin in aports_origins:
                raise SourceBundleError("同一 origin 只能有一份 aports 资产。")
            aports_origins.add(origin)
        elif (
            len(path.parts) < 3
            or path.parts[0] != "distfiles"
            or path.parts[1] != origin
        ):
            raise SourceBundleError("上游资产必须位于对应 origin 的 distfiles。")
        validate_url(source_url, allow_local_sources)
        assets.append(
            Asset(
                kind,
                origin,
                commit,
                path_text,
                int(size_text),
                digest,
                source_url,
            )
        )
        paths.add(path_text)
        previous_path = path_text
    if aports_origins != set(origins):
        raise SourceBundleError("每个锁定 origin 必须恰有一份 aports 资产。")
    return tuple(assets)


def parse_binary_reference(
    data: bytes,
    packages_bytes: bytes,
    package_count: int,
    origin_count: int,
) -> None:
    rows = split_tsv(
        data,
        (
            "alpine_version\tarchive_name\tarchive_size\tarchive_sha256"
            "\tsource_url"
            "\tinstalled_size\tinstalled_sha256\tpackages_size"
            "\tpackages_sha256\tpackage_count\torigin_count"
        ),
        "二进制参照清单",
    )
    if len(rows) != 1 or len(rows[0]) != 11:
        raise SourceBundleError("二进制参照清单必须只有一条十一列记录。")
    (
        alpine_version,
        archive_name,
        archive_size,
        archive_sha256,
        source_url,
        installed_size,
        installed_sha256,
        packages_size,
        packages_sha256,
        package_count_text,
        origin_count_text,
    ) = rows[0]
    numeric = (
        archive_size,
        installed_size,
        packages_size,
        package_count_text,
        origin_count_text,
    )
    if (
        not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", alpine_version)
        or "/" in archive_name
        or not archive_name
        or any(not value.isdecimal() or str(int(value)) != value for value in numeric)
        or any(int(value) < 1 for value in numeric)
        or any(
            not HEX64.fullmatch(value)
            for value in (
                archive_sha256,
                installed_sha256,
                packages_sha256,
            )
        )
    ):
        raise SourceBundleError("二进制参照清单名称、大小或摘要格式非法。")
    validate_url(source_url, False)
    expected_source_url = (
        "https://dl-cdn.alpinelinux.org/alpine/"
        f"v{alpine_version.rsplit('.', 1)[0]}/releases/aarch64/"
        f"{archive_name}"
    )
    if (
        archive_name
        != f"alpine-minirootfs-{alpine_version}-aarch64.tar.gz"
        or source_url != expected_source_url
        or int(packages_size) != len(packages_bytes)
        or packages_sha256 != hashlib.sha256(packages_bytes).hexdigest()
        or int(package_count_text) != package_count
        or int(origin_count_text) != origin_count
    ):
        raise SourceBundleError("二进制参照清单与受跟踪包清单不一致。")


def load_locks(args: argparse.Namespace) -> Locks:
    packages_bytes = read_lock(args.packages, "二进制包清单")
    package_count, package_origins, package_keys = parse_packages(packages_bytes)
    static_link_sources_bytes = read_lock(
        args.static_link_sources, "静态链接来源清单"
    )
    static_sources, static_origin_commits = parse_static_link_sources(
        static_link_sources_bytes,
        package_origins,
        package_keys,
        not args.allow_local_sources,
    )
    origins_bytes = read_lock(args.origins, "aports origin 清单")
    origins = parse_origins(
        origins_bytes, package_origins, static_origin_commits
    )
    assets_bytes = read_lock(args.assets, "源码资产清单")
    assets = parse_assets(
        assets_bytes, origins, args.allow_local_sources
    )
    binary_reference_bytes = read_lock(
        args.binary_reference, "二进制参照清单"
    )
    parse_binary_reference(
        binary_reference_bytes,
        packages_bytes,
        package_count,
        len(package_origins),
    )
    readme_bytes = read_lock(args.readme, "源码包说明")
    return Locks(
        packages_bytes,
        static_link_sources_bytes,
        origins_bytes,
        assets_bytes,
        binary_reference_bytes,
        readme_bytes,
        origins,
        assets,
        static_sources,
    )


def git_object_sha1(kind: bytes, data: bytes) -> bytes:
    header = kind + b" " + str(len(data)).encode("ascii") + b"\0"
    return hashlib.sha1(header + data).digest()


def parse_apkbuild_sha512sums(
    apkbuild: bytes,
    regular_files: dict[str, bytes],
    origin: Origin,
) -> set[tuple[str, str, str]]:
    marker = b'sha512sums="\n'
    lines = apkbuild.splitlines(keepends=True)
    declarations = [
        index for index, line in enumerate(lines) if b"sha512sums" in line
    ]
    if not declarations:
        return set()
    if len(declarations) != 1 or lines[declarations[0]] != marker:
        raise SourceBundleError(
            f"{origin.name} 的 APKBUILD sha512sums 声明不是唯一规范字面块。"
        )
    start = declarations[0] + 1
    try:
        end = lines.index(b'"\n', start)
    except ValueError as error:
        raise SourceBundleError(
            f"{origin.name} 的 APKBUILD sha512sums 字面块未规范结束。"
        ) from error

    requirements: set[tuple[str, str, str]] = set()
    filenames: set[str] = set()
    for line in lines[start:end]:
        if line == b"\n":
            continue
        match = re.fullmatch(rb"([0-9a-f]{128})  ([^\r\n]+)\n", line)
        if match is None:
            raise SourceBundleError(
                f"{origin.name} 的 APKBUILD sha512sums 条目格式非法。"
            )
        try:
            filename = match.group(2).decode("utf-8")
        except UnicodeDecodeError as error:
            raise SourceBundleError(
                f"{origin.name} 的 APKBUILD sha512sums 文件名不是 UTF-8。"
            ) from error
        filename_path = PurePosixPath(filename)
        if (
            not filename
            or str(filename_path) != filename
            or filename_path.name != filename
            or filename in filenames
        ):
            raise SourceBundleError(
                f"{origin.name} 的 APKBUILD sha512sums 文件名非法或重复。"
            )
        filenames.add(filename)
        digest = match.group(1).decode("ascii")
        if filename in regular_files:
            if hashlib.sha512(regular_files[filename]).hexdigest() != digest:
                raise SourceBundleError(
                    f"{origin.name} 的本地源码文件 SHA-512 不匹配：{filename}"
                )
        else:
            requirements.add((origin.name, filename, digest))
    return requirements


def validate_aports_archive(
    source,
    asset: Asset,
    origin: Origin,
    static_source: StaticLinkSource | None = None,
) -> set[tuple[str, str, str]]:
    expected_root = (
        f"aports-{origin.commit}-{origin.commit}-main-{origin.name}"
    )
    directories: set[str] = set()
    tree_entries: list[tuple[bytes, bytes, bytes]] = []
    regular_files: dict[str, bytes] = {}
    names: set[str] = set()
    apkbuild_found = False
    try:
        with tarfile.open(fileobj=source if isinstance(source, io.BytesIO) else None,
                          name=None if isinstance(source, io.BytesIO) else source,
                          mode="r:gz") as archive:
            for member in archive.getmembers():
                path = PurePosixPath(member.name)
                if (
                    path.is_absolute()
                    or str(path) != member.name
                    or any(part in ("", ".", "..") for part in path.parts)
                    or member.name in names
                ):
                    raise SourceBundleError(
                        f"{asset.bundle_path} 含重复或逃逸路径。"
                    )
                names.add(member.name)
                parts = path.parts
                if (
                    not parts
                    or parts[0] != expected_root
                    or (len(parts) >= 2 and parts[1] != "main")
                    or (len(parts) >= 3 and parts[2] != origin.name)
                ):
                    raise SourceBundleError(
                        f"{asset.bundle_path} 含 origin 目录外成员。"
                    )
                if len(parts) <= 3:
                    if not member.isdir():
                        raise SourceBundleError(
                            f"{asset.bundle_path} 的包装目录类型非法。"
                        )
                    directories.add(str(path))
                    continue
                if len(parts) != 4:
                    raise SourceBundleError(
                        f"{asset.bundle_path} 含未锁定的嵌套目录。"
                    )
                filename = parts[3]
                filename_bytes = filename.encode("utf-8")
                if member.isfile():
                    extracted = archive.extractfile(member)
                    if extracted is None:
                        raise SourceBundleError(
                            f"{asset.bundle_path} 无法读取 {filename}。"
                        )
                    data = extracted.read()
                    regular_files[filename] = data
                    mode = b"100755" if member.mode & 0o111 else b"100644"
                    blob_sha1 = git_object_sha1(b"blob", data)
                    apkbuild_found |= filename == "APKBUILD"
                elif member.issym():
                    target = PurePosixPath(member.linkname)
                    if (
                        not member.linkname
                        or target.is_absolute()
                        or str(target) != member.linkname
                        or any(part in ("", ".", "..") for part in target.parts)
                    ):
                        raise SourceBundleError(
                            f"{asset.bundle_path} 含逃逸符号链接。"
                        )
                    mode = b"120000"
                    blob_sha1 = git_object_sha1(
                        b"blob", member.linkname.encode("utf-8")
                    )
                else:
                    raise SourceBundleError(
                        f"{asset.bundle_path} 含不允许的节点类型。"
                    )
                tree_entries.append((filename_bytes, mode, blob_sha1))
    except (tarfile.TarError, OSError, UnicodeError) as error:
        raise SourceBundleError(
            f"{asset.bundle_path} 不是有效的 aports 目录归档。"
        ) from error
    expected_directories = {
        expected_root,
        f"{expected_root}/main",
        f"{expected_root}/main/{origin.name}",
    }
    if directories != expected_directories:
        raise SourceBundleError(f"{asset.bundle_path} 的包装目录集合不完整。")
    if not apkbuild_found or len(tree_entries) != origin.entry_count:
        raise SourceBundleError(
            f"{asset.bundle_path} 缺少 APKBUILD 或 Git 条目数不一致。"
        )
    tree_body = b"".join(
        mode + b" " + name + b"\0" + blob_sha1
        for name, mode, blob_sha1 in sorted(tree_entries)
    )
    tree_sha1 = git_object_sha1(b"tree", tree_body).hex()
    if tree_sha1 != origin.tree_sha1:
        raise SourceBundleError(
            f"{asset.bundle_path} 的规范 Git tree SHA-1 不匹配。"
        )
    if static_source is not None:
        version, separator, release = static_source.source_version.rpartition(
            "-r"
        )
        apkbuild = regular_files["APKBUILD"]
        expected_assignments = (
            (b"pkgname=", f"pkgname={origin.name}\n".encode()),
            (b"pkgver=", f"pkgver={version}\n".encode()),
            (b"pkgrel=", f"pkgrel={release}\n".encode()),
        )
        apkbuild_lines = apkbuild.splitlines(keepends=True)
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
            raise SourceBundleError(
                f"{origin.name} 的静态链接来源版本与 APKBUILD 不一致。"
            )
    return parse_apkbuild_sha512sums(
        regular_files["APKBUILD"], regular_files, origin
    )


def validate_upstream_closure(
    requirements: set[tuple[str, str, str]], locks: Locks
) -> None:
    manifest: dict[tuple[str, str], str] = {}
    for asset in locks.assets:
        if asset.kind != "upstream":
            continue
        filename = PurePosixPath(asset.bundle_path).name
        key = (asset.origin, filename)
        if key in manifest:
            raise SourceBundleError(
                "同一 origin 的上游源码资产文件名不能重复。"
            )
        manifest[key] = asset.sha512
    declared = {
        (origin, filename, digest)
        for (origin, filename), digest in manifest.items()
    }
    if declared != requirements:
        missing = len(requirements - declared)
        extra = len(declared - requirements)
        raise SourceBundleError(
            "源码资产清单未闭合 APKBUILD sha512sums"
            f"（缺少 {missing} 项，多余 {extra} 项）。"
        )


def validate_asset_bytes(
    data: bytes, asset: Asset, locks: Locks
) -> set[tuple[str, str, str]]:
    if (
        len(data) != asset.size
        or hashlib.sha512(data).hexdigest() != asset.sha512
    ):
        raise SourceBundleError(
            f"源码资产大小或 SHA-512 不匹配：{asset.bundle_path}"
        )
    if asset.kind == "aports":
        static_source = next(
            (
                source
                for source in locks.static_link_sources
                if source.source_origin == asset.origin
            ),
            None,
        )
        return validate_aports_archive(
            io.BytesIO(data),
            asset,
            locks.origins[asset.origin],
            static_source,
        )
    return set()


def open_cache_parent(cache: Path, relative: str, create: bool) -> tuple[int, str]:
    path = validate_bundle_path(relative)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        raise SourceBundleError(
            "当前平台不支持安全的目录相对打开，无法访问源码缓存。"
        )
    directory_flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NONBLOCK", 0)
        | nofollow
        | directory
    )
    if create:
        try:
            cache.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise SourceBundleError(f"无法创建源码缓存：{cache}") from error
    try:
        current_descriptor = os.open(cache, directory_flags)
    except OSError as error:
        if error.errno == errno.ENOENT:
            raise SourceBundleError(f"源码缓存不存在：{cache}") from error
        if error.errno in (errno.ELOOP, errno.ENOTDIR):
            raise SourceBundleError(f"源码缓存必须是实体目录：{cache}") from error
        raise SourceBundleError(f"无法打开源码缓存：{error}") from error
    try:
        for part in path.parts[:-1]:
            try:
                next_descriptor = os.open(
                    part, directory_flags, dir_fd=current_descriptor
                )
            except OSError as error:
                if error.errno == errno.ENOENT and create:
                    try:
                        os.mkdir(part, 0o755, dir_fd=current_descriptor)
                    except FileExistsError:
                        pass
                    try:
                        next_descriptor = os.open(
                            part, directory_flags, dir_fd=current_descriptor
                        )
                    except OSError as open_error:
                        error = open_error
                    else:
                        os.close(current_descriptor)
                        current_descriptor = next_descriptor
                        continue
                if error.errno in (errno.ELOOP, errno.ENOENT, errno.ENOTDIR):
                    raise SourceBundleError(
                        "源码缓存父路径不存在、含符号链接或非实体目录："
                        f"{relative}"
                    ) from error
                raise SourceBundleError(
                    f"无法打开源码缓存父目录：{error}"
                ) from error
            os.close(current_descriptor)
            current_descriptor = next_descriptor
        return current_descriptor, path.name
    except BaseException:
        os.close(current_descriptor)
        raise


def read_asset_snapshot(
    cache: Path, asset: Asset, locks: Locks
) -> tuple[bytes, set[tuple[str, str, str]]]:
    parent_descriptor, leaf_name = open_cache_parent(
        cache, asset.bundle_path, False
    )
    flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NONBLOCK", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    try:
        try:
            descriptor = os.open(
                leaf_name, flags, dir_fd=parent_descriptor
            )
        except OSError as error:
            if error.errno in (errno.ELOOP, errno.ENOENT, errno.ENOTDIR):
                raise SourceBundleError(
                    f"源码资产不存在或是符号链接：{asset.bundle_path}"
                ) from error
            raise SourceBundleError(
                f"无法打开源码资产 {asset.bundle_path}：{error}"
            ) from error
    finally:
        os.close(parent_descriptor)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise SourceBundleError(
                f"源码资产必须是普通文件：{asset.bundle_path}"
            )
        if metadata.st_size != asset.size:
            raise SourceBundleError(
                f"源码资产大小或 SHA-512 不匹配：{asset.bundle_path}"
            )
        data = read_descriptor(descriptor, asset.size)
    finally:
        os.close(descriptor)
    closure = validate_asset_bytes(data, asset, locks)
    return data, closure


def download_asset(
    cache: Path, asset: Asset, locks: Locks
) -> set[tuple[str, str, str]]:
    parent_descriptor, leaf_name = open_cache_parent(
        cache, asset.bundle_path, True
    )
    descriptor: int | None = None
    temporary_name: str | None = None
    try:
        flags = (
            os.O_RDWR
            | os.O_CREAT
            | os.O_EXCL
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        for _attempt in range(100):
            candidate = f".source-download.{secrets.token_hex(8)}"
            try:
                descriptor = os.open(
                    candidate,
                    flags,
                    0o600,
                    dir_fd=parent_descriptor,
                )
            except FileExistsError:
                continue
            temporary_name = candidate
            break
        if descriptor is None or temporary_name is None:
            raise SourceBundleError("无法创建唯一的源码下载临时文件。")
        with os.fdopen(os.dup(descriptor), "wb") as output:
            request = urllib.request.Request(
                asset.source_url,
                headers={"User-Agent": "ish-multiarch-source-bundle/1"},
            )
            with urllib.request.urlopen(request, timeout=120) as response:
                total = 0
                digest = hashlib.sha512()
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > asset.size:
                        raise SourceBundleError(
                            f"下载的源码资产超过锁定大小：{asset.bundle_path}"
                        )
                    digest.update(chunk)
                    output.write(chunk)
        if total != asset.size or digest.hexdigest() != asset.sha512:
            raise SourceBundleError(
                f"下载的源码资产大小或 SHA-512 不匹配：{asset.bundle_path}"
            )
        if os.fstat(descriptor).st_size != asset.size:
            raise SourceBundleError(
                f"下载的源码资产大小或 SHA-512 不匹配：{asset.bundle_path}"
            )
        data = read_descriptor(descriptor, asset.size)
        closure = validate_asset_bytes(data, asset, locks)
        os.fchmod(descriptor, 0o644)
        os.replace(
            temporary_name,
            leaf_name,
            src_dir_fd=parent_descriptor,
            dst_dir_fd=parent_descriptor,
        )
        temporary_name = None
        return closure
    finally:
        cleanup_error: OSError | None = None
        active_error = sys.exc_info()[0] is not None
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError as error:
                cleanup_error = error
        if temporary_name is not None:
            try:
                os.unlink(temporary_name, dir_fd=parent_descriptor)
            except FileNotFoundError:
                pass
            except OSError as error:
                cleanup_error = cleanup_error or error
        try:
            os.close(parent_descriptor)
        except OSError as error:
            cleanup_error = cleanup_error or error
        if cleanup_error is not None and not active_error:
            raise SourceBundleError(
                f"清理源码下载临时资源失败：{cleanup_error}"
            ) from cleanup_error


def fetch_command(args: argparse.Namespace) -> None:
    locks = load_locks(args)
    cache = Path(args.cache).absolute()
    requirements: set[tuple[str, str, str]] = set()
    for asset in locks.assets:
        try:
            _data, closure = read_asset_snapshot(cache, asset, locks)
            requirements.update(closure)
            print(f"复用源码资产：{asset.bundle_path}")
            del _data
            continue
        except SourceBundleError:
            if args.offline:
                raise
        closure = download_asset(cache, asset, locks)
        requirements.update(closure)
        print(f"已取得源码资产：{asset.bundle_path}")
    validate_upstream_closure(requirements, locks)


def tar_info(name: str, size: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size = size
    info.mode = 0o644
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    info.type = tarfile.REGTYPE
    return info


def metadata_members(locks: Locks) -> dict[str, bytes]:
    return {
        f"{BUNDLE_ROOT}/README.md": locks.readme_bytes,
        f"{BUNDLE_ROOT}/manifest/binary-reference.tsv": (
            locks.binary_reference_bytes
        ),
        f"{BUNDLE_ROOT}/manifest/origins.tsv": locks.origins_bytes,
        f"{BUNDLE_ROOT}/manifest/packages.tsv": locks.packages_bytes,
        f"{BUNDLE_ROOT}/manifest/source-assets.tsv": locks.assets_bytes,
        f"{BUNDLE_ROOT}/manifest/static-link-sources.tsv": (
            locks.static_link_sources_bytes
        ),
    }


def build_bundle(output: Path, cache: Path, locks: Locks) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", dir=output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w+b") as raw:
            requirements: set[tuple[str, str, str]] = set()
            with tarfile.open(
                fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT
            ) as archive:
                members = [
                    *metadata_members(locks).items(),
                    *(
                        (
                            f"{BUNDLE_ROOT}/{asset.bundle_path}",
                            asset,
                        )
                        for asset in locks.assets
                    ),
                ]
                for archive_name, source in sorted(members):
                    if isinstance(source, bytes):
                        archive.addfile(
                            tar_info(archive_name, len(source)),
                            io.BytesIO(source),
                        )
                    else:
                        data, closure = read_asset_snapshot(
                            cache, source, locks
                        )
                        requirements.update(closure)
                        archive.addfile(
                            tar_info(archive_name, len(data)),
                            io.BytesIO(data),
                        )
                        del data
            validate_upstream_closure(requirements, locks)
            raw.flush()
            raw.seek(0)
            verify_bundle_contents(raw, locks)
            os.fchmod(raw.fileno(), 0o644)
        os.replace(temporary, output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def hash_tar_member(
    archive: tarfile.TarFile, member: tarfile.TarInfo
) -> tuple[str, bytes | None]:
    extracted = archive.extractfile(member)
    if extracted is None:
        raise SourceBundleError(f"无法读取源码包成员：{member.name}")
    digest = hashlib.sha512()
    captured = io.BytesIO() if member.name.startswith(
        f"{BUNDLE_ROOT}/aports/"
    ) else None
    for chunk in iter(lambda: extracted.read(1024 * 1024), b""):
        digest.update(chunk)
        if captured is not None:
            captured.write(chunk)
    return digest.hexdigest(), captured.getvalue() if captured else None


def verify_bundle_contents(source, locks: Locks) -> None:
    expected_metadata = metadata_members(locks)
    assets_by_name = {
        f"{BUNDLE_ROOT}/{asset.bundle_path}": asset for asset in locks.assets
    }
    expected_names = set(expected_metadata) | set(assets_by_name)
    requirements: set[tuple[str, str, str]] = set()
    try:
        source.seek(0)
        with tarfile.open(fileobj=source, mode="r:") as archive:
            members = archive.getmembers()
            names = [member.name for member in members]
            if len(names) != len(set(names)) or set(names) != expected_names:
                raise SourceBundleError("源码包成员集合缺失、重复或含额外文件。")
            for member in members:
                path = PurePosixPath(member.name)
                if (
                    path.is_absolute()
                    or ".." in path.parts
                    or member.type != tarfile.REGTYPE
                    or member.mode != 0o644
                    or member.uid != 0
                    or member.gid != 0
                    or member.uname
                    or member.gname
                    or member.mtime != 0
                ):
                    raise SourceBundleError(
                        f"源码包成员元数据不规范：{member.name}"
                    )
                if member.name in expected_metadata:
                    extracted = archive.extractfile(member)
                    if (
                        extracted is None
                        or extracted.read() != expected_metadata[member.name]
                    ):
                        raise SourceBundleError(
                            f"源码包内嵌清单不一致：{member.name}"
                        )
                    continue
                asset = assets_by_name[member.name]
                if member.size != asset.size:
                    raise SourceBundleError(
                        f"源码包成员大小不一致：{member.name}"
                    )
                digest, aports_data = hash_tar_member(archive, member)
                if digest != asset.sha512:
                    raise SourceBundleError(
                        f"源码包成员 SHA-512 不一致：{member.name}"
                    )
                if aports_data is not None:
                    static_source = next(
                        (
                            source
                            for source in locks.static_link_sources
                            if source.source_origin == asset.origin
                        ),
                        None,
                    )
                    closure = validate_aports_archive(
                        io.BytesIO(aports_data),
                        asset,
                        locks.origins[asset.origin],
                        static_source,
                    )
                    requirements.update(closure)
            validate_upstream_closure(requirements, locks)
    except (tarfile.TarError, OSError) as error:
        raise SourceBundleError("无法读取对应源码 tar。") from error


def sha256_file(path: Path) -> str:
    descriptor = open_safe_file(path, "对应源码包")
    digest = hashlib.sha256()
    with os.fdopen(descriptor, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bundle_command(args: argparse.Namespace) -> None:
    locks = load_locks(args)
    output = Path(args.output).absolute()
    build_bundle(output, Path(args.cache).absolute(), locks)
    print(f"已生成对应源码包：{output}")
    print(f"SHA-256：{sha256_file(output)}")


def parse_checksum(path: Path, bundle_name: str) -> str:
    data = read_lock(path, "对应源码包 SHA-256 清单")
    match = re.fullmatch(
        rb"([0-9a-f]{64})  ([^\t\r\n/]+)\n", data
    )
    if match is None or match.group(2).decode("utf-8") != bundle_name:
        raise SourceBundleError("对应源码包 SHA-256 清单格式或文件名错误。")
    return match.group(1).decode("ascii")


def verify_command(args: argparse.Namespace) -> None:
    locks = load_locks(args)
    bundle = Path(args.bundle).absolute()
    expected = parse_checksum(Path(args.checksum), bundle.name)
    descriptor = open_safe_file(bundle, "对应源码包")
    with os.fdopen(descriptor, "rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
        if digest.hexdigest() != expected:
            raise SourceBundleError("对应源码包 SHA-256 不匹配。")
        verify_bundle_contents(source, locks)
    print("Alpine AArch64 对应源码包验证通过")


def check_locks_command(args: argparse.Namespace) -> None:
    load_locks(args)
    parse_checksum(Path(args.checksum), BUNDLE_FILENAME)
    print("Alpine AArch64 对应源码锁验证通过")


def add_lock_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--packages", type=Path, default=DEFAULT_PACKAGES)
    parser.add_argument(
        "--static-link-sources",
        type=Path,
        default=DEFAULT_STATIC_LINK_SOURCES,
    )
    parser.add_argument("--origins", type=Path, default=DEFAULT_ORIGINS)
    parser.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument(
        "--binary-reference",
        type=Path,
        default=DEFAULT_BINARY_REFERENCE,
    )
    parser.add_argument("--readme", type=Path, default=DEFAULT_README)
    parser.add_argument(
        "--allow-local-sources",
        action="store_true",
        help=(
            "只用于本地测试夹具，允许 file:// 源码 URL，"
            "并跳过固定生产静态来源集合"
        ),
    )


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="取得、生成并验证固定 Alpine AArch64 对应源码包"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    fetch = subparsers.add_parser("fetch", help="显式取得锁定源码资产")
    fetch.add_argument("cache")
    fetch.add_argument("--offline", action="store_true")
    add_lock_arguments(fetch)
    fetch.set_defaults(function=fetch_command)

    bundle = subparsers.add_parser("bundle", help="生成确定性源码 tar")
    bundle.add_argument("cache")
    bundle.add_argument("output")
    add_lock_arguments(bundle)
    bundle.set_defaults(function=bundle_command)

    verify = subparsers.add_parser("verify", help="离线验证源码 tar")
    verify.add_argument("bundle")
    verify.add_argument("--checksum", type=Path, default=DEFAULT_CHECKSUM)
    add_lock_arguments(verify)
    verify.set_defaults(function=verify_command)

    check_locks = subparsers.add_parser(
        "check-locks", help="纯离线验证源码锁与对应源码包摘要清单"
    )
    check_locks.add_argument(
        "--checksum", type=Path, default=DEFAULT_CHECKSUM
    )
    add_lock_arguments(check_locks)
    check_locks.set_defaults(function=check_locks_command)
    return parser


def main() -> int:
    parser = create_parser()
    args = parser.parse_args()
    try:
        args.function(args)
    except (
        SourceBundleError,
        OSError,
        urllib.error.URLError,
    ) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

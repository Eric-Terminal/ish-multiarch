#!/usr/bin/env python3

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import zipfile
import zlib


sys.dont_write_bytecode = True

SCHEMA = "ish-apple-release-manifest-v1"
MANIFEST_FILENAME = "APPLE-RELEASE-MANIFEST.json"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
SOURCE_DATE_EPOCH = re.compile(r"^[0-9]+$")
SAFE_FIELD = re.compile(r"^[A-Za-z0-9][A-Za-z0-9+._-]*$")
GIT_ENV = {
    key: value for key, value in os.environ.items()
    if not key.startswith("GIT_")
}
GIT_ENV["GIT_CONFIG_GLOBAL"] = os.devnull
GIT_ENV["GIT_CONFIG_NOSYSTEM"] = "1"
GIT_ENV["GIT_OPTIONAL_LOCKS"] = "0"
GIT_ENV["GIT_NO_REPLACE_OBJECTS"] = "1"
GIT_ENV["LC_ALL"] = "C"

PROFILE_PATH = "distribution/apple/release-profiles.tsv"
PROFILE_HEADER = (
    "profile\trole\ttarget\tscheme\tdebug_configuration"
    "\trelease_configuration\tkernel\trootfs"
)
PROFILE_ROWS = (
    (
        "core",
        "iphone-app",
        "iSH",
        "iSH",
        "Debug-ApplePleaseFixFB19282108",
        "Release",
        "ish",
        "fixed-alpine-seed",
    ),
    (
        "core",
        "watch-app",
        "iSHWatch",
        "iSHWatch",
        "Debug-ApplePleaseFixFB19282108",
        "Release",
        "ish",
        "fixed-alpine-seed",
    ),
    (
        "with-linux",
        "iphone-app",
        "iSH+Linux",
        "iSH+Linux",
        "DebugLinux",
        "ReleaseLinux",
        "linux",
        "online-root-tar",
    ),
    (
        "with-linux",
        "watch-app",
        "iSHWatch",
        "iSHWatch",
        "Debug-ApplePleaseFixFB19282108",
        "Release",
        "ish",
        "fixed-alpine-seed",
    ),
)

PROJECT_INPUTS_PATH = "distribution/apple/project-license/inputs.tsv"
PROJECT_INPUTS_HEADER = "section\trole\tpath\tsource_url\tsize\tsha256"
DEPENDENCIES_PATH = "third_party/apple-host/dependencies.tsv"
DEPENDENCIES_HEADER = (
    "component\tversion\tversion_source\tgitlink_path\tgitlink_commit"
    "\tsource_url\tdelivery_unit\tdelivery_kind\tdelivery_name"
    "\tinput_count\tinput_sha256"
)
NOTICE_PATHS = (
    "distribution/apple/project-license/PROJECT-LICENSES.txt",
    "third_party/alpine/3.24.1-aarch64/THIRD-PARTY-NOTICES.txt",
    "third_party/apple-host/APPLE-HOST-NOTICES.txt",
)
TOOLCHAIN_KEYS = (
    "clang_version",
    "host_arch",
    "iphoneos_sdk_build",
    "ld_version",
    "meson_version",
    "ninja_version",
    "source_date_epoch",
    "watchos_sdk_build",
    "xcode_build",
)
COMMON_ASSETS = (
    ("checksum", "alpine-rootfs", "corresponding-source.sha256"),
    ("metadata", "toolchain", "apple-toolchain.tsv"),
    ("product", "iphone-app", "iphone-app.ipa"),
    ("product", "watch-app", "watch-app.ipa"),
    (
        "source",
        "alpine-rootfs",
        "alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar",
    ),
    ("source", "deps-libapps", "deps-libapps-source.tar"),
    ("source", "deps-libarchive", "deps-libarchive-source.tar"),
    ("source", "deps-linux", "deps-linux-source.tar"),
    ("source", "project", "project-source.tar"),
    ("symbols", "iphone-app", "iphone-app.dSYM.zip"),
    ("symbols", "watch-app", "watch-app.dSYM.zip"),
)
WITH_LINUX_ASSETS = (
    (
        "checksum",
        "online-rootfs",
        "online-rootfs-corresponding-source.sha256",
    ),
    (
        "source",
        "online-rootfs",
        "online-rootfs-corresponding-source.tar",
    ),
)


class ManifestError(ValueError):
    pass


class ChineseArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"错误：命令行参数无效：{message}\n")


def fail(message: str) -> None:
    raise ManifestError(message)


def run_git(root: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=GIT_ENV,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        fail(f"Git 检查失败：{detail or '未知错误'}")
    return result.stdout


def decode_utf8(data: bytes, description: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"{description}不是 UTF-8：{error}")


def resolve_directory(path: Path, description: str) -> Path:
    try:
        if stat.S_ISLNK(path.lstat().st_mode):
            fail(f"{description}不能是符号链接")
        resolved = path.resolve(strict=True)
    except FileNotFoundError:
        fail(f"{description}不存在：{path}")
    except (OSError, RuntimeError) as error:
        fail(f"无法检查{description}：{path}（{error}）")
    if not resolved.is_dir():
        fail(f"{description}必须是目录：{path}")
    return resolved


def resolve_regular(path: Path, description: str, mode=None) -> Path:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        fail(f"{description}不存在：{path}")
    except OSError as error:
        fail(f"无法检查{description}：{path}（{error}）")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        fail(f"{description}必须是普通文件且不能是符号链接：{path}")
    if mode is not None and stat.S_IMODE(metadata.st_mode) != mode:
        fail(f"{description}权限必须为 {mode:04o}：{path}")
    try:
        return path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        fail(f"无法解析{description}：{path}（{error}）")


def validate_relative(value: str, description: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or str(path) != value
        or "\\" in value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        fail(f"{description}不是安全的相对路径：{value}")
    return value


def validate_flat_filename(value: str, description: str) -> str:
    validate_relative(value, description)
    if "/" in value or SAFE_FIELD.fullmatch(value) is None:
        fail(f"{description}必须是扁平安全文件名：{value}")
    return value


def parse_lf_tsv(data: bytes, header: str | None, columns: int, description: str):
    if not data or not data.endswith(b"\n") or b"\r" in data:
        fail(f"{description}必须非空、只使用 LF 并以 LF 结尾")
    text = decode_utf8(data, description)
    lines = text[:-1].split("\n")
    if any(not line for line in lines):
        fail(f"{description}不能含空行")
    if header is not None:
        if not lines or lines[0] != header:
            fail(f"{description}表头漂移")
        lines = lines[1:]
    rows = []
    for line in lines:
        fields = tuple(line.split("\t"))
        if len(fields) != columns or any(not field for field in fields):
            fail(f"{description}含空字段或列数错误")
        rows.append(fields)
    if not rows:
        fail(f"{description}没有记录")
    return tuple(rows)


def index_entries(root: Path):
    records = run_git(root, "ls-files", "--stage", "-z")
    entries = {}
    for raw in records.split(b"\0"):
        if not raw:
            continue
        text = decode_utf8(raw, "Git 索引")
        if "\t" not in text:
            fail("Git 索引记录格式非法")
        metadata, relative = text.split("\t", 1)
        fields = metadata.split()
        if len(fields) != 3 or fields[2] != "0" or relative in entries:
            fail(f"Git 索引含重复路径或非零 stage：{relative}")
        entries[relative] = (fields[0], fields[1])
    return entries


def head_entry(root: Path, relative: str):
    records = run_git(root, "ls-tree", "-z", "HEAD", "--", relative)
    rows = [row for row in records.split(b"\0") if row]
    suffix = b"\t" + relative.encode("utf-8")
    if len(rows) != 1 or not rows[0].endswith(suffix):
        fail(f"HEAD 缺少唯一受跟踪输入：{relative}")
    fields = decode_utf8(
        rows[0][:-len(suffix)], "HEAD tree"
    ).split()
    if len(fields) != 3:
        fail(f"HEAD tree 记录非法：{relative}")
    return fields[0], fields[2]


def physical_repo_file(root: Path, relative: str, description: str) -> Path:
    validate_relative(relative, description)
    current = root
    parts = PurePosixPath(relative).parts
    for index, part in enumerate(parts):
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            fail(f"{description}不存在或不可读：{relative}（{error}）")
        if stat.S_ISLNK(metadata.st_mode):
            fail(f"{description}不能经过符号链接：{relative}")
        if index + 1 < len(parts) and not stat.S_ISDIR(metadata.st_mode):
            fail(f"{description}父路径不是目录：{relative}")
    if not stat.S_ISREG(current.lstat().st_mode):
        fail(f"{description}必须是普通文件：{relative}")
    return current


def read_tracked_100644(root: Path, entries, relative: str, description: str):
    if entries.get(relative, (None,))[0] != "100644":
        fail(f"{description}在索引中必须是 100644：{relative}")
    head_mode, head_oid = head_entry(root, relative)
    if head_mode != "100644" or entries[relative] != (head_mode, head_oid):
        fail(f"{description}的 index 与 HEAD 不一致：{relative}")
    path = physical_repo_file(root, relative, description)
    try:
        data = path.read_bytes()
    except OSError as error:
        fail(f"无法读取{description}：{relative}（{error}）")
    expected = run_git(root, "cat-file", "blob", f"HEAD:{relative}")
    if data != expected:
        fail(f"{description}的工作树字节与 HEAD 不一致：{relative}")
    return data


def ensure_plain_index(root: Path, allow_sparse=False) -> None:
    lines = decode_utf8(
        run_git(root, "ls-files", "-v"), "Git 索引标志"
    ).splitlines()
    allowed = ("H ", "S ") if allow_sparse else ("H ",)
    if any(not line.startswith(allowed) for line in lines):
        fail("Git 索引含隐藏修改或异常索引标志")


def validate_repository(root_argument: Path, revision: str, tag: str):
    root = resolve_directory(root_argument, "仓库根目录")
    top = decode_utf8(
        run_git(root, "rev-parse", "--show-toplevel"), "Git 顶层路径"
    ).strip()
    try:
        top_path = Path(top).resolve(strict=True)
    except (OSError, RuntimeError):
        fail("Git 顶层路径无法解析")
    if top_path != root:
        fail("指定的 --root 必须是真实 Git 顶层目录")
    if HEX40.fullmatch(revision) is None:
        fail("revision 必须是完整的小写 40 位 Git 对象 ID")
    if not tag or tag.startswith("-") or any(
        character in tag for character in "\0\r\n"
    ):
        fail("tag 名称非法")
    check_ref = subprocess.run(
        ["git", "-C", str(root), "check-ref-format", f"refs/tags/{tag}"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=GIT_ENV,
    )
    if check_ref.returncode != 0:
        fail("tag 不是合法的 Git tag 名称")
    head = decode_utf8(
        run_git(root, "rev-parse", "--verify", "HEAD"), "HEAD"
    ).strip()
    if head != revision:
        fail("HEAD 与显式 revision 不一致")
    if run_git(root, "status", "--porcelain=v1", "--untracked-files=all",
               "--ignore-submodules=all"):
        fail("仓库工作树必须干净")
    reference = f"refs/tags/{tag}"
    tag_type = decode_utf8(
        run_git(root, "cat-file", "-t", reference), "tag 类型"
    ).strip()
    if tag_type != "tag":
        fail("显式 tag 必须是 annotated tag")
    peeled = decode_utf8(
        run_git(root, "rev-parse", "--verify", f"{reference}^{{commit}}"),
        "tag revision",
    ).strip()
    if peeled != revision:
        fail("tag 没有指向显式 revision")
    tag_object = decode_utf8(
        run_git(root, "rev-parse", "--verify", reference), "tag 对象"
    ).strip()
    if HEX40.fullmatch(tag_object) is None:
        fail("tag 对象 ID 非法")
    return root, tag_object


def validate_https_url(value: str, description: str) -> str:
    if "\\" in value or any(character.isspace() for character in value):
        fail(f"{description}含反斜杠或空白字符")
    try:
        parsed = urllib.parse.urlsplit(value)
        hostname = parsed.hostname
        parsed.port
    except ValueError as error:
        fail(f"{description}格式非法：{error}")
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or not hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        fail(f"{description}必须是无凭据、query 与 fragment 的 HTTPS URL")
    return value


def comparable_url(value: str) -> str:
    return value.rstrip("/").removesuffix(".git")


def load_project_source(root: Path, entries) -> str:
    rows = parse_lf_tsv(
        read_tracked_100644(
            root, entries, PROJECT_INPUTS_PATH, "项目源码输入锁"
        ),
        PROJECT_INPUTS_HEADER,
        6,
        "项目源码输入锁",
    )
    source_rows = [
        row for row in rows if row[0] == "source" and row[1] == "source"
    ]
    if len(source_rows) != 1:
        fail("项目源码输入锁必须有唯一 source/source 记录")
    row = source_rows[0]
    if row[2] != "-" or row[4] != "-" or row[5] != "-":
        fail("项目源码入口不能冒充文件输入")
    source_url = validate_https_url(row[3], "项目源码 URL")
    remote_lines = decode_utf8(
        run_git(
            root, "config", "--local", "--get-all", "remote.origin.url"
        ),
        "origin URL",
    ).splitlines()
    if (
        len(remote_lines) != 1
        or comparable_url(remote_lines[0]) != comparable_url(source_url)
    ):
        fail("origin URL 与项目源码输入锁不一致")
    return source_url


def load_profiles(root: Path, entries, profile: str):
    rows = parse_lf_tsv(
        read_tracked_100644(root, entries, PROFILE_PATH, "发行 profile 清单"),
        PROFILE_HEADER,
        8,
        "发行 profile 清单",
    )
    if rows != PROFILE_ROWS:
        fail("发行 profile 合同漂移")
    selected = [row for row in rows if row[0] == profile]
    if len(selected) != 2:
        fail(f"未知或不完整的发行 profile：{profile}")
    fields = (
        "profile",
        "role",
        "target",
        "scheme",
        "debug_configuration",
        "release_configuration",
        "kernel",
        "rootfs",
    )
    return [
        {key: value for key, value in zip(fields[1:], row[1:])}
        for row in selected
    ]


def load_gitmodules(root: Path, entries):
    data = read_tracked_100644(root, entries, ".gitmodules", "子模块配置")
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    try:
        parser.read_string(decode_utf8(data, "子模块配置"))
    except configparser.Error as error:
        fail(f"子模块配置无法解析：{error}")
    modules = {}
    for section in parser.sections():
        if not section.startswith('submodule "') or not section.endswith('"'):
            fail("子模块配置含未知 section")
        if set(parser[section]) - {"path", "url", "update", "shallow"}:
            fail(f"子模块配置含未知字段：{section}")
        path = parser[section].get("path")
        url = parser[section].get("url")
        if not path or not url or path in modules:
            fail(f"子模块配置缺少唯一路径或 URL：{section}")
        validate_relative(path, "子模块路径")
        modules[path] = validate_https_url(url, "子模块 URL")
    return modules


def verify_child_gitlink(root: Path, path: str, revision: str) -> None:
    directory = resolve_directory(root / path, f"gitlink {path}")
    top = decode_utf8(
        run_git(directory, "rev-parse", "--show-toplevel"), "子模块顶层"
    ).strip()
    try:
        child_top = Path(top).resolve(strict=True)
    except (OSError, RuntimeError):
        fail(f"gitlink 顶层路径无法解析：{path}")
    if child_top != directory:
        fail(f"gitlink 不是独立 Git 顶层：{path}")
    child_head = decode_utf8(
        run_git(directory, "rev-parse", "--verify", "HEAD"), "子模块 HEAD"
    ).strip()
    if child_head != revision:
        fail(f"gitlink 的子模块 HEAD 漂移：{path}")
    if run_git(
        directory,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
    ):
        fail(f"gitlink 子模块工作树不干净：{path}")
    # Linux 子模块使用合法 sparse checkout；只在该固定路径放行大写 S。
    ensure_plain_index(directory, allow_sparse=path == "deps/linux")


def load_gitlinks(root: Path, entries):
    rows = parse_lf_tsv(
        read_tracked_100644(
            root, entries, DEPENDENCIES_PATH, "宿主依赖来源锁"
        ),
        DEPENDENCIES_HEADER,
        11,
        "宿主依赖来源锁",
    )
    locked = {}
    components = set()
    for row in rows:
        component, path, revision, source_url = (
            row[0], row[3], row[4], row[5]
        )
        if component in components:
            fail(f"宿主依赖 component 重复：{component}")
        components.add(component)
        validate_relative(path, "宿主依赖 gitlink 路径")
        if HEX40.fullmatch(revision) is None:
            fail(f"宿主依赖 gitlink revision 非法：{path}")
        validate_https_url(source_url, "宿主依赖来源 URL")
        identity = (revision, source_url)
        if path in locked and locked[path] != identity:
            fail(f"宿主依赖对同一 gitlink 声明了不同来源：{path}")
        locked[path] = identity

    parent_gitlinks = {
        path: oid for path, (mode, oid) in entries.items() if mode == "160000"
    }
    if set(parent_gitlinks) != set(locked):
        fail("父仓库全部 gitlink 与宿主依赖来源锁不一致")
    modules = load_gitmodules(root, entries)
    if modules != {path: value[1] for path, value in locked.items()}:
        fail(".gitmodules 与宿主依赖来源锁不一致")

    result = []
    for path in sorted(parent_gitlinks):
        index_oid = parent_gitlinks[path]
        head_mode, head_oid = head_entry(root, path)
        lock_oid, source_url = locked[path]
        if (
            head_mode != "160000"
            or index_oid != head_oid
            or index_oid != lock_oid
        ):
            fail(f"gitlink 的 HEAD/index/来源锁 revision 不一致：{path}")
        verify_child_gitlink(root, path, index_oid)
        result.append(
            {
                "path": path,
                "revision": index_oid,
                "source_url": source_url,
            }
        )
    return result


def archive_parts(value: str, description: str, directory=False):
    candidate = value[:-1] if directory and value.endswith("/") else value
    if (
        not candidate
        or candidate.startswith("/")
        or "\\" in candidate
        or "\0" in candidate
    ):
        fail(f"{description}含不安全路径：{value}")
    parts = candidate.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        fail(f"{description}含不安全、不规范或逃逸路径：{value}")
    return parts


def validate_link_target(base, target: str, description: str) -> None:
    if (
        not target
        or target.startswith("/")
        or "\\" in target
        or "\0" in target
    ):
        fail(f"{description}不是安全相对链接：{target}")
    stack = list(base)
    for part in target.split("/"):
        if part in {"", "."}:
            continue
        if part == "..":
            if not stack:
                fail(f"{description}逃逸出归档根：{target}")
            stack.pop()
        else:
            stack.append(part)


def validate_zip(path: Path, description: str) -> None:
    try:
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            if not members:
                fail(f"{description}不能为空归档")
            seen = set()
            regular_count = 0
            for item in members:
                mode = item.external_attr >> 16
                kind = stat.S_IFMT(mode)
                is_directory = item.is_dir() or kind == stat.S_IFDIR
                parts = archive_parts(
                    item.filename, description, directory=is_directory
                )
                logical = "/".join(parts)
                if logical in seen:
                    fail(f"{description}含重复成员：{logical}")
                seen.add(logical)
                if kind == stat.S_IFLNK:
                    if item.file_size > 4096:
                        fail(f"{description}的符号链接目标过长：{logical}")
                    try:
                        target = archive.read(item).decode("utf-8")
                    except (RuntimeError, UnicodeDecodeError) as error:
                        fail(f"{description}的符号链接目标不可读：{error}")
                    validate_link_target(
                        parts[:-1], target, f"{description}成员 {logical}"
                    )
                elif is_directory:
                    if not item.filename.endswith("/"):
                        fail(f"{description}目录成员格式非法：{logical}")
                elif kind not in {0, stat.S_IFREG}:
                    fail(f"{description}含设备、FIFO 或其他特殊成员：{logical}")
                else:
                    regular_count += 1
            if regular_count == 0:
                fail(f"{description}至少要有一个普通文件")
            damaged = archive.testzip()
            if damaged is not None:
                fail(f"{description}成员 CRC 失败：{damaged}")
    except (
        zipfile.BadZipFile, RuntimeError, NotImplementedError, zlib.error
    ) as error:
        fail(f"{description}不是有效 ZIP：{error}")


def validate_tar(path: Path, description: str) -> None:
    try:
        with tarfile.open(path, mode="r:*") as archive:
            members = archive.getmembers()
            if not members:
                fail(f"{description}不能为空归档")
            seen = set()
            regular_count = 0
            for item in members:
                parts = archive_parts(
                    item.name, description, directory=item.isdir()
                )
                logical = "/".join(parts)
                if logical in seen:
                    fail(f"{description}含重复成员：{logical}")
                seen.add(logical)
                if item.isreg():
                    regular_count += 1
                elif item.isdir():
                    pass
                elif item.issym():
                    validate_link_target(
                        parts[:-1],
                        item.linkname,
                        f"{description}符号链接 {logical}",
                    )
                elif item.islnk():
                    validate_link_target(
                        (), item.linkname, f"{description}硬链接 {logical}"
                    )
                else:
                    fail(f"{description}含设备、FIFO 或其他特殊成员：{logical}")
            if regular_count == 0:
                fail(f"{description}至少要有一个普通文件")
    except (tarfile.TarError, EOFError) as error:
        fail(f"{description}不是有效 tar：{error}")


def expected_assets(profile: str):
    rows = list(COMMON_ASSETS)
    if profile == "with-linux":
        rows.extend(WITH_LINUX_ASSETS)
    return tuple(sorted(rows))


def load_asset_index(path: Path, profile: str):
    data = resolve_regular(path, "候选资产索引").read_bytes()
    rows = parse_lf_tsv(data, None, 3, "候选资产索引")
    for category, role, filename in rows:
        if SAFE_FIELD.fullmatch(category) is None:
            fail(f"资产 category 非法：{category}")
        if SAFE_FIELD.fullmatch(role) is None:
            fail(f"资产 role 非法：{role}")
        validate_flat_filename(filename, "资产文件名")
    if rows != tuple(sorted(rows)) or len(rows) != len(set(rows)):
        fail("候选资产索引必须按完整记录唯一排序")
    identities = [(row[0], row[1]) for row in rows]
    filenames = [row[2] for row in rows]
    if len(identities) != len(set(identities)):
        fail("候选资产索引含重复角色")
    if len(filenames) != len(set(filenames)):
        fail("候选资产索引含重复文件名")
    if rows != expected_assets(profile):
        fail(f"{profile} profile 的候选资产角色、文件名或集合不匹配")
    return rows


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def manifest_is_inside_assets(manifest: Path | None, assets: Path) -> bool:
    return (
        manifest is not None
        and manifest.parent == assets
        and manifest.name == MANIFEST_FILENAME
    )


def load_assets(assets_argument: Path, rows, manifest: Path | None = None):
    assets = resolve_directory(assets_argument, "候选资产目录")
    inside = (
        manifest_is_inside_assets(manifest, assets)
        and manifest is not None
        and manifest.exists()
    )
    expected_names = {row[2] for row in rows}
    allowed_names = expected_names | ({MANIFEST_FILENAME} if inside else set())
    try:
        actual_names = {entry.name for entry in os.scandir(assets)}
    except OSError as error:
        fail(f"无法枚举候选资产目录：{error}")
    if actual_names != allowed_names:
        missing = sorted(allowed_names - actual_names)
        extra = sorted(actual_names - allowed_names)
        fail(f"候选资产目录集合不精确（缺失：{missing}；多余：{extra}）")

    records = []
    paths = {}
    for category, role, filename in rows:
        path = resolve_regular(
            assets / filename, f"候选资产 {filename}", mode=0o644
        )
        size = path.stat().st_size
        if size == 0:
            fail(f"候选资产不能为空：{filename}")
        digest = sha256_file(path)
        if category in {"product", "symbols"}:
            validate_zip(path, f"候选资产 {filename}")
        elif category == "source":
            validate_tar(path, f"候选资产 {filename}")
        paths[(category, role)] = path
        records.append(
            {
                "category": category,
                "role": role,
                "filename": filename,
                "size": size,
                "sha256": digest,
            }
        )

    by_identity = {
        (record["category"], record["role"]): record for record in records
    }
    for role in ("alpine-rootfs", "online-rootfs"):
        checksum = by_identity.get(("checksum", role))
        if checksum is None:
            continue
        source = by_identity[("source", role)]
        checksum_bytes = paths[("checksum", role)].read_bytes()
        expected = f"{source['sha256']}  {source['filename']}\n".encode("ascii")
        if checksum_bytes != expected:
            fail(f"{role} SHA-256 摘要清单正文与对应源码资产不一致")

    toolchain_path = paths[("metadata", "toolchain")]
    toolchain_rows = parse_lf_tsv(
        toolchain_path.read_bytes(), None, 2, "Apple 工具链元数据"
    )
    if tuple(row[0] for row in toolchain_rows) != TOOLCHAIN_KEYS:
        fail("Apple toolchain 元数据键集合或排序漂移")
    toolchain = {}
    for key, value in toolchain_rows:
        if value.strip() != value or "\0" in value:
            fail(f"Apple 工具链元数据值非法：{key}")
        toolchain[key] = value
    if SOURCE_DATE_EPOCH.fullmatch(toolchain["source_date_epoch"]) is None:
        fail("source_date_epoch 必须只含 ASCII 十进制数字")
    return assets, records, toolchain


def load_notices(root: Path, entries):
    notices = []
    for relative in sorted(NOTICE_PATHS):
        data = read_tracked_100644(root, entries, relative, "发布声明")
        text = decode_utf8(data, f"发布声明 {relative}")
        notices.append(
            {
                "path": relative,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "text": text,
            }
        )
    return notices


def build_document(
    root_argument: Path,
    profile: str,
    revision: str,
    tag: str,
    assets_argument: Path,
    rows,
    manifest: Path | None = None,
):
    root, tag_object = validate_repository(root_argument, revision, tag)
    entries = index_entries(root)
    products = load_profiles(root, entries, profile)
    source_url = load_project_source(root, entries)
    gitlinks = load_gitlinks(root, entries)
    notices = load_notices(root, entries)
    ensure_plain_index(root)
    _assets, asset_records, toolchain = load_assets(
        assets_argument, rows, manifest
    )
    return {
        "assets": asset_records,
        "gitlinks": gitlinks,
        "notices": notices,
        "products": products,
        "profile": profile,
        "repository": {
            "revision": revision,
            "source_url": source_url,
            "tag": tag,
            "tag_object": tag_object,
        },
        "schema": SCHEMA,
        "toolchain": toolchain,
    }


def canonical_json(document) -> bytes:
    return (
        json.dumps(
            document,
            ensure_ascii=False,
            sort_keys=True,
            indent=2,
            allow_nan=False,
        )
        + "\n"
    ).encode("utf-8")


def resolve_output(path: Path):
    if path.name != MANIFEST_FILENAME:
        fail(f"输出文件名必须是 {MANIFEST_FILENAME}")
    parent = resolve_directory(path.parent, "manifest 输出父目录")
    output = parent / MANIFEST_FILENAME
    try:
        output.lstat()
    except FileNotFoundError:
        return output
    except OSError as error:
        fail(f"无法检查 manifest 输出：{error}")
    fail("manifest 输出已存在，拒绝覆盖")


def write_exclusive_atomic(output: Path, data: bytes) -> None:
    descriptor = -1
    temporary = None
    try:
        descriptor, name = tempfile.mkstemp(
            prefix=f".{MANIFEST_FILENAME}.",
            suffix=".tmp",
            dir=output.parent,
        )
        temporary = Path(name)
        os.fchmod(descriptor, 0o644)
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        try:
            os.link(temporary, output, follow_symlinks=False)
        except FileExistsError:
            fail("manifest 输出已存在，拒绝覆盖")
        temporary.unlink()
        temporary = None
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def render(arguments) -> None:
    output = resolve_output(arguments.output)
    rows = load_asset_index(arguments.index, arguments.profile)
    document = build_document(
        arguments.root,
        arguments.profile,
        arguments.revision,
        arguments.tag,
        arguments.assets,
        rows,
        output,
    )
    write_exclusive_atomic(output, canonical_json(document))
    print(f"Apple Release manifest 已生成：{output}")


def load_manifest(path: Path):
    if path.name != MANIFEST_FILENAME:
        fail(f"manifest 文件名必须是 {MANIFEST_FILENAME}")
    resolved = resolve_regular(path, "Apple Release manifest", mode=0o644)
    data = resolved.read_bytes()
    try:
        document = json.loads(
            data,
            parse_constant=lambda value: fail(
                f"JSON 不能包含非有限常量：{value}"
            ),
        )
    except (ValueError, UnicodeError) as error:
        fail(f"Apple Release manifest 不是有效 UTF-8 JSON：{error}")
    if not isinstance(document, dict):
        fail("Apple Release manifest 顶层必须是对象")
    if canonical_json(document) != data:
        fail("Apple Release manifest 不是规范 JSON")
    return resolved, data, document


def manifest_string(document, *path):
    value = document
    for component in path:
        if not isinstance(value, dict) or component not in value:
            fail(f"Apple Release manifest 缺少字段：{'.'.join(path)}")
        value = value[component]
    if not isinstance(value, str) or not value:
        fail(f"Apple Release manifest 字段必须是非空字符串：{'.'.join(path)}")
    return value


def verify(arguments) -> None:
    manifest, data, document = load_manifest(arguments.manifest)
    if manifest_string(document, "schema") != SCHEMA:
        fail("Apple Release manifest schema 不受支持")
    profile = manifest_string(document, "profile")
    if profile not in {"core", "with-linux"}:
        fail("Apple Release manifest profile 不受支持")
    revision = manifest_string(document, "repository", "revision")
    tag = manifest_string(document, "repository", "tag")
    rows = expected_assets(profile)
    expected = build_document(
        arguments.root,
        profile,
        revision,
        tag,
        arguments.assets,
        rows,
        manifest,
    )
    if canonical_json(expected) != data:
        fail("Apple Release manifest 与当前仓库、tag 或候选资产不一致")
    print("Apple Release manifest 校验通过")


def make_parser():
    parser = ChineseArgumentParser(
        description="生成并校验精确绑定 Apple 产品、源码与 Git revision 的 manifest"
    )
    commands = parser.add_subparsers(
        dest="command",
        required=True,
        parser_class=ChineseArgumentParser,
    )
    render_parser = commands.add_parser("render", help="生成确定性 manifest")
    render_parser.add_argument("--root", type=Path, required=True)
    render_parser.add_argument(
        "--profile", choices=("core", "with-linux"), required=True
    )
    render_parser.add_argument("--revision", required=True)
    render_parser.add_argument("--tag", required=True)
    render_parser.add_argument("--assets", type=Path, required=True)
    render_parser.add_argument("--index", type=Path, required=True)
    render_parser.add_argument("--output", type=Path, required=True)
    verify_parser = commands.add_parser("verify", help="全量重算并校验 manifest")
    verify_parser.add_argument("--root", type=Path, required=True)
    verify_parser.add_argument("--manifest", type=Path, required=True)
    verify_parser.add_argument("--assets", type=Path, required=True)
    return parser


def main(argv=None) -> int:
    arguments = make_parser().parse_args(argv)
    try:
        if arguments.command == "render":
            render(arguments)
        else:
            verify(arguments)
        return 0
    except (ManifestError, OSError, UnicodeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

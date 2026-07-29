#!/usr/bin/env python3

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import signal
import stat
import subprocess
import sys
import tempfile


sys.dont_write_bytecode = True

from apple_release_archive import (
    ArchiveError,
    ArchiveSpec,
    TreeEntry,
    git_blob_digest,
    verify_archive,
    write_archive,
)
from apple_release_stage import (
    StageError,
    TERMINATION_SIGNALS,
    cleanup_stage,
    ensure_child_directory_identity,
    ensure_directory_identity,
    open_child_directory_descriptor,
    open_directory_descriptor,
    publish_no_replace,
)


HEX40 = re.compile(r"^[0-9a-f]{40}$")
DEPENDENCIES_PATH = "third_party/apple-host/dependencies.tsv"
DEPENDENCIES_HEADER = (
    "component\tversion\tversion_source\tgitlink_path\tgitlink_commit"
    "\tsource_url\tdelivery_unit\tdelivery_kind\tdelivery_name"
    "\tinput_count\tinput_sha256"
)
GITLINK_ASSETS = {
    "deps/libapps": ("deps-libapps-source.tar", "libapps"),
    "deps/libarchive": ("deps-libarchive-source.tar", "libarchive"),
    "deps/linux": ("deps-linux-source.tar", "linux"),
}
PROJECT_ASSET = "project-source.tar"
TOOLCHAIN_ASSET = "apple-toolchain.tsv"
ASSET_FILENAMES = (
    PROJECT_ASSET,
    "deps-libapps-source.tar",
    "deps-libarchive-source.tar",
    "deps-linux-source.tar",
    TOOLCHAIN_ASSET,
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
GIT_ENV = {
    key: value
    for key, value in os.environ.items()
    if not key.startswith("GIT_")
}
GIT_ENV.update(
    {
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_NO_LAZY_FETCH": "1",
        "GIT_NO_REPLACE_OBJECTS": "1",
        "GIT_OPTIONAL_LOCKS": "0",
        "LC_ALL": "C",
    }
)
TOOL_ENV = {
    key: value
    for key, value in os.environ.items()
    if not key.startswith("GIT_")
}
TOOL_ENV["LC_ALL"] = "C"


class AssetsError(ValueError):
    pass


class ChineseArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"错误：命令行参数无效：{message}\n")


def fail(message: str) -> None:
    raise AssetsError(message)


def decode_utf8(data: bytes, description: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"{description}不是 UTF-8：{error}")


def run_git(root: Path, *arguments: str, allow_absent=False) -> bytes | None:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=GIT_ENV,
    )
    if allow_absent and result.returncode == 1:
        return None
    if result.returncode != 0:
        detail = decode_utf8(
            result.stderr[:4096], "Git 错误输出"
        ).strip()
        fail(f"Git 检查失败：{detail or '未知错误'}")
    return result.stdout


def resolve_directory(path: Path, description: str, required_mode=None) -> Path:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        fail(f"{description}不存在：{path}")
    except OSError as error:
        fail(f"无法检查{description}：{path}（{error}）")
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        fail(f"{description}必须是实体目录且不能是符号链接：{path}")
    if (
        required_mode is not None
        and stat.S_IMODE(metadata.st_mode) != required_mode
    ):
        fail(f"{description}权限必须为 {required_mode:04o}：{path}")
    try:
        return path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        fail(f"无法解析{description}：{path}（{error}）")


def resolve_repository(path: Path, description: str) -> Path:
    root = resolve_directory(path, description)
    top = decode_utf8(
        run_git(root, "rev-parse", "--show-toplevel") or b"",
        f"{description}顶层",
    ).strip()
    try:
        top_path = Path(top).resolve(strict=True)
    except (OSError, RuntimeError):
        fail(f"{description}不是可解析的 Git 顶层")
    if top_path != root:
        fail(f"{description}必须直接指向独立 Git 顶层")
    object_format = decode_utf8(
        run_git(root, "rev-parse", "--show-object-format") or b"",
        f"{description}对象格式",
    ).strip()
    if object_format != "sha1":
        fail(f"{description}必须使用当前发行合同支持的 SHA-1 Git 对象格式")
    partial_extension = run_git(
        root, "config", "--local", "--get", "extensions.partialClone",
        allow_absent=True,
    )
    promisor = run_git(
        root,
        "config",
        "--local",
        "--get-regexp",
        r"^remote\..*\.promisor$",
        allow_absent=True,
    )
    if partial_extension or promisor:
        fail(f"{description}不能是 partial clone 或 promisor 仓库")
    return root


def validate_relative(value: str, description: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or str(path) != value
        or "\\" in value
        or "\0" in value
        or "\r" in value
        or "\n" in value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        fail(f"{description}不是安全、规范的相对路径：{value}")
    return value


@dataclass(frozen=True)
class RepositorySnapshot:
    head: str
    tree_raw: bytes
    index_raw: bytes
    flags_raw: bytes
    status_raw: bytes
    entries: tuple[TreeEntry, ...]

def parse_tree(raw: bytes, description: str) -> tuple[TreeEntry, ...]:
    entries: list[TreeEntry] = []
    previous: bytes | None = None
    for record in raw.split(b"\0"):
        if not record:
            continue
        if b"\t" not in record:
            fail(f"{description}含非法 Git tree 记录")
        metadata, path_bytes = record.split(b"\t", 1)
        fields = metadata.split()
        if len(fields) != 3:
            fail(f"{description}含非法 Git tree 元数据")
        mode_bytes, kind_bytes, oid_bytes = fields
        path = decode_utf8(path_bytes, f"{description}路径")
        validate_relative(path, f"{description}路径")
        mode = decode_utf8(mode_bytes, f"{description}模式")
        kind = decode_utf8(kind_bytes, f"{description}类型")
        oid = decode_utf8(oid_bytes, f"{description}对象 ID")
        if (
            mode not in {"100644", "100755", "120000", "160000"}
            or kind != ("commit" if mode == "160000" else "blob")
            or HEX40.fullmatch(oid) is None
            or (previous is not None and path_bytes <= previous)
        ):
            fail(f"{description}含不支持、重复或未排序的 tree 条目：{path}")
        entries.append(TreeEntry(path, path_bytes, mode, kind, oid))
        previous = path_bytes
    if not entries:
        fail(f"{description}不能是空 tree")
    return tuple(entries)


def parse_index(raw: bytes, description: str):
    entries = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        if b"\t" not in record:
            fail(f"{description}含非法 Git index 记录")
        metadata, path_bytes = record.split(b"\t", 1)
        fields = metadata.split()
        if len(fields) != 3:
            fail(f"{description}含非法 Git index 元数据")
        mode_bytes, oid_bytes, stage_bytes = fields
        path = decode_utf8(path_bytes, f"{description}路径")
        validate_relative(path, f"{description}路径")
        mode = decode_utf8(mode_bytes, f"{description}模式")
        oid = decode_utf8(oid_bytes, f"{description}对象 ID")
        if (
            stage_bytes != b"0"
            or mode not in {"100644", "100755", "120000", "160000"}
            or HEX40.fullmatch(oid) is None
        ):
            fail(f"{description}含非零 stage 或不支持的条目：{path}")
        entries.append((path_bytes, mode, oid))
    return tuple(entries)


def parse_flags(raw: bytes, description: str):
    flags = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        if len(record) < 3 or record[1:2] != b" ":
            fail(f"{description}含非法 Git index flag 记录")
        path_bytes = record[2:]
        path = decode_utf8(path_bytes, f"{description}路径")
        validate_relative(path, f"{description}路径")
        flags.append((path_bytes, decode_utf8(record[:1], f"{description}标志")))
    return tuple(flags)


def file_blob_digest(path: Path, size: int) -> str:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        fail("当前平台缺少 O_NOFOLLOW，无法安全检查 sparse 文件")
    descriptor = os.open(path, flags | nofollow)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size != size:
            fail(f"sparse 工作树条目类型或大小漂移：{path}")
        digest = hashlib.sha1()
        digest.update(f"blob {size}\0".encode("ascii"))
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def verify_materialized_sparse(root: Path, entry: TreeEntry) -> None:
    path = root.joinpath(*PurePosixPath(entry.path).parts)
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return
    except OSError as error:
        fail(f"无法检查 sparse 工作树条目 {entry.path}：{error}")
    if entry.mode == "120000":
        if not stat.S_ISLNK(metadata.st_mode):
            fail(f"sparse 工作树条目类型漂移：{entry.path}")
        target = os.fsencode(os.readlink(path))
        if git_blob_digest(target) != entry.oid:
            fail(f"sparse 工作树条目内容漂移：{entry.path}")
        return
    if entry.mode not in {"100644", "100755"}:
        fail(f"sparse 工作树含不支持的条目：{entry.path}")
    if bool(stat.S_IMODE(metadata.st_mode) & 0o111) != (
        entry.mode == "100755"
    ):
        fail(f"sparse 工作树条目 executable bit 漂移：{entry.path}")
    if file_blob_digest(path, metadata.st_size) != entry.oid:
        fail(f"sparse 工作树条目内容漂移：{entry.path}")


def inspect_repository(
    root: Path,
    revision: str,
    description: str,
    allow_sparse: bool,
) -> RepositorySnapshot:
    head = decode_utf8(
        run_git(root, "rev-parse", "--verify", "HEAD") or b"",
        f"{description} HEAD",
    ).strip()
    if head != revision:
        fail(f"{description} HEAD 与固定 revision 不一致")
    tree_raw = run_git(
        root, "ls-tree", "-r", "--full-tree", "-z", revision
    ) or b""
    entries = parse_tree(tree_raw, f"{description} tree")
    index_raw = run_git(root, "ls-files", "--stage", "-z") or b""
    index_entries = parse_index(index_raw, f"{description} index")
    expected_index = tuple(
        (entry.path_bytes, entry.mode, entry.oid) for entry in entries
    )
    if index_entries != expected_index:
        fail(f"{description} index 与固定 revision tree 不一致")
    flags_raw = run_git(root, "ls-files", "-v", "-z") or b""
    flags = parse_flags(flags_raw, f"{description} index flag")
    if tuple(path for path, _flag in flags) != tuple(
        entry.path_bytes for entry in entries
    ):
        fail(f"{description} index flag 路径集合漂移")
    by_path = {entry.path_bytes: entry for entry in entries}
    for path_bytes, flag in flags:
        if flag == "H":
            continue
        if flag == "S" and allow_sparse:
            # Linux 稀疏工作树可以不物化文件；一旦物化，仍必须与固定 blob 一致。
            verify_materialized_sparse(root, by_path[path_bytes])
            continue
        path = decode_utf8(path_bytes, f"{description} index flag 路径")
        fail(f"{description}含不允许的 index 标志 {flag}：{path}")
    status_raw = run_git(
        root,
        "status",
        "--porcelain=v1",
        "-z",
        "--untracked-files=all",
        "--ignore-submodules=none",
    ) or b""
    if status_raw:
        fail(f"{description}工作树、index 或未跟踪文件不干净")
    return RepositorySnapshot(
        head, tree_raw, index_raw, flags_raw, status_raw, entries
    )


def resolve_revision(root: Path, revision: str) -> str:
    if HEX40.fullmatch(revision) is None:
        fail("revision 必须是小写完整 40 位 Git OID")
    resolved = decode_utf8(
        run_git(root, "rev-parse", "--verify", f"{revision}^{{commit}}") or b"",
        "发行 revision",
    ).strip()
    if resolved != revision:
        fail("revision 不能缩写、漂移或指向非 commit 对象")
    return revision


def load_locked_gitlinks(
    root: Path,
    revision: str,
    entries: tuple[TreeEntry, ...],
) -> dict[str, str]:
    data = run_git(root, "show", f"{revision}:{DEPENDENCIES_PATH}") or b""
    if not data or not data.endswith(b"\n") or b"\r" in data:
        fail("宿主依赖来源锁必须非空、只使用 LF 并以 LF 结尾")
    lines = decode_utf8(data, "宿主依赖来源锁").splitlines()
    if not lines or lines[0] != DEPENDENCIES_HEADER:
        fail("宿主依赖来源锁表头漂移")
    locked: dict[str, str] = {}
    components: set[str] = set()
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 11 or any(not field for field in fields):
            fail("宿主依赖来源锁含空字段或列数错误")
        component, path, oid = fields[0], fields[3], fields[4]
        validate_relative(path, "宿主依赖 gitlink 路径")
        if component in components or HEX40.fullmatch(oid) is None:
            fail("宿主依赖来源锁含重复 component 或非法 gitlink OID")
        components.add(component)
        previous = locked.setdefault(path, oid)
        if previous != oid:
            fail(f"宿主依赖对同一 gitlink 声明了不同 OID：{path}")
    parent = {
        entry.path: entry.oid for entry in entries if entry.mode == "160000"
    }
    if set(parent) != set(GITLINK_ASSETS) or set(locked) != set(parent):
        fail("父仓库全部 gitlink、公共资产合同与宿主依赖来源锁不一致")
    for path, oid in parent.items():
        if locked[path] != oid:
            fail(f"gitlink 的 commit tree 与宿主依赖来源锁不一致：{path}")
    return parent


def collect_specs(root_argument: Path, revision_argument: str):
    root = resolve_repository(root_argument, "项目根目录")
    revision = resolve_revision(root, revision_argument)
    parent_snapshot = inspect_repository(
        root, revision, "项目仓库", allow_sparse=False
    )
    gitlinks = load_locked_gitlinks(root, revision, parent_snapshot.entries)
    specs = [
        ArchiveSpec(
            PROJECT_ASSET,
            f"ish-multiarch-{revision}",
            root,
            revision,
            parent_snapshot.entries,
        )
    ]
    snapshots = {".": parent_snapshot}
    for relative in sorted(gitlinks):
        child = resolve_repository(root / relative, f"gitlink {relative}")
        child_revision = gitlinks[relative]
        child_snapshot = inspect_repository(
            child,
            child_revision,
            f"gitlink {relative}",
            allow_sparse=relative == "deps/linux",
        )
        if any(entry.mode == "160000" for entry in child_snapshot.entries):
            fail(f"gitlink {relative} 含当前四份源码资产无法表达的嵌套 gitlink")
        filename, prefix_name = GITLINK_ASSETS[relative]
        specs.append(
            ArchiveSpec(
                filename,
                f"{prefix_name}-{child_revision}",
                child,
                child_revision,
                child_snapshot.entries,
            )
        )
        snapshots[relative] = child_snapshot
    return root, revision, tuple(specs), snapshots


def run_tool(arguments: list[str], description: str) -> tuple[str, str]:
    try:
        result = subprocess.run(
            arguments,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=TOOL_ENV,
            timeout=30,
        )
    except FileNotFoundError:
        fail(f"未找到{description}命令：{arguments[0]}")
    except subprocess.TimeoutExpired:
        fail(f"{description}命令执行超时")
    stdout = decode_utf8(result.stdout, f"{description}标准输出")
    stderr = decode_utf8(result.stderr, f"{description}错误输出")
    if result.returncode != 0:
        detail = (stderr or stdout).strip()[:4096]
        fail(f"{description}命令失败：{detail or '未知错误'}")
    return stdout, stderr


def first_nonempty_line(value: str, description: str) -> str:
    lines = [line for line in value.splitlines() if line]
    if not lines:
        fail(f"{description}没有返回版本值")
    return lines[0]


def validate_toolchain_value(key: str, value: str) -> str:
    if (
        not value
        or value.strip() != value
        or any(character in value for character in ("\t", "\r", "\n", "\0"))
    ):
        fail(f"Apple 工具链元数据值非法：{key}")
    return value


def source_date_epoch(root: Path, revision: str) -> str:
    value = os.environ.get("SOURCE_DATE_EPOCH")
    if value is None:
        value = decode_utf8(
            run_git(root, "show", "-s", "--format=%ct", revision) or b"",
            "提交时间",
        ).strip()
    if not value or not value.isascii() or not value.isdecimal():
        fail("SOURCE_DATE_EPOCH 必须是非负 ASCII 十进制整数")
    return value


def collect_toolchain(root: Path, revision: str) -> bytes:
    clang_stdout, _ = run_tool(
        ["xcrun", "clang", "--version"], "Apple Clang"
    )
    ld_stdout, _ = run_tool(
        ["xcrun", "ld", "-version_details"], "Apple ld"
    )
    try:
        ld_document = json.loads(ld_stdout)
        ld_raw = ld_document["version"]
    except (KeyError, TypeError, ValueError, json.JSONDecodeError):
        fail("Apple ld -version_details 没有返回含 version 的 JSON")
    if not isinstance(ld_raw, (str, int, float)):
        fail("Apple ld version 类型非法")
    xcode_stdout, _ = run_tool(["xcodebuild", "-version"], "Xcode")
    xcode_lines = [
        line.removeprefix("Build version ")
        for line in xcode_stdout.splitlines()
        if line.startswith("Build version ")
    ]
    if len(xcode_lines) != 1:
        fail("xcodebuild -version 没有返回唯一 Build version")
    iphone_stdout, _ = run_tool(
        ["xcrun", "--sdk", "iphoneos", "--show-sdk-build-version"],
        "iPhoneOS SDK",
    )
    watch_stdout, _ = run_tool(
        ["xcrun", "--sdk", "watchos", "--show-sdk-build-version"],
        "watchOS SDK",
    )
    meson_stdout, _ = run_tool(
        [os.environ.get("MESON", "meson"), "--version"], "Meson"
    )
    ninja_stdout, _ = run_tool(
        [os.environ.get("NINJA", "ninja"), "--version"], "Ninja"
    )
    values = {
        "clang_version": first_nonempty_line(
            clang_stdout, "Apple Clang"
        ),
        "host_arch": platform.machine(),
        "iphoneos_sdk_build": first_nonempty_line(
            iphone_stdout, "iPhoneOS SDK"
        ),
        "ld_version": f"ld64-{ld_raw}",
        "meson_version": first_nonempty_line(meson_stdout, "Meson"),
        "ninja_version": first_nonempty_line(ninja_stdout, "Ninja"),
        "source_date_epoch": source_date_epoch(root, revision),
        "watchos_sdk_build": first_nonempty_line(
            watch_stdout, "watchOS SDK"
        ),
        "xcode_build": xcode_lines[0],
    }
    rows = [
        f"{key}\t{validate_toolchain_value(key, values[key])}\n"
        for key in TOOLCHAIN_KEYS
    ]
    return "".join(rows).encode("utf-8")


def write_regular(path: Path, data: bytes) -> None:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        fail("当前平台缺少 O_NOFOLLOW，无法安全生成发行资产")
    descriptor = os.open(
        path,
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_CLOEXEC", 0)
        | nofollow,
        0o600,
    )
    try:
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
        os.fchmod(descriptor, 0o644)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def expected_epoch(root: Path, revision: str) -> str:
    return source_date_epoch(root, revision)


def read_toolchain_asset(path: Path) -> bytes:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        fail(f"公共资产不存在：{path.name}")
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISREG(metadata.st_mode)
        or stat.S_IMODE(metadata.st_mode) != 0o644
    ):
        fail(f"公共资产必须是 0644 普通文件：{path.name}")
    if metadata.st_size > 64 * 1024:
        fail("Apple 工具链元数据超过固定大小上限")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        fail("当前平台缺少 O_NOFOLLOW，无法安全校验发行资产")
    descriptor = os.open(
        path,
        os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | nofollow,
    )
    try:
        opened = os.fstat(descriptor)
        if (
            (opened.st_dev, opened.st_ino)
            != (metadata.st_dev, metadata.st_ino)
            or not stat.S_ISREG(opened.st_mode)
            or stat.S_IMODE(opened.st_mode) != 0o644
            or opened.st_size != metadata.st_size
        ):
            fail("Apple 工具链元数据在打开期间被替换")
        chunks = []
        while True:
            chunk = os.read(descriptor, 4096)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def verify_toolchain(path: Path, root: Path, revision: str) -> None:
    data = read_toolchain_asset(path)
    if not data or not data.endswith(b"\n") or b"\r" in data or b"\0" in data:
        fail("Apple 工具链元数据必须非空、只使用 LF 并以 LF 结尾")
    lines = decode_utf8(data, "Apple 工具链元数据")[:-1].split("\n")
    if len(lines) != len(TOOLCHAIN_KEYS):
        fail("Apple 工具链元数据行数漂移")
    values = {}
    for expected_key, line in zip(TOOLCHAIN_KEYS, lines):
        fields = line.split("\t")
        if len(fields) != 2 or fields[0] != expected_key:
            fail("Apple 工具链元数据键集合或排序漂移")
        values[expected_key] = validate_toolchain_value(
            expected_key, fields[1]
        )
    if values["source_date_epoch"] != expected_epoch(root, revision):
        fail("Apple 工具链 source_date_epoch 与当前发行环境不一致")


def verify_assets_directory(
    assets_argument: Path,
    root: Path,
    revision: str,
    specs: tuple[ArchiveSpec, ...],
) -> Path:
    assets = resolve_directory(
        assets_argument, "公共资产目录", required_mode=0o755
    )
    try:
        children = sorted(item.name for item in assets.iterdir())
    except OSError as error:
        fail(f"无法列举公共资产目录：{error}")
    if children != sorted(ASSET_FILENAMES):
        fail("公共资产目录必须精确包含固定五项资产")
    by_filename = {spec.filename: spec for spec in specs}
    for filename in ASSET_FILENAMES[:-1]:
        verify_archive(assets / filename, by_filename[filename])
    verify_toolchain(assets / TOOLCHAIN_ASSET, root, revision)
    return assets


def ensure_repository_unchanged(
    root: Path,
    revision: str,
    previous: dict[str, RepositorySnapshot],
) -> None:
    current_parent = inspect_repository(
        root, revision, "项目仓库", allow_sparse=False
    )
    if current_parent != previous["."]:
        fail("生成期间项目仓库状态发生变化")
    gitlinks = load_locked_gitlinks(root, revision, current_parent.entries)
    if set(gitlinks) != set(previous) - {"."}:
        fail("生成期间 gitlink 集合发生变化")
    for relative, child_revision in sorted(gitlinks.items()):
        child = resolve_repository(root / relative, f"gitlink {relative}")
        current = inspect_repository(
            child,
            child_revision,
            f"gitlink {relative}",
            allow_sparse=relative == "deps/linux",
        )
        if current != previous[relative]:
            fail(f"生成期间 gitlink {relative} 状态发生变化")


def resolve_output(path: Path) -> Path:
    if not path.name or path.name in {".", ".."}:
        fail("输出目录名非法")
    parent = resolve_directory(path.parent, "输出父目录")
    output = parent / path.name
    try:
        output.lstat()
    except FileNotFoundError:
        return output
    except OSError as error:
        fail(f"无法检查输出目录：{error}")
    fail("输出目录已存在，拒绝覆盖")


def render(arguments) -> None:
    output = resolve_output(arguments.output)
    parent_descriptor, parent_identity = open_directory_descriptor(
        output.parent
    )
    private_stage = None
    private_stage_name = None
    private_stage_descriptor = -1
    candidate_name = "candidate"
    candidate_descriptor = -1
    try:
        root, revision, specs, snapshots = collect_specs(
            arguments.root, arguments.revision
        )
        private_stage = Path(
            tempfile.mkdtemp(
                prefix=".apple-release-assets-stage-", dir=output.parent
            )
        )
        private_stage_name = private_stage.name
        (
            private_stage_descriptor,
            private_stage_identity,
        ) = open_directory_descriptor(private_stage)
        os.mkdir(
            candidate_name,
            mode=0o700,
            dir_fd=private_stage_descriptor,
        )
        (
            candidate_descriptor,
            candidate_identity,
        ) = open_child_directory_descriptor(
            private_stage_descriptor, candidate_name
        )
        stage = private_stage / candidate_name
        for spec in specs:
            write_archive(stage / spec.filename, spec, GIT_ENV)
        write_regular(
            stage / TOOLCHAIN_ASSET, collect_toolchain(root, revision)
        )
        os.fchmod(candidate_descriptor, 0o755)
        verify_assets_directory(stage, root, revision, specs)
        ensure_repository_unchanged(root, revision, snapshots)
        ensure_directory_identity(
            output.parent,
            parent_descriptor,
            parent_identity,
            "输出父目录",
        )
        ensure_directory_identity(
            private_stage,
            private_stage_descriptor,
            private_stage_identity,
            "私有暂存目录",
        )
        ensure_child_directory_identity(
            private_stage_descriptor,
            candidate_name,
            candidate_descriptor,
            candidate_identity,
            "发行候选目录",
        )
        os.fsync(candidate_descriptor)
        os.fsync(private_stage_descriptor)
        publish_no_replace(
            private_stage_descriptor,
            candidate_name,
            parent_descriptor,
            output.name,
            private_stage_name,
            f"Apple 发行公共资产已生成并校验：{output}\n",
        )
    finally:
        cleanup_mask = signal.pthread_sigmask(
            signal.SIG_BLOCK, TERMINATION_SIGNALS
        )
        try:
            try:
                cleanup_stage(
                    parent_descriptor,
                    private_stage_name,
                    private_stage_descriptor,
                    candidate_name,
                    candidate_descriptor,
                    ASSET_FILENAMES,
                )
            finally:
                os.close(parent_descriptor)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, cleanup_mask)


def verify(arguments) -> None:
    root, revision, specs, snapshots = collect_specs(
        arguments.root, arguments.revision
    )
    verify_assets_directory(arguments.assets, root, revision, specs)
    ensure_repository_unchanged(root, revision, snapshots)
    print("Apple 发行公共资产校验通过")


def make_parser():
    parser = ChineseArgumentParser(
        description="生成并校验 profile 无关的 Apple 发行公共资产"
    )
    commands = parser.add_subparsers(
        dest="command",
        required=True,
        parser_class=ChineseArgumentParser,
    )
    render_parser = commands.add_parser(
        "render", help="原子生成四份源码 tar 与工具链元数据"
    )
    render_parser.add_argument("--root", type=Path, required=True)
    render_parser.add_argument("--revision", required=True)
    render_parser.add_argument("--output", type=Path, required=True)
    verify_parser = commands.add_parser(
        "verify", help="只读校验既有公共资产"
    )
    verify_parser.add_argument("--root", type=Path, required=True)
    verify_parser.add_argument("--revision", required=True)
    verify_parser.add_argument("--assets", type=Path, required=True)
    return parser


def interrupted(signum: int, _frame) -> None:
    signal_name = signal.Signals(signum).name
    fail(f"收到 {signal_name}，已中止发行公共资产暂存")


def main(argv=None) -> int:
    arguments = make_parser().parse_args(argv)
    previous_handlers = {
        current_signal: signal.signal(current_signal, interrupted)
        for current_signal in TERMINATION_SIGNALS
    }
    try:
        try:
            if arguments.command == "render":
                render(arguments)
            else:
                verify(arguments)
            return 0
        except (
            AssetsError,
            ArchiveError,
            StageError,
            OSError,
            UnicodeError,
        ) as error:
            print(f"错误：{error}", file=sys.stderr)
            return 1
    finally:
        for current_signal, handler in previous_handlers.items():
            signal.signal(current_signal, handler)


if __name__ == "__main__":
    raise SystemExit(main())

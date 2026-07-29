#!/usr/bin/env python3

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile


sys.dont_write_bytecode = True

PROJECT_ROOT = Path(__file__).resolve().parent.parent
TOOL = PROJECT_ROOT / "tools" / "apple-release-assets.py"
DEPENDENCIES_HEADER = (
    "component\tversion\tversion_source\tgitlink_path\tgitlink_commit"
    "\tsource_url\tdelivery_unit\tdelivery_kind\tdelivery_name"
    "\tinput_count\tinput_sha256"
)
ASSETS = (
    "project-source.tar",
    "deps-libapps-source.tar",
    "deps-libarchive-source.tar",
    "deps-linux-source.tar",
    "apple-toolchain.tsv",
)
EPOCH = "1700000000"
GIT_ENV = {
    key: value
    for key, value in os.environ.items()
    if not key.startswith("GIT_")
}
GIT_ENV.update(
    {
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_AUTHOR_NAME": "发行资产测试",
        "GIT_AUTHOR_EMAIL": "release-assets@example.invalid",
        "GIT_COMMITTER_NAME": "发行资产测试",
        "GIT_COMMITTER_EMAIL": "release-assets@example.invalid",
        "GIT_AUTHOR_DATE": "1700000000 +0000",
        "GIT_COMMITTER_DATE": "1700000000 +0000",
        "LC_ALL": "C",
    }
)


@dataclass
class Fixture:
    container: Path
    root: Path
    fake_bin: Path
    outputs: Path
    revision: str
    children: dict[str, str]


def git(root: Path, *arguments: str, check=True) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=GIT_ENV,
    )
    if check and result.returncode != 0:
        raise AssertionError(
            f"Git 命令失败：{' '.join(arguments)}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    return result.stdout


def init_repository(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    git(root, "init", "-q")
    git(root, "config", "commit.gpgsign", "false")
    git(root, "config", "core.hooksPath", os.devnull)


def write(path: Path, data: str, mode=0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(data)
    path.chmod(mode)


def commit_all(root: Path, message: str) -> str:
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", message)
    return git(root, "rev-parse", "HEAD").decode().strip()


def make_child(root: Path, name: str, sparse=False) -> str:
    init_repository(root)
    write(root / "README.txt", f"{name} 源码\n")
    write(root / "bin" / "run", "#!/bin/sh\nexit 0\n", 0o755)
    write(root / "target.txt", "符号链接目标\n")
    os.symlink("target.txt", root / "target-link")
    if sparse:
        write(root / "sparse.txt", "未物化但必须进入归档\n")
    revision = commit_all(root, "test: 建立子仓库夹具")
    if sparse:
        git(root, "update-index", "--skip-worktree", "sparse.txt")
        (root / "sparse.txt").unlink()
    return revision


def dependency_row(component: str, path: str, oid: str) -> str:
    return (
        f"{component}\tsnapshot\tgitlink\t{path}\t{oid}"
        f"\thttps://example.invalid/{component}"
        f"\t{component}\tsource\t{component}.a\t1\t"
        f"{hashlib.sha256(component.encode()).hexdigest()}\n"
    )


def write_dependencies(root: Path, children: dict[str, str]) -> None:
    data = DEPENDENCIES_HEADER + "\n"
    data += dependency_row("libapps", "deps/libapps", children["deps/libapps"])
    data += dependency_row(
        "libarchive", "deps/libarchive", children["deps/libarchive"]
    )
    data += dependency_row("linux", "deps/linux", children["deps/linux"])
    write(root / "third_party" / "apple-host" / "dependencies.tsv", data)


def write_fake_tools(fake_bin: Path) -> None:
    fake_bin.mkdir()
    scripts = {
        "xcrun": """#!/bin/sh
case "$*" in
  "clang --version")
    printf '%s\\n' 'Apple clang version 21.0.0 (clang-test)' 'Target: ignored'
    ;;
  "ld -version_details")
    printf '%s\\n' '{"version":"1267-test","ignored":"field"}'
    ;;
  "--sdk iphoneos --show-sdk-build-version")
    printf '%s\\n' '23F81a'
    ;;
  "--sdk watchos --show-sdk-build-version")
    printf '%s\\n' '23T570'
    ;;
  *)
    printf '%s\\n' '未知 xcrun 参数' >&2
    exit 9
    ;;
esac
""",
        "xcodebuild": """#!/bin/sh
test "$*" = "-version" || exit 9
printf '%s\\n' 'Xcode 26.6' 'Build version 17F113'
""",
        "meson": """#!/bin/sh
test "$*" = "--version" || exit 9
printf '%s\\n' '1.11.2'
""",
        "ninja": """#!/bin/sh
test "$*" = "--version" || exit 9
printf '%s\\n' '1.13.2'
""",
    }
    for name, contents in scripts.items():
        write(fake_bin / name, contents, 0o755)


def poison_fake_tools(fake_bin: Path) -> None:
    for name in ("xcrun", "xcodebuild", "meson", "ninja"):
        write(
            fake_bin / name,
            "#!/bin/sh\nprintf '%s\\n' 'verify 不得探测工具链' >&2\nexit 99\n",
            0o755,
        )


def make_fixture(container: Path) -> Fixture:
    root = container / "project"
    outputs = container / "outputs"
    fake_bin = container / "fake-bin"
    outputs.mkdir(parents=True)
    children = {
        "deps/libapps": make_child(root / "deps" / "libapps", "libapps"),
        "deps/libarchive": make_child(
            root / "deps" / "libarchive", "libarchive"
        ),
        "deps/linux": make_child(
            root / "deps" / "linux", "linux", sparse=True
        ),
    }
    init_repository(root)
    write(root / "README.txt", "项目源码\n")
    write(root / "run-project", "#!/bin/sh\nexit 0\n", 0o755)
    write(root / "project-target.txt", "项目符号链接目标\n")
    os.symlink("project-target.txt", root / "project-link")
    modules = "".join(
        (
            f'[submodule "{path}"]\n'
            f"\tpath = {path}\n"
            f"\turl = https://example.invalid/{path.rsplit('/', 1)[-1]}\n"
        )
        for path in sorted(children)
    )
    write(root / ".gitmodules", modules)
    write_dependencies(root, children)
    git(
        root,
        "add",
        ".gitmodules",
        "README.txt",
        "run-project",
        "project-target.txt",
        "project-link",
        "third_party/apple-host/dependencies.tsv",
    )
    for path, oid in sorted(children.items()):
        git(
            root,
            "update-index",
            "--add",
            "--cacheinfo",
            f"160000,{oid},{path}",
        )
    git(root, "commit", "-q", "-m", "test: 建立父仓库夹具")
    revision = git(root, "rev-parse", "HEAD").decode().strip()
    write_fake_tools(fake_bin)
    return Fixture(container, root, fake_bin, outputs, revision, children)


def clone_fixture(base: Fixture, destination: Path) -> Fixture:
    shutil.copytree(base.container, destination, symlinks=True)
    return Fixture(
        destination,
        destination / "project",
        destination / "fake-bin",
        destination / "outputs",
        base.revision,
        dict(base.children),
    )


def tool_environment(fixture: Fixture, fake_tools=True):
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("GIT_")
    }
    if fake_tools:
        environment["PATH"] = (
            f"{fixture.fake_bin}{os.pathsep}{environment['PATH']}"
        )
    environment.update(
        {
            "LC_ALL": "C",
            "SOURCE_DATE_EPOCH": EPOCH,
            "TZ": "Pacific/Chatham",
        }
    )
    return environment


def invoke(
    fixture: Fixture,
    command: str,
    path: Path,
    *,
    fake_tools=True,
    environment=None,
) -> subprocess.CompletedProcess:
    arguments = [
        sys.executable,
        "-B",
        str(TOOL),
        command,
        "--root",
        str(fixture.root),
        "--revision",
        fixture.revision,
    ]
    arguments.extend(
        ["--output", str(path)]
        if command == "render"
        else ["--assets", str(path)]
    )
    return subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment or tool_environment(fixture, fake_tools),
    )


def assert_success(result: subprocess.CompletedProcess, description: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{description}失败\nstdout={result.stdout.decode(errors='replace')}"
            f"\nstderr={result.stderr.decode(errors='replace')}"
        )


def assert_failure(
    result: subprocess.CompletedProcess,
    description: str,
    expected: str | None = None,
) -> None:
    stderr = result.stderr.decode(errors="replace")
    if result.returncode == 0 or "错误：" not in stderr or "Traceback" in stderr:
        raise AssertionError(
            f"{description}没有稳定失败\nstdout="
            f"{result.stdout.decode(errors='replace')}\nstderr={stderr}"
        )
    if expected is not None and expected not in stderr:
        raise AssertionError(f"{description}未包含预期诊断 {expected!r}：{stderr}")


def assert_no_stage(fixture: Fixture) -> None:
    leftovers = list(
        fixture.outputs.glob(".apple-release-assets-stage-*")
    )
    if leftovers:
        raise AssertionError(f"失败后残留私有暂存目录：{leftovers}")


def expected_members(
    fixture: Fixture, relative: str | None, prefix_name: str
) -> dict[str, tuple[str, str, bytes | None]]:
    root = fixture.root if relative is None else fixture.root / relative
    revision = fixture.revision if relative is None else fixture.children[relative]
    raw = git(root, "ls-tree", "-r", "--full-tree", "-z", revision)
    prefix = f"{prefix_name}-{revision}"
    result = {prefix: ("dir", "", None)}
    for record in raw.split(b"\0"):
        if not record:
            continue
        metadata, path_bytes = record.split(b"\t", 1)
        mode, kind, oid = metadata.decode().split()
        path = path_bytes.decode()
        name = f"{prefix}/{path}"
        if mode == "160000":
            result[name] = ("dir", oid, None)
        elif mode == "120000":
            result[name] = (
                "link",
                oid,
                git(root, "cat-file", "blob", oid),
            )
        else:
            result[name] = (
                "file",
                oid,
                git(root, "cat-file", "blob", oid),
            )
    return result


def inspect_archive(
    path: Path,
    expected: dict[str, tuple[str, str, bytes | None]],
) -> None:
    with tarfile.open(path, "r:") as archive:
        members = archive.getmembers()
        if [member.name for member in members] != list(expected):
            raise AssertionError(f"{path.name} 成员集合或顺序漂移")
        for member in members:
            kind, _oid, data = expected[member.name]
            if (
                member.uid != 0
                or member.gid != 0
                or member.uname != ""
                or member.gname != ""
                or member.mtime != 0
                or member.pax_headers
            ):
                raise AssertionError(f"{member.name} 规范元数据漂移")
            if kind == "dir":
                if not member.isdir() or member.mode != 0o755:
                    raise AssertionError(f"{member.name} 目录元数据漂移")
            elif kind == "link":
                if (
                    not member.issym()
                    or member.mode != 0o777
                    or member.linkname.encode() != data
                ):
                    raise AssertionError(f"{member.name} 符号链接漂移")
            else:
                source = archive.extractfile(member)
                contents = source.read() if source is not None else None
                expected_mode = (
                    0o755 if member.name.endswith("/bin/run")
                    or member.name.endswith("/run-project") else 0o644
                )
                if (
                    not member.isfile()
                    or member.mode != expected_mode
                    or contents != data
                ):
                    raise AssertionError(f"{member.name} 普通文件漂移")


def positive_contract(base: Fixture, work: Path) -> None:
    fixture = clone_fixture(base, work / "positive")
    first = fixture.outputs / "first"
    second = fixture.outputs / "second"
    default_epoch = fixture.outputs / "default-epoch"
    before_status = {
        path: git(
            fixture.root if path == "." else fixture.root / path,
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
        )
        for path in (".", *sorted(fixture.children))
    }
    assert_success(invoke(fixture, "render", first), "首次公共资产生成")
    previous_umask = os.umask(0o077)
    try:
        assert_success(invoke(fixture, "render", second), "第二次公共资产生成")
    finally:
        os.umask(previous_umask)
    default_environment = tool_environment(fixture)
    default_environment.pop("SOURCE_DATE_EPOCH")
    default_environment["MESON"] = str(fixture.fake_bin / "meson")
    default_environment["NINJA"] = str(fixture.fake_bin / "ninja")
    assert_success(
        invoke(
            fixture,
            "render",
            default_epoch,
            environment=default_environment,
        ),
        "默认提交时间公共资产生成",
    )
    assert_success(
        invoke(
            fixture,
            "verify",
            default_epoch,
            environment=default_environment,
        ),
        "默认提交时间公共资产复核",
    )
    for directory in (first, second):
        if stat.S_IMODE(directory.lstat().st_mode) != 0o755:
            raise AssertionError("公共资产目录权限不是 0755")
        if sorted(item.name for item in directory.iterdir()) != sorted(ASSETS):
            raise AssertionError("公共资产目录内容漂移")
        for name in ASSETS:
            metadata = (directory / name).lstat()
            if (
                not stat.S_ISREG(metadata.st_mode)
                or stat.S_IMODE(metadata.st_mode) != 0o644
            ):
                raise AssertionError(f"{name} 不是 0644 普通文件")
            if (directory / name).read_bytes() != (second / name).read_bytes():
                raise AssertionError(f"{name} 两次生成字节不一致")
    inspect_archive(
        first / "project-source.tar",
        expected_members(fixture, None, "ish-multiarch"),
    )
    inspect_archive(
        first / "deps-libapps-source.tar",
        expected_members(fixture, "deps/libapps", "libapps"),
    )
    inspect_archive(
        first / "deps-libarchive-source.tar",
        expected_members(fixture, "deps/libarchive", "libarchive"),
    )
    linux_expected = expected_members(fixture, "deps/linux", "linux")
    inspect_archive(first / "deps-linux-source.tar", linux_expected)
    if not any(name.endswith("/sparse.txt") for name in linux_expected):
        raise AssertionError("未物化 sparse 文件没有进入 Linux tar")
    toolchain = (first / "apple-toolchain.tsv").read_text().splitlines()
    expected_toolchain = [
        "clang_version\tApple clang version 21.0.0 (clang-test)",
        f"host_arch\t{platform.machine()}",
        "iphoneos_sdk_build\t23F81a",
        "ld_version\tld64-1267-test",
        "meson_version\t1.11.2",
        "ninja_version\t1.13.2",
        f"source_date_epoch\t{EPOCH}",
        "watchos_sdk_build\t23T570",
        "xcode_build\t17F113",
    ]
    if toolchain != expected_toolchain:
        raise AssertionError("工具链元数据正文漂移")
    poison_fake_tools(fixture.fake_bin)
    assert_success(invoke(fixture, "verify", first), "无工具链只读复核")
    for path, status_before in before_status.items():
        status_after = git(
            fixture.root if path == "." else fixture.root / path,
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
        )
        if status_after != status_before:
            raise AssertionError(f"生成修改了仓库状态：{path}")


def expect_render_failure(
    base: Fixture,
    work: Path,
    name: str,
    mutate,
    expected: str | None = None,
    environment=None,
) -> None:
    fixture = clone_fixture(base, work / name)
    mutate(fixture)
    output = fixture.outputs / "failed"
    result = invoke(
        fixture,
        "render",
        output,
        environment=environment(fixture) if callable(environment) else environment,
    )
    assert_failure(result, name, expected)
    if output.exists() or output.is_symlink():
        raise AssertionError(f"{name} 失败后发布了输出目录")
    assert_no_stage(fixture)


def mutate_main_tracked(fixture: Fixture) -> None:
    write(fixture.root / "README.txt", "脏工作树\n")


def mutate_main_untracked(fixture: Fixture) -> None:
    write(fixture.root / "untracked.txt", "未跟踪\n")


def mutate_assume_unchanged(fixture: Fixture) -> None:
    git(fixture.root, "update-index", "--assume-unchanged", "README.txt")
    write(fixture.root / "README.txt", "被隐藏的修改\n")


def mutate_child_head(fixture: Fixture) -> None:
    child = fixture.root / "deps/libapps"
    write(child / "new.txt", "推进 HEAD\n")
    commit_all(child, "test: 推进子仓库")


def mutate_child_dirty(fixture: Fixture) -> None:
    write(fixture.root / "deps/libarchive" / "README.txt", "子仓库脏树\n")


def mutate_sparse_content(fixture: Fixture) -> None:
    write(fixture.root / "deps/linux" / "sparse.txt", "被隐藏的 sparse 修改\n")


def mutate_sparse_mode(fixture: Fixture) -> None:
    write(
        fixture.root / "deps/linux" / "sparse.txt",
        "未物化但必须进入归档\n",
        0o755,
    )


def mutate_partial_clone(fixture: Fixture) -> None:
    git(fixture.root, "config", "extensions.partialClone", "origin")


def mutate_missing_child(fixture: Fixture) -> None:
    shutil.move(
        fixture.root / "deps/libapps",
        fixture.container / "missing-libapps",
    )


def mutate_symlink_child(fixture: Fixture) -> None:
    child = fixture.root / "deps/libapps"
    outside = fixture.container / "outside-libapps"
    shutil.move(child, outside)
    os.symlink(outside, child)


def advance_parent_for_child(
    fixture: Fixture, relative: str, child_revision: str
) -> None:
    old = fixture.children[relative]
    lock = fixture.root / "third_party/apple-host/dependencies.tsv"
    contents = lock.read_text(encoding="utf-8").replace(old, child_revision)
    with lock.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(contents)
    git(fixture.root, "add", "third_party/apple-host/dependencies.tsv")
    git(
        fixture.root,
        "update-index",
        "--cacheinfo",
        f"160000,{child_revision},{relative}",
    )
    git(fixture.root, "commit", "-q", "-m", "test: 更新子仓库锁")
    fixture.children[relative] = child_revision
    fixture.revision = git(fixture.root, "rev-parse", "HEAD").decode().strip()


def mutate_nested_gitlink(fixture: Fixture) -> None:
    child = fixture.root / "deps/libapps"
    nested = child / "nested"
    nested_oid = make_child(nested, "nested")
    git(
        child,
        "update-index",
        "--add",
        "--cacheinfo",
        f"160000,{nested_oid},nested",
    )
    git(child, "commit", "-q", "-m", "test: 增加嵌套 gitlink")
    child_revision = git(child, "rev-parse", "HEAD").decode().strip()
    advance_parent_for_child(fixture, "deps/libapps", child_revision)


def mutate_unsafe_link(fixture: Fixture) -> None:
    os.symlink("../outside", fixture.root / "escape")
    fixture.revision = commit_all(fixture.root, "test: 增加逃逸符号链接")


def mutate_output_exists(fixture: Fixture) -> None:
    output = fixture.outputs / "failed"
    output.mkdir()
    write(output / "sentinel", "不能覆盖\n")


def broken_tool_environment(fixture: Fixture):
    environment = tool_environment(fixture)
    write(
        fixture.fake_bin / "xcodebuild",
        "#!/bin/sh\nprintf '%s\\n' 'Xcode 不可用' >&2\nexit 7\n",
        0o755,
    )
    return environment


def invalid_epoch_environment(fixture: Fixture):
    environment = tool_environment(fixture)
    environment["SOURCE_DATE_EPOCH"] = "现在"
    return environment


def interrupted_environment(fixture: Fixture):
    environment = tool_environment(fixture)
    write(
        fixture.fake_bin / "xcodebuild",
        "#!/bin/sh\nkill -TERM \"$PPID\"\nsleep 1\nexit 7\n",
        0o755,
    )
    return environment


def negative_contracts(base: Fixture, work: Path) -> None:
    cases = (
        ("dirty-main", mutate_main_tracked, "不干净", None),
        ("untracked-main", mutate_main_untracked, "不干净", None),
        ("assume-unchanged", mutate_assume_unchanged, "index 标志", None),
        ("child-head-drift", mutate_child_head, None, None),
        ("child-dirty", mutate_child_dirty, "不干净", None),
        ("sparse-content", mutate_sparse_content, "sparse", None),
        ("sparse-mode", mutate_sparse_mode, "executable bit", None),
        ("partial-clone", mutate_partial_clone, "partial clone", None),
        ("missing-child", mutate_missing_child, None, None),
        ("symlink-child", mutate_symlink_child, None, None),
        ("nested-gitlink", mutate_nested_gitlink, "嵌套 gitlink", None),
        ("unsafe-link", mutate_unsafe_link, "符号链接", None),
        ("tool-failure", lambda _fixture: None, "Xcode", broken_tool_environment),
        (
            "invalid-epoch",
            lambda _fixture: None,
            "SOURCE_DATE_EPOCH",
            invalid_epoch_environment,
        ),
        (
            "interrupted",
            lambda _fixture: None,
            "SIGTERM",
            interrupted_environment,
        ),
    )
    for name, mutate, expected, environment in cases:
        expect_render_failure(
            base, work, name, mutate, expected, environment
        )

    fixture = clone_fixture(base, work / "output-exists")
    mutate_output_exists(fixture)
    sentinel = fixture.outputs / "failed" / "sentinel"
    before = sentinel.read_bytes()
    result = invoke(fixture, "render", fixture.outputs / "failed")
    assert_failure(result, "输出已存在", "已存在")
    if sentinel.read_bytes() != before:
        raise AssertionError("既有输出目录被修改")
    assert_no_stage(fixture)

    fixture = clone_fixture(base, work / "tampered")
    assets = fixture.outputs / "assets"
    assert_success(invoke(fixture, "render", assets), "篡改夹具生成")
    archive = assets / "project-source.tar"
    original_archive = archive.read_bytes()
    with archive.open("r+b") as stream:
        stream.seek(512)
        original = stream.read(1)
        stream.seek(512)
        stream.write(bytes([original[0] ^ 1]))
    assert_failure(
        invoke(fixture, "verify", assets, fake_tools=False),
        "篡改资产复核",
        "源码",
    )
    archive.write_bytes(original_archive)
    with tarfile.open(archive, "r:") as source:
        padded = next(
            member
            for member in source.getmembers()
            if member.isfile() and member.size % tarfile.BLOCKSIZE
        )
    with archive.open("r+b") as stream:
        stream.seek(padded.offset_data + padded.size)
        stream.write(b"X")
    assert_failure(
        invoke(fixture, "verify", assets, fake_tools=False),
        "正文 padding 篡改复核",
        "padding",
    )
    archive.write_bytes(original_archive)
    with archive.open("r+b") as stream:
        stream.seek(-1, os.SEEK_END)
        stream.write(b"X")
    assert_failure(
        invoke(fixture, "verify", assets, fake_tools=False),
        "结尾 padding 篡改复核",
        "结尾",
    )

    fixture = clone_fixture(base, work / "extra-asset")
    assets = fixture.outputs / "assets"
    assert_success(invoke(fixture, "render", assets), "额外资产夹具生成")
    write(assets / "extra.txt", "多余资产\n")
    assert_failure(
        invoke(fixture, "verify", assets, fake_tools=False),
        "额外资产复核",
        "精确包含",
    )


def main() -> int:
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-release-assets-test-"
    ) as temporary:
        work = Path(temporary)
        base = make_fixture(work / "base")
        positive_contract(base, work)
        negative_contracts(base, work)
    print("Apple 发行公共资产测试通过：3 组正例与 20 组负例")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

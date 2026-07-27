#!/usr/bin/env python3

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from io import BytesIO
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
from typing import Callable, Optional
import zipfile


sys.dont_write_bytecode = True
PROJECT_ROOT = Path(__file__).resolve().parent.parent
TOOL = PROJECT_ROOT / "tools" / "apple-release-manifest.py"
MANIFEST_FILENAME = "APPLE-RELEASE-MANIFEST.json"
TAG = "v1.2.3"
INDEX_FILENAME = "apple-release-assets.tsv"
RELEASE_PROFILES = (
    "profile\trole\ttarget\tscheme\tdebug_configuration"
    "\trelease_configuration\tkernel\trootfs\n"
    "core\tiphone-app\tiSH\tiSH\tDebug-ApplePleaseFixFB19282108"
    "\tRelease\tish\tfixed-alpine-seed\n"
    "core\twatch-app\tiSHWatch\tiSHWatch"
    "\tDebug-ApplePleaseFixFB19282108\tRelease\tish"
    "\tfixed-alpine-seed\n"
    "with-linux\tiphone-app\tiSH+Linux\tiSH+Linux\tDebugLinux"
    "\tReleaseLinux\tlinux\tonline-root-tar\n"
    "with-linux\twatch-app\tiSHWatch\tiSHWatch"
    "\tDebug-ApplePleaseFixFB19282108\tRelease\tish"
    "\tfixed-alpine-seed\n"
)
PROJECT_INPUTS = (
    "section\trole\tpath\tsource_url\tsize\tsha256\n"
    "source\tsource\t-\thttps://example.invalid/ish-multiarch\t-\t-\n"
)
DEPENDENCY_HEADER = (
    "component\tversion\tversion_source\tgitlink_path\tgitlink_commit"
    "\tsource_url\tdelivery_unit\tdelivery_kind\tdelivery_name"
    "\tinput_count\tinput_sha256\n"
)
NOTICE_PATHS = (
    "distribution/apple/project-license/PROJECT-LICENSES.txt",
    "third_party/alpine/3.24.1-aarch64/THIRD-PARTY-NOTICES.txt",
    "third_party/apple-host/APPLE-HOST-NOTICES.txt",
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
WITH_LINUX_ASSETS = tuple(
    sorted(
        (
            *COMMON_ASSETS,
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
    )
)
TOOLCHAIN_ROWS = (
    ("clang_version", "Apple clang version 17.0.0"),
    ("host_arch", "arm64"),
    ("iphoneos_sdk_build", "23A339"),
    ("ld_version", "ld64-1234.5"),
    ("meson_version", "1.9.1"),
    ("ninja_version", "1.13.1"),
    ("source_date_epoch", "1700000000"),
    ("watchos_sdk_build", "23R339"),
    ("xcode_build", "17A400"),
)


class TestFailure(Exception):
    pass


@dataclass(frozen=True)
class Fixture:
    container: Path
    root: Path
    assets: Path
    index: Path
    output: Path
    revision: str
    profile: str


def fail(message: str) -> None:
    raise TestFailure(message)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(content)


def command_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "LC_ALL": "C",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
    )
    return environment


def tool_environment() -> dict[str, str]:
    environment = command_environment()
    poison = PROJECT_ROOT / ".release-manifest-test-poison"
    environment["GIT_DIR"] = str(poison / "git-dir")
    environment["GIT_WORK_TREE"] = str(poison / "work-tree")
    environment["GIT_INDEX_FILE"] = str(poison / "index")
    return environment


def run_command(
    arguments: list[str],
    cwd: Path,
    *,
    input_text: Optional[str] = None,
) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        check=False,
        input=None if input_text is None else input_text.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=command_environment(),
    )
    output = (
        result.stdout.decode("utf-8", errors="replace")
        + result.stderr.decode("utf-8", errors="replace")
    )
    if result.returncode != 0:
        fail(f"辅助命令失败：{' '.join(arguments)}\n{output}")
    return result.stdout.decode("utf-8", errors="strict").strip()


def git(root: Path, *arguments: str) -> str:
    return run_command(["git", *arguments], root)


def initialize_repository(root: Path) -> None:
    root.mkdir(parents=True)
    git(root, "init", "-q")
    git(root, "config", "user.name", "发布清单测试")
    git(root, "config", "user.email", "release-test@example.invalid")
    git(root, "config", "commit.gpgsign", "false")
    git(root, "config", "tag.gpgSign", "false")


def create_child_repository(path: Path, name: str) -> str:
    initialize_repository(path)
    write_text(path / "README.txt", f"{name} 合成源码\n")
    git(path, "add", "README.txt")
    git(path, "commit", "-q", "-m", f"test: 初始化 {name}")
    return git(path, "rev-parse", "HEAD")


def dependency_row(
    component: str, gitlink_path: str, revision: str
) -> str:
    return (
        f"{component}\tsnapshot\tgitlink\t{gitlink_path}\t{revision}"
        f"\thttps://example.invalid/{component}\t{component}"
        f"\tstatic-library\tlib{component}.a\t1\t"
        f"{hashlib.sha256(component.encode()).hexdigest()}\n"
    )


def create_parent_repository(container: Path) -> tuple[Path, str]:
    root = container / "repo"
    initialize_repository(root)
    git(
        root,
        "remote",
        "add",
        "origin",
        "https://example.invalid/ish-multiarch",
    )
    revisions = {
        "libapps": create_child_repository(root / "deps/libapps", "libapps"),
        "libarchive": create_child_repository(
            root / "deps/libarchive", "libarchive"
        ),
        "linux": create_child_repository(root / "deps/linux", "linux"),
    }
    write_text(
        root / "distribution/apple/release-profiles.tsv", RELEASE_PROFILES
    )
    write_text(
        root / "distribution/apple/project-license/inputs.tsv",
        PROJECT_INPUTS,
    )
    write_text(
        root / ".gitmodules",
        '[submodule "deps/libapps"]\n'
        "\tpath = deps/libapps\n"
        "\turl = https://example.invalid/libapps\n"
        '[submodule "deps/libarchive"]\n'
        "\tpath = deps/libarchive\n"
        "\turl = https://example.invalid/libarchive\n"
        '[submodule "deps/linux"]\n'
        "\tpath = deps/linux\n"
        "\turl = https://example.invalid/linux\n",
    )
    dependency_rows = [
        dependency_row("libapps", "deps/libapps", revisions["libapps"]),
        dependency_row(
            "libarchive", "deps/libarchive", revisions["libarchive"]
        ),
        dependency_row("linux", "deps/linux", revisions["linux"]),
    ]
    write_text(
        root / "third_party/apple-host/dependencies.tsv",
        DEPENDENCY_HEADER + "".join(dependency_rows),
    )
    for index, relative in enumerate(NOTICE_PATHS, start=1):
        write_text(root / relative, f"合成发布声明 {index}\n")
    write_text(root / "README-fixture.txt", "合成发布仓库\n")
    git(root, "add", "--all")
    git(root, "commit", "-q", "-m", "test: 初始化发布清单夹具")
    revision = git(root, "rev-parse", "HEAD")
    git(root, "tag", "-a", TAG, "-m", "合成发布标签")
    if git(root, "status", "--porcelain"):
        fail("合成父仓库初始化后不是干净状态")
    return root, revision


def write_zip(path: Path, member: str, content: bytes) -> None:
    information = zipfile.ZipInfo(member, date_time=(1980, 1, 1, 0, 0, 0))
    information.create_system = 3
    information.external_attr = 0o100644 << 16
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.writestr(information, content)


def write_tar(path: Path, member: str, content: bytes) -> None:
    information = tarfile.TarInfo(member)
    information.mode = 0o644
    information.mtime = 0
    information.size = len(content)
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        archive.addfile(information, BytesIO(content))


def write_tar_with_relative_symlink(path: Path) -> None:
    content = b"safe relative link target\n"
    regular = tarfile.TarInfo("linux/include/target.h")
    regular.mode = 0o644
    regular.mtime = 0
    regular.size = len(content)
    link = tarfile.TarInfo("linux/include/link.h")
    link.mode = 0o777
    link.mtime = 0
    link.type = tarfile.SYMTYPE
    link.linkname = "target.h"
    hardlink = tarfile.TarInfo("linux/include/hardlink.h")
    hardlink.type = tarfile.LNKTYPE
    hardlink.linkname = "linux/include/target.h"
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        archive.addfile(regular, BytesIO(content))
        archive.addfile(link)
        archive.addfile(hardlink)


def write_tar_with_escaping_link(path: Path, link_type: bytes, link_target: str) -> None:
    content = b"archive content\n"
    regular = tarfile.TarInfo("source/README.txt")
    regular.mode = 0o644
    regular.mtime = 0
    regular.size = len(content)
    link = tarfile.TarInfo("source/escape")
    link.mode = 0o777
    link.mtime = 0
    link.type = link_type
    link.linkname = link_target
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        archive.addfile(regular, BytesIO(content))
        archive.addfile(link)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def write_checksum(path: Path, source: Path) -> None:
    write_text(path, f"{sha256(source)}  {source.name}\n")


def write_toolchain(path: Path, rows: tuple[tuple[str, str], ...]) -> None:
    write_text(path, "".join(f"{key}\t{value}\n" for key, value in rows))


def create_assets(container: Path, profile: str) -> tuple[Path, Path]:
    assets = container / "assets"
    assets.mkdir()
    records = COMMON_ASSETS if profile == "core" else WITH_LINUX_ASSETS
    for category, role, filename in records:
        path = assets / filename
        if category in {"product", "symbols"}:
            write_zip(path, f"{role}/payload.bin", role.encode("utf-8"))
        elif category == "source":
            write_tar(path, f"{role}/README.txt", role.encode("utf-8"))
    write_tar_with_relative_symlink(
        assets / "deps-linux-source.tar"
    )
    alpine_source = assets / (
        "alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar"
    )
    write_checksum(assets / "corresponding-source.sha256", alpine_source)
    if profile == "with-linux":
        online_source = assets / "online-rootfs-corresponding-source.tar"
        write_checksum(
            assets / "online-rootfs-corresponding-source.sha256",
            online_source,
        )
    write_toolchain(assets / "apple-toolchain.tsv", TOOLCHAIN_ROWS)
    for path in assets.iterdir():
        path.chmod(0o644)
    index = container / INDEX_FILENAME
    write_text(
        index,
        "".join(
            f"{category}\t{role}\t{filename}\n"
            for category, role, filename in records
        ),
    )
    expected_count = 11 if profile == "core" else 13
    if len(records) != expected_count:
        fail(f"{profile} 合成资产数量不是 {expected_count}")
    return assets, index


def create_fixture(container: Path, profile: str) -> Fixture:
    container.mkdir()
    root, revision = create_parent_repository(container)
    assets, index = create_assets(container, profile)
    output_parent = container / "output"
    output_parent.mkdir()
    return Fixture(
        container=container,
        root=root,
        assets=assets,
        index=index,
        output=output_parent / MANIFEST_FILENAME,
        revision=revision,
        profile=profile,
    )


def clone_fixture(base: Fixture, destination: Path) -> Fixture:
    shutil.copytree(base.container, destination)
    return Fixture(
        container=destination,
        root=destination / base.root.relative_to(base.container),
        assets=destination / base.assets.relative_to(base.container),
        index=destination / base.index.relative_to(base.container),
        output=destination / base.output.relative_to(base.container),
        revision=base.revision,
        profile=base.profile,
    )


def invoke_tool(
    arguments: list[str],
    *,
    expect_success: bool,
    expected_error: Optional[str] = None,
) -> str:
    command = [sys.executable, "-B", str(TOOL), *arguments]
    result = subprocess.run(
        command,
        cwd=PROJECT_ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=tool_environment(),
    )
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    output = stdout + stderr
    if expect_success:
        if result.returncode != 0:
            fail(f"工具命令意外失败：{' '.join(command)}\n{output}")
        if "Traceback" in output:
            fail(f"成功命令泄漏 Python traceback：\n{output}")
    else:
        if result.returncode == 0:
            fail(f"负例意外成功：{' '.join(command)}")
        if "错误：" not in output or "Traceback" in output:
            fail(f"负例没有返回简洁中文错误：\n{output}")
        if expected_error and expected_error not in output:
            fail(f"负例没有命中“{expected_error}”：\n{output}")
    return stdout


def render_arguments(
    fixture: Fixture,
    *,
    profile: Optional[str] = None,
    revision: Optional[str] = None,
    output: Optional[Path] = None,
) -> list[str]:
    return [
        "render",
        "--root",
        str(fixture.root),
        "--profile",
        profile or fixture.profile,
        "--revision",
        revision or fixture.revision,
        "--tag",
        TAG,
        "--assets",
        str(fixture.assets),
        "--index",
        str(fixture.index),
        "--output",
        str(output or fixture.output),
    ]


def verify_arguments(fixture: Fixture) -> list[str]:
    return [
        "verify",
        "--root",
        str(fixture.root),
        "--manifest",
        str(fixture.output),
        "--assets",
        str(fixture.assets),
    ]


def render(fixture: Fixture, **overrides: object) -> None:
    invoke_tool(
        render_arguments(fixture, **overrides),
        expect_success=True,
    )


def verify(fixture: Fixture) -> None:
    invoke_tool(verify_arguments(fixture), expect_success=True)


def assert_clean(root: Path, description: str) -> None:
    status = git(root, "status", "--porcelain")
    if status:
        fail(f"{description} 没有保持干净 Git 状态：\n{status}")


def test_positive(
    base: Fixture, cases: Path, profile: str
) -> None:
    fixture = clone_fixture(base, cases / f"positive-{profile}")
    render(fixture)
    verify(fixture)
    first = fixture.output.read_bytes()
    if not first.endswith(b"\n"):
        fail(f"{profile} manifest 没有规范 LF 结尾")
    try:
        manifest = json.loads(first)
    except json.JSONDecodeError as error:
        raise TestFailure(
            f"{profile} manifest 不是有效 JSON：{error}"
        ) from error
    if manifest.get("profile") != profile:
        fail(f"{profile} manifest 没有记录所选 profile")
    second_parent = fixture.container / "second-output"
    second_parent.mkdir()
    second_output = second_parent / MANIFEST_FILENAME
    render(fixture, output=second_output)
    if first != second_output.read_bytes():
        fail(f"{profile} 相同输入的两次 manifest 不逐字节一致")
    assert_clean(fixture.root, f"{profile} 正例")


def test_in_directory_manifest(base: Fixture, cases: Path) -> None:
    copied = clone_fixture(base, cases / "positive-in-assets")
    fixture = Fixture(
        container=copied.container,
        root=copied.root,
        assets=copied.assets,
        index=copied.index,
        output=copied.assets / MANIFEST_FILENAME,
        revision=copied.revision,
        profile=copied.profile,
    )
    render(fixture)
    verify(fixture)
    if not fixture.output.is_file():
        fail("资产目录内 manifest 正例没有生成目标文件")
    assert_clean(fixture.root, "资产目录内 manifest 正例")


def negative_render(
    base: Fixture,
    cases: Path,
    name: str,
    mutate: Callable[[Fixture], None],
    *,
    expected_error: Optional[str] = None,
    profile: Optional[str] = None,
    revision: Optional[str] = None,
) -> None:
    fixture = clone_fixture(base, cases / name)
    mutate(fixture)
    previous_output = (
        (fixture.output.read_bytes(), fixture.output.stat().st_mode)
        if fixture.output.exists()
        else None
    )
    invoke_tool(
        render_arguments(
            fixture,
            profile=profile,
            revision=revision,
        ),
        expect_success=False,
        expected_error=expected_error,
    )
    if previous_output is None and fixture.output.exists():
        fail(f"{name} 失败后遗留了 manifest 输出")
    if previous_output is not None and (
        fixture.output.read_bytes(),
        fixture.output.stat().st_mode,
    ) != previous_output:
        fail(f"{name} 失败后改变了既有 manifest 的内容或权限")
    temporary = list(fixture.output.parent.glob(f".{MANIFEST_FILENAME}.*.tmp"))
    if temporary:
        fail(f"{name} 失败后遗留了 manifest 临时文件")


def negative_verify(
    base: Fixture,
    cases: Path,
    name: str,
    mutate: Callable[[Fixture], None],
    *,
    expected_error: Optional[str] = None,
) -> None:
    fixture = clone_fixture(base, cases / name)
    render(fixture)
    mutate(fixture)
    invoke_tool(
        verify_arguments(fixture),
        expect_success=False,
        expected_error=expected_error,
    )


def mutate_dirty_tree(fixture: Fixture) -> None:
    write_text(fixture.root / "README-fixture.txt", "工作树已修改\n")


def mutate_lightweight_tag(fixture: Fixture) -> None:
    git(fixture.root, "tag", "-d", TAG)
    git(fixture.root, "tag", TAG, "HEAD")


def mutate_tag_peel(fixture: Fixture) -> None:
    tree = git(fixture.root, "rev-parse", "HEAD^{tree}")
    other = run_command(
        ["git", "commit-tree", tree, "-p", fixture.revision],
        fixture.root,
        input_text="另一合成提交\n",
    )
    git(fixture.root, "tag", "-d", TAG)
    git(fixture.root, "tag", "-a", TAG, other, "-m", "错误目标标签")


def mutate_gitlink_head(fixture: Fixture) -> None:
    child = fixture.root / "deps/libapps"
    write_text(child / "README.txt", "漂移后的合成源码\n")
    git(child, "add", "README.txt")
    git(child, "commit", "-q", "-m", "test: 制造 gitlink HEAD 漂移")
    git(fixture.root, "config", "diff.ignoreSubmodules", "all")
    assert_clean(fixture.root, "gitlink 漂移夹具")


def mutate_gitlink_child_index_flag(fixture: Fixture) -> None:
    child = fixture.root / "deps/libapps"
    git(child, "update-index", "--assume-unchanged", "README.txt")
    write_text(child / "README.txt", "被 child index flag 隐藏的漂移\n")
    if git(child, "status", "--porcelain"):
        fail("child index flag 夹具没有隐藏工作树漂移")


def mutate_missing_asset(fixture: Fixture) -> None:
    (fixture.assets / "iphone-app.ipa").unlink()

def mutate_extra_asset(fixture: Fixture) -> None:
    write_text(fixture.assets / "unexpected.txt", "多余资产\n")


def mutate_swapped_roles(fixture: Fixture) -> None:
    records = []
    for line in fixture.index.read_text(encoding="utf-8").splitlines():
        category, role, filename = line.split("\t")
        if category == "product" and role == "iphone-app":
            filename = "watch-app.ipa"
        elif category == "product" and role == "watch-app":
            filename = "iphone-app.ipa"
        records.append((category, role, filename))
    write_text(
        fixture.index,
        "".join(
            f"{category}\t{role}\t{filename}\n"
            for category, role, filename in sorted(records)
        ),
    )


def mutate_checksum(fixture: Fixture) -> None:
    source = (
        "alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar"
    )
    write_text(
        fixture.assets / "corresponding-source.sha256",
        f"{'0' * 64}  {source}\n",
    )


def mutate_notice(fixture: Fixture) -> None:
    document = json.loads(fixture.output.read_bytes())
    content = "声明正文已漂移\n"
    data = content.encode("utf-8")
    notice = document["notices"][1]
    notice["text"] = content
    notice["size"] = len(data)
    notice["sha256"] = hashlib.sha256(data).hexdigest()
    canonical = (
        json.dumps(
            document,
            ensure_ascii=False,
            sort_keys=True,
            indent=2,
            allow_nan=False,
        )
        + "\n"
    )
    write_text(fixture.output, canonical)


def mutate_asset_bytes(fixture: Fixture) -> None:
    write_zip(
        fixture.assets / "iphone-app.ipa",
        "iphone-app/payload.bin",
        b"changed asset content",
    )


def mutate_symlink(fixture: Fixture) -> None:
    asset = fixture.assets / "iphone-app.ipa"
    asset.unlink()
    target = fixture.container / "outside-asset.bin"
    target.write_bytes("外部文件".encode("utf-8"))
    asset.symlink_to(target)


def mutate_archive_traversal(fixture: Fixture) -> None:
    write_tar(
        fixture.assets / "project-source.tar",
        "../escape.txt",
        "不安全成员".encode("utf-8"),
    )


def mutate_archive_link_escape(fixture: Fixture) -> None:
    write_tar_with_escaping_link(
        fixture.assets / "deps-linux-source.tar",
        tarfile.SYMTYPE,
        "../../escape",
    )


def mutate_archive_hardlink_escape(fixture: Fixture) -> None:
    write_tar_with_escaping_link(
        fixture.assets / "deps-linux-source.tar",
        tarfile.LNKTYPE,
        "../escape",
    )


def mutate_zip_traversal(fixture: Fixture) -> None:
    write_zip(
        fixture.assets / "iphone-app.ipa",
        "../escape.txt",
        b"unsafe zip member",
    )


def mutate_asset_mode(fixture: Fixture) -> None:
    (fixture.assets / "iphone-app.ipa").chmod(0o600)


def mutate_existing_output(fixture: Fixture) -> None:
    write_text(fixture.output, "不得覆盖\n")
    fixture.output.chmod(0o600)


def mutate_manifest_noncanonical(fixture: Fixture) -> None:
    with fixture.output.open("ab") as output:
        output.write(b" ")


def mutate_manifest_stale_repo(fixture: Fixture) -> None:
    git(
        fixture.root,
        "commit",
        "--allow-empty",
        "-q",
        "-m",
        "test: 推进仓库 revision",
    )
    assert_clean(fixture.root, "revision 漂移夹具")


def rewrite_toolchain(
    fixture: Fixture, rows: tuple[tuple[str, str], ...]
) -> None:
    write_toolchain(fixture.assets / "apple-toolchain.tsv", rows)


def mutate_toolchain_missing(fixture: Fixture) -> None:
    rewrite_toolchain(fixture, TOOLCHAIN_ROWS[1:])


def mutate_toolchain_unsorted(fixture: Fixture) -> None:
    rows = list(TOOLCHAIN_ROWS)
    rows[0], rows[1] = rows[1], rows[0]
    rewrite_toolchain(fixture, tuple(rows))


def mutate_toolchain_epoch(fixture: Fixture) -> None:
    rows = tuple(
        (key, "现在" if key == "source_date_epoch" else value)
        for key, value in TOOLCHAIN_ROWS
    )
    rewrite_toolchain(fixture, rows)


def different_revision(revision: str) -> str:
    replacement = "0" if revision[0] != "0" else "1"
    return replacement + revision[1:]


def run_negative_cases(
    core: Fixture, with_linux: Fixture, cases: Path
) -> None:
    negative_render(
        core,
        cases,
        "dirty-tree",
        mutate_dirty_tree,
        expected_error="干净",
    )
    negative_render(
        core,
        cases,
        "head-revision-mismatch",
        lambda _fixture: None,
        revision=different_revision(core.revision),
        expected_error="revision",
    )
    negative_render(
        core,
        cases,
        "lightweight-tag",
        mutate_lightweight_tag,
        expected_error="annotated tag",
    )
    negative_render(
        core,
        cases,
        "tag-peel-mismatch",
        mutate_tag_peel,
        expected_error="revision",
    )
    negative_render(
        core,
        cases,
        "gitlink-drift",
        mutate_gitlink_head,
        expected_error="gitlink",
    )
    negative_render(
        core,
        cases,
        "gitlink-child-index-flag",
        mutate_gitlink_child_index_flag,
        expected_error="Git 索引含",
    )
    negative_render(
        with_linux,
        cases,
        "wrong-profile-assets",
        lambda _fixture: None,
        profile="core",
        expected_error="core profile",
    )
    negative_render(
        core,
        cases,
        "missing-asset",
        mutate_missing_asset,
        expected_error="缺失",
    )
    negative_render(
        core,
        cases,
        "extra-asset",
        mutate_extra_asset,
        expected_error="多余",
    )
    negative_render(
        core,
        cases,
        "role-filename-swap",
        mutate_swapped_roles,
        expected_error="文件名",
    )
    negative_render(
        core,
        cases,
        "checksum-drift",
        mutate_checksum,
        expected_error="SHA-256",
    )
    negative_verify(
        core,
        cases,
        "notice-drift",
        mutate_notice,
        expected_error="不一致",
    )
    negative_verify(
        core,
        cases,
        "asset-byte-drift",
        mutate_asset_bytes,
        expected_error="不一致",
    )
    negative_render(
        core,
        cases,
        "symlink-asset",
        mutate_symlink,
        expected_error="符号链接",
    )
    negative_render(
        core,
        cases,
        "archive-traversal",
        mutate_archive_traversal,
        expected_error="逃逸",
    )
    negative_render(
        core,
        cases,
        "archive-link-escape",
        mutate_archive_link_escape,
        expected_error="链接",
    )
    negative_render(
        core,
        cases,
        "archive-hardlink-escape",
        mutate_archive_hardlink_escape,
        expected_error="链接",
    )
    negative_render(
        core,
        cases,
        "zip-traversal",
        mutate_zip_traversal,
        expected_error="逃逸",
    )
    negative_render(
        core,
        cases,
        "asset-mode",
        mutate_asset_mode,
        expected_error="0644",
    )
    negative_render(
        core,
        cases,
        "output-exists",
        mutate_existing_output,
        expected_error="存在",
    )
    negative_verify(
        core,
        cases,
        "manifest-noncanonical",
        mutate_manifest_noncanonical,
        expected_error="规范",
    )
    negative_verify(
        core,
        cases,
        "manifest-repo-stale",
        mutate_manifest_stale_repo,
        expected_error="revision",
    )
    negative_render(
        core,
        cases,
        "toolchain-missing",
        mutate_toolchain_missing,
        expected_error="键集合",
    )
    negative_render(
        core,
        cases,
        "toolchain-unsorted",
        mutate_toolchain_unsorted,
        expected_error="排序",
    )
    negative_render(
        core,
        cases,
        "toolchain-invalid-epoch",
        mutate_toolchain_epoch,
        expected_error="source_date_epoch",
    )


def main() -> int:
    if not TOOL.is_file():
        fail(f"找不到待测工具：{TOOL}")
    with tempfile.TemporaryDirectory(
        prefix="ish-apple-release-manifest-test-"
    ) as temporary:
        temporary_root = Path(temporary)
        bases = temporary_root / "bases"
        cases = temporary_root / "cases"
        bases.mkdir()
        cases.mkdir()
        core = create_fixture(bases / "core", "core")
        with_linux = create_fixture(
            bases / "with-linux", "with-linux"
        )
        test_positive(core, cases, "core")
        test_positive(with_linux, cases, "with-linux")
        test_in_directory_manifest(core, cases)
        run_negative_cases(core, with_linux, cases)
    print(
        "Apple Release manifest 合成回归通过："
        "2 个 profile、资产目录内 manifest 正例与 25 个负例。"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TestFailure as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1) from None

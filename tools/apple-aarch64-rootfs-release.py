#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import errno
import os
from pathlib import Path
import secrets
import signal
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = ROOT / "third_party" / "alpine" / "3.24.1-aarch64"
SOURCE_TOOL = ROOT / "tools" / "apple-aarch64-rootfs-sources.py"
BUNDLE_FILENAME = (
    "alpine-minirootfs-3.24.1-aarch64-corresponding-source.tar"
)
CHECKSUM_FILENAME = "corresponding-source.sha256"
DEFAULT_PACKAGES = SOURCE_ROOT / "packages.tsv"
DEFAULT_STATIC_LINK_SOURCES = SOURCE_ROOT / "static-link-sources.tsv"
DEFAULT_ORIGINS = SOURCE_ROOT / "origins.tsv"
DEFAULT_ASSETS = SOURCE_ROOT / "source-assets.tsv"
DEFAULT_BINARY_REFERENCE = SOURCE_ROOT / "binary-reference.tsv"
DEFAULT_README = SOURCE_ROOT / "SOURCE-BUNDLE.md"
DEFAULT_CHECKSUM = SOURCE_ROOT / CHECKSUM_FILENAME
RELEASE_FILENAMES = frozenset({BUNDLE_FILENAME, CHECKSUM_FILENAME})
FIXTURE_MODE_ENV = "ISH_AARCH64_RELEASE_TEST_MODE"
RENAME_EXCL = 0x00000004
RENAME_NOREPLACE = 1
TERMINATION_SIGNALS = frozenset(
    {signal.SIGHUP, signal.SIGINT, signal.SIGTERM}
)


class ReleaseStageError(Exception):
    pass


def source_arguments(args: argparse.Namespace) -> list[str]:
    result = [
        "--packages",
        str(args.packages),
        "--static-link-sources",
        str(args.static_link_sources),
        "--origins",
        str(args.origins),
        "--assets",
        str(args.assets),
        "--binary-reference",
        str(args.binary_reference),
        "--readme",
        str(args.readme),
    ]
    if args.allow_local_sources:
        result.append("--allow-local-sources")
    return result


def terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.communicate(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.communicate()


def run_source_tool(
    phase: str,
    arguments: list[str],
    inherited_descriptors: tuple[int, ...] = (),
    working_directory_descriptor: int | None = None,
) -> None:
    parent_mask = signal.pthread_sigmask(
        signal.SIG_BLOCK, TERMINATION_SIGNALS
    )

    def prepare_child() -> None:
        if working_directory_descriptor is not None:
            os.fchdir(working_directory_descriptor)
        signal.pthread_sigmask(signal.SIG_SETMASK, parent_mask)

    try:
        process = subprocess.Popen(
            [sys.executable, str(SOURCE_TOOL), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=inherited_descriptors,
            preexec_fn=prepare_child,
            start_new_session=True,
        )
    except BaseException:
        signal.pthread_sigmask(signal.SIG_SETMASK, parent_mask)
        raise
    try:
        signal.pthread_sigmask(signal.SIG_SETMASK, parent_mask)
        _stdout, stderr = process.communicate()
    except BaseException:
        previous_mask = signal.pthread_sigmask(
            signal.SIG_BLOCK, TERMINATION_SIGNALS
        )
        try:
            terminate_process_group(process)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        raise
    if process.returncode == 0:
        return
    message = stderr.decode("utf-8", errors="replace").strip()
    if message.startswith("错误："):
        message = message.removeprefix("错误：")
    if not message:
        message = f"子进程退出码 {process.returncode}"
    raise ReleaseStageError(f"{phase}失败：{message}")


def read_regular_file(path: Path, description: str) -> bytes:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise ReleaseStageError("当前平台不支持拒绝符号链接的文件读取。")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | nofollow
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ReleaseStageError(
            f"{description}不存在、不可读或是符号链接：{path}"
        ) from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise ReleaseStageError(f"{description}必须是普通文件：{path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def write_regular_file(
    directory_descriptor: int, filename: str, data: bytes
) -> None:
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(
        filename, flags, 0o644, dir_fd=directory_descriptor
    )
    try:
        os.fchmod(descriptor, 0o644)
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            view = view[written:]
    finally:
        os.close(descriptor)


def create_private_directory(parent_descriptor: int) -> tuple[str, int]:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        raise ReleaseStageError("当前平台不支持安全的私有发布候选。")
    flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | nofollow
        | directory
    )
    for _attempt in range(128):
        name = f".release-stage.{secrets.token_hex(8)}"
        try:
            os.mkdir(name, 0o700, dir_fd=parent_descriptor)
        except FileExistsError:
            continue
        try:
            descriptor = os.open(
                name, flags, dir_fd=parent_descriptor
            )
        except BaseException:
            os.rmdir(name, dir_fd=parent_descriptor)
            raise
        return name, descriptor
    raise ReleaseStageError("无法创建唯一的发布暂存目录。")


def open_physical_directory(
    path: Path, description: str
) -> tuple[Path, int, tuple[int, int]]:
    absolute = Path(os.path.abspath(os.fspath(path)))
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise ReleaseStageError(f"{description}不存在。") from error
    if absolute != resolved:
        raise ReleaseStageError(f"{description}路径不能包含符号链接。")

    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        raise ReleaseStageError("当前平台不支持安全的目录相对发布。")
    try:
        descriptor = os.open(
            absolute,
            os.O_RDONLY
            | getattr(os, "O_CLOEXEC", 0)
            | nofollow
            | directory,
        )
    except OSError as error:
        raise ReleaseStageError(
            f"{description}不可读、不是实体目录或路径含符号链接。"
        ) from error
    metadata = os.fstat(descriptor)
    identity = (metadata.st_dev, metadata.st_ino)
    try:
        ensure_directory_identity(absolute, descriptor, identity, description)
    except BaseException:
        os.close(descriptor)
        raise
    return absolute, descriptor, identity


def ensure_directory_identity(
    path: Path,
    descriptor: int,
    identity: tuple[int, int],
    description: str,
) -> None:
    descriptor_metadata = os.fstat(descriptor)
    try:
        path_metadata = os.stat(path, follow_symlinks=False)
    except OSError as error:
        raise ReleaseStageError(f"{description}在暂存期间失效。") from error
    if (
        not stat.S_ISDIR(path_metadata.st_mode)
        or (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
        != identity
        or (path_metadata.st_dev, path_metadata.st_ino) != identity
    ):
        raise ReleaseStageError(f"{description}在暂存期间被替换。")


def ensure_output_absent(
    parent_descriptor: int, output_name: str
) -> None:
    try:
        os.stat(
            output_name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
    except FileNotFoundError:
        return
    raise ReleaseStageError("发布输出目录已经存在，拒绝覆盖。")


def rename_exclusive(
    source_descriptor: int,
    source_name: str,
    destination_descriptor: int,
    output_name: str,
) -> None:
    library = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        try:
            rename = library.renameatx_np
        except AttributeError as error:
            raise ReleaseStageError(
                "当前 macOS 不支持排他目录 rename。"
            ) from error
        rename.argtypes = (
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        )
        rename.restype = ctypes.c_int
        flags = RENAME_EXCL
    elif sys.platform.startswith("linux"):
        try:
            rename = library.renameat2
        except AttributeError as error:
            raise ReleaseStageError(
                "当前 Linux libc 不支持排他目录 rename。"
            ) from error
        rename.argtypes = (
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        )
        rename.restype = ctypes.c_int
        flags = RENAME_NOREPLACE
    else:
        raise ReleaseStageError("当前平台没有受支持的排他目录 rename。")

    result = rename(
        source_descriptor,
        os.fsencode(source_name),
        destination_descriptor,
        os.fsencode(output_name),
        flags,
    )
    if result == 0:
        return
    error_number = ctypes.get_errno()
    if error_number in (errno.EEXIST, errno.ENOTEMPTY):
        raise ReleaseStageError("发布输出目录已经存在，拒绝覆盖。")
    raise OSError(error_number, os.strerror(error_number))


def validate_fixture_mode(args: argparse.Namespace) -> None:
    custom_inputs = (
        args.checksum != DEFAULT_CHECKSUM
        or args.packages != DEFAULT_PACKAGES
        or args.static_link_sources != DEFAULT_STATIC_LINK_SOURCES
        or args.origins != DEFAULT_ORIGINS
        or args.assets != DEFAULT_ASSETS
        or args.binary_reference != DEFAULT_BINARY_REFERENCE
        or args.readme != DEFAULT_README
        or args.allow_local_sources
    )
    if custom_inputs and os.environ.get(FIXTURE_MODE_ENV) != "fixture":
        raise ReleaseStageError("自定义源码锁只能用于显式 fixture 模式。")


def create_candidate(
    candidate_descriptor: int,
    cache: Path,
    checksum_bytes: bytes,
    checksum: Path,
    common: list[str],
) -> None:
    run_source_tool(
        "生成对应源码包",
        ["bundle", str(cache), BUNDLE_FILENAME, *common],
        (candidate_descriptor,),
        candidate_descriptor,
    )
    write_regular_file(
        candidate_descriptor, CHECKSUM_FILENAME, checksum_bytes
    )
    run_source_tool(
        "验证对应源码包",
        [
            "verify",
            BUNDLE_FILENAME,
            "--checksum",
            CHECKSUM_FILENAME,
            *common,
        ],
        (candidate_descriptor,),
        candidate_descriptor,
    )
    if read_regular_file(checksum, "对应源码包 SHA-256 清单") != (
        checksum_bytes
    ):
        raise ReleaseStageError("对应源码包 SHA-256 清单在暂存期间发生变化。")


def validate_candidate(directory_descriptor: int) -> None:
    entries = os.listdir(directory_descriptor)
    if set(entries) != RELEASE_FILENAMES:
        raise ReleaseStageError("发布候选目录必须恰好包含源码包与校验清单。")
    for entry in entries:
        metadata = os.stat(
            entry,
            dir_fd=directory_descriptor,
            follow_symlinks=False,
        )
        if not stat.S_ISREG(metadata.st_mode):
            raise ReleaseStageError("发布候选只能包含普通文件。")
        if stat.S_IMODE(metadata.st_mode) != 0o644:
            raise ReleaseStageError("发布候选文件权限必须固定为 0644。")


def open_candidate_file(
    directory_descriptor: int, filename: str
) -> int:
    flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(
        filename, flags, dir_fd=directory_descriptor
    )
    if not stat.S_ISREG(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise ReleaseStageError("发布候选只能包含普通文件。")
    return descriptor


def compare_files(
    first_directory: int, second_directory: int, filename: str
) -> bool:
    first = open_candidate_file(first_directory, filename)
    try:
        second = open_candidate_file(second_directory, filename)
    except BaseException:
        os.close(first)
        raise
    try:
        if os.fstat(first).st_size != os.fstat(second).st_size:
            return False
        while True:
            left_chunk = os.read(first, 1024 * 1024)
            right_chunk = os.read(second, 1024 * 1024)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True
    finally:
        os.close(first)
        os.close(second)


def verify_reproducible(first: int, second: int) -> None:
    for filename in sorted(RELEASE_FILENAMES):
        if not compare_files(first, second, filename):
            raise ReleaseStageError(
                f"两次发布候选不一致：{filename}"
            )


def remove_candidate(
    parent_descriptor: int,
    candidate_name: str,
    candidate_descriptor: int,
) -> None:
    previous_mask = signal.pthread_sigmask(
        signal.SIG_BLOCK, TERMINATION_SIGNALS
    )
    try:
        try:
            for entry in os.listdir(candidate_descriptor):
                metadata = os.stat(
                    entry,
                    dir_fd=candidate_descriptor,
                    follow_symlinks=False,
                )
                if stat.S_ISDIR(metadata.st_mode):
                    raise ReleaseStageError(
                        "发布候选意外包含子目录，拒绝递归删除。"
                    )
                os.unlink(entry, dir_fd=candidate_descriptor)
            os.rmdir(candidate_name, dir_fd=parent_descriptor)
        finally:
            os.close(candidate_descriptor)
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)


def stage_release_impl(args: argparse.Namespace) -> None:
    validate_fixture_mode(args)
    output_text = os.fspath(args.output)
    if any(character in output_text for character in "*?["):
        raise ReleaseStageError("发布输出路径不能包含 glob 元字符。")
    output = Path(output_text).absolute()
    if output.name in ("", ".", ".."):
        raise ReleaseStageError("发布输出路径必须包含普通目录名。")

    parent: Path | None = None
    parent_descriptor = -1
    cache_descriptor = -1
    staging_name: str | None = None
    staging_descriptor = -1
    candidates: list[tuple[str, int]] = []
    try:
        parent, parent_descriptor, parent_identity = open_physical_directory(
            output.parent, "发布输出父目录"
        )
        ensure_output_absent(parent_descriptor, output.name)
        cache, cache_descriptor, cache_identity = open_physical_directory(
            args.cache, "源码缓存"
        )
        prospective_output = parent / output.name
        if os.path.commonpath((prospective_output, cache)) == str(cache):
            raise ReleaseStageError("发布输出目录不能位于源码缓存内部。")
        common = source_arguments(args)
        run_source_tool(
            "校验对应源码锁",
            [
                "check-locks",
                "--checksum",
                str(args.checksum),
                *common,
            ],
        )
        checksum_bytes = read_regular_file(
            args.checksum, "对应源码包 SHA-256 清单"
        )
        staging_name, staging_descriptor = create_private_directory(
            parent_descriptor
        )

        for _index in range(2):
            ensure_directory_identity(
                parent,
                parent_descriptor,
                parent_identity,
                "发布输出父目录",
            )
            ensure_directory_identity(
                cache,
                cache_descriptor,
                cache_identity,
                "源码缓存",
            )
            candidate = create_private_directory(staging_descriptor)
            candidates.append(candidate)
            create_candidate(
                candidate[1],
                cache,
                checksum_bytes,
                args.checksum,
                common,
            )

        first_name, first_descriptor = candidates[0]
        second_name, second_descriptor = candidates[1]
        validate_candidate(first_descriptor)
        validate_candidate(second_descriptor)
        verify_reproducible(first_descriptor, second_descriptor)
        remove_candidate(
            staging_descriptor, second_name, second_descriptor
        )
        candidates.pop()
        ensure_directory_identity(
            parent,
            parent_descriptor,
            parent_identity,
            "发布输出父目录",
        )
        ensure_directory_identity(
            cache,
            cache_descriptor,
            cache_identity,
            "源码缓存",
        )
        os.close(cache_descriptor)
        cache_descriptor = -1
        os.fchmod(first_descriptor, 0o755)
        previous_mask = signal.pthread_sigmask(
            signal.SIG_BLOCK, TERMINATION_SIGNALS
        )
        try:
            rename_exclusive(
                staging_descriptor,
                first_name,
                parent_descriptor,
                output.name,
            )
        except BaseException:
            signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
            raise

        candidates.clear()
        try:
            os.rmdir(staging_name, dir_fd=parent_descriptor)
        except OSError:
            pass
        # rename 是唯一提交点；成功后直接退出，避免清理或关闭错误制造模糊终态。
        os._exit(0)
    finally:
        cleanup_mask = signal.pthread_sigmask(
            signal.SIG_BLOCK, TERMINATION_SIGNALS
        )
        try:
            active_error = sys.exc_info()[0] is not None
            cleanup_error: BaseException | None = None
            for candidate_name, candidate_descriptor in candidates:
                try:
                    remove_candidate(
                        staging_descriptor,
                        candidate_name,
                        candidate_descriptor,
                    )
                except FileNotFoundError:
                    pass
                except BaseException as error:
                    cleanup_error = cleanup_error or error
            if staging_descriptor >= 0:
                try:
                    os.close(staging_descriptor)
                    staging_descriptor = -1
                except OSError as error:
                    cleanup_error = cleanup_error or error
            if staging_name is not None and parent_descriptor >= 0:
                try:
                    os.rmdir(staging_name, dir_fd=parent_descriptor)
                except FileNotFoundError:
                    pass
                except OSError as error:
                    cleanup_error = cleanup_error or error
            if cache_descriptor >= 0:
                try:
                    os.close(cache_descriptor)
                except OSError as error:
                    cleanup_error = cleanup_error or error
            if parent_descriptor >= 0:
                try:
                    os.close(parent_descriptor)
                except OSError as error:
                    cleanup_error = cleanup_error or error
            if cleanup_error is not None and not active_error:
                raise ReleaseStageError(
                    f"清理发布暂存资源失败：{cleanup_error}"
                ) from cleanup_error
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, cleanup_mask)


def interrupt_stage(signum: int, _frame) -> None:
    signal_name = signal.Signals(signum).name
    raise ReleaseStageError(
        f"收到 {signal_name}，已中止对应源码发布暂存。"
    )


def stage_release(args: argparse.Namespace) -> None:
    previous_handlers = {
        current_signal: signal.signal(current_signal, interrupt_stage)
        for current_signal in TERMINATION_SIGNALS
    }
    try:
        stage_release_impl(args)
    finally:
        for current_signal, handler in previous_handlers.items():
            signal.signal(current_signal, handler)


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="离线重建、验证并暂存 Alpine AArch64 对应源码发布资产"
    )
    parser.add_argument("cache", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--checksum",
        type=Path,
        default=DEFAULT_CHECKSUM,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--packages",
        type=Path,
        default=DEFAULT_PACKAGES,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--static-link-sources",
        type=Path,
        default=DEFAULT_STATIC_LINK_SOURCES,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--origins",
        type=Path,
        default=DEFAULT_ORIGINS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--assets",
        type=Path,
        default=DEFAULT_ASSETS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--binary-reference",
        type=Path,
        default=DEFAULT_BINARY_REFERENCE,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--readme",
        type=Path,
        default=DEFAULT_README,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--allow-local-sources",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    return parser


def main() -> int:
    args = create_parser().parse_args()
    try:
        stage_release(args)
    except (ReleaseStageError, OSError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

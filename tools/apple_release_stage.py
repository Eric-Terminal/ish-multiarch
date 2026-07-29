from __future__ import annotations

import ctypes
import errno
import os
from pathlib import Path
import signal
import stat
import sys


TERMINATION_SIGNALS = frozenset(
    {signal.SIGHUP, signal.SIGINT, signal.SIGTERM}
)


class StageError(ValueError):
    pass


def fail(message: str) -> None:
    raise StageError(message)


def open_directory_descriptor(path: Path):
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        fail("当前平台不支持安全的目录相对发布")
    descriptor = os.open(
        path,
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | nofollow
        | directory,
    )
    metadata = os.fstat(descriptor)
    return descriptor, (metadata.st_dev, metadata.st_ino)


def open_child_directory_descriptor(
    parent_descriptor: int, name: str
):
    nofollow = getattr(os, "O_NOFOLLOW", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or directory is None:
        fail("当前平台不支持安全的目录相对发布")
    descriptor = os.open(
        name,
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | nofollow
        | directory,
        dir_fd=parent_descriptor,
    )
    metadata = os.fstat(descriptor)
    return descriptor, (metadata.st_dev, metadata.st_ino)


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
        fail(f"{description}在暂存期间失效：{error}")
    if (
        not stat.S_ISDIR(path_metadata.st_mode)
        or (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
        != identity
        or (path_metadata.st_dev, path_metadata.st_ino) != identity
    ):
        fail(f"{description}在暂存期间被替换")


def ensure_child_directory_identity(
    parent_descriptor: int,
    name: str,
    descriptor: int,
    identity: tuple[int, int],
    description: str,
) -> None:
    descriptor_metadata = os.fstat(descriptor)
    try:
        path_metadata = os.stat(
            name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
    except OSError as error:
        fail(f"{description}在暂存期间失效：{error}")
    if (
        not stat.S_ISDIR(path_metadata.st_mode)
        or (descriptor_metadata.st_dev, descriptor_metadata.st_ino)
        != identity
        or (path_metadata.st_dev, path_metadata.st_ino) != identity
    ):
        fail(f"{description}在暂存期间被替换")


def publish_no_replace(
    source_descriptor: int,
    source_name: str,
    destination_descriptor: int,
    output_name: str,
    private_stage_name: str,
    success_message: str,
) -> None:
    # 目录 rename 是五项资产唯一可见的提交点，且不能覆盖并发出现的候选目录。
    success_bytes = success_message.encode("utf-8", errors="replace")
    library = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        try:
            rename = library.renameatx_np
        except AttributeError:
            fail("当前 macOS 不支持排他目录 rename")
        flags = 0x00000004
    elif sys.platform.startswith("linux"):
        try:
            rename = library.renameat2
        except AttributeError:
            fail("当前 Linux libc 缺少 renameat2，无法无覆盖发布目录")
        flags = 0x00000001
    else:
        fail("当前平台不支持无覆盖原子发布目录")
    rename.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    rename.restype = ctypes.c_int
    previous_mask = signal.pthread_sigmask(
        signal.SIG_BLOCK, TERMINATION_SIGNALS
    )
    result = rename(
        source_descriptor,
        os.fsencode(source_name),
        destination_descriptor,
        os.fsencode(output_name),
        flags,
    )
    if result != 0:
        error_number = ctypes.get_errno()
        signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        if error_number in {errno.EEXIST, errno.ENOTEMPTY}:
            fail("输出目录已存在，拒绝覆盖")
        raise OSError(error_number, os.strerror(error_number), output_name)
    try:
        try:
            os.rmdir(private_stage_name, dir_fd=destination_descriptor)
        except OSError:
            pass
        os.write(sys.stdout.fileno(), success_bytes)
    except BaseException:
        pass
    finally:
        # rename 成功即为唯一提交点；直接成功退出，避免收尾错误制造模糊终态。
        os._exit(0)


def cleanup_stage(
    parent_descriptor: int,
    private_stage_name: str | None,
    private_stage_descriptor: int,
    candidate_name: str,
    candidate_descriptor: int,
    asset_filenames: tuple[str, ...],
) -> None:
    if candidate_descriptor >= 0:
        try:
            for filename in asset_filenames:
                try:
                    os.unlink(filename, dir_fd=candidate_descriptor)
                except FileNotFoundError:
                    pass
        finally:
            os.close(candidate_descriptor)
    if private_stage_descriptor >= 0:
        try:
            try:
                os.rmdir(candidate_name, dir_fd=private_stage_descriptor)
            except FileNotFoundError:
                pass
        finally:
            os.close(private_stage_descriptor)
    if private_stage_name is not None:
        try:
            os.rmdir(private_stage_name, dir_fd=parent_descriptor)
        except FileNotFoundError:
            pass

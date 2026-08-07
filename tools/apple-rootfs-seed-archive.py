#!/usr/bin/env python3

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile
import tempfile


FORMAT = "ish-rootfs-seed-archive-v1"
REQUIRED_TOP = {
    "data": "directory",
    "meta.db": "file",
    "rootfs-hardlinks.tsv": "file",
    "rootfs-manifest.txt": "file",
}


class ArchiveError(Exception):
    pass


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="把 iSH fakefs seed 打包为可校验的 gzip/USTAR 归档。"
    )
    parser.add_argument("seed", type=Path, help="目录型 fakefs seed")
    parser.add_argument("output", type=Path, help="输出 .tar.gz 文件")
    parser.add_argument(
        "--metadata",
        required=True,
        type=Path,
        help="输出供 App 固定校验参数的 JSON 清单",
    )
    return parser.parse_args()


def require_directory(path, description):
    try:
        metadata = path.lstat()
    except OSError as error:
        raise ArchiveError(f"{description}不可访问：{error}") from error
    if not stat.S_ISDIR(metadata.st_mode) or path.is_symlink():
        raise ArchiveError(f"{description}必须是普通目录且不能是符号链接。")
    return path.resolve(strict=True)


def require_output(path, description):
    if path.name in {"", ".", ".."}:
        raise ArchiveError(f"{description}文件名无效。")
    parent = require_directory(path.parent, f"{description}父目录")
    return parent / path.name


def read_regular(path, description, limit):
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode) or path.is_symlink():
        raise ArchiveError(f"{description}必须是普通文件。")
    if metadata.st_size < 0 or metadata.st_size > limit:
        raise ArchiveError(f"{description}尺寸超出格式边界。")
    return path.read_bytes()


def validate_seed(seed):
    actual = {}
    for entry in os.scandir(seed):
        if entry.is_symlink():
            raise ArchiveError(f"seed 顶层不能包含符号链接：{entry.name}")
        if entry.is_dir(follow_symlinks=False):
            kind = "directory"
        elif entry.is_file(follow_symlinks=False):
            kind = "file"
        else:
            raise ArchiveError(f"seed 顶层包含不支持的宿主对象：{entry.name}")
        actual[entry.name] = kind
    if actual != REQUIRED_TOP:
        raise ArchiveError(
            "seed 顶层必须且只能包含 data、meta.db、"
            "rootfs-hardlinks.tsv 与 rootfs-manifest.txt。"
        )

    manifest_path = seed / "rootfs-manifest.txt"
    manifest = read_regular(manifest_path, "RootFS seed manifest", 64 * 1024)
    try:
        lines = manifest.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise ArchiveError("RootFS seed manifest 不是 UTF-8。") from error
    values = {}
    for line in lines:
        if "=" not in line:
            raise ArchiveError("RootFS seed manifest 行格式无效。")
        key, value = line.split("=", 1)
        if key in values:
            raise ArchiveError(f"RootFS seed manifest 重复字段：{key}")
        values[key] = value
    if values.get("format") != "ish-fakefs-v3":
        raise ArchiveError("RootFS seed manifest 格式不是 ish-fakefs-v3。")
    if values.get("guest_arch") != "aarch64":
        raise ArchiveError("RootFS seed 不是 AArch64 guest。")
    archive_sha = values.get("archive_sha256", "")
    if len(archive_sha) != 64 or any(
        byte not in "0123456789abcdef" for byte in archive_sha
    ):
        raise ArchiveError("RootFS seed 的上游归档 SHA-256 无效。")
    required_values = {
        "alpine_version": "Alpine 版本",
        "packager": "seed 打包器",
        "source_kind": "来源类型",
        "source_url": "来源 URL",
    }
    for key, description in required_values.items():
        if not values.get(key):
            raise ArchiveError(f"RootFS seed manifest 缺少{description}。")
    return hashlib.sha256(manifest).hexdigest(), values


def collect_entries(seed):
    entries = []

    def visit(directory, relative):
        with os.scandir(directory) as iterator:
            children = sorted(iterator, key=lambda item: os.fsencode(item.name))
        for child in children:
            child_relative = relative / child.name
            name = child_relative.as_posix()
            if child.is_symlink():
                raise ArchiveError(f"seed 不能包含宿主符号链接：{name}")
            metadata = child.stat(follow_symlinks=False)
            if stat.S_ISDIR(metadata.st_mode):
                entries.append((name, Path(child.path), metadata, True))
                visit(Path(child.path), child_relative)
            elif stat.S_ISREG(metadata.st_mode):
                entries.append((name, Path(child.path), metadata, False))
            else:
                raise ArchiveError(f"seed 包含不支持的宿主对象：{name}")

    visit(seed, PurePosixPath())
    return entries


def tar_info(name, metadata, directory):
    info = tarfile.TarInfo(name=name)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    if directory:
        info.type = tarfile.DIRTYPE
        info.mode = 0o700
        info.size = 0
    else:
        info.type = tarfile.REGTYPE
        info.mode = 0o600
        info.size = metadata.st_size
    # 在写入前主动触发 USTAR 路径边界检查，禁止隐式 PAX/GNU 扩展。
    try:
        info.tobuf(format=tarfile.USTAR_FORMAT, encoding="utf-8", errors="strict")
    except (UnicodeError, ValueError) as error:
        raise ArchiveError(f"seed 路径无法表示为 USTAR：{name}") from error
    return info


def open_stable_regular(path, expected):
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ArchiveError(f"无法读取 seed 文件 {path}：{error}") from error
    actual = os.fstat(descriptor)
    if (
        not stat.S_ISREG(actual.st_mode)
        or actual.st_dev != expected.st_dev
        or actual.st_ino != expected.st_ino
        or actual.st_size != expected.st_size
        or actual.st_mtime_ns != expected.st_mtime_ns
    ):
        os.close(descriptor)
        raise ArchiveError(f"打包期间 seed 文件发生变化：{path}")
    return os.fdopen(descriptor, "rb", closefd=True)


def verify_opened_regular(source, path, expected):
    actual = os.fstat(source.fileno())
    if (
        actual.st_dev != expected.st_dev
        or actual.st_ino != expected.st_ino
        or actual.st_size != expected.st_size
        or actual.st_mtime_ns != expected.st_mtime_ns
    ):
        raise ArchiveError(f"打包期间 seed 文件发生变化：{path}")


def write_archive(entries, output):
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as raw:
            with gzip.GzipFile(
                filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0
            ) as compressed:
                with tarfile.open(
                    mode="w",
                    fileobj=compressed,
                    format=tarfile.USTAR_FORMAT,
                    encoding="utf-8",
                    errors="strict",
                ) as archive:
                    for name, path, metadata, directory in entries:
                        info = tar_info(name, metadata, directory)
                        if directory:
                            archive.addfile(info)
                        else:
                            with open_stable_regular(path, metadata) as source:
                                archive.addfile(info, source)
                                verify_opened_regular(source, path, metadata)
            raw.flush()
            os.fsync(raw.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, output)
        directory = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except Exception:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def sha256_file(path):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(64 * 1024):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def write_metadata(document, output):
    body = (json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb", closefd=True) as target:
            target.write(body)
            target.flush()
            os.fsync(target.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, output)
        directory = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except Exception:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def main():
    arguments = parse_arguments()
    try:
        seed = require_directory(arguments.seed, "RootFS seed")
        output = require_output(arguments.output, "RootFS 归档输出")
        metadata_output = require_output(arguments.metadata, "RootFS 归档清单输出")
        if output == metadata_output:
            raise ArchiveError("归档和清单不能写入同一个路径。")
        seed_manifest_sha256, seed_manifest = validate_seed(seed)
        entries = collect_entries(seed)
        if not entries:
            raise ArchiveError("RootFS seed 不能为空。")
        uncompressed_bytes = sum(
            item.st_size for _, _, item, directory in entries if not directory
        )
        if uncompressed_bytes <= 0:
            raise ArchiveError("RootFS seed 没有可安装的文件内容。")

        write_archive(entries, output)
        archive_sha256, archive_bytes = sha256_file(output)
        write_metadata(
            {
                "archiveBytes": archive_bytes,
                "archiveFile": output.name,
                "archiveSHA256": archive_sha256,
                "alpineVersion": seed_manifest["alpine_version"],
                "compression": "gzip",
                "entryCount": len(entries),
                "format": FORMAT,
                "guestArchitecture": "aarch64",
                "seedManifestSHA256": seed_manifest_sha256,
                "seedPackager": seed_manifest["packager"],
                "sourceKind": seed_manifest["source_kind"],
                "sourceURL": seed_manifest["source_url"],
                "uncompressedBytes": uncompressed_bytes,
                "upstreamArchiveSHA256": seed_manifest["archive_sha256"],
            },
            metadata_output,
        )
    except (ArchiveError, OSError, tarfile.TarError) as error:
        raise SystemExit(f"错误：{error}") from error


if __name__ == "__main__":
    main()

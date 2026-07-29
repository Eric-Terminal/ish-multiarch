from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import tarfile
import tempfile


class ArchiveError(ValueError):
    pass


def fail(message: str) -> None:
    raise ArchiveError(message)


def decode_utf8(data: bytes, description: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"{description}不是 UTF-8：{error}")


def validate_link_target(source: str, target: str) -> None:
    if (
        not target
        or target.startswith("/")
        or "\\" in target
        or "\0" in target
        or "\r" in target
        or "\n" in target
    ):
        fail(f"源码符号链接目标不安全：{source} -> {target}")
    resolved = list(PurePosixPath(source).parent.parts)
    for part in PurePosixPath(target).parts:
        if part in {"", "."}:
            continue
        if part == "..":
            if not resolved:
                fail(f"源码符号链接逃逸归档根：{source} -> {target}")
            resolved.pop()
        else:
            resolved.append(part)


@dataclass(frozen=True)
class TreeEntry:
    path: str
    path_bytes: bytes
    mode: str
    kind: str
    oid: str


@dataclass(frozen=True)
class ArchiveSpec:
    filename: str
    prefix: str
    root: Path
    revision: str
    entries: tuple[TreeEntry, ...]


def git_blob_digest(data: bytes) -> str:
    digest = hashlib.sha1()
    digest.update(f"blob {len(data)}\0".encode("ascii"))
    digest.update(data)
    return digest.hexdigest()


class BlobStream:
    def __init__(self, owner: "GitBlobBatch", size: int):
        self.owner = owner
        self.remaining = size
        self.finished = False

    def read(self, size=-1):
        if self.finished or self.remaining == 0:
            return b""
        if size is None or size < 0:
            size = self.remaining
        size = min(size, self.remaining)
        data = self.owner.stdout.read(size)
        if not data:
            fail("Git blob 在声明大小结束前意外 EOF")
        self.remaining -= len(data)
        return data

    def finish(self) -> None:
        if self.remaining:
            fail("Git blob 没有被完整写入归档")
        if self.owner.stdout.read(1) != b"\n":
            fail("Git cat-file batch 分隔符漂移")
        self.finished = True
        self.owner.active = None


class GitBlobBatch:
    def __init__(self, root: Path, environment: dict[str, str]):
        self.root = root
        self.stderr = tempfile.TemporaryFile()
        # 单个长驻 batch 进程避免为 Linux 的数万条目逐文件启动 Git。
        self.process = subprocess.Popen(
            ["git", "-C", str(root), "cat-file", "--batch"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            env=environment,
        )
        if self.process.stdin is None or self.process.stdout is None:
            self.process.kill()
            fail("无法建立 Git cat-file 流")
        self.stdin = self.process.stdin
        self.stdout = self.process.stdout
        self.active: BlobStream | None = None

    def open_blob(self, oid: str) -> BlobStream:
        if self.active is not None:
            fail("前一个 Git blob 尚未消费完")
        try:
            self.stdin.write(oid.encode("ascii") + b"\n")
            self.stdin.flush()
        except BrokenPipeError:
            self._raise_process_error()
        header = self.stdout.readline()
        fields = header.rstrip(b"\n").split()
        if len(fields) != 3:
            self._raise_process_error("Git cat-file 返回非法 blob 头")
        returned_oid, kind, size_bytes = fields
        try:
            size = int(size_bytes)
        except ValueError:
            fail("Git cat-file 返回非法 blob 大小")
        if (
            returned_oid.decode("ascii", errors="ignore") != oid
            or kind != b"blob"
            or size < 0
        ):
            fail("Git cat-file 返回的对象身份或类型漂移")
        stream = BlobStream(self, size)
        self.active = stream
        return stream

    def read_blob(self, oid: str) -> bytes:
        stream = self.open_blob(oid)
        data = stream.read()
        stream.finish()
        return data

    def _raise_process_error(self, fallback="Git cat-file 提前退出"):
        self.process.poll()
        self.stderr.seek(0)
        detail = decode_utf8(
            self.stderr.read(4096), "Git cat-file 错误输出"
        ).strip()
        fail(f"{fallback}：{detail or '未知错误'}")

    def close(self, failed=False) -> None:
        try:
            if failed:
                if self.process.poll() is None:
                    self.process.terminate()
            else:
                if self.active is not None:
                    fail("Git cat-file 关闭时仍有未消费的 blob")
                self.stdin.close()
            return_code = self.process.wait(timeout=10)
            if not failed and return_code != 0:
                self._raise_process_error()
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            if not failed:
                fail("Git cat-file 退出超时")
        finally:
            self.stdin.close()
            self.stdout.close()
            self.stderr.close()

    def __enter__(self):
        return self

    def __exit__(self, exception_type, _exception, _traceback):
        self.close(failed=exception_type is not None)
        return False


def canonical_info(name: str, mode: int, kind: bytes) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.mode = mode
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    info.type = kind
    info.size = 0
    return info


def write_archive(
    path: Path,
    spec: ArchiveSpec,
    git_environment: dict[str, str],
) -> None:
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
    raw = os.fdopen(descriptor, "wb", buffering=0, closefd=False)
    try:
        with GitBlobBatch(spec.root, git_environment) as blobs:
            with tarfile.open(
                fileobj=raw, mode="w|", format=tarfile.USTAR_FORMAT
            ) as archive:
                archive.addfile(
                    canonical_info(spec.prefix, 0o755, tarfile.DIRTYPE)
                )
                for entry in spec.entries:
                    name = f"{spec.prefix}/{entry.path}"
                    if entry.mode == "160000":
                        archive.addfile(
                            canonical_info(name, 0o755, tarfile.DIRTYPE)
                        )
                    elif entry.mode == "120000":
                        target_bytes = blobs.read_blob(entry.oid)
                        target = decode_utf8(
                            target_bytes, f"源码符号链接 {entry.path}"
                        )
                        validate_link_target(entry.path, target)
                        info = canonical_info(
                            name, 0o777, tarfile.SYMTYPE
                        )
                        info.linkname = target
                        archive.addfile(info)
                    else:
                        stream = blobs.open_blob(entry.oid)
                        info = canonical_info(
                            name,
                            0o755 if entry.mode == "100755" else 0o644,
                            tarfile.REGTYPE,
                        )
                        info.size = stream.remaining
                        archive.addfile(info, stream)
                        stream.finish()
    except ArchiveError:
        raise
    except (tarfile.TarError, UnicodeError, ValueError) as error:
        fail(f"无法写入规范 USTAR：{spec.filename}（{error}）")
    else:
        os.fchmod(descriptor, 0o644)
        os.fsync(descriptor)
    finally:
        raw.close()
        os.close(descriptor)


def read_exact(source, size: int, description: str) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = source.read(remaining)
        if not chunk:
            fail(f"{description}提前结束")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_member_header(source, description: str):
    header = read_exact(source, tarfile.BLOCKSIZE, description)
    try:
        member = tarfile.TarInfo.frombuf(
            header, encoding="utf-8", errors="strict"
        )
    except (tarfile.HeaderError, UnicodeError, ValueError) as error:
        fail(f"{description}不是规范 USTAR header：{error}")
    return header, member


def canonical_header(info: tarfile.TarInfo) -> bytes:
    try:
        return info.tobuf(
            format=tarfile.USTAR_FORMAT,
            encoding="utf-8",
            errors="strict",
        )
    except (ValueError, UnicodeError) as error:
        fail(f"无法编码规范 USTAR header：{info.name}（{error}）")


def hash_member_content(source, member: tarfile.TarInfo) -> str:
    digest = hashlib.sha1()
    digest.update(f"blob {member.size}\0".encode("ascii"))
    remaining = member.size
    while remaining:
        chunk = source.read(min(1024 * 1024, remaining))
        if not chunk:
            fail(f"源码 tar 文件提前结束：{member.name}")
        digest.update(chunk)
        remaining -= len(chunk)
    padding_size = (-member.size) % tarfile.BLOCKSIZE
    padding = read_exact(source, padding_size, f"{member.name} 的 tar padding")
    if any(padding):
        fail(f"源码 tar 文件 padding 不是全零：{member.name}")
    return digest.hexdigest()


def expected_member(
    spec: ArchiveSpec,
    entry: TreeEntry | None,
    actual: tarfile.TarInfo,
) -> tarfile.TarInfo:
    if entry is None:
        return canonical_info(spec.prefix, 0o755, tarfile.DIRTYPE)
    name = f"{spec.prefix}/{entry.path}"
    if entry.mode == "160000":
        return canonical_info(name, 0o755, tarfile.DIRTYPE)
    if entry.mode == "120000":
        validate_link_target(entry.path, actual.linkname)
        try:
            link_bytes = actual.linkname.encode("utf-8")
        except UnicodeEncodeError as error:
            fail(f"源码符号链接目标不是 UTF-8：{entry.path}（{error}）")
        if git_blob_digest(link_bytes) != entry.oid:
            fail(f"源码符号链接内容漂移：{entry.path}")
        info = canonical_info(name, 0o777, tarfile.SYMTYPE)
        info.linkname = actual.linkname
        return info
    info = canonical_info(
        name,
        0o755 if entry.mode == "100755" else 0o644,
        tarfile.REGTYPE,
    )
    info.size = actual.size
    return info


def verify_archive(path: Path, spec: ArchiveSpec) -> None:
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
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        fail("当前平台缺少 O_NOFOLLOW，无法安全校验发行资产")
    descriptor = os.open(
        path,
        os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | nofollow,
    )
    opened_metadata = os.fstat(descriptor)
    if (
        (opened_metadata.st_dev, opened_metadata.st_ino)
        != (metadata.st_dev, metadata.st_ino)
        or not stat.S_ISREG(opened_metadata.st_mode)
        or stat.S_IMODE(opened_metadata.st_mode) != 0o644
    ):
        os.close(descriptor)
        fail(f"公共资产在打开期间被替换：{path.name}")
    metadata = opened_metadata
    raw = os.fdopen(descriptor, "rb", buffering=0, closefd=False)
    offset = 0
    try:
        for entry in (None, *spec.entries):
            header, actual = read_member_header(
                raw, f"{path.name} 的源码成员 header"
            )
            expected = expected_member(spec, entry, actual)
            if header != canonical_header(expected):
                fail(f"源码 tar header 或成员顺序漂移：{actual.name}")
            offset += tarfile.BLOCKSIZE
            if entry is not None and entry.mode in {"100644", "100755"}:
                if actual.size > metadata.st_size - offset:
                    fail(f"源码 tar 文件大小越过归档边界：{entry.path}")
                if hash_member_content(raw, actual) != entry.oid:
                    fail(f"源码 tar 文件内容漂移：{entry.path}")
                offset += actual.size + (-actual.size) % tarfile.BLOCKSIZE
        expected_size = (
            (
                offset
                + 2 * tarfile.BLOCKSIZE
                + tarfile.RECORDSIZE
                - 1
            )
            // tarfile.RECORDSIZE
        ) * tarfile.RECORDSIZE
        if metadata.st_size != expected_size:
            fail(f"源码 tar 长度或结尾填充漂移：{path.name}")
        trailer = read_exact(
            raw, expected_size - offset, f"{path.name} 的 tar 结尾"
        )
        if any(trailer) or raw.read(1):
            fail(f"源码 tar 结尾不是规范的全零记录：{path.name}")
    finally:
        raw.close()
        os.close(descriptor)

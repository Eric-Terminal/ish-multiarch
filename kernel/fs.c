#include "debug.h"
#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/dev.h"

static struct fd *at_fd(fd_t f) {
    if (f == AT_FDCWD_)
        return AT_PWD;
    return f_get(f);
}

static void apply_umask(mode_t_ *mode) {
    struct fs_info *fs = current->fs;
    lock(&fs->lock);
    *mode &= ~fs->umask;
    unlock(&fs->lock);
}

int access_check_identity(const struct fs_access_identity *identity,
        const struct statbuf *stat, int check) {
    if (identity->uid == 0) return 0;
    if (check == 0) return 0;
    // 将请求权限移动到文件所有者或所属组对应的位段。
    if (identity->uid == stat->uid) {
        check <<= 6;
    } else if (identity->gid == stat->gid) {
        check <<= 3;
    }
    mode_t_ requested = (mode_t_) check;
    if ((stat->mode & requested) != requested)
        return _EACCES;
    return 0;
}

int access_check_task(struct task *task, struct statbuf *stat, int check) {
    struct task_credentials credentials;
    task_credentials_snapshot(task, &credentials);
    const struct fs_access_identity identity = {
        .uid = credentials.euid,
        .gid = credentials.egid,
    };
    return access_check_identity(&identity, stat, check);
}

int access_check(struct statbuf *stat, int check) {
    return access_check_task(current, stat, check);
}

// TODO ENAMETOOLONG

#define AT_EACCESS_ 0x200
dword_t sys_access(addr_t path_addr, dword_t mode) {
    return sys_faccessat(AT_FDCWD_, path_addr, mode, 0);
}
dword_t sys_faccessat(fd_t at_f, addr_t path_addr, mode_t_ mode, dword_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    STRACE("faccessat(%d, \"%s\", 0x%x, %d)", at_f, path, mode, flags);

    struct task_credentials credentials;
    task_credentials_snapshot(current, &credentials);
    const struct fs_access_identity identity = {
        .uid = flags & AT_EACCESS_ ? credentials.euid : credentials.uid,
        .gid = flags & AT_EACCESS_ ? credentials.egid : credentials.gid,
    };
    return generic_accessat_task(current, at, path, mode, &identity);
}

fd_t sys_openat(fd_t at_f, addr_t path_addr, dword_t flags, mode_t_ mode) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("openat(%d, \"%s\", 0x%x, 0x%x)", at_f, path, flags, mode);
    return file_openat_task(current, at_f, path, flags, mode);
}

fd_t sys_open(addr_t path_addr, dword_t flags, mode_t_ mode) {
    return sys_openat(AT_FDCWD_, path_addr, flags, mode);
}

dword_t sys_readlinkat(fd_t at_f, addr_t path_addr, addr_t buf_addr, dword_t bufsize) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("readlinkat(%d, \"%s\", %#x, %#x)", at_f, path, buf_addr, bufsize);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    char buf[bufsize];
    ssize_t size = generic_readlinkat(at, path, buf, bufsize);
    if (size >= 0) {
        STRACE(" \"%.*s\"", size, buf);
        if (user_write(buf_addr, buf, size))
            return _EFAULT;
    }
    return size;
}

dword_t sys_readlink(addr_t path_addr, addr_t buf_addr, dword_t bufsize) {
    return sys_readlinkat(AT_FDCWD_, path_addr, buf_addr, bufsize);
}

dword_t sys_linkat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr) {
    char src[MAX_PATH];
    if (user_read_string(src_addr, src, sizeof(src)))
        return _EFAULT;
    char dst[MAX_PATH];
    if (user_read_string(dst_addr, dst, sizeof(dst)))
        return _EFAULT;
    STRACE("linkat(%d, \"%s\", %d, \"%s\")", src_at_f, src, dst_at_f, dst);
    struct fd *src_at = at_fd(src_at_f);
    if (src_at == NULL)
        return _EBADF;
    struct fd *dst_at = at_fd(dst_at_f);
    if (dst_at == NULL)
        return _EBADF;
    return generic_linkat(src_at, src, dst_at, dst);
}

dword_t sys_link(addr_t src_addr, addr_t dst_addr) {
    return sys_linkat(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr);
}

dword_t sys_unlinkat(fd_t at_f, addr_t path_addr, int_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("unlinkat(%d, \"%s\", %d)", at_f, path, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    if (flags & AT_REMOVEDIR_)
        return generic_rmdirat(at, path);
    else
        return generic_unlinkat(at, path);
}

dword_t sys_unlink(addr_t path_addr) {
    return sys_unlinkat(AT_FDCWD_, path_addr, 0);
}

dword_t sys_renameat2(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr, int_t flags) {
    if (flags != 0)
        return _EINVAL;
    char src[MAX_PATH];
    if (user_read_string(src_addr, src, sizeof(src)))
        return _EFAULT;
    char dst[MAX_PATH];
    if (user_read_string(dst_addr, dst, sizeof(dst)))
        return _EFAULT;
    STRACE("renameat(%d, \"%s\", %d, \"%s\")", src_at_f, src, dst_at_f, dst);
    struct fd *src_at = at_fd(src_at_f);
    if (src_at == NULL)
        return _EBADF;
    struct fd *dst_at = at_fd(dst_at_f);
    if (dst_at == NULL)
        return _EBADF;
    return generic_renameat(src_at, src, dst_at, dst);
}

dword_t sys_renameat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr) {
    return sys_renameat2(src_at_f, src_addr, dst_at_f, dst_addr, 0);
}

dword_t sys_rename(addr_t src_addr, addr_t dst_addr) {
    return sys_renameat2(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr, 0);
}

dword_t sys_symlinkat(addr_t target_addr, fd_t at_f, addr_t link_addr) {
    char target[MAX_PATH];
    if (user_read_string(target_addr, target, sizeof(target)))
        return _EFAULT;
    char link[MAX_PATH];
    if (user_read_string(link_addr, link, sizeof(link)))
        return _EFAULT;
    STRACE("symlinkat(\"%s\", %d, \"%s\")", target, at_f, link);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    return generic_symlinkat(target, at, link);
}

dword_t sys_symlink(addr_t target_addr, addr_t link_addr) {
    return sys_symlinkat(target_addr, AT_FDCWD_, link_addr);
}

dword_t sys_mknodat(fd_t at_f, addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("mknodat(%d, \"%s\", %#x, %#x)", at_f, path, mode, dev);
    apply_umask(&mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    return generic_mknodat(at, path, mode, dev);
}

dword_t sys_mknod(addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    return sys_mknodat(AT_FDCWD_, path_addr, mode, dev);
}

dword_t sys_read(fd_t fd_no, addr_t buf_addr, dword_t size) {
    STRACE("read(%d, 0x%x, %d)", fd_no, buf_addr, size);
    char *buf = (char *) malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    int_t res = file_read_task(current, fd_no, buf, size);
    if (res >= 0) {
        size_t print_size = res;
        if (print_size > 100) print_size = 100;
        STRACE(" \"%.*s\"", print_size, buf);
        if (user_write(buf_addr, buf, res))
            res = _EFAULT;
    }
    free(buf);
    return res;
}

dword_t sys_write(fd_t fd_no, addr_t buf_addr, dword_t size) {
    // FIXME this is a DOS vector, should ideally use vectorized I/O
    char *buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    dword_t res = _EFAULT;
    if (user_read(buf_addr, buf, size))
        goto out;

    size_t print_size = size;
    if (print_size > 100) print_size = 100;
    STRACE("write(%d, \"%.*s\", %d)", fd_no, print_size, buf, size);

    res = file_write_task(current, fd_no, buf, size);
out:
    free(buf);
    return res;
}

// The vector operations work by flattening the vector into a malloc buffer.
// This at least isn't much worse than what it was before, which copied each
// element of the vector into a malloc buffer. The perfect solution would be to
// construct a vector with an entry for each page of the buffer. I haven't done
// that yet because it's more work and the efficiency gain from that is dwarfed
// by the inefficiency of the emulator.

static struct iovec_ *read_iovec(addr_t iovec_addr, unsigned iovec_count) {
    dword_t iovec_size = sizeof(struct iovec_) * iovec_count;
    struct iovec_ *iovec = malloc(iovec_size);
    if (iovec == NULL)
        return ERR_PTR(_ENOMEM);
    if (user_read(iovec_addr, iovec, iovec_size)) {
        free(iovec);
        return ERR_PTR(_EFAULT);
    }
    return iovec;
}

static ssize_t iovec_size(struct iovec_ *iovec, unsigned iovec_count) {
    size_t size = 0;
    for (unsigned i = 0; i < iovec_count; i++)
        size += iovec[i].len;
    return size;
}

dword_t sys_readv(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    STRACE("readv(%d, %#x, %d)", fd_no, iovec_addr, iovec_count);
    struct iovec_ *iovec = read_iovec(iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    size_t io_size = iovec_size(iovec, iovec_count);
    char *buf = malloc(io_size);
    if (buf == NULL) {
        free(iovec);
        return _ENOMEM;
    }
    ssize_t res = file_read_task(current, fd_no, buf, io_size);
    if (res < 0)
        goto error;

    size_t print_size = res;
    if (print_size > 100) print_size = 100;
    STRACE(" \"%.*s\"", print_size, buf);

    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        print_size = iovec[i].len;
        if (print_size > 100) print_size = 100;
        STRACE(" {\"%.*s\", %u}", print_size, buf + offset, iovec[i].len);

        if (user_write(iovec[i].base, buf + offset, iovec[i].len)) {
            res = _EFAULT;
            goto error;
        }
        offset += iovec[i].len;
    }

error:
    free(buf);
    free(iovec);
    return res;
}

dword_t sys_writev(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    STRACE("writev(%d, %#x, %d)", fd_no, iovec_addr, iovec_count);
    struct iovec_ *iovec = read_iovec(iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    size_t io_size = iovec_size(iovec, iovec_count);
    char *buf = malloc(io_size);
    if (buf == NULL) {
        free(iovec);
        return _ENOMEM;
    }

    ssize_t res = 0;
    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (user_read(iovec[i].base, buf + offset, iovec[i].len)) {
            res = _EFAULT;
            goto error;
        }

        size_t print_size = iovec[i].len;
        if (print_size > 100) print_size = 100;
        STRACE(" {\"%.*s\", %u}", print_size, buf + offset, iovec[i].len);
        offset += iovec[i].len;
    }
    res = file_write_task(current, fd_no, buf, io_size);

error:
    free(buf);
    free(iovec);
    return res;
}

dword_t sys__llseek(fd_t f, dword_t off_high, dword_t off_low, addr_t res_addr, dword_t whence) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!fd->ops->lseek)
        return _ESPIPE;
    lock(&fd->lock);
    off_t_ off = ((qword_t) off_high << 32) | off_low;
    STRACE("llseek(%d, %lu, %#x, %d)", f, off, res_addr, whence);
    off_t_ res = fd->ops->lseek(fd, off, whence);
    STRACE(" -> %lu", res);
    unlock(&fd->lock);
    if (res < 0)
        return res;
    if (user_put(res_addr, res))
        return _EFAULT;
    return 0;
}

dword_t sys_lseek(fd_t f, dword_t off, dword_t whence) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!fd->ops->lseek)
        return _ESPIPE;
    lock(&fd->lock);
    off_t res = fd->ops->lseek(fd, off, whence);
    unlock(&fd->lock);
    if ((dword_t) res != res)
        return _EOVERFLOW;
    return res;
}

dword_t sys_pread(fd_t f, addr_t buf_addr, dword_t size, off_t_ off) {
    STRACE("pread(%d, 0x%x, %d, %d)", f, buf_addr, size, off);
    struct fd *fd = f_get_task_retain(current, f);
    if (fd == NULL)
        return _EBADF;
    size_t allocation_size = size == 0 ? 1 : (size_t) size;
    char *buf = malloc(allocation_size);
    if (buf == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }
    ssize_t res = file_pread_fd(fd, buf, size, off);
    if (res >= 0) {
        size_t print_size = (size_t) res;
        if (print_size > 99)
            print_size = 99;
        STRACE(" \"%.*s\"", (int) print_size, buf);
        if (user_write(buf_addr, buf, res))
            res = _EFAULT;
    }
    free(buf);
    fd_close(fd);
    return res;
}

dword_t sys_pwrite(fd_t f, addr_t buf_addr, dword_t size, off_t_ off) {
    STRACE("pwrite(%d, 0x%x, %d, %d)", f, buf_addr, size, off);
    struct fd *fd = f_get_task_retain(current, f);
    if (fd == NULL)
        return _EBADF;
    size_t allocation_size = size == 0 ? 1 : (size_t) size;
    char *buf = malloc(allocation_size);
    if (buf == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }
    if (user_read(buf_addr, buf, size)) {
        free(buf);
        fd_close(fd);
        return _EFAULT;
    }
    ssize_t res = file_pwrite_fd(fd, buf, size, off);
    free(buf);
    fd_close(fd);
    return res;
}

static int fd_ioctl(struct fd *fd, dword_t cmd, dword_t arg) {
    ssize_t size = -1;
    if (fd->ops->ioctl_size)
        size = fd->ops->ioctl_size(cmd);
    if (size < 0)
        return _ENOTTY;
    if (size == 0)
        return fd->ops->ioctl(fd, cmd, (void *) (long) arg);

    // praying that this won't break
    char buf[size];
    if (user_read(arg, buf, size))
        return _EFAULT;
    int res = fd->ops->ioctl(fd, cmd, buf);
    if (res < 0)
        return res;
    if (user_write(arg, buf, size))
        return _EFAULT;
    return res;
}

static int set_nonblock(struct fd *fd, addr_t nb_addr) {
    dword_t nonblock;
    if (user_get(nb_addr, nonblock))
        return _EFAULT;
    int flags = fd_getflags(fd);
    if (nonblock)
        flags |= O_NONBLOCK_;
    else
        flags &= ~O_NONBLOCK_;
    return fd_setflags(fd, flags);
}

dword_t sys_ioctl(fd_t f, dword_t cmd, dword_t arg) {
    STRACE("ioctl(%d, 0x%x, 0x%x)", f, cmd, arg);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;

    switch (cmd) {
        case FIONBIO_:
            return set_nonblock(fd, arg);
        case FIOCLEX_:
            bit_set(f, current->files->cloexec);
            return 0;
        case FIONCLEX_:
            bit_clear(f, current->files->cloexec);
            return 0;
    }
    return fd_ioctl(fd, cmd, arg);
}

dword_t sys_getcwd(addr_t buf_addr, dword_t size) {
    STRACE("getcwd(%#x, %#x)", buf_addr, size);
    char pwd[MAX_PATH + 1];
    ssize_t result = fs_getcwd_task(current, pwd, size);
    if (result < 0)
        return result;
    STRACE(" \"%.*s\"", (int) result, pwd);
    if (user_write(buf_addr, pwd, (size_t) result))
        return _EFAULT;
    return result;
}

static struct fd *open_dir(const char *path) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, true);
    if (err < 0)
        return ERR_PTR(err);
    if (!(stat.mode & S_IFDIR))
        return ERR_PTR(_ENOTDIR);

    return generic_open(path, O_RDONLY_, 0);
}

void fs_chdir(struct fs_info *fs, struct fd *fd) {
    lock(&fs->lock);
    fd_close(fs->pwd);
    fs->pwd = fd;
    unlock(&fs->lock);
}

dword_t sys_chdir(addr_t path_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("chdir(\"%s\")", path);

    struct fd *dir = open_dir(path);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    fs_chdir(current->fs, dir);
    return 0;
}

dword_t sys_fchdir(fd_t f) {
    STRACE("fchdir(%d)", f);
    return file_fchdir_task(current, f);
}

dword_t sys_chroot(addr_t path_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("chroot(\"%s\")", path);

    struct fd *dir = open_dir(path);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    lock(&current->fs->lock);
    fd_close(current->fs->root);
    current->fs->root = dir;
    unlock(&current->fs->lock);
    return 0;
}

dword_t sys_umask(dword_t mask) {
    STRACE("umask(0%o)", mask);
    struct fs_info *fs = current->fs;
    lock(&fs->lock);
    mode_t_ old_umask = fs->umask;
    fs->umask = ((mode_t_) mask) & 0777;
    unlock(&fs->lock);
    return old_umask;
}

static int mount_statfs(struct mount *mount, struct statfsbuf *stat) {
    int err = 0;
    if (mount->fs->statfs)
        err = mount->fs->statfs(mount, stat);
    if (stat->type == 0)
        stat->type = mount->fs->magic;
    return err;
}

static int_t statfs_mount(struct mount *mount, addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct statfs_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

static int_t statfs64_mount(struct mount *mount, addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct statfs64_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

dword_t sys_statfs(addr_t path_addr, addr_t buf_addr) {
    char path_raw[MAX_PATH];
    if (user_read_string(path_addr, path_raw, sizeof(path_raw)))
        return _EFAULT;
    STRACE("statfs(\"%s\", %#x)", path_raw, buf_addr);
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    err = statfs_mount(mount, buf_addr);
    mount_release(mount);
    return err;
}

dword_t sys_statfs64(addr_t path_addr, dword_t buf_size, addr_t buf_addr) {
    char path_raw[MAX_PATH];
    if (user_read_string(path_addr, path_raw, sizeof(path_raw)))
        return _EFAULT;
    STRACE("statfs64(\"%s\", %d, %#x)", path_raw, buf_size, buf_addr);
    if (buf_size != sizeof(struct statfs64_))
        return _EINVAL;
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    err = statfs64_mount(mount, buf_addr);
    mount_release(mount);
    return err;
}

dword_t sys_fstatfs(fd_t f, addr_t buf_addr) {
    return statfs_mount(f_get(f)->mount, buf_addr);
}

dword_t sys_fstatfs64(fd_t f, addr_t buf_addr) {
    return statfs64_mount(f_get(f)->mount, buf_addr);
}

dword_t sys_flock(fd_t f, dword_t operation) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    // TODO: POSIX doesn't allow flock to fail in this way. The check is here
    // because a segfault is worse.
    if (fd->mount->fs->flock == NULL)
        return _EBADF;
    return fd->mount->fs->flock(fd, operation);
}

static dword_t sys_utime_common(fd_t at_f, addr_t path_addr, struct timespec atime, struct timespec mtime, dword_t flags) {
    char path[MAX_PATH];
    if (path_addr != 0)
        if (user_read_string(path_addr, path, sizeof(path)))
            return _EFAULT;
    STRACE("utimensat(%d, %s, {{%d, %d}, {%d, %d}}, %d)", at_f, path,
            atime.tv_sec, atime.tv_nsec, mtime.tv_sec, mtime.tv_nsec, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;

    bool follow_links = flags & AT_SYMLINK_NOFOLLOW_ ? false : true;
    return generic_utime(at, path_addr != 0 ? path : ".", atime, mtime, follow_links);
}

dword_t sys_utimensat(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct timespec_ times[2];
        if (user_get(times_addr, times))
            return _EFAULT;
        atime = convert_timespec(times[0]);
        mtime = convert_timespec(times[1]);
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, flags);
}

dword_t sys_utimes(addr_t path_addr, addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct timeval_ times[2];
        if (user_get(times_addr, times))
            return _EFAULT;
        atime = convert_timeval(times[0]);
        mtime = convert_timeval(times[1]);
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}

dword_t sys_utime(addr_t path_addr, addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct utimbuf_ {
            time_t_ actime;
            time_t_ modtime;
        } times;
        if (user_get(times_addr, times))
            return _EFAULT;
        atime.tv_sec = times.actime;
        atime.tv_nsec = 0;
        mtime.tv_sec = times.modtime;
        mtime.tv_nsec = 0;
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}

static int generic_fsetattr(struct fd *fd, struct attr attr) {
    if (fd->mount->fs->fsetattr == NULL)
        return _EPERM;
    return fd->mount->fs->fsetattr(fd, attr);
}

dword_t sys_fchmod(fd_t f, dword_t mode) {
    STRACE("fchmod(%d, %o)", f, mode);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    mode &= ~S_IFMT;
    return generic_fsetattr(fd, make_attr(mode, mode));
}

dword_t sys_fchmodat(fd_t at_f, addr_t path_addr, dword_t mode) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("fchmodat(%d, \"%s\", %o)", at_f, path, mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    mode &= ~S_IFMT;
    return generic_setattrat(at, path, make_attr(mode, mode), true);
}

dword_t sys_chmod(addr_t path_addr, dword_t mode) {
    return sys_fchmodat(AT_FDCWD_, path_addr, mode);
}

dword_t sys_fchown32(fd_t f, uid_t_ owner, uid_t_ group) {
    STRACE("fchown(%d, %d, %d)", f, owner, group);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err;
    if (owner != (uid_t) -1) {
        err = generic_fsetattr(fd, make_attr(uid, owner));
        if (err < 0)
            return err;
    }
    if (group != (uid_t) -1) {
        err = generic_fsetattr(fd, make_attr(gid, group));
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_fchownat(fd_t at_f, addr_t path_addr, dword_t owner, dword_t group, int flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("fchownat(%d, \"%s\", %d, %d, %d)", at_f, path, owner, group, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    int err;
    bool follow_links = flags & AT_SYMLINK_NOFOLLOW_ ? false : true;
    if (owner != (uid_t) -1) {
        err = generic_setattrat(at, path, make_attr(uid, owner), follow_links);
        if (err < 0)
            return err;
    }
    if (group != (uid_t) -1) {
        err = generic_setattrat(at, path, make_attr(gid, group), follow_links);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_chown32(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat(AT_FDCWD_, path_addr, owner, group, 0);
}

dword_t sys_lchown(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat(AT_FDCWD_, path_addr, owner, group, AT_SYMLINK_NOFOLLOW_);
}

static off_t_ truncate_size64(
        dword_t size_low, dword_t size_high) {
    qword_t bits = ((qword_t) size_high << 32) | size_low;
    off_t_ size;
    _Static_assert(sizeof(size) == sizeof(bits),
            "i386 truncate64 长度必须保持 64 位 wire 宽度");
    memcpy(&size, &bits, sizeof(size));
    return size;
}

dword_t sys_truncate(addr_t path_addr, dword_t raw_size) {
    off_t_ size = (off_t_) (sdword_t) raw_size;
    if (size < 0)
        return _EINVAL;
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    return file_truncate_task(current, path, size);
}

dword_t sys_ftruncate(fd_t f, dword_t raw_size) {
    off_t_ size = (off_t_) (sdword_t) raw_size;
    return file_ftruncate_task(current, f, size);
}

dword_t sys_truncate64(addr_t path_addr, dword_t size_low, dword_t size_high) {
    off_t_ size = truncate_size64(size_low, size_high);
    if (size < 0)
        return _EINVAL;
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    return file_truncate_task(current, path, size);
}

dword_t sys_ftruncate64(fd_t f, dword_t size_low, dword_t size_high) {
    off_t_ size = truncate_size64(size_low, size_high);
    return file_ftruncate_task(current, f, size);
}

dword_t sys_fallocate(fd_t f, dword_t UNUSED(mode), dword_t offset_low, dword_t offset_high, dword_t len_low, dword_t len_high) {
    off_t_ offset = ((qword_t) offset_high << 32) | offset_low;
    off_t_ len = ((qword_t) len_high << 32) | len_low;
    struct fd *fd = f_get_task_retain(current, f);
    if (fd == NULL)
        return _EBADF;
    int result;
    if (offset < 0 || len <= 0)
        result = _EINVAL;
    else if ((qword_t) offset >
            (qword_t) INT64_MAX - (qword_t) len)
        result = _EFBIG;
    else
        result = file_grow_fd(fd, offset + len);
    fd_close(fd);
    return result;
}

dword_t sys_mkdirat(fd_t at_f, addr_t path_addr, mode_t_ mode) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("mkdirat(%d, %s, 0%o)", at_f, path, mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    apply_umask(&mode);
    mode &= 0777;
    return generic_mkdirat(at, path, mode);
}

dword_t sys_mkdir(addr_t path_addr, mode_t_ mode) {
    return sys_mkdirat(AT_FDCWD_, path_addr, mode);
}

dword_t sys_rmdir(addr_t path_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("rmdir(%s)", path);
    return generic_rmdirat(AT_PWD, path);
}

dword_t sys_fsync(fd_t f) {
    return file_sync_task(current, f, false);
}

dword_t sys_fdatasync(fd_t f) {
    return file_sync_task(current, f, true);
}

// a few stubs
dword_t sys_sendfile(fd_t UNUSED(out_fd), fd_t UNUSED(in_fd), addr_t UNUSED(offset_addr), dword_t UNUSED(count)) {
    return _EINVAL;
}
dword_t sys_sendfile64(fd_t UNUSED(out_fd), fd_t UNUSED(in_fd), addr_t UNUSED(offset_addr), dword_t UNUSED(count)) {
    return _EINVAL;
}
dword_t sys_splice(fd_t UNUSED(in_fd), addr_t UNUSED(in_off_addr), fd_t UNUSED(out_fd), addr_t UNUSED(out_off_addr), dword_t UNUSED(count), dword_t UNUSED(flags)) {
    return _EINVAL;
}
dword_t sys_copy_file_range(fd_t UNUSED(in_fd), addr_t UNUSED(in_off), fd_t UNUSED(out_fd),
        addr_t UNUSED(out_off), dword_t UNUSED(len), uint_t UNUSED(flags)) {
    return _EPERM; // good enough for ruby
}

dword_t sys_xattr_stub(addr_t UNUSED(path_addr), addr_t UNUSED(name_addr),
        addr_t UNUSED(value_addr), dword_t UNUSED(size), dword_t UNUSED(flags)) {
    return _ENOTSUP;
}

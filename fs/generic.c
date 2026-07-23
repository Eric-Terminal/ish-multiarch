#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/dev.h"
#include "kernel/task.h"
#include "kernel/errno.h"

struct mount *find_mount_and_trim_path(char *path) {
    struct mount *mount = mount_find(path);
    char *dst = path;
    const char *src = path + strlen(mount->point);
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';
    return mount;
}

bool contains_mount_point(const char *path) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        int n = strlen(path);
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/'))
            return true;
    }
    return false;
}

static struct fd *provider_open(struct mount *mount, const char *path,
        int flags, int mode,
        const struct fs_access_identity *identity) {
    if (mount->fs->open_identity != NULL)
        return mount->fs->open_identity(
                mount, path, flags, mode, identity);
    return mount->fs->open(mount, path, flags, mode);
}

static struct fd *generic_openat_task_access(struct task *task,
        struct fd *at, const char *path_raw,
        int flags, int mode, int accmode) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);

    // TODO really, really, seriously reconsider what I'm doing with the strings
    struct task_credentials credentials;
    task_credentials_snapshot(task, &credentials);
    const struct fs_access_identity identity = {
        .uid = credentials.euid,
        .gid = credentials.egid,
    };
    char path[MAX_PATH];
    int err = path_normalize_task_access(task, at, path_raw, path,
            N_SYMLINK_FOLLOW |
            (flags & O_CREAT_ ? N_PARENT_DIR_WRITE : 0), &identity);
    if (err < 0)
        return ERR_PTR(err);
    struct mount *mount = find_mount_and_trim_path(path);
    int provider_flags = flags & ~O_TRUNC_;
    bool upgraded_truncate = (flags & O_TRUNC_) != 0 &&
            (flags & O_ACCMODE_) == O_RDONLY_;
    if (upgraded_truncate) {
        /* 用户态 provider 需要一份可截断且仍可读的稳定句柄。 */
        provider_flags = (provider_flags & ~O_ACCMODE_) | O_RDWR_;
    }
    struct fd *fd = provider_open(
            mount, path, provider_flags, mode, &identity);
    if (IS_ERR(fd)) {
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;

    lock(&inodes_lock); // TODO: don't do this
    struct statbuf stat = {};
    err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        unlock(&inodes_lock);
        goto error;
    }
    if (upgraded_truncate && !S_ISREG(stat.mode)) {
        /* 非普通文件不发生截断，重新按 guest 访问模式打开。 */
        unlock(&inodes_lock);
        mount_retain(mount);
        fd_close(fd);
        provider_flags = flags & ~O_TRUNC_;
        fd = provider_open(
                mount, path, provider_flags, mode, &identity);
        if (IS_ERR(fd)) {
            mount_release(mount);
            return fd;
        }
        fd->mount = mount;
        lock(&inodes_lock);
        memset(&stat, 0, sizeof(stat));
        err = fd->mount->fs->fstat(fd, &stat);
        if (err < 0) {
            unlock(&inodes_lock);
            goto error;
        }
    }
    fd->inode = inode_get_unlocked(
            mount, stat.inode_device, stat.inode);
    unlock(&inodes_lock);
    fd->type = stat.mode & S_IFMT;
    fd->flags = flags & ~(O_CREAT_ | O_EXCL_ | O_NOCTTY_ |
            O_TRUNC_ | O_CLOEXEC_);
    fd->logical_access_mode = true;

    // 目录查找先锁定对象类型，避免把普通文件的权限错误误报给调用方。
    err = _ENOTDIR;
    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_)
        goto error;
    err = _EISDIR;
    if (S_ISDIR(fd->type) &&
            (flags & (O_RDWR_ | O_WRONLY_ | O_TRUNC_)))
        goto error;

    if (fd->opened_created) {
        err = 0;
    } else if (accmode == AC_X && !(flags & O_DIRECTORY_) &&
            !(stat.mode & 0111)) {
        err = _EACCES;
    } else {
        err = access_check_identity(&identity, &stat,
                accmode | ((flags & O_TRUNC_) ? AC_W : 0));
    }
    if (err < 0)
        goto error;

    assert(!S_ISLNK(fd->type)); // would mean path_normalize didn't do its job
    if (S_ISBLK(fd->type) || S_ISCHR(fd->type)) {
        int type;
        if (S_ISBLK(fd->type))
            type = DEV_BLOCK;
        else
            type = DEV_CHAR;
        err = dev_open(dev_major(stat.rdev), dev_minor(stat.rdev), type, fd);
        if (err < 0)
            goto error;
    }
    err = _ENXIO;
    if (S_ISSOCK(fd->type))
        goto error;
    if (!fd->opened_created && S_ISREG(fd->type) &&
            (flags & O_TRUNC_) != 0) {
        err = file_truncate_open_fd(fd);
        if (err < 0)
            goto error;
    }
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

struct fd *generic_openat_task(struct task *task, struct fd *at,
        const char *path_raw, int flags, int mode) {
    int accmode;
    if (flags & O_RDWR_) accmode = AC_R | AC_W;
    else if (flags & O_WRONLY_) accmode = AC_W;
    else accmode = AC_R;
    return generic_openat_task_access(
            task, at, path_raw, flags, mode, accmode);
}

struct fd *generic_openat_exec_task(struct task *task,
        struct fd *at, const char *path) {
    return generic_openat_task_access(
            task, at, path, O_RDONLY_, 0, AC_X);
}

struct fd *generic_open_directory_task(struct task *task, const char *path) {
    return generic_openat_task_access(
            task, AT_PWD, path, O_DIRECTORY_, 0, AC_X);
}

struct fd *generic_open_exec(const char *path) {
    return generic_openat_exec_task(current, AT_PWD, path);
}

struct fd *generic_openat(struct fd *at, const char *path_raw, int flags, int mode) {
    return generic_openat_task(current, at, path_raw, flags, mode);
}

struct fd *generic_open(const char *path, int flags, int mode) {
    return generic_openat(AT_PWD, path, flags, mode);
}

int generic_getpath(struct fd *fd, char *buf) {
    int err = fd->mount->fs->getpath(fd, buf);
    if (err < 0)
        return err;
    if (strlen(buf) + strlen(fd->mount->point) >= MAX_PATH)
        return _ENAMETOOLONG;
    memmove(buf + strlen(fd->mount->point), buf, strlen(buf) + 1);
    memcpy(buf, fd->mount->point, strlen(fd->mount->point));
    if (buf[0] == '\0')
        strcpy(buf, "/");
    return 0;
}

int generic_accessat_task(struct task *task, struct fd *dirfd,
        const char *path_raw, int mode,
        const struct fs_access_identity *identity) {
    char path[MAX_PATH];
    int err = path_normalize_task_access(task, dirfd, path_raw, path,
            N_SYMLINK_FOLLOW, identity);
    if (err < 0)
        return err;

    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    mount_release(mount);
    if (err < 0)
        return err;
    return access_check_identity(identity, &stat, mode);
}

int generic_accessat(struct fd *dirfd, const char *path_raw, int mode) {
    struct task_credentials credentials;
    task_credentials_snapshot(current, &credentials);
    const struct fs_access_identity identity = {
        .uid = credentials.euid,
        .gid = credentials.egid,
    };
    return generic_accessat_task(
            current, dirfd, path_raw, mode, &identity);
}

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_unlinkat_task(struct task *task,
        struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize_task(
            task, at, path_raw, path,
            N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    mount_release(mount);
    return err;
}

int generic_unlinkat(struct fd *at, const char *path_raw) {
    return generic_unlinkat_task(current, at, path_raw);
}

int generic_renameat_task(struct task *task,
        struct fd *src_at, const char *src_raw,
        struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize_task(
            task, src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize_task(task, dst_at, dst_raw, dst,
            N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->rename == NULL)
        err = _EPERM;
    else
        err = mount->fs->rename(mount, src, dst);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw,
        struct fd *dst_at, const char *dst_raw) {
    return generic_renameat_task(
            current, src_at, src_raw, dst_at, dst_raw);
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(link);
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    mount_release(mount);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    mount_release(mount);
    return err;
}

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->setattr)
        err = mount->fs->setattr(mount, path, attr);
    mount_release(mount);
    return err;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat_task(struct task *task, struct fd *at,
        const char *path_raw, char *buffer, size_t size) {
    char path[MAX_PATH];
    int error = path_normalize_task(
            task, at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (error < 0)
        return error;
    struct mount *mount = find_mount_and_trim_path(path);
    ssize_t result = _EINVAL;
    if (mount->fs->readlink)
        result = mount->fs->readlink(mount, path, buffer, size);
    mount_release(mount);
    return result;
}

ssize_t generic_readlinkat(
        struct fd *at, const char *path, char *buffer, size_t size) {
    return generic_readlinkat_task(current, at, path, buffer, size);
}

int generic_mkdirat_task(struct task *task,
        struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    int err = path_normalize_task(task, at, path_raw, path,
            N_SYMLINK_FOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    mount_release(mount);
    return err;
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    return generic_mkdirat_task(current, at, path_raw, mode);
}

int generic_rmdirat_task(struct task *task,
        struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize_task(task, at, path_raw, path,
            N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    mount_release(mount);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    return generic_rmdirat_task(current, at, path_raw);
}

int generic_seek(struct fd *fd, off_t_ off, int whence, off_t_ size) {
    off_t_ base;
    if (whence == LSEEK_SET) {
        base = 0;
    } else if (whence == LSEEK_CUR) {
        base = fd->offset;
    } else if (whence == LSEEK_END) {
        base = size;
    } else {
        return _EINVAL;
    }

    off_t_ new_off;
    if (__builtin_add_overflow(base, off, &new_off) || new_off < 0)
        return _EINVAL;
    fd->offset = new_off;
    return 0;
}

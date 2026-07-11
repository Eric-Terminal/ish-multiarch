#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

static struct fd *at_fd_task(struct task *task, fd_t fd_number) {
    if (fd_number == AT_FDCWD_)
        return AT_PWD;
    return f_get_task(task, fd_number);
}

static void apply_umask_task(struct task *task, mode_t_ *mode) {
    struct fs_info *fs = task->fs;
    lock(&fs->lock);
    *mode &= ~fs->umask;
    unlock(&fs->lock);
}

ssize_t file_read_task(struct task *task, fd_t fd_number,
        void *buffer, size_t size) {
    struct fd *fd = f_get_task(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    if (S_ISDIR(fd->type))
        return _EISDIR;

    if (fd->ops->read != NULL)
        return fd->ops->read(fd, buffer, size);
    if (fd->ops->pread == NULL)
        return _EBADF;

    ssize_t result = fd->ops->pread(fd, buffer, size, fd->offset);
    if (result > 0)
        fd->ops->lseek(fd, result, LSEEK_CUR);
    return result;
}

ssize_t file_write_task(struct task *task, fd_t fd_number,
        const void *buffer, size_t size) {
    struct fd *fd = f_get_task(task, fd_number);
    if (fd == NULL)
        return _EBADF;

    if (fd->ops->write != NULL)
        return fd->ops->write(fd, buffer, size);
    if (fd->ops->pwrite == NULL)
        return _EBADF;

    ssize_t result = fd->ops->pwrite(fd, buffer, size, fd->offset);
    if (result > 0)
        fd->ops->lseek(fd, result, LSEEK_CUR);
    return result;
}

static int file_fstat_fd(struct fd *fd, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    if (fd == NULL)
        return _EBADF;
    return fd->mount->fs->fstat(fd, stat);
}

int file_fstat_task(struct task *task, fd_t fd_number, struct statbuf *stat) {
    return file_fstat_fd(f_get_task(task, fd_number), stat);
}

int file_statat_task(struct task *task, fd_t dirfd,
        const char *path, int flags, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    if (flags & ~AT_STATAT_SUPPORTED_FLAGS_)
        return _EINVAL;
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        struct fd *fd;
        bool retained = false;
        if (dirfd == AT_FDCWD_) {
            lock(&task->fs->lock);
            fd = fd_retain(task->fs->pwd);
            unlock(&task->fs->lock);
            retained = true;
        } else {
            fd = f_get_task(task, dirfd);
        }
        if (fd == NULL)
            return _EBADF;
        int error = file_fstat_fd(fd, stat);
        if (retained)
            fd_close(fd);
        return error;
    }

    // 绝对路径和 openat 一样从目标 root 解析，不检查传入的 dirfd。
    struct fd *at = path[0] == '/' ? AT_PWD : at_fd_task(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type))
        return _ENOTDIR;
    return generic_statat_task(task, at, path, stat,
            !(flags & AT_SYMLINK_NOFOLLOW_));
}

ssize_t fs_getcwd_task(struct task *task, char *buffer, size_t size) {
    struct fs_info *fs = task->fs;
    lock(&fs->lock);
    char path[MAX_PATH + 1];
    int error = generic_getpath(fs->pwd, path);
    unlock(&fs->lock);
    if (error < 0)
        return error;

    size_t length = strlen(path) + 1;
    if (length > size)
        return _ERANGE;
    memcpy(buffer, path, length);
    return (ssize_t) length;
}

fd_t file_openat_task(struct task *task, fd_t dirfd,
        const char *path, int flags, mode_t_ mode) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return _EINVAL;
    if (path[0] == '\0')
        return _ENOENT;
    if (flags & O_CREAT_) {
        mode &= 07777;
        apply_umask_task(task, &mode);
    }

    // 绝对路径从目标任务根目录解析，Linux 不检查传入的 dirfd。
    struct fd *at = path[0] == '/' ? AT_PWD : at_fd_task(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type))
        return _ENOTDIR;
    struct fd *fd = generic_openat_task(task, at, path, flags, mode);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    return f_install_task(task, fd, flags);
}

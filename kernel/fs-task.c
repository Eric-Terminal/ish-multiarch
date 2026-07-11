#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

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

int file_fstat_task(struct task *task, fd_t fd_number, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    struct fd *fd = f_get_task(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    return fd->mount->fs->fstat(fd, stat);
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

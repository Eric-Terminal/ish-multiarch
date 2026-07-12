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

ssize_t file_read_fd(struct fd *fd, void *buffer, size_t size) {
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

ssize_t file_read_task(struct task *task, fd_t fd_number,
        void *buffer, size_t size) {
    return file_read_fd(f_get_task(task, fd_number), buffer, size);
}

ssize_t file_write_fd(struct fd *fd, const void *buffer, size_t size) {
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

ssize_t file_write_task(struct task *task, fd_t fd_number,
        const void *buffer, size_t size) {
    return file_write_fd(f_get_task(task, fd_number), buffer, size);
}

sqword_t file_lseek_task(
        struct task *task, fd_t fd_number, sqword_t offset, int whence) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops->lseek == NULL) {
        fd_close(fd);
        return _ESPIPE;
    }
    lock(&fd->lock);
    sqword_t result = fd->ops->lseek(fd, offset, whence);
    unlock(&fd->lock);
    fd_close(fd);
    return result;
}

int file_write_check_fd(struct fd *fd) {
    if (fd == NULL)
        return _EBADF;
    int flags = fd_getflags(fd);
    if (flags < 0)
        return flags;
    int access_mode = flags & O_ACCMODE_;
    if (access_mode != O_WRONLY_ && access_mode != O_RDWR_)
        return _EBADF;
    if (fd->ops->write == NULL && fd->ops->pwrite == NULL)
        return _EBADF;
    return 0;
}

int file_read_check_fd(struct fd *fd) {
    if (fd == NULL)
        return _EBADF;
    if (S_ISDIR(fd->type))
        return _EISDIR;
    int flags = fd_getflags(fd);
    if (flags < 0)
        return flags;
    int access_mode = flags & O_ACCMODE_;
    if (access_mode != O_RDONLY_ && access_mode != O_RDWR_)
        return _EBADF;
    if (fd->ops->read == NULL && fd->ops->pread == NULL)
        return _EBADF;
    return 0;
}

int file_write_check_task(struct task *task, fd_t fd_number) {
    return file_write_check_fd(f_get_task(task, fd_number));
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

int file_chdir_task(struct task *task, const char *path) {
    struct fd *directory = generic_open_directory_task(task, path);
    if (IS_ERR(directory))
        return PTR_ERR(directory);
    fs_chdir(task->fs, directory);
    return 0;
}

int file_fchdir_task(struct task *task, fd_t fd_number) {
    struct fd *directory = f_get_task_retain(task, fd_number);
    if (directory == NULL)
        return _EBADF;
    if (!S_ISDIR(directory->type)) {
        fd_close(directory);
        return _ENOTDIR;
    }

    struct statbuf stat;
    int error = file_fstat_fd(directory, &stat);
    if (error == 0)
        error = access_check_task(task, &stat, AC_X);
    if (error < 0) {
        fd_close(directory);
        return error;
    }
    fs_chdir(task->fs, directory);
    return 0;
}

ssize_t file_readlinkat_task(struct task *task, fd_t dirfd,
        const char *path, char *buffer, size_t size) {
    if (path[0] == '\0')
        return _ENOENT;
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = retained ? f_get_task_retain(task, dirfd) : AT_PWD;
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    ssize_t result = generic_readlinkat_task(
            task, at, path, buffer, size);
    if (retained)
        fd_close(at);
    return result;
}

ssize_t file_ioctl_size_fd(struct fd *fd, dword_t command) {
    switch (command) {
        case FIONBIO_:
            return sizeof(dword_t);
        case FIOCLEX_:
        case FIONCLEX_:
            return 0;
    }
    if (fd->ops->ioctl_size == NULL || fd->ops->ioctl == NULL)
        return _ENOTTY;
    return fd->ops->ioctl_size((int) command);
}

int file_ioctl_fd_task(struct task *task, fd_t fd_number, struct fd *fd,
        dword_t command, void *buffer, dword_t scalar) {
    if (command == FIONBIO_) {
        int flags = fd_getflags(fd);
        if (*(const dword_t *) buffer != 0)
            flags |= O_NONBLOCK_;
        else
            flags &= ~O_NONBLOCK_;
        return fd_setflags(fd, flags);
    }
    if (command == FIOCLEX_ || command == FIONCLEX_) {
        struct fdtable *table = task->files;
        lock(&table->lock);
        if (fdtable_get(table, fd_number) != fd) {
            unlock(&table->lock);
            return _EBADF;
        }
        if (command == FIOCLEX_)
            bit_set((size_t) fd_number, table->cloexec);
        else
            bit_clear((size_t) fd_number, table->cloexec);
        unlock(&table->lock);
        return 0;
    }
    if (fd->ops->ioctl == NULL)
        return _ENOTTY;
    void *argument = buffer != NULL ? buffer : (void *) (uintptr_t) scalar;
    return fd->ops->ioctl(fd, (int) command, argument);
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
        return (fd_t) PTR_ERR(fd);
    return f_install_task(task, fd, flags);
}

int file_unlinkat_task(struct task *task, fd_t dirfd,
        const char *path, bool remove_directory) {
    if (path[0] == '\0')
        return _ENOENT;

    // 绝对路径从目标任务根目录解析，不检查传入的 dirfd。
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = retained ? f_get_task_retain(task, dirfd) : AT_PWD;
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    int result = remove_directory ?
            generic_rmdirat_task(task, at, path) :
            generic_unlinkat_task(task, at, path);
    if (retained)
        fd_close(at);
    return result;
}

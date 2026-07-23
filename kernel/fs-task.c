#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "guest/memory/file-pager.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

_Static_assert(sizeof(off_t) >= sizeof(off_t_),
        "host off_t 必须完整容纳 guest 文件偏移");

static struct fd *at_fd_task_retain(
        struct task *task, fd_t fd_number) {
    if (fd_number == AT_FDCWD_)
        return AT_PWD;
    return f_get_task_retain(task, fd_number);
}

static struct fd *task_cwd_retain(struct task *task) {
    struct fs_info *fs = task->fs;
    lock(&fs->lock);
    struct fd *cwd = fs->pwd == NULL ? NULL : fd_retain(fs->pwd);
    unlock(&fs->lock);
    return cwd;
}

static void apply_umask_task(struct task *task, mode_t_ *mode) {
    struct fs_info *fs = task->fs;
    lock(&fs->lock);
    *mode &= ~fs->umask;
    unlock(&fs->lock);
}

struct file_io_guard {
    struct inode_data *inode;
    struct guest_file_pager *pager;
};

static bool fd_has_coherent_page_cache(const struct fd *fd) {
    return fd != NULL && fd->inode != NULL && S_ISREG(fd->type) &&
            fd->ops->page_cacheable;
}

static void begin_file_io(
        struct fd *fd, struct file_io_guard *guard) {
    *guard = (struct file_io_guard) {0};
    if (!fd_has_coherent_page_cache(fd))
        return;
    struct inode_data *inode = fd->inode;

    for (;;) {
        lock(&inode->lock);
        struct guest_file_pager *pager = inode->file_pager;
        assert((pager == NULL) ==
                (inode->file_pager_context == NULL));
        if (pager != NULL) {
            if (guest_file_pager_try_retain(pager)) {
                unlock(&inode->lock);
                lock(&inode->file_io_lock);
                *guard = (struct file_io_guard) {
                    .inode = inode,
                    .pager = pager,
                };
                return;
            }
            int waited = wait_for_ignore_signals(
                    &inode->file_pager_changed,
                    &inode->lock, NULL);
            assert(waited == 0);
            unlock(&inode->lock);
            continue;
        }
        unlock(&inode->lock);

        lock(&inode->file_io_lock);
        lock(&inode->lock);
        pager = inode->file_pager;
        assert((pager == NULL) ==
                (inode->file_pager_context == NULL));
        if (pager == NULL) {
            unlock(&inode->lock);
            *guard = (struct file_io_guard) {.inode = inode};
            return;
        }
        if (guest_file_pager_try_retain(pager)) {
            unlock(&inode->lock);
            *guard = (struct file_io_guard) {
                .inode = inode,
                .pager = pager,
            };
            return;
        }
        unlock(&inode->lock);
        unlock(&inode->file_io_lock);
    }
}

static void end_file_io(struct file_io_guard *guard) {
    if (guard->inode == NULL)
        return;
    unlock(&guard->inode->file_io_lock);
    guest_file_pager_release(guard->pager);
    *guard = (struct file_io_guard) {0};
}

static struct guest_file_pager *retain_file_pager(struct fd *fd) {
    if (!fd_has_coherent_page_cache(fd))
        return NULL;
    struct inode_data *inode = fd->inode;
    for (;;) {
        lock(&inode->lock);
        struct guest_file_pager *pager = inode->file_pager;
        assert((pager == NULL) ==
                (inode->file_pager_context == NULL));
        if (pager == NULL) {
            unlock(&inode->lock);
            return NULL;
        }
        if (guest_file_pager_try_retain(pager)) {
            unlock(&inode->lock);
            return pager;
        }
        int waited = wait_for_ignore_signals(
                &inode->file_pager_changed, &inode->lock, NULL);
        assert(waited == 0);
        unlock(&inode->lock);
    }
}

static ssize_t read_locked(struct fd *fd, void *buffer, size_t size,
        bool track_offset, qword_t *file_offset, bool *offset_known) {
    *offset_known = false;
    if (fd->ops->read != NULL) {
        off_t_ start = !track_offset || fd->ops->lseek == NULL ? _ESPIPE :
                fd->ops->lseek(fd, 0, LSEEK_CUR);
        ssize_t result = fd->ops->read(fd, buffer, size);
        if (result > 0 && start >= 0) {
            *file_offset = (qword_t) start;
            *offset_known = true;
        }
        return result;
    }
    if (fd->ops->pread == NULL)
        return _EBADF;
    if (fd->offset < 0 || fd->ops->lseek == NULL)
        return _ESPIPE;

    *file_offset = (qword_t) fd->offset;
    ssize_t result = fd->ops->pread(fd, buffer, size, fd->offset);
    if (result > 0) {
        off_t_ positioned = fd->ops->lseek(fd, result, LSEEK_CUR);
        if (positioned < 0)
            return positioned;
        *offset_known = true;
    }
    return result;
}

static ssize_t pread_locked(struct fd *fd, void *buffer,
        size_t size, off_t_ offset) {
    if (fd->ops->pread != NULL)
        return fd->ops->pread(fd, buffer, size, (off_t) offset);
    if (fd->ops->lseek == NULL)
        return _ESPIPE;

    off_t_ saved_offset = fd->ops->lseek(fd, 0, LSEEK_CUR);
    if (saved_offset < 0)
        return saved_offset;
    off_t_ positioned = fd->ops->lseek(fd, offset, LSEEK_SET);
    ssize_t result = positioned < 0 ? positioned :
            fd->ops->read(fd, buffer, size);
    off_t_ restored = fd->ops->lseek(
            fd, saved_offset, LSEEK_SET);
    assert(restored == saved_offset);
    return result;
}

static ssize_t write_locked(struct fd *fd, const void *buffer,
        size_t size, bool track_offset,
        qword_t *file_offset, bool *offset_known) {
    *offset_known = false;
    if (fd->ops->write != NULL) {
        ssize_t result = fd->ops->write(fd, buffer, size);
        if (result <= 0 || !track_offset || fd->ops->lseek == NULL)
            return result;
        off_t_ end = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if (end >= result) {
            *file_offset = (qword_t) (end - result);
            *offset_known = true;
        }
        return result;
    }
    if (fd->ops->pwrite == NULL)
        return _EBADF;
    if (fd->offset < 0 || fd->ops->lseek == NULL)
        return _ESPIPE;
    if (track_offset)
        *file_offset = (qword_t) fd->offset;
    ssize_t result = fd->ops->pwrite(fd, buffer, size, fd->offset);
    if (result > 0) {
        off_t_ positioned = fd->ops->lseek(
                fd, result, LSEEK_CUR);
        if (positioned < 0)
            return positioned;
        *offset_known = track_offset;
    }
    return result;
}

static ssize_t pwrite_locked(struct fd *fd, const void *buffer,
        size_t size, off_t_ offset) {
    if (fd->ops->pwrite != NULL)
        return fd->ops->pwrite(fd, buffer, size, (off_t) offset);
    if (fd->ops->lseek == NULL)
        return _ESPIPE;

    off_t_ saved_offset = fd->ops->lseek(fd, 0, LSEEK_CUR);
    if (saved_offset < 0)
        return saved_offset;
    off_t_ positioned = fd->ops->lseek(fd, offset, LSEEK_SET);
    ssize_t result = positioned < 0 ? positioned :
            fd->ops->write(fd, buffer, size);
    off_t_ restored = fd->ops->lseek(
            fd, saved_offset, LSEEK_SET);
    assert(restored == saved_offset);
    return result;
}

ssize_t file_read_fd(struct fd *fd, void *buffer, size_t size) {
    int error = file_read_check_fd(fd);
    if (error < 0)
        return error;

    struct file_io_guard guard;
    begin_file_io(fd, &guard);
    qword_t offset = 0;
    bool offset_known;
    bool lock_fd = fd_has_coherent_page_cache(fd);
    if (lock_fd)
        lock(&fd->lock);
    ssize_t result = read_locked(
            fd, buffer, size, guard.pager != NULL,
            &offset, &offset_known);
    if (lock_fd)
        unlock(&fd->lock);
    if (result > 0 && offset_known && guard.pager != NULL)
        guest_file_pager_read_resident(
                guard.pager, offset, buffer, (size_t) result);
    end_file_io(&guard);
    return result;
}

ssize_t file_read_task(struct task *task, fd_t fd_number,
        void *buffer, size_t size) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    ssize_t result = file_read_fd(fd, buffer, size);
    if (fd != NULL)
        fd_close(fd);
    return result;
}

ssize_t file_pread_fd(struct fd *fd, void *buffer,
        size_t size, off_t_ offset) {
    int error = file_read_check_fd(fd);
    if (error < 0)
        return error;
    if (offset < 0)
        return _EINVAL;

    struct file_io_guard guard;
    begin_file_io(fd, &guard);
    lock(&fd->lock);
    ssize_t result = pread_locked(fd, buffer, size, offset);
    unlock(&fd->lock);
    if (result > 0 && guard.pager != NULL)
        guest_file_pager_read_resident(guard.pager,
                (qword_t) offset, buffer, (size_t) result);
    end_file_io(&guard);
    return result;
}

ssize_t file_pread_fd_uncoordinated(struct fd *fd, void *buffer,
        size_t size, off_t_ offset) {
    int error = file_read_check_fd(fd);
    if (error < 0)
        return error;
    if (offset < 0)
        return _EINVAL;
    lock(&fd->lock);
    ssize_t result = pread_locked(fd, buffer, size, offset);
    unlock(&fd->lock);
    return result;
}

ssize_t file_write_fd(struct fd *fd, const void *buffer, size_t size) {
    int error = file_write_check_fd(fd);
    if (error < 0)
        return error;

    struct file_io_guard guard;
    begin_file_io(fd, &guard);
    qword_t offset = 0;
    bool offset_known;
    bool lock_fd = fd_has_coherent_page_cache(fd);
    if (lock_fd)
        lock(&fd->lock);
    ssize_t result = write_locked(
            fd, buffer, size, guard.pager != NULL,
            &offset, &offset_known);
    if (lock_fd)
        unlock(&fd->lock);
    if (result > 0 && offset_known && guard.pager != NULL)
        guest_file_pager_commit_file_write(
                guard.pager, offset, buffer, (size_t) result);
    end_file_io(&guard);
    return result;
}

ssize_t file_write_task(struct task *task, fd_t fd_number,
        const void *buffer, size_t size) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    ssize_t result = file_write_fd(fd, buffer, size);
    if (fd != NULL)
        fd_close(fd);
    return result;
}

ssize_t file_pwrite_fd(struct fd *fd, const void *buffer,
        size_t size, off_t_ offset) {
    int error = file_write_check_fd(fd);
    if (error < 0)
        return error;
    if (offset < 0)
        return _EINVAL;

    struct file_io_guard guard;
    begin_file_io(fd, &guard);
    qword_t actual_offset = (qword_t) offset;
    lock(&fd->lock);
    if (guard.pager != NULL && size != 0) {
        int flags = fd->ops->getflags != NULL ?
                fd->ops->getflags(fd) : (int) fd->flags;
        if (flags < 0) {
            unlock(&fd->lock);
            end_file_io(&guard);
            return flags;
        }
        if ((flags & O_APPEND_) != 0) {
            struct statbuf stat;
            int stat_error = file_fstat_fd(fd, &stat);
            if (stat_error < 0) {
                unlock(&fd->lock);
                end_file_io(&guard);
                return stat_error;
            }
            actual_offset = stat.size;
        }
    }
    ssize_t result = pwrite_locked(fd, buffer, size, offset);
    unlock(&fd->lock);
    if (result > 0 && guard.pager != NULL)
        guest_file_pager_commit_file_write(guard.pager,
                actual_offset, buffer, (size_t) result);
    end_file_io(&guard);
    return result;
}

ssize_t file_page_pwrite_fd_uncoordinated(struct fd *fd,
        const void *buffer, size_t size, off_t_ offset) {
    if (fd == NULL)
        return _EBADF;
    int flags = fd_getflags(fd);
    if (flags < 0)
        return flags;
    int access_mode = flags & O_ACCMODE_;
    if (access_mode != O_WRONLY_ && access_mode != O_RDWR_)
        return _EBADF;
    if (offset < 0)
        return _EINVAL;
    if (fd->ops->page_pwrite == NULL)
        return _EBADF;
    lock(&fd->lock);
    ssize_t result = fd->ops->page_pwrite(
            fd, buffer, size, (off_t) offset);
    unlock(&fd->lock);
    return result;
}

static bool fd_supports_sync(const struct fd *fd, bool data_only) {
    return fd != NULL && (fd->ops->fsync != NULL ||
            (data_only && fd->ops->fdatasync != NULL));
}

int file_sync_fd_uncoordinated(struct fd *fd, bool data_only) {
    if (fd == NULL)
        return _EBADF;
    int (*operation)(struct fd *) = data_only &&
            fd->ops->fdatasync != NULL ?
            fd->ops->fdatasync : fd->ops->fsync;
    if (operation == NULL)
        return _EINVAL;
    lock(&fd->lock);
    int result = operation(fd);
    unlock(&fd->lock);
    return result;
}

static int pager_sync_error(enum guest_file_sync_result result) {
    switch (result) {
        case GUEST_FILE_SYNC_OK:
            return 0;
        case GUEST_FILE_SYNC_IO_ERROR:
            return _EIO;
        case GUEST_FILE_SYNC_UNSUPPORTED:
            return _EINVAL;
    }
    assert(false);
    return _EIO;
}

int file_sync_fd(struct fd *fd, bool data_only) {
    if (fd == NULL)
        return _EBADF;
    if (!fd_supports_sync(fd, data_only))
        return _EINVAL;

    struct guest_file_pager *pager = retain_file_pager(fd);
    if (pager != NULL) {
        int error = pager_sync_error(guest_file_pager_sync_range(
                pager, 0, UINT64_MAX));
        guest_file_pager_release(pager);
        if (error < 0)
            return error;
        /* 生产 pager 的 range sync 已完成 data-only durability。 */
        if (data_only)
            return 0;
    }

    struct inode_data *inode = fd_has_coherent_page_cache(fd) ?
            fd->inode : NULL;
    if (inode != NULL)
        lock(&inode->file_io_lock);
    int result = file_sync_fd_uncoordinated(fd, data_only);
    if (inode != NULL)
        unlock(&inode->file_io_lock);
    return result;
}

int file_sync_task(
        struct task *task, fd_t fd_number, bool data_only) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    int result = file_sync_fd(fd, data_only);
    fd_close(fd);
    return result;
}

static int file_resize_fd(struct fd *fd, off_t_ size,
        bool require_writable, bool grow_only) {
    if (size < 0)
        return _EINVAL;
    if (fd == NULL)
        return _EBADF;
    if (!S_ISREG(fd->type))
        return _EINVAL;
    if (require_writable) {
        int flags = fd_getflags(fd);
        if (flags < 0)
            return flags;
        int access_mode = flags & O_ACCMODE_;
        if (access_mode != O_WRONLY_ && access_mode != O_RDWR_)
            return _EINVAL;
    }
    if (fd->mount == NULL || fd->mount->fs->fsetattr == NULL)
        return _EPERM;
    if ((fd->mount->flags & MS_READONLY_) != 0)
        return _EROFS;

    struct file_io_guard guard;
    begin_file_io(fd, &guard);
    lock(&fd->lock);
    struct statbuf stat;
    int result = file_fstat_fd(fd, &stat);
    bool resized = result == 0 &&
            (!grow_only || stat.size < (qword_t) size);
    if (resized)
        result = fd->mount->fs->fsetattr(
                fd, make_attr(size, size));
    unlock(&fd->lock);
    if (result == 0 && resized && guard.pager != NULL)
        guest_file_pager_resize_resident(
                guard.pager, stat.size, (qword_t) size);
    end_file_io(&guard);
    return result;
}

int file_ftruncate_fd(struct fd *fd, off_t_ size) {
    return file_resize_fd(fd, size, true, false);
}

int file_ftruncate_task(
        struct task *task, fd_t fd_number, off_t_ size) {
    if (size < 0)
        return _EINVAL;
    struct fd *fd = f_get_task_retain(task, fd_number);
    int result = file_ftruncate_fd(fd, size);
    if (fd != NULL)
        fd_close(fd);
    return result;
}

int file_flock_task(
        struct task *task, fd_t fd_number, int operation) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;

    int command = operation & ~LOCK_NB_;
    int result;
    if (command != LOCK_SH_ && command != LOCK_EX_ &&
            command != LOCK_UN_)
        result = _EINVAL;
    else if (fd->mount->fs->flock == NULL)
        result = _EBADF;
    else
        result = fd->mount->fs->flock(fd, operation);
    fd_close(fd);
    return result;
}

int file_grow_fd(struct fd *fd, off_t_ size) {
    return file_resize_fd(fd, size, true, true);
}

int file_truncate_open_fd(struct fd *fd) {
    return file_resize_fd(fd, 0, false, false);
}

int file_truncate_task(
        struct task *task, const char *path, off_t_ size) {
    if (size < 0)
        return _EINVAL;

    struct statbuf stat;
    int result = file_statat_task(
            task, AT_FDCWD_, path, 0, &stat);
    if (result < 0)
        return result;
    if (S_ISDIR(stat.mode))
        return _EISDIR;
    if (!S_ISREG(stat.mode))
        return _EINVAL;

    struct fd *fd = generic_openat_task(
            task, AT_PWD, path, O_WRONLY_ | O_NONBLOCK_, 0);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    if (S_ISDIR(fd->type))
        result = _EISDIR;
    else if (!S_ISREG(fd->type))
        result = _EINVAL;
    else
        result = file_ftruncate_fd(fd, size);
    fd_close(fd);
    return result;
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
    struct fd *fd = f_get_task_retain(task, fd_number);
    int result = file_write_check_fd(fd);
    if (fd != NULL)
        fd_close(fd);
    return result;
}

int file_fstat_fd(struct fd *fd, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    if (fd == NULL)
        return _EBADF;
    return fd->mount->fs->fstat(fd, stat);
}

int file_fstat_task(struct task *task, fd_t fd_number, struct statbuf *stat) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    int result = file_fstat_fd(fd, stat);
    if (fd != NULL)
        fd_close(fd);
    return result;
}

int file_fstatfs_task(
        struct task *task, fd_t fd_number, struct statfsbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;

    int result = 0;
    if (fd->mount->fs->statfs != NULL)
        result = fd->mount->fs->statfs(fd->mount, stat);
    if (result == 0) {
        if (stat->type == 0)
            stat->type = fd->mount->fs->magic;
        if (stat->frsize == 0)
            stat->frsize = stat->bsize;
        stat->flags = ST_VALID_ | (fd->mount->flags &
                (MS_READONLY_ | MS_NOSUID_ | MS_NODEV_ | MS_NOEXEC_));
    }
    fd_close(fd);
    return result;
}

int file_accessat_task(struct task *task, fd_t dirfd,
        const char *path, int mode) {
    if (mode & ~(AC_R | AC_W | AC_X))
        return _EINVAL;
    if (path[0] == '\0')
        return _ENOENT;

    // 绝对路径从目标任务根目录解析，Linux 不检查传入的 dirfd。
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = path[0] == '/' ? AT_PWD :
            at_fd_task_retain(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }

    struct task_credentials credentials;
    task_credentials_snapshot(task, &credentials);
    const struct fs_access_identity identity = {
        .uid = credentials.uid,
        .gid = credentials.gid,
    };
    int result = generic_accessat_task(
            task, at, path, mode, &identity);
    if (retained)
        fd_close(at);
    return result;
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
        if (dirfd == AT_FDCWD_) {
            lock(&task->fs->lock);
            fd = fd_retain(task->fs->pwd);
            unlock(&task->fs->lock);
        } else {
            fd = f_get_task_retain(task, dirfd);
        }
        if (fd == NULL)
            return _EBADF;
        int error = file_fstat_fd(fd, stat);
        fd_close(fd);
        return error;
    }

    // 绝对路径和 openat 一样从目标 root 解析，不检查传入的 dirfd。
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = path[0] == '/' ? AT_PWD :
            at_fd_task_retain(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    int result = generic_statat_task(task, at, path, stat,
            !(flags & AT_SYMLINK_NOFOLLOW_));
    if (retained)
        fd_close(at);
    return result;
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
        // Linux 在 fdget 后按描述符编号修改当前槽位；并发复用也遵循该语义。
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
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = path[0] == '/' ? AT_PWD :
            at_fd_task_retain(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    struct fd *fd = generic_openat_task(task, at, path, flags, mode);
    if (retained)
        fd_close(at);
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

int file_mkdirat_task(struct task *task, fd_t dirfd,
        const char *path, mode_t_ mode) {
    if (path[0] == '\0')
        return _ENOENT;
    mode &= 0777;
    apply_umask_task(task, &mode);

    // 绝对路径从目标 root 解析，不检查传入的 dirfd。
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = retained ? f_get_task_retain(task, dirfd) : AT_PWD;
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    int result = generic_mkdirat_task(task, at, path, mode);
    if (retained)
        fd_close(at);
    return result;
}

int file_renameat_task(struct task *task,
        fd_t source_dirfd, const char *source,
        fd_t destination_dirfd, const char *destination) {
    if (source[0] == '\0' || destination[0] == '\0')
        return _ENOENT;

    bool source_retained =
            source[0] != '/' && source_dirfd != AT_FDCWD_;
    struct fd *source_at = source_retained ?
            f_get_task_retain(task, source_dirfd) : AT_PWD;
    if (source_at == NULL)
        return _EBADF;
    if (source_at != AT_PWD && !S_ISDIR(source_at->type)) {
        fd_close(source_at);
        return _ENOTDIR;
    }

    bool destination_retained =
            destination[0] != '/' && destination_dirfd != AT_FDCWD_;
    struct fd *destination_at = destination_retained ?
            f_get_task_retain(task, destination_dirfd) : AT_PWD;
    if (destination_at == NULL) {
        if (source_retained)
            fd_close(source_at);
        return _EBADF;
    }
    if (destination_at != AT_PWD && !S_ISDIR(destination_at->type)) {
        if (destination_retained)
            fd_close(destination_at);
        if (source_retained)
            fd_close(source_at);
        return _ENOTDIR;
    }

    int result = generic_renameat_task(task,
            source_at, source, destination_at, destination);
    if (destination_retained)
        fd_close(destination_at);
    if (source_retained)
        fd_close(source_at);
    return result;
}

int file_symlinkat_task(struct task *task, const char *target,
        fd_t dirfd, const char *link) {
    if (target[0] == '\0' || link[0] == '\0')
        return _ENOENT;

    // 绝对链接路径从目标 root 解析，不检查传入的 dirfd。
    bool retained = link[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = retained ? f_get_task_retain(task, dirfd) : AT_PWD;
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    int result = generic_symlinkat_task(task, target, at, link);
    if (retained)
        fd_close(at);
    return result;
}

static struct attr mode_attr(mode_t_ mode) {
    return make_attr(mode, mode & 07777);
}

static int file_fchmod_fd(struct fd *fd, mode_t_ mode) {
    if (fd == NULL)
        return _EBADF;
    if (fd->mount == NULL || fd->mount->fs->fsetattr == NULL)
        return _EPERM;
    return fd->mount->fs->fsetattr(fd, mode_attr(mode));
}

int file_fchmod_task(struct task *task, fd_t fd_number, mode_t_ mode) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    int result = file_fchmod_fd(fd, mode);
    fd_close(fd);
    return result;
}

int file_fchmodat_task(struct task *task, fd_t dirfd,
        const char *path, mode_t_ mode) {
    if (path == NULL)
        return _EFAULT;
    if (path[0] == '\0')
        return _ENOENT;

    // 旧 fchmodat 固定跟随最终符号链接；带 flags 的 fchmodat2 是独立 syscall。
    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = path[0] == '/' ? AT_PWD :
            at_fd_task_retain(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    int result = generic_setattrat_task(
            task, at, path, mode_attr(mode), true);
    if (retained)
        fd_close(at);
    return result;
}

static struct attr ownership_attr(uid_t_ owner, uid_t_ group) {
    return (struct attr) {
        .type = attr_ownership,
        .ownership = {
            .uid = owner,
            .gid = group,
        },
    };
}

static int file_fchown_fd(
        struct fd *fd, uid_t_ owner, uid_t_ group) {
    if (fd == NULL)
        return _EBADF;
    if (fd->mount == NULL || fd->mount->fs->fsetattr == NULL)
        return _EPERM;
    return fd->mount->fs->fsetattr(
            fd, ownership_attr(owner, group));
}

int file_fchown_task(struct task *task, fd_t fd_number,
        uid_t_ owner, uid_t_ group) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    int result = file_fchown_fd(fd, owner, group);
    fd_close(fd);
    return result;
}

int file_fchownat_task(struct task *task, fd_t dirfd,
        const char *path, uid_t_ owner, uid_t_ group, int flags) {
    if (flags & ~(AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_))
        return _EINVAL;
    if (path == NULL)
        return _EFAULT;
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        struct fd *fd = dirfd == AT_FDCWD_ ?
                task_cwd_retain(task) :
                f_get_task_retain(task, dirfd);
        if (fd == NULL)
            return _EBADF;
        int result = file_fchown_fd(fd, owner, group);
        fd_close(fd);
        return result;
    }

    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = path[0] == '/' ? AT_PWD :
            at_fd_task_retain(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }

    bool follow_links = !(flags & AT_SYMLINK_NOFOLLOW_);
    int result = generic_setattrat_task(task, at, path,
            ownership_attr(owner, group), follow_links);
    if (retained)
        fd_close(at);
    return result;
}

int file_utimensat_task(struct task *task, fd_t dirfd,
        const char *path, const struct file_timespec times[2], int flags) {
    if (times[0].nsec == LINUX_UTIME_OMIT_ &&
            times[1].nsec == LINUX_UTIME_OMIT_)
        return 0;
    if (flags & ~(AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_))
        return _EINVAL;

    if (path == NULL) {
        if (dirfd == AT_FDCWD_)
            return _EFAULT;
        if (flags != 0)
            return _EINVAL;
        struct fd *fd = f_get_task_retain(task, dirfd);
        if (fd == NULL)
            return _EBADF;
        int result = generic_futimens(fd, times);
        fd_close(fd);
        return result;
    }

    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        struct fd *fd = dirfd == AT_FDCWD_ ?
                task_cwd_retain(task) :
                f_get_task_retain(task, dirfd);
        if (fd == NULL)
            return _EBADF;
        int result = generic_futimens(fd, times);
        fd_close(fd);
        return result;
    }

    bool retained = path[0] != '/' && dirfd != AT_FDCWD_;
    struct fd *at = path[0] == '/' ? AT_PWD :
            at_fd_task_retain(task, dirfd);
    if (at == NULL)
        return _EBADF;
    if (at != AT_PWD && !S_ISDIR(at->type)) {
        fd_close(at);
        return _ENOTDIR;
    }
    int result = generic_utimens_task(task, at, path, times,
            !(flags & AT_SYMLINK_NOFOLLOW_));
    if (retained)
        fd_close(at);
    return result;
}

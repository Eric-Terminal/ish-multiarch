#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-exec-image.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define AARCH64_EXEC_IMAGE_READ_CHUNK (1024 * 1024)

static void release_snapshot_fd(void *opaque) {
    fd_close(opaque);
}

static int snapshot_fd(struct fd *fd,
        struct ish_aarch64_exec_image *image) {
    struct statbuf stat;
    int error = fd->mount->fs->fstat(fd, &stat);
    if (error < 0)
        return error;
    if (!S_ISREG(fd->type) || fd->inode == NULL ||
            fd->inode->futex_sequence == 0)
        return _ENOEXEC;

    size_t size = (size_t) stat.size;
    if ((qword_t) size != stat.size || stat.size > INT64_MAX)
        return _EFBIG;
    byte_t *data = malloc(size == 0 ? 1 : size);
    if (data == NULL)
        return _ENOMEM;
    if (size == 0)
        goto out_source;

    bool positioned = fd->ops->pread != NULL;
    off_t_ saved_offset = 0;
    bool offset_locked = false;
    bool restore_offset = false;
    if (!positioned) {
        if (fd->ops->read == NULL || fd->ops->lseek == NULL) {
            error = _EIO;
            goto out_free;
        }
        lock(&fd->lock);
        offset_locked = true;
        saved_offset = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if (saved_offset < 0) {
            error = (int) saved_offset;
            goto out_unlock;
        }
        restore_offset = true;
        off_t_ start = fd->ops->lseek(fd, 0, LSEEK_SET);
        if (start != 0) {
            error = start < 0 ? (int) start : _EIO;
            goto out_restore;
        }
    }

    size_t copied = 0;
    while (copied != size) {
        size_t remaining = size - copied;
        size_t chunk = remaining < AARCH64_EXEC_IMAGE_READ_CHUNK ?
                remaining : AARCH64_EXEC_IMAGE_READ_CHUNK;
        ssize_t read = positioned ?
                fd->ops->pread(fd, data + copied, chunk, (off_t) copied) :
                fd->ops->read(fd, data + copied, chunk);
        if (read < 0) {
            error = (int) read;
            goto out_restore;
        }
        if (read == 0 || (size_t) read > chunk) {
            error = _EIO;
            goto out_restore;
        }
        copied += (size_t) read;
    }

out_restore:
    if (restore_offset) {
        off_t_ restored = fd->ops->lseek(
                fd, saved_offset, LSEEK_SET);
        if (error == 0 && restored != saved_offset)
            error = restored < 0 ? (int) restored : _EIO;
    }
out_unlock:
    if (offset_locked)
        unlock(&fd->lock);
    if (error < 0)
        goto out_free;
out_source:;
    struct fd *source_fd = fd_retain(fd);
    struct guest_file_source *file_source = guest_file_source_create(
            fd->inode->futex_sequence, source_fd, release_snapshot_fd);
    if (file_source == NULL) {
        fd_close(source_fd);
        error = _ENOMEM;
        goto out_free;
    }
    *image = (struct ish_aarch64_exec_image) {
        .data = data,
        .size = size,
        .file_source = file_source,
    };
    return 0;

out_free:
    free(data);
    return error;
}

void ish_aarch64_exec_images_destroy(
        struct ish_aarch64_exec_images *images) {
    if (images == NULL)
        return;
    guest_file_source_release(images->main.file_source);
    guest_file_source_release(images->interpreter.file_source);
    free(images->main.data);
    free(images->interpreter.data);
    *images = (struct ish_aarch64_exec_images) {0};
}

int ish_aarch64_exec_images_read(struct task *task, struct fd *main_fd,
        struct ish_aarch64_exec_images *images) {
    assert(task != NULL && task == current && main_fd != NULL &&
            images != NULL);
    *images = (struct ish_aarch64_exec_images) {0};

    int error = snapshot_fd(main_fd, &images->main);
    if (error < 0)
        return error;

    char interpreter_path[MAX_PATH];
    struct aarch64_linux_interpreter_path_result path =
            aarch64_linux_copy_interpreter_path(
                    images->main.data, images->main.size,
                    interpreter_path, sizeof(interpreter_path));
    if (path.status == AARCH64_LINUX_INTERPRETER_PATH_BAD_ELF) {
        error = _ENOEXEC;
        goto out_destroy;
    }
    if (path.status == AARCH64_LINUX_INTERPRETER_PATH_NONE)
        return 0;
    if (path.status != AARCH64_LINUX_INTERPRETER_PATH_COPIED) {
        error = _ENAMETOOLONG;
        goto out_destroy;
    }

    struct fd *interpreter_fd = generic_openat_exec_task(
            task, AT_PWD, interpreter_path);
    if (IS_ERR(interpreter_fd)) {
        error = (int) PTR_ERR(interpreter_fd);
        goto out_destroy;
    }
    error = snapshot_fd(interpreter_fd, &images->interpreter);
    fd_close(interpreter_fd);
    if (error < 0)
        goto out_destroy;
    return 0;

out_destroy:
    ish_aarch64_exec_images_destroy(images);
    return error;
}

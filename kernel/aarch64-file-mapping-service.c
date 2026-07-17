#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/linux/mman.h"
#include "kernel/aarch64-file-mapping-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

struct kernel_file_pager {
    struct fd *fd;
    struct guest_file_pager *pager;
};

static enum guest_file_page_result read_file_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->fd != NULL &&
            page != NULL && valid_bytes != NULL);
    assert((file_offset & GUEST_MEMORY_PAGE_MASK) == 0);
    *valid_bytes = 0;

    struct statbuf stat;
    if (file_fstat_fd(provider->fd, &stat) < 0)
        return GUEST_FILE_PAGE_IO_ERROR;
    if (file_offset >= stat.size)
        return GUEST_FILE_PAGE_END_OF_FILE;

    qword_t remaining = stat.size - file_offset;
    size_t target = remaining < GUEST_MEMORY_PAGE_SIZE ?
            (size_t) remaining : GUEST_MEMORY_PAGE_SIZE;
    size_t completed = 0;
    while (completed < target) {
        assert(file_offset <= INT64_MAX &&
                completed <= (size_t) (INT64_MAX - file_offset));
        ssize_t read = file_pread_fd(provider->fd,
                page + completed, target - completed,
                (off_t_) (file_offset + completed));
        if (read < 0)
            return GUEST_FILE_PAGE_IO_ERROR;
        if (read == 0)
            break;
        completed += (size_t) read;
    }
    if (completed == 0)
        return GUEST_FILE_PAGE_END_OF_FILE;
    *valid_bytes = (dword_t) completed;
    return GUEST_FILE_PAGE_OK;
}

static void release_file_pager(struct guest_file_pager *pager,
        void *opaque) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->fd != NULL &&
            provider->pager == pager);
    struct fd *fd = provider->fd;
    struct inode_data *inode = fd->inode;
    assert(inode != NULL);

    lock(&inode->lock);
    if (inode->file_pager == pager)
        inode->file_pager = NULL;
    unlock(&inode->lock);

    /* fd_close 可能进入 inodes_lock，不能在 inode 锁内执行。 */
    fd_close(fd);
    free(provider);
}

/* 接管 retained_fd 的强引用，无论成功失败都不归还给调用方。 */
static struct guest_file_pager *acquire_inode_pager(
        struct fd *retained_fd) {
    assert(retained_fd != NULL && retained_fd->inode != NULL);
    struct inode_data *inode = retained_fd->inode;

    lock(&inode->lock);
    struct guest_file_pager *pager = inode->file_pager;
    if (pager != NULL && guest_file_pager_try_retain(pager)) {
        unlock(&inode->lock);
        fd_close(retained_fd);
        return pager;
    }
    if (pager != NULL)
        inode->file_pager = NULL;
    unlock(&inode->lock);

    struct kernel_file_pager *provider = malloc(sizeof(*provider));
    if (provider == NULL) {
        fd_close(retained_fd);
        return NULL;
    }
    *provider = (struct kernel_file_pager) {
        .fd = retained_fd,
    };
    struct guest_file_pager *candidate = guest_file_pager_create(
            inode->futex_sequence,
            (struct guest_file_pager_provider) {
                .opaque = provider,
                .read_page = read_file_page,
                .release = release_file_pager,
            });
    if (candidate == NULL) {
        fd_close(retained_fd);
        free(provider);
        return NULL;
    }
    provider->pager = candidate;

    lock(&inode->lock);
    pager = inode->file_pager;
    if (pager != NULL && guest_file_pager_try_retain(pager)) {
        unlock(&inode->lock);
        guest_file_pager_release(candidate);
        return pager;
    }
    if (pager != NULL)
        inode->file_pager = NULL;
    inode->file_pager = candidate;
    unlock(&inode->lock);
    return candidate;
}

static fd_t mapping_fd(qword_t argument) {
    return (fd_t) (sdword_t) (dword_t) argument;
}

static int validate_file_mapping_request(
        const struct guest_linux_file_mapping_request *request,
        const struct fd *fd, qword_t *maximum_protection) {
    if (request->length == 0)
        return _EINVAL;
    if (request->length > UINT64_MAX - GUEST_MEMORY_PAGE_MASK)
        return _ENOMEM;
    qword_t mapped_size = (request->length + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    if (mapped_size > (qword_t) INT64_MAX ||
            request->offset > (qword_t) INT64_MAX - mapped_size)
        return _EOVERFLOW;

    qword_t mapping_type = request->flags & GUEST_LINUX_MAP_TYPE;
    if (mapping_type != GUEST_LINUX_MAP_PRIVATE)
        return mapping_type == GUEST_LINUX_MAP_SHARED ?
                _EOPNOTSUPP : _EINVAL;
    qword_t allowed = GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_FIXED | GUEST_LINUX_MAP_FIXED_NOREPLACE;
    if ((request->flags & ~allowed) != 0)
        return _EINVAL;
    int flags = fd_getflags((struct fd *) fd);
    if (flags < 0)
        return flags;
    int access_mode = flags & O_ACCMODE_;
    if (access_mode != O_RDONLY_ && access_mode != O_RDWR_)
        return _EACCES;
    bool noexec = fd->mount != NULL &&
            (fd->mount->flags & MS_NOEXEC_) != 0;
    if ((request->protection & GUEST_LINUX_PROT_EXEC) != 0 && noexec)
        return _EPERM;

    *maximum_protection = GUEST_LINUX_PROT_MASK;
    if (noexec)
        *maximum_protection &= ~GUEST_LINUX_PROT_EXEC;
    return 0;
}

static sdword_t acquire_file_mapping(
        const struct guest_linux_file_mapping_context *context,
        qword_t fd_number,
        struct guest_linux_file_mapping_handle *handle) {
    assert(context != NULL && handle != NULL);
    assert(handle->opaque == NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && task == current);
    struct fd *fd = f_get_task_retain(task, mapping_fd(fd_number));
    if (fd == NULL)
        return _EBADF;
    handle->opaque = fd;
    return 0;
}

static void release_file_mapping(
        struct guest_linux_file_mapping_handle *handle) {
    assert(handle != NULL && handle->opaque != NULL);
    struct fd *fd = handle->opaque;
    handle->opaque = NULL;
    fd_close(fd);
}

static sdword_t open_file_mapping(
        const struct guest_linux_file_mapping_handle *handle,
        const struct guest_linux_file_mapping_request *request,
        struct guest_linux_file_mapping *mapping) {
    assert(handle != NULL && handle->opaque != NULL &&
            request != NULL && mapping != NULL);
    struct fd *fd = handle->opaque;
    *mapping = (struct guest_linux_file_mapping) {0};

    if ((request->offset & GUEST_MEMORY_PAGE_MASK) != 0)
        return _EINVAL;

    qword_t maximum_protection;
    int error = validate_file_mapping_request(
            request, fd, &maximum_protection);
    if (error < 0)
        return error;
    if (!S_ISREG(fd->type) || fd->mount == NULL || fd->inode == NULL ||
            !fd->ops->page_cacheable ||
            fd->ops->pread == NULL)
        return _ENODEV;

    struct guest_file_pager *pager = acquire_inode_pager(fd_retain(fd));
    if (pager == NULL)
        return _ENOMEM;
    *mapping = (struct guest_linux_file_mapping) {
        .pager = pager,
        .maximum_protection = maximum_protection,
    };
    return 0;
}

const struct guest_linux_file_mapping_service
        ish_aarch64_linux_file_mapping_service = {
    .acquire = acquire_file_mapping,
    .release = release_file_mapping,
    .open = open_file_mapping,
};

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
    struct fd *reader_fd;
    struct fd *writer_fd;
    struct guest_file_pager *pager;
    struct inode_data *inode;
};

static void begin_pager_io(void *opaque) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->inode != NULL);
    lock(&provider->inode->file_io_lock);
}

static void end_pager_io(void *opaque) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->inode != NULL &&
            lock_owned_by_current(&provider->inode->file_io_lock));
    unlock(&provider->inode->file_io_lock);
}

static enum guest_file_page_result read_file_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->reader_fd != NULL &&
            page != NULL && valid_bytes != NULL);
    assert(lock_owned_by_current(&provider->inode->file_io_lock));
    assert((file_offset & GUEST_MEMORY_PAGE_MASK) == 0);
    *valid_bytes = 0;

    struct statbuf stat;
    if (file_fstat_fd(provider->reader_fd, &stat) < 0)
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
        ssize_t read = file_pread_fd_uncoordinated(provider->reader_fd,
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

static enum guest_file_sync_result write_file_page(void *opaque,
        qword_t file_offset, const byte_t *page) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && page != NULL &&
            lock_owned_by_current(&provider->inode->file_io_lock));
    assert((file_offset & GUEST_MEMORY_PAGE_MASK) == 0);

    struct fd *writer = provider->writer_fd;
    if (writer == NULL)
        return GUEST_FILE_SYNC_UNSUPPORTED;
    struct statbuf stat;
    if (file_fstat_fd(writer, &stat) < 0)
        return GUEST_FILE_SYNC_IO_ERROR;
    if (file_offset >= stat.size)
        return GUEST_FILE_SYNC_OK;

    qword_t remaining = stat.size - file_offset;
    size_t target = remaining < GUEST_MEMORY_PAGE_SIZE ?
            (size_t) remaining : GUEST_MEMORY_PAGE_SIZE;
    size_t completed = 0;
    while (completed < target) {
        assert(file_offset <= INT64_MAX &&
                completed <= (size_t) (INT64_MAX - file_offset));
        ssize_t written = file_page_pwrite_fd_uncoordinated(writer,
                page + completed, target - completed,
                (off_t_) (file_offset + completed));
        if (written <= 0)
            return GUEST_FILE_SYNC_IO_ERROR;
        completed += (size_t) written;
    }
    return GUEST_FILE_SYNC_OK;
}

static enum guest_file_sync_result sync_file_range(void *opaque,
        qword_t file_offset, qword_t length) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->reader_fd != NULL &&
            lock_owned_by_current(&provider->inode->file_io_lock));
    use(file_offset, length);
    struct fd *fd = provider->writer_fd != NULL ?
            provider->writer_fd : provider->reader_fd;
    int error = file_sync_fd_uncoordinated(fd, true);
    if (error == 0)
        return GUEST_FILE_SYNC_OK;
    return error == _EINVAL ? GUEST_FILE_SYNC_UNSUPPORTED :
            GUEST_FILE_SYNC_IO_ERROR;
}

static void notify_failed_drain(
        struct guest_file_pager *pager, void *opaque) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->pager == pager &&
            provider->inode != NULL);
    lock(&provider->inode->lock);
    assert(provider->inode->file_pager == pager &&
            provider->inode->file_pager_context == provider);
    /* 等待者会 retain 同一个已复活 pager，不能建立第二套 inode cache。 */
    notify(&provider->inode->file_pager_changed);
    unlock(&provider->inode->lock);
}

static void release_file_pager(struct guest_file_pager *pager,
        void *opaque) {
    struct kernel_file_pager *provider = opaque;
    assert(provider != NULL && provider->reader_fd != NULL &&
            provider->pager == pager && provider->inode != NULL);
    struct inode_data *inode = provider->inode;

    lock(&inode->lock);
    if (inode->file_pager == pager) {
        assert(inode->file_pager_context == provider);
        inode->file_pager = NULL;
        inode->file_pager_context = NULL;
        notify(&inode->file_pager_changed);
    } else {
        assert(inode->file_pager_context != provider);
    }
    unlock(&inode->lock);

    /* fd_close 可能进入 inodes_lock，不能在 inode 锁内执行。 */
    if (provider->writer_fd != NULL)
        fd_close(provider->writer_fd);
    fd_close(provider->reader_fd);
    free(provider);
}

/* 接管 writer 强引用；只会安装第一个可写句柄。 */
static void upgrade_inode_pager_writer(
        struct kernel_file_pager *provider, struct fd *writer) {
    assert(provider != NULL && writer != NULL &&
            writer->inode == provider->inode);
    lock(&provider->inode->file_io_lock);
    if (provider->writer_fd == NULL) {
        provider->writer_fd = writer;
        writer = NULL;
    }
    unlock(&provider->inode->file_io_lock);
    if (writer != NULL)
        fd_close(writer);
}

/* 接管 retained_fd 的强引用，无论成功失败都不归还给调用方。 */
static struct guest_file_pager *acquire_inode_pager(
        struct fd *retained_fd, bool need_writer, int *error) {
    assert(retained_fd != NULL && retained_fd->inode != NULL &&
            error != NULL);
    *error = 0;
    struct inode_data *inode = retained_fd->inode;

    for (;;) {
        lock(&inode->lock);
        struct guest_file_pager *pager = inode->file_pager;
        struct kernel_file_pager *provider =
                inode->file_pager_context;
        assert((pager == NULL) == (provider == NULL));
        if (pager == NULL) {
            if (inode->host_shared_mapping_count != 0) {
                unlock(&inode->lock);
                fd_close(retained_fd);
                *error = _EBUSY;
                return NULL;
            }
            unlock(&inode->lock);
            break;
        }
        assert(inode->host_shared_mapping_count == 0);
        if (guest_file_pager_try_retain(pager)) {
            assert(provider->pager == pager &&
                    provider->inode == inode);
            unlock(&inode->lock);
            if (need_writer)
                upgrade_inode_pager_writer(provider, retained_fd);
            else
                fd_close(retained_fd);
            return pager;
        }
        int waited = wait_for_ignore_signals(
                &inode->file_pager_changed, &inode->lock, NULL);
        assert(waited == 0);
        unlock(&inode->lock);
    }

    struct kernel_file_pager *provider = malloc(sizeof(*provider));
    if (provider == NULL) {
        fd_close(retained_fd);
        *error = _ENOMEM;
        return NULL;
    }
    *provider = (struct kernel_file_pager) {
        .reader_fd = retained_fd,
        .writer_fd = need_writer ? fd_retain(retained_fd) : NULL,
        .inode = inode,
    };
    struct guest_file_pager *candidate = guest_file_pager_create(
            inode->futex_sequence,
            (struct guest_file_pager_provider) {
                .opaque = provider,
                .begin_io = begin_pager_io,
                .end_io = end_pager_io,
                .read_page = read_file_page,
                .write_page = write_file_page,
                .sync_range = sync_file_range,
                .drain_failed = notify_failed_drain,
                .release = release_file_pager,
            });
    if (candidate == NULL) {
        if (provider->writer_fd != NULL)
            fd_close(provider->writer_fd);
        fd_close(retained_fd);
        free(provider);
        *error = _ENOMEM;
        return NULL;
    }
    provider->pager = candidate;

    for (;;) {
        lock(&inode->lock);
        struct guest_file_pager *pager = inode->file_pager;
        struct kernel_file_pager *installed =
                inode->file_pager_context;
        assert((pager == NULL) == (installed == NULL));
        if (pager == NULL) {
            if (inode->host_shared_mapping_count != 0) {
                unlock(&inode->lock);
                guest_file_pager_release(candidate);
                *error = _EBUSY;
                return NULL;
            }
            inode->file_pager = candidate;
            inode->file_pager_context = provider;
            unlock(&inode->lock);
            return candidate;
        }
        assert(inode->host_shared_mapping_count == 0);
        if (guest_file_pager_try_retain(pager)) {
            assert(installed->pager == pager &&
                    installed->inode == inode);
            unlock(&inode->lock);
            if (need_writer) {
                struct fd *writer = provider->writer_fd;
                assert(writer != NULL);
                provider->writer_fd = NULL;
                upgrade_inode_pager_writer(installed, writer);
            }
            guest_file_pager_release(candidate);
            return pager;
        }
        int waited = wait_for_ignore_signals(
                &inode->file_pager_changed, &inode->lock, NULL);
        assert(waited == 0);
        unlock(&inode->lock);
    }
}

static fd_t mapping_fd(qword_t argument) {
    return (fd_t) (sdword_t) (dword_t) argument;
}

static int validate_file_mapping_request(
        const struct guest_linux_file_mapping_request *request,
        const struct fd *fd, qword_t *maximum_protection,
        bool *need_writer) {
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
    bool shared = mapping_type == GUEST_LINUX_MAP_SHARED ||
            mapping_type == GUEST_LINUX_MAP_SHARED_VALIDATE;
    if (!shared && mapping_type != GUEST_LINUX_MAP_PRIVATE)
        return _EINVAL;
    qword_t effective_flags = request->flags;
    if (mapping_type == GUEST_LINUX_MAP_SHARED) {
        effective_flags &= GUEST_LINUX_MAP_LEGACY_MASK;
    } else if (mapping_type == GUEST_LINUX_MAP_SHARED_VALIDATE &&
            (effective_flags & ~GUEST_LINUX_MAP_LEGACY_MASK) != 0) {
        return _EOPNOTSUPP;
    }
    if ((effective_flags & GUEST_LINUX_MAP_HUGETLB) != 0)
        return _EINVAL;
    int flags = fd_getflags((struct fd *) fd);
    if (flags < 0)
        return flags;
    int access_mode = flags & O_ACCMODE_;
    if (access_mode != O_RDONLY_ && access_mode != O_RDWR_)
        return _EACCES;
    if (shared && access_mode == O_RDONLY_ &&
            (request->protection & GUEST_LINUX_PROT_WRITE) != 0)
        return _EACCES;
    bool noexec = fd->mount != NULL &&
            (fd->mount->flags & MS_NOEXEC_) != 0;
    if ((request->protection & GUEST_LINUX_PROT_EXEC) != 0 && noexec)
        return _EPERM;

    *maximum_protection = GUEST_LINUX_PROT_MASK;
    if (shared && access_mode == O_RDONLY_)
        *maximum_protection &= ~GUEST_LINUX_PROT_WRITE;
    if (noexec)
        *maximum_protection &= ~GUEST_LINUX_PROT_EXEC;
    *need_writer = shared && access_mode == O_RDWR_;
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
    bool need_writer;
    int error = validate_file_mapping_request(
            request, fd, &maximum_protection, &need_writer);
    if (error < 0)
        return error;
    if (!S_ISREG(fd->type) || fd->mount == NULL || fd->inode == NULL ||
            !fd->ops->page_cacheable ||
            fd->ops->pread == NULL)
        return _ENODEV;
    if (need_writer && fd->ops->page_pwrite == NULL)
        return _ENODEV;
    if ((request->flags & GUEST_LINUX_MAP_GROWSDOWN) != 0)
        return _EINVAL;

    int acquire_error;
    struct guest_file_pager *pager = acquire_inode_pager(
            fd_retain(fd), need_writer, &acquire_error);
    if (pager == NULL) {
        assert(acquire_error < 0);
        return acquire_error;
    }
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

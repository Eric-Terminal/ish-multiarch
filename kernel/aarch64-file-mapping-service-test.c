#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-backing.h"
#include "kernel/aarch64-file-mapping-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 文件映射服务测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct page_write_gate {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool entered;
    bool allow;
};

struct file_probe {
    byte_t bytes[GUEST_MEMORY_PAGE_SIZE * 2];
    qword_t size;
    unsigned pread_calls;
    unsigned pwrite_calls;
    unsigned page_pwrite_calls;
    unsigned fsync_calls;
    unsigned fdatasync_calls;
    unsigned fstat_calls;
    unsigned close_calls;
    qword_t last_write_offset;
    size_t last_write_size;
    size_t page_pwrite_bytes;
    size_t page_pwrite_max;
    bool page_pwrite_zero;
    int pread_error;
    int pwrite_error;
    int page_pwrite_error;
    int fsync_error;
    int fdatasync_error;
    int fstat_error;
    struct page_write_gate *page_write_gate;
};

struct mapping_fixture {
    struct task task;
    struct tgroup group;
    struct mount mount;
};

static ssize_t probe_pread(struct fd *fd, void *buffer,
        size_t size, off_t offset) {
    struct file_probe *probe = fd->data;
    if (fd->ops->page_cacheable)
        assert(fd->inode != NULL &&
                lock_owned_by_current(&fd->inode->file_io_lock));
    probe->pread_calls++;
    if (probe->pread_error != 0)
        return probe->pread_error;
    if (offset < 0 || (qword_t) offset >= probe->size)
        return 0;
    qword_t available = probe->size - (qword_t) offset;
    if (available < size)
        size = (size_t) available;
    memcpy(buffer, probe->bytes + (size_t) offset, size);
    return (ssize_t) size;
}

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    ssize_t result = probe_pread(fd, buffer, size, (off_t) fd->offset);
    if (result > 0)
        fd->offset += (off_t_) result;
    return result;
}

static ssize_t probe_write_at(struct fd *fd, const void *buffer,
        size_t size, off_t offset, bool page_write) {
    struct file_probe *probe = fd->data;
    if (page_write)
        probe->page_pwrite_calls++;
    else
        probe->pwrite_calls++;
    int error = page_write ?
            probe->page_pwrite_error : probe->pwrite_error;
    if (error != 0)
        return error;
    if (page_write && probe->page_pwrite_zero)
        return 0;
    if (page_write && probe->page_pwrite_max != 0 &&
            size > probe->page_pwrite_max)
        size = probe->page_pwrite_max;
    if (offset < 0)
        return _EINVAL;
    qword_t positioned = !page_write &&
            (fd->flags & O_APPEND_) != 0 ?
            probe->size : (qword_t) offset;
    if (positioned > sizeof(probe->bytes) ||
            size > sizeof(probe->bytes) - (size_t) positioned)
        return _EFBIG;
    if (page_write && probe->page_write_gate != NULL) {
        struct page_write_gate *gate = probe->page_write_gate;
        assert(pthread_mutex_lock(&gate->lock) == 0);
        gate->entered = true;
        assert(pthread_cond_broadcast(&gate->changed) == 0);
        while (!gate->allow)
            assert(pthread_cond_wait(&gate->changed, &gate->lock) == 0);
        assert(pthread_mutex_unlock(&gate->lock) == 0);
    }
    memcpy(probe->bytes + (size_t) positioned, buffer, size);
    qword_t end = positioned + size;
    if (end > probe->size)
        probe->size = end;
    probe->last_write_offset = positioned;
    probe->last_write_size = size;
    if (page_write)
        probe->page_pwrite_bytes += size;
    return (ssize_t) size;
}

static ssize_t probe_pwrite(struct fd *fd, const void *buffer,
        size_t size, off_t offset) {
    return probe_write_at(fd, buffer, size, offset, false);
}

static ssize_t probe_page_pwrite(struct fd *fd, const void *buffer,
        size_t size, off_t offset) {
    assert(fd->inode != NULL &&
            lock_owned_by_current(&fd->inode->file_io_lock));
    return probe_write_at(fd, buffer, size, offset, true);
}

static void write_backing_byte(struct guest_page_backing *backing,
        size_t offset, byte_t value) {
    const struct guest_page_sync *sync =
            guest_page_backing_sync(backing);
    sync->ops->write_lock(sync->opaque);
    guest_page_backing_bytes(backing)[offset] = value;
    sync->ops->written(sync->opaque, offset, 1);
    sync->ops->write_unlock(sync->opaque);
}

static off_t_ probe_lseek(struct fd *fd, off_t_ offset, int whence) {
    if (whence == LSEEK_CUR)
        offset += fd->offset;
    else if (whence != LSEEK_SET)
        return _EINVAL;
    if (offset < 0)
        return _EINVAL;
    fd->offset = offset;
    return offset;
}

static int probe_close(struct fd *fd) {
    struct file_probe *probe = fd->data;
    probe->close_calls++;
    return 0;
}

static int probe_fsync(struct fd *fd) {
    struct file_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    if (fd->inode != NULL && fd->ops->page_cacheable)
        assert(lock_owned_by_current(&fd->inode->file_io_lock));
    probe->fsync_calls++;
    return probe->fsync_error;
}

static int probe_fdatasync(struct fd *fd) {
    struct file_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    if (fd->inode != NULL && fd->ops->page_cacheable)
        assert(lock_owned_by_current(&fd->inode->file_io_lock));
    probe->fdatasync_calls++;
    return probe->fdatasync_error;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct file_probe *probe = fd->data;
    probe->fstat_calls++;
    if (probe->fstat_error != 0)
        return probe->fstat_error;
    stat->mode = S_IFREG | 0644;
    stat->size = probe->size;
    stat->inode = fd->inode->number;
    stat->inode_device = fd->inode->device;
    return 0;
}

static const struct fd_ops cacheable_ops = {
    .page_cacheable = true,
    .pread = probe_pread,
    .pwrite = probe_pwrite,
    .page_pwrite = probe_page_pwrite,
    .fsync = probe_fsync,
    .fdatasync = probe_fdatasync,
    .close = probe_close,
};

static const struct fd_ops cacheable_without_page_write_ops = {
    .page_cacheable = true,
    .pread = probe_pread,
    .pwrite = probe_pwrite,
    .fsync = probe_fsync,
    .fdatasync = probe_fdatasync,
    .close = probe_close,
};

static const struct fd_ops cacheable_page_write_only_ops = {
    .page_cacheable = true,
    .pread = probe_pread,
    .page_pwrite = probe_page_pwrite,
    .fsync = probe_fsync,
    .fdatasync = probe_fdatasync,
    .close = probe_close,
};

static const struct fd_ops uncacheable_ops = {
    .pread = probe_pread,
    .close = probe_close,
};

static const struct fd_ops fallback_only_ops = {
    .page_cacheable = true,
    .read = probe_read,
    .lseek = probe_lseek,
    .close = probe_close,
};

static const struct fs_ops probe_fs = {
    .fstat = probe_fstat,
};

static bool fixture_init(struct mapping_fixture *fixture, int mount_flags) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(4);
    if (IS_ERR(fixture->task.files))
        return false;
    fixture->mount = (struct mount) {
        .flags = mount_flags,
        .fs = &probe_fs,
    };
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct mapping_fixture *fixture) {
    fdtable_release(fixture->task.files);
    fixture->task.files = NULL;
    current = NULL;
    assert(fixture->mount.refcount == 0);
}

static fd_t install_probe_fd(struct mapping_fixture *fixture,
        struct file_probe *probe, const struct fd_ops *ops,
        mode_t_ type, int flags, qword_t device, ino_t inode_number) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return _ENOMEM;
    fd->data = probe;
    fd->type = type;
    fd->flags = (unsigned) flags;
    fd->mount = &fixture->mount;
    mount_retain(&fixture->mount);
    fd->inode = inode_get(&fixture->mount, device, inode_number);
    return f_install_task(&fixture->task, fd, 0);
}

static sdword_t open_mapping_task(struct task *task,
        qword_t fd, qword_t length, qword_t protection,
        qword_t flags, qword_t offset,
        struct guest_linux_file_mapping *mapping) {
    if ((offset & GUEST_MEMORY_PAGE_MASK) != 0)
        return _EINVAL;
    const struct guest_linux_file_mapping_context context = {
        .task_opaque = task,
    };
    const struct guest_linux_file_mapping_request request = {
        .fd = fd,
        .offset = offset,
        .length = length,
        .protection = protection,
        .flags = flags,
    };
    struct guest_linux_file_mapping_handle handle = {0};
    sdword_t result = ish_aarch64_linux_file_mapping_service.acquire(
            &context, fd, &handle);
    if (result != 0)
        return result;
    result = ish_aarch64_linux_file_mapping_service.open(
            &handle, &request, mapping);
    ish_aarch64_linux_file_mapping_service.release(&handle);
    assert(handle.opaque == NULL);
    return result;
}

static sdword_t open_mapping(struct mapping_fixture *fixture,
        qword_t fd, qword_t length, qword_t protection,
        qword_t flags, qword_t offset,
        struct guest_linux_file_mapping *mapping) {
    return open_mapping_task(&fixture->task, fd, length,
            protection, flags, offset, mapping);
}

static struct timespec deadline_after_milliseconds(long milliseconds) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    return deadline;
}

static bool wait_for_page_write(
        struct page_write_gate *gate, long milliseconds) {
    struct timespec deadline = deadline_after_milliseconds(milliseconds);
    assert(pthread_mutex_lock(&gate->lock) == 0);
    while (!gate->entered) {
        int result = pthread_cond_timedwait(
                &gate->changed, &gate->lock, &deadline);
        if (result == ETIMEDOUT)
            break;
        assert(result == 0);
    }
    bool entered = gate->entered;
    assert(pthread_mutex_unlock(&gate->lock) == 0);
    return entered;
}

static bool task_waits_on_condition(
        struct task *task, cond_t *condition,
        lock_t *condition_lock, long milliseconds) {
    struct timespec deadline = deadline_after_milliseconds(milliseconds);
    for (;;) {
        lock(&task->waiting_cond_lock);
        bool waiting = task->waiting_cond == condition &&
                task->waiting_lock == condition_lock;
        unlock(&task->waiting_cond_lock);
        if (waiting)
            return true;

        struct timespec now;
        assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                now.tv_nsec >= deadline.tv_nsec))
            return false;
        sched_yield();
    }
}

struct pager_release_thread {
    struct guest_file_pager *pager;
    atomic_bool finished;
};

static void *release_pager_thread(void *opaque) {
    struct pager_release_thread *thread = opaque;
    guest_file_pager_release(thread->pager);
    atomic_store_explicit(&thread->finished, true, memory_order_release);
    return NULL;
}

struct mapping_open_thread {
    struct task task;
    qword_t fd;
    struct guest_linux_file_mapping mapping;
    sdword_t result;
    atomic_bool started;
    atomic_bool finished;
};

static void *open_mapping_thread(void *opaque) {
    struct mapping_open_thread *thread = opaque;
    current = &thread->task;
    atomic_store_explicit(&thread->started, true, memory_order_release);
    thread->result = open_mapping_task(&thread->task, thread->fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &thread->mapping);
    atomic_store_explicit(&thread->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static int check_error_contract(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化错误语义夹具");
    struct file_probe readable = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe readwrite = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe no_page_write = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe page_write_only = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe write_only = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe unsupported = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe fallback_only = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe mountless = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t readable_fd = install_probe_fd(&fixture, &readable,
            &cacheable_ops, S_IFREG, O_RDONLY_, 1, 10);
    fd_t readwrite_fd = install_probe_fd(&fixture, &readwrite,
            &cacheable_ops, S_IFREG, O_RDWR_, 1, 14);
    fd_t no_page_write_fd = install_probe_fd(&fixture, &no_page_write,
            &cacheable_without_page_write_ops,
            S_IFREG, O_RDWR_, 1, 15);
    fd_t page_write_only_fd = install_probe_fd(&fixture, &page_write_only,
            &cacheable_page_write_only_ops,
            S_IFREG, O_RDWR_, 1, 16);
    fd_t write_only_fd = install_probe_fd(&fixture, &write_only,
            &cacheable_ops, S_IFREG, O_WRONLY_, 1, 11);
    fd_t unsupported_fd = install_probe_fd(&fixture, &unsupported,
            &uncacheable_ops, S_IFREG, O_RDONLY_, 1, 12);
    fd_t fallback_only_fd = install_probe_fd(&fixture, &fallback_only,
            &fallback_only_ops, S_IFREG, O_RDONLY_, 1, 13);
    struct fd *mountless_file = fd_create(&uncacheable_ops);
    CHECK(mountless_file != NULL, "创建无 mount 文件描述符");
    mountless_file->data = &mountless;
    mountless_file->type = S_IFREG;
    mountless_file->flags = O_RDONLY_;
    fd_t mountless_fd = f_install_task(
            &fixture.task, mountless_file, 0);
    CHECK(readable_fd >= 0 && readwrite_fd >= 0 &&
            no_page_write_fd >= 0 && page_write_only_fd >= 0 &&
            write_only_fd >= 0 &&
            unsupported_fd >= 0 && fallback_only_fd >= 0 &&
            mountless_fd >= 0,
            "安装错误语义文件描述符");

    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, UINT64_C(99), 0,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0,
            &mapping) == _EBADF,
            "有效 offset 下无效 fd 先返回 EBADF");
    CHECK(open_mapping(&fixture, UINT64_C(99), GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 1,
            &mapping) == _EINVAL,
            "未对齐 offset 先于无效 fd 返回 EINVAL");
    CHECK(open_mapping(&fixture, (qword_t) write_only_fd, 0,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0,
            &mapping) == _EINVAL,
            "有效 fd 的零长度先于访问模式返回 EINVAL");
    CHECK(open_mapping(&fixture, (qword_t) write_only_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _EACCES,
            "MAP_PRIVATE 拒绝只写 fd");
    CHECK(open_mapping(&fixture, (qword_t) write_only_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_GROWSDOWN,
            0, &mapping) == _EACCES,
            "向下增长标志不覆盖普通文件读取权限错误");
    CHECK(open_mapping(&fixture, (qword_t) readable_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE,
            (qword_t) INT64_MAX & ~GUEST_MEMORY_PAGE_MASK,
            &mapping) == _EOVERFLOW,
            "文件映射范围越过 signed 文件上限返回 EOVERFLOW");
    CHECK(open_mapping(&fixture, (qword_t) unsupported_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _ENODEV,
            "未声明 page-cache 能力的普通文件返回 ENODEV");
    CHECK(open_mapping(&fixture, (qword_t) unsupported_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_GROWSDOWN,
            0, &mapping) == _ENODEV,
            "向下增长标志不覆盖普通文件 provider 能力错误");
    CHECK(open_mapping(&fixture, (qword_t) fallback_only_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _ENODEV,
            "分页 provider 必须提供不改描述偏移的 positioned read");
    CHECK(open_mapping(&fixture, (qword_t) mountless_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _ENODEV,
            "无 mount 文件描述符安全返回 ENODEV");

    CHECK(open_mapping(&fixture, (qword_t) readable_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0 &&
            mapping.pager != NULL &&
            mapping.maximum_protection == GUEST_LINUX_PROT_MASK,
            "只读 fd 允许可写私有映射并保留最大权限");
    guest_file_pager_release(mapping.pager);

    CHECK(open_mapping(&fixture, (qword_t) readable_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == _EACCES,
            "只读 fd 拒绝当前可写共享映射");
    CHECK(open_mapping(&fixture, (qword_t) readable_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == 0 &&
            mapping.maximum_protection ==
                    (GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_EXEC),
            "只读 fd 的共享映射移除后续写权限");
    guest_file_pager_release(mapping.pager);

    CHECK(open_mapping(&fixture, (qword_t) no_page_write_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == _ENODEV,
            "读写共享映射要求精确 pager 写回能力");
    CHECK(open_mapping(&fixture, (qword_t) page_write_only_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == 0,
            "pager 专用定位写无需普通 write 或 pwrite");
    struct guest_page_backing *page_write_only_page = NULL;
    CHECK(guest_file_pager_get_page(mapping.pager, 0,
            &page_write_only_page) == GUEST_FILE_PAGE_OK &&
            page_write_only_page != NULL,
            "载入只有 pager 写能力的共享页");
    write_backing_byte(page_write_only_page, 17, 0xd4);
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_OK &&
            page_write_only.page_pwrite_calls == 1 &&
            page_write_only.fdatasync_calls == 1 &&
            page_write_only.bytes[17] == 0xd4,
            "只有 page_pwrite 的 provider 可完成脏页写回和 data sync");
    guest_page_backing_release(page_write_only_page);
    guest_file_pager_release(mapping.pager);
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == 0 &&
            mapping.maximum_protection == GUEST_LINUX_PROT_MASK,
            "读写 fd 的只读共享映射保留后续写权限");
    guest_file_pager_release(mapping.pager);

    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED_VALIDATE, 0, &mapping) == 0,
            "无扩展的 MAP_SHARED_VALIDATE 共享映射可建立");
    guest_file_pager_release(mapping.pager);

    const qword_t compatible_legacy_flags =
            GUEST_LINUX_MAP_DENYWRITE |
            GUEST_LINUX_MAP_EXECUTABLE |
            GUEST_LINUX_MAP_LOCKED |
            GUEST_LINUX_MAP_NORESERVE |
            GUEST_LINUX_MAP_POPULATE |
            GUEST_LINUX_MAP_NONBLOCK |
            GUEST_LINUX_MAP_STACK |
            GUEST_LINUX_MAP_UNINITIALIZED |
            GUEST_LINUX_MAP_HUGE_2MB |
            GUEST_LINUX_MAP_HUGE_1GB;
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED_VALIDATE | compatible_legacy_flags,
            0, &mapping) == 0,
            "MAP_SHARED_VALIDATE 接受固定 Linux 历史兼容位");
    guest_file_pager_release(mapping.pager);

    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_SYNC,
            0, &mapping) == 0,
            "普通 MAP_SHARED 静默忽略不支持的 MAP_SYNC");
    guest_file_pager_release(mapping.pager);
    const qword_t unknown_extension = UINT64_C(1) << 62;
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED | unknown_extension,
            0, &mapping) == 0,
            "普通 MAP_SHARED 静默忽略未知非历史扩展位");
    guest_file_pager_release(mapping.pager);
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED_VALIDATE | GUEST_LINUX_MAP_SYNC,
            0, &mapping) == _EOPNOTSUPP,
            "MAP_SHARED_VALIDATE 精确拒绝不支持的 MAP_SYNC");
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED_VALIDATE | unknown_extension,
            0, &mapping) == _EOPNOTSUPP,
            "MAP_SHARED_VALIDATE 精确拒绝未知扩展位");
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED_VALIDATE |
                    GUEST_LINUX_MAP_FIXED_NOREPLACE,
            0, &mapping) == _EOPNOTSUPP,
            "MAP_SHARED_VALIDATE 拒绝不在历史掩码内的 NOREPLACE");
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED_VALIDATE |
                    GUEST_LINUX_MAP_GROWSDOWN,
            0, &mapping) == _EINVAL,
            "普通文件拒绝向下增长的共享映射");
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_HUGETLB,
            0, &mapping) == _EINVAL,
            "普通文件拒绝 hugetlb 共享映射");
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_SYNC,
            0, &mapping) == 0,
            "MAP_PRIVATE 接受没有私有同步含义的 MAP_SYNC");
    guest_file_pager_release(mapping.pager);
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | unknown_extension,
            0, &mapping) == 0,
            "MAP_PRIVATE 忽略未知扩展位");
    guest_file_pager_release(mapping.pager);
    CHECK(open_mapping(&fixture, (qword_t) readwrite_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            0, 0, &mapping) == _EINVAL,
            "缺失文件映射类型返回 EINVAL");
    fixture_destroy(&fixture);
    CHECK(readable.close_calls == 1 && readwrite.close_calls == 1 &&
            no_page_write.close_calls == 1 &&
            page_write_only.close_calls == 1 &&
            write_only.close_calls == 1 && unsupported.close_calls == 1 &&
            fallback_only.close_calls == 1 && mountless.close_calls == 1,
            "错误路径和成功路径平衡 fd 生命周期");
    return 0;
}

static int check_noexec_contract(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, MS_NOEXEC_), "初始化 noexec 夹具");
    struct file_probe probe = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t fd = install_probe_fd(&fixture, &probe,
            &cacheable_ops, S_IFREG, O_RDONLY_, 2, 20);
    CHECK(fd >= 0, "安装 noexec 文件描述符");

    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _EPERM,
            "noexec mount 拒绝当前 PROT_EXEC");
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_GROWSDOWN,
            0, &mapping) == _EPERM,
            "向下增长标志不覆盖 noexec 权限错误");
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0 &&
            mapping.maximum_protection ==
                    (GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE),
            "noexec mount 同时移除后续 mprotect 的执行权限");
    guest_file_pager_release(mapping.pager);
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == 0 &&
            mapping.maximum_protection == GUEST_LINUX_PROT_READ,
            "noexec 只读共享映射同时移除后续写与执行权限");
    guest_file_pager_release(mapping.pager);
    fixture_destroy(&fixture);
    return 0;
}

static int check_host_shared_mapping_exclusion(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0),
            "初始化跨 ABI host 共享映射互斥夹具");
    struct file_probe probe = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t fd = install_probe_fd(&fixture, &probe,
            &cacheable_ops, S_IFREG, O_RDWR_, 2, 20);
    CHECK(fd >= 0, "安装跨 ABI 互斥描述符");
    struct inode_data *inode = f_get_task(&fixture.task, fd)->inode;

    CHECK(inode_try_begin_host_shared_mapping(inode),
            "先登记 i386 host 共享映射 token");
    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_GROWSDOWN,
            0, &mapping) == _EINVAL,
            "文件标志错误先于跨 ABI 活跃后备冲突返回");
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _EBUSY &&
            mapping.pager == NULL,
            "host 共享映射阻止建立 AArch64 私有 pager");
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == _EBUSY &&
            mapping.pager == NULL,
            "host 共享映射阻止建立 AArch64 共享 pager");
    lock(&inode->lock);
    CHECK(inode->host_shared_mapping_count == 1 &&
            inode->file_pager == NULL &&
            inode->file_pager_context == NULL,
            "EBUSY 路径不安装 pager 或消费 host token");
    unlock(&inode->lock);

    inode_end_host_shared_mapping(inode);
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0 &&
            mapping.pager != NULL,
            "host token 归还后可以建立 AArch64 pager");
    CHECK(!inode_try_begin_host_shared_mapping(inode),
            "活跃 AArch64 pager 阻止登记 host 共享映射");
    guest_file_pager_release(mapping.pager);

    CHECK(inode_try_begin_host_shared_mapping(inode),
            "AArch64 pager 完整释放后可以重新登记 host token");
    inode_end_host_shared_mapping(inode);
    fixture_destroy(&fixture);
    CHECK(probe.close_calls == 1,
            "跨 ABI 互斥夹具平衡描述符生命周期");
    return 0;
}

static int check_inode_cache_and_lifetime(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化 inode 共享夹具");
    struct file_probe first = {.size = GUEST_MEMORY_PAGE_SIZE + 3};
    struct file_probe second = {.size = GUEST_MEMORY_PAGE_SIZE + 3};
    for (size_t index = 0; index < sizeof(first.bytes); index++)
        first.bytes[index] = second.bytes[index] = (byte_t) (index ^ 0x5a);
    fd_t first_fd = install_probe_fd(&fixture, &first,
            &cacheable_ops, S_IFREG, O_RDONLY_, 3, 30);
    fd_t second_fd = install_probe_fd(&fixture, &second,
            &cacheable_ops, S_IFREG, O_RDWR_, 3, 30);
    CHECK(first_fd >= 0 && second_fd >= 0,
            "安装同 inode 的独立描述符");
    struct fd *installed = f_get_task(&fixture.task, first_fd);
    struct fd *writer = f_get_task(&fixture.task, second_fd);
    struct inode_data *inode = installed->inode;

    struct guest_linux_file_mapping left = {0};
    struct guest_linux_file_mapping right = {0};
    CHECK(open_mapping(&fixture, (qword_t) first_fd,
            GUEST_MEMORY_PAGE_SIZE * 2, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &left) == 0,
            "从第一个描述符获取 pager");
    CHECK(open_mapping(&fixture, (qword_t) second_fd,
            GUEST_MEMORY_PAGE_SIZE * 2, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &right) == 0,
            "从读写描述符升级同 inode 的共享 pager");
    CHECK(left.pager == right.pager && inode->file_pager == left.pager &&
            inode->file_pager_context != NULL,
            "读写升级保留 pager 身份并成对登记弱槽");

    struct guest_page_backing *first_page = NULL;
    struct guest_page_backing *same_page = NULL;
    CHECK(guest_file_pager_get_page(left.pager, 0, &first_page) ==
            GUEST_FILE_PAGE_OK &&
            guest_file_pager_get_page(right.pager, 0, &same_page) ==
                    GUEST_FILE_PAGE_OK &&
            first_page == same_page && first.pread_calls == 1 &&
            second.pread_calls == 0,
            "共享 pager 对同一偏移只执行一次 provider 读取");

    write_backing_byte(first_page, 17, 0xd1);
    byte_t observed = 0;
    CHECK(file_pread_fd(writer, &observed, 1, 17) == 1 &&
            observed == 0xd1 && second.pread_calls == 1,
            "普通 pread 用驻留映射脏页覆盖底层旧数据");
    const byte_t committed = 0xc2;
    CHECK(file_pwrite_fd(writer, &committed, 1, 18) == 1 &&
            second.pwrite_calls == 1 &&
            second.last_write_offset == 18 &&
            guest_page_backing_bytes(first_page)[18] == committed,
            "普通 pwrite 成功后合并同 inode 驻留页");
    guest_page_backing_release(same_page);
    guest_page_backing_release(first_page);

    CHECK(f_close_task(&fixture.task, first_fd) == 0 &&
            first.close_calls == 0,
            "关闭原 guest fd 后 pager 继续保活 provider fd");
    struct file_probe replacement = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t reused = install_probe_fd(&fixture, &replacement,
            &cacheable_ops, S_IFREG, O_RDONLY_, 4, 40);
    CHECK(reused == first_fd, "复用已经关闭的 guest fd 槽");

    struct guest_page_backing *tail = NULL;
    CHECK(guest_file_pager_get_page(left.pager,
            GUEST_MEMORY_PAGE_SIZE, &tail) == GUEST_FILE_PAGE_OK,
            "原 fd 关闭并复用后仍可读取后续文件页");
    const byte_t *tail_bytes = guest_page_backing_bytes(tail);
    CHECK(tail_bytes[0] == first.bytes[GUEST_MEMORY_PAGE_SIZE] &&
            tail_bytes[2] == first.bytes[GUEST_MEMORY_PAGE_SIZE + 2] &&
            tail_bytes[3] == 0 &&
            tail_bytes[GUEST_MEMORY_PAGE_SIZE - 1] == 0,
            "文件尾页保留有效前缀并补零");

    CHECK(fd_setflags(writer, O_APPEND_) == 0,
            "为 writer 打开 O_APPEND 测试精确写回");
    write_backing_byte(tail, 1, 0xee);
    second.page_pwrite_max = 1;
    CHECK(guest_file_pager_sync_range(left.pager,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE) ==
                    GUEST_FILE_SYNC_OK &&
            second.page_pwrite_calls == 3 &&
            second.page_pwrite_bytes == 3 &&
            second.pwrite_calls == 1 &&
            second.last_write_offset == GUEST_MEMORY_PAGE_SIZE + 2 &&
            second.last_write_size == 1 &&
            second.bytes[GUEST_MEMORY_PAGE_SIZE + 1] == 0xee &&
            second.size == GUEST_MEMORY_PAGE_SIZE + 3,
            "pager 写回循环短写、忽略 O_APPEND 且只提交 EOF 前缀");
    second.page_pwrite_max = 0;
    guest_page_backing_release(tail);

    guest_file_pager_release(left.pager);
    CHECK(inode->file_pager != NULL &&
            inode->file_pager_context != NULL && first.close_calls == 0,
            "仍有映射引用时不摘除 inode 弱槽");
    guest_file_pager_release(right.pager);
    CHECK(inode->file_pager == NULL &&
            inode->file_pager_context == NULL && first.close_calls == 1,
            "最后映射释放后成对摘弱槽并关闭 provider fd");
    fixture_destroy(&fixture);
    CHECK(second.close_calls == 1 && replacement.close_calls == 1,
            "独立描述符和复用槽各自恰好关闭一次");
    return 0;
}

static int check_fault_translation_source(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化读取故障夹具");
    struct file_probe probe = {
        .size = GUEST_MEMORY_PAGE_SIZE,
        .fstat_error = _EIO,
    };
    fd_t fd = install_probe_fd(&fixture, &probe,
            &cacheable_ops, S_IFREG, O_RDONLY_, 5, 50);
    CHECK(fd >= 0, "安装读取故障描述符");
    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0,
            "映射建立时不执行 fstat 或读取");
    CHECK(probe.fstat_calls == 0 && probe.pread_calls == 0,
            "生产 mapping service 保持 lazy I/O");
    struct guest_page_backing *page = NULL;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_IO_ERROR && page == NULL &&
            probe.fstat_calls == 1 && probe.pread_calls == 0,
            "fault 时 fstat 错误转换为 pager I/O 故障");

    probe.fstat_error = 0;
    probe.pread_error = _EIO;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_IO_ERROR && page == NULL &&
            probe.fstat_calls == 2 && probe.pread_calls == 1,
            "fault 时 positioned read 错误转换为 pager I/O 故障");
    probe.pread_error = 0;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_OK && page != NULL &&
            probe.fstat_calls == 3 && probe.pread_calls == 2,
            "provider I/O 失败不缓存并允许后续 fault 重试");
    guest_page_backing_release(page);
    guest_file_pager_release(mapping.pager);
    fixture_destroy(&fixture);
    return 0;
}

static int check_writeback_errors_and_retry(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化写回故障夹具");
    struct file_probe probe = {.size = GUEST_MEMORY_PAGE_SIZE};
    for (size_t index = 0; index < GUEST_MEMORY_PAGE_SIZE; index++)
        probe.bytes[index] = (byte_t) index;
    fd_t fd = install_probe_fd(&fixture, &probe,
            &cacheable_ops, S_IFREG, O_RDWR_, 6, 60);
    CHECK(fd >= 0, "安装写回故障描述符");
    CHECK(file_sync_task(&fixture.task, 99, false) == _EBADF,
            "文件同步先拒绝无效描述符");
    CHECK(file_sync_task(&fixture.task, fd, true) == 0 &&
            probe.fdatasync_calls == 1 && probe.fsync_calls == 0,
            "无 pager 时 fdatasync 直接进入 data-only provider");

    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED, 0, &mapping) == 0,
            "建立可写共享映射");
    struct guest_page_backing *page = NULL;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_OK && page != NULL,
            "载入待写回共享页");
    write_backing_byte(page, 9, 0xf1);

    byte_t snapshot[GUEST_MEMORY_PAGE_SIZE];
    qword_t generation;
    probe.page_pwrite_zero = true;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_IO_ERROR &&
            probe.page_pwrite_calls == 1 &&
            probe.page_pwrite_bytes == 0 &&
            guest_page_backing_copy_dirty(page, snapshot, &generation),
            "provider 零进展转换为 I/O 错误并保留脏页");

    probe.page_pwrite_zero = false;
    probe.page_pwrite_error = _ENOSPC;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_IO_ERROR &&
            probe.page_pwrite_calls == 2 &&
            guest_page_backing_copy_dirty(page, snapshot, &generation),
            "provider 写错误保留脏页供重试");

    probe.page_pwrite_error = 0;
    probe.page_pwrite_max = 1024;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_OK &&
            probe.page_pwrite_calls == 6 &&
            probe.page_pwrite_bytes == GUEST_MEMORY_PAGE_SIZE &&
            probe.fdatasync_calls == 2 &&
            probe.bytes[9] == 0xf1 &&
            !guest_page_backing_copy_dirty(page, snapshot, &generation),
            "短写循环后提交整页、data sync 并清除同世代脏状态");

    probe.fdatasync_error = _ENOSPC;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_IO_ERROR &&
            probe.page_pwrite_calls == 6 &&
            probe.fdatasync_calls == 3,
            "底层 data sync 错误从显式 range sync 返回");
    probe.fdatasync_error = 0;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_OK &&
            probe.page_pwrite_calls == 6 &&
            probe.fdatasync_calls == 4,
            "data sync 错误可在不重复写页时重试");

    write_backing_byte(page, 10, 0xf2);
    probe.fstat_error = _EIO;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_IO_ERROR &&
            probe.page_pwrite_calls == 6 &&
            guest_page_backing_copy_dirty(page, snapshot, &generation),
            "写回前 fstat 失败不提交或清除脏页");

    probe.fstat_error = 0;
    probe.size = 0;
    CHECK(guest_file_pager_sync_range(mapping.pager, 0,
            GUEST_MEMORY_PAGE_SIZE) == GUEST_FILE_SYNC_OK &&
            probe.page_pwrite_calls == 6 && probe.size == 0 &&
            probe.fdatasync_calls == 5 &&
            !guest_page_backing_copy_dirty(page, snapshot, &generation),
            "页面起点已越过当前 EOF 时成功丢弃脏页且不扩展文件");

    probe.size = GUEST_MEMORY_PAGE_SIZE;
    write_backing_byte(page, 11, 0xa3);
    CHECK(file_sync_task(&fixture.task, fd, true) == 0 &&
            probe.page_pwrite_calls == 10 &&
            probe.fdatasync_calls == 6 && probe.fsync_calls == 0 &&
            probe.bytes[11] == 0xa3,
            "fdatasync 先写回 inode pager 再完成 data durability");
    write_backing_byte(page, 12, 0xb4);
    CHECK(file_sync_task(&fixture.task, fd, false) == 0 &&
            probe.page_pwrite_calls == 14 &&
            probe.fdatasync_calls == 7 && probe.fsync_calls == 1 &&
            probe.bytes[12] == 0xb4,
            "fsync 先完成 pager data sync 再执行完整元数据同步");

    probe.fsync_error = _EROFS;
    CHECK(file_sync_task(&fixture.task, fd, false) == _EROFS &&
            probe.fdatasync_calls == 8 && probe.fsync_calls == 2,
            "完整 fsync 原样传播当前 fd 的 provider 错误");
    probe.fsync_error = 0;

    guest_page_backing_release(page);
    guest_file_pager_release(mapping.pager);
    fixture_destroy(&fixture);
    CHECK(probe.close_calls == 1, "写回重试夹具平衡 fd 生命周期");
    return 0;
}

static int check_draining_weak_slot_and_inode_isolation(void) {
    struct page_write_gate gate;
    CHECK(pthread_mutex_init(&gate.lock, NULL) == 0 &&
            pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化最终写回门禁");
    gate.entered = false;
    gate.allow = false;

    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化 pager 析构竞态夹具");

    struct file_probe blocked = {
        .size = GUEST_MEMORY_PAGE_SIZE,
        .page_write_gate = &gate,
    };
    struct file_probe successor = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe independent = {.size = GUEST_MEMORY_PAGE_SIZE};
    memset(blocked.bytes, 0x31, sizeof(blocked.bytes));
    memset(successor.bytes, 0x48, sizeof(successor.bytes));
    memset(independent.bytes, 0x62, sizeof(independent.bytes));
    fd_t blocked_fd = install_probe_fd(&fixture, &blocked,
            &cacheable_ops, S_IFREG, O_RDWR_, 7, 70);
    fd_t successor_fd = install_probe_fd(&fixture, &successor,
            &cacheable_ops, S_IFREG, O_RDWR_, 7, 70);
    fd_t independent_fd = install_probe_fd(&fixture, &independent,
            &cacheable_ops, S_IFREG, O_RDWR_, 7, 71);
    CHECK(blocked_fd >= 0 && successor_fd >= 0 && independent_fd >= 0,
            "安装同 inode 双描述符与无关 inode 描述符");

    struct guest_linux_file_mapping old_mapping = {0};
    struct guest_linux_file_mapping independent_mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) blocked_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED, 0, &old_mapping) == 0 &&
            open_mapping(&fixture, (qword_t) independent_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED, 0, &independent_mapping) == 0,
            "建立阻塞 inode 与无关 inode 的共享映射");

    struct fd *installed = f_get_task(&fixture.task, blocked_fd);
    struct inode_data *inode = installed->inode;
    struct guest_file_pager *old_pager = old_mapping.pager;
    void *old_context;
    lock(&inode->lock);
    old_context = inode->file_pager_context;
    CHECK(inode->file_pager == old_pager && old_context != NULL,
            "最终释放前登记旧 pager 弱槽");
    unlock(&inode->lock);

    struct guest_page_backing *dirty = NULL;
    CHECK(guest_file_pager_get_page(old_pager, 0, &dirty) ==
            GUEST_FILE_PAGE_OK && dirty != NULL,
            "载入最终析构需要写回的页面");
    write_backing_byte(dirty, 23, 0xa7);
    guest_page_backing_release(dirty);

    struct pager_release_thread release = {
        .pager = old_pager,
        .finished = ATOMIC_VAR_INIT(false),
    };
    pthread_t release_thread;
    CHECK(pthread_create(&release_thread, NULL,
            release_pager_thread, &release) == 0 &&
            wait_for_page_write(&gate, 3000),
            "最后一个强引用进入受控最终写回");

    struct mapping_open_thread reopen = {
        .fd = (qword_t) successor_fd,
        .started = ATOMIC_VAR_INIT(false),
        .finished = ATOMIC_VAR_INIT(false),
    };
    reopen.task.group = &fixture.group;
    reopen.task.files = fixture.task.files;
    lock_init(&reopen.task.waiting_cond_lock);
    pthread_t reopen_thread;
    CHECK(pthread_create(&reopen_thread, NULL,
            open_mapping_thread, &reopen) == 0,
            "并发启动同 inode 的新映射请求");
    while (!atomic_load_explicit(&reopen.started, memory_order_acquire))
        sched_yield();
    CHECK(task_waits_on_condition(&reopen.task,
            &inode->file_pager_changed, &inode->lock, 3000),
            "新映射等待 refs 为零的旧 pager 完整摘槽");

    lock(&inode->lock);
    CHECK(inode->file_pager == old_pager &&
            inode->file_pager_context == old_context &&
            !guest_file_pager_try_retain(old_pager),
            "draining 期间不清除、替换或复活旧 pager 弱槽");
    unlock(&inode->lock);
    CHECK(!atomic_load_explicit(&release.finished, memory_order_acquire) &&
            !atomic_load_explicit(&reopen.finished, memory_order_acquire),
            "最终写回放行前析构与新映射都未越过门禁");
    CHECK(!inode_try_begin_host_shared_mapping(inode),
            "refs 归零但弱槽仍在 drain 时拒绝 host 共享映射");

    struct guest_page_backing *independent_page = NULL;
    CHECK(guest_file_pager_get_page(independent_mapping.pager, 0,
            &independent_page) == GUEST_FILE_PAGE_OK &&
            independent_page != NULL &&
            guest_page_backing_bytes(independent_page)[0] == 0x62,
            "阻塞 inode 写回时无关 inode 的生产 fault 仍可前进");
    guest_page_backing_release(independent_page);

    CHECK(pthread_mutex_lock(&gate.lock) == 0,
            "取得最终写回门禁");
    gate.allow = true;
    CHECK(pthread_cond_broadcast(&gate.changed) == 0 &&
            pthread_mutex_unlock(&gate.lock) == 0,
            "放行旧 pager 最终写回");
    CHECK(pthread_join(release_thread, NULL) == 0 &&
            pthread_join(reopen_thread, NULL) == 0,
            "等待旧 pager 析构和新映射完成");
    CHECK(reopen.result == 0 && reopen.mapping.pager != NULL &&
            atomic_load_explicit(&release.finished, memory_order_acquire) &&
            atomic_load_explicit(&reopen.finished, memory_order_acquire) &&
            blocked.page_pwrite_calls == 1 && blocked.bytes[23] == 0xa7,
            "旧写回完成后新映射才建立");
    CHECK(reopen.task.waiting_cond == NULL &&
            reopen.task.waiting_lock == NULL,
            "新映射醒来后清除 task 等待登记");

    lock(&inode->lock);
    CHECK(inode->file_pager == reopen.mapping.pager &&
            inode->file_pager_context != NULL,
            "旧弱槽摘除后原子登记新 pager 对");
    unlock(&inode->lock);

    struct guest_page_backing *successor_page = NULL;
    CHECK(guest_file_pager_get_page(reopen.mapping.pager, 0,
            &successor_page) == GUEST_FILE_PAGE_OK &&
            successor_page != NULL &&
            guest_page_backing_bytes(successor_page)[0] == 0x48 &&
            successor.pread_calls == 1,
            "新 pager 从等待者描述符重新建立 provider");
    guest_page_backing_release(successor_page);

    guest_file_pager_release(reopen.mapping.pager);
    guest_file_pager_release(independent_mapping.pager);
    fixture_destroy(&fixture);
    CHECK(blocked.close_calls == 1 && successor.close_calls == 1 &&
            independent.close_calls == 1,
            "析构竞态夹具平衡描述符生命周期");
    CHECK(pthread_cond_destroy(&gate.changed) == 0 &&
            pthread_mutex_destroy(&gate.lock) == 0 &&
            pthread_mutex_destroy(&reopen.task.waiting_cond_lock.m) == 0,
            "销毁最终写回与等待门禁");
    return 0;
}

int main(void) {
    if (check_error_contract() != 0)
        return 1;
    if (check_noexec_contract() != 0)
        return 1;
    if (check_host_shared_mapping_exclusion() != 0)
        return 1;
    if (check_inode_cache_and_lifetime() != 0)
        return 1;
    if (check_fault_translation_source() != 0)
        return 1;
    if (check_writeback_errors_and_retry() != 0)
        return 1;
    if (check_draining_weak_slot_and_inode_isolation() != 0)
        return 1;
    return 0;
}

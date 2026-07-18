#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/task.h"

#define TEST_BASE UINT32_C(0x51000000)

#define TEST_MS_ASYNC 1
#define TEST_MS_INVALIDATE 2
#define TEST_MS_SYNC 4

struct sync_probe {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned fsync_calls;
    unsigned fdatasync_calls;
    int fdatasync_error;
    bool block;
    bool entered;
    bool allow;
};

struct msync_thread {
    struct task *task;
    addr_t address;
    dword_t length;
    int_t result;
};

static void check_pthread(int result) {
    assert(result == 0);
}

static void probe_init(struct sync_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    check_pthread(pthread_mutex_init(&probe->lock, NULL));
    check_pthread(pthread_cond_init(&probe->changed, NULL));
}

static void probe_destroy(struct sync_probe *probe) {
    check_pthread(pthread_cond_destroy(&probe->changed));
    check_pthread(pthread_mutex_destroy(&probe->lock));
}

static int probe_fsync(struct fd *fd) {
    struct sync_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->fsync_calls++;
    check_pthread(pthread_mutex_unlock(&probe->lock));
    return 0;
}

static int probe_fdatasync(struct fd *fd) {
    struct sync_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->fdatasync_calls++;
    probe->entered = true;
    check_pthread(pthread_cond_broadcast(&probe->changed));
    while (probe->block && !probe->allow)
        check_pthread(pthread_cond_wait(&probe->changed, &probe->lock));
    int error = probe->fdatasync_error;
    check_pthread(pthread_mutex_unlock(&probe->lock));
    return error;
}

static const struct fd_ops sync_ops = {
    .fsync = probe_fsync,
    .fdatasync = probe_fdatasync,
};

static const struct fd_ops unsupported_ops = {0};

static struct fd *make_fd(
        const struct fd_ops *ops, struct sync_probe *probe,
        unsigned flags) {
    struct fd *fd = fd_create(ops);
    assert(fd != NULL);
    fd->data = probe;
    fd->flags = flags;
    fd->type = S_IFREG;
    return fd;
}

static void map_file(struct task *task, addr_t address,
        pages_t pages, unsigned flags, struct fd *fd,
        qword_t file_offset) {
    size_t size = (size_t) pages * PAGE_SIZE;
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);

    write_wrlock(&task->mem->lock);
    int result = pt_map(task->mem, PAGE(address), pages,
            memory, 0, flags | P_FILE_BACKED);
    if (result == 0) {
        struct data *data = mem_pt(task->mem, PAGE(address))->data;
        data->fd = fd_retain(fd);
        data->file_offset = file_offset;
        data->file_backing_offset = file_offset;
        data->name = "/i386-msync-test";
    }
    write_wrunlock(&task->mem->lock);
    assert(result == 0);
}

static void map_anonymous(
        struct task *task, addr_t address, unsigned flags) {
    write_wrlock(&task->mem->lock);
    int result = pt_map_nothing(
            task->mem, PAGE(address), 1, flags);
    write_wrunlock(&task->mem->lock);
    assert(result == 0);
}

static unsigned fdatasync_calls(struct sync_probe *probe) {
    check_pthread(pthread_mutex_lock(&probe->lock));
    unsigned calls = probe->fdatasync_calls;
    check_pthread(pthread_mutex_unlock(&probe->lock));
    return calls;
}

static void set_sync_error(struct sync_probe *probe, int error) {
    check_pthread(pthread_mutex_lock(&probe->lock));
    probe->fdatasync_error = error;
    check_pthread(pthread_mutex_unlock(&probe->lock));
}

static void *run_msync(void *opaque) {
    struct msync_thread *thread = opaque;
    current = thread->task;
    thread->result = sys_msync(
            thread->address, thread->length, TEST_MS_SYNC);
    current = NULL;
    return NULL;
}

int main(void) {
    struct task task = {
        .pid = 6101,
        .tgid = 6101,
    };
    struct mm *memory = mm_new();
    assert(memory != NULL);
    task_set_mm(&task, memory);
    current = &task;

    assert(sys_msync(TEST_BASE + 1, 0, 0) == _EINVAL);
    assert(sys_msync(TEST_BASE, 0,
            TEST_MS_ASYNC | TEST_MS_SYNC) == _EINVAL);
    assert(sys_msync(TEST_BASE, 0, INT32_MIN) == _EINVAL);
    assert(sys_msync(TEST_BASE, 0, 0) == 0);
    assert(sys_msync(UINT32_C(0xfffff000), UINT32_MAX, 0) == 0);
    assert(sys_msync(UINT32_C(0xfffff000), PAGE_SIZE, 0) ==
            _ENOMEM);

    struct sync_probe probe;
    probe_init(&probe);
    struct fd *sync_fd = make_fd(&sync_ops, &probe, O_RDWR_);
    struct fd *read_fd = make_fd(&sync_ops, &probe, O_RDONLY_);
    struct fd *unsupported_fd = make_fd(
            &unsupported_ops, NULL, O_RDWR_);

    map_file(&task, TEST_BASE, 2,
            P_READ | P_WRITE | P_SHARED, sync_fd, 0);
    map_file(&task, TEST_BASE + 3 * PAGE_SIZE, 1,
            P_READ | P_WRITE | P_SHARED, sync_fd, 3 * PAGE_SIZE);
    map_file(&task, TEST_BASE + 4 * PAGE_SIZE, 1,
            P_READ | P_WRITE | P_COW, sync_fd, 4 * PAGE_SIZE);
    map_file(&task, TEST_BASE + 5 * PAGE_SIZE, 1,
            P_READ | P_SHARED, read_fd, 5 * PAGE_SIZE);
    map_anonymous(&task, TEST_BASE + 6 * PAGE_SIZE,
            P_READ | P_WRITE | P_SHARED);
    map_file(&task, TEST_BASE + 7 * PAGE_SIZE, 1,
            P_READ | P_WRITE | P_SHARED, unsupported_fd,
            7 * PAGE_SIZE);

    dword_t four_pages = 4 * PAGE_SIZE;
    assert(sys_msync(TEST_BASE, four_pages, TEST_MS_ASYNC) ==
            _ENOMEM);
    assert(sys_msync(TEST_BASE, four_pages, 0) == _ENOMEM);
    assert(sys_msync(TEST_BASE, four_pages,
            TEST_MS_INVALIDATE) == _ENOMEM);
    assert(sys_msync(TEST_BASE, four_pages,
            TEST_MS_ASYNC | TEST_MS_INVALIDATE) == _ENOMEM);
    assert(fdatasync_calls(&probe) == 0);

    assert(sys_msync(TEST_BASE, four_pages, TEST_MS_SYNC) ==
            _ENOMEM);
    assert(fdatasync_calls(&probe) == 2);
    assert(sys_msync(TEST_BASE + 4 * PAGE_SIZE,
            PAGE_SIZE, TEST_MS_SYNC) == 0);
    assert(sys_msync(TEST_BASE + 5 * PAGE_SIZE,
            PAGE_SIZE, TEST_MS_SYNC) == 0);
    assert(sys_msync(TEST_BASE + 6 * PAGE_SIZE,
            PAGE_SIZE, TEST_MS_SYNC) == 0);
    assert(fdatasync_calls(&probe) == 2);

    set_sync_error(&probe, _ENOSPC);
    assert(sys_msync(TEST_BASE + 2 * PAGE_SIZE,
            2 * PAGE_SIZE, TEST_MS_SYNC) == _ENOSPC);
    assert(fdatasync_calls(&probe) == 3);
    set_sync_error(&probe, 0);
    assert(sys_msync(TEST_BASE + 7 * PAGE_SIZE,
            PAGE_SIZE, TEST_MS_SYNC) == _EINVAL);

    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.block = true;
    probe.entered = false;
    probe.allow = false;
    check_pthread(pthread_mutex_unlock(&probe.lock));
    struct msync_thread blocked = {
        .task = &task,
        .address = TEST_BASE,
        .length = PAGE_SIZE,
    };
    pthread_t thread;
    check_pthread(pthread_create(&thread, NULL, run_msync, &blocked));
    check_pthread(pthread_mutex_lock(&probe.lock));
    while (!probe.entered)
        check_pthread(pthread_cond_wait(&probe.changed, &probe.lock));
    check_pthread(pthread_mutex_unlock(&probe.lock));

    assert(sys_munmap(TEST_BASE, 2 * PAGE_SIZE) == 0);
    check_pthread(pthread_mutex_lock(&probe.lock));
    probe.allow = true;
    check_pthread(pthread_cond_broadcast(&probe.changed));
    check_pthread(pthread_mutex_unlock(&probe.lock));
    check_pthread(pthread_join(thread, NULL));
    assert(blocked.result == 0 && fdatasync_calls(&probe) == 4);

    check_pthread(pthread_mutex_lock(&probe.lock));
    assert(probe.fsync_calls == 0);
    check_pthread(pthread_mutex_unlock(&probe.lock));

    current = NULL;
    mm_release(memory);
    fd_close(sync_fd);
    fd_close(read_fd);
    fd_close(unsupported_fd);
    probe_destroy(&probe);
    return 0;
}

#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "文件锁边界测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static const struct fd_ops lock_ops = {};

static void init_inode(struct inode_data *inode) {
    memset(inode, 0, sizeof(*inode));
    list_init(&inode->posix_locks);
    cond_init(&inode->posix_unlock);
    lock_init(&inode->lock);
}

struct blocking_lock_call {
    struct task *task;
    struct fd *fd;
    struct flock_ flock;
    atomic_bool returned;
    int result;
};

static void *run_blocking_lock(void *opaque) {
    struct blocking_lock_call *call = opaque;
    current = call->task;
    task_thread_store(call->task, pthread_self());
    call->result = fcntl_setlk(call->fd, &call->flock, true);
    atomic_store_explicit(&call->returned, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool wait_until_blocked(struct blocking_lock_call *call,
        struct inode_data *inode) {
    for (unsigned attempt = 0; attempt < 1000000; attempt++) {
        lock(&call->task->waiting_cond_lock);
        bool waiting = call->task->waiting_cond == &inode->posix_unlock;
        unlock(&call->task->waiting_cond_lock);
        if (waiting)
            return true;
        if (atomic_load_explicit(
                &call->returned, memory_order_acquire))
            return false;
        sched_yield();
    }
    return false;
}

int main(void) {
    struct task task = {0};
    struct fdtable owner = {0};
    task.files = &owner;
    task.pid = 42;
    task.tgid = 420;
    current = &task;

    struct inode_data inode;
    init_inode(&inode);
    struct fd fd = {
        .flags = O_RDWR_,
        .ops = &lock_ops,
        .inode = &inode,
    };
    lock_init(&fd.lock);

    struct flock_ lock_request = {
        .type = F_WRLCK_,
        .whence = LSEEK_SET,
        .start = INT64_C(0x100000005),
        .len = 0,
    };
    CHECK(fcntl_setlk(&fd, &lock_request, false) == 0,
            "零长度锁接受超过 4 GiB 的起点");
    CHECK(list_size(&inode.posix_locks) == 1,
            "零长度锁写入 inode 锁表");
    struct file_lock *stored = list_first_entry(
            &inode.posix_locks, struct file_lock, locks);
    CHECK(stored->start == lock_request.start &&
            stored->end == INT64_MAX && stored->pid == task.tgid,
            "零长度锁延伸到 64 位文件偏移上限");
    file_lock_remove_owned_by(&fd, task.files);

    lock_request.start = INT64_C(0x100000010);
    lock_request.len = -5;
    CHECK(fcntl_setlk(&fd, &lock_request, false) == 0,
            "负长度锁接受超过 4 GiB 的反向区间");
    stored = list_first_entry(
            &inode.posix_locks, struct file_lock, locks);
    CHECK(stored->start == INT64_C(0x10000000b) &&
            stored->end == INT64_C(0x10000000f),
            "负长度锁保持完整的 64 位区间");
    file_lock_remove_owned_by(&fd, task.files);

    lock_request.start = INT64_MAX;
    lock_request.len = 2;
    CHECK(fcntl_setlk(&fd, &lock_request, false) == _EOVERFLOW &&
            list_empty(&inode.posix_locks),
            "正长度锁拒绝结束位置溢出");

    lock_request.start = 0;
    lock_request.len = -1;
    CHECK(fcntl_setlk(&fd, &lock_request, false) == _EINVAL &&
            list_empty(&inode.posix_locks),
            "负长度锁拒绝越过文件起点");

    fd.flags = O_RDONLY_;
    lock_request = (struct flock_) {
        .type = F_WRLCK_,
        .whence = LSEEK_SET,
        .start = INT64_MAX,
        .len = 2,
    };
    CHECK(fcntl_setlk(&fd, &lock_request, false) == _EOVERFLOW,
            "SETLK 在访问模式前报告区间溢出");
    lock_request = (struct flock_) {
        .type = 99,
        .whence = LSEEK_SET,
        .start = 0,
        .len = 1,
    };
    CHECK(fcntl_setlk(&fd, &lock_request, false) == _EINVAL,
            "SETLK 在访问模式前拒绝无效锁类型");
    lock_request.type = F_WRLCK_;
    CHECK(fcntl_setlk(&fd, &lock_request, false) == _EBADF,
            "SETLK 拒绝只读 fd 的写锁");
    fd.flags = O_RDWR_;

    struct fdtable waiter_owner = {0};
    struct task waiter = {
        .files = &waiter_owner,
        .pid = 84,
        .tgid = 840,
        .sighand = sighand_new(),
    };
    CHECK(waiter.sighand != NULL, "初始化阻塞锁 waiter 信号状态");
    list_init(&waiter.queue);
    lock_init(&waiter.waiting_cond_lock);

    lock_request = (struct flock_) {
        .type = F_WRLCK_,
        .whence = LSEEK_SET,
        .start = 0,
        .len = 16,
    };
    CHECK(fcntl_setlk(&fd, &lock_request, false) == 0,
            "owner 建立显式解锁唤醒夹具锁");
    struct blocking_lock_call call = {
        .task = &waiter,
        .fd = &fd,
        .flock = lock_request,
    };
    atomic_init(&call.returned, false);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_blocking_lock, &call) == 0,
            "启动 F_SETLKW 显式解锁 waiter");
    CHECK(wait_until_blocked(&call, &inode),
            "F_SETLKW 在冲突锁上真实进入阻塞");
    struct flock_ unlock_request = lock_request;
    unlock_request.type = F_UNLCK_;
    CHECK(fcntl_setlk(&fd, &unlock_request, false) == 0,
            "owner 显式解锁并通知 waiter");
    CHECK(pthread_join(thread, NULL) == 0 && call.result == 0 &&
            atomic_load_explicit(&call.returned, memory_order_acquire),
            "F_SETLKW 在显式解锁后获得区间");
    file_lock_remove_owned_by(&fd, waiter.files);

    current = &task;
    CHECK(fcntl_setlk(&fd, &lock_request, false) == 0,
            "owner 建立 close 唤醒夹具锁");
    call.flock = lock_request;
    call.result = _EINTR;
    atomic_store_explicit(&call.returned, false, memory_order_release);
    CHECK(pthread_create(&thread, NULL, run_blocking_lock, &call) == 0,
            "启动 F_SETLKW close waiter");
    CHECK(wait_until_blocked(&call, &inode),
            "close waiter 在冲突锁上真实进入阻塞");
    file_lock_remove_owned_by(&fd, task.files);
    CHECK(pthread_join(thread, NULL) == 0 && call.result == 0,
            "owner close 释放记录锁后唤醒 F_SETLKW");
    file_lock_remove_owned_by(&fd, waiter.files);

    current = NULL;
    lock_destroy(&waiter.waiting_cond_lock);
    sighand_release(waiter.sighand);
    cond_destroy(&inode.posix_unlock);
    lock_destroy(&inode.lock);
    lock_destroy(&fd.lock);

    return 0;
}

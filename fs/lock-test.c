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

int main(void) {
    struct task task = {0};
    struct fdtable owner = {0};
    task.files = &owner;
    task.pid = 42;
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
            stored->end == INT64_MAX,
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

    current = NULL;
    return 0;
}

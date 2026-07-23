#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "realfs provider 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        goto out; \
    } \
} while (0)

int main(void) {
    int status = 1;
    int first_host_fd = -1;
    int second_host_fd = -1;
    int root_fd = -1;
    char path[] = "/tmp/ish-real-provider-XXXXXX";

    first_host_fd = mkstemp(path);
    CHECK(first_host_fd >= 0, "创建第一份文件描述符");
    second_host_fd = open(path, O_RDWR);
    CHECK(second_host_fd >= 0 && unlink(path) == 0,
            "独立打开同一文件并解除路径绑定");

    struct fd first = {.real_fd = first_host_fd};
    struct fd second = {.real_fd = second_host_fd};
    CHECK(realfs_flock(&first, LOCK_EX_ | LOCK_NB_) == 0,
            "第一份描述符取得非阻塞独占锁");
    CHECK(realfs_flock(&second, LOCK_EX_ | LOCK_NB_) == _EAGAIN,
            "竞争锁把 Darwin errno 映射为 Linux EAGAIN");
    CHECK(realfs_flock(&first, LOCK_UN_) == 0 &&
            realfs_flock(&second, LOCK_EX_ | LOCK_NB_) == 0 &&
            realfs_flock(&second, LOCK_UN_) == 0,
            "解锁后另一份描述符可以取得并释放锁");

    root_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(root_fd >= 0, "打开 statfs 根目录");
    struct mount mount = {
        .fs = &realfs,
        .root_fd = root_fd,
    };
    struct statfsbuf stat = {};
    CHECK(realfs_statfs(&mount, &stat) == 0 &&
            stat.bsize > 0 && stat.blocks > 0 && stat.namelen > 0,
            "realfs 返回宿主文件系统容量与名称边界");
    CHECK(close(root_fd) == 0, "关闭 statfs 根目录");
    root_fd = -1;
    mount.root_fd = -1;
    CHECK(realfs_statfs(&mount, &stat) == _EBADF,
            "realfs 映射 fstatvfs 的无效描述符错误");

    status = 0;
out:
    if (root_fd >= 0)
        close(root_fd);
    if (second_host_fd >= 0)
        close(second_host_fd);
    if (first_host_fd >= 0)
        close(first_host_fd);
    if (status != 0)
        unlink(path);
    return status;
}

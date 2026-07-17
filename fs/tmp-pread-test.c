#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "tmpfs 定位读取测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        goto out; \
    } \
} while (0)

int main(void) {
    int status = 1;
    struct mount *mount = NULL;
    struct fd *fd = NULL;
    struct task task = {
        .euid = 1000,
        .egid = 1000,
    };
    current = &task;

    lock(&mounts_lock);
    int mount_error = do_mount(&tmpfs, "", "", "", 0);
    unlock(&mounts_lock);
    CHECK(mount_error == 0, "挂载测试 tmpfs");

    char root_path[] = "/";
    mount = mount_find(root_path);
    fd = mount->fs->open(
            mount, "/positioned", O_CREAT_ | O_RDWR_, 0644);
    CHECK(!IS_ERR(fd), "创建测试普通文件");
    fd->mount = mount;
    mount = NULL;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    CHECK(fd->ops->page_cacheable && fd->ops->pread != NULL,
            "tmpfs 普通文件必须提供可分页的原生 positioned read");

    const char contents[] = "abcdef";
    CHECK(file_write_fd(fd, contents, sizeof(contents) - 1) ==
            (ssize_t) (sizeof(contents) - 1),
            "写入顺序读取夹具");

    fd->offset = 4;
    char positioned[3] = {0};
    CHECK(fd->ops->pread(fd, positioned, 2, 1) == 2 &&
            strcmp(positioned, "bc") == 0 && fd->offset == 4,
            "原生定位读取返回指定区间且不修改顺序 offset");

    char sequential[3] = {0};
    CHECK(file_read_fd(fd, sequential, 2) == 2 &&
            strcmp(sequential, "ef") == 0 && fd->offset == 6,
            "定位读取后顺序读取仍从原 offset 继续");

    fd->offset = 3;
    CHECK(fd->ops->pread(fd, positioned, 1, -1) == _EINVAL &&
            fd->offset == 3,
            "负定位偏移不改变顺序 offset");
    CHECK(fd->ops->pread(fd, positioned, 1, 99) == 0 &&
            fd->offset == 3,
            "越过 EOF 的定位读取不改变顺序 offset");

    int close_error = fd_close(fd);
    fd = NULL;
    CHECK(close_error == 0, "关闭测试文件");
    status = 0;

out:
    if (fd != NULL && !IS_ERR(fd))
        fd_close(fd);
    if (mount != NULL)
        mount_release(mount);
    current = NULL;
    return status;
}

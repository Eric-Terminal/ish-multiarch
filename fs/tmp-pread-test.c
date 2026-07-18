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
    CHECK(fd->opened_created, "tmpfs 原子报告新建文件");
    struct fd *existing = fd->mount->fs->open(
            fd->mount, "/positioned", O_CREAT_ | O_RDWR_, 0644);
    CHECK(!IS_ERR(existing) && !existing->opened_created,
            "tmpfs 原子区分既存文件");
    fd_close(existing);
    CHECK(fd->ops->page_cacheable && fd->ops->pread != NULL &&
            fd->ops->pwrite != NULL && fd->ops->page_pwrite != NULL,
            "tmpfs 普通文件必须提供可分页的精确定位读写");

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

    CHECK(file_pwrite_fd(fd, "XY", 2, 1) == 2 && fd->offset == 3,
            "普通 pwrite 覆盖指定位置且不改顺序 offset");
    CHECK(fd_setflags(fd, O_APPEND_) == 0,
            "为零长度追加测试设置 O_APPEND");
    CHECK(file_write_fd(fd, "", 0) == 0 && fd->offset == 3,
            "O_APPEND 下的零长度 write 不移动顺序 offset");
    CHECK(file_pwrite_fd(fd, "g", 1, 0) == 1 && fd->offset == 3,
            "tmpfs 的普通 pwrite 在 O_APPEND 下追加且恢复 offset");

    ssize_t page_written = file_page_pwrite_fd_uncoordinated(
            fd, "Z", 1, 0);
    CHECK(page_written == 1 && fd->offset == 3,
            "pager 精确写回忽略 O_APPEND 且不改顺序 offset");

    char final[8] = {0};
    CHECK(file_pread_fd(fd, final, 7, 0) == 7 &&
            strcmp(final, "ZXYdefg") == 0,
            "普通追加与 pager 精确写入落在不同文件位置");
    CHECK(file_pwrite_fd(fd, "", 0, INT64_MAX) == 0,
            "零长定位写不因超大 offset 扩展文件");
    struct statbuf stat;
    CHECK(file_fstat_fd(fd, &stat) == 0 && stat.size == 7,
            "零长定位写后文件大小保持不变");
    CHECK(file_sync_fd(fd, true) == 0 &&
            file_sync_fd(fd, false) == 0,
            "tmpfs 的 fdatasync 与 fsync 均为空操作成功");

    CHECK(fd->mount->fs->fsetattr(fd, make_attr(size, 3)) == 0,
            "fd 属性入口可缩短 tmpfs 普通文件");
    memset(final, 0x7f, sizeof(final));
    CHECK(file_fstat_fd(fd, &stat) == 0 && stat.size == 3 &&
            file_pread_fd(fd, final, sizeof(final), 0) == 3 &&
            memcmp(final, "ZXY", 3) == 0,
            "缩短后只保留新 EOF 之前的数据");

    CHECK(fd->mount->fs->fsetattr(fd, make_attr(size, 7)) == 0,
            "fd 属性入口可增长 tmpfs 普通文件");
    memset(final, 0x7f, sizeof(final));
    CHECK(file_pread_fd(fd, final, 7, 0) == 7 &&
            memcmp(final, "ZXY\0\0\0\0", 7) == 0,
            "重新增长的区间全部零填充而不泄漏截断内容");

    CHECK(fd->mount->fs->fsetattr(fd, make_attr(size, -1)) == _EINVAL,
            "负文件大小被拒绝");
    memset(final, 0x7f, sizeof(final));
    CHECK(file_fstat_fd(fd, &stat) == 0 && stat.size == 7 &&
            file_pread_fd(fd, final, 7, 0) == 7 &&
            memcmp(final, "ZXY\0\0\0\0", 7) == 0,
            "尺寸变更失败不修改原大小或内容");

#if SIZE_MAX < INT64_MAX
    CHECK(fd->mount->fs->fsetattr(
            fd, make_attr(size, INT64_MAX)) == _EFBIG,
            "超过宿主 size_t 的文件大小返回 EFBIG");
    CHECK(file_fstat_fd(fd, &stat) == 0 && stat.size == 7,
            "超出宿主范围失败后文件大小保持不变");
#endif

    CHECK(fd->mount->fs->setattr(
            fd->mount, "/positioned", make_attr(size, 0)) == 0,
            "路径属性入口可将 tmpfs 普通文件截断为零");
    CHECK(file_fstat_fd(fd, &stat) == 0 && stat.size == 0 &&
            file_pread_fd(fd, final, sizeof(final), 0) == 0,
            "零长度截断释放内容并发布空文件大小");
    CHECK(fd->mount->fs->setattr(
            fd->mount, "/positioned", make_attr(size, 4)) == 0,
            "路径属性入口可从零增长 tmpfs 普通文件");
    memset(final, 0x7f, sizeof(final));
    CHECK(file_pread_fd(fd, final, 4, 0) == 4 &&
            memcmp(final, "\0\0\0\0", 4) == 0,
            "从零增长的文件内容全部零填充");

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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    int link_created = 0;
    struct fd *nofollow_fd = NULL;
    char path[] = "/tmp/ish-real-provider-XXXXXX";

    first_host_fd = mkstemp(path);
    CHECK(first_host_fd >= 0, "创建第一份文件描述符");
    second_host_fd = open(path, O_RDWR);
    CHECK(second_host_fd >= 0, "独立打开同一文件");

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
    const char *link_name = strrchr(path, '/') + 1;

    struct attr group_attr = make_attr(gid, (uid_t_) getgid());
    group_attr.follow_links = true;
    CHECK(realfs_setattr(&mount, link_name, group_attr) == 0,
            "路径 GID 更新使用 group 参数槽");
    struct stat host_stat;
    CHECK(fstat(first_host_fd, &host_stat) == 0 &&
            host_stat.st_uid == getuid() && host_stat.st_gid == getgid(),
            "路径 GID 更新不误改 UID");
    CHECK(realfs_fsetattr(&first, group_attr) == 0 &&
            fstat(first_host_fd, &host_stat) == 0 &&
            host_stat.st_uid == getuid() && host_stat.st_gid == getgid(),
            "fd GID 更新不误改 UID");

    const struct attr ownership_attr = {
        .type = attr_ownership,
        .follow_links = true,
        .ownership = {
            .uid = (uid_t_) getuid(),
            .gid = (uid_t_) getgid(),
        },
    };
    CHECK(realfs_setattr(&mount, link_name, ownership_attr) == 0 &&
            realfs_fsetattr(&first, ownership_attr) == 0,
            "路径与 fd ownership 在一次 provider 调用中提交");

    struct attr mode_attr = make_attr(mode, 0600);
    mode_attr.follow_links = true;
    CHECK(realfs_setattr(&mount, link_name, mode_attr) == 0 &&
            stat(path, &host_stat) == 0 &&
            (host_stat.st_mode & 07777) == 0600,
            "路径 mode 写入真实 provider");
    mode_attr.mode = 0640;
    CHECK(realfs_fsetattr(&first, mode_attr) == 0 &&
            fstat(first_host_fd, &host_stat) == 0 &&
            (host_stat.st_mode & 07777) == 0640,
            "fd mode 写入真实 provider");

    const struct timespec path_atime = {.tv_sec = 946684800};
    const struct timespec path_mtime = {.tv_sec = 946684900};
    CHECK(realfs_utime(
            &mount, link_name, path_atime, path_mtime, true) == 0 &&
            stat(path, &host_stat) == 0 &&
            host_stat.st_atime == path_atime.tv_sec &&
            host_stat.st_mtime == path_mtime.tv_sec,
            "路径时间戳精确写入真实 provider");

    const struct timespec fd_atime = {.tv_sec = 978307200};
    const struct timespec fd_mtime = {.tv_sec = 978307300};
    CHECK(realfs_futime(&first, fd_atime, fd_mtime) == 0 &&
            fstat(first_host_fd, &host_stat) == 0 &&
            host_stat.st_atime == fd_atime.tv_sec &&
            host_stat.st_mtime == fd_mtime.tv_sec,
            "fd 时间戳不通过路径回查");
    struct fd invalid = {.real_fd = -1};
    CHECK(realfs_futime(&invalid, fd_atime, fd_mtime) == _EBADF &&
            realfs_fsetattr(&invalid, ownership_attr) == _EBADF &&
            realfs_fsetattr(&invalid, mode_attr) == _EBADF,
            "真实 provider 映射无效 fd 的元数据错误");

    CHECK(unlink(path) == 0, "元数据验证后解除临时文件路径绑定");
    const struct timespec unlinked_atime = {.tv_sec = 1001894400};
    const struct timespec unlinked_mtime = {.tv_sec = 1001894500};
    CHECK(realfs_futime(&first,
                    unlinked_atime, unlinked_mtime) == 0 &&
            fstat(first_host_fd, &host_stat) == 0 &&
            host_stat.st_atime == unlinked_atime.tv_sec &&
            host_stat.st_mtime == unlinked_mtime.tv_sec,
            "fd 时间戳在路径解除绑定后仍作用于打开文件");
    mode_attr.mode = 0600;
    CHECK(realfs_fsetattr(&first, mode_attr) == 0 &&
            fstat(first_host_fd, &host_stat) == 0 &&
            (host_stat.st_mode & 07777) == 0600,
            "fd mode 在路径解除绑定后仍作用于打开文件");
    CHECK(symlinkat(".", root_fd, link_name) == 0,
            "创建最终分量符号链接");
    link_created = 1;
    struct attr nofollow_group = group_attr;
    nofollow_group.follow_links = false;
    CHECK(realfs_setattr(&mount, link_name, nofollow_group) == 0 &&
            fstatat(root_fd, link_name, &host_stat,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(host_stat.st_mode) &&
            host_stat.st_uid == getuid() && host_stat.st_gid == getgid(),
            "NOFOLLOW ownership 作用于最终符号链接而非目标目录");
    const struct timespec link_atime = {.tv_sec = 1009843200};
    const struct timespec link_mtime = {.tv_sec = 1009843300};
    CHECK(realfs_utime(&mount, link_name,
                    link_atime, link_mtime, false) == 0 &&
            fstatat(root_fd, link_name, &host_stat,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
            host_stat.st_atime == link_atime.tv_sec &&
            host_stat.st_mtime == link_mtime.tv_sec,
            "NOFOLLOW 时间戳作用于最终符号链接");
    nofollow_fd = realfs_open(
            &mount, link_name, O_RDONLY_ | O_NOFOLLOW_, 0);
    CHECK(IS_ERR(nofollow_fd) && PTR_ERR(nofollow_fd) == _ELOOP,
            "realfs 把 NOFOLLOW 映射到宿主并拒绝最终符号链接");
    nofollow_fd = realfs_open(
            &mount, ".", O_RDONLY_ | O_NOFOLLOW_, 0);
    CHECK(!IS_ERR(nofollow_fd), "realfs 允许 NOFOLLOW 打开普通目标");
    nofollow_fd->flags = O_RDONLY_ | O_LARGEFILE_ |
            O_DIRECTORY_ | O_NOFOLLOW_;
    nofollow_fd->logical_access_mode = true;
    CHECK((fd_getflags(nofollow_fd) &
                    (O_LARGEFILE_ | O_DIRECTORY_ | O_NOFOLLOW_)) ==
                    (O_LARGEFILE_ | O_DIRECTORY_ | O_NOFOLLOW_),
            "F_GETFL 保留 guest 路径打开标志");
    CHECK(fd_close(nofollow_fd) == 0, "关闭 NOFOLLOW 普通目标");
    nofollow_fd = NULL;
    CHECK(unlinkat(root_fd, link_name, 0) == 0,
            "清理最终分量符号链接");
    link_created = 0;

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
    if (nofollow_fd != NULL && !IS_ERR(nofollow_fd))
        fd_close(nofollow_fd);
    if (link_created && root_fd >= 0) {
        const char *link_name = strrchr(path, '/') + 1;
        unlinkat(root_fd, link_name, 0);
    }
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

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "realfs 定位写测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        goto out; \
    } \
} while (0)

static unsigned host_shared_mapping_count(struct inode_data *inode) {
    lock(&inode->lock);
    unsigned count = inode->host_shared_mapping_count;
    unlock(&inode->lock);
    return count;
}

static void attach_mapping_fd(
        struct mem *mem, page_t page, struct fd *fd) {
    struct data *data = mem_pt(mem, page)->data;
    data->fd = fd_retain(fd);
    data->file_offset = 0;
    data->file_backing_offset = 0;
}

int main(void) {
    int status = 1;
    struct fd *fd = NULL;
    char path[] = "/tmp/ish-real-pwrite-XXXXXX";
    int host_fd = mkstemp(path);
    CHECK(host_fd >= 0, "创建临时文件");
    CHECK(unlink(path) == 0, "取消临时文件路径绑定");
    CHECK(write(host_fd, "abcdef", 6) == 6 &&
            lseek(host_fd, 2, SEEK_SET) == 2,
            "建立文件内容与顺序 offset 夹具");

    fd = fd_create(&realfs_fdops);
    CHECK(fd != NULL, "创建 realfs 文件对象");
    fd->real_fd = host_fd;
    fd->type = S_IFREG;
    struct stat host_stat;
    CHECK(fstat(host_fd, &host_stat) == 0, "读取 host 文件身份");
    struct mount mount = {.fs = &realfs};
    fd->mount = &mount;
    mount_retain(&mount);
    fd->inode = inode_get(&mount,
            realfs_inode_device(host_stat.st_dev), host_stat.st_ino);

    CHECK(fd_setflags(fd, O_APPEND_) == 0,
            "打开 guest O_APPEND");
    CHECK(file_pwrite_fd(fd, "g", 1, 0) == 1 &&
            lseek(host_fd, 0, SEEK_CUR) == 2,
            "普通 pwrite 按 Linux 语义追加且不改 offset");
    CHECK(file_page_pwrite_fd_uncoordinated(fd, "Z", 1, 0) == 1 &&
            lseek(host_fd, 0, SEEK_CUR) == 2,
            "pager 写回忽略 O_APPEND 且不改 offset");
    CHECK((fcntl(host_fd, F_GETFL) & O_APPEND) != 0,
            "pager 写回后恢复 host O_APPEND");

    char contents[9] = {0};
    CHECK(file_pread_fd(fd, contents, 7, 0) == 7 &&
            strcmp(contents, "Zbcdefg") == 0,
            "追加写与 pager 精确写落在不同位置");
    CHECK(file_pwrite_fd(fd, "h", 1, 0) == 1 &&
            file_pread_fd(fd, contents, 8, 0) == 8 &&
            strcmp(contents, "Zbcdefgh") == 0,
            "精确写回不破坏后续普通追加语义");

    struct mem parent;
    struct mem child;
    mem_init(&parent);
    mem_init(&child);
    const page_t mapping_page = 0x40000;
    CHECK(realfs_mmap(fd, &parent, mapping_page, 0, 0,
            P_READ | P_SHARED, MMAP_SHARED) == _EINVAL &&
            host_shared_mapping_count(fd->inode) == 0,
            "host mmap 失败时回滚共享映射 token");
    mem_test_fail_pt_map_at(0);
    CHECK(realfs_mmap(fd, &parent, mapping_page, 1, 0,
            P_READ | P_SHARED, MMAP_SHARED) == _ENOMEM &&
            host_shared_mapping_count(fd->inode) == 0 &&
            mem_pt(&parent, mapping_page) == NULL,
            "pt_map 元数据分配失败时回收 host VM 与共享 token");
    mem_test_fail_pt_map_at(SIZE_MAX);
    CHECK(realfs_mmap(fd, &parent, mapping_page, 1, 0,
            P_READ | P_WRITE | P_SHARED, MMAP_SHARED) == 0,
            "建立 i386 风格的 realfs 共享映射");
    attach_mapping_fd(&parent, mapping_page, fd);
    CHECK(host_shared_mapping_count(fd->inode) == 1,
            "独立 host 共享后备只登记一个 token");

    CHECK(realfs_mmap(fd, &parent, mapping_page, 1, 0,
            P_READ | P_WRITE | P_SHARED, MMAP_SHARED) == 0,
            "固定替换同页时先登记新 host 后备");
    attach_mapping_fd(&parent, mapping_page, fd);
    CHECK(host_shared_mapping_count(fd->inode) == 1,
            "固定替换归还旧 token 且保留新 token");

    CHECK(pt_copy_on_write(&parent, &child,
            mapping_page, 1) == 0 &&
            host_shared_mapping_count(fd->inode) == 1,
            "fork 共享同一 data 后备且不重复登记 token");
    CHECK(pt_unmap_always(&parent, mapping_page, 1) == 0 &&
            host_shared_mapping_count(fd->inode) == 1,
            "父映射释放后子映射继续保有 token");
    mem_destroy(&parent);
    CHECK(host_shared_mapping_count(fd->inode) == 1,
            "销毁空父地址空间不重复归还 token");
    mem_destroy(&child);
    CHECK(host_shared_mapping_count(fd->inode) == 0,
            "最后一份 data 引用释放后归还 host 共享 token");

    mem_init(&parent);
    mem_init(&child);
    CHECK(realfs_mmap(fd, &parent, mapping_page, 1, 0,
            P_READ | P_SHARED, MMAP_SHARED) == 0 &&
            realfs_mmap(fd, &child, mapping_page, 1, 0,
            P_READ | P_SHARED, MMAP_SHARED) == 0,
            "建立两个独立的 host 共享后备");
    attach_mapping_fd(&parent, mapping_page, fd);
    attach_mapping_fd(&child, mapping_page, fd);
    CHECK(host_shared_mapping_count(fd->inode) == 2,
            "独立 host 后备分别持有 token");
    mem_destroy(&parent);
    CHECK(host_shared_mapping_count(fd->inode) == 1,
            "释放第一个独立后备只归还自己的 token");
    mem_destroy(&child);
    CHECK(host_shared_mapping_count(fd->inode) == 0,
            "释放第二个独立后备后 token 清零");

    int close_error = fd_close(fd);
    fd = NULL;
    CHECK(close_error == 0 && mount.refcount == 0,
            "关闭 realfs 测试文件并平衡 inode 与 mount 引用");
    status = 0;

out:
    if (fd != NULL)
        fd_close(fd);
    else if (status != 0 && host_fd >= 0)
        close(host_fd);
    return status;
}

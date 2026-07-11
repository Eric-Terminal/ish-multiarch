#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "文件任务语义测试失败：%s（第 %d 行）\n", message, __LINE__); \
        return 1; \
    } \
} while (0)

struct io_probe {
    const char *input;
    size_t input_size;
    char output[32];
    size_t output_size;
    unsigned read_calls;
    unsigned write_calls;
    off_t_ positioned_offset;
    unsigned seek_calls;
    int read_error;
    int write_error;
    int stat_error;
    int path_error;
    qword_t inode;
    const char *path;
};

struct task_fixture {
    struct task task;
    struct tgroup group;
    struct fs_info fs;
    struct mount mount;
};

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    probe->read_calls++;
    if (probe->read_error != 0)
        return probe->read_error;
    size_t copied = size < probe->input_size ? size : probe->input_size;
    memcpy(buffer, probe->input, copied);
    return (ssize_t) copied;
}

static ssize_t probe_write(struct fd *fd, const void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    probe->write_calls++;
    if (probe->write_error != 0)
        return probe->write_error;
    size_t copied = size < sizeof(probe->output) ? size : sizeof(probe->output);
    memcpy(probe->output, buffer, copied);
    probe->output_size = copied;
    return (ssize_t) copied;
}

static ssize_t probe_pread(struct fd *fd, void *buffer, size_t size, off_t offset) {
    struct io_probe *probe = fd->data;
    probe->positioned_offset = offset;
    return probe_read(fd, buffer, size);
}

static ssize_t probe_pwrite(struct fd *fd, const void *buffer, size_t size, off_t offset) {
    struct io_probe *probe = fd->data;
    probe->positioned_offset = offset;
    return probe_write(fd, buffer, size);
}

static off_t_ probe_lseek(struct fd *fd, off_t_ offset, int whence) {
    struct io_probe *probe = fd->data;
    if (whence != LSEEK_CUR)
        return _EINVAL;
    fd->offset += offset;
    probe->seek_calls++;
    return fd->offset;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct io_probe *probe = fd->data;
    if (probe->stat_error != 0)
        return probe->stat_error;
    stat->inode = probe->inode;
    stat->mode = S_IFREG | 0644;
    return 0;
}

static int probe_getpath(struct fd *fd, char *buffer) {
    struct io_probe *probe = fd->data;
    if (probe->path_error != 0)
        return probe->path_error;
    strcpy(buffer, probe->path);
    return 0;
}

static const struct fd_ops direct_ops = {
    .read = probe_read,
    .write = probe_write,
};

static const struct fd_ops positioned_ops = {
    .pread = probe_pread,
    .pwrite = probe_pwrite,
    .lseek = probe_lseek,
};

static const struct fd_ops empty_ops = {};

static const struct fs_ops probe_fs = {
    .fstat = probe_fstat,
    .getpath = probe_getpath,
};

static void init_fixture(struct task_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {8, 8};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    lock_init(&fixture->fs.lock);
    fixture->task.fs = &fixture->fs;
    fixture->mount.point = "";
    fixture->mount.fs = &probe_fs;
}

static struct fd *make_fd(struct task_fixture *fixture,
        struct io_probe *probe, const struct fd_ops *ops, mode_t_ type) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    fd->data = probe;
    fd->type = type;
    fd->mount = &fixture->mount;
    fixture->mount.refcount++;
    return fd;
}

int main(void) {
    struct task_fixture decoy;
    struct task_fixture target;
    init_fixture(&decoy);
    init_fixture(&target);
    CHECK(!IS_ERR(decoy.task.files) && !IS_ERR(target.task.files),
            "测试 fd 表创建成功");

    struct io_probe decoy_io = {
        .input = "错误任务",
        .input_size = strlen("错误任务"),
        .inode = 1,
        .path = "/decoy",
    };
    struct io_probe target_io = {
        .input = "right",
        .input_size = 5,
        .inode = UINT64_C(0x1122334455667788),
        .path = "/target",
    };
    struct io_probe positioned_io = {
        .input = "pos",
        .input_size = 3,
        .inode = 2,
        .path = "/positioned",
    };
    struct io_probe directory_io = {.path = "/directory"};
    struct io_probe empty_io = {.path = "/empty"};

    struct fd *decoy_fd = make_fd(&decoy, &decoy_io, &direct_ops, S_IFREG);
    struct fd *target_fd = make_fd(&target, &target_io, &direct_ops, S_IFREG);
    struct fd *positioned_fd = make_fd(&target, &positioned_io, &positioned_ops, S_IFREG);
    struct fd *directory_fd = make_fd(&target, &directory_io, &direct_ops, S_IFDIR);
    struct fd *empty_fd = make_fd(&target, &empty_io, &empty_ops, S_IFREG);
    struct fd *decoy_pwd = make_fd(&decoy, &decoy_io, &empty_ops, S_IFDIR);
    struct fd *target_pwd = make_fd(&target, &target_io, &empty_ops, S_IFDIR);
    CHECK(decoy_fd != NULL && target_fd != NULL && positioned_fd != NULL &&
            directory_fd != NULL && empty_fd != NULL && decoy_pwd != NULL &&
            target_pwd != NULL, "测试 fd 创建成功");
    decoy.fs.pwd = decoy_pwd;
    target.fs.pwd = target_pwd;
    CHECK(f_install_task(&decoy.task, decoy_fd, 0) == 0, "诱饵 fd 安装成功");
    CHECK(f_install_task(&target.task, target_fd, 0) == 0, "目标 fd 安装成功");
    CHECK(f_install_task(&target.task, positioned_fd, 0) == 1, "定位回退 fd 安装成功");
    CHECK(f_install_task(&target.task, directory_fd, 0) == 2, "目录 fd 安装成功");
    CHECK(f_install_task(&target.task, empty_fd, 0) == 3, "空操作 fd 安装成功");
    current = &decoy.task;

    char buffer[16] = {0};
    CHECK(file_read_task(&target.task, 0, buffer, 4) == 4,
            "host-buffer 读取使用显式目标任务");
    CHECK(memcmp(buffer, "righ", 4) == 0 && decoy_io.read_calls == 0,
            "显式读取不得落入 current 的同号 fd");
    CHECK(file_write_task(&target.task, 0, "write", 5) == 5,
            "host-buffer 写入使用显式目标任务");
    CHECK(target_io.output_size == 5 && memcmp(target_io.output, "write", 5) == 0 &&
            decoy_io.write_calls == 0, "显式写入只修改目标 fd");

    struct statbuf stat;
    memset(&stat, 0xff, sizeof(stat));
    CHECK(file_fstat_task(&target.task, 0, &stat) == 0,
            "host-buffer fstat 使用显式目标任务");
    CHECK(stat.inode == target_io.inode && stat.dev == 0 && stat.size == 0,
            "fstat 先清零架构中立结果再填充字段");

    char exact_cwd[8];
    CHECK(fs_getcwd_task(&target.task, exact_cwd, sizeof(exact_cwd)) == 8 &&
            strcmp(exact_cwd, "/target") == 0, "getcwd 接受恰好容纳 NUL 的 buffer");
    char cwd[16];
    memset(cwd, 0xa5, sizeof(cwd));
    CHECK(fs_getcwd_task(&target.task, cwd, sizeof(cwd)) == 8,
            "getcwd 返回包含结尾 NUL 的长度");
    CHECK(strcmp(cwd, "/target") == 0, "getcwd 使用目标任务的工作目录");
    memset(cwd, 0xa5, sizeof(cwd));
    CHECK(fs_getcwd_task(&target.task, cwd, 7) == _ERANGE,
            "getcwd 在空间不足时返回 ERANGE");
    for (size_t i = 0; i < sizeof(cwd); i++)
        CHECK((unsigned char) cwd[i] == 0xa5, "getcwd 失败时不修改 host buffer");
    target_io.path_error = _EIO;
    CHECK(fs_getcwd_task(&target.task, cwd, sizeof(cwd)) == _EIO,
            "getcwd 保持底层 getpath 错误");
    target_io.path_error = 0;
    CHECK(fs_getcwd_task(&target.task, cwd, sizeof(cwd)) == 8,
            "getpath 失败后必须释放 fs 锁");

    positioned_fd->offset = 7;
    memset(buffer, 0, sizeof(buffer));
    CHECK(file_read_task(&target.task, 1, buffer, sizeof(buffer)) == 3,
            "pread 回退返回底层部分读取长度");
    CHECK(positioned_io.positioned_offset == 7 && positioned_fd->offset == 10 &&
            positioned_io.seek_calls == 1, "pread 回退按实际读取量推进 offset");
    CHECK(file_write_task(&target.task, 1, "xy", 2) == 2,
            "pwrite 回退返回底层写入长度");
    CHECK(positioned_io.positioned_offset == 10 && positioned_fd->offset == 12 &&
            positioned_io.seek_calls == 2, "pwrite 回退按实际写入量推进 offset");
    positioned_io.read_error = _EIO;
    positioned_io.write_error = _ENOSPC;
    CHECK(file_read_task(&target.task, 1, buffer, 1) == _EIO &&
            file_write_task(&target.task, 1, buffer, 1) == _ENOSPC,
            "positioned I/O 保持底层错误");
    CHECK(positioned_fd->offset == 12 && positioned_io.seek_calls == 2,
            "positioned I/O 失败时不得推进 offset");

    CHECK(file_read_task(&target.task, 2, buffer, 1) == _EISDIR,
            "目录读取返回 EISDIR");
    CHECK(file_read_task(&target.task, 3, buffer, 1) == _EBADF &&
            file_write_task(&target.task, 3, buffer, 1) == _EBADF,
            "缺少读写操作时返回 EBADF");
    CHECK(file_read_task(&target.task, 99, buffer, 1) == _EBADF &&
            file_write_task(&target.task, 99, buffer, 1) == _EBADF,
            "无效描述符返回 EBADF");
    memset(&stat, 0xff, sizeof(stat));
    CHECK(file_fstat_task(&target.task, 99, &stat) == _EBADF && stat.inode == 0,
            "无效 fstat 返回 EBADF 并清零 host 结果");
    target_io.read_error = _EIO;
    target_io.write_error = _ENOSPC;
    target_io.stat_error = _EIO;
    memset(&stat, 0xff, sizeof(stat));
    CHECK(file_read_task(&target.task, 0, buffer, 1) == _EIO &&
            file_write_task(&target.task, 0, buffer, 1) == _ENOSPC &&
            file_fstat_task(&target.task, 0, &stat) == _EIO && stat.inode == 0,
            "底层文件操作错误保持原值返回");
    target_io.read_error = 0;
    target_io.write_error = 0;
    CHECK(file_read_task(&target.task, 0, buffer, 0) == 0,
            "零长度读取仍遵循 fd 操作语义");
    CHECK(file_write_task(&target.task, 0, buffer, 0) == 0,
            "零长度写入仍遵循 fd 操作语义");

    current = NULL;
    fdtable_release(decoy.task.files);
    fdtable_release(target.task.files);
    fd_close(decoy_pwd);
    fd_close(target_pwd);
    CHECK(decoy.mount.refcount == 0 && target.mount.refcount == 0,
            "清理阶段释放全部 mount 引用");
    return 0;
}

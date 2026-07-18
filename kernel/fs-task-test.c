#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/calls.h"
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

_Static_assert(sizeof(((struct fd *) 0)->offset) == sizeof(off_t_),
        "fd 顺序位置必须使用完整 guest 文件偏移宽度");

struct close_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool entered;
    bool released;
    unsigned observed_refcount;
};

struct io_probe {
    const char *input;
    size_t input_size;
    char output[32];
    size_t output_size;
    unsigned read_calls;
    unsigned write_calls;
    off_t_ positioned_offset;
    off_t_ sequential_read_offset;
    off_t_ sequential_write_offset;
    unsigned seek_calls;
    unsigned fsync_calls;
    unsigned fdatasync_calls;
    int read_error;
    int write_error;
    int fsync_error;
    int fdatasync_error;
    int stat_error;
    int resize_error;
    int path_error;
    qword_t inode;
    qword_t size;
    off_t_ requested_size;
    unsigned resize_calls;
    const char *path;
    struct close_gate *close_gate;
    unsigned close_calls;
};

struct task_fixture {
    struct task task;
    struct tgroup group;
    struct fs_info fs;
    struct mount mount;
};

static void wait_for_concurrent_close(struct fd *fd) {
    struct io_probe *probe = fd->data;
    struct close_gate *gate = probe->close_gate;
    if (gate == NULL)
        return;

    assert(pthread_mutex_lock(&gate->mutex) == 0);
    gate->entered = true;
    assert(pthread_cond_signal(&gate->cond) == 0);
    while (!gate->released)
        assert(pthread_cond_wait(&gate->cond, &gate->mutex) == 0);
    gate->observed_refcount = atomic_load_explicit(
            &fd->refcount, memory_order_relaxed);
    assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    wait_for_concurrent_close(fd);
    probe->read_calls++;
    probe->sequential_read_offset = fd->offset;
    if (probe->read_error != 0)
        return probe->read_error;
    size_t copied = size < probe->input_size ? size : probe->input_size;
    memcpy(buffer, probe->input, copied);
    return (ssize_t) copied;
}

static ssize_t probe_write(struct fd *fd, const void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    wait_for_concurrent_close(fd);
    probe->write_calls++;
    probe->sequential_write_offset = fd->offset;
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
    if (whence == LSEEK_CUR) {
        fd->offset += offset;
    } else if (whence == LSEEK_SET) {
        fd->offset = offset;
    } else {
        return _EINVAL;
    }
    probe->seek_calls++;
    return fd->offset;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct io_probe *probe = fd->data;
    wait_for_concurrent_close(fd);
    if (probe->stat_error != 0)
        return probe->stat_error;
    stat->inode = probe->inode;
    stat->mode = S_IFREG | 0644;
    stat->size = probe->size;
    return 0;
}

static int probe_fsetattr(struct fd *fd, struct attr attr) {
    struct io_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    assert(attr.type == attr_size);
    wait_for_concurrent_close(fd);
    probe->resize_calls++;
    probe->requested_size = attr.size;
    if (probe->resize_error != 0)
        return probe->resize_error;
    probe->size = (qword_t) attr.size;
    return 0;
}

static int probe_getflags(struct fd *fd) {
    wait_for_concurrent_close(fd);
    return fd->flags;
}

static int probe_close(struct fd *fd) {
    struct io_probe *probe = fd->data;
    probe->close_calls++;
    return 0;
}

static int probe_fsync(struct fd *fd) {
    struct io_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    probe->fsync_calls++;
    return probe->fsync_error;
}

static int probe_fdatasync(struct fd *fd) {
    struct io_probe *probe = fd->data;
    assert(lock_owned_by_current(&fd->lock));
    probe->fdatasync_calls++;
    return probe->fdatasync_error;
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
    .fsync = probe_fsync,
    .fdatasync = probe_fdatasync,
    .close = probe_close,
    .getflags = probe_getflags,
};

static const struct fd_ops fsync_only_ops = {
    .fsync = probe_fsync,
    .close = probe_close,
};

static const struct fd_ops positioned_ops = {
    .pread = probe_pread,
    .pwrite = probe_pwrite,
    .lseek = probe_lseek,
};

static const struct fd_ops seekable_ops = {
    .read = probe_read,
    .write = probe_write,
    .lseek = probe_lseek,
};

static const struct fd_ops empty_ops = {};

static const struct fs_ops probe_fs = {
    .fstat = probe_fstat,
    .fsetattr = probe_fsetattr,
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

enum close_race_operation {
    CLOSE_RACE_READ,
    CLOSE_RACE_WRITE,
    CLOSE_RACE_WRITE_CHECK,
    CLOSE_RACE_FSTAT,
    CLOSE_RACE_FTRUNCATE,
};

struct close_race_context {
    struct task *task;
    enum close_race_operation operation;
    fd_t fd_number;
    ssize_t result;
};

static void *run_close_race_operation(void *opaque) {
    struct close_race_context *context = opaque;
    char byte = 'x';
    struct statbuf stat;
    switch (context->operation) {
        case CLOSE_RACE_READ:
            context->result = file_read_task(context->task,
                    context->fd_number, &byte, sizeof(byte));
            break;
        case CLOSE_RACE_WRITE:
            context->result = file_write_task(context->task,
                    context->fd_number, &byte, sizeof(byte));
            break;
        case CLOSE_RACE_WRITE_CHECK:
            context->result = file_write_check_task(
                    context->task, context->fd_number);
            break;
        case CLOSE_RACE_FSTAT:
            context->result = file_fstat_task(
                    context->task, context->fd_number, &stat);
            break;
        case CLOSE_RACE_FTRUNCATE:
            context->result = file_ftruncate_task(
                    context->task, context->fd_number, 7);
            break;
    }
    return NULL;
}

static bool test_close_during_operation(struct task_fixture *fixture,
        enum close_race_operation operation) {
    struct close_gate gate = {0};
    if (pthread_mutex_init(&gate.mutex, NULL) != 0)
        return false;
    if (pthread_cond_init(&gate.cond, NULL) != 0) {
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }

    struct io_probe probe = {
        .input = "r",
        .input_size = 1,
        .inode = 3,
        .close_gate = &gate,
    };
    struct fd *fd = make_fd(
            fixture, &probe, &direct_ops, S_IFREG);
    if (fd == NULL) {
        pthread_cond_destroy(&gate.cond);
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }
    fd->flags = O_RDWR_;
    fd_t fd_number = f_install_task(&fixture->task, fd, 0);
    if (fd_number < 0) {
        pthread_cond_destroy(&gate.cond);
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }

    struct close_race_context context = {
        .task = &fixture->task,
        .operation = operation,
        .fd_number = fd_number,
        .result = _EIO,
    };
    pthread_t thread;
    if (pthread_create(
            &thread, NULL, run_close_race_operation, &context) != 0) {
        f_close_task(&fixture->task, fd_number);
        pthread_cond_destroy(&gate.cond);
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }

    assert(pthread_mutex_lock(&gate.mutex) == 0);
    while (!gate.entered)
        assert(pthread_cond_wait(&gate.cond, &gate.mutex) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);

    int close_result = f_close_task(&fixture->task, fd_number);
    bool retained_during_close = probe.close_calls == 0;

    assert(pthread_mutex_lock(&gate.mutex) == 0);
    gate.released = true;
    assert(pthread_cond_signal(&gate.cond) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
    bool joined = pthread_join(thread, NULL) == 0;

    ssize_t expected = operation == CLOSE_RACE_READ ||
            operation == CLOSE_RACE_WRITE ? 1 : 0;
    bool passed = close_result == 0 && retained_during_close && joined &&
            context.result == expected && gate.observed_refcount == 1 &&
            probe.close_calls == 1 &&
            f_get_task(&fixture->task, fd_number) == NULL;
    assert(pthread_cond_destroy(&gate.cond) == 0);
    assert(pthread_mutex_destroy(&gate.mutex) == 0);
    return passed;
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
    struct io_probe seekable_io = {
        .input = "seek",
        .input_size = 4,
        .path = "/seekable",
    };
    struct io_probe directory_io = {.path = "/directory"};
    struct io_probe empty_io = {.path = "/empty"};
    struct io_probe fsync_only_io = {.path = "/fsync-only"};

    struct fd *decoy_fd = make_fd(&decoy, &decoy_io, &direct_ops, S_IFREG);
    struct fd *target_fd = make_fd(&target, &target_io, &direct_ops, S_IFREG);
    struct fd *positioned_fd = make_fd(&target, &positioned_io, &positioned_ops, S_IFREG);
    struct fd *seekable_fd = make_fd(&target, &seekable_io, &seekable_ops, S_IFREG);
    struct fd *directory_fd = make_fd(&target, &directory_io, &direct_ops, S_IFDIR);
    struct fd *empty_fd = make_fd(&target, &empty_io, &empty_ops, S_IFREG);
    struct fd *fsync_only_fd = make_fd(
            &target, &fsync_only_io, &fsync_only_ops, S_IFREG);
    struct fd *decoy_pwd = make_fd(&decoy, &decoy_io, &empty_ops, S_IFDIR);
    struct fd *target_pwd = make_fd(&target, &target_io, &empty_ops, S_IFDIR);
    CHECK(decoy_fd != NULL && target_fd != NULL && positioned_fd != NULL &&
            seekable_fd != NULL &&
            directory_fd != NULL && empty_fd != NULL && decoy_pwd != NULL &&
            target_pwd != NULL && fsync_only_fd != NULL,
            "测试 fd 创建成功");
    target_fd->flags = O_RDWR_;
    positioned_fd->flags = O_RDWR_;
    seekable_fd->flags = O_RDWR_;
    empty_fd->flags = O_RDWR_;
    fsync_only_fd->flags = O_RDWR_;
    decoy.fs.pwd = decoy_pwd;
    target.fs.pwd = target_pwd;
    CHECK(f_install_task(&decoy.task, decoy_fd, 0) == 0, "诱饵 fd 安装成功");
    CHECK(f_install_task(&target.task, target_fd, 0) == 0, "目标 fd 安装成功");
    CHECK(f_install_task(&target.task, positioned_fd, 0) == 1, "定位回退 fd 安装成功");
    CHECK(f_install_task(&target.task, directory_fd, 0) == 2, "目录 fd 安装成功");
    CHECK(f_install_task(&target.task, empty_fd, 0) == 3, "空操作 fd 安装成功");
    CHECK(f_install_task(&target.task, seekable_fd, 0) == 4,
            "可定位顺序 fd 安装成功");
    CHECK(f_install_task(&target.task, fsync_only_fd, 0) == 5,
            "完整同步回退 fd 安装成功");
    current = &decoy.task;

    const enum close_race_operation close_races[] = {
        CLOSE_RACE_READ,
        CLOSE_RACE_WRITE,
        CLOSE_RACE_WRITE_CHECK,
        CLOSE_RACE_FSTAT,
        CLOSE_RACE_FTRUNCATE,
    };
    for (size_t index = 0; index < array_size(close_races); index++) {
        CHECK(test_close_during_operation(&target, close_races[index]),
                "fd 表并发关闭不得释放正在操作的文件对象");
    }

    char buffer[16] = {0};
    CHECK(file_read_task(&target.task, 0, buffer, 4) == 4,
            "host-buffer 读取使用显式目标任务");
    CHECK(memcmp(buffer, "righ", 4) == 0 && decoy_io.read_calls == 0,
            "显式读取不得落入 current 的同号 fd");
    CHECK(file_write_task(&target.task, 0, "write", 5) == 5,
            "host-buffer 写入使用显式目标任务");
    CHECK(target_io.output_size == 5 && memcmp(target_io.output, "write", 5) == 0 &&
            decoy_io.write_calls == 0, "显式写入只修改目标 fd");
    CHECK(file_write_check_task(&target.task, 0) == 0 &&
            file_write_check_task(&target.task, 1) == 0,
            "写入预检接受可写与读写 fd");
    CHECK(file_write_check_task(&target.task, 3) == _EBADF &&
            file_write_check_task(&target.task, 99) == _EBADF,
            "写入预检拒绝无写操作与无效 fd");
    CHECK(file_sync_task(&target.task, 0, false) == 0 &&
            target_io.fsync_calls == 1 &&
            target_io.fdatasync_calls == 0,
            "完整同步使用目标任务 fd 的 fsync 回调");
    CHECK(file_sync_task(&target.task, 0, true) == 0 &&
            target_io.fsync_calls == 1 &&
            target_io.fdatasync_calls == 1,
            "数据同步优先使用 fdatasync 回调");
    target_io.fsync_error = _EIO;
    target_io.fdatasync_error = _ENOSPC;
    CHECK(file_sync_task(&target.task, 0, false) == _EIO &&
            file_sync_task(&target.task, 0, true) == _ENOSPC,
            "文件同步原样传播底层错误");
    target_io.fsync_error = 0;
    target_io.fdatasync_error = 0;
    CHECK(file_sync_task(&target.task, 1, false) == _EINVAL &&
            file_sync_task(&target.task, 1, true) == _EINVAL,
            "缺少同步操作的有效 fd 返回 EINVAL");
    CHECK(file_sync_task(&target.task, 5, true) == 0 &&
            fsync_only_io.fsync_calls == 1,
            "缺少 fdatasync 时数据同步回退到更强的 fsync");
    CHECK(file_sync_task(&target.task, 99, false) == _EBADF,
            "文件同步先拒绝无效 fd");

    target_io.size = 8192;
    target_fd->offset = 37;
    CHECK(file_ftruncate_task(&target.task, 0, 4097) == 0 &&
            target_io.size == 4097 && target_io.requested_size == 4097 &&
            target_io.resize_calls == 1 && target_fd->offset == 37,
            "ftruncate 修改文件大小且保持共享顺序位置");
    CHECK(file_ftruncate_task(&target.task, 0, 4097) == 0 &&
            target_io.resize_calls == 2,
            "等长 ftruncate 仍提交底层尺寸操作");
    target_io.resize_error = _ENOSPC;
    CHECK(file_ftruncate_task(&target.task, 0, 3) == _ENOSPC &&
            target_io.size == 4097 && target_io.resize_calls == 3,
            "底层截断失败保持原大小并传播错误");
    target_io.resize_error = 0;
    CHECK(file_grow_fd(target_fd, 1024) == 0 &&
            target_io.resize_calls == 3 && target_io.size == 4097,
            "仅增长入口不得缩短现有文件");
    CHECK(file_grow_fd(target_fd, 8193) == 0 &&
            target_io.resize_calls == 4 && target_io.size == 8193,
            "仅增长入口在目标超过 EOF 时提交尺寸变更");
    target.mount.flags = MS_READONLY_;
    CHECK(file_ftruncate_task(&target.task, 0, 0) == _EROFS &&
            target_io.resize_calls == 4,
            "只读挂载在底层调用前拒绝截断");
    target.mount.flags = 0;
    CHECK(file_ftruncate_task(&target.task, 99, -1) == _EINVAL &&
            file_ftruncate_task(&target.task, 99, 0) == _EBADF,
            "负长度优先于 fd 查表，无效非负 fd 返回 EBADF");
    CHECK(file_ftruncate_task(&target.task, 2, 0) == _EINVAL,
            "ftruncate 对目录返回 EINVAL");
    current = &target.task;
    CHECK(sys_fsync(0) == 0 && sys_fdatasync(0) == 0 &&
            target_io.fsync_calls == 3 &&
            target_io.fdatasync_calls == 3,
            "i386 fsync 与 fdatasync 系统调用选择独立同步模式");
    CHECK(sys_ftruncate(0, 123) == 0 && target_io.size == 123 &&
            target_io.resize_calls == 5,
            "i386 93 按有符号 32 位长度调用公共 ftruncate 服务");
    CHECK(sys_ftruncate64(0, 1, 1) == 0 &&
            target_io.size == UINT64_C(0x100000001) &&
            target_io.resize_calls == 6,
            "i386 194 按 low/high 顺序保留完整 64 位长度");
    CHECK(sys_ftruncate(99, UINT32_MAX) == (dword_t) _EINVAL &&
            sys_ftruncate64(99, 0, UINT32_MAX) ==
                    (dword_t) _EINVAL &&
            sys_truncate(0, UINT32_MAX) == (dword_t) _EINVAL &&
            sys_truncate64(0, 0, UINT32_MAX) ==
                    (dword_t) _EINVAL &&
            target_io.resize_calls == 6,
            "i386 两套 truncate 的负长度均优先于路径或 fd 访问");
    CHECK(sys_ftruncate(99, 0) == (dword_t) _EBADF &&
            sys_ftruncate64(99, 0, 0) == (dword_t) _EBADF,
            "i386 两套 ftruncate 对非负长度的无效 fd 返回 EBADF");
    current = &decoy.task;
    target_fd->flags = O_RDONLY_;
    CHECK(file_write_check_task(&target.task, 0) == _EBADF,
            "写入预检拒绝只读 fd");
    CHECK(file_ftruncate_task(&target.task, 0, 0) == _EINVAL &&
            target_io.resize_calls == 6,
            "ftruncate 对只读 fd 返回 EINVAL 且不触碰底层");
    target_fd->flags = O_RDWR_;

    struct statbuf stat;
    memset(&stat, 0xff, sizeof(stat));
    CHECK(file_fstat_task(&target.task, 0, &stat) == 0,
            "host-buffer fstat 使用显式目标任务");
    CHECK(stat.inode == target_io.inode && stat.dev == 0 &&
            stat.size == target_io.size,
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

    const off_t_ large_offset = INT64_C(0x100000005);
    CHECK(generic_seek(positioned_fd, large_offset,
            LSEEK_SET, 0) == 0 && positioned_fd->offset == large_offset,
            "SEEK_SET 保留超过 4 GiB 的文件位置");
    CHECK(generic_seek(positioned_fd, 7, LSEEK_CUR, 0) == 0 &&
            positioned_fd->offset == large_offset + 7,
            "SEEK_CUR 跨越 4 GiB 后保持 64 位位置");
    off_t_ unchanged_offset = positioned_fd->offset;
    CHECK(generic_seek(positioned_fd, INT64_MAX, LSEEK_CUR, 0) == _EINVAL &&
            positioned_fd->offset == unchanged_offset,
            "SEEK_CUR 溢出失败且不修改原位置");
    CHECK(generic_seek(positioned_fd, -1, LSEEK_SET, 0) == _EINVAL &&
            positioned_fd->offset == unchanged_offset,
            "SEEK_SET 拒绝负位置且不修改原位置");
    CHECK(generic_seek(positioned_fd, 11, LSEEK_END,
            large_offset) == 0 &&
            positioned_fd->offset == large_offset + 11,
            "SEEK_END 接受超过 4 GiB 的文件大小");

    positioned_fd->offset = large_offset;
    memset(buffer, 0, sizeof(buffer));
    CHECK(file_read_task(&target.task, 1, buffer, sizeof(buffer)) == 3,
            "pread 回退返回底层部分读取长度");
    CHECK(positioned_io.positioned_offset == large_offset &&
            positioned_fd->offset == large_offset + 3 &&
            positioned_io.seek_calls == 1, "pread 回退按实际读取量推进 offset");
    CHECK(file_write_task(&target.task, 1, "xy", 2) == 2,
            "pwrite 回退返回底层写入长度");
    CHECK(positioned_io.positioned_offset == large_offset + 3 &&
            positioned_fd->offset == large_offset + 5 &&
            positioned_io.seek_calls == 2, "pwrite 回退按实际写入量推进 offset");
    positioned_io.read_error = _EIO;
    positioned_io.write_error = _ENOSPC;
    CHECK(file_read_task(&target.task, 1, buffer, 1) == _EIO &&
            file_write_task(&target.task, 1, buffer, 1) == _ENOSPC,
            "positioned I/O 保持底层错误");
    CHECK(positioned_fd->offset == large_offset + 5 &&
            positioned_io.seek_calls == 2,
            "positioned I/O 失败时不得推进 offset");

    positioned_fd->flags = O_RDWR_;
    positioned_io.read_error = 0;
    positioned_io.write_error = 0;
    memset(buffer, 0, sizeof(buffer));
    CHECK(file_pread_fd(positioned_fd, buffer, 3, 19) == 3 &&
            positioned_io.positioned_offset == 19 &&
            positioned_fd->offset == large_offset + 5,
            "显式 pread 回调使用指定位置且不改变顺序 offset");
    CHECK(file_pwrite_fd(positioned_fd, "ab", 2, 23) == 2 &&
            positioned_io.positioned_offset == 23 &&
            positioned_fd->offset == large_offset + 5,
            "显式 pwrite 回调使用指定位置且不改变顺序 offset");

    const off_t_ seekable_saved_offset = INT64_C(0x180000029);
    const off_t_ seekable_read_offset = INT64_C(0x180000007);
    const off_t_ seekable_write_offset = INT64_C(0x180000009);
    seekable_fd->offset = seekable_saved_offset;
    memset(buffer, 0, sizeof(buffer));
    CHECK(file_pread_fd(seekable_fd, buffer, 4,
            seekable_read_offset) == 4 &&
            memcmp(buffer, "seek", 4) == 0 &&
            seekable_io.sequential_read_offset == seekable_read_offset &&
            seekable_fd->offset == seekable_saved_offset &&
            seekable_io.seek_calls == 3,
            "read+lseek 回退保留 64 位目标并恢复共享顺序 offset");
    CHECK(file_pwrite_fd(seekable_fd, "back", 4,
            seekable_write_offset) == 4 &&
            seekable_io.output_size == 4 &&
            memcmp(seekable_io.output, "back", 4) == 0 &&
            seekable_io.sequential_write_offset == seekable_write_offset &&
            seekable_fd->offset == seekable_saved_offset &&
            seekable_io.seek_calls == 6,
            "write+lseek 回退保留 64 位目标并恢复共享顺序 offset");
    CHECK(file_pread_fd(target_fd, buffer, 1, 0) == _ESPIPE &&
            file_pwrite_fd(target_fd, buffer, 1, 0) == _ESPIPE,
            "不可定位的顺序 fd 对显式偏移返回 ESPIPE");
    CHECK(file_pread_fd(seekable_fd, buffer, 1, -1) == _EINVAL &&
            file_pwrite_fd(seekable_fd, buffer, 1, -1) == _EINVAL,
            "显式定位读写拒绝负 offset");

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

#include <assert.h>
#include <string.h>

#include "fs/fd.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define AARCH64_LINUX_MAX_RW_COUNT UINT64_C(0x7ffff000)
#define AARCH64_LINUX_IO_CHUNK_SIZE 4096
#define AARCH64_LINUX_O_LARGEFILE UINT32_C(0x8000)

// 该 host-buffer 引导通道按块限制内存；消息型 fd 接入时必须改用保留消息边界的单次 I/O 通道。

enum aarch64_linux_syscall_number {
    AARCH64_LINUX_SYS_GETCWD = 17,
    AARCH64_LINUX_SYS_OPENAT = 56,
    AARCH64_LINUX_SYS_CLOSE = 57,
    AARCH64_LINUX_SYS_READ = 63,
    AARCH64_LINUX_SYS_WRITE = 64,
    AARCH64_LINUX_SYS_NEWFSTATAT = 79,
    AARCH64_LINUX_SYS_FSTAT = 80,
    AARCH64_LINUX_SYS_RT_SIGPROCMASK = 135,
    AARCH64_LINUX_SYS_UNAME = 160,
    AARCH64_LINUX_SYS_GETPID = 172,
    AARCH64_LINUX_SYS_GETPPID = 173,
    AARCH64_LINUX_SYS_GETUID = 174,
    AARCH64_LINUX_SYS_GETEUID = 175,
    AARCH64_LINUX_SYS_GETGID = 176,
    AARCH64_LINUX_SYS_GETEGID = 177,
};

_Static_assert(sizeof(guest_addr_t) == 4,
        "AArch64 系统调用后端必须在官方 i386 内核类型域中编译");

static qword_t syscall_result(sqword_t result) {
    return (qword_t) result;
}

static fd_t syscall_fd(qword_t argument) {
    return (fd_t) (sdword_t) (dword_t) argument;
}

static qword_t user_range_error(struct guest_linux_user_fault *fault,
        qword_t address, enum guest_memory_access access) {
    *fault = (struct guest_linux_user_fault) {
        .address = address,
        .access = (dword_t) access,
        .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
    };
    return syscall_result(_EFAULT);
}

static bool user_range_fits(qword_t address, dword_t size) {
    return size == 0 || address <= UINT64_MAX - (qword_t) size + 1;
}

static qword_t copy_path_from_user(
        const struct guest_linux_syscall_context *context,
        qword_t address, char path[MAX_PATH],
        struct guest_linux_user_fault *fault) {
    assert(context->user.read != NULL);
    for (dword_t index = 0; index < (dword_t) MAX_PATH; index++) {
        if (address > UINT64_MAX - index)
            return user_range_error(fault, address, GUEST_MEMORY_READ);
        if (!context->user.read(context->user.opaque,
                address + index, &path[index], 1, fault))
            return syscall_result(_EFAULT);
        if (path[index] == '\0')
            return 0;
    }
    return syscall_result(_ENAMETOOLONG);
}

static qword_t dispatch_getcwd(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    char path[MAX_PATH + 1];
    qword_t guest_size = syscall->arguments[1];
    size_t host_size = guest_size < sizeof(path) ?
            (size_t) guest_size : sizeof(path);
    ssize_t length = fs_getcwd_task(task, path, host_size);
    if (length < 0)
        return syscall_result(length);
    assert((size_t) length <= sizeof(path));
    dword_t copy_size = (dword_t) length;
    if (!user_range_fits(syscall->arguments[0], copy_size))
        return user_range_error(fault, syscall->arguments[0],
                GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            syscall->arguments[0], path, copy_size, fault))
        return syscall_result(_EFAULT);
    return syscall_result(length);
}

static qword_t dispatch_openat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    const dword_t supported_flags = O_ACCMODE_ | O_CREAT_ | O_EXCL_ |
            O_NOCTTY_ | O_TRUNC_ | O_APPEND_ | O_NONBLOCK_ |
            O_DIRECTORY_ | O_CLOEXEC_;
    dword_t raw_flags = (dword_t) syscall->arguments[2];
    if (raw_flags & ~(supported_flags | AARCH64_LINUX_O_LARGEFILE))
        return syscall_result(_EINVAL);

    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[1], path, fault);
    if ((sqword_t) copied < 0)
        return copied;
    int flags = (int) (raw_flags & supported_flags);
    mode_t_ mode = (mode_t_) (word_t) syscall->arguments[3];
    return syscall_result(file_openat_task(task,
            syscall_fd(syscall->arguments[0]), path, flags, mode));
}

static qword_t dispatch_read(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    fd_t fd = syscall_fd(syscall->arguments[0]);
    qword_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2] < AARCH64_LINUX_MAX_RW_COUNT ?
            syscall->arguments[2] : AARCH64_LINUX_MAX_RW_COUNT;
    byte_t buffer[AARCH64_LINUX_IO_CHUNK_SIZE];
    if (remaining == 0)
        return syscall_result(file_read_task(task, fd, buffer, 0));

    qword_t completed = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        ssize_t read = file_read_task(task, fd, buffer, chunk);
        if (read < 0)
            return completed != 0 ? completed : syscall_result(read);
        assert((size_t) read <= chunk);
        if (read == 0)
            return completed;
        dword_t copy_size = (dword_t) read;
        if (!user_range_fits(address, copy_size))
            return completed != 0 ? completed :
                    user_range_error(fault, address, GUEST_MEMORY_WRITE);
        assert(context->user.write != NULL);
        // 跨页写回失败前可能已复制前缀；文件位置沿用现有 host-buffer 语义，不尝试回滚。
        if (!context->user.write(context->user.opaque,
                address, buffer, copy_size, fault))
            return completed != 0 ? completed : syscall_result(_EFAULT);
        completed += (qword_t) read;
        if ((size_t) read != chunk)
            return completed;
        remaining -= (qword_t) read;
        if (remaining == 0)
            return completed;
        if (address > UINT64_MAX - (qword_t) read) {
            user_range_error(fault, address, GUEST_MEMORY_WRITE);
            return completed;
        }
        address += (qword_t) read;
    }
    return completed;
}

static qword_t dispatch_write(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    fd_t fd = syscall_fd(syscall->arguments[0]);
    qword_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2] < AARCH64_LINUX_MAX_RW_COUNT ?
            syscall->arguments[2] : AARCH64_LINUX_MAX_RW_COUNT;
    byte_t buffer[AARCH64_LINUX_IO_CHUNK_SIZE];
    if (remaining == 0)
        return syscall_result(file_write_task(task, fd, buffer, 0));

    qword_t completed = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        dword_t copy_size = (dword_t) chunk;
        if (!user_range_fits(address, copy_size))
            return completed != 0 ? completed :
                    user_range_error(fault, address, GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address, buffer, copy_size, fault))
            return completed != 0 ? completed : syscall_result(_EFAULT);
        ssize_t written = file_write_task(task, fd, buffer, chunk);
        if (written < 0)
            return completed != 0 ? completed : syscall_result(written);
        assert((size_t) written <= chunk);
        completed += (qword_t) written;
        if ((size_t) written != chunk)
            return completed;
        remaining -= (qword_t) written;
        if (remaining == 0)
            return completed;
        if (address > UINT64_MAX - (qword_t) written) {
            user_range_error(fault, address, GUEST_MEMORY_READ);
            return completed;
        }
        address += (qword_t) written;
    }
    return completed;
}

static struct aarch64_linux_stat pack_stat(const struct statbuf *source) {
    return (struct aarch64_linux_stat) {
        .dev = source->dev,
        .ino = source->inode,
        .mode = source->mode,
        .nlink = source->nlink,
        .uid = source->uid,
        .gid = source->gid,
        .rdev = source->rdev,
        .size = (sqword_t) source->size,
        .blksize = (sdword_t) source->blksize,
        .blocks = (sqword_t) source->blocks,
        .atime_sec = (sqword_t) (sdword_t) source->atime,
        .atime_nsec = source->atime_nsec,
        .mtime_sec = (sqword_t) (sdword_t) source->mtime,
        .mtime_nsec = source->mtime_nsec,
        .ctime_sec = (sqword_t) (sdword_t) source->ctime,
        .ctime_nsec = source->ctime_nsec,
    };
}

static qword_t copy_stat_to_user(
        const struct guest_linux_syscall_context *context,
        const struct statbuf *host_stat, qword_t address,
        struct guest_linux_user_fault *fault) {
    struct aarch64_linux_stat guest_stat = pack_stat(host_stat);
    dword_t size = sizeof(guest_stat);
    if (!user_range_fits(address, size))
        return user_range_error(fault, address, GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, &guest_stat, size, fault))
        return syscall_result(_EFAULT);
    return 0;
}

static qword_t dispatch_newfstatat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    dword_t raw_flags = (dword_t) syscall->arguments[3];
    int flags = (int) (sdword_t) raw_flags;
    fd_t dirfd = syscall_fd(syscall->arguments[0]);
    char path[MAX_PATH];
    qword_t path_error = 0;
    if (syscall->arguments[1] == 0 && (raw_flags & AT_EMPTY_PATH_)) {
        path[0] = '\0';
    } else {
        path_error = copy_path_from_user(
                context, syscall->arguments[1], path, fault);
    }

    struct statbuf host_stat;
    // Linux 对非负 fd 的空名称走 fstat 快路；该分支不消费其余 flags。
    if ((sqword_t) path_error >= 0 && path[0] == '\0' &&
            (raw_flags & AT_EMPTY_PATH_) && dirfd >= 0) {
        int error = file_fstat_task(task, dirfd, &host_stat);
        if (error < 0)
            return syscall_result(error);
        return copy_stat_to_user(context, &host_stat,
                syscall->arguments[2], fault);
    }
    if (flags & ~AT_STATAT_SUPPORTED_FLAGS_) {
        *fault = (struct guest_linux_user_fault) {0};
        return syscall_result(_EINVAL);
    }
    if ((sqword_t) path_error < 0)
        return path_error;

    int error = file_statat_task(task, dirfd, path, flags, &host_stat);
    if (error < 0)
        return syscall_result(error);
    return copy_stat_to_user(context, &host_stat,
            syscall->arguments[2], fault);
}

static qword_t dispatch_fstat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct statbuf host_stat;
    int error = file_fstat_task(
            task, syscall_fd(syscall->arguments[0]), &host_stat);
    if (error < 0)
        return syscall_result(error);
    return copy_stat_to_user(context, &host_stat,
            syscall->arguments[1], fault);
}

static qword_t dispatch_uname(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    dword_t size = sizeof(struct uname);
    qword_t address = syscall->arguments[0];
    if (!user_range_fits(address, size))
        return user_range_error(fault, address, GUEST_MEMORY_WRITE);

    struct uname uts;
    do_uname(&uts, "aarch64");
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, &uts, size, fault))
        return syscall_result(_EFAULT);
    return 0;
}

static qword_t dispatch_rt_sigprocmask(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    if (syscall->arguments[3] != sizeof(sigset_t_))
        return syscall_result(_EINVAL);

    qword_t oldset_address = syscall->arguments[2];
    sigset_t_ oldset;
    if (oldset_address != 0)
        task_sigprocmask(task, 0, NULL, &oldset);

    qword_t set_address = syscall->arguments[1];
    sigset_t_ set;
    if (set_address != 0) {
        if (!user_range_fits(set_address, sizeof(set)))
            return user_range_error(fault, set_address, GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                set_address, &set, sizeof(set), fault))
            return syscall_result(_EFAULT);
        int error = task_sigprocmask(task,
                (dword_t) syscall->arguments[0], &set, NULL);
        if (error < 0)
            return syscall_result(error);
    }
    if (oldset_address == 0)
        return 0;
    if (!user_range_fits(oldset_address, sizeof(oldset)))
        return user_range_error(fault, oldset_address,
                GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            oldset_address, &oldset, sizeof(oldset), fault))
        return syscall_result(_EFAULT);
    return 0;
}

static qword_t dispatch_syscall(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    assert(context != NULL && syscall != NULL && fault != NULL);
    struct task *task = context->task_opaque;
    assert(task != NULL && current == task);
    *fault = (struct guest_linux_user_fault) {0};

    switch (syscall->number) {
        case AARCH64_LINUX_SYS_GETCWD:
            return dispatch_getcwd(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_OPENAT:
            return dispatch_openat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_CLOSE:
            return syscall_result(f_close_task(
                    task, syscall_fd(syscall->arguments[0])));
        case AARCH64_LINUX_SYS_READ:
            return dispatch_read(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_WRITE:
            return dispatch_write(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_NEWFSTATAT:
            return dispatch_newfstatat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_FSTAT:
            return dispatch_fstat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RT_SIGPROCMASK:
            return dispatch_rt_sigprocmask(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_UNAME:
            return dispatch_uname(context, syscall, fault);
        case AARCH64_LINUX_SYS_GETPID:
            return (qword_t) task_getpid(task);
        case AARCH64_LINUX_SYS_GETPPID:
            return (qword_t) task_getppid(task);
        case AARCH64_LINUX_SYS_GETUID:
            return task->uid;
        case AARCH64_LINUX_SYS_GETEUID:
            return task->euid;
        case AARCH64_LINUX_SYS_GETGID:
            return task->gid;
        case AARCH64_LINUX_SYS_GETEGID:
            return task->egid;
        default:
            return syscall_result(_ENOSYS);
    }
}

const struct guest_linux_syscall_service ish_aarch64_linux_syscall_service = {
    .dispatch = dispatch_syscall,
};

#include <assert.h>
#include <string.h>

#include "fs/fd.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-exec.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-wait-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define AARCH64_LINUX_MAX_RW_COUNT UINT64_C(0x7ffff000)
#define AARCH64_LINUX_IO_CHUNK_SIZE 4096
#define AARCH64_LINUX_IOV_MAX UINT64_C(1024)
#define AARCH64_LINUX_IOV_TRANSACTION_LIMIT UINT64_C(0x100000)
#define AARCH64_LINUX_USER_ADDRESS_LIMIT \
    (AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1))

#define AARCH64_LINUX_O_ACCMODE UINT32_C(0x000003)
#define AARCH64_LINUX_O_CREAT UINT32_C(0x000040)
#define AARCH64_LINUX_O_EXCL UINT32_C(0x000080)
#define AARCH64_LINUX_O_NOCTTY UINT32_C(0x000100)
#define AARCH64_LINUX_O_TRUNC UINT32_C(0x000200)
#define AARCH64_LINUX_O_APPEND UINT32_C(0x000400)
#define AARCH64_LINUX_O_NONBLOCK UINT32_C(0x000800)
#define AARCH64_LINUX_O_DIRECTORY UINT32_C(0x004000)
#define AARCH64_LINUX_O_LARGEFILE UINT32_C(0x020000)
#define AARCH64_LINUX_O_CLOEXEC UINT32_C(0x080000)

// 标量 host-buffer 通道按块限制内存；向量写入则先聚合，以保留一次 fd 操作的消息边界。

_Static_assert(SIZE_MAX >= AARCH64_LINUX_MAX_RW_COUNT,
        "Apple host 的 size_t 必须容纳 Linux MAX_RW_COUNT");
_Static_assert(AARCH64_LINUX_IOV_TRANSACTION_LIMIT <=
        AARCH64_LINUX_MAX_RW_COUNT,
        "向量事务缓冲上限不得超过 Linux MAX_RW_COUNT");

enum aarch64_linux_syscall_number {
    AARCH64_LINUX_SYS_GETCWD = 17,
    AARCH64_LINUX_SYS_OPENAT = 56,
    AARCH64_LINUX_SYS_CLOSE = 57,
    AARCH64_LINUX_SYS_GETDENTS64 = 61,
    AARCH64_LINUX_SYS_READ = 63,
    AARCH64_LINUX_SYS_WRITE = 64,
    AARCH64_LINUX_SYS_WRITEV = 66,
    AARCH64_LINUX_SYS_NEWFSTATAT = 79,
    AARCH64_LINUX_SYS_FSTAT = 80,
    AARCH64_LINUX_SYS_SIGALTSTACK = 132,
    AARCH64_LINUX_SYS_RT_SIGACTION = 134,
    AARCH64_LINUX_SYS_RT_SIGPROCMASK = 135,
    AARCH64_LINUX_SYS_RT_SIGPENDING = 136,
    AARCH64_LINUX_SYS_GETGROUPS = 158,
    AARCH64_LINUX_SYS_UNAME = 160,
    AARCH64_LINUX_SYS_GETPID = 172,
    AARCH64_LINUX_SYS_GETPPID = 173,
    AARCH64_LINUX_SYS_GETUID = 174,
    AARCH64_LINUX_SYS_GETEUID = 175,
    AARCH64_LINUX_SYS_GETGID = 176,
    AARCH64_LINUX_SYS_GETEGID = 177,
    AARCH64_LINUX_SYS_CLONE = 220,
    AARCH64_LINUX_SYS_EXECVE = 221,
    AARCH64_LINUX_SYS_WAIT4 = 260,
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

static bool user_range_fits(qword_t address, qword_t size) {
    return size == 0 || address <= UINT64_MAX - (qword_t) size + 1;
}

static bool aarch64_user_range_fits(qword_t address, qword_t size) {
    return address <= AARCH64_LINUX_USER_ADDRESS_LIMIT &&
            size <= AARCH64_LINUX_USER_ADDRESS_LIMIT - address;
}

static int copy_iovecs_from_user(
        const struct guest_linux_syscall_context *context,
        qword_t address, qword_t count,
        struct aarch64_linux_iovec **vectors_out, qword_t *total_out,
        struct guest_linux_user_fault *fault) {
    *vectors_out = NULL;
    *total_out = 0;
    if (count > AARCH64_LINUX_IOV_MAX)
        return _EINVAL;
    if (count == 0)
        return 0;

    dword_t byte_count = (dword_t) count *
            (dword_t) sizeof(struct aarch64_linux_iovec);
    if (!aarch64_user_range_fits(address, byte_count)) {
        user_range_error(fault, address, GUEST_MEMORY_READ);
        return _EFAULT;
    }
    struct aarch64_linux_iovec *vectors = malloc(byte_count);
    if (vectors == NULL)
        return _ENOMEM;
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, vectors, byte_count, fault)) {
        free(vectors);
        return _EFAULT;
    }

    qword_t total = 0;
    for (size_t index = 0; index < (size_t) count; index++) {
        qword_t length = vectors[index].length;
        if ((sqword_t) length < 0) {
            free(vectors);
            return _EINVAL;
        }
        qword_t checked_length = length;
        if (count == 1 && checked_length > AARCH64_LINUX_MAX_RW_COUNT)
            checked_length = AARCH64_LINUX_MAX_RW_COUNT;
        if (!aarch64_user_range_fits(
                vectors[index].base, checked_length)) {
            user_range_error(fault, vectors[index].base,
                    GUEST_MEMORY_READ);
            free(vectors);
            return _EFAULT;
        }
        qword_t available = AARCH64_LINUX_MAX_RW_COUNT - total;
        total += length < available ? length : available;
    }
    *vectors_out = vectors;
    *total_out = total;
    return 0;
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

static int copy_string_array_from_user(
        const struct guest_linux_syscall_context *context,
        qword_t address, char *buffer, size_t capacity,
        size_t *count, size_t *budget, bool normalize_empty,
        struct guest_linux_user_fault *fault) {
    *count = 0;
    size_t used = 0;
    if (address == 0) {
        buffer[0] = '\0';
        size_t required = sizeof(qword_t);
        if (normalize_empty)
            required += sizeof(qword_t) + 1;
        if (*budget < required)
            return _E2BIG;
        *budget -= required;
        if (normalize_empty) {
            buffer[1] = '\0';
            *count = 1;
        }
        return 0;
    }
    while (true) {
        if (*budget < sizeof(qword_t))
            return _E2BIG;
        *budget -= sizeof(qword_t);
        qword_t pointer_offset = (qword_t) *count * sizeof(qword_t);
        if (pointer_offset / sizeof(qword_t) != *count ||
                address > UINT64_MAX - pointer_offset) {
            user_range_error(fault, address, GUEST_MEMORY_READ);
            return _EFAULT;
        }
        qword_t string_address;
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address + pointer_offset, &string_address,
                sizeof(string_address), fault))
            return _EFAULT;
        if (string_address == 0)
            break;

        while (true) {
            if (used == capacity || *budget == 0)
                return _E2BIG;
            if (!context->user.read(context->user.opaque,
                    string_address, &buffer[used], 1, fault))
                return _EFAULT;
            char copied = buffer[used++];
            (*budget)--;
            if (copied == '\0')
                break;
            if (string_address == UINT64_MAX) {
                user_range_error(fault,
                        string_address, GUEST_MEMORY_READ);
                return _EFAULT;
            }
            string_address++;
        }
        (*count)++;
    }
    if (*count == 0 && normalize_empty) {
        if (capacity < 2 || *budget < sizeof(qword_t) + 1)
            return _E2BIG;
        *budget -= sizeof(qword_t) + 1;
        buffer[0] = '\0';
        buffer[1] = '\0';
        *count = 1;
        return 0;
    }
    if (used == capacity)
        return _E2BIG;
    buffer[used] = '\0';
    return 0;
}

static qword_t dispatch_execve(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    if (context->completion == NULL)
        return syscall_result(_EINVAL);

    char filename[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[0], filename, fault);
    if ((sqword_t) copied < 0)
        return copied;

    char *arguments = malloc(ISH_AARCH64_EXEC_ARG_MAX);
    char *environment = malloc(ISH_AARCH64_EXEC_ARG_MAX);
    if (arguments == NULL || environment == NULL) {
        free(environment);
        free(arguments);
        return syscall_result(_ENOMEM);
    }
    size_t argument_count;
    size_t argument_budget = ISH_AARCH64_EXEC_ARG_MAX;
    int error = copy_string_array_from_user(context,
            syscall->arguments[1], arguments,
            ISH_AARCH64_EXEC_ARG_MAX, &argument_count,
            &argument_budget, true, fault);
    size_t environment_count = 0;
    if (error == 0) {
        error = copy_string_array_from_user(context,
                syscall->arguments[2], environment,
                ISH_AARCH64_EXEC_ARG_MAX, &environment_count,
                &argument_budget, false, fault);
    }
    if (error == 0) {
        error = do_execve(filename, argument_count,
                arguments, environment);
        if (error == 0) {
            assert(task_has_aarch64_exec_candidate(task));
            context->completion->disposition =
                    GUEST_LINUX_SYSCALL_REPLACED_IMAGE;
        }
    }
    free(environment);
    free(arguments);
    return syscall_result(error);
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
    dword_t raw_flags = (dword_t) syscall->arguments[2];
    const dword_t supported_flags = AARCH64_LINUX_O_ACCMODE |
            AARCH64_LINUX_O_CREAT | AARCH64_LINUX_O_EXCL |
            AARCH64_LINUX_O_NOCTTY | AARCH64_LINUX_O_TRUNC |
            AARCH64_LINUX_O_APPEND | AARCH64_LINUX_O_NONBLOCK |
            AARCH64_LINUX_O_DIRECTORY | AARCH64_LINUX_O_LARGEFILE |
            AARCH64_LINUX_O_CLOEXEC;
    dword_t access_mode = raw_flags & AARCH64_LINUX_O_ACCMODE;
    if (access_mode == AARCH64_LINUX_O_ACCMODE ||
            (raw_flags & ~supported_flags) != 0 ||
            (raw_flags & (AARCH64_LINUX_O_CREAT |
                    AARCH64_LINUX_O_DIRECTORY)) ==
                    (AARCH64_LINUX_O_CREAT |
                    AARCH64_LINUX_O_DIRECTORY))
        return syscall_result(_EINVAL);

    int flags = access_mode == 1 ? O_WRONLY_ :
            access_mode == 2 ? O_RDWR_ : O_RDONLY_;
    static const struct {
        dword_t guest;
        int internal;
    } mappings[] = {
        {AARCH64_LINUX_O_CREAT, O_CREAT_},
        {AARCH64_LINUX_O_EXCL, O_EXCL_},
        {AARCH64_LINUX_O_NOCTTY, O_NOCTTY_},
        {AARCH64_LINUX_O_TRUNC, O_TRUNC_},
        {AARCH64_LINUX_O_APPEND, O_APPEND_},
        {AARCH64_LINUX_O_NONBLOCK, O_NONBLOCK_},
        {AARCH64_LINUX_O_DIRECTORY, O_DIRECTORY_},
        {AARCH64_LINUX_O_CLOEXEC, O_CLOEXEC_},
    };
    for (size_t index = 0; index < array_size(mappings); index++)
        if (raw_flags & mappings[index].guest)
            flags |= mappings[index].internal;

    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[1], path, fault);
    if ((sqword_t) copied < 0)
        return copied;
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

static qword_t dispatch_writev(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct fd *target = f_get_task_retain(
            task, syscall_fd(syscall->arguments[0]));
    if (target == NULL)
        return syscall_result(_EBADF);
    int error = file_write_check_fd(target);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }

    struct aarch64_linux_iovec *vectors;
    qword_t total;
    error = copy_iovecs_from_user(context,
            syscall->arguments[1], syscall->arguments[2],
            &vectors, &total, fault);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }
    if (total == 0) {
        free(vectors);
        fd_close(target);
        return 0;
    }
    if (total > AARCH64_LINUX_IOV_TRANSACTION_LIMIT) {
        free(vectors);
        fd_close(target);
        return syscall_result(_ENOMEM);
    }

    // 有界聚合后只执行一次 fd 写入；既保留消息边界，也避免 watchOS 出现不可控内存峰值。
    byte_t *buffer = malloc((size_t) total);
    if (buffer == NULL) {
        free(vectors);
        fd_close(target);
        return syscall_result(_ENOMEM);
    }
    qword_t copied = 0;
    for (size_t index = 0;
            index < (size_t) syscall->arguments[2] && copied < total;
            index++) {
        qword_t length = vectors[index].length;
        qword_t remaining = total - copied;
        if (length > remaining)
            length = remaining;
        if (length == 0)
            continue;
        assert(length <= UINT32_MAX);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                vectors[index].base, buffer + (size_t) copied,
                (dword_t) length, fault)) {
            free(buffer);
            free(vectors);
            fd_close(target);
            return syscall_result(_EFAULT);
        }
        copied += length;
    }
    assert(copied == total);
    ssize_t written = file_write_fd(target, buffer, (size_t) total);
    assert(written < 0 || (qword_t) written <= total);
    free(buffer);
    free(vectors);
    fd_close(target);
    return syscall_result(written);
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

static qword_t dispatch_getgroups(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    sdword_t capacity = (sdword_t) (dword_t) syscall->arguments[0];
    if (capacity < 0)
        return syscall_result(_EINVAL);
    assert(task->ngroups <= MAX_GROUPS);
    if (capacity == 0)
        return task->ngroups;
    if ((dword_t) capacity < task->ngroups)
        return syscall_result(_EINVAL);
    if (task->ngroups == 0)
        return 0;

    dword_t size = (dword_t) (
            task->ngroups * sizeof(task->groups[0]));
    qword_t address = syscall->arguments[1];
    if (!aarch64_user_range_fits(address, size))
        return user_range_error(fault, address, GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, task->groups, size, fault))
        return syscall_result(_EFAULT);
    return task->ngroups;
}

struct aarch64_getdents_context {
    const struct guest_linux_syscall_context *syscall;
    struct guest_linux_user_fault *fault;
    qword_t address;
    dword_t remaining;
};

static sqword_t emit_aarch64_dirent(void *opaque,
        const struct dir_entry *entry, unsigned long next_position) {
    struct aarch64_getdents_context *context = opaque;
    size_t name_size = strlen(entry->name) + 1;
    size_t unaligned = AARCH64_LINUX_DIRENT64_NAME_OFFSET + name_size;
    const size_t alignment = AARCH64_LINUX_DIRENT64_ALIGNMENT;
    dword_t length = (dword_t) ((unaligned + alignment - 1) &
            ~(alignment - 1));
    assert(length <= AARCH64_LINUX_DIRENT64_MAX_SIZE);
    if (length > context->remaining)
        return _EINVAL;

    byte_t record[AARCH64_LINUX_DIRENT64_MAX_SIZE] = {0};
    struct aarch64_linux_dirent64 *wire = (void *) record;
    wire->inode = entry->inode;
    wire->next_offset = (sqword_t) (qword_t) next_position;
    wire->length = (word_t) length;
    wire->type = 0;
    memcpy(record + AARCH64_LINUX_DIRENT64_NAME_OFFSET,
            entry->name, name_size);

    if (!aarch64_user_range_fits(context->address, length)) {
        user_range_error(context->fault, context->address,
                GUEST_MEMORY_WRITE);
        return _EFAULT;
    }
    assert(context->syscall->user.write != NULL);
    if (!context->syscall->user.write(context->syscall->user.opaque,
            context->address, record, length, context->fault))
        return _EFAULT;
    context->address += length;
    context->remaining -= length;
    return (sqword_t) length;
}

static qword_t dispatch_getdents64(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct aarch64_getdents_context output = {
        .syscall = context,
        .fault = fault,
        .address = syscall->arguments[1],
        .remaining = (dword_t) syscall->arguments[2],
    };
    return syscall_result(file_getdents_task(task,
            syscall_fd(syscall->arguments[0]),
            emit_aarch64_dirent, &output));
}

static qword_t dispatch_rt_sigprocmask(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    if (syscall->arguments[3] != sizeof(sigset_t_))
        return syscall_result(_EINVAL);

    qword_t set_address = syscall->arguments[1];
    sigset_t_ set;
    if (set_address != 0) {
        if (!user_range_fits(set_address, sizeof(set)))
            return user_range_error(fault, set_address, GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                set_address, &set, sizeof(set), fault))
            return syscall_result(_EFAULT);
    }

    qword_t oldset_address = syscall->arguments[2];
    sigset_t_ oldset;
    int error = task_sigprocmask(task,
            (dword_t) syscall->arguments[0],
            set_address != 0 ? &set : NULL,
            oldset_address != 0 ? &oldset : NULL);
    if (error < 0)
        return syscall_result(error);
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

static qword_t dispatch_sigaltstack(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    qword_t new_address = syscall->arguments[0];
    struct signal_altstack new_stack;
    if (new_address != 0) {
        struct aarch64_linux_stack wire;
        if (!user_range_fits(new_address, sizeof(wire)))
            return user_range_error(fault, new_address,
                    GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                new_address, &wire, sizeof(wire), fault))
            return syscall_result(_EFAULT);
        if (wire.flags != 0 && wire.flags != SS_DISABLE_)
            return syscall_result(_EINVAL);
        new_stack = (struct signal_altstack) {
            .stack = wire.sp,
            .size = wire.size,
            .flags = (dword_t) wire.flags,
        };
    }

    qword_t old_address = syscall->arguments[1];
    struct signal_altstack old_stack;
    int error = task_sigaltstack(task, context->stack_pointer,
            new_address != 0 ? &new_stack : NULL,
            old_address != 0 ? &old_stack : NULL,
            AARCH64_LINUX_MINSIGSTKSZ,
            AARCH64_LINUX_USER_ADDRESS_MAX);
    if (error < 0)
        return syscall_result(error);
    if (old_address == 0)
        return 0;

    struct aarch64_linux_stack wire_old = {
        .sp = old_stack.stack,
        .flags = (sdword_t) old_stack.flags,
        .size = old_stack.size,
    };
    if (!user_range_fits(old_address, sizeof(wire_old)))
        return user_range_error(fault, old_address,
                GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            old_address, &wire_old, sizeof(wire_old), fault))
        return syscall_result(_EFAULT);
    return 0;
}

static qword_t dispatch_rt_sigpending(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    qword_t requested_size = syscall->arguments[1];
    if (requested_size > sizeof(sigset_t_))
        return syscall_result(_EINVAL);

    dword_t size = (dword_t) requested_size;
    sigset_t_ pending = task_sigpending(task);
    if (size == 0)
        return 0;
    qword_t address = syscall->arguments[0];
    if (!user_range_fits(address, size))
        return user_range_error(fault, address, GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, &pending, size, fault))
        return syscall_result(_EFAULT);
    return 0;
}

static struct signal_action unpack_aarch64_sigaction(
        const struct aarch64_linux_sigaction *wire) {
    return (struct signal_action) {
        .handler = wire->handler,
        .flags = wire->flags & AARCH64_LINUX_SA_SUPPORTED_FLAGS,
        .restorer = wire->restorer,
        .mask = wire->mask,
    };
}

static struct aarch64_linux_sigaction pack_aarch64_sigaction(
        const struct signal_action *action) {
    return (struct aarch64_linux_sigaction) {
        .handler = action->handler,
        .flags = action->flags & AARCH64_LINUX_SA_SUPPORTED_FLAGS,
        .restorer = action->restorer,
        .mask = action->mask,
    };
}

static qword_t dispatch_rt_sigaction(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    if (syscall->arguments[3] != sizeof(sigset_t_))
        return syscall_result(_EINVAL);

    qword_t action_address = syscall->arguments[1];
    struct signal_action action;
    if (action_address != 0) {
        struct aarch64_linux_sigaction wire_action;
        if (!user_range_fits(action_address, sizeof(wire_action)))
            return user_range_error(fault, action_address,
                    GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                action_address, &wire_action, sizeof(wire_action), fault))
            return syscall_result(_EFAULT);
        action = unpack_aarch64_sigaction(&wire_action);
    }

    qword_t oldaction_address = syscall->arguments[2];
    struct signal_action oldaction;
    int error = task_sigaction(task,
            (sdword_t) (dword_t) syscall->arguments[0],
            action_address != 0 ? &action : NULL,
            oldaction_address != 0 ? &oldaction : NULL);
    if (error < 0)
        return syscall_result(error);
    if (oldaction_address == 0)
        return 0;

    struct aarch64_linux_sigaction wire_oldaction =
            pack_aarch64_sigaction(&oldaction);
    if (!user_range_fits(oldaction_address, sizeof(wire_oldaction)))
        return user_range_error(fault, oldaction_address,
                GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            oldaction_address, &wire_oldaction,
            sizeof(wire_oldaction), fault))
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
        case AARCH64_LINUX_SYS_GETDENTS64:
            return dispatch_getdents64(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_READ:
            return dispatch_read(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_WRITE:
            return dispatch_write(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_WRITEV:
            return dispatch_writev(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_NEWFSTATAT:
            return dispatch_newfstatat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_FSTAT:
            return dispatch_fstat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_SIGALTSTACK:
            return dispatch_sigaltstack(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RT_SIGACTION:
            return dispatch_rt_sigaction(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RT_SIGPROCMASK:
            return dispatch_rt_sigprocmask(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RT_SIGPENDING:
            return dispatch_rt_sigpending(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_GETGROUPS:
            return dispatch_getgroups(
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
        case AARCH64_LINUX_SYS_CLONE:
            // 旧 clone ABI 的高 32 位不参与 flags；未请求的尾参数不读取。
            if ((dword_t) syscall->arguments[0] != SIGCHLD_ ||
                    syscall->arguments[1] != 0)
                return syscall_result(_EINVAL);
            assert(task_has_aarch64_process(task));
            return syscall_result((sdword_t) sys_clone(
                    (dword_t) syscall->arguments[0], 0, 0, 0, 0));
        case AARCH64_LINUX_SYS_EXECVE:
            return dispatch_execve(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_WAIT4:
            return aarch64_linux_dispatch_wait4(
                    context, syscall, fault);
        default:
            return syscall_result(_ENOSYS);
    }
}

const struct guest_linux_syscall_service ish_aarch64_linux_syscall_service = {
    .dispatch = dispatch_syscall,
};

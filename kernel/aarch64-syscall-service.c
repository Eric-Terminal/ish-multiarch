#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/tty.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/aarch64/linux-futex-abi.h"
#include "guest/aarch64/linux-process-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-signal-info.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-exec.h"
#include "kernel/aarch64-fd-service.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-time-service.h"
#include "kernel/aarch64-wait-service.h"
#include "kernel/calls.h"
#include "kernel/epoll.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "util/timer.h"

#define AARCH64_LINUX_MAX_RW_COUNT UINT64_C(0x7ffff000)
#define AARCH64_LINUX_IO_CHUNK_SIZE 4096
#define AARCH64_LINUX_IOV_MAX UINT64_C(1024)
#define AARCH64_LINUX_IOV_TRANSACTION_LIMIT UINT64_C(0x100000)
#define AARCH64_LINUX_MAX_PID_NS_LEVEL UINT64_C(32)
#define AARCH64_LINUX_CLONE3_ZERO_CHUNK 32
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
    AARCH64_LINUX_SYS_EPOLL_CREATE1 = 20,
    AARCH64_LINUX_SYS_EPOLL_CTL = 21,
    AARCH64_LINUX_SYS_EPOLL_PWAIT = 22,
    AARCH64_LINUX_SYS_DUP = 23,
    AARCH64_LINUX_SYS_DUP3 = 24,
    AARCH64_LINUX_SYS_FCNTL = 25,
    AARCH64_LINUX_SYS_IOCTL = 29,
    AARCH64_LINUX_SYS_MKDIRAT = 34,
    AARCH64_LINUX_SYS_UNLINKAT = 35,
    AARCH64_LINUX_SYS_RENAMEAT = 38,
    AARCH64_LINUX_SYS_TRUNCATE = 45,
    AARCH64_LINUX_SYS_FTRUNCATE = 46,
    AARCH64_LINUX_SYS_CHDIR = 49,
    AARCH64_LINUX_SYS_FCHDIR = 50,
    AARCH64_LINUX_SYS_OPENAT = 56,
    AARCH64_LINUX_SYS_CLOSE = 57,
    AARCH64_LINUX_SYS_PIPE2 = 59,
    AARCH64_LINUX_SYS_GETDENTS64 = 61,
    AARCH64_LINUX_SYS_LSEEK = 62,
    AARCH64_LINUX_SYS_READ = 63,
    AARCH64_LINUX_SYS_WRITE = 64,
    AARCH64_LINUX_SYS_READV = 65,
    AARCH64_LINUX_SYS_WRITEV = 66,
    AARCH64_LINUX_SYS_PREAD64 = 67,
    AARCH64_LINUX_SYS_PWRITE64 = 68,
    AARCH64_LINUX_SYS_PREADV = 69,
    AARCH64_LINUX_SYS_PWRITEV = 70,
    AARCH64_LINUX_SYS_PSELECT6 = 72,
    AARCH64_LINUX_SYS_PPOLL = 73,
    AARCH64_LINUX_SYS_READLINKAT = 78,
    AARCH64_LINUX_SYS_NEWFSTATAT = 79,
    AARCH64_LINUX_SYS_FSTAT = 80,
    AARCH64_LINUX_SYS_FSYNC = 82,
    AARCH64_LINUX_SYS_FDATASYNC = 83,
    AARCH64_LINUX_SYS_FUTEX = 98,
    AARCH64_LINUX_SYS_SET_ROBUST_LIST = 99,
    AARCH64_LINUX_SYS_GET_ROBUST_LIST = 100,
    AARCH64_LINUX_SYS_NANOSLEEP = 101,
    AARCH64_LINUX_SYS_CLOCK_GETTIME = 113,
    AARCH64_LINUX_SYS_KILL = 129,
    AARCH64_LINUX_SYS_TKILL = 130,
    AARCH64_LINUX_SYS_TGKILL = 131,
    AARCH64_LINUX_SYS_SIGALTSTACK = 132,
    AARCH64_LINUX_SYS_RT_SIGSUSPEND = 133,
    AARCH64_LINUX_SYS_RT_SIGACTION = 134,
    AARCH64_LINUX_SYS_RT_SIGPROCMASK = 135,
    AARCH64_LINUX_SYS_RT_SIGPENDING = 136,
    AARCH64_LINUX_SYS_RT_SIGQUEUEINFO = 138,
    AARCH64_LINUX_SYS_GETGROUPS = 158,
    AARCH64_LINUX_SYS_UNAME = 160,
    AARCH64_LINUX_SYS_GETPID = 172,
    AARCH64_LINUX_SYS_GETPPID = 173,
    AARCH64_LINUX_SYS_GETUID = 174,
    AARCH64_LINUX_SYS_GETEUID = 175,
    AARCH64_LINUX_SYS_GETGID = 176,
    AARCH64_LINUX_SYS_GETEGID = 177,
    AARCH64_LINUX_SYS_SOCKET = 198,
    AARCH64_LINUX_SYS_BIND = 200,
    AARCH64_LINUX_SYS_CONNECT = 203,
    AARCH64_LINUX_SYS_SENDTO = 206,
    AARCH64_LINUX_SYS_RECVFROM = 207,
    AARCH64_LINUX_SYS_SETSOCKOPT = 208,
    AARCH64_LINUX_SYS_CLONE = 220,
    AARCH64_LINUX_SYS_EXECVE = 221,
    AARCH64_LINUX_SYS_RT_TGSIGQUEUEINFO = 240,
    AARCH64_LINUX_SYS_WAIT4 = 260,
    AARCH64_LINUX_SYS_RENAMEAT2 = 276,
    AARCH64_LINUX_SYS_PREADV2 = 286,
    AARCH64_LINUX_SYS_PWRITEV2 = 287,
    AARCH64_LINUX_SYS_CLONE3 = 435,
    AARCH64_LINUX_SYS_FUTEX_WAITV = 449,
};

_Static_assert(sizeof(guest_addr_t) == 4,
        "AArch64 系统调用后端必须在官方 i386 内核类型域中编译");
_Static_assert(sizeof(struct pollfd_) == 8 &&
        __builtin_offsetof(struct pollfd_, fd) == 0 &&
        __builtin_offsetof(struct pollfd_, events) == 4 &&
        __builtin_offsetof(struct pollfd_, revents) == 6,
        "AArch64 pollfd 必须保持 8 字节 Linux wire 布局");

static qword_t syscall_result(sqword_t result) {
    return (qword_t) result;
}

static bool socket_timeout_enabled(
        struct fd *socket, bool receive) {
    if (socket == NULL || socket->ops != &socket_fdops)
        return false;
    struct timeval timeout = {0};
    socklen_t length = sizeof(timeout);
    int option = receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    return getsockopt(socket->real_fd, SOL_SOCKET,
            option, &timeout, &length) == 0 &&
            (timeout.tv_sec != 0 || timeout.tv_usec != 0);
}

static void complete_socket_interrupt(
        const struct guest_linux_syscall_context *context,
        bool timeout_enabled, sqword_t result) {
    if (result == _EINTR && timeout_enabled &&
            context->completion != NULL)
        context->completion->restart =
                GUEST_LINUX_SYSCALL_RESTART_NEVER;
}

static fd_t syscall_fd(qword_t argument) {
    return (fd_t) (sdword_t) (dword_t) argument;
}

static pid_t_ syscall_pid(qword_t argument) {
    return (pid_t_) (sdword_t) (dword_t) argument;
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

static int check_clone3_zero_tail(
        const struct guest_linux_syscall_context *context,
        qword_t address, qword_t size,
        struct guest_linux_user_fault *fault) {
    byte_t bytes[AARCH64_LINUX_CLONE3_ZERO_CHUNK];
    while (size != 0) {
        dword_t chunk = size < sizeof(bytes) ?
                (dword_t) size : (dword_t) sizeof(bytes);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address, bytes, chunk, fault))
            return _EFAULT;
        for (dword_t index = 0; index < chunk; index++) {
            if (bytes[index] != 0)
                return _E2BIG;
        }
        address += chunk;
        size -= chunk;
    }
    return 0;
}

static int copy_clone3_args_from_user(
        const struct guest_linux_syscall_context *context,
        qword_t address, qword_t size,
        struct aarch64_linux_clone_args *args,
        struct guest_linux_user_fault *fault) {
    if (size > GUEST_MEMORY_PAGE_SIZE)
        return _E2BIG;
    if (size < AARCH64_LINUX_CLONE_ARGS_SIZE_VER0)
        return _EINVAL;
    if (!aarch64_user_range_fits(address, size)) {
        (void) user_range_error(fault, address, GUEST_MEMORY_READ);
        return _EFAULT;
    }

    if (size > sizeof(*args)) {
        int error = check_clone3_zero_tail(context,
                address + sizeof(*args), size - sizeof(*args), fault);
        if (error < 0)
            return error;
    }

    *args = (struct aarch64_linux_clone_args) {0};
    dword_t copy_size = size < sizeof(*args) ?
            (dword_t) size : (dword_t) sizeof(*args);
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, args, copy_size, fault))
        return _EFAULT;
    return 0;
}

static int validate_clone3_args(
        const struct aarch64_linux_clone_args *args,
        qword_t size, qword_t *stack_pointer) {
    if (args->set_tid_size > AARCH64_LINUX_MAX_PID_NS_LEVEL)
        return _EINVAL;
    if ((args->set_tid == 0) != (args->set_tid_size == 0))
        return _EINVAL;
    if ((args->exit_signal & ~AARCH64_LINUX_CSIGNAL) != 0)
        return _EINVAL;
    if ((args->flags & AARCH64_LINUX_CLONE_INTO_CGROUP) != 0 &&
            (size < AARCH64_LINUX_CLONE_ARGS_SIZE_VER2 ||
                    args->cgroup > INT_MAX))
        return _EINVAL;

    if ((args->flags & (AARCH64_LINUX_CLONE_DETACHED |
                    (AARCH64_LINUX_CSIGNAL &
                            ~AARCH64_LINUX_CLONE_NEWTIME))) != 0)
        return _EINVAL;
    if ((args->flags & (AARCH64_LINUX_CLONE_SIGHAND |
                    AARCH64_LINUX_CLONE_CLEAR_SIGHAND)) ==
            (AARCH64_LINUX_CLONE_SIGHAND |
                    AARCH64_LINUX_CLONE_CLEAR_SIGHAND))
        return _EINVAL;
    if ((args->flags & (AARCH64_LINUX_CLONE_THREAD |
                    AARCH64_LINUX_CLONE_PARENT)) != 0 &&
            args->exit_signal != 0)
        return _EINVAL;

    if ((args->stack == 0) != (args->stack_size == 0))
        return _EINVAL;
    if (args->stack != 0 && !aarch64_user_range_fits(
            args->stack, args->stack_size))
        return _EINVAL;
    if (args->exit_signal > NUM_SIGS)
        return _EINVAL;

    // 当前 PID 分配器不支持请求指定 PID；非零 set_tid 必须显式失败。
    if (args->set_tid != 0)
        return _EINVAL;
    if ((args->flags & ~AARCH64_LINUX_CLONE_SUPPORTED_FLAGS) != 0)
        return _EINVAL;

    *stack_pointer = args->stack == 0 ? 0 :
            args->stack + args->stack_size;
    return 0;
}

static qword_t dispatch_clone3(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct aarch64_linux_clone_args args;
    qword_t size = syscall->arguments[1];
    int error = copy_clone3_args_from_user(context,
            syscall->arguments[0], size, &args, fault);
    if (error < 0)
        return syscall_result(error);

    qword_t stack_pointer;
    error = validate_clone3_args(&args, size, &stack_pointer);
    if (error < 0)
        return syscall_result(error);
    if (!task_has_aarch64_process(task))
        return syscall_result(_EINVAL);

    dword_t legacy_flags = (dword_t) args.flags |
            (dword_t) args.exit_signal;
    return syscall_result((sdword_t) sys_clone_aarch64(
            legacy_flags, stack_pointer, args.parent_tid,
            args.tls, args.child_tid, fault));
}

static int copy_iovecs_from_user(
        const struct guest_linux_syscall_context *context,
        qword_t address, qword_t count,
        enum guest_memory_access payload_access,
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
                    payload_access);
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

static qword_t dispatch_readv_at(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault,
        bool positioned, off_t_ offset, dword_t flags) {
    if (positioned && offset < 0)
        return syscall_result(_EINVAL);
    struct fd *target = f_get_task_retain(
            task, syscall_fd(syscall->arguments[0]));
    if (target == NULL)
        return syscall_result(_EBADF);
    int error = file_read_check_fd(target);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }

    struct aarch64_linux_iovec *vectors;
    qword_t total;
    error = copy_iovecs_from_user(context,
            syscall->arguments[1], syscall->arguments[2],
            GUEST_MEMORY_WRITE, &vectors, &total, fault);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }
    if (total == 0) {
        free(vectors);
        fd_close(target);
        return 0;
    }
    if (flags != 0) {
        free(vectors);
        fd_close(target);
        return syscall_result(_EOPNOTSUPP);
    }
    if (total > AARCH64_LINUX_IOV_TRANSACTION_LIMIT) {
        free(vectors);
        fd_close(target);
        return syscall_result(_ENOMEM);
    }
    if (positioned && total > (qword_t) INT64_MAX - (qword_t) offset) {
        free(vectors);
        fd_close(target);
        return syscall_result(_EINVAL);
    }

    // 单次读取保留管道与套接字的消息语义，再按实际读取长度分散写回。
    byte_t *buffer = malloc((size_t) total);
    if (buffer == NULL) {
        free(vectors);
        fd_close(target);
        return syscall_result(_ENOMEM);
    }
    bool timeout_enabled = !positioned &&
            socket_timeout_enabled(target, true);
    ssize_t read = positioned ?
            file_pread_fd(target, buffer, (size_t) total, offset) :
            file_read_fd(target, buffer, (size_t) total);
    assert(read < 0 || (qword_t) read <= total);
    if (read <= 0) {
        if (!positioned)
            complete_socket_interrupt(
                    context, timeout_enabled, read);
        free(buffer);
        free(vectors);
        fd_close(target);
        return syscall_result(read);
    }

    qword_t copied = 0;
    for (size_t index = 0;
            index < (size_t) syscall->arguments[2] && copied < (qword_t) read;
            index++) {
        qword_t length = vectors[index].length;
        qword_t remaining = (qword_t) read - copied;
        if (length > remaining)
            length = remaining;
        if (length == 0)
            continue;
        assert(length <= UINT32_MAX);
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                vectors[index].base, buffer + (size_t) copied,
                (dword_t) length, fault)) {
            qword_t result = copied != 0 ? copied : syscall_result(_EFAULT);
            free(buffer);
            free(vectors);
            fd_close(target);
            return result;
        }
        copied += length;
    }
    assert(copied == (qword_t) read);
    free(buffer);
    free(vectors);
    fd_close(target);
    return copied;
}

static qword_t dispatch_readv(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    return dispatch_readv_at(
            context, syscall, task, fault, false, 0, 0);
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

struct execve_host_buffers {
    char *arguments;
    char *environment;
};

static atomic_size_t execve_live_host_buffer_sets = ATOMIC_VAR_INIT(0);

size_t ish_aarch64_execve_test_live_host_buffer_sets(void) {
    return atomic_load_explicit(
            &execve_live_host_buffer_sets, memory_order_acquire);
}

static void release_execve_host_buffers(void *opaque) {
    struct execve_host_buffers *buffers = opaque;
    free(buffers->environment);
    free(buffers->arguments);
    size_t previous = atomic_fetch_sub_explicit(
            &execve_live_host_buffer_sets, 1, memory_order_acq_rel);
    assert(previous != 0);
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
    struct execve_host_buffers host_buffers = {
        .arguments = arguments,
        .environment = environment,
    };
    atomic_fetch_add_explicit(
            &execve_live_host_buffer_sets, 1, memory_order_release);
    size_t argument_count;
    size_t argument_budget = ISH_AARCH64_EXEC_ARG_MAX;
    int error;
    pthread_cleanup_push(release_execve_host_buffers, &host_buffers);
    {
        error = copy_string_array_from_user(context,
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
    }
    pthread_cleanup_pop(1);
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

static qword_t dispatch_unlinkat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    dword_t raw_flags = (dword_t) syscall->arguments[2];
    if (raw_flags & ~(dword_t) AT_REMOVEDIR_)
        return syscall_result(_EINVAL);
    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[1], path, fault);
    if ((sqword_t) copied < 0)
        return copied;
    return syscall_result(file_unlinkat_task(task,
            syscall_fd(syscall->arguments[0]), path,
            raw_flags == AT_REMOVEDIR_));
}

static qword_t dispatch_mkdirat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[1], path, fault);
    if ((sqword_t) copied < 0)
        return copied;
    return syscall_result(file_mkdirat_task(task,
            syscall_fd(syscall->arguments[0]), path,
            (mode_t_) (dword_t) syscall->arguments[2]));
}

static qword_t dispatch_renameat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault,
        bool has_flags) {
    if (has_flags && (dword_t) syscall->arguments[4] != 0)
        return syscall_result(_EINVAL);
    char source[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[1], source, fault);
    if ((sqword_t) copied < 0)
        return copied;
    char destination[MAX_PATH];
    copied = copy_path_from_user(
            context, syscall->arguments[3], destination, fault);
    if ((sqword_t) copied < 0)
        return copied;
    return syscall_result(file_renameat_task(task,
            syscall_fd(syscall->arguments[0]), source,
            syscall_fd(syscall->arguments[2]), destination));
}

static qword_t dispatch_truncate(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    off_t_ size = (off_t_) (sqword_t) syscall->arguments[1];
    if (size < 0)
        return syscall_result(_EINVAL);
    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[0], path, fault);
    if ((sqword_t) copied < 0)
        return copied;
    return syscall_result(file_truncate_task(task, path, size));
}

static qword_t dispatch_connect(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    fd_t socket_fd = syscall_fd(syscall->arguments[0]);
    struct fd *fd = f_get_task_retain(task, socket_fd);
    if (fd == NULL)
        return syscall_result(_EBADF);
    qword_t result;
    dword_t length = (dword_t) syscall->arguments[2];
    if (length > sizeof(struct sockaddr_storage)) {
        result = syscall_result(_EINVAL);
        goto out;
    }
    qword_t address = syscall->arguments[1];
    if (length != 0 && !aarch64_user_range_fits(address, length)) {
        result = user_range_error(
                fault, address, GUEST_MEMORY_READ);
        goto out;
    }
    struct sockaddr_storage socket_address = {0};
    if (length != 0) {
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address, &socket_address, length, fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }
    }
    bool timeout_enabled = socket_timeout_enabled(fd, false);
    result = syscall_result(socket_connect_retained_task(
            task, fd, &socket_address, length));
    complete_socket_interrupt(
            context, timeout_enabled, (sqword_t) result);
out:
    fd_close(fd);
    return result;
}

static int copy_socket_address_from_user(
        const struct guest_linux_syscall_context *context,
        qword_t address, sdword_t length,
        struct sockaddr_storage *socket_address,
        struct guest_linux_user_fault *fault) {
    if (length < 0 ||
            (size_t) length > sizeof(*socket_address))
        return _EINVAL;
    *socket_address = (struct sockaddr_storage) {0};
    if (length == 0)
        return 0;
    if (!aarch64_user_range_fits(address, (dword_t) length)) {
        (void) user_range_error(fault, address, GUEST_MEMORY_READ);
        return _EFAULT;
    }
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, socket_address, (dword_t) length, fault))
        return _EFAULT;
    return 0;
}

static qword_t dispatch_bind(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct socket_ref socket;
    int error = socket_ref_get_task(
            task, syscall_fd(syscall->arguments[0]), &socket);
    if (error < 0)
        return syscall_result(error);

    struct sockaddr_storage address;
    sdword_t length = (sdword_t) (dword_t) syscall->arguments[2];
    error = copy_socket_address_from_user(context,
            syscall->arguments[1], length, &address, fault);
    qword_t result = error < 0 ? syscall_result(error) :
            syscall_result(socket_bind_ref_task(
                    task, &socket, &address, (size_t) length));
    socket_ref_release(&socket);
    return result;
}

static qword_t dispatch_sendto(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    qword_t requested = syscall->arguments[2];
    size_t length = requested < AARCH64_LINUX_MAX_RW_COUNT ?
            (size_t) requested : (size_t) AARCH64_LINUX_MAX_RW_COUNT;
    qword_t buffer_address = syscall->arguments[1];
    if (!aarch64_user_range_fits(buffer_address, length))
        return user_range_error(
                fault, buffer_address, GUEST_MEMORY_READ);

    struct socket_ref socket;
    int error = socket_ref_get_task(
            task, syscall_fd(syscall->arguments[0]), &socket);
    if (error < 0)
        return syscall_result(error);

    qword_t result;
    struct sockaddr_storage guest_address = {0};
    sdword_t address_length = 0;
    bool has_destination = syscall->arguments[4] != 0;
    struct socket_address destination;
    struct socket_address *destination_pointer = NULL;
    byte_t *buffer = NULL;
    if (has_destination) {
        address_length =
                (sdword_t) (dword_t) syscall->arguments[5];
        error = copy_socket_address_from_user(context,
                syscall->arguments[4], address_length,
                &guest_address, fault);
        if (error < 0) {
            result = syscall_result(error);
            goto out;
        }
    }

    dword_t flags = (dword_t) syscall->arguments[3];
    error = socket_sendto_validate(&socket, length, flags);
    if (error < 0) {
        result = syscall_result(error);
        goto out;
    }
    byte_t prefix[8];
    size_t prefix_size = socket_sendto_prefix_size(&socket);
    if (prefix_size != 0) {
        assert(prefix_size <= sizeof(prefix) && context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                buffer_address, prefix, (dword_t) prefix_size, fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }
        error = socket_sendto_prefix_validate(
                &socket, prefix, prefix_size);
        if (error < 0) {
            result = syscall_result(error);
            goto out;
        }
    }
    bool defer_unix_lookup = has_destination &&
            socket.fd->socket.domain == AF_LOCAL_ &&
            socket.fd->socket.type != SOCK_STREAM_;
    bool defer_unix_destination =
            socket.fd->socket.domain == AF_LOCAL_ &&
            socket.fd->socket.type != SOCK_STREAM_;
    bool stream_socket = socket.fd->socket.type == SOCK_STREAM_;
    if (has_destination && stream_socket &&
            socket.fd->socket.domain == AF_LOCAL_) {
        const struct socket_address supplied_name = {
            .length = (socklen_t) address_length,
        };
        error = socket_sendto_destination_check(
                &socket, &supplied_name, flags);
        assert(error < 0);
        result = syscall_result(error);
        goto out;
    }
    if (has_destination && !stream_socket) {
        error = socket_address_validate_for_socket(&socket,
                &guest_address, (size_t) address_length);
        if (error < 0) {
            result = syscall_result(error);
            goto out;
        }
        if (!defer_unix_lookup) {
            error = socket_address_prepare_task(task,
                    &socket, &guest_address, (size_t) address_length,
                    &destination);
            if (error < 0) {
                result = syscall_result(error);
                goto out;
            }
            destination_pointer = &destination;
        }
    }

    error = socket_sendto_transaction_validate(&socket, length);
    if (error < 0) {
        result = syscall_result(error);
        goto out;
    }
    if (!defer_unix_destination) {
        bool destination_timeout_enabled =
                socket_timeout_enabled(socket.fd, false);
        error = socket_sendto_destination_check(
                &socket, destination_pointer, flags);
        if (error < 0) {
            complete_socket_interrupt(context,
                    destination_timeout_enabled, error);
            result = syscall_result(error);
            goto out;
        }
    }

    size_t transaction_length = socket_sendto_transaction_size(
            &socket, length, flags);
    buffer = transaction_length == 0 ? NULL : malloc(transaction_length);
    if (transaction_length != 0 && buffer == NULL) {
        result = syscall_result(_ENOMEM);
        goto out;
    }
    if (transaction_length != 0) {
        assert(context->user.read != NULL);
        memcpy(buffer, prefix, prefix_size);
        size_t remaining = transaction_length - prefix_size;
        if (remaining != 0 && !context->user.read(context->user.opaque,
                buffer_address + prefix_size, buffer + prefix_size,
                (dword_t) remaining, fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }
    }
    if (defer_unix_lookup) {
        error = socket_address_prepare_task(task,
                &socket, &guest_address, (size_t) address_length,
                &destination);
        if (error < 0) {
            result = syscall_result(error);
            goto out;
        }
        destination_pointer = &destination;
    }
    bool timeout_enabled = socket_timeout_enabled(socket.fd, false);
    ssize_t sent = socket_sendto_ref(&socket,
            buffer, transaction_length, flags, destination_pointer);
    complete_socket_interrupt(context, timeout_enabled, sent);
    result = syscall_result(sent);
out:
    free(buffer);
    if (destination_pointer != NULL)
        socket_address_release(destination_pointer);
    socket_ref_release(&socket);
    return result;
}

static qword_t dispatch_recvfrom(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    qword_t requested = syscall->arguments[2];
    size_t length = requested < AARCH64_LINUX_MAX_RW_COUNT ?
            (size_t) requested : (size_t) AARCH64_LINUX_MAX_RW_COUNT;
    qword_t buffer_address = syscall->arguments[1];
    if (!aarch64_user_range_fits(buffer_address, length))
        return user_range_error(
                fault, buffer_address, GUEST_MEMORY_WRITE);

    struct socket_ref socket;
    int error = socket_ref_get_task(
            task, syscall_fd(syscall->arguments[0]), &socket);
    if (error < 0)
        return syscall_result(error);
    qword_t result;
    dword_t flags = (dword_t) syscall->arguments[3];
    bool full_datagram_length = (flags & MSG_TRUNC_) != 0 &&
            socket.fd->socket.type != SOCK_STREAM_;
    size_t transaction_length = full_datagram_length ?
            SOCKET_IO_TRANSACTION_LIMIT :
            (length < SOCKET_IO_TRANSACTION_LIMIT ?
                    length : SOCKET_IO_TRANSACTION_LIMIT);
    byte_t *buffer = transaction_length == 0 ? NULL :
            malloc(transaction_length);
    if (transaction_length != 0 && buffer == NULL) {
        result = syscall_result(_ENOMEM);
        goto out;
    }

    bool wants_address = syscall->arguments[4] != 0;
    struct socket_address source;
    bool timeout_enabled = socket_timeout_enabled(socket.fd, true);
    ssize_t received = socket_recvfrom_ref(&socket,
            buffer, transaction_length,
            flags,
            wants_address ? &source : NULL);
    if (received < 0) {
        complete_socket_interrupt(
                context, timeout_enabled, received);
        free(buffer);
        result = syscall_result(received);
        goto out;
    }

    size_t payload_size = (size_t) received < length ?
            (size_t) received : length;
    // MSG_TRUNC 的返回值可以大于内部事务缓冲区。
    if (payload_size > transaction_length)
        payload_size = transaction_length;
    if ((socket.fd->socket.domain == AF_INET_ ||
            socket.fd->socket.domain == AF_INET6_) &&
            socket.fd->socket.type == SOCK_STREAM_ &&
            (flags & MSG_TRUNC_))
        payload_size = 0;
    if (payload_size != 0) {
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                buffer_address, buffer,
                (dword_t) payload_size, fault)) {
            free(buffer);
            result = syscall_result(_EFAULT);
            goto out;
        }
    }
    free(buffer);

    if (wants_address) {
        qword_t length_address = syscall->arguments[5];
        if (!aarch64_user_range_fits(
                length_address, sizeof(sdword_t))) {
            result = user_range_error(
                    fault, length_address, GUEST_MEMORY_READ);
            goto out;
        }
        sdword_t capacity;
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                length_address, &capacity,
                sizeof(capacity), fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }
        if (capacity < 0) {
            result = syscall_result(_EINVAL);
            goto out;
        }

        dword_t true_length = (dword_t) source.length;
        if (!aarch64_user_range_fits(
                length_address, sizeof(true_length))) {
            result = user_range_error(
                    fault, length_address, GUEST_MEMORY_WRITE);
            goto out;
        }
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                length_address, &true_length,
                sizeof(true_length), fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }

        dword_t copied = (dword_t) capacity < true_length ?
                (dword_t) capacity : true_length;
        qword_t address = syscall->arguments[4];
        if (copied != 0 && !aarch64_user_range_fits(address, copied)) {
            result = user_range_error(
                    fault, address, GUEST_MEMORY_WRITE);
            goto out;
        }
        if (copied != 0 && !context->user.write(
                context->user.opaque, address,
                &source.storage, copied, fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }
    }
    result = (qword_t) received;
out:
    socket_ref_release(&socket);
    return result;
}

static qword_t dispatch_setsockopt(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct socket_ref socket;
    int error = socket_ref_get_task(
            task, syscall_fd(syscall->arguments[0]), &socket);
    if (error < 0)
        return syscall_result(error);

    sdword_t level = (sdword_t) (dword_t) syscall->arguments[1];
    sdword_t option = (sdword_t) (dword_t) syscall->arguments[2];
    sdword_t value_length =
            (sdword_t) (dword_t) syscall->arguments[4];
    ssize_t copied = socket_setsockopt_value_size(
            level, option, value_length, SOCKET_GUEST_AARCH64);
    qword_t result;
    if (copied < 0) {
        result = syscall_result(copied);
        goto out;
    }
    byte_t value[32] = {0};
    assert((size_t) copied <= sizeof(value));
    qword_t value_address = syscall->arguments[3];
    if (copied != 0 &&
            !aarch64_user_range_fits(value_address, (qword_t) copied)) {
        result = user_range_error(
                fault, value_address, GUEST_MEMORY_READ);
        goto out;
    }
    if (copied != 0) {
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                value_address, value, (dword_t) copied, fault)) {
            result = syscall_result(_EFAULT);
            goto out;
        }
    }
    result = syscall_result(socket_setsockopt_ref(
            &socket, level, option, value, value_length,
            SOCKET_GUEST_AARCH64));
out:
    socket_ref_release(&socket);
    return result;
}

static qword_t dispatch_chdir(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[0], path, fault);
    if ((sqword_t) copied < 0)
        return copied;
    return syscall_result(file_chdir_task(task, path));
}

static qword_t dispatch_readlinkat(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    sdword_t requested = (sdword_t) (dword_t) syscall->arguments[3];
    if (requested <= 0)
        return syscall_result(_EINVAL);

    char path[MAX_PATH];
    qword_t copied = copy_path_from_user(
            context, syscall->arguments[1], path, fault);
    if ((sqword_t) copied < 0)
        return copied;

    char buffer[MAX_PATH];
    size_t capacity = (dword_t) requested < sizeof(buffer) ?
            (dword_t) requested : sizeof(buffer);
    ssize_t length = file_readlinkat_task(task,
            syscall_fd(syscall->arguments[0]), path, buffer, capacity);
    if (length < 0)
        return syscall_result(length);
    assert((size_t) length <= capacity);
    if (!aarch64_user_range_fits(syscall->arguments[2], (qword_t) length))
        return user_range_error(
                fault, syscall->arguments[2], GUEST_MEMORY_WRITE);
    if (length == 0)
        return 0;
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            syscall->arguments[2], buffer, (dword_t) length, fault))
        return syscall_result(_EFAULT);
    return (qword_t) length;
}

enum aarch64_linux_ioctl_direction {
    AARCH64_LINUX_IOCTL_INPUT = 1,
    AARCH64_LINUX_IOCTL_OUTPUT = 2,
};

static dword_t ioctl_direction(dword_t command, size_t size) {
    switch (command) {
        case TCGETS_:
        case TIOCGPGRP_:
        case TIOCGWINSZ_:
        case FIONREAD_:
            return AARCH64_LINUX_IOCTL_OUTPUT;
        case TCSETS_:
        case TCSETSW_:
        case TCSETSF_:
        case TIOCSPGRP_:
        case TIOCSWINSZ_:
        case TIOCPKT_:
        case FIONBIO_:
            return AARCH64_LINUX_IOCTL_INPUT;
    }
    dword_t encoded = command >> 30;
    if (encoded != 0)
        return encoded;
    return size == 0 ? 0 :
            AARCH64_LINUX_IOCTL_INPUT | AARCH64_LINUX_IOCTL_OUTPUT;
}

static qword_t dispatch_ioctl(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    fd_t fd_number = syscall_fd(syscall->arguments[0]);
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return syscall_result(_EBADF);

    dword_t command = (dword_t) syscall->arguments[1];
    ssize_t buffer_size = file_ioctl_size_fd(fd, command);
    if (buffer_size < 0) {
        fd_close(fd);
        return syscall_result(buffer_size);
    }
    assert((qword_t) buffer_size <= UINT32_MAX);

    dword_t size = (dword_t) buffer_size;
    dword_t direction = ioctl_direction(command, size);
    byte_t *buffer = size == 0 ? NULL : malloc(size);
    if (size != 0 && buffer == NULL) {
        fd_close(fd);
        return syscall_result(_ENOMEM);
    }
    if (size != 0 && direction & AARCH64_LINUX_IOCTL_INPUT) {
        if (!aarch64_user_range_fits(syscall->arguments[2], size)) {
            free(buffer);
            fd_close(fd);
            return user_range_error(
                    fault, syscall->arguments[2], GUEST_MEMORY_READ);
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[2], buffer, size, fault)) {
            free(buffer);
            fd_close(fd);
            return syscall_result(_EFAULT);
        }
    }

    int result = file_ioctl_fd_task(task, fd_number, fd,
            command, buffer, (dword_t) syscall->arguments[2]);
    if (result >= 0 && size != 0 &&
            direction & AARCH64_LINUX_IOCTL_OUTPUT) {
        if (!aarch64_user_range_fits(syscall->arguments[2], size)) {
            free(buffer);
            fd_close(fd);
            return user_range_error(
                    fault, syscall->arguments[2], GUEST_MEMORY_WRITE);
        }
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                syscall->arguments[2], buffer, size, fault))
            result = _EFAULT;
    }
    free(buffer);
    fd_close(fd);
    return syscall_result(result);
}

static qword_t dispatch_pselect6(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct aarch64_linux_pselect_sigmask mask_argument = {0};
    if (syscall->arguments[5] != 0) {
        if (!aarch64_user_range_fits(
                syscall->arguments[5], sizeof(mask_argument)))
            return user_range_error(
                    fault, syscall->arguments[5], GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[5], &mask_argument,
                sizeof(mask_argument), fault))
            return syscall_result(_EFAULT);
    }

    struct timer_time deadline;
    bool has_timeout = syscall->arguments[4] != 0;
    bool zero_timeout = false;
    if (has_timeout) {
        struct aarch64_linux_timespec timeout;
        if (!aarch64_user_range_fits(
                syscall->arguments[4], sizeof(timeout)))
            return user_range_error(
                    fault, syscall->arguments[4], GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[4], &timeout,
                sizeof(timeout), fault))
            return syscall_result(_EFAULT);
        if (timeout.sec < 0 || timeout.nsec < 0 ||
                timeout.nsec >= INT64_C(1000000000))
            return syscall_result(_EINVAL);
        zero_timeout = timeout.sec == 0 && timeout.nsec == 0;
        deadline = timer_time_add(
                timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)),
                (struct timer_time) {timeout.sec, timeout.nsec});
    }

    sigset_t_ mask = 0;
    bool has_mask = mask_argument.address != 0;
    if (has_mask) {
        if (mask_argument.size != sizeof(mask))
            return syscall_result(_EINVAL);
        if (!aarch64_user_range_fits(
                mask_argument.address, sizeof(mask)))
            return user_range_error(
                    fault, mask_argument.address, GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                mask_argument.address, &mask, sizeof(mask), fault))
            return syscall_result(_EFAULT);
    }

    bool mask_applied = false;
    if (has_mask) {
        sigmask_set_temp_task(task, mask);
        mask_applied = true;
    }
    fd_t nfds = (fd_t) (sdword_t) (dword_t) syscall->arguments[0];
    sqword_t result = 0;
    size_t fdset_size = 0;
    byte_t *readfds = NULL;
    byte_t *writefds = NULL;
    byte_t *exceptfds = NULL;
    if (nfds < 0 || (rlim_t_) nfds >
            rlimit_task(task, RLIMIT_NOFILE_)) {
        result = _EINVAL;
        goto finish;
    }
    fdset_size = (((size_t) nfds + 63) / 64) * sizeof(qword_t);

    if (fdset_size != 0 && syscall->arguments[1] != 0) {
        readfds = malloc(fdset_size);
        if (readfds == NULL) {
            result = _ENOMEM;
            goto finish;
        }
    }
    if (fdset_size != 0 && syscall->arguments[2] != 0) {
        writefds = malloc(fdset_size);
        if (writefds == NULL) {
            result = _ENOMEM;
            goto finish;
        }
    }
    if (fdset_size != 0 && syscall->arguments[3] != 0) {
        exceptfds = malloc(fdset_size);
        if (exceptfds == NULL) {
            result = _ENOMEM;
            goto finish;
        }
    }

    if (readfds != NULL) {
        if (!aarch64_user_range_fits(
                syscall->arguments[1], fdset_size)) {
            (void) user_range_error(
                    fault, syscall->arguments[1], GUEST_MEMORY_READ);
            result = _EFAULT;
            goto finish;
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[1], readfds,
                (dword_t) fdset_size, fault)) {
            result = _EFAULT;
            goto finish;
        }
    }
    if (writefds != NULL) {
        if (!aarch64_user_range_fits(
                syscall->arguments[2], fdset_size)) {
            (void) user_range_error(
                    fault, syscall->arguments[2], GUEST_MEMORY_READ);
            result = _EFAULT;
            goto finish;
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[2], writefds,
                (dword_t) fdset_size, fault)) {
            result = _EFAULT;
            goto finish;
        }
    }
    if (exceptfds != NULL) {
        if (!aarch64_user_range_fits(
                syscall->arguments[3], fdset_size)) {
            (void) user_range_error(
                    fault, syscall->arguments[3], GUEST_MEMORY_READ);
            result = _EFAULT;
            goto finish;
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[3], exceptfds,
                (dword_t) fdset_size, fault)) {
            result = _EFAULT;
            goto finish;
        }
    }

    result = file_select_task(task, nfds,
            readfds, writefds, exceptfds, fdset_size,
            has_timeout ? &deadline : NULL);
    if (result >= 0 && readfds != NULL) {
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                syscall->arguments[1], readfds,
                (dword_t) fdset_size, fault))
            result = _EFAULT;
    }
    if (result >= 0 && writefds != NULL) {
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                syscall->arguments[2], writefds,
                (dword_t) fdset_size, fault))
            result = _EFAULT;
    }
    if (result >= 0 && exceptfds != NULL) {
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                syscall->arguments[3], exceptfds,
                (dword_t) fdset_size, fault))
            result = _EFAULT;
    }

finish:
    free(readfds);
    free(writefds);
    free(exceptfds);

    if (mask_applied && result != _EINTR) {
        sigmask_restore_temp_task(task);
        mask_applied = false;
    }
    if (has_timeout && !zero_timeout) {
        struct timer_time remaining = timer_time_subtract(
                deadline,
                timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)));
        if (!timer_time_positive(remaining))
            remaining = (struct timer_time) {0};
        const struct aarch64_linux_timespec timeout = {
            .sec = remaining.sec,
            .nsec = remaining.nsec,
        };
        assert(context->user.write != NULL);
        // Linux raw syscall 的剩余超时写回不覆盖主要返回值。
        struct guest_linux_user_fault timeout_fault;
        (void) context->user.write(context->user.opaque,
                syscall->arguments[4], &timeout,
                sizeof(timeout), &timeout_fault);
    }
    return syscall_result(result);
}

static qword_t dispatch_ppoll(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    qword_t nfds = syscall->arguments[1];
    if (nfds > rlimit_task(task, RLIMIT_NOFILE_) ||
            nfds > UINT32_MAX / sizeof(struct pollfd_))
        return syscall_result(_EINVAL);

    struct timer_time deadline;
    const struct timer_time *deadline_pointer = NULL;
    if (syscall->arguments[2] != 0) {
        struct aarch64_linux_timespec wire;
        if (!aarch64_user_range_fits(
                syscall->arguments[2], sizeof(wire)))
            return user_range_error(
                    fault, syscall->arguments[2], GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[2], &wire, sizeof(wire), fault))
            return syscall_result(_EFAULT);
        if (wire.sec < 0 || wire.nsec < 0 ||
                wire.nsec >= INT64_C(1000000000))
            return syscall_result(_EINVAL);
        deadline = timer_time_add(
                timer_time_from_timespec(
                        timespec_now(CLOCK_MONOTONIC)),
                (struct timer_time) {wire.sec, wire.nsec});
        deadline_pointer = &deadline;
    }

    sigset_t_ mask = 0;
    if (syscall->arguments[3] != 0) {
        if (syscall->arguments[4] != sizeof(mask))
            return syscall_result(_EINVAL);
        if (!aarch64_user_range_fits(
                syscall->arguments[3], sizeof(mask)))
            return user_range_error(
                    fault, syscall->arguments[3], GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[3], &mask, sizeof(mask), fault))
            return syscall_result(_EFAULT);
    }

    size_t byte_count = (size_t) nfds * sizeof(struct pollfd_);
    struct pollfd_ *polls = byte_count == 0 ? NULL : malloc(byte_count);
    if (byte_count != 0 && polls == NULL)
        return syscall_result(_ENOMEM);
    if (byte_count != 0) {
        if (!aarch64_user_range_fits(syscall->arguments[0], byte_count)) {
            free(polls);
            return user_range_error(
                    fault, syscall->arguments[0], GUEST_MEMORY_READ);
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[0], polls, (dword_t) byte_count, fault)) {
            free(polls);
            return syscall_result(_EFAULT);
        }
    }

    bool has_mask = syscall->arguments[3] != 0;
    if (has_mask)
        sigmask_set_temp_task(task, mask);
    sqword_t result = file_poll_until_task(
            task, polls, (size_t) nfds, deadline_pointer);
    if (result >= 0 && byte_count != 0) {
        if (!aarch64_user_range_fits(syscall->arguments[0], byte_count)) {
            free(polls);
            return user_range_error(
                    fault, syscall->arguments[0], GUEST_MEMORY_WRITE);
        }
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                syscall->arguments[0], polls,
                (dword_t) byte_count, fault))
            result = _EFAULT;
    }
    free(polls);
    if (has_mask && result != _EINTR)
        sigmask_restore_temp_task(task);
    return syscall_result(result);
}

static qword_t dispatch_epoll_ctl(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    int_t operation = (int_t) (sdword_t) (dword_t) syscall->arguments[1];
    struct epoll_event_value event;
    const struct epoll_event_value *event_pointer = NULL;
    if (operation == EPOLL_CTL_ADD_ || operation == EPOLL_CTL_MOD_) {
        qword_t address = syscall->arguments[3];
        struct aarch64_linux_epoll_event wire;
        if (!aarch64_user_range_fits(address, sizeof(wire)))
            return user_range_error(fault, address, GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address, &wire, sizeof(wire), fault))
            return syscall_result(_EFAULT);
        event = (struct epoll_event_value) {
            .events = wire.events,
            .data = wire.data,
        };
        event_pointer = &event;
    }
    return syscall_result(epoll_ctl_task(task,
            syscall_fd(syscall->arguments[0]), operation,
            syscall_fd(syscall->arguments[2]), event_pointer));
}

static qword_t dispatch_epoll_pwait(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    int_t max_events = (int_t) (sdword_t) (dword_t) syscall->arguments[2];
    if (max_events <= 0 || max_events >
            INT_MAX / (int) sizeof(struct aarch64_linux_epoll_event))
        return syscall_result(_EINVAL);

    qword_t output_size = (qword_t) (dword_t) max_events *
            sizeof(struct aarch64_linux_epoll_event);
    if (!aarch64_user_range_fits(syscall->arguments[1], output_size))
        return user_range_error(
                fault, syscall->arguments[1], GUEST_MEMORY_WRITE);

    bool has_mask = syscall->arguments[4] != 0;
    sigset_t_ mask = 0;
    if (has_mask) {
        if (syscall->arguments[5] != sizeof(sigset_t_))
            return syscall_result(_EINVAL);
        if (!aarch64_user_range_fits(
                syscall->arguments[4], sizeof(mask)))
            return user_range_error(
                    fault, syscall->arguments[4], GUEST_MEMORY_READ);
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                syscall->arguments[4], &mask, sizeof(mask), fault))
            return syscall_result(_EFAULT);
    }

    if (has_mask)
        sigmask_set_temp_task(task, mask);
    struct epoll_event_value *events;
    int_t result = epoll_wait_task(task,
            syscall_fd(syscall->arguments[0]), max_events,
            (int_t) (sdword_t) (dword_t) syscall->arguments[3], &events);
    if (result <= 0) {
        if (has_mask && result != _EINTR)
            sigmask_restore_temp_task(task);
        return syscall_result(result);
    }

    for (int index = 0; index < result; index++) {
        struct aarch64_linux_epoll_event wire = {
            .events = events[index].events,
            .padding = 0,
            .data = events[index].data,
        };
        memcpy((byte_t *) events +
                (size_t) index * sizeof(wire), &wire, sizeof(wire));
    }
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            syscall->arguments[1], events,
            (dword_t) ((size_t) result *
                    sizeof(struct aarch64_linux_epoll_event)), fault))
        result = _EFAULT;
    free(events);
    if (has_mask)
        sigmask_restore_temp_task(task);
    return syscall_result(result);
}

static qword_t dispatch_read(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    // guest 写回可能并发关闭并复用槽位；整次 read 固定同一文件对象。
    struct fd *target = f_get_task_retain(
            task, syscall_fd(syscall->arguments[0]));
    if (target == NULL)
        return syscall_result(_EBADF);
    int error = file_read_check_fd(target);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }
    qword_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2] < AARCH64_LINUX_MAX_RW_COUNT ?
            syscall->arguments[2] : AARCH64_LINUX_MAX_RW_COUNT;
    byte_t buffer[AARCH64_LINUX_IO_CHUNK_SIZE];
    qword_t result;
    bool timeout_enabled = false;
    if (remaining == 0) {
        timeout_enabled = socket_timeout_enabled(target, true);
        result = syscall_result(file_read_fd(target, buffer, 0));
        goto out;
    }

    qword_t completed = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        timeout_enabled = socket_timeout_enabled(target, true);
        ssize_t read = file_read_fd(target, buffer, chunk);
        if (read < 0) {
            result = completed != 0 ? completed : syscall_result(read);
            goto out;
        }
        assert((size_t) read <= chunk);
        if (read == 0) {
            result = completed;
            goto out;
        }
        dword_t copy_size = (dword_t) read;
        if (!user_range_fits(address, copy_size)) {
            result = completed != 0 ? completed :
                    user_range_error(fault, address, GUEST_MEMORY_WRITE);
            goto out;
        }
        assert(context->user.write != NULL);
        // 跨页写回失败前可能已复制前缀；文件位置沿用现有 host-buffer 语义，不尝试回滚。
        if (!context->user.write(context->user.opaque,
                address, buffer, copy_size, fault)) {
            result = completed != 0 ?
                    completed : syscall_result(_EFAULT);
            goto out;
        }
        completed += (qword_t) read;
        if ((size_t) read != chunk) {
            result = completed;
            goto out;
        }
        remaining -= (qword_t) read;
        if (remaining == 0) {
            result = completed;
            goto out;
        }
        if (address > UINT64_MAX - (qword_t) read) {
            user_range_error(fault, address, GUEST_MEMORY_WRITE);
            result = completed;
            goto out;
        }
        address += (qword_t) read;
    }
    result = completed;
out:
    complete_socket_interrupt(
            context, timeout_enabled, (sqword_t) result);
    fd_close(target);
    return result;
}

static qword_t dispatch_write(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    // guest 读取可能并发关闭并复用槽位；整次 write 固定同一文件对象。
    struct fd *target = f_get_task_retain(
            task, syscall_fd(syscall->arguments[0]));
    if (target == NULL)
        return syscall_result(_EBADF);
    int error = file_write_check_fd(target);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }
    qword_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2] < AARCH64_LINUX_MAX_RW_COUNT ?
            syscall->arguments[2] : AARCH64_LINUX_MAX_RW_COUNT;
    byte_t buffer[AARCH64_LINUX_IO_CHUNK_SIZE];
    qword_t result;
    bool timeout_enabled = false;
    if (remaining == 0) {
        timeout_enabled = socket_timeout_enabled(target, false);
        result = syscall_result(file_write_fd(target, buffer, 0));
        goto out;
    }

    qword_t completed = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        dword_t copy_size = (dword_t) chunk;
        if (!user_range_fits(address, copy_size)) {
            result = completed != 0 ? completed :
                    user_range_error(fault, address, GUEST_MEMORY_READ);
            goto out;
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address, buffer, copy_size, fault)) {
            result = completed != 0 ?
                    completed : syscall_result(_EFAULT);
            goto out;
        }
        timeout_enabled = socket_timeout_enabled(target, false);
        ssize_t written = file_write_fd(target, buffer, chunk);
        if (written < 0) {
            result = completed != 0 ?
                    completed : syscall_result(written);
            goto out;
        }
        assert((size_t) written <= chunk);
        completed += (qword_t) written;
        if ((size_t) written != chunk) {
            result = completed;
            goto out;
        }
        remaining -= (qword_t) written;
        if (remaining == 0) {
            result = completed;
            goto out;
        }
        if (address > UINT64_MAX - (qword_t) written) {
            user_range_error(fault, address, GUEST_MEMORY_READ);
            result = completed;
            goto out;
        }
        address += (qword_t) written;
    }
    result = completed;
out:
    complete_socket_interrupt(
            context, timeout_enabled, (sqword_t) result);
    fd_close(target);
    return result;
}

static qword_t dispatch_pread64(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    off_t_ offset = (off_t_) syscall->arguments[3];
    if (offset < 0)
        return syscall_result(_EINVAL);

    struct fd *target = f_get_task_retain(
            task, syscall_fd(syscall->arguments[0]));
    if (target == NULL)
        return syscall_result(_EBADF);
    int error = file_read_check_fd(target);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }

    qword_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2] < AARCH64_LINUX_MAX_RW_COUNT ?
            syscall->arguments[2] : AARCH64_LINUX_MAX_RW_COUNT;
    if (remaining > (qword_t) INT64_MAX - (qword_t) offset) {
        fd_close(target);
        return syscall_result(_EINVAL);
    }

    byte_t buffer[AARCH64_LINUX_IO_CHUNK_SIZE];
    if (remaining == 0) {
        ssize_t result = file_pread_fd(target, buffer, 0, offset);
        fd_close(target);
        return syscall_result(result);
    }

    qword_t completed = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        ssize_t read = file_pread_fd(target, buffer, chunk,
                offset + (off_t_) completed);
        if (read < 0) {
            qword_t result = completed != 0 ?
                    completed : syscall_result(read);
            fd_close(target);
            return result;
        }
        assert((size_t) read <= chunk);
        if (read == 0) {
            fd_close(target);
            return completed;
        }
        dword_t copy_size = (dword_t) read;
        if (!user_range_fits(address, copy_size)) {
            qword_t result = completed != 0 ? completed :
                    user_range_error(
                            fault, address, GUEST_MEMORY_WRITE);
            fd_close(target);
            return result;
        }
        assert(context->user.write != NULL);
        if (!context->user.write(context->user.opaque,
                address, buffer, copy_size, fault)) {
            qword_t result = completed != 0 ?
                    completed : syscall_result(_EFAULT);
            fd_close(target);
            return result;
        }
        completed += (qword_t) read;
        if ((size_t) read != chunk) {
            fd_close(target);
            return completed;
        }
        remaining -= (qword_t) read;
        if (remaining == 0) {
            fd_close(target);
            return completed;
        }
        if (address > UINT64_MAX - (qword_t) read) {
            user_range_error(fault, address, GUEST_MEMORY_WRITE);
            fd_close(target);
            return completed;
        }
        address += (qword_t) read;
    }
    fd_close(target);
    return completed;
}

static qword_t dispatch_pwrite64(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    off_t_ offset = (off_t_) syscall->arguments[3];
    if (offset < 0)
        return syscall_result(_EINVAL);

    struct fd *target = f_get_task_retain(
            task, syscall_fd(syscall->arguments[0]));
    if (target == NULL)
        return syscall_result(_EBADF);
    int error = file_write_check_fd(target);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }

    qword_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2] < AARCH64_LINUX_MAX_RW_COUNT ?
            syscall->arguments[2] : AARCH64_LINUX_MAX_RW_COUNT;
    if (remaining > (qword_t) INT64_MAX - (qword_t) offset) {
        fd_close(target);
        return syscall_result(_EINVAL);
    }

    byte_t buffer[AARCH64_LINUX_IO_CHUNK_SIZE];
    if (remaining == 0) {
        ssize_t result = file_pwrite_fd(target, buffer, 0, offset);
        fd_close(target);
        return syscall_result(result);
    }

    qword_t completed = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buffer) ?
                (size_t) remaining : sizeof(buffer);
        dword_t copy_size = (dword_t) chunk;
        if (!user_range_fits(address, copy_size)) {
            qword_t result = completed != 0 ? completed :
                    user_range_error(
                            fault, address, GUEST_MEMORY_READ);
            fd_close(target);
            return result;
        }
        assert(context->user.read != NULL);
        if (!context->user.read(context->user.opaque,
                address, buffer, copy_size, fault)) {
            qword_t result = completed != 0 ?
                    completed : syscall_result(_EFAULT);
            fd_close(target);
            return result;
        }
        ssize_t written = file_pwrite_fd(target, buffer, chunk,
                offset + (off_t_) completed);
        if (written < 0) {
            qword_t result = completed != 0 ?
                    completed : syscall_result(written);
            fd_close(target);
            return result;
        }
        assert((size_t) written <= chunk);
        completed += (qword_t) written;
        if ((size_t) written != chunk) {
            fd_close(target);
            return completed;
        }
        remaining -= (qword_t) written;
        if (remaining == 0) {
            fd_close(target);
            return completed;
        }
        if (address > UINT64_MAX - (qword_t) written) {
            user_range_error(fault, address, GUEST_MEMORY_READ);
            fd_close(target);
            return completed;
        }
        address += (qword_t) written;
    }
    fd_close(target);
    return completed;
}

static qword_t dispatch_writev_at(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault,
        bool positioned, off_t_ offset, dword_t flags) {
    if (positioned && offset < 0)
        return syscall_result(_EINVAL);
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
            GUEST_MEMORY_READ, &vectors, &total, fault);
    if (error < 0) {
        fd_close(target);
        return syscall_result(error);
    }
    if (total == 0) {
        free(vectors);
        fd_close(target);
        return 0;
    }
    if (flags != 0) {
        free(vectors);
        fd_close(target);
        return syscall_result(_EOPNOTSUPP);
    }
    if (total > AARCH64_LINUX_IOV_TRANSACTION_LIMIT) {
        free(vectors);
        fd_close(target);
        return syscall_result(_ENOMEM);
    }
    if (positioned && total > (qword_t) INT64_MAX - (qword_t) offset) {
        free(vectors);
        fd_close(target);
        return syscall_result(_EINVAL);
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
    bool timeout_enabled = !positioned &&
            socket_timeout_enabled(target, false);
    ssize_t written = positioned ?
            file_pwrite_fd(target, buffer, (size_t) total, offset) :
            file_write_fd(target, buffer, (size_t) total);
    assert(written < 0 || (qword_t) written <= total);
    if (!positioned)
        complete_socket_interrupt(
                context, timeout_enabled, written);
    free(buffer);
    free(vectors);
    fd_close(target);
    return syscall_result(written);
}

static qword_t dispatch_writev(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    return dispatch_writev_at(
            context, syscall, task, fault, false, 0, 0);
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
        const struct dir_entry *entry, off_t_ next_position) {
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
    wire->next_offset = next_position;
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

static struct siginfo_ import_aarch64_sigqueueinfo(
        const struct guest_linux_signal_info *info) {
    assert(info->payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_QUEUE);
    return (struct siginfo_) {
        .sig = info->signal,
        .sig_errno = info->error,
        .code = info->code,
        .payload_kind = SIGNAL_INFO_PAYLOAD_QUEUE,
        .queue = {
            .pid = info->queue.pid,
            .uid = info->queue.uid,
            .value = info->queue.value,
        },
    };
}

static qword_t dispatch_rt_sigqueueinfo(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault, bool thread_directed) {
    qword_t address = syscall->arguments[thread_directed ? 3 : 2];
    if (!aarch64_user_range_fits(
            address, sizeof(struct aarch64_linux_siginfo)))
        return user_range_error(fault, address, GUEST_MEMORY_READ);

    struct aarch64_linux_siginfo wire;
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, &wire, sizeof(wire), fault))
        return syscall_result(_EFAULT);

    int signal = (sdword_t) (dword_t)
            syscall->arguments[thread_directed ? 2 : 1];
    struct guest_linux_signal_info imported =
            aarch64_linux_unpack_sigqueueinfo(signal, &wire);
    struct siginfo_ info = import_aarch64_sigqueueinfo(&imported);
    int error = thread_directed ?
            task_rt_tgsigqueueinfo(
                    syscall_pid(syscall->arguments[0]),
                    syscall_pid(syscall->arguments[1]),
                    signal, &info) :
            task_rt_sigqueueinfo(
                    syscall_pid(syscall->arguments[0]),
                    signal, &info);
    return syscall_result(error);
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
    if (context->completion != NULL)
        context->completion->restart =
                GUEST_LINUX_SYSCALL_RESTART_DEFAULT;

    switch (syscall->number) {
        case AARCH64_LINUX_SYS_GETCWD:
            return dispatch_getcwd(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_EPOLL_CREATE1:
            return syscall_result(epoll_create_task(task,
                    (int_t) (sdword_t) (dword_t) syscall->arguments[0]));
        case AARCH64_LINUX_SYS_EPOLL_CTL:
            return dispatch_epoll_ctl(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_EPOLL_PWAIT:
            return dispatch_epoll_pwait(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_DUP:
            return aarch64_linux_dispatch_dup(syscall, task);
        case AARCH64_LINUX_SYS_DUP3:
            return aarch64_linux_dispatch_dup3(syscall, task);
        case AARCH64_LINUX_SYS_FCNTL:
            return aarch64_linux_dispatch_fcntl(syscall, task);
        case AARCH64_LINUX_SYS_IOCTL:
            return dispatch_ioctl(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_MKDIRAT:
            return dispatch_mkdirat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_UNLINKAT:
            return dispatch_unlinkat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RENAMEAT:
            return dispatch_renameat(
                    context, syscall, task, fault, false);
        case AARCH64_LINUX_SYS_TRUNCATE:
            return dispatch_truncate(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_FTRUNCATE:
            return syscall_result(file_ftruncate_task(task,
                    syscall_fd(syscall->arguments[0]),
                    (off_t_) (sqword_t) syscall->arguments[1]));
        case AARCH64_LINUX_SYS_CHDIR:
            return dispatch_chdir(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_FCHDIR:
            return syscall_result(file_fchdir_task(
                    task, syscall_fd(syscall->arguments[0])));
        case AARCH64_LINUX_SYS_OPENAT:
            return dispatch_openat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_CLOSE:
            return syscall_result(f_close_task(
                    task, syscall_fd(syscall->arguments[0])));
        case AARCH64_LINUX_SYS_PIPE2:
            return aarch64_linux_dispatch_pipe2(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_GETDENTS64:
            return dispatch_getdents64(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_LSEEK:
            return syscall_result(file_lseek_task(task,
                    syscall_fd(syscall->arguments[0]),
                    (sqword_t) syscall->arguments[1],
                    (sdword_t) (dword_t) syscall->arguments[2]));
        case AARCH64_LINUX_SYS_READ:
            return dispatch_read(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_WRITE:
            return dispatch_write(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_READV:
            return dispatch_readv(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_WRITEV:
            return dispatch_writev(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_PREAD64:
            return dispatch_pread64(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_PWRITE64:
            return dispatch_pwrite64(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_PREADV:
            return dispatch_readv_at(context, syscall, task, fault,
                    true, (off_t_) syscall->arguments[3], 0);
        case AARCH64_LINUX_SYS_PWRITEV:
            return dispatch_writev_at(context, syscall, task, fault,
                    true, (off_t_) syscall->arguments[3], 0);
        case AARCH64_LINUX_SYS_PSELECT6:
            return dispatch_pselect6(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_PPOLL:
            return dispatch_ppoll(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_READLINKAT:
            return dispatch_readlinkat(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_NEWFSTATAT:
            return dispatch_newfstatat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_FSTAT:
            return dispatch_fstat(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_FSYNC:
            return syscall_result(file_sync_task(task,
                    syscall_fd(syscall->arguments[0]), false));
        case AARCH64_LINUX_SYS_FDATASYNC:
            return syscall_result(file_sync_task(task,
                    syscall_fd(syscall->arguments[0]), true));
        case AARCH64_LINUX_SYS_FUTEX:
            if (!task_has_aarch64_process(task))
                return syscall_result(_EINVAL);
            return syscall_result((sdword_t) sys_futex_aarch64(
                    syscall->arguments[0],
                    (dword_t) syscall->arguments[1],
                    (dword_t) syscall->arguments[2],
                    syscall->arguments[3], syscall->arguments[4],
                    (dword_t) syscall->arguments[5], fault));
        case AARCH64_LINUX_SYS_FUTEX_WAITV:
            if (!task_has_aarch64_process(task))
                return syscall_result(_EINVAL);
            return syscall_result(sys_futex_waitv_aarch64(
                    syscall->arguments[0],
                    (dword_t) syscall->arguments[1],
                    (dword_t) syscall->arguments[2],
                    syscall->arguments[3],
                    (sdword_t) (dword_t) syscall->arguments[4],
                    &context->user, fault));
        case AARCH64_LINUX_SYS_SET_ROBUST_LIST:
            return syscall_result(sys_set_robust_list_aarch64(
                    syscall->arguments[0], syscall->arguments[1]));
        case AARCH64_LINUX_SYS_GET_ROBUST_LIST: {
            qword_t robust_list;
            int_t error = sys_get_robust_list_aarch64(
                    syscall_pid(syscall->arguments[0]), &robust_list);
            if (error < 0)
                return syscall_result(error);
            const qword_t length =
                    sizeof(struct aarch64_linux_robust_list_head);
            qword_t length_address = syscall->arguments[2];
            if (!aarch64_user_range_fits(
                    length_address, sizeof(length)))
                return user_range_error(
                        fault, length_address, GUEST_MEMORY_WRITE);
            assert(context->user.write != NULL);
            if (!context->user.write(context->user.opaque,
                    length_address, &length, sizeof(length), fault))
                return syscall_result(_EFAULT);

            qword_t head_address = syscall->arguments[1];
            if (!aarch64_user_range_fits(
                    head_address, sizeof(robust_list)))
                return user_range_error(
                        fault, head_address, GUEST_MEMORY_WRITE);
            if (!context->user.write(context->user.opaque,
                    head_address, &robust_list,
                    sizeof(robust_list), fault))
                return syscall_result(_EFAULT);
            return 0;
        }
        case AARCH64_LINUX_SYS_NANOSLEEP:
            return aarch64_linux_dispatch_nanosleep(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_CLOCK_GETTIME:
            return aarch64_linux_dispatch_clock_gettime(
                    context, syscall, fault);
        case AARCH64_LINUX_SYS_KILL:
            return syscall_result((sdword_t) sys_kill(
                    syscall_pid(syscall->arguments[0]),
                    (dword_t) syscall->arguments[1]));
        case AARCH64_LINUX_SYS_TKILL:
            return syscall_result((sdword_t) sys_tkill(
                    syscall_pid(syscall->arguments[0]),
                    (dword_t) syscall->arguments[1]));
        case AARCH64_LINUX_SYS_TGKILL:
            return syscall_result((sdword_t) sys_tgkill(
                    syscall_pid(syscall->arguments[0]),
                    syscall_pid(syscall->arguments[1]),
                    (dword_t) syscall->arguments[2]));
        case AARCH64_LINUX_SYS_SIGALTSTACK:
            return dispatch_sigaltstack(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RT_SIGSUSPEND:
            return aarch64_linux_dispatch_rt_sigsuspend(
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
        case AARCH64_LINUX_SYS_RT_SIGQUEUEINFO:
            return dispatch_rt_sigqueueinfo(
                    context, syscall, fault, false);
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
        case AARCH64_LINUX_SYS_SOCKET:
            return syscall_result(socket_create_task(task,
                    (dword_t) syscall->arguments[0],
                    (dword_t) syscall->arguments[1],
                    (dword_t) syscall->arguments[2]));
        case AARCH64_LINUX_SYS_BIND:
            return dispatch_bind(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_CONNECT:
            return dispatch_connect(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_SENDTO:
            return dispatch_sendto(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RECVFROM:
            return dispatch_recvfrom(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_SETSOCKOPT:
            return dispatch_setsockopt(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_CLONE:
            // 旧 clone ABI 的高 32 位不参与 flags。
            if (!task_has_aarch64_process(task))
                return syscall_result(_EINVAL);
            return syscall_result((sdword_t) sys_clone_aarch64(
                    (dword_t) syscall->arguments[0],
                    syscall->arguments[1], syscall->arguments[2],
                    syscall->arguments[3], syscall->arguments[4], fault));
        case AARCH64_LINUX_SYS_CLONE3:
            return dispatch_clone3(
                    context, syscall, task, fault);
        case AARCH64_LINUX_SYS_EXECVE:
            return dispatch_execve(context, syscall, task, fault);
        case AARCH64_LINUX_SYS_RT_TGSIGQUEUEINFO:
            return dispatch_rt_sigqueueinfo(
                    context, syscall, fault, true);
        case AARCH64_LINUX_SYS_WAIT4:
            return aarch64_linux_dispatch_wait4(
                    context, syscall, fault);
        case AARCH64_LINUX_SYS_RENAMEAT2:
            return dispatch_renameat(
                    context, syscall, task, fault, true);
        case AARCH64_LINUX_SYS_PREADV2: {
            dword_t flags = (dword_t) syscall->arguments[5];
            off_t_ offset = (off_t_) syscall->arguments[3];
            return dispatch_readv_at(context, syscall, task, fault,
                    offset != -1, offset == -1 ? 0 : offset, flags);
        }
        case AARCH64_LINUX_SYS_PWRITEV2: {
            dword_t flags = (dword_t) syscall->arguments[5];
            off_t_ offset = (off_t_) syscall->arguments[3];
            return dispatch_writev_at(context, syscall, task, fault,
                    offset != -1, offset == -1 ? 0 : offset, flags);
        }
        default:
            return syscall_result(_ENOSYS);
    }
}

const struct guest_linux_syscall_service ish_aarch64_linux_syscall_service = {
    .dispatch = dispatch_syscall,
};

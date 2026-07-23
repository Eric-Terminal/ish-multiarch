#include <assert.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/pipe.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-fd-service.h"
#include "kernel/errno.h"
#include "kernel/fs.h"

#define AARCH64_FD_USER_ADDRESS_LIMIT \
    (AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1))
#define AARCH64_LINUX_O_DIRECTORY UINT32_C(0x004000)
#define AARCH64_LINUX_O_NOFOLLOW UINT32_C(0x008000)
#define AARCH64_LINUX_O_LARGEFILE UINT32_C(0x020000)

struct aarch64_linux_pipe_fds {
    sdword_t read_fd;
    sdword_t write_fd;
};

_Static_assert(sizeof(struct aarch64_linux_pipe_fds) == 8,
        "AArch64 pipe2 必须写回两个连续的 32 位文件描述符");

static qword_t fd_result(sqword_t result) {
    return (qword_t) result;
}

static fd_t fd_argument(qword_t argument) {
    return (fd_t) (sdword_t) (dword_t) argument;
}

static int aarch64_fcntl_getfl(struct task *task, fd_t fd) {
    int flags = f_getfl_task(task, fd);
    if (flags < 0)
        return flags;
    int guest_flags = flags & (O_ACCMODE_ | O_APPEND_ | O_NONBLOCK_);
    if (flags & O_LARGEFILE_)
        guest_flags |= AARCH64_LINUX_O_LARGEFILE;
    if (flags & O_DIRECTORY_)
        guest_flags |= AARCH64_LINUX_O_DIRECTORY;
    if (flags & O_NOFOLLOW_)
        guest_flags |= AARCH64_LINUX_O_NOFOLLOW;
    return guest_flags;
}

static bool fd_user_range_fits(qword_t address, qword_t size) {
    return address <= AARCH64_FD_USER_ADDRESS_LIMIT &&
            size <= AARCH64_FD_USER_ADDRESS_LIMIT - address;
}

static int fd_user_range_error(struct guest_linux_user_fault *fault,
        qword_t address, enum guest_memory_access access) {
    *fault = (struct guest_linux_user_fault) {
        .address = address,
        .access = (dword_t) access,
        .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
    };
    return _EFAULT;
}

static int read_flock(const struct guest_linux_syscall_context *context,
        qword_t address, struct aarch64_linux_flock *wire,
        struct guest_linux_user_fault *fault) {
    if (!fd_user_range_fits(address, sizeof(*wire)))
        return fd_user_range_error(fault, address, GUEST_MEMORY_READ);
    assert(context->user.read != NULL);
    if (!context->user.read(context->user.opaque,
            address, wire, sizeof(*wire), fault))
        return _EFAULT;
    return 0;
}

static int write_flock(const struct guest_linux_syscall_context *context,
        qword_t address, const struct aarch64_linux_flock *wire,
        struct guest_linux_user_fault *fault) {
    if (!fd_user_range_fits(address, sizeof(*wire)))
        return fd_user_range_error(fault, address, GUEST_MEMORY_WRITE);
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, wire, sizeof(*wire), fault))
        return _EFAULT;
    return 0;
}

static struct flock_ flock_from_wire(
        const struct aarch64_linux_flock *wire) {
    return (struct flock_) {
        .type = wire->type,
        .whence = wire->whence,
        .start = wire->start,
        .len = wire->len,
        .pid = wire->pid,
    };
}

static void flock_to_wire(const struct flock_ *flock,
        struct aarch64_linux_flock *wire) {
    wire->type = flock->type;
    wire->whence = flock->whence;
    wire->start = flock->start;
    wire->len = flock->len;
    wire->pid = flock->pid;
}

qword_t aarch64_linux_dispatch_dup(
        const struct guest_linux_syscall *syscall, struct task *task) {
    return fd_result(f_dupfd_task(
            task, fd_argument(syscall->arguments[0]), 0, 0));
}

qword_t aarch64_linux_dispatch_dup3(
        const struct guest_linux_syscall *syscall, struct task *task) {
    return fd_result(f_dup3_task(task,
            fd_argument(syscall->arguments[0]),
            fd_argument(syscall->arguments[1]),
            (int) (dword_t) syscall->arguments[2]));
}

qword_t aarch64_linux_dispatch_fcntl(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    fd_t fd = fd_argument(syscall->arguments[0]);
    dword_t command = (dword_t) syscall->arguments[1];
    dword_t argument = (dword_t) syscall->arguments[2];
    switch (command) {
        case F_DUPFD_:
            return fd_result(f_dupfd_task(
                    task, fd, (fd_t) (sdword_t) argument, 0));
        case F_DUPFD_CLOEXEC_:
            return fd_result(f_dupfd_task(task, fd,
                    (fd_t) (sdword_t) argument, O_CLOEXEC_));
        case F_GETFD_:
            return fd_result(f_getfd_task(task, fd));
        case F_SETFD_:
            return fd_result(f_setfd_task(task, fd, (int) argument));
        case F_GETFL_:
            return fd_result(aarch64_fcntl_getfl(task, fd));
        case F_SETFL_:
            return fd_result(f_setfl_task(task, fd, (int) argument));
    }

    struct fd *file = f_get_task_retain(task, fd);
    if (file == NULL)
        return fd_result(_EBADF);

    int error;
    struct aarch64_linux_flock wire;
    struct flock_ flock;
    qword_t address = syscall->arguments[2];
    switch (command) {
        case F_GETLK_:
            error = read_flock(context, address, &wire, fault);
            if (error < 0)
                break;
            if (file->inode == NULL) {
                error = _EBADF;
                break;
            }
            flock = flock_from_wire(&wire);
            error = fcntl_getlk(file, &flock);
            if (error < 0)
                break;
            flock_to_wire(&flock, &wire);
            error = write_flock(context, address, &wire, fault);
            break;
        case F_SETLK_:
        case F_SETLKW_:
            error = read_flock(context, address, &wire, fault);
            if (error < 0)
                break;
            if (file->inode == NULL) {
                error = _EBADF;
                break;
            }
            flock = flock_from_wire(&wire);
            error = fcntl_setlk(file, &flock,
                    command == F_SETLKW_);
            break;
        default:
            error = _EINVAL;
            break;
    }
    fd_close(file);
    return fd_result(error);
}

static bool pipe_output_fits(qword_t address) {
    return fd_user_range_fits(
            address, sizeof(struct aarch64_linux_pipe_fds));
}

qword_t aarch64_linux_dispatch_pipe2(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct task *task, struct guest_linux_user_fault *fault) {
    struct file_pipe_result pipe_result;
    int error = file_pipe2_task(task,
            (int) (dword_t) syscall->arguments[1], &pipe_result);
    if (error < 0)
        return fd_result(error);

    qword_t address = syscall->arguments[0];
    if (!pipe_output_fits(address)) {
        file_pipe_result_rollback(task, &pipe_result);
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return fd_result(_EFAULT);
    }

    const struct aarch64_linux_pipe_fds wire = {
        .read_fd = pipe_result.fds[0],
        .write_fd = pipe_result.fds[1],
    };
    assert(context->user.write != NULL);
    if (!context->user.write(context->user.opaque,
            address, &wire, sizeof(wire), fault)) {
        file_pipe_result_rollback(task, &pipe_result);
        return fd_result(_EFAULT);
    }
    file_pipe_result_release(&pipe_result);
    return 0;
}

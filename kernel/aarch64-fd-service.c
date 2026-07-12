#include <assert.h>

#include "fs/fd.h"
#include "fs/pipe.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-fd-service.h"
#include "kernel/errno.h"
#include "kernel/fs.h"

#define AARCH64_FD_USER_ADDRESS_LIMIT \
    (AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1))

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
        const struct guest_linux_syscall *syscall, struct task *task) {
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
            return fd_result(f_getfl_task(task, fd));
        case F_SETFL_:
            return fd_result(f_setfl_task(task, fd, (int) argument));
        default:
            return fd_result(f_getfd_task(task, fd) < 0 ?
                    _EBADF : _EINVAL);
    }
}

static bool pipe_output_fits(qword_t address) {
    return address <= AARCH64_FD_USER_ADDRESS_LIMIT &&
            sizeof(struct aarch64_linux_pipe_fds) <=
            AARCH64_FD_USER_ADDRESS_LIMIT - address;
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

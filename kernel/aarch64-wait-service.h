#ifndef KERNEL_AARCH64_WAIT_SERVICE_H
#define KERNEL_AARCH64_WAIT_SERVICE_H

#include "guest/linux/syscall-service.h"

qword_t aarch64_linux_dispatch_wait4(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault);

#endif

#ifndef GUEST_LINUX_SYSCALL_SERVICE_H
#define GUEST_LINUX_SYSCALL_SERVICE_H

#include "guest/linux/syscall.h"
#include "guest/memory/address-space.h"

struct guest_linux_user_access {
    void *opaque;
    bool (*read)(void *opaque, qword_t address,
            void *destination, size_t size,
            struct guest_memory_fault *fault);
    bool (*write)(void *opaque, qword_t address,
            const void *source, size_t size,
            struct guest_memory_fault *fault);
};

struct guest_linux_syscall_context {
    void *runtime_opaque;
    void *task_opaque;
    struct guest_linux_user_access user;
};

typedef qword_t (*guest_linux_syscall_dispatch)(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_memory_fault *fault);

struct guest_linux_syscall_service {
    void *runtime_opaque;
    guest_linux_syscall_dispatch dispatch;
};

#endif

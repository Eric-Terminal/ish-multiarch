#ifndef GUEST_LINUX_SYSCALL_SERVICE_H
#define GUEST_LINUX_SYSCALL_SERVICE_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

#include "guest/linux/syscall.h"

struct guest_linux_user_fault {
    qword_t address;
    dword_t access;
    dword_t kind;
};

_Static_assert(offsetof(struct guest_linux_user_fault, address) == 0 &&
        sizeof(struct guest_linux_user_fault) == 16,
        "Linux syscall service 的用户故障 ABI 必须与 guest 选择无关");
_Static_assert(offsetof(struct guest_linux_user_fault, access) == 8 &&
        offsetof(struct guest_linux_user_fault, kind) == 12,
        "Linux syscall service 的用户故障字段偏移必须固定");

struct guest_linux_user_access {
    void *opaque;
    bool (*read)(void *opaque, qword_t address,
            void *destination, dword_t size,
            struct guest_linux_user_fault *fault);
    bool (*write)(void *opaque, qword_t address,
            const void *source, dword_t size,
            struct guest_linux_user_fault *fault);
};

struct guest_linux_syscall_context {
    void *runtime_opaque;
    void *task_opaque;
    struct guest_linux_user_access user;
};

typedef qword_t (*guest_linux_syscall_dispatch)(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault);

struct guest_linux_syscall_service {
    void *runtime_opaque;
    guest_linux_syscall_dispatch dispatch;
};

#endif

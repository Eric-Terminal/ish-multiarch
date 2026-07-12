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

enum guest_linux_syscall_disposition {
    GUEST_LINUX_SYSCALL_RETURN,
    GUEST_LINUX_SYSCALL_REPLACED_IMAGE,
};

struct guest_linux_syscall_completion {
    dword_t disposition;
};

struct guest_linux_syscall_context {
    void *runtime_opaque;
    void *task_opaque;
    // 供 sigaltstack 等没有显式 SP 参数的系统调用读取调用点 guest 栈指针。
    qword_t stack_pointer;
    // exec 成功后由服务端标记；runtime 随即停止写回旧 CPU 并返回安全点。
    struct guest_linux_syscall_completion *completion;
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

_Static_assert(sizeof(enum guest_linux_syscall_disposition) == 4 &&
        sizeof(struct guest_linux_syscall_completion) == 4,
        "Linux syscall 完成状态必须保持 32 位");

#endif

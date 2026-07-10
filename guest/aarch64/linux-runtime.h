#ifndef GUEST_AARCH64_LINUX_RUNTIME_H
#define GUEST_AARCH64_LINUX_RUNTIME_H

#include "guest/aarch64/linux-syscall.h"
#include "guest/memory/tlb.h"

struct aarch64_linux_services {
    void *opaque;
    sqword_t (*write)(void *opaque, qword_t fd,
            const byte_t *data, size_t size);
};

enum aarch64_linux_syscall_action {
    AARCH64_LINUX_SYSCALL_RESUME,
    AARCH64_LINUX_SYSCALL_EXIT,
    AARCH64_LINUX_SYSCALL_EXIT_GROUP,
};

struct aarch64_linux_syscall_result {
    enum aarch64_linux_syscall_action action;
    qword_t return_value;
    dword_t exit_status;
    struct guest_memory_fault fault;
};

struct aarch64_linux_syscall_result aarch64_linux_dispatch_syscall(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_linux_services *services);

#endif

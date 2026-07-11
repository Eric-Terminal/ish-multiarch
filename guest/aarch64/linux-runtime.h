#ifndef GUEST_AARCH64_LINUX_RUNTIME_H
#define GUEST_AARCH64_LINUX_RUNTIME_H

#include "guest/aarch64/linux-syscall.h"
#include "guest/linux/memory.h"
#include "guest/linux/signal-service.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/tlb.h"

struct aarch64_linux_services {
    const struct guest_linux_syscall_service *syscalls;
    const struct guest_linux_signal_service *signals;
    // 未设置 SA_RESTORER 时使用的 guest 可执行 rt_sigreturn 入口。
    guest_addr_t signal_trampoline;
};

struct aarch64_linux_runtime {
    struct guest_linux_mm memory;
    const struct aarch64_linux_services *services;
};

struct aarch64_linux_task {
    pid_t_ tid;
    guest_addr_t clear_child_tid;
    void *service_opaque;
};

enum aarch64_linux_syscall_action {
    AARCH64_LINUX_SYSCALL_RESUME,
    AARCH64_LINUX_SYSCALL_EXIT,
    AARCH64_LINUX_SYSCALL_EXIT_GROUP,
    AARCH64_LINUX_SYSCALL_STOP,
    AARCH64_LINUX_SYSCALL_TERMINATE,
};

struct aarch64_linux_syscall_result {
    enum aarch64_linux_syscall_action action;
    qword_t return_value;
    dword_t exit_status;
    sdword_t signal;
    struct guest_memory_fault fault;
};

void aarch64_linux_runtime_init(struct aarch64_linux_runtime *runtime,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit,
        const struct aarch64_linux_services *services);
void aarch64_linux_task_init(struct aarch64_linux_task *task, pid_t_ tid,
        void *service_opaque);

// 调用方应在可观察到完整 guest CPU 状态的安全点轮询。
struct guest_linux_signal_poll_result aarch64_linux_poll_signals(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_linux_runtime *runtime,
        const struct aarch64_linux_task *task);

struct aarch64_linux_syscall_result aarch64_linux_dispatch_syscall(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        struct aarch64_linux_runtime *runtime,
        struct aarch64_linux_task *task);

#endif

#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/aarch64/linux-signal-frame.h"
#include "guest/linux/errno.h"
#include "guest/linux/user-memory.h"
#include "guest/memory/page-table.h"

#define STACK_BASE UINT64_C(0x0000700000000000)
#define STACK_SIZE (4 * GUEST_MEMORY_PAGE_SIZE)
#define STACK_TOP (STACK_BASE + STACK_SIZE)
#define ALTSTACK_BASE UINT64_C(0x0000710000000000)
#define ALTSTACK_SIZE (3 * GUEST_MEMORY_PAGE_SIZE)
#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00004000eeee0000)
#define RESTORER UINT64_C(0x00004000dddd0000)
#define HANDLER UINT64_C(0x0000400011110000)
#define FORCED_HANDLER UINT64_C(0x0000400022220000)
#define SYS_RT_SIGRETURN UINT64_C(139)

struct return_probe {
    void *expected_task_opaque;
    struct cpu_state *live_cpu;
    struct cpu_state callback_cpu;
    struct cpu_state poll_cpu;
    struct guest_linux_signal_delivery delivery;
    struct guest_linux_signal_restore_request restored;
    dword_t poll_status;
    sdword_t poll_signal;
    unsigned poll_calls;
    unsigned restore_calls;
    unsigned bad_frame_calls;
    unsigned syscall_calls;
    qword_t bad_frame_address;
    bool install_delivery;
};

static qword_t encoded_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static void map_range(struct guest_page_table *table,
        guest_addr_t base, qword_t size) {
    for (qword_t offset = 0; offset < size;
            offset += GUEST_MEMORY_PAGE_SIZE) {
        byte_t *page;
        assert(guest_page_table_map(table, base + offset,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
                &page) == GUEST_PAGE_TABLE_OK);
    }
}

static struct cpu_state make_interrupted_cpu(void) {
    struct cpu_state cpu = {
        .cycle = 41,
        .sp = STACK_TOP,
        .pc = UINT64_C(0x00004000abcdef00),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000000),
        .tpidr_el0 = UINT64_C(0x1122334455667788),
        .exclusive = {
            .address = UINT64_C(0x0000500000000000),
            .size = 8,
            .valid = true,
        },
        .segfault_addr = UINT64_C(0x00006000aaaabbbb),
    };
    for (byte_t i = 0; i < 31; i++)
        cpu.x[i] = UINT64_C(0x1000000000000000) + i;
    for (byte_t reg = 0; reg < 32; reg++) {
        for (byte_t index = 0; index < 16; index++)
            cpu.v[reg].b[index] = (byte_t) (reg * 16 + index);
    }
    return cpu;
}

static struct cpu_state build_handler_cpu(
        const struct cpu_state *interrupted,
        struct guest_tlb *tlb, guest_addr_t *frame_address) {
    const struct aarch64_linux_signal_delivery delivery = {
        .signal = 10,
        .handler = HANDLER,
        .restorer = RESTORER,
        .action_flags = AARCH64_LINUX_SA_SIGINFO |
                AARCH64_LINUX_SA_RESTORER,
        .blocked_mask = UINT64_C(0x1020304050607080),
        .altstack = {
            .sp = ALTSTACK_BASE,
            .size = ALTSTACK_SIZE,
        },
        .stack_bottom = STACK_BASE,
        .stack_top = interrupted->sp,
        .fault_address = interrupted->segfault_addr,
    };
    struct cpu_state handler;
    struct guest_memory_fault fault;
    assert(aarch64_linux_build_rt_sigframe(
            interrupted, tlb, &delivery, &handler,
            frame_address, &fault) == AARCH64_LINUX_SIGNAL_FRAME_OK);
    // 模拟 canonical trampoline 已执行 mov x8 与 svc 两条指令。
    handler.pc = RESTORER + 8;
    handler.x[8] = SYS_RT_SIGRETURN;
    return handler;
}

static struct aarch64_linux_rt_sigframe read_frame(
        struct guest_tlb *tlb, guest_addr_t address) {
    struct aarch64_linux_rt_sigframe frame;
    struct guest_memory_fault fault;
    assert(guest_linux_copy_from_user(
            tlb, address, &frame, sizeof(frame), &fault));
    return frame;
}

static void write_frame(struct guest_tlb *tlb, guest_addr_t address,
        const struct aarch64_linux_rt_sigframe *frame) {
    struct guest_memory_fault fault;
    assert(guest_linux_copy_to_user(
            tlb, address, frame, sizeof(*frame), &fault));
}

static void assert_context(
        const struct guest_linux_signal_context *context,
        const struct return_probe *probe) {
    assert(context->runtime_opaque == probe);
    assert(context->task_opaque == probe->expected_task_opaque);
}

static void restore_signal(
        const struct guest_linux_signal_context *context,
        const struct guest_linux_signal_restore_request *request) {
    struct return_probe *probe = context->runtime_opaque;
    assert_context(context, probe);
    assert(memcmp(probe->live_cpu, &probe->callback_cpu,
            sizeof(*probe->live_cpu)) == 0);
    probe->restored = *request;
    probe->restore_calls++;
}

static void report_bad_frame(
        const struct guest_linux_signal_context *context,
        qword_t frame_address) {
    struct return_probe *probe = context->runtime_opaque;
    assert_context(context, probe);
    assert(memcmp(probe->live_cpu, &probe->callback_cpu,
            sizeof(*probe->live_cpu)) == 0);
    probe->bad_frame_address = frame_address;
    probe->bad_frame_calls++;
}

static struct guest_linux_signal_poll_result poll_signal(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    struct return_probe *probe = context->runtime_opaque;
    assert_context(context, probe);
    assert(memcmp(probe->live_cpu, &probe->poll_cpu,
            sizeof(*probe->live_cpu)) == 0);
    probe->poll_calls++;
    if (probe->install_delivery) {
        assert(installer(installer_opaque, &probe->delivery) ==
                GUEST_LINUX_SIGNAL_INSTALL_COMPLETE);
    }
    return (struct guest_linux_signal_poll_result) {
        .status = probe->poll_status,
        .signal = probe->poll_signal,
    };
}

static qword_t unexpected_syscall(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    (void) syscall;
    (void) fault;
    struct return_probe *probe = context->runtime_opaque;
    probe->syscall_calls++;
    return encoded_error(GUEST_LINUX_ENOSYS);
}

static struct aarch64_linux_services make_services(
        struct return_probe *probe,
        struct guest_linux_signal_service *signals,
        struct guest_linux_syscall_service *syscalls) {
    *signals = (struct guest_linux_signal_service) {
        .runtime_opaque = probe,
        .poll = poll_signal,
        .restore = restore_signal,
        .bad_frame = report_bad_frame,
    };
    *syscalls = (struct guest_linux_syscall_service) {
        .runtime_opaque = probe,
        .dispatch = unexpected_syscall,
    };
    return (struct aarch64_linux_services) {
        .syscalls = syscalls,
        .signals = signals,
        .signal_trampoline = SIGNAL_TRAMPOLINE,
    };
}

static struct guest_linux_signal_delivery forced_delivery(void) {
    return (struct guest_linux_signal_delivery) {
        .info = {
            .signal = 11,
            .code = 1,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_FAULT,
            .fault.address = UINT64_C(0x00007000deadbeef),
        },
        .action = {
            .handler = FORCED_HANDLER,
            .flags = AARCH64_LINUX_SA_SIGINFO |
                    AARCH64_LINUX_SA_RESTORER,
            .restorer = RESTORER,
        },
        .blocked_mask = UINT64_C(0x55aa55aa55aa55aa),
    };
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    map_range(&table, STACK_BASE, STACK_SIZE);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);

    int task_opaque;
    struct aarch64_linux_task task;
    aarch64_linux_task_init(&task, 1234, &task_opaque);
    struct guest_linux_mm memory;
    struct aarch64_linux_runtime runtime;
    struct return_probe probe = {0};
    struct guest_linux_signal_service signal_service;
    struct guest_linux_syscall_service syscall_service;
    struct aarch64_linux_services services = make_services(
            &probe, &signal_service, &syscall_service);
    aarch64_linux_runtime_init(&runtime, &memory, &table,
            BRK_BASE, BRK_LIMIT, &services);
    probe.expected_task_opaque = &task_opaque;

    struct cpu_state interrupted = make_interrupted_cpu();
    struct cpu_state expected_resume = interrupted;
    aarch64_clear_exclusive(&expected_resume);
    guest_addr_t frame_address;
    struct cpu_state handler = build_handler_cpu(
            &interrupted, &tlb, &frame_address);
    probe.live_cpu = &handler;
    probe.callback_cpu = handler;
    probe.poll_cpu = expected_resume;
    struct aarch64_linux_syscall_result result =
            aarch64_linux_dispatch_syscall(
                    &handler, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.return_value == interrupted.x[0]);
    assert(result.signal == 0);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(probe.restore_calls == 1 && probe.bad_frame_calls == 0 &&
            probe.poll_calls == 1 && probe.syscall_calls == 0);
    assert(probe.restored.stack_pointer == interrupted.sp);
    assert(probe.restored.blocked_mask ==
            UINT64_C(0x1020304050607080));
    assert(probe.restored.altstack.base == ALTSTACK_BASE);
    assert(probe.restored.altstack.size == ALTSTACK_SIZE);
    assert(probe.restored.altstack.flags == 0 &&
            probe.restored.altstack.reserved == 0);
    assert(memcmp(&handler, &expected_resume, sizeof(handler)) == 0);
    assert(handler.x[0] != encoded_error(GUEST_LINUX_ENOSYS));

    // 恢复掩码后新变为可派送的信号必须抢在下一条 guest 指令前安装。
    probe = (struct return_probe) {
        .expected_task_opaque = &task_opaque,
        .delivery = forced_delivery(),
        .poll_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .poll_signal = 11,
        .install_delivery = true,
    };
    signal_service.runtime_opaque = &probe;
    syscall_service.runtime_opaque = &probe;
    handler = build_handler_cpu(&interrupted, &tlb, &frame_address);
    probe.live_cpu = &handler;
    probe.callback_cpu = handler;
    probe.poll_cpu = expected_resume;
    result = aarch64_linux_dispatch_syscall(
            &handler, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.return_value == interrupted.x[0]);
    assert(result.signal == 11 && probe.restore_calls == 1 &&
            probe.bad_frame_calls == 0 && probe.poll_calls == 1);
    assert(handler.pc == FORCED_HANDLER && handler.x[0] == 11);
    struct aarch64_linux_rt_sigframe frame =
            read_frame(&tlb, handler.sp);
    assert(frame.uc.mcontext.regs[0] == interrupted.x[0]);
    assert(frame.uc.mcontext.pc == interrupted.pc);

    // ptrace 可消费坏帧信号并让 poll 返回 IDLE，恢复状态仍不得提交。
    probe = (struct return_probe) {
        .expected_task_opaque = &task_opaque,
        .poll_status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
    signal_service.runtime_opaque = &probe;
    syscall_service.runtime_opaque = &probe;
    handler = build_handler_cpu(&interrupted, &tlb, &frame_address);
    frame = read_frame(&tlb, frame_address);
    frame.uc.mcontext.pstate |= 1;
    write_frame(&tlb, frame_address, &frame);
    struct cpu_state bad_handler = handler;
    struct cpu_state bad_return = bad_handler;
    bad_return.x[0] = 0;
    probe.live_cpu = &handler;
    probe.callback_cpu = bad_handler;
    probe.poll_cpu = bad_return;
    result = aarch64_linux_dispatch_syscall(
            &handler, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.signal == 0 && probe.restore_calls == 0 &&
            probe.bad_frame_calls == 1 && probe.poll_calls == 1);
    assert(probe.bad_frame_address == frame_address);
    assert(result.return_value == 0);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(&handler, &bad_return, sizeof(handler)) == 0);

    // 未对齐帧也可在同一安全点改派一个可捕获的强制 SIGSEGV。
    probe = (struct return_probe) {
        .expected_task_opaque = &task_opaque,
        .delivery = forced_delivery(),
        .poll_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .poll_signal = 11,
        .install_delivery = true,
    };
    signal_service.runtime_opaque = &probe;
    syscall_service.runtime_opaque = &probe;
    handler = build_handler_cpu(&interrupted, &tlb, &frame_address);
    handler.sp += 8;
    bad_handler = handler;
    bad_return = bad_handler;
    bad_return.x[0] = 0;
    probe.live_cpu = &handler;
    probe.callback_cpu = bad_handler;
    probe.poll_cpu = bad_return;
    result = aarch64_linux_dispatch_syscall(
            &handler, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.signal == 11 && probe.bad_frame_calls == 1 &&
            probe.restore_calls == 0 && probe.poll_calls == 1);
    assert(probe.bad_frame_address == bad_handler.sp);
    assert(handler.pc == FORCED_HANDLER && handler.x[0] == 11);
    frame = read_frame(&tlb, handler.sp);
    assert(frame.uc.mcontext.regs[0] == 0);
    assert(frame.uc.mcontext.sp == bad_handler.sp);
    assert(frame.uc.mcontext.pc == bad_handler.pc);
    assert(frame.uc.mcontext.pc == RESTORER + 8);
    assert(frame.uc.mcontext.regs[8] == SYS_RT_SIGRETURN);

    // 跨页读取失败同样保持 handler CPU，并保留精确内存故障诊断。
    probe = (struct return_probe) {
        .expected_task_opaque = &task_opaque,
        .poll_status = GUEST_LINUX_SIGNAL_POLL_TERMINATE,
        .poll_signal = 11,
    };
    signal_service.runtime_opaque = &probe;
    syscall_service.runtime_opaque = &probe;
    handler = build_handler_cpu(&interrupted, &tlb, &frame_address);
    handler.sp = STACK_TOP - 64;
    bad_handler = handler;
    bad_return = bad_handler;
    bad_return.x[0] = 0;
    probe.live_cpu = &handler;
    probe.callback_cpu = bad_handler;
    probe.poll_cpu = bad_return;
    result = aarch64_linux_dispatch_syscall(
            &handler, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_TERMINATE);
    assert(probe.bad_frame_calls == 1 &&
            probe.bad_frame_address == bad_handler.sp);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == STACK_TOP);
    assert(result.return_value == 0);
    assert(memcmp(&handler, &bad_return, sizeof(handler)) == 0);

    // 缺少完整 signal capability 时 syscall 139 必须本地返回 ENOSYS。
    probe = (struct return_probe) {0};
    syscall_service.runtime_opaque = &probe;
    const struct aarch64_linux_services no_signal_services = {
        .syscalls = &syscall_service,
    };
    runtime.services = &no_signal_services;
    handler = build_handler_cpu(&interrupted, &tlb, &frame_address);
    result = aarch64_linux_dispatch_syscall(
            &handler, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.return_value == encoded_error(GUEST_LINUX_ENOSYS));
    assert(handler.x[0] == encoded_error(GUEST_LINUX_ENOSYS));
    assert(probe.syscall_calls == 0);

    // 任一恢复回调缺失都不得半解码帧或下沉到普通 syscall service。
    for (unsigned missing = 0; missing < 3; missing++) {
        probe = (struct return_probe) {
            .expected_task_opaque = &task_opaque,
        };
        services = make_services(
                &probe, &signal_service, &syscall_service);
        if (missing == 0)
            signal_service.poll = NULL;
        else if (missing == 1)
            signal_service.restore = NULL;
        else
            signal_service.bad_frame = NULL;
        runtime.services = &services;
        handler = build_handler_cpu(
                &interrupted, &tlb, &frame_address);
        struct cpu_state unavailable = handler;
        unavailable.x[0] = encoded_error(GUEST_LINUX_ENOSYS);
        probe.live_cpu = &handler;
        probe.poll_cpu = unavailable;
        result = aarch64_linux_dispatch_syscall(
                &handler, &tlb, &runtime, &task);
        assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
        assert(result.return_value ==
                encoded_error(GUEST_LINUX_ENOSYS));
        assert(result.signal == 0 && probe.restore_calls == 0 &&
                probe.bad_frame_calls == 0 && probe.syscall_calls == 0);
        assert(probe.poll_calls == (missing == 0 ? 0 : 1));
        assert(memcmp(&handler, &unavailable,
                sizeof(handler)) == 0);
    }

    guest_page_table_destroy(&table);
    return 0;
}

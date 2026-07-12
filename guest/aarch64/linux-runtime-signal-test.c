#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/linux/errno.h"
#include "guest/linux/user-memory.h"
#include "guest/memory/page-table.h"

#define NORMAL_STACK_BASE UINT64_C(0x0000700000000000)
#define NORMAL_STACK_SIZE (4 * GUEST_MEMORY_PAGE_SIZE)
#define NORMAL_STACK_TOP (NORMAL_STACK_BASE + NORMAL_STACK_SIZE)
#define ALTSTACK_BASE UINT64_C(0x0000710000000000)
#define ALTSTACK_SIZE (3 * GUEST_MEMORY_PAGE_SIZE)
#define ALTSTACK_TOP (ALTSTACK_BASE + ALTSTACK_SIZE)
#define SMALL_ALTSTACK_BASE UINT64_C(0x0000720000000000)
#define SMALL_ALTSTACK_SIZE \
    (sizeof(struct aarch64_linux_rt_sigframe) + 16 - 1)
#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00004000eeee0000)
#define SYS_READ UINT64_C(63)
#define SYS_PSELECT6 UINT64_C(72)
#define SYS_NANOSLEEP UINT64_C(101)
#define DISABLED_ALTSTACK 2
#define MAX_INSTALLS 2

struct signal_probe {
    void *expected_runtime_opaque;
    void *expected_task_opaque;
    struct cpu_state *watched_cpu;
    struct cpu_state unpublished_cpu;
    struct guest_linux_signal_delivery deliveries[MAX_INSTALLS];
    enum guest_linux_signal_install_status expected[MAX_INSTALLS];
    size_t delivery_count;
    dword_t return_status;
    sdword_t return_signal;
    unsigned calls;
    unsigned unpublished_checks;
    bool saw_context;
    bool omit_signal_trampoline;
};

struct poll_bridge {
    struct signal_probe *probe;
};

struct interrupted_syscall_probe {
    qword_t number;
    qword_t result;
    unsigned calls;
};

static qword_t return_interrupted_syscall(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct interrupted_syscall_probe *probe = context->runtime_opaque;
    assert(syscall->number == probe->number);
    assert(fault->kind == GUEST_MEMORY_FAULT_NONE);
    probe->calls++;
    return probe->result;
}

static struct guest_linux_signal_poll_result fake_signal_poll(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    struct poll_bridge *bridge = context->runtime_opaque;
    struct signal_probe *probe = bridge->probe;
    assert(context->runtime_opaque == probe->expected_runtime_opaque);
    assert(context->task_opaque == probe->expected_task_opaque);
    assert(memcmp(probe->watched_cpu, &probe->unpublished_cpu,
            sizeof(*probe->watched_cpu)) == 0);
    probe->calls++;
    probe->saw_context = true;

    for (size_t i = 0; i < probe->delivery_count; i++) {
        enum guest_linux_signal_install_status status = installer(
                installer_opaque, &probe->deliveries[i]);
        assert(status == probe->expected[i]);
        // installer 只能准备候选状态，CPU 要等 poll 完成后再发布。
        assert(memcmp(probe->watched_cpu, &probe->unpublished_cpu,
                sizeof(*probe->watched_cpu)) == 0);
        probe->unpublished_checks++;
    }
    return (struct guest_linux_signal_poll_result) {
        .status = probe->return_status,
        .signal = probe->return_signal,
    };
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

static struct cpu_state make_cpu(guest_addr_t sp) {
    struct cpu_state cpu = {
        .cycle = 41,
        .sp = sp,
        .pc = UINT64_C(0x0000400012345000),
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
    return cpu;
}

static struct guest_linux_signal_delivery make_delivery(
        sdword_t signal, qword_t handler) {
    return (struct guest_linux_signal_delivery) {
        .info = {
            .signal = signal,
            .error = -3,
            .code = 1,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_FAULT,
            .fault.address = UINT64_C(0x0000600011112222),
        },
        .action = {
            .handler = handler,
            .flags = AARCH64_LINUX_SA_SIGINFO,
            .restorer = UINT64_C(0x00004000ffff0000),
            .mask = UINT64_C(0x0000000000000040),
        },
        .blocked_mask = UINT64_C(0x1020304050607080),
    };
}

static guest_addr_t expected_frame_address(guest_addr_t stack_top) {
    guest_addr_t record = (stack_top - 16) & ~UINT64_C(0xf);
    return record - sizeof(struct aarch64_linux_rt_sigframe);
}

static struct aarch64_linux_rt_sigframe read_frame(
        struct guest_tlb *tlb, guest_addr_t address) {
    struct aarch64_linux_rt_sigframe frame;
    struct guest_memory_fault fault;
    assert(guest_linux_copy_from_user(
            tlb, address, &frame, sizeof(frame), &fault));
    return frame;
}

static struct guest_linux_signal_poll_result run_probe(
        struct aarch64_linux_runtime *runtime,
        const struct aarch64_linux_task *task,
        struct guest_tlb *tlb, struct cpu_state *cpu,
        struct signal_probe *probe) {
    struct poll_bridge bridge = {.probe = probe};
    const struct guest_linux_signal_service signal_service = {
        .runtime_opaque = &bridge,
        .poll = fake_signal_poll,
    };
    const struct aarch64_linux_services services = {
        .signals = &signal_service,
        .signal_trampoline = probe->omit_signal_trampoline ?
                0 : SIGNAL_TRAMPOLINE,
    };
    probe->expected_runtime_opaque = &bridge;
    probe->expected_task_opaque = task->service_opaque;
    probe->watched_cpu = cpu;
    probe->unpublished_cpu = *cpu;
    runtime->services = &services;
    struct guest_linux_signal_poll_result result =
            aarch64_linux_poll_signals(cpu, tlb, runtime, task);
    runtime->services = NULL;
    assert(probe->calls == 1 && probe->saw_context);
    return result;
}

static struct aarch64_linux_syscall_result dispatch_probe(
        struct aarch64_linux_runtime *runtime,
        struct aarch64_linux_task *task,
        struct guest_tlb *tlb, struct cpu_state *cpu,
        struct signal_probe *probe,
        const struct cpu_state *safe_point_cpu) {
    struct poll_bridge bridge = {.probe = probe};
    const struct guest_linux_signal_service signal_service = {
        .runtime_opaque = &bridge,
        .poll = fake_signal_poll,
    };
    const struct aarch64_linux_services services = {
        .signals = &signal_service,
        .signal_trampoline = SIGNAL_TRAMPOLINE,
    };
    probe->expected_runtime_opaque = &bridge;
    probe->expected_task_opaque = task->service_opaque;
    probe->watched_cpu = cpu;
    probe->unpublished_cpu = *safe_point_cpu;
    runtime->services = &services;
    struct aarch64_linux_syscall_result result =
            aarch64_linux_dispatch_syscall(
                    cpu, tlb, runtime, task);
    runtime->services = NULL;
    assert(probe->calls == 1 && probe->saw_context);
    return result;
}

static struct aarch64_linux_syscall_result dispatch_interrupted_probe(
        struct aarch64_linux_runtime *runtime,
        struct aarch64_linux_task *task,
        struct guest_tlb *tlb, struct cpu_state *cpu,
        struct signal_probe *signal_probe,
        struct interrupted_syscall_probe *syscall_probe) {
    struct poll_bridge bridge = {.probe = signal_probe};
    const struct guest_linux_signal_service signal_service = {
        .runtime_opaque = &bridge,
        .poll = fake_signal_poll,
    };
    const struct guest_linux_syscall_service syscall_service = {
        .runtime_opaque = syscall_probe,
        .dispatch = return_interrupted_syscall,
    };
    const struct aarch64_linux_services services = {
        .syscalls = &syscall_service,
        .signals = &signal_service,
        .signal_trampoline = SIGNAL_TRAMPOLINE,
    };
    signal_probe->expected_runtime_opaque = &bridge;
    signal_probe->expected_task_opaque = task->service_opaque;
    signal_probe->watched_cpu = cpu;
    signal_probe->unpublished_cpu = *cpu;
    signal_probe->unpublished_cpu.x[0] = syscall_probe->result;
    runtime->services = &services;
    struct aarch64_linux_syscall_result result =
            aarch64_linux_dispatch_syscall(
                    cpu, tlb, runtime, task);
    runtime->services = NULL;
    assert(signal_probe->calls == 1 && signal_probe->saw_context);
    assert(syscall_probe->calls == 1);
    return result;
}

static void assert_handler(const struct cpu_state *cpu,
        const struct cpu_state *interrupted,
        const struct guest_linux_signal_delivery *delivery,
        guest_addr_t stack_top) {
    guest_addr_t frame_address = expected_frame_address(stack_top);
    assert(cpu->pc == delivery->action.handler);
    assert(cpu->sp == frame_address);
    assert(cpu->x[0] == (qword_t) (dword_t) delivery->info.signal);
    assert(cpu->x[1] == frame_address);
    assert(cpu->x[2] == frame_address + 128);
    qword_t expected_restorer =
            delivery->action.flags & AARCH64_LINUX_SA_RESTORER ?
            delivery->action.restorer : SIGNAL_TRAMPOLINE;
    assert(cpu->x[30] == expected_restorer);
    assert(cpu->nzcv == interrupted->nzcv);
    assert(!cpu->exclusive.valid);
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    map_range(&table, NORMAL_STACK_BASE, NORMAL_STACK_SIZE);
    map_range(&table, ALTSTACK_BASE, ALTSTACK_SIZE);
    map_range(&table, SMALL_ALTSTACK_BASE,
            2 * GUEST_MEMORY_PAGE_SIZE);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);

    struct guest_linux_mm memory;
    struct aarch64_linux_runtime runtime;
    aarch64_linux_runtime_init(&runtime, &memory, &table,
            BRK_BASE, BRK_LIMIT, NULL);
    int task_opaque;
    struct aarch64_linux_task task;
    aarch64_linux_task_init(&task, 1234, &task_opaque);

    struct cpu_state cpu = make_cpu(NORMAL_STACK_TOP);
    struct cpu_state cpu_before = cpu;
    struct guest_linux_signal_poll_result result =
            aarch64_linux_poll_signals(&cpu, &tlb, &runtime, &task);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    const struct aarch64_linux_services no_signal_services = {0};
    runtime.services = &no_signal_services;
    result = aarch64_linux_poll_signals(
            &cpu, &tlb, &runtime, &task);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    const struct guest_linux_signal_service no_poll_service = {0};
    const struct aarch64_linux_services no_poll_services = {
        .signals = &no_poll_service,
    };
    runtime.services = &no_poll_services;
    result = aarch64_linux_poll_signals(
            &cpu, &tlb, &runtime, &task);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    struct signal_probe probe = {
        .return_status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(probe.unpublished_checks == 0);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu_before = cpu;
    struct guest_linux_signal_delivery normal = make_delivery(
            11, UINT64_C(0x0000400011110000));
    probe = (struct signal_probe) {
        .deliveries = {normal},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = normal.info.signal,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert(result.signal == normal.info.signal);
    assert(probe.unpublished_checks == 1);
    assert_handler(&cpu, &cpu_before, &normal, cpu_before.sp);
    struct aarch64_linux_rt_sigframe frame =
            read_frame(&tlb, cpu.sp);
    assert(frame.info.fault.address == normal.info.fault.address);
    assert(frame.uc.mcontext.fault_address == cpu_before.segfault_addr);
    assert(frame.info.fault.address != frame.uc.mcontext.fault_address);
    assert(frame.uc.mcontext.sp == cpu_before.sp);
    assert(frame.uc.mcontext.pc == cpu_before.pc);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu_before = cpu;
    struct guest_linux_signal_delivery explicit_restorer = normal;
    explicit_restorer.action.flags |= AARCH64_LINUX_SA_RESTORER;
    probe = (struct signal_probe) {
        .deliveries = {explicit_restorer},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = explicit_restorer.info.signal,
        .omit_signal_trampoline = true,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert_handler(&cpu, &cpu_before, &explicit_restorer,
            cpu_before.sp);
    assert(cpu.x[30] == explicit_restorer.action.restorer);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu_before = cpu;
    probe = (struct signal_probe) {
        .deliveries = {normal},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_IDLE,
        .omit_signal_trampoline = true,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu_before = cpu;
    struct guest_linux_signal_delivery enter_altstack = make_delivery(
            12, UINT64_C(0x0000400022220000));
    enter_altstack.action.flags |= AARCH64_LINUX_SA_ONSTACK;
    enter_altstack.altstack = (struct guest_linux_signal_stack) {
        .base = ALTSTACK_BASE,
        .size = ALTSTACK_SIZE,
    };
    probe = (struct signal_probe) {
        .deliveries = {enter_altstack},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = enter_altstack.info.signal,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert_handler(&cpu, &cpu_before, &enter_altstack, ALTSTACK_TOP);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.stack.sp == ALTSTACK_BASE);
    assert(frame.uc.stack.size == ALTSTACK_SIZE);
    assert(frame.uc.mcontext.sp == NORMAL_STACK_TOP);

    guest_addr_t nested_sp = ALTSTACK_TOP - 256;
    cpu = make_cpu(nested_sp);
    cpu_before = cpu;
    struct guest_linux_signal_delivery nested = enter_altstack;
    nested.info.signal = 13;
    nested.action.handler = UINT64_C(0x0000400033330000);
    probe = (struct signal_probe) {
        .deliveries = {nested},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = nested.info.signal,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert_handler(&cpu, &cpu_before, &nested, nested_sp);
    assert(cpu.sp != expected_frame_address(ALTSTACK_TOP));

    guest_addr_t fallback_sp = NORMAL_STACK_TOP - 128;
    cpu = make_cpu(fallback_sp);
    cpu_before = cpu;
    struct guest_linux_signal_delivery disabled = enter_altstack;
    disabled.info.signal = 14;
    disabled.action.handler = UINT64_C(0x0000400044440000);
    disabled.altstack.size = 0;
    disabled.altstack.flags = DISABLED_ALTSTACK;
    probe = (struct signal_probe) {
        .deliveries = {disabled},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = disabled.info.signal,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert_handler(&cpu, &cpu_before, &disabled, fallback_sp);
    assert(cpu.sp >= NORMAL_STACK_BASE && cpu.sp < NORMAL_STACK_TOP);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu_before = cpu;
    struct guest_linux_signal_delivery overflow = enter_altstack;
    overflow.altstack.base = AARCH64_LINUX_USER_ADDRESS_MAX - 8;
    overflow.altstack.size = 16;
    probe = (struct signal_probe) {
        .deliveries = {overflow},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(probe.unpublished_checks == 1);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    struct guest_linux_signal_delivery too_small = enter_altstack;
    too_small.altstack.base = SMALL_ALTSTACK_BASE;
    too_small.altstack.size = SMALL_ALTSTACK_SIZE;
    probe = (struct signal_probe) {
        .deliveries = {too_small},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    struct guest_linux_signal_delivery recovered = enter_altstack;
    recovered.info.signal = 15;
    recovered.action.handler = UINT64_C(0x0000400055550000);
    probe = (struct signal_probe) {
        .deliveries = {too_small, recovered},
        .expected = {
            GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT,
            GUEST_LINUX_SIGNAL_INSTALL_COMPLETE,
        },
        .delivery_count = 2,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = recovered.info.signal,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER);
    assert(result.signal == recovered.info.signal);
    assert(probe.unpublished_checks == 2);
    assert_handler(&cpu, &cpu_before, &recovered, ALTSTACK_TOP);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu_before = cpu;
    probe = (struct signal_probe) {
        .return_status = GUEST_LINUX_SIGNAL_POLL_STOP,
        .return_signal = 19,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_STOP);
    assert(result.signal == 19);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    probe = (struct signal_probe) {
        .return_status = GUEST_LINUX_SIGNAL_POLL_TERMINATE,
        .return_signal = 9,
    };
    result = run_probe(&runtime, &task, &tlb, &cpu, &probe);
    assert(result.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE);
    assert(result.signal == 9);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);

    // syscall 的 x0 必须先写回，handler 帧才能保存真实返回值。
    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu.x[8] = 178;
    cpu.x[0] = UINT64_MAX;
    struct cpu_state syscall_safe_point = cpu;
    syscall_safe_point.x[0] = (qword_t) task.tid;
    normal.info.signal = 10;
    probe = (struct signal_probe) {
        .deliveries = {normal},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = normal.info.signal,
    };
    struct aarch64_linux_syscall_result syscall = dispatch_probe(
            &runtime, &task, &tlb, &cpu, &probe,
            &syscall_safe_point);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(syscall.return_value == (qword_t) task.tid);
    assert(syscall.signal == normal.info.signal);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.mcontext.regs[0] == (qword_t) task.tid);

    const qword_t interrupted_result =
            (qword_t) -(sqword_t) GUEST_LINUX_EINTR;
    struct guest_linux_signal_delivery restarting = normal;
    restarting.info.signal = 12;
    restarting.action.flags |= AARCH64_LINUX_SA_RESTART;
    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu.x[8] = SYS_READ;
    cpu.x[0] = UINT64_C(0x0000600012340000);
    cpu.x[1] = UINT64_C(0x55aa);
    struct cpu_state syscall_entry = cpu;
    struct interrupted_syscall_probe interrupted = {
        .number = SYS_READ,
        .result = interrupted_result,
    };
    probe = (struct signal_probe) {
        .deliveries = {restarting},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = restarting.info.signal,
    };
    syscall = dispatch_interrupted_probe(
            &runtime, &task, &tlb, &cpu, &probe, &interrupted);
    assert(syscall.return_value == interrupted_result &&
            syscall.signal == restarting.info.signal);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.mcontext.pc == syscall_entry.pc - 4);
    assert(frame.uc.mcontext.regs[0] == syscall_entry.x[0]);
    assert(frame.uc.mcontext.regs[1] == syscall_entry.x[1]);
    assert(frame.uc.mcontext.regs[8] == SYS_READ);

    struct guest_linux_signal_delivery nonrestarting = restarting;
    nonrestarting.info.signal = 10;
    nonrestarting.action.flags &= ~AARCH64_LINUX_SA_RESTART;
    cpu = syscall_entry;
    interrupted = (struct interrupted_syscall_probe) {
        .number = SYS_READ,
        .result = interrupted_result,
    };
    probe = (struct signal_probe) {
        .deliveries = {nonrestarting},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = nonrestarting.info.signal,
    };
    syscall = dispatch_interrupted_probe(
            &runtime, &task, &tlb, &cpu, &probe, &interrupted);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.mcontext.pc == syscall_entry.pc);
    assert(frame.uc.mcontext.regs[0] == interrupted_result);

    cpu = syscall_entry;
    cpu.x[8] = SYS_NANOSLEEP;
    syscall_entry = cpu;
    interrupted = (struct interrupted_syscall_probe) {
        .number = SYS_NANOSLEEP,
        .result = interrupted_result,
    };
    probe = (struct signal_probe) {
        .deliveries = {restarting},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = restarting.info.signal,
    };
    syscall = dispatch_interrupted_probe(
            &runtime, &task, &tlb, &cpu, &probe, &interrupted);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.mcontext.pc == syscall_entry.pc);
    assert(frame.uc.mcontext.regs[0] == interrupted_result);

    cpu = syscall_entry;
    cpu.x[8] = SYS_PSELECT6;
    syscall_entry = cpu;
    interrupted = (struct interrupted_syscall_probe) {
        .number = SYS_PSELECT6,
        .result = interrupted_result,
    };
    probe = (struct signal_probe) {
        .deliveries = {restarting},
        .expected = {GUEST_LINUX_SIGNAL_INSTALL_COMPLETE},
        .delivery_count = 1,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = restarting.info.signal,
    };
    syscall = dispatch_interrupted_probe(
            &runtime, &task, &tlb, &cpu, &probe, &interrupted);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.mcontext.pc == syscall_entry.pc);
    assert(frame.uc.mcontext.regs[0] == interrupted_result);

    struct guest_linux_signal_delivery failed_restart = too_small;
    failed_restart.action.flags |= AARCH64_LINUX_SA_RESTART;
    struct guest_linux_signal_delivery forced_after_fault = recovered;
    forced_after_fault.action.flags |= AARCH64_LINUX_SA_RESTART;
    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu.x[8] = SYS_READ;
    syscall_entry = cpu;
    interrupted = (struct interrupted_syscall_probe) {
        .number = SYS_READ,
        .result = interrupted_result,
    };
    probe = (struct signal_probe) {
        .deliveries = {failed_restart, forced_after_fault},
        .expected = {
            GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT,
            GUEST_LINUX_SIGNAL_INSTALL_COMPLETE,
        },
        .delivery_count = 2,
        .return_status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
        .return_signal = forced_after_fault.info.signal,
    };
    syscall = dispatch_interrupted_probe(
            &runtime, &task, &tlb, &cpu, &probe, &interrupted);
    frame = read_frame(&tlb, cpu.sp);
    assert(frame.uc.mcontext.pc == syscall_entry.pc);
    assert(frame.uc.mcontext.regs[0] == interrupted_result);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu.x[8] = 178;
    syscall_safe_point = cpu;
    syscall_safe_point.x[0] = (qword_t) task.tid;
    probe = (struct signal_probe) {
        .return_status = GUEST_LINUX_SIGNAL_POLL_STOP,
        .return_signal = 19,
    };
    syscall = dispatch_probe(&runtime, &task, &tlb, &cpu,
            &probe, &syscall_safe_point);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_STOP);
    assert(syscall.signal == 19);
    assert(memcmp(&cpu, &syscall_safe_point, sizeof(cpu)) == 0);

    cpu = make_cpu(NORMAL_STACK_TOP);
    cpu.x[8] = 178;
    syscall_safe_point = cpu;
    syscall_safe_point.x[0] = (qword_t) task.tid;
    probe = (struct signal_probe) {
        .return_status = GUEST_LINUX_SIGNAL_POLL_TERMINATE,
        .return_signal = 9,
    };
    syscall = dispatch_probe(&runtime, &task, &tlb, &cpu,
            &probe, &syscall_safe_point);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_TERMINATE);
    assert(syscall.signal == 9);
    assert(memcmp(&cpu, &syscall_safe_point, sizeof(cpu)) == 0);

    // 已决定退出的任务不得再消费一个待处理信号。
    struct poll_bridge bridge = {.probe = &probe};
    const struct guest_linux_signal_service signal_service = {
        .runtime_opaque = &bridge,
        .poll = fake_signal_poll,
    };
    const struct aarch64_linux_services exit_services = {
        .signals = &signal_service,
    };
    probe.calls = 0;
    runtime.services = &exit_services;
    cpu.x[8] = 93;
    cpu.x[0] = 42;
    syscall = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_EXIT);
    assert(syscall.exit_status == 42);
    assert(probe.calls == 0);

    guest_page_table_destroy(&table);
    return 0;
}

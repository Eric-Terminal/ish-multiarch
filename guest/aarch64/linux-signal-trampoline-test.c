#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/aarch64/linux-signal-trampoline.h"
#include "guest/aarch64/runner.h"
#include "guest/linux/user-memory.h"

#define TRAMPOLINE_PAGE UINT64_C(0x00007ffffffe0000)
#define CONFLICT_PAGE UINT64_C(0x00007ffffffd0000)
#define HANDLER_PAGE UINT64_C(0x0000400000010000)
#define STACK_BASE UINT64_C(0x0000700000000000)
#define STACK_SIZE (4 * GUEST_MEMORY_PAGE_SIZE)
#define STACK_TOP (STACK_BASE + STACK_SIZE)
#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 2 * GUEST_MEMORY_PAGE_SIZE)

struct restore_probe {
    struct cpu_state *cpu;
    struct cpu_state initial_cpu;
    struct cpu_state expected_poll_cpu;
    void *expected_task_opaque;
    struct guest_linux_signal_delivery delivery;
    struct guest_linux_signal_restore_request request;
    unsigned restore_calls;
    unsigned poll_calls;
};

static void map_stack(struct guest_page_table *table) {
    for (guest_addr_t page = STACK_BASE; page < STACK_TOP;
            page += GUEST_MEMORY_PAGE_SIZE) {
        byte_t *host_page;
        assert(guest_page_table_map(table, page,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
                &host_page) == GUEST_PAGE_TABLE_OK);
    }
}

static struct cpu_state make_interrupted_cpu(void) {
    struct cpu_state cpu = {
        .sp = STACK_TOP,
        .pc = UINT64_C(0x00004000abcdef00),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000000),
        .exclusive = {
            .address = UINT64_C(0x0000500000000000),
            .size = 8,
            .valid = true,
        },
    };
    for (byte_t i = 0; i < 31; i++)
        cpu.x[i] = UINT64_C(0x1000000000000000) + i;
    return cpu;
}

static void restore_signal(
        const struct guest_linux_signal_context *context,
        const struct guest_linux_signal_restore_request *request) {
    struct restore_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == probe->expected_task_opaque);
    assert(probe->cpu->pc == TRAMPOLINE_PAGE + 8);
    assert(probe->cpu->x[8] == 139);
    probe->request = *request;
    probe->restore_calls++;
}

static void reject_bad_frame(
        const struct guest_linux_signal_context *context,
        qword_t frame_address) {
    (void) context;
    (void) frame_address;
    assert(false);
}

static struct guest_linux_signal_poll_result poll_signal(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    struct restore_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == probe->expected_task_opaque);
    if (probe->poll_calls++ == 0) {
        assert(memcmp(probe->cpu, &probe->initial_cpu,
                sizeof(*probe->cpu)) == 0);
        assert(installer(installer_opaque, &probe->delivery) ==
                GUEST_LINUX_SIGNAL_INSTALL_COMPLETE);
        return (struct guest_linux_signal_poll_result) {
            .status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
            .signal = probe->delivery.info.signal,
        };
    }
    assert(memcmp(probe->cpu, &probe->expected_poll_cpu,
            sizeof(*probe->cpu)) == 0);
    return (struct guest_linux_signal_poll_result) {
        .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));

    guest_addr_t entry = UINT64_C(0xdeadbeefdeadbeef);
    assert(aarch64_linux_map_signal_trampoline(
            &table, 0, &entry) == GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(entry == UINT64_C(0xdeadbeefdeadbeef));
    assert(aarch64_linux_map_signal_trampoline(
            &table, TRAMPOLINE_PAGE + 8, &entry) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(entry == UINT64_C(0xdeadbeefdeadbeef));
    assert(aarch64_linux_map_signal_trampoline(
            &table, UINT64_C(0x0001000000000000), &entry) ==
            GUEST_PAGE_TABLE_INVALID_ADDRESS);
    assert(entry == UINT64_C(0xdeadbeefdeadbeef));

    byte_t *conflict_page;
    assert(guest_page_table_map(&table, CONFLICT_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
            &conflict_page) == GUEST_PAGE_TABLE_OK);
    conflict_page[0] = 0xa5;
    assert(aarch64_linux_map_signal_trampoline(
            &table, CONFLICT_PAGE, &entry) ==
            GUEST_PAGE_TABLE_ALREADY_MAPPED);
    assert(entry == UINT64_C(0xdeadbeefdeadbeef) &&
            conflict_page[0] == 0xa5);

    assert(aarch64_linux_map_signal_trampoline(
            &table, TRAMPOLINE_PAGE, &entry) ==
            GUEST_PAGE_TABLE_OK);
    assert(entry == TRAMPOLINE_PAGE);
    byte_t *trampoline_page;
    unsigned permissions;
    assert(guest_page_table_lookup(&table, TRAMPOLINE_PAGE,
            &trampoline_page, &permissions) == GUEST_PAGE_TABLE_OK);
    static const byte_t expected_code[] = {
        0x68, 0x11, 0x80, 0xd2,
        0x01, 0x00, 0x00, 0xd4,
    };
    assert(permissions == (GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE));
    assert(memcmp(trampoline_page,
            expected_code, sizeof(expected_code)) == 0);

    byte_t *handler_page;
    assert(guest_page_table_map(&table, HANDLER_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_EXECUTE,
            &handler_page) == GUEST_PAGE_TABLE_OK);
    static const byte_t ret_instruction[] = {0xc0, 0x03, 0x5f, 0xd6};
    memcpy(handler_page, ret_instruction, sizeof(ret_instruction));
    map_stack(&table);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    byte_t value = 0xff;
    struct guest_memory_fault fault;
    assert(!guest_tlb_write(&tlb, TRAMPOLINE_PAGE,
            &value, sizeof(value), &fault));
    assert(fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            fault.access == GUEST_MEMORY_WRITE);

    struct cpu_state interrupted = make_interrupted_cpu();
    struct cpu_state cpu = interrupted;
    struct restore_probe probe = {
        .cpu = &cpu,
        .initial_cpu = interrupted,
        .expected_task_opaque = &table,
        .delivery = {
            .info = {.signal = 10},
            .action = {.handler = HANDLER_PAGE},
            .blocked_mask = UINT64_C(0x1020304050607080),
        },
    };
    const struct guest_linux_signal_service signal_service = {
        .runtime_opaque = &probe,
        .poll = poll_signal,
        .restore = restore_signal,
        .bad_frame = reject_bad_frame,
    };
    const struct aarch64_linux_services services = {
        .signals = &signal_service,
        .signal_trampoline = entry,
    };
    struct guest_linux_mm memory;
    struct aarch64_linux_runtime runtime;
    aarch64_linux_runtime_init(&runtime, &memory, &table,
            BRK_BASE, BRK_LIMIT, &services);
    struct aarch64_linux_task task;
    aarch64_linux_task_init(&task, 1234, &table);
    struct guest_linux_signal_poll_result delivered =
            aarch64_linux_poll_signals(
                    &cpu, &tlb, &runtime, &task);
    assert(delivered.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            delivered.signal == 10);
    assert(cpu.x[30] == entry && cpu.pc == HANDLER_PAGE);

    struct aarch64_runner runner;
    aarch64_runner_init(&runner, &tlb);
    struct aarch64_step_result step = aarch64_run_one(&runner, &cpu);
    assert(step.stop == AARCH64_STEP_RETIRED && cpu.pc == entry);
    step = aarch64_run_one(&runner, &cpu);
    assert(step.stop == AARCH64_STEP_RETIRED &&
            cpu.pc == entry + 4 && cpu.x[8] == 139);
    step = aarch64_run_one(&runner, &cpu);
    assert(step.stop == AARCH64_STEP_SYSCALL &&
            cpu.pc == entry + 8 && cpu.x[8] == 139);

    probe.expected_poll_cpu = interrupted;
    probe.expected_poll_cpu.cycle = cpu.cycle;
    aarch64_clear_exclusive(&probe.expected_poll_cpu);
    struct aarch64_linux_syscall_result syscall =
            aarch64_linux_dispatch_syscall(
                    &cpu, &tlb, &runtime, &task);
    assert(syscall.action == AARCH64_LINUX_SYSCALL_RESUME &&
            syscall.return_value == interrupted.x[0] &&
            syscall.signal == 0);
    assert(probe.restore_calls == 1 && probe.poll_calls == 2);
    assert(probe.request.stack_pointer == interrupted.sp);
    assert(probe.request.blocked_mask ==
            probe.delivery.blocked_mask);
    assert(memcmp(&cpu, &probe.expected_poll_cpu, sizeof(cpu)) == 0);

    guest_page_table_destroy(&table);
    return 0;
}

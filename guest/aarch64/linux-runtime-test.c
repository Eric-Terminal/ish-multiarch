#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/linux/errno.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-table.h"

#define DATA_PAGE UINT64_C(0x000056789abcd000)
#define READONLY_PAGE (DATA_PAGE + 2 * GUEST_MEMORY_PAGE_SIZE)
#define BRK_BASE UINT64_C(0x0000600000000000)
#define BRK_LIMIT (BRK_BASE + 4 * GUEST_MEMORY_PAGE_SIZE)

struct test_sink {
    byte_t data[64];
    size_t size;
    unsigned calls;
};

struct syscall_probe {
    void *expected_task;
    unsigned calls;
};

static qword_t encoded_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static qword_t dispatch_sink(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    if (syscall->number != 64)
        return encoded_error(GUEST_LINUX_ENOSYS);
    struct test_sink *sink = context->runtime_opaque;
    assert(syscall->arguments[0] == 1);
    assert(syscall->arguments[2] <= sizeof(sink->data) - sink->size);
    dword_t size = (dword_t) syscall->arguments[2];
    if (!context->user.read(context->user.opaque,
            syscall->arguments[1], sink->data + sink->size, size, fault))
        return encoded_error(GUEST_LINUX_EFAULT);
    sink->size += size;
    sink->calls++;
    return size;
}

static qword_t dispatch_probe(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    struct syscall_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == probe->expected_task);
    assert(context->stack_pointer == UINT64_C(0x00007fffffff1230));
    assert(syscall->number == 56);
    for (unsigned i = 0; i < GUEST_LINUX_SYSCALL_ARGUMENT_COUNT; i++)
        assert(syscall->arguments[i] ==
                UINT64_C(0x1234567800000000) + i);

    byte_t value;
    assert(context->user.read(context->user.opaque,
            DATA_PAGE + 8, &value, sizeof(value), fault));
    assert(value == 0x5a);
    assert(fault->address == DATA_PAGE + 8);
    assert(fault->access == GUEST_MEMORY_READ);
    assert(fault->kind == GUEST_MEMORY_FAULT_NONE);
    value = 0xc3;
    assert(context->user.write(context->user.opaque,
            DATA_PAGE + 9, &value, sizeof(value), fault));
    assert(fault->address == DATA_PAGE + 9);
    assert(fault->access == GUEST_MEMORY_WRITE);
    assert(fault->kind == GUEST_MEMORY_FAULT_NONE);
    assert(!context->user.read(context->user.opaque,
            DATA_PAGE + GUEST_MEMORY_PAGE_SIZE,
            &value, sizeof(value), fault));
    assert(fault->address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(fault->access == GUEST_MEMORY_READ);
    assert(fault->kind == GUEST_MEMORY_FAULT_UNMAPPED);
    probe->calls++;
    return encoded_error(GUEST_LINUX_EIO);
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *data_page;
    assert(guest_page_table_map(&table, DATA_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &data_page) ==
            GUEST_PAGE_TABLE_OK);
    byte_t *readonly_page;
    assert(guest_page_table_map(&table, READONLY_PAGE,
            GUEST_MEMORY_READ, &readonly_page) == GUEST_PAGE_TABLE_OK);
    static const byte_t message[] = "AArch64 runtime works";
    memcpy(data_page + 32, message, sizeof(message) - 1);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct test_sink sink = {0};
    const struct guest_linux_syscall_service sink_service = {
        .runtime_opaque = &sink,
        .dispatch = dispatch_sink,
    };
    const struct aarch64_linux_services services = {
        .syscalls = &sink_service,
    };
    struct aarch64_linux_runtime runtime;
    aarch64_linux_runtime_init(&runtime, &table,
            BRK_BASE, BRK_LIMIT, &services);
    int host_task;
    struct aarch64_linux_task task;
    aarch64_linux_task_init(&task, 1234, &host_task);
    struct cpu_state cpu = {0};
    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + 32;
    cpu.x[2] = sizeof(message) - 1;
    struct aarch64_linux_syscall_result result =
            aarch64_linux_dispatch_syscall(
                    &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(cpu.x[0] == sizeof(message) - 1);
    assert(sink.size == sizeof(message) - 1);
    assert(sink.calls == 1);
    assert(memcmp(sink.data, message, sink.size) == 0);

    sink = (struct test_sink) {0};
    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE;
    cpu.x[2] = 1;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EFAULT));
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(sink.size == 0 && sink.calls == 0);

    data_page[8] = 0x5a;
    readonly_page[3] = 0xa5;
    static const guest_addr_t clear_tid_addresses[] = {
        DATA_PAGE + 8,
        READONLY_PAGE + 3,
        DATA_PAGE + GUEST_MEMORY_PAGE_SIZE,
        DATA_PAGE + 9,
        UINT64_MAX,
        0,
    };
    for (size_t i = 0; i < array_size(clear_tid_addresses); i++) {
        cpu.x[8] = 96;
        cpu.x[0] = clear_tid_addresses[i];
        result = aarch64_linux_dispatch_syscall(
                &cpu, &tlb, &runtime, &task);
        assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
        assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
        assert(cpu.x[0] == 1234);
        assert(task.clear_child_tid == clear_tid_addresses[i]);
        assert(data_page[8] == 0x5a && readonly_page[3] == 0xa5);
    }

    cpu.x[8] = 178;
    cpu.x[0] = UINT64_MAX;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 1234);
    assert(task.clear_child_tid == 0);

    struct aarch64_linux_task second_task;
    aarch64_linux_task_init(&second_task, 5678, NULL);
    cpu.x[8] = 96;
    cpu.x[0] = READONLY_PAGE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &second_task);
    assert(cpu.x[0] == 5678);
    assert(second_task.clear_child_tid == READONLY_PAGE);
    assert(task.clear_child_tid == 0);

    cpu.x[8] = 99;
    cpu.x[0] = DATA_PAGE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));
    assert(task.clear_child_tid == 0);

    cpu.x[8] = 999;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));

    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE;
    cpu.x[2] = 1;
    runtime.services = NULL;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));
    const struct aarch64_linux_services empty_services = {0};
    runtime.services = &empty_services;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));
    const struct guest_linux_syscall_service empty_syscall_service = {0};
    const struct aarch64_linux_services no_dispatch_services = {
        .syscalls = &empty_syscall_service,
    };
    runtime.services = &no_dispatch_services;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));
    runtime.services = &services;

    struct syscall_probe probe = {
        .expected_task = &host_task,
    };
    const struct guest_linux_syscall_service syscall_service = {
        .runtime_opaque = &probe,
        .dispatch = dispatch_probe,
    };
    const struct aarch64_linux_services bridged_services = {
        .syscalls = &syscall_service,
    };
    runtime.services = &bridged_services;
    cpu.sp = UINT64_C(0x00007fffffff1230);
    cpu.x[8] = 178;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 1234);
    assert(probe.calls == 0);

    cpu.x[8] = 56;
    for (unsigned i = 0; i < GUEST_LINUX_SYSCALL_ARGUMENT_COUNT; i++)
        cpu.x[i] = UINT64_C(0x1234567800000000) + i;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EIO));
    assert(probe.calls == 1);
    assert(data_page[9] == 0xc3);

    cpu.x[8] = 214;
    cpu.x[0] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(cpu.x[0] == BRK_BASE);
    cpu.x[0] = BRK_BASE + 1;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(cpu.x[0] == BRK_BASE + 1);

    cpu.x[8] = 222;
    cpu.x[0] = 0;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    cpu.x[2] = GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS;
    cpu.x[4] = UINT64_MAX;
    cpu.x[5] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    guest_addr_t mapped = (guest_addr_t) runtime.memory.mmap_base;
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(cpu.x[0] == mapped);
    byte_t *mapped_page;
    unsigned mapped_permissions;
    assert(guest_page_table_lookup(&table, mapped,
            &mapped_page, &mapped_permissions) == GUEST_PAGE_TABLE_OK);
    assert(mapped_permissions ==
            (GUEST_MEMORY_READ | GUEST_MEMORY_WRITE));

    cpu.x[8] = 226;
    cpu.x[0] = mapped;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    cpu.x[2] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0);
    assert(guest_page_table_lookup(&table, mapped,
            &mapped_page, &mapped_permissions) == GUEST_PAGE_TABLE_OK);
    assert(mapped_permissions == 0);

    cpu.x[8] = 215;
    cpu.x[0] = mapped;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0);
    assert(guest_page_table_lookup(&table, mapped,
            &mapped_page, &mapped_permissions) ==
            GUEST_PAGE_TABLE_NOT_MAPPED);
    assert(probe.calls == 1);

    cpu.x[8] = 222;
    cpu.x[0] = 0;
    cpu.x[1] = 0;
    cpu.x[2] = 0;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_ANONYMOUS;
    cpu.x[4] = UINT64_MAX;
    cpu.x[5] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EINVAL));
    assert(probe.calls == 1);

    memset(data_page + 12, 0x6b, sizeof(dword_t));
    cpu.x[8] = 96;
    cpu.x[0] = DATA_PAGE + 12;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 1234);

    cpu.x[8] = 93;
    cpu.x[0] = UINT64_C(0x1234);
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_EXIT);
    assert(result.exit_status == 0x34);
    for (size_t i = 0; i < sizeof(dword_t); i++)
        assert(data_page[12 + i] == 0x6b);

    task.clear_child_tid = UINT64_MAX;
    cpu.x[8] = 94;
    cpu.x[0] = 42;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_EXIT_GROUP);
    assert(result.exit_status == 42);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    guest_page_table_destroy(&table);
    return 0;
}

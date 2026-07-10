#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/linux/errno.h"
#include "guest/memory/page-table.h"

#define DATA_PAGE UINT64_C(0x000056789abcd000)

struct test_sink {
    byte_t data[64];
    size_t size;
    sqword_t next_result;
};

static sqword_t write_sink(void *opaque, qword_t fd,
        const byte_t *data, size_t size) {
    struct test_sink *sink = opaque;
    assert(fd == 1);
    if (sink->next_result != 0) {
        sqword_t result = sink->next_result;
        sink->next_result = 0;
        if (result > 0) {
            memcpy(sink->data + sink->size, data, (size_t) result);
            sink->size += (size_t) result;
        }
        return result;
    }
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    return (sqword_t) size;
}

static qword_t encoded_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    byte_t *data_page;
    assert(guest_page_table_map(&table, DATA_PAGE,
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, &data_page) ==
            GUEST_PAGE_TABLE_OK);
    static const byte_t message[] = "AArch64 runtime works";
    memcpy(data_page + 32, message, sizeof(message) - 1);
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    struct test_sink sink = {0};
    const struct aarch64_linux_services services = {
        .opaque = &sink,
        .write = write_sink,
    };
    struct cpu_state cpu = {0};
    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + 32;
    cpu.x[2] = sizeof(message) - 1;
    struct aarch64_linux_syscall_result result =
            aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(cpu.x[0] == sizeof(message) - 1);
    assert(sink.size == sizeof(message) - 1);
    assert(memcmp(sink.data, message, sink.size) == 0);

    memcpy(data_page + GUEST_MEMORY_PAGE_SIZE - 20, message, 20);
    sink = (struct test_sink) {0};
    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 20;
    cpu.x[2] = 40;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(cpu.x[0] == 16);
    assert(sink.size == 16);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == DATA_PAGE + GUEST_MEMORY_PAGE_SIZE);

    sink = (struct test_sink) {.next_result = 3};
    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + 32;
    cpu.x[2] = sizeof(message) - 1;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(cpu.x[0] == 3);
    assert(sink.size == 3);

    sink = (struct test_sink) {.next_result = -GUEST_LINUX_EIO};
    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + 32;
    cpu.x[2] = 4;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EIO));

    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE;
    cpu.x[2] = 1;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EFAULT));
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);

    cpu.x[8] = 999;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));

    cpu.x[8] = 64;
    cpu.x[0] = 1;
    cpu.x[1] = DATA_PAGE;
    cpu.x[2] = 1;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, NULL);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));

    cpu.x[8] = 93;
    cpu.x[0] = UINT64_C(0x1234);
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(result.action == AARCH64_LINUX_SYSCALL_EXIT);
    assert(result.exit_status == 0x34);

    cpu.x[8] = 94;
    cpu.x[0] = 42;
    result = aarch64_linux_dispatch_syscall(&cpu, &tlb, &services);
    assert(result.action == AARCH64_LINUX_SYSCALL_EXIT_GROUP);
    assert(result.exit_status == 42);
    guest_page_table_destroy(&table);
    return 0;
}

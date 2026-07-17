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

struct exec_probe {
    unsigned calls;
};

struct file_mapping_probe {
    void *expected_task;
    struct guest_linux_file_mapping_request request;
    qword_t last_page_offset;
    unsigned acquire_calls;
    unsigned handle_release_calls;
    unsigned open_calls;
    unsigned read_calls;
    unsigned release_calls;
    sdword_t acquire_error;
    sdword_t open_error;
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

static qword_t dispatch_exec(
        const struct guest_linux_syscall_context *context,
        const struct guest_linux_syscall *syscall,
        struct guest_linux_user_fault *fault) {
    use(fault);
    struct exec_probe *probe = context->runtime_opaque;
    assert(syscall->number == 400);
    assert(context->completion != NULL &&
            context->completion->disposition ==
                    GUEST_LINUX_SYSCALL_RETURN);
    context->completion->disposition =
            GUEST_LINUX_SYSCALL_REPLACED_IMAGE;
    probe->calls++;
    return UINT64_C(0xfeedfacecafebeef);
}

static enum guest_file_page_result read_mapping_page(void *opaque,
        qword_t file_offset, byte_t *page, dword_t *valid_bytes) {
    struct file_mapping_probe *probe = opaque;
    probe->read_calls++;
    probe->last_page_offset = file_offset;
    memset(page, 0x6d, GUEST_MEMORY_PAGE_SIZE);
    *valid_bytes = GUEST_MEMORY_PAGE_SIZE;
    return GUEST_FILE_PAGE_OK;
}

static void release_mapping_pager(struct guest_file_pager *pager,
        void *opaque) {
    use(pager);
    struct file_mapping_probe *probe = opaque;
    probe->release_calls++;
}

static sdword_t acquire_file_mapping_probe(
        const struct guest_linux_file_mapping_context *context,
        qword_t fd, struct guest_linux_file_mapping_handle *handle) {
    struct file_mapping_probe *probe = context->runtime_opaque;
    assert(context->task_opaque == probe->expected_task &&
            handle->opaque == NULL);
    probe->acquire_calls++;
    if (probe->acquire_error != 0)
        return probe->acquire_error;
    assert(fd == 7);
    handle->opaque = probe;
    return 0;
}

static void release_file_mapping_probe(
        struct guest_linux_file_mapping_handle *handle) {
    assert(handle != NULL && handle->opaque != NULL);
    struct file_mapping_probe *probe = handle->opaque;
    probe->handle_release_calls++;
    handle->opaque = NULL;
}

static sdword_t open_file_mapping_probe(
        const struct guest_linux_file_mapping_handle *handle,
        const struct guest_linux_file_mapping_request *request,
        struct guest_linux_file_mapping *mapping) {
    struct file_mapping_probe *probe = handle->opaque;
    assert(probe != NULL);
    probe->request = *request;
    probe->open_calls++;
    if (probe->open_error != 0)
        return probe->open_error;
    struct guest_file_pager *pager = guest_file_pager_create(
            UINT64_C(0xabcddcba),
            (struct guest_file_pager_provider) {
                .opaque = probe,
                .read_page = read_mapping_page,
                .release = release_mapping_pager,
            });
    assert(pager != NULL);
    *mapping = (struct guest_linux_file_mapping) {
        .pager = pager,
        .maximum_protection = GUEST_LINUX_PROT_READ |
                GUEST_LINUX_PROT_WRITE,
    };
    return 0;
}

static struct guest_linux_signal_poll_result count_signal_poll(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    use(installer, installer_opaque);
    unsigned *calls = context->runtime_opaque;
    (*calls)++;
    return (struct guest_linux_signal_poll_result) {
        .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
    };
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
    struct guest_linux_mm memory;
    struct aarch64_linux_runtime runtime;
    aarch64_linux_runtime_init(&runtime, &memory, &table,
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

    cpu.x[8] = 283;
    cpu.x[0] = UINT64_C(0xa5a5a5a500000000) |
            GUEST_LINUX_MEMBARRIER_CMD_QUERY;
    cpu.x[1] = UINT64_C(0x5a5a5a5a00000000);
    cpu.x[2] = UINT64_MAX;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == GUEST_LINUX_MEMBARRIER_SUPPORTED_COMMANDS);
    assert(sink.calls == 1);

    cpu.x[0] = GUEST_LINUX_MEMBARRIER_CMD_QUERY;
    cpu.x[1] = UINT64_C(0xa5a5a5a500000001);
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EINVAL));
    cpu.x[0] = GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED;
    cpu.x[1] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EPERM));
    cpu.x[0] = GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0);

    cpu.x[0] = UINT64_C(0x1234567800000000) |
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
    cpu.x[1] = UINT64_C(0x8765432100000000);
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0);
    cpu.x[0] = GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS;
    cpu.x[1] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] ==
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED);
    cpu.x[0] = GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0);
    cpu.x[0] = 1;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EINVAL));
    cpu.x[0] = GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED |
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EINVAL));
    assert(sink.calls == 1);

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
    guest_addr_t mapped = (guest_addr_t) runtime.memory->mmap_base;
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(cpu.x[0] == mapped);
    byte_t *mapped_page;
    unsigned mapped_permissions;
    assert(guest_page_table_lookup(&table, mapped,
            &mapped_page, &mapped_permissions) == GUEST_PAGE_TABLE_OK);
    assert(mapped_permissions ==
            (GUEST_MEMORY_READ | GUEST_MEMORY_WRITE));

    memset(mapped_page, 0xa5, GUEST_MEMORY_PAGE_SIZE);
    cpu.x[8] = 233;
    cpu.x[0] = mapped;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    cpu.x[2] = GUEST_LINUX_MADV_DONTNEED;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0 && mapped_page[0] == 0 &&
            mapped_page[GUEST_MEMORY_PAGE_SIZE - 1] == 0);
    assert(probe.calls == 1);

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

    cpu.x[0] = 0;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    cpu.x[2] = GUEST_LINUX_PROT_READ;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE;
    cpu.x[4] = 7;
    cpu.x[5] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOSYS));

    struct file_mapping_probe mapping_probe = {
        .expected_task = &host_task,
    };
    const struct guest_linux_file_mapping_service mapping_service = {
        .runtime_opaque = &mapping_probe,
        .acquire = acquire_file_mapping_probe,
        .release = release_file_mapping_probe,
        .open = open_file_mapping_probe,
    };
    const struct aarch64_linux_services file_services = {
        .syscalls = &syscall_service,
        .file_mappings = &mapping_service,
    };
    runtime.services = &file_services;

    cpu.x[5] = 1;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EINVAL) &&
            mapping_probe.acquire_calls == 0 &&
            mapping_probe.open_calls == 0 &&
            mapping_probe.handle_release_calls == 0);

    mapping_probe.acquire_error = -(sdword_t) GUEST_LINUX_EBADF;
    cpu.x[1] = 0;
    cpu.x[4] = 99;
    cpu.x[5] = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EBADF) &&
            mapping_probe.acquire_calls == 1 &&
            mapping_probe.open_calls == 0 &&
            mapping_probe.handle_release_calls == 0);

    mapping_probe.acquire_error = 0;
    cpu.x[4] = 7;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EINVAL) &&
            mapping_probe.acquire_calls == 2 &&
            mapping_probe.open_calls == 0 &&
            mapping_probe.handle_release_calls == 1);

    mapping_probe.open_error = -(sdword_t) GUEST_LINUX_EOVERFLOW;
    cpu.x[0] = DATA_PAGE;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_FIXED_NOREPLACE;
    cpu.x[5] = (qword_t) INT64_MAX & ~GUEST_MEMORY_PAGE_MASK;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EEXIST) &&
            mapping_probe.acquire_calls == 3 &&
            mapping_probe.open_calls == 0 &&
            mapping_probe.handle_release_calls == 2);

    unsigned acquire_calls = mapping_probe.acquire_calls;
    unsigned handle_release_calls = mapping_probe.handle_release_calls;
    unsigned open_calls = mapping_probe.open_calls;
    cpu.x[0] = DATA_PAGE;
    cpu.x[5] = UINT64_MAX & ~GUEST_MEMORY_PAGE_MASK;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EEXIST) &&
            mapping_probe.acquire_calls == acquire_calls + 1 &&
            mapping_probe.open_calls == open_calls &&
            mapping_probe.handle_release_calls ==
                    handle_release_calls + 1 &&
            mapping_probe.release_calls == 0);

    acquire_calls = mapping_probe.acquire_calls;
    handle_release_calls = mapping_probe.handle_release_calls;
    open_calls = mapping_probe.open_calls;
    cpu.x[0] = 0;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EOVERFLOW) &&
            mapping_probe.acquire_calls == acquire_calls + 1 &&
            mapping_probe.open_calls == open_calls + 1 &&
            mapping_probe.handle_release_calls ==
                    handle_release_calls + 1 &&
            mapping_probe.release_calls == 0);

    mapping_probe.open_error = -(sdword_t) GUEST_LINUX_EACCES;
    cpu.x[0] = UINT64_C(1) << 48;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED;
    cpu.x[5] = GUEST_MEMORY_PAGE_SIZE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_ENOMEM) &&
            mapping_probe.acquire_calls == 6 &&
            mapping_probe.open_calls == 1 &&
            mapping_probe.handle_release_calls == 5);

    cpu.x[0] = 0;
    cpu.x[3] = GUEST_LINUX_MAP_PRIVATE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == encoded_error(GUEST_LINUX_EACCES) &&
            mapping_probe.open_calls == 2 &&
            mapping_probe.acquire_calls == 7 &&
            mapping_probe.handle_release_calls == 6 &&
            mapping_probe.release_calls == 0);

    mapping_probe.open_error = 0;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    guest_addr_t file_mapped = (guest_addr_t) cpu.x[0];
    assert(result.action == AARCH64_LINUX_SYSCALL_RESUME &&
            result.fault.kind == GUEST_MEMORY_FAULT_NONE &&
            mapping_probe.open_calls == 3 &&
            mapping_probe.acquire_calls == 8 &&
            mapping_probe.handle_release_calls == 7 &&
            mapping_probe.request.fd == 7 &&
            mapping_probe.request.offset == GUEST_MEMORY_PAGE_SIZE &&
            mapping_probe.request.length == GUEST_MEMORY_PAGE_SIZE &&
            mapping_probe.request.protection == GUEST_LINUX_PROT_READ &&
            mapping_probe.request.flags == GUEST_LINUX_MAP_PRIVATE &&
            mapping_probe.read_calls == 0 &&
            mapping_probe.release_calls == 0);
    assert(guest_page_table_lookup(&table, file_mapped,
            &mapped_page, &mapped_permissions) ==
            GUEST_PAGE_TABLE_NOT_MAPPED);

    byte_t file_value = 0;
    struct guest_memory_fault file_fault;
    assert(guest_tlb_read(&tlb, file_mapped, &file_value,
            sizeof(file_value), GUEST_MEMORY_READ, &file_fault) &&
            file_value == 0x6d &&
            file_fault.kind == GUEST_MEMORY_FAULT_NONE &&
            mapping_probe.read_calls == 1 &&
            mapping_probe.last_page_offset == GUEST_MEMORY_PAGE_SIZE);

    cpu.x[8] = 215;
    cpu.x[0] = file_mapped;
    cpu.x[1] = GUEST_MEMORY_PAGE_SIZE;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(cpu.x[0] == 0 && mapping_probe.release_calls == 1 &&
            mapping_probe.handle_release_calls == 7);

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

    struct exec_probe exec_probe = {0};
    const struct guest_linux_syscall_service exec_service = {
        .runtime_opaque = &exec_probe,
        .dispatch = dispatch_exec,
    };
    unsigned exec_signal_polls = 0;
    const struct guest_linux_signal_service exec_signal_service = {
        .runtime_opaque = &exec_signal_polls,
        .poll = count_signal_poll,
    };
    const struct aarch64_linux_services exec_services = {
        .syscalls = &exec_service,
        .signals = &exec_signal_service,
    };
    runtime.services = &exec_services;
    cpu.x[0] = UINT64_C(0x1122334455667788);
    cpu.x[8] = 400;
    result = aarch64_linux_dispatch_syscall(
            &cpu, &tlb, &runtime, &task);
    assert(result.action == AARCH64_LINUX_SYSCALL_EXEC &&
            result.fault.kind == GUEST_MEMORY_FAULT_NONE &&
            cpu.x[0] == UINT64_C(0x1122334455667788) &&
            exec_probe.calls == 1 && exec_signal_polls == 0);

    guest_linux_mm_destroy(&memory);
    guest_page_table_destroy(&table);
    return 0;
}

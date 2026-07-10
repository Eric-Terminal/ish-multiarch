#include <assert.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/linux/errno.h"

#define AARCH64_LINUX_SYS_WRITE 64
#define AARCH64_LINUX_SYS_EXIT 93
#define AARCH64_LINUX_SYS_EXIT_GROUP 94
#define AARCH64_LINUX_SYS_BRK 214
#define AARCH64_LINUX_WRITE_CHUNK_SIZE 16

static qword_t linux_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static qword_t dispatch_write(const struct guest_linux_syscall *syscall,
        struct guest_tlb *tlb, const struct aarch64_linux_services *services,
        struct guest_memory_fault *fault) {
    if (services == NULL || services->write == NULL)
        return linux_error(GUEST_LINUX_ENOSYS);

    qword_t fd = syscall->arguments[0];
    guest_addr_t address = syscall->arguments[1];
    qword_t remaining = syscall->arguments[2];
    qword_t completed = 0;
    while (remaining != 0) {
        // write 的部分完成边界不能随底层 TLB 最大访问宽度改变。
        byte_t bytes[AARCH64_LINUX_WRITE_CHUNK_SIZE];
        size_t chunk = remaining < sizeof(bytes) ?
                (size_t) remaining : sizeof(bytes);
        if (!guest_tlb_read(tlb, address, bytes, chunk,
                GUEST_MEMORY_READ, fault))
            return completed != 0 ? completed : linux_error(GUEST_LINUX_EFAULT);
        sqword_t written = services->write(
                services->opaque, fd, bytes, chunk);
        if (written < 0)
            return completed != 0 ? completed : (qword_t) written;
        assert((qword_t) written <= chunk);
        completed += (qword_t) written;
        if ((size_t) written != chunk)
            return completed;
        address += (guest_addr_t) chunk;
        remaining -= chunk;
    }
    return completed;
}

void aarch64_linux_runtime_init(struct aarch64_linux_runtime *runtime,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit,
        const struct aarch64_linux_services *services) {
    aarch64_linux_mm_init(&runtime->memory, page_table,
            start_brk, brk_limit);
    runtime->services = services;
}

struct aarch64_linux_syscall_result aarch64_linux_dispatch_syscall(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        struct aarch64_linux_runtime *runtime) {
    assert(runtime != NULL);
    struct guest_linux_syscall syscall;
    aarch64_linux_read_syscall(cpu, &syscall);
    struct aarch64_linux_syscall_result result = {
        .action = AARCH64_LINUX_SYSCALL_RESUME,
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };

    if (syscall.number == AARCH64_LINUX_SYS_EXIT ||
            syscall.number == AARCH64_LINUX_SYS_EXIT_GROUP) {
        result.action = syscall.number == AARCH64_LINUX_SYS_EXIT ?
                AARCH64_LINUX_SYSCALL_EXIT :
                AARCH64_LINUX_SYSCALL_EXIT_GROUP;
        result.exit_status = (dword_t) syscall.arguments[0] & UINT32_C(0xff);
        return result;
    }

    if (syscall.number == AARCH64_LINUX_SYS_WRITE)
        result.return_value = dispatch_write(
                &syscall, tlb, runtime->services, &result.fault);
    else if (syscall.number == AARCH64_LINUX_SYS_BRK)
        result.return_value = aarch64_linux_brk(
                &runtime->memory, syscall.arguments[0]);
    else
        result.return_value = linux_error(GUEST_LINUX_ENOSYS);
    aarch64_linux_write_syscall_result(cpu, result.return_value);
    return result;
}

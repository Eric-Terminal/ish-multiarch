#include <assert.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/linux/errno.h"
#include "guest/linux/user-memory.h"

#define AARCH64_LINUX_SYS_WRITE 64
#define AARCH64_LINUX_SYS_EXIT 93
#define AARCH64_LINUX_SYS_EXIT_GROUP 94
#define AARCH64_LINUX_SYS_SET_TID_ADDRESS 96
#define AARCH64_LINUX_SYS_GETTID 178
#define AARCH64_LINUX_SYS_BRK 214
#define AARCH64_LINUX_SYS_MUNMAP 215
#define AARCH64_LINUX_SYS_MMAP 222
#define AARCH64_LINUX_SYS_MPROTECT 226
#define AARCH64_LINUX_WRITE_CHUNK_SIZE 16
#define AARCH64_LINUX_MAX_TID INT32_C(0x3fffffff)

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
        if (!guest_linux_copy_from_user(
                tlb, address, bytes, chunk, fault))
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

static void export_user_fault(struct guest_linux_user_fault *destination,
        const struct guest_memory_fault *source) {
    *destination = (struct guest_linux_user_fault) {
        .address = source->address,
        .access = (dword_t) source->access,
        .kind = (dword_t) source->kind,
    };
}

static bool service_read_user(void *opaque, qword_t address,
        void *destination, size_t size,
        struct guest_linux_user_fault *fault) {
    struct guest_memory_fault memory_fault = {0};
    bool copied = guest_linux_copy_from_user(opaque,
            address, destination, size, &memory_fault);
    export_user_fault(fault, &memory_fault);
    return copied;
}

static bool service_write_user(void *opaque, qword_t address,
        const void *source, size_t size,
        struct guest_linux_user_fault *fault) {
    struct guest_memory_fault memory_fault = {0};
    bool copied = guest_linux_copy_to_user(
            opaque, address, source, size, &memory_fault);
    export_user_fault(fault, &memory_fault);
    return copied;
}

static qword_t dispatch_service(const struct guest_linux_syscall *syscall,
        struct guest_tlb *tlb, const struct aarch64_linux_services *services,
        const struct aarch64_linux_task *task,
        struct guest_memory_fault *fault) {
    if (services == NULL || services->syscalls == NULL ||
            services->syscalls->dispatch == NULL)
        return linux_error(GUEST_LINUX_ENOSYS);
    const struct guest_linux_syscall_context context = {
        .runtime_opaque = services->syscalls->runtime_opaque,
        .task_opaque = task->service_opaque,
        .user = {
            .opaque = tlb,
            .read = service_read_user,
            .write = service_write_user,
        },
    };
    struct guest_linux_user_fault service_fault = {0};
    qword_t result = services->syscalls->dispatch(
            &context, syscall, &service_fault);
    *fault = (struct guest_memory_fault) {
        .address = (guest_addr_t) service_fault.address,
        .access = (enum guest_memory_access) service_fault.access,
        .kind = (enum guest_memory_fault_kind) service_fault.kind,
    };
    return result;
}

void aarch64_linux_runtime_init(struct aarch64_linux_runtime *runtime,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit,
        const struct aarch64_linux_services *services) {
    guest_linux_mm_init(&runtime->memory, page_table,
            start_brk, brk_limit);
    runtime->services = services;
}

void aarch64_linux_task_init(struct aarch64_linux_task *task, pid_t_ tid,
        void *service_opaque) {
    assert(tid > 0 && tid <= AARCH64_LINUX_MAX_TID);
    *task = (struct aarch64_linux_task) {
        .tid = tid,
        .service_opaque = service_opaque,
    };
}

struct aarch64_linux_syscall_result aarch64_linux_dispatch_syscall(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        struct aarch64_linux_runtime *runtime,
        struct aarch64_linux_task *task) {
    assert(runtime != NULL && task != NULL);
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
    else if (syscall.number == AARCH64_LINUX_SYS_SET_TID_ADDRESS) {
        task->clear_child_tid = syscall.arguments[0];
        result.return_value = (qword_t) task->tid;
    } else if (syscall.number == AARCH64_LINUX_SYS_GETTID) {
        result.return_value = (qword_t) task->tid;
    } else if (syscall.number == AARCH64_LINUX_SYS_BRK) {
        result.return_value = guest_linux_brk(
                &runtime->memory, syscall.arguments[0]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MUNMAP) {
        result.return_value = guest_linux_munmap(&runtime->memory,
                syscall.arguments[0], syscall.arguments[1]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MMAP) {
        result.return_value = guest_linux_mmap(&runtime->memory,
                syscall.arguments[0], syscall.arguments[1],
                syscall.arguments[2], syscall.arguments[3],
                syscall.arguments[4], syscall.arguments[5]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MPROTECT) {
        result.return_value = guest_linux_mprotect(&runtime->memory,
                syscall.arguments[0], syscall.arguments[1],
                syscall.arguments[2]);
    } else {
        result.return_value = dispatch_service(
                &syscall, tlb, runtime->services, task, &result.fault);
    }
    aarch64_linux_write_syscall_result(cpu, result.return_value);
    return result;
}

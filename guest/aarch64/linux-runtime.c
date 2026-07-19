#include <assert.h>

#include "guest/aarch64/linux-runtime.h"
#include "guest/aarch64/linux-signal-frame.h"
#include "guest/aarch64/linux-signal-info.h"
#include "guest/linux/errno.h"
#include "guest/linux/mman.h"
#include "guest/linux/user-memory.h"

#define AARCH64_LINUX_SYS_FCNTL 25
#define AARCH64_LINUX_SYS_IOCTL 29
#define AARCH64_LINUX_SYS_OPENAT 56
#define AARCH64_LINUX_SYS_READ 63
#define AARCH64_LINUX_SYS_WRITE 64
#define AARCH64_LINUX_SYS_READV 65
#define AARCH64_LINUX_SYS_WRITEV 66
#define AARCH64_LINUX_SYS_PREAD64 67
#define AARCH64_LINUX_SYS_PWRITE64 68
#define AARCH64_LINUX_SYS_PREADV 69
#define AARCH64_LINUX_SYS_PWRITEV 70
#define AARCH64_LINUX_SYS_EXIT 93
#define AARCH64_LINUX_SYS_EXIT_GROUP 94
#define AARCH64_LINUX_SYS_SET_TID_ADDRESS 96
#define AARCH64_LINUX_SYS_FUTEX 98
#define AARCH64_LINUX_SYS_RT_SIGRETURN 139
#define AARCH64_LINUX_SYS_GETTID 178
#define AARCH64_LINUX_SYS_CONNECT 203
#define AARCH64_LINUX_SYS_SENDTO 206
#define AARCH64_LINUX_SYS_RECVFROM 207
#define AARCH64_LINUX_SYS_BRK 214
#define AARCH64_LINUX_SYS_MUNMAP 215
#define AARCH64_LINUX_SYS_MREMAP 216
#define AARCH64_LINUX_SYS_MMAP 222
#define AARCH64_LINUX_SYS_MPROTECT 226
#define AARCH64_LINUX_SYS_MSYNC 227
#define AARCH64_LINUX_SYS_MADVISE 233
#define AARCH64_LINUX_SYS_WAIT4 260
#define AARCH64_LINUX_SYS_MEMBARRIER 283
#define AARCH64_LINUX_SYS_PREADV2 286
#define AARCH64_LINUX_SYS_PWRITEV2 287
#define AARCH64_LINUX_SYS_FUTEX_WAITV 449
static qword_t linux_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static qword_t untag_user_address(qword_t address) {
    // AArch64 Linux 只对允许 tagged-address ABI 的用户指针移除顶字节标签。
    return (address & (UINT64_C(1) << 55)) != 0 ? address :
            address & UINT64_C(0x00ffffffffffffff);
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
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct guest_memory_fault memory_fault = {0};
    bool copied = guest_linux_copy_from_user(opaque,
            address, destination, size, &memory_fault);
    export_user_fault(fault, &memory_fault);
    return copied;
}

static bool service_write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct guest_memory_fault memory_fault = {0};
    bool copied = guest_linux_copy_to_user(
            opaque, address, source, size, &memory_fault);
    export_user_fault(fault, &memory_fault);
    return copied;
}

struct signal_install_state {
    const struct cpu_state *interrupted;
    const struct cpu_state *restart;
    struct guest_tlb *tlb;
    struct cpu_state candidate;
    guest_addr_t signal_trampoline;
    bool candidate_valid;
    bool installer_called;
};

static bool checked_altstack_top(
        const struct guest_linux_signal_stack *stack,
        guest_addr_t *top) {
    if (stack->size == 0) {
        *top = stack->base;
        return true;
    }
    if (stack->base > AARCH64_LINUX_USER_ADDRESS_MAX ||
            stack->size > AARCH64_LINUX_USER_ADDRESS_MAX - stack->base)
        return false;
    *top = stack->base + stack->size;
    return true;
}

static enum guest_linux_signal_install_status install_signal_frame(
        void *opaque,
        const struct guest_linux_signal_delivery *delivery) {
    struct signal_install_state *state = opaque;
    state->candidate_valid = false;

    // 只有导致系统调用中断的第一个 handler 可以决定是否重启；若其
    // 帧安装失败，随后强制投递的同步故障不得重新执行原系统调用。
    const struct cpu_state *interrupted = state->interrupted;
    if (!state->installer_called && state->restart != NULL &&
            (delivery->action.flags & AARCH64_LINUX_SA_RESTART) != 0)
        interrupted = state->restart;
    state->installer_called = true;

    guest_addr_t altstack_top;
    if (!checked_altstack_top(&delivery->altstack, &altstack_top))
        return GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;

    guest_addr_t sp = interrupted->sp;
    bool altstack_configured = delivery->altstack.size != 0;
    bool on_altstack = altstack_configured &&
            sp > delivery->altstack.base &&
            sp - delivery->altstack.base <= delivery->altstack.size;
    bool enter_altstack = altstack_configured && !on_altstack &&
            (delivery->action.flags & AARCH64_LINUX_SA_ONSTACK);
    qword_t restorer = delivery->action.restorer;
    if (!(delivery->action.flags & AARCH64_LINUX_SA_RESTORER)) {
        if (state->signal_trampoline == 0)
            return GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;
        restorer = state->signal_trampoline;
    }
    struct aarch64_linux_signal_delivery wire = {
        .signal = delivery->info.signal,
        .info = aarch64_linux_pack_siginfo(&delivery->info),
        .handler = delivery->action.handler,
        .restorer = restorer,
        .action_flags = delivery->action.flags,
        .blocked_mask = delivery->blocked_mask,
        .altstack = {
            .sp = delivery->altstack.base,
            .flags = (sdword_t) delivery->altstack.flags,
            .size = delivery->altstack.size,
        },
        .stack_top = enter_altstack ? altstack_top : sp,
        .stack_bottom = on_altstack || enter_altstack ?
                delivery->altstack.base : 0,
        .fault_address = interrupted->segfault_addr,
    };
    struct cpu_state candidate;
    guest_addr_t frame_address;
    struct guest_memory_fault fault;
    enum aarch64_linux_signal_frame_status status =
            aarch64_linux_build_rt_sigframe(interrupted,
                    state->tlb, &wire, &candidate,
                    &frame_address, &fault);
    if (status != AARCH64_LINUX_SIGNAL_FRAME_OK)
        return GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;
    state->candidate = candidate;
    state->candidate_valid = true;
    return GUEST_LINUX_SIGNAL_INSTALL_COMPLETE;
}

struct service_dispatch_result {
    qword_t return_value;
    bool replaced_image;
};

static struct service_dispatch_result dispatch_service(
        const struct guest_linux_syscall *syscall,
        struct guest_tlb *tlb, const struct aarch64_linux_services *services,
        const struct aarch64_linux_task *task,
        guest_addr_t stack_pointer,
        struct guest_memory_fault *fault) {
    if (services == NULL || services->syscalls == NULL ||
            services->syscalls->dispatch == NULL) {
        return (struct service_dispatch_result) {
            .return_value = linux_error(GUEST_LINUX_ENOSYS),
        };
    }
    struct guest_linux_syscall_completion completion = {
        .disposition = GUEST_LINUX_SYSCALL_RETURN,
    };
    const struct guest_linux_syscall_context context = {
        .runtime_opaque = services->syscalls->runtime_opaque,
        .task_opaque = task->service_opaque,
        .stack_pointer = stack_pointer,
        .completion = &completion,
        .user = {
            .opaque = tlb,
            .read = service_read_user,
            .write = service_write_user,
        },
    };
    struct guest_linux_user_fault service_fault = {0};
    qword_t result = services->syscalls->dispatch(
            &context, syscall, &service_fault);
    assert(completion.disposition <=
            GUEST_LINUX_SYSCALL_REPLACED_IMAGE);
    *fault = (struct guest_memory_fault) {
        .address = (guest_addr_t) service_fault.address,
        .access = (enum guest_memory_access) service_fault.access,
        .kind = (enum guest_memory_fault_kind) service_fault.kind,
    };
    return (struct service_dispatch_result) {
        .return_value = result,
        .replaced_image = completion.disposition ==
                GUEST_LINUX_SYSCALL_REPLACED_IMAGE,
    };
}

static const struct guest_linux_signal_service *runtime_signal_service(
        const struct aarch64_linux_runtime *runtime) {
    return runtime->services == NULL ? NULL : runtime->services->signals;
}

static qword_t dispatch_mmap(
        struct aarch64_linux_runtime *runtime,
        const struct aarch64_linux_task *task,
        const struct guest_linux_syscall *syscall) {
    qword_t flags = syscall->arguments[3];
    if ((flags & GUEST_LINUX_MAP_ANONYMOUS) != 0) {
        return guest_linux_mmap(runtime->memory,
                syscall->arguments[0], syscall->arguments[1],
                syscall->arguments[2], flags,
                syscall->arguments[4], syscall->arguments[5]);
    }
    // arm64 mmap 的 byte offset 对齐检查先于 fd 获取。
    if ((syscall->arguments[5] & GUEST_MEMORY_PAGE_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);
    const struct guest_linux_file_mapping_service *service =
            runtime->services == NULL ? NULL :
                    runtime->services->file_mappings;
    if (service == NULL || service->acquire == NULL ||
            service->release == NULL || service->open == NULL)
        return linux_error(GUEST_LINUX_ENOSYS);

    const struct guest_linux_file_mapping_context context = {
        .runtime_opaque = service->runtime_opaque,
        .task_opaque = task->service_opaque,
    };
    const struct guest_linux_file_mapping_request request = {
        .fd = syscall->arguments[4],
        .offset = syscall->arguments[5],
        .length = syscall->arguments[1],
        .protection = syscall->arguments[2],
        .flags = flags,
    };
    struct guest_linux_file_mapping_handle handle = {0};
    sdword_t acquired = service->acquire(
            &context, request.fd, &handle);
    if (acquired != 0) {
        assert(acquired < 0 && handle.opaque == NULL);
        return (qword_t) (sqword_t) acquired;
    }
    assert(handle.opaque != NULL);

    // 普通文件 provider 不支持 hugetlb；Linux 在 fget 后、选址前拒绝。
    if ((flags & GUEST_LINUX_MAP_HUGETLB) != 0) {
        service->release(&handle);
        assert(handle.opaque == NULL);
        return linux_error(GUEST_LINUX_EINVAL);
    }

    qword_t preflight = guest_linux_mmap_file_preflight(
            runtime->memory, syscall->arguments[0], request.length,
            request.protection, request.flags, request.offset);
    if ((sqword_t) preflight < 0) {
        service->release(&handle);
        assert(handle.opaque == NULL);
        return preflight;
    }

    struct guest_linux_file_mapping mapping = {0};
    sdword_t opened = service->open(&handle, &request, &mapping);
    if (opened != 0) {
        assert(opened < 0 && mapping.pager == NULL);
        service->release(&handle);
        assert(handle.opaque == NULL);
        return (qword_t) (sqword_t) opened;
    }
    assert(mapping.pager != NULL &&
            (mapping.maximum_protection & ~GUEST_LINUX_PROT_MASK) == 0);
    qword_t mapping_type = flags & GUEST_LINUX_MAP_TYPE;
    bool shared = mapping_type == GUEST_LINUX_MAP_SHARED ||
            mapping_type == GUEST_LINUX_MAP_SHARED_VALIDATE;
    assert(shared || mapping_type == GUEST_LINUX_MAP_PRIVATE);
    qword_t normalized_flags = (shared ? GUEST_LINUX_MAP_SHARED :
            GUEST_LINUX_MAP_PRIVATE) |
            (flags & (GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_FIXED_NOREPLACE));
    qword_t result = shared ?
            guest_linux_mmap_file_shared(runtime->memory,
                    syscall->arguments[0], syscall->arguments[1],
                    syscall->arguments[2], mapping.maximum_protection,
                    normalized_flags, mapping.pager,
                    syscall->arguments[5]) :
            guest_linux_mmap_file_private(runtime->memory,
                    syscall->arguments[0], syscall->arguments[1],
                    syscall->arguments[2], mapping.maximum_protection,
                    normalized_flags, mapping.pager,
                    syscall->arguments[5]);
    guest_file_pager_release(mapping.pager);
    service->release(&handle);
    assert(handle.opaque == NULL);
    return result;
}

static struct guest_linux_signal_context make_signal_context(
        const struct guest_linux_signal_service *service,
        const struct aarch64_linux_task *task) {
    return (struct guest_linux_signal_context) {
        .runtime_opaque = service->runtime_opaque,
        .task_opaque = task->service_opaque,
    };
}

static void apply_signal_result(
        struct aarch64_linux_syscall_result *syscall,
        struct guest_linux_signal_poll_result signal) {
    syscall->signal = signal.signal;
    if (signal.status == GUEST_LINUX_SIGNAL_POLL_STOP)
        syscall->action = AARCH64_LINUX_SYSCALL_STOP;
    else if (signal.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE)
        syscall->action = AARCH64_LINUX_SYSCALL_TERMINATE;
}

void aarch64_linux_runtime_init(struct aarch64_linux_runtime *runtime,
        struct guest_linux_mm *memory, struct guest_page_table *page_table,
        guest_addr_t start_brk, guest_addr_t brk_limit,
        const struct aarch64_linux_services *services) {
    assert(memory != NULL);
    guest_linux_mm_init(memory, page_table, start_brk, brk_limit);
    runtime->memory = memory;
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

static struct guest_linux_signal_poll_result poll_signals_with_restart(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_linux_runtime *runtime,
        const struct aarch64_linux_task *task,
        const struct cpu_state *restart) {
    assert(cpu != NULL && tlb != NULL && runtime != NULL && task != NULL);
    const struct guest_linux_signal_service *service =
            runtime_signal_service(runtime);
    if (service == NULL || service->poll == NULL) {
        return (struct guest_linux_signal_poll_result) {
            .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
        };
    }

    const struct cpu_state interrupted = *cpu;
    struct signal_install_state install = {
        .interrupted = &interrupted,
        .restart = restart,
        .tlb = tlb,
        .signal_trampoline = runtime->services->signal_trampoline,
    };
    const struct guest_linux_signal_context context =
            make_signal_context(service, task);
    struct guest_linux_signal_poll_result result = service->poll(
            &context, install_signal_frame, &install);
    assert(result.status <= GUEST_LINUX_SIGNAL_POLL_TERMINATE);
    if (result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER) {
        assert(install.candidate_valid);
        *cpu = install.candidate;
    }
    return result;
}

struct guest_linux_signal_poll_result aarch64_linux_poll_signals(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_linux_runtime *runtime,
        const struct aarch64_linux_task *task) {
    return poll_signals_with_restart(
            cpu, tlb, runtime, task, NULL);
}

static bool syscall_may_restart(qword_t number) {
    switch (number) {
        case AARCH64_LINUX_SYS_FCNTL:
        case AARCH64_LINUX_SYS_IOCTL:
        case AARCH64_LINUX_SYS_OPENAT:
        case AARCH64_LINUX_SYS_READ:
        case AARCH64_LINUX_SYS_WRITE:
        case AARCH64_LINUX_SYS_READV:
        case AARCH64_LINUX_SYS_WRITEV:
        case AARCH64_LINUX_SYS_PREAD64:
        case AARCH64_LINUX_SYS_PWRITE64:
        case AARCH64_LINUX_SYS_PREADV:
        case AARCH64_LINUX_SYS_PWRITEV:
        case AARCH64_LINUX_SYS_FUTEX:
        case AARCH64_LINUX_SYS_FUTEX_WAITV:
        case AARCH64_LINUX_SYS_CONNECT:
        case AARCH64_LINUX_SYS_SENDTO:
        case AARCH64_LINUX_SYS_RECVFROM:
        case AARCH64_LINUX_SYS_WAIT4:
        case AARCH64_LINUX_SYS_PREADV2:
        case AARCH64_LINUX_SYS_PWRITEV2:
            return true;
        default:
            return false;
    }
}

static struct aarch64_linux_syscall_result dispatch_rt_sigreturn(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_linux_runtime *runtime,
        const struct aarch64_linux_task *task,
        const struct guest_linux_signal_service *service) {
    struct aarch64_linux_syscall_result result = {
        .action = AARCH64_LINUX_SYSCALL_RESUME,
        .return_value = cpu->x[0],
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };
    guest_addr_t frame_address = cpu->sp;
    struct aarch64_linux_signal_resume resume;
    enum aarch64_linux_signal_frame_status status =
            aarch64_linux_decode_rt_sigreturn(
                    cpu, tlb, &resume, &result.fault);
    const struct guest_linux_signal_context context =
            make_signal_context(service, task);
    if (status != AARCH64_LINUX_SIGNAL_FRAME_OK) {
        service->bad_frame(&context, frame_address);
        // Linux 的坏帧分支返回 0；强制 SIGSEGV 的上下文应观察该返回值。
        result.return_value = 0;
        aarch64_linux_write_syscall_result(cpu, result.return_value);
        struct guest_linux_signal_poll_result signal =
                aarch64_linux_poll_signals(
                        cpu, tlb, runtime, task);
        apply_signal_result(&result, signal);
        return result;
    }

    const struct guest_linux_signal_restore_request request = {
        .stack_pointer = resume.cpu.sp,
        .blocked_mask = resume.blocked_mask,
        .altstack = {
            .base = resume.altstack.sp,
            .size = resume.altstack.size,
            .flags = (dword_t) resume.altstack.flags,
        },
    };
    service->restore(&context, &request);
    *cpu = resume.cpu;
    result.return_value = cpu->x[0];
    apply_signal_result(&result,
            aarch64_linux_poll_signals(cpu, tlb, runtime, task));
    return result;
}

struct aarch64_linux_syscall_result aarch64_linux_dispatch_syscall(
        struct cpu_state *cpu, struct guest_tlb *tlb,
        struct aarch64_linux_runtime *runtime,
        struct aarch64_linux_task *task) {
    assert(runtime != NULL && task != NULL);
    struct guest_linux_syscall syscall;
    aarch64_linux_read_syscall(cpu, &syscall);
    struct cpu_state restart = *cpu;
    struct aarch64_linux_syscall_result result = {
        .action = AARCH64_LINUX_SYSCALL_RESUME,
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };

    const struct guest_linux_signal_service *signal_service =
            runtime_signal_service(runtime);
    bool can_restore_signal = signal_service != NULL &&
            signal_service->poll != NULL &&
            signal_service->restore != NULL &&
            signal_service->bad_frame != NULL;
    if (syscall.number == AARCH64_LINUX_SYS_RT_SIGRETURN &&
            can_restore_signal) {
        return dispatch_rt_sigreturn(
                cpu, tlb, runtime, task, signal_service);
    }

    if (syscall.number == AARCH64_LINUX_SYS_EXIT ||
            syscall.number == AARCH64_LINUX_SYS_EXIT_GROUP) {
        result.action = syscall.number == AARCH64_LINUX_SYS_EXIT ?
                AARCH64_LINUX_SYSCALL_EXIT :
                AARCH64_LINUX_SYSCALL_EXIT_GROUP;
        result.exit_status = (dword_t) syscall.arguments[0] & UINT32_C(0xff);
        return result;
    }

    if (syscall.number == AARCH64_LINUX_SYS_RT_SIGRETURN) {
        result.return_value = linux_error(GUEST_LINUX_ENOSYS);
    } else if (syscall.number == AARCH64_LINUX_SYS_SET_TID_ADDRESS) {
        task->clear_child_tid = syscall.arguments[0];
        result.return_value = (qword_t) task->tid;
    } else if (syscall.number == AARCH64_LINUX_SYS_GETTID) {
        result.return_value = (qword_t) task->tid;
    } else if (syscall.number == AARCH64_LINUX_SYS_BRK) {
        result.return_value = guest_linux_brk(
                runtime->memory, syscall.arguments[0]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MUNMAP) {
        result.return_value = guest_linux_munmap(runtime->memory,
                syscall.arguments[0], syscall.arguments[1]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MREMAP) {
        result.return_value = guest_linux_mremap(runtime->memory,
                untag_user_address(syscall.arguments[0]),
                syscall.arguments[1], syscall.arguments[2],
                syscall.arguments[3], syscall.arguments[4]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MMAP) {
        result.return_value = dispatch_mmap(runtime, task, &syscall);
    } else if (syscall.number == AARCH64_LINUX_SYS_MPROTECT) {
        result.return_value = guest_linux_mprotect(runtime->memory,
                syscall.arguments[0], syscall.arguments[1],
                syscall.arguments[2]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MSYNC) {
        result.return_value = guest_linux_msync(runtime->memory,
                untag_user_address(syscall.arguments[0]),
                syscall.arguments[1],
                (dword_t) syscall.arguments[2]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MADVISE) {
        result.return_value = guest_linux_madvise(runtime->memory,
                syscall.arguments[0], syscall.arguments[1],
                (dword_t) syscall.arguments[2]);
    } else if (syscall.number == AARCH64_LINUX_SYS_MEMBARRIER) {
        result.return_value = guest_linux_membarrier(runtime->memory,
                (sdword_t) (dword_t) syscall.arguments[0],
                (dword_t) syscall.arguments[1]);
    } else {
        struct service_dispatch_result service = dispatch_service(
                &syscall, tlb, runtime->services, task,
                cpu->sp, &result.fault);
        if (service.replaced_image) {
            result.action = AARCH64_LINUX_SYSCALL_EXEC;
            return result;
        }
        result.return_value = service.return_value;
    }
    aarch64_linux_write_syscall_result(cpu, result.return_value);
    const struct cpu_state *restart_state = NULL;
    if (result.return_value == linux_error(GUEST_LINUX_EINTR) &&
            syscall_may_restart(syscall.number)) {
        assert(restart.pc >= 4);
        restart.pc -= 4;
        restart_state = &restart;
    }
    apply_signal_result(&result,
            poll_signals_with_restart(
                    cpu, tlb, runtime, task, restart_state));
    return result;
}

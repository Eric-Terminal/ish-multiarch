#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "guest/aarch64/elf64-loader.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-runtime.h"
#include "guest/aarch64/linux-signal-trampoline.h"
#include "guest/aarch64/linux-stack.h"
#include "guest/aarch64/runner.h"

#define AARCH64_LINUX_PROCESS_ADDRESS_BITS 48
#define AARCH64_LINUX_PROCESS_ADDRESS_LIMIT \
    (UINT64_C(1) << AARCH64_LINUX_PROCESS_ADDRESS_BITS)
#define ELF_PT_LOAD 1

_Static_assert(AARCH64_LINUX_PROCESS_RANDOM_SIZE ==
                AARCH64_LINUX_RANDOM_SIZE,
        "process 与初始栈的随机字节长度必须一致");

struct aarch64_linux_process_memory {
    atomic_uint references;
    struct guest_page_table page_table;
    struct guest_linux_mm linux_mm;
};

struct aarch64_linux_process {
    struct aarch64_linux_process_memory *memory;
    struct guest_tlb tlb;
    struct aarch64_runner runner;
    struct cpu_state cpu;
    struct guest_linux_syscall_service syscall_service;
    struct guest_linux_signal_service signal_service;
    struct aarch64_linux_services services;
    struct aarch64_linux_runtime runtime;
    struct aarch64_linux_task task;
};

static struct aarch64_linux_process_memory *process_memory_create(void) {
    struct aarch64_linux_process_memory *memory =
            calloc(1, sizeof(*memory));
    if (memory == NULL)
        return NULL;
    if (!guest_page_table_init(&memory->page_table,
            AARCH64_LINUX_PROCESS_ADDRESS_BITS)) {
        free(memory);
        return NULL;
    }
    atomic_init(&memory->references, 1);
    return memory;
}

static struct aarch64_linux_process_memory *process_memory_clone(
        const struct aarch64_linux_process_memory *source) {
    struct aarch64_linux_process_memory *copy =
            calloc(1, sizeof(*copy));
    if (copy == NULL)
        return NULL;
    if (!guest_linux_mm_clone(&copy->linux_mm,
            &copy->page_table, &source->linux_mm)) {
        free(copy);
        return NULL;
    }
    atomic_init(&copy->references, 1);
    return copy;
}

static void process_memory_retain(
        struct aarch64_linux_process_memory *memory) {
    // 锁必须先启用，再把第二个可运行线程发布给调度器。
    guest_page_table_enable_concurrency(&memory->page_table);
    unsigned previous = atomic_fetch_add_explicit(
            &memory->references, 1, memory_order_relaxed);
    assert(previous != 0 && previous != UINT_MAX);
}

static void process_memory_release(
        struct aarch64_linux_process_memory *memory) {
    if (memory == NULL || atomic_fetch_sub_explicit(
            &memory->references, 1, memory_order_acq_rel) != 1)
        return;
    guest_page_table_destroy(&memory->page_table);
    free(memory);
}

static void set_error(struct aarch64_linux_process_error *error,
        enum aarch64_linux_process_error_stage stage, dword_t detail) {
    if (error != NULL) {
        *error = (struct aarch64_linux_process_error) {
            .stage = (dword_t) stage,
            .detail = detail,
        };
    }
}

static struct aarch64_linux_interpreter_path_result interpreter_path_result(
        enum aarch64_linux_interpreter_path_status status) {
    return (struct aarch64_linux_interpreter_path_result) {
        .status = (dword_t) status,
    };
}

struct aarch64_linux_executable_info
        aarch64_linux_inspect_executable(
        const void *elf_data, size_t elf_size) {
    if (elf_data == NULL && elf_size != 0) {
        return (struct aarch64_linux_executable_info) {
            .status = AARCH64_LINUX_EXECUTABLE_BAD_ELF,
            .elf_error = AARCH64_ELF64_BAD_IDENTIFICATION,
        };
    }
    struct aarch64_elf64_image image;
    enum aarch64_elf64_error error = aarch64_elf64_parse(
            elf_data, elf_size, &image);
    if (error != AARCH64_ELF64_OK) {
        return (struct aarch64_linux_executable_info) {
            .status = AARCH64_LINUX_EXECUTABLE_BAD_ELF,
            .elf_error = (dword_t) error,
        };
    }
    return (struct aarch64_linux_executable_info) {
        .status = AARCH64_LINUX_EXECUTABLE_VALID,
        .position_independent = image.position_independent ? 1 : 0,
    };
}

struct aarch64_linux_interpreter_path_result
        aarch64_linux_copy_interpreter_path(
        const void *elf_data, size_t elf_size,
        char *destination, size_t capacity) {
    if (elf_data == NULL && elf_size != 0) {
        struct aarch64_linux_interpreter_path_result result =
                interpreter_path_result(
                        AARCH64_LINUX_INTERPRETER_PATH_BAD_ELF);
        result.elf_error = AARCH64_ELF64_BAD_IDENTIFICATION;
        return result;
    }
    struct aarch64_elf64_image image;
    enum aarch64_elf64_error elf_error = aarch64_elf64_parse(
            elf_data, elf_size, &image);
    if (elf_error != AARCH64_ELF64_OK) {
        struct aarch64_linux_interpreter_path_result result =
                interpreter_path_result(
                        AARCH64_LINUX_INTERPRETER_PATH_BAD_ELF);
        result.elf_error = (dword_t) elf_error;
        return result;
    }
    if (image.interpreter_path == NULL)
        return interpreter_path_result(
                AARCH64_LINUX_INTERPRETER_PATH_NONE);

    qword_t required_size = (qword_t) image.interpreter_path_length + 1;
    if (destination == NULL || capacity < required_size) {
        struct aarch64_linux_interpreter_path_result result =
                interpreter_path_result(
                        AARCH64_LINUX_INTERPRETER_PATH_BUFFER_TOO_SMALL);
        result.required_size = required_size;
        return result;
    }
    memcpy(destination, image.interpreter_path, (size_t) required_size);
    struct aarch64_linux_interpreter_path_result result =
            interpreter_path_result(
                    AARCH64_LINUX_INTERPRETER_PATH_COPIED);
    result.required_size = required_size;
    return result;
}

static struct aarch64_linux_process *create_failed(
        struct aarch64_linux_process *process,
        struct aarch64_linux_process_error *error,
        enum aarch64_linux_process_error_stage stage, dword_t detail) {
    aarch64_linux_process_destroy(process);
    set_error(error, stage, detail);
    return NULL;
}

static bool page_aligned(qword_t value) {
    return (value & GUEST_MEMORY_PAGE_MASK) == 0;
}

static bool ranges_overlap(qword_t first_start, qword_t first_end,
        qword_t second_start, qword_t second_end) {
    if (first_start >= first_end || second_start >= second_end)
        return false;
    return first_start < second_end && second_start < first_end;
}

static bool valid_config(
        const struct aarch64_linux_process_config *config) {
    if (config == NULL || config->elf_data == NULL ||
            config->elf_size == 0 || config->executable == NULL ||
            config->random == NULL || config->tid <= 0 ||
            config->tid > AARCH64_LINUX_MAX_TID ||
            (config->argument_count != 0 && config->arguments == NULL) ||
            (config->environment_count != 0 &&
                    config->environment == NULL) ||
            config->stack_size == 0 || config->stack_size > SIZE_MAX ||
            !page_aligned(config->stack_top) ||
            !page_aligned(config->stack_size) ||
            config->stack_size > config->stack_top ||
            config->stack_top > AARCH64_LINUX_PROCESS_ADDRESS_LIMIT ||
            config->signal_trampoline_page == 0 ||
            !page_aligned(config->signal_trampoline_page) ||
            config->signal_trampoline_page >=
                    AARCH64_LINUX_PROCESS_ADDRESS_LIMIT ||
            !page_aligned(config->brk_limit) ||
            config->brk_limit >= AARCH64_LINUX_PROCESS_ADDRESS_LIMIT)
        return false;
    for (size_t i = 0; i < config->argument_count; i++) {
        if (config->arguments[i] == NULL)
            return false;
    }
    for (size_t i = 0; i < config->environment_count; i++) {
        if (config->environment[i] == NULL)
            return false;
    }
    return true;
}

static bool loaded_image_overlaps_range(
        const struct aarch64_elf64_image *image,
        guest_addr_t load_bias, qword_t range_start,
        qword_t range_end) {
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (segment.type != ELF_PT_LOAD || segment.memory_size == 0)
            continue;
        assert(segment.virtual_address <= UINT64_MAX - load_bias);
        qword_t address = segment.virtual_address + load_bias;
        assert(segment.memory_size <=
                AARCH64_LINUX_PROCESS_ADDRESS_LIMIT - address);
        qword_t segment_start = address & ~GUEST_MEMORY_PAGE_MASK;
        qword_t segment_end = (address + segment.memory_size +
                GUEST_MEMORY_PAGE_MASK) & ~GUEST_MEMORY_PAGE_MASK;
        if (ranges_overlap(range_start, range_end,
                segment_start, segment_end))
            return true;
    }
    return false;
}

static void copy_services(struct aarch64_linux_process *process,
        const struct aarch64_linux_process_config *config,
        guest_addr_t signal_trampoline) {
    if (config->syscalls != NULL) {
        process->syscall_service = *config->syscalls;
        process->services.syscalls = &process->syscall_service;
    }
    if (config->signals != NULL) {
        process->signal_service = *config->signals;
        process->services.signals = &process->signal_service;
    }
    process->services.signal_trampoline = signal_trampoline;
}

static void copy_fork_services(struct aarch64_linux_process *child,
        const struct aarch64_linux_process *parent) {
    if (parent->services.syscalls != NULL) {
        child->syscall_service = *parent->services.syscalls;
        child->services.syscalls = &child->syscall_service;
    }
    if (parent->services.signals != NULL) {
        child->signal_service = *parent->services.signals;
        child->services.signals = &child->signal_service;
    }
    child->services.signal_trampoline =
            parent->services.signal_trampoline;
}

static void copy_fork_cpu(struct cpu_state *child,
        const struct cpu_state *parent) {
    memcpy(child->x, parent->x, sizeof(child->x));
    memcpy(child->v, parent->v, sizeof(child->v));
    child->sp = parent->sp;
    child->pc = parent->pc;
    child->nzcv = parent->nzcv;
    child->fpcr = parent->fpcr;
    child->fpsr = parent->fpsr;
    child->tpidr_el0 = parent->tpidr_el0;
    child->x[0] = 0;
    aarch64_clear_exclusive(child);
}

static void apply_clone_state(struct aarch64_linux_process *child,
        qword_t stack_pointer, dword_t set_tls, qword_t tls,
        qword_t clear_child_tid) {
    if (stack_pointer != 0)
        child->cpu.sp = (guest_addr_t) stack_pointer;
    if (set_tls != 0)
        child->cpu.tpidr_el0 = tls;
    child->task.clear_child_tid =
            (guest_addr_t) clear_child_tid;
}

struct aarch64_linux_process *aarch64_linux_process_create(
        const struct aarch64_linux_process_config *config,
        struct aarch64_linux_process_error *error) {
    return aarch64_linux_process_create_with_interpreter(
            config, NULL, error);
}

struct aarch64_linux_process *aarch64_linux_process_create_with_interpreter(
        const struct aarch64_linux_process_config *config,
        const struct aarch64_linux_interpreter_image *interpreter,
        struct aarch64_linux_process_error *error) {
    set_error(error, AARCH64_LINUX_PROCESS_ERROR_NONE, 0);
    if (!valid_config(config)) {
        set_error(error, AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
        return NULL;
    }

    struct aarch64_elf64_image main_image;
    enum aarch64_elf64_error elf_error = aarch64_elf64_parse(
            config->elf_data, config->elf_size, &main_image);
    if (elf_error != AARCH64_ELF64_OK)
        return create_failed(NULL, error,
                AARCH64_LINUX_PROCESS_ERROR_ELF,
                (dword_t) elf_error);
    bool needs_interpreter = main_image.interpreter_path != NULL;
    if (needs_interpreter && interpreter == NULL)
        return create_failed(NULL, error,
                AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
                AARCH64_LINUX_INTERPRETER_CONFIG_REQUIRED);
    if (!needs_interpreter && interpreter != NULL)
        return create_failed(NULL, error,
                AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
                AARCH64_LINUX_INTERPRETER_CONFIG_UNEXPECTED);

    struct aarch64_elf64_image interpreter_image;
    if (interpreter != NULL) {
        if (interpreter->data == NULL || interpreter->size == 0)
            return create_failed(NULL, error,
                    AARCH64_LINUX_PROCESS_ERROR_INTERPRETER,
                    AARCH64_LINUX_INTERPRETER_CONFIG_INVALID);
        elf_error = aarch64_elf64_parse_as_interpreter(
                interpreter->data, interpreter->size,
                &interpreter_image);
        if (elf_error != AARCH64_ELF64_OK)
            return create_failed(NULL, error,
                    AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_ELF,
                    (dword_t) elf_error);
        // 与 Linux 一致，解释器自身的 PT_INTERP 不递归处理。
    }

    struct aarch64_linux_process *process = calloc(1, sizeof(*process));
    if (process == NULL) {
        set_error(error, AARCH64_LINUX_PROCESS_ERROR_ALLOCATION, 0);
        return NULL;
    }
    process->memory = process_memory_create();
    if (process->memory == NULL)
        return create_failed(process, error,
                AARCH64_LINUX_PROCESS_ERROR_ALLOCATION, 0);
    struct guest_page_table *page_table =
            &process->memory->page_table;

    struct aarch64_elf64_load_result main_loaded;
    enum aarch64_elf64_load_error load_error = aarch64_elf64_load(
            &main_image, page_table,
            (guest_addr_t) config->load_bias, &main_loaded);
    if (load_error != AARCH64_ELF64_LOAD_OK)
        return create_failed(process, error,
                AARCH64_LINUX_PROCESS_ERROR_LOAD,
                (dword_t) load_error);

    struct aarch64_elf64_load_result interpreter_loaded = {0};
    if (interpreter != NULL) {
        load_error = aarch64_elf64_load(
                &interpreter_image, page_table,
                (guest_addr_t) interpreter->load_bias,
                &interpreter_loaded);
        if (load_error != AARCH64_ELF64_LOAD_OK)
            return create_failed(process, error,
                    AARCH64_LINUX_PROCESS_ERROR_INTERPRETER_LOAD,
                    (dword_t) load_error);
    }

    qword_t stack_bottom = config->stack_top - config->stack_size;
    qword_t trampoline_end = config->signal_trampoline_page +
            GUEST_MEMORY_PAGE_SIZE;
    if (config->brk_limit < main_loaded.brk_end ||
            ranges_overlap(main_loaded.brk_end, config->brk_limit,
                    stack_bottom, config->stack_top) ||
            ranges_overlap(main_loaded.brk_end, config->brk_limit,
                    config->signal_trampoline_page, trampoline_end) ||
            (interpreter != NULL && loaded_image_overlaps_range(
                    &interpreter_image, interpreter_loaded.load_bias,
                    main_loaded.brk_end, config->brk_limit)))
        return create_failed(process, error,
                AARCH64_LINUX_PROCESS_ERROR_LAYOUT, 0);

    guest_addr_t signal_trampoline;
    enum guest_page_table_result page_table_error =
            aarch64_linux_map_signal_trampoline(
                    page_table,
                    (guest_addr_t) config->signal_trampoline_page,
                    &signal_trampoline);
    if (page_table_error != GUEST_PAGE_TABLE_OK)
        return create_failed(process, error,
                AARCH64_LINUX_PROCESS_ERROR_TRAMPOLINE,
                (dword_t) page_table_error);

    const struct aarch64_linux_stack_config stack_config = {
        .stack_top = (guest_addr_t) config->stack_top,
        .stack_size = (size_t) config->stack_size,
        .executable = config->executable,
        .arguments = config->arguments,
        .argument_count = config->argument_count,
        .environment = config->environment,
        .environment_count = config->environment_count,
        .random = config->random,
        .uid = config->uid,
        .euid = config->euid,
        .gid = config->gid,
        .egid = config->egid,
        .secure = config->secure,
        .interpreter_base = interpreter_loaded.load_bias,
    };
    struct aarch64_linux_stack_result stack;
    enum aarch64_linux_stack_error stack_error =
            aarch64_linux_build_initial_stack(
                    page_table, &main_loaded,
                    &stack_config, &stack);
    if (stack_error != AARCH64_LINUX_STACK_OK)
        return create_failed(process, error,
                AARCH64_LINUX_PROCESS_ERROR_STACK,
                (dword_t) stack_error);

    copy_services(process, config, signal_trampoline);
    aarch64_linux_runtime_init(&process->runtime,
            &process->memory->linux_mm, page_table, main_loaded.brk_end,
            (guest_addr_t) config->brk_limit, &process->services);
    aarch64_linux_task_init(&process->task,
            config->tid, config->task_opaque);
    guest_tlb_init(&process->tlb,
            &page_table->address_space);
    aarch64_runner_init(&process->runner, &process->tlb);
    guest_addr_t initial_pc = interpreter == NULL ?
            main_loaded.entry : interpreter_loaded.entry;
    aarch64_linux_prepare_cpu_at(
            &process->cpu, initial_pc, &stack);
    return process;
}

struct aarch64_linux_process *aarch64_linux_process_fork(
        const struct aarch64_linux_process *parent,
        const struct aarch64_linux_process_fork_config *config,
        struct aarch64_linux_process_error *error) {
    set_error(error, AARCH64_LINUX_PROCESS_ERROR_NONE, 0);
    if (parent == NULL || config == NULL || config->tid <= 0 ||
            config->tid > AARCH64_LINUX_MAX_TID || config->set_tls > 1) {
        set_error(error, AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
        return NULL;
    }

    struct aarch64_linux_process *child = calloc(1, sizeof(*child));
    if (child == NULL) {
        set_error(error, AARCH64_LINUX_PROCESS_ERROR_ALLOCATION, 0);
        return NULL;
    }
    child->memory = process_memory_clone(parent->memory);
    if (child->memory == NULL)
        return create_failed(child, error,
                AARCH64_LINUX_PROCESS_ERROR_ALLOCATION, 0);

    copy_fork_services(child, parent);
    child->runtime = parent->runtime;
    child->runtime.memory = &child->memory->linux_mm;
    child->runtime.services = &child->services;
    aarch64_linux_task_init(&child->task,
            config->tid, config->task_opaque);
    guest_tlb_init(&child->tlb,
            &child->memory->page_table.address_space);
    aarch64_runner_init(&child->runner, &child->tlb);
    copy_fork_cpu(&child->cpu, &parent->cpu);
    apply_clone_state(child, config->stack_pointer,
            config->set_tls, config->tls, config->clear_child_tid);
    return child;
}

struct aarch64_linux_process *aarch64_linux_process_clone_thread(
        const struct aarch64_linux_process *parent,
        const struct aarch64_linux_process_thread_config *config,
        struct aarch64_linux_process_error *error) {
    set_error(error, AARCH64_LINUX_PROCESS_ERROR_NONE, 0);
    if (parent == NULL || config == NULL || config->tid <= 0 ||
            config->tid > AARCH64_LINUX_MAX_TID || config->set_tls > 1) {
        set_error(error, AARCH64_LINUX_PROCESS_ERROR_ARGUMENT, 0);
        return NULL;
    }

    struct aarch64_linux_process *child = calloc(1, sizeof(*child));
    if (child == NULL) {
        set_error(error, AARCH64_LINUX_PROCESS_ERROR_ALLOCATION, 0);
        return NULL;
    }
    process_memory_retain(parent->memory);
    child->memory = parent->memory;
    copy_fork_services(child, parent);
    child->runtime = parent->runtime;
    child->runtime.services = &child->services;
    aarch64_linux_task_init(&child->task,
            config->tid, config->task_opaque);
    guest_tlb_init(&child->tlb,
            &child->memory->page_table.address_space);
    aarch64_runner_init(&child->runner, &child->tlb);
    copy_fork_cpu(&child->cpu, &parent->cpu);
    apply_clone_state(child, config->stack_pointer,
            config->set_tls, config->tls, config->clear_child_tid);
    return child;
}

void aarch64_linux_process_destroy(
        struct aarch64_linux_process *process) {
    if (process == NULL)
        return;
    process_memory_release(process->memory);
    free(process);
}

const void *aarch64_linux_process_memory_identity(
        const struct aarch64_linux_process *process) {
    assert(process != NULL);
    return process->memory;
}

qword_t aarch64_linux_process_take_clear_child_tid(
        struct aarch64_linux_process *process) {
    assert(process != NULL);
    qword_t address = process->task.clear_child_tid;
    process->task.clear_child_tid = 0;
    return address;
}

bool aarch64_linux_process_uses_services(
        const struct aarch64_linux_process *process,
        pid_t_ tid, const void *task_opaque,
        const struct guest_linux_syscall_service *syscalls,
        const struct guest_linux_signal_service *signals) {
    if (process == NULL || syscalls == NULL || signals == NULL ||
            process->services.syscalls == NULL ||
            process->services.signals == NULL)
        return false;
    const struct guest_linux_syscall_service *owned_syscalls =
            process->services.syscalls;
    const struct guest_linux_signal_service *owned_signals =
            process->services.signals;
    return process->task.tid == tid &&
            process->task.service_opaque == task_opaque &&
            owned_syscalls->runtime_opaque ==
                    syscalls->runtime_opaque &&
            owned_syscalls->dispatch == syscalls->dispatch &&
            owned_signals->runtime_opaque ==
                    signals->runtime_opaque &&
            owned_signals->poll == signals->poll &&
            owned_signals->restore == signals->restore &&
            owned_signals->bad_frame == signals->bad_frame;
}

#if defined(AARCH64_LINUX_PROCESS_TESTING)
bool aarch64_linux_process_test_has_owned_state(
        const struct aarch64_linux_process *process,
        pid_t_ tid, const void *task_opaque,
        guest_addr_t clear_child_tid,
        guest_addr_t signal_trampoline,
        bool require_empty_tlb) {
    if (process == NULL)
        return false;
    bool owns_syscalls = process->services.syscalls == NULL ||
            process->services.syscalls == &process->syscall_service;
    bool owns_signals = process->services.signals == NULL ||
            process->services.signals == &process->signal_service;
    bool tlb_is_empty = true;
    for (unsigned i = 0; i < GUEST_TLB_SIZE; i++) {
        if (process->tlb.entries[i].valid) {
            tlb_is_empty = false;
            break;
        }
    }
    const struct guest_page_table *page_table =
            &process->memory->page_table;
    return owns_syscalls && owns_signals &&
            page_table->address_space.opaque == page_table &&
            process->tlb.address_space ==
                    &page_table->address_space &&
            process->tlb.observed_generation ==
                    page_table->address_space.generation &&
            (!require_empty_tlb || tlb_is_empty) &&
            process->runner.tlb == &process->tlb &&
            process->runtime.memory == &process->memory->linux_mm &&
            process->runtime.memory->page_table == page_table &&
            process->runtime.services == &process->services &&
            process->services.signal_trampoline == signal_trampoline &&
            process->task.tid == tid &&
            process->task.service_opaque == task_opaque &&
            process->task.clear_child_tid == clear_child_tid &&
            process->cpu.mmu == NULL &&
            process->cpu.poked_ptr == NULL && !process->cpu._poked;
}

bool aarch64_linux_process_test_has_thread_state(
        const struct aarch64_linux_process *process,
        guest_addr_t stack_pointer, qword_t tls,
        guest_addr_t clear_child_tid) {
    return process != NULL && process->cpu.sp == stack_pointer &&
            process->cpu.tpidr_el0 == tls &&
            process->task.clear_child_tid == clear_child_tid;
}
#endif

static struct aarch64_linux_process_result process_result(
        enum aarch64_linux_process_status status) {
    return (struct aarch64_linux_process_result) {
        .status = (dword_t) status,
    };
}

static void export_fault(struct guest_linux_user_fault *destination,
        const struct guest_memory_fault *source) {
    *destination = (struct guest_linux_user_fault) {
        .address = source->address,
        .access = (dword_t) source->access,
        .kind = (dword_t) source->kind,
    };
}

bool aarch64_linux_process_read_memory(
        struct aarch64_linux_process *process, qword_t address,
        void *destination, size_t size,
        struct guest_linux_user_fault *fault) {
    assert(process != NULL && destination != NULL);
    struct guest_memory_fault memory_fault;
    if (guest_tlb_read(&process->tlb, (guest_addr_t) address,
            destination, size, GUEST_MEMORY_READ, &memory_fault))
        return true;
    if (fault != NULL)
        export_fault(fault, &memory_fault);
    return false;
}

bool aarch64_linux_process_read_u32(
        struct aarch64_linux_process *process, qword_t address,
        dword_t *value, struct guest_linux_user_fault *fault) {
    assert(value != NULL);
    return aarch64_linux_process_read_memory(
            process, address, value, sizeof(*value), fault);
}

bool aarch64_linux_process_write_u32(
        struct aarch64_linux_process *process, qword_t address,
        dword_t value, struct guest_linux_user_fault *fault) {
    assert(process != NULL);
    struct guest_memory_fault memory_fault;
    if (guest_tlb_write(&process->tlb, (guest_addr_t) address,
            &value, sizeof(value), &memory_fault))
        return true;
    if (fault != NULL)
        export_fault(fault, &memory_fault);
    return false;
}

static void apply_signal_result(
        struct aarch64_linux_process_result *result,
        struct guest_linux_signal_poll_result signal) {
    result->signal = signal.signal;
    if (signal.status == GUEST_LINUX_SIGNAL_POLL_STOP)
        result->status = AARCH64_LINUX_PROCESS_STOP;
    else if (signal.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE)
        result->status = AARCH64_LINUX_PROCESS_TERMINATE;
}

struct aarch64_linux_process_result aarch64_linux_process_poll_signals(
        struct aarch64_linux_process *process) {
    assert(process != NULL);
    struct aarch64_linux_process_result result = process_result(
            AARCH64_LINUX_PROCESS_RUNNABLE);
    apply_signal_result(&result, aarch64_linux_poll_signals(
            &process->cpu, &process->tlb,
            &process->runtime, &process->task));
    return result;
}

static void apply_syscall_result(
        struct aarch64_linux_process_result *result,
        const struct aarch64_linux_syscall_result *syscall) {
    result->exit_status = syscall->exit_status;
    result->signal = syscall->signal;
    if (syscall->fault.kind != GUEST_MEMORY_FAULT_NONE)
        export_fault(&result->fault, &syscall->fault);
    switch (syscall->action) {
        case AARCH64_LINUX_SYSCALL_RESUME:
            break;
        case AARCH64_LINUX_SYSCALL_EXIT:
            result->status = AARCH64_LINUX_PROCESS_EXIT;
            break;
        case AARCH64_LINUX_SYSCALL_EXIT_GROUP:
            result->status = AARCH64_LINUX_PROCESS_EXIT_GROUP;
            break;
        case AARCH64_LINUX_SYSCALL_STOP:
            result->status = AARCH64_LINUX_PROCESS_STOP;
            break;
        case AARCH64_LINUX_SYSCALL_TERMINATE:
            result->status = AARCH64_LINUX_PROCESS_TERMINATE;
            break;
        case AARCH64_LINUX_SYSCALL_EXEC:
            result->status = AARCH64_LINUX_PROCESS_EXEC;
            break;
    }
}

struct aarch64_linux_process_result aarch64_linux_process_run_one(
        struct aarch64_linux_process *process) {
    assert(process != NULL);
    struct aarch64_linux_process_result result = process_result(
            AARCH64_LINUX_PROCESS_RUNNABLE);
    struct aarch64_step_result step = aarch64_run_one(
            &process->runner, &process->cpu);
    result.instruction = step.instruction;
    switch (step.stop) {
        case AARCH64_STEP_RETIRED:
            apply_signal_result(&result, aarch64_linux_poll_signals(
                    &process->cpu, &process->tlb,
                    &process->runtime, &process->task));
            break;
        case AARCH64_STEP_FETCH_FAULT:
            result.status = AARCH64_LINUX_PROCESS_FETCH_FAULT;
            export_fault(&result.fault, &step.fault);
            process->cpu.segfault_addr = step.fault.address;
            process->cpu.segfault_was_write = false;
            break;
        case AARCH64_STEP_DATA_FAULT:
            result.status = AARCH64_LINUX_PROCESS_DATA_FAULT;
            export_fault(&result.fault, &step.fault);
            process->cpu.segfault_addr = step.fault.address;
            process->cpu.segfault_was_write =
                    step.fault.access == GUEST_MEMORY_WRITE;
            break;
        case AARCH64_STEP_UNDEFINED:
            result.status = AARCH64_LINUX_PROCESS_UNDEFINED;
            result.fault.address = process->cpu.pc;
            break;
        case AARCH64_STEP_SYSCALL: {
            struct aarch64_linux_syscall_result syscall =
                    aarch64_linux_dispatch_syscall(
                            &process->cpu, &process->tlb,
                            &process->runtime, &process->task);
            apply_syscall_result(&result, &syscall);
            break;
        }
    }
    return result;
}

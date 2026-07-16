#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/linux/futex-abi.h"
#include "guest/linux/mman.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/futex.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/time.h"

#define IMAGE_SIZE 1024
#define TEXT_BASE UINT64_C(0x400000)
#define RUNTIME_COW_BASE UINT64_C(0x600000)
#define PADZERO_BASE UINT64_C(0x610000)
#define PIN_COW_BASE UINT64_C(0x620000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define SHARED_BASE UINT64_C(0x0000000100000000)
#define PRIVATE_BASE UINT64_C(0x0000000100010000)
#define READ_ONLY_PRIVATE_BASE UINT64_C(0x0000000100020000)
#define READ_ONLY_SHARED_BASE UINT64_C(0x0000000100030000)
#define READ_ONLY_STACK_BASE (STACK_TOP - 2 * GUEST_MEMORY_PAGE_SIZE)
#define WAITERS_BASE (PRIVATE_BASE + UINT64_C(0x100))
#define TIMEOUT_BASE (PRIVATE_BASE + UINT64_C(0xe00))
#define PRIVATE_WORD (PRIVATE_BASE + UINT64_C(0xf00))
#define SHARED_WORD_BASE (SHARED_BASE + UINT64_C(0x200))
#define UNMAPPED_ADDRESS UINT64_C(0x0000000100100000)
#define FUTEX_WAITV_SYSCALL UINT64_C(449)
#define FUTEX_WAKE_ UINT32_C(1)
#define FUTEX_REQUEUE_ UINT32_C(3)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 futex_waitv 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct fixture {
    struct task *parent;
    struct tgroup group;
    size_t pin_cow_protect_steps;
};

struct user_probe {
    struct task *task;
    qword_t fail_read_at;
    unsigned read_calls;
    qword_t last_read_address;
    dword_t last_read_size;
};

struct wait_thread {
    pthread_t thread;
    struct task *task;
    qword_t waiters;
    qword_t count;
    qword_t flags;
    qword_t timeout;
    qword_t clock;
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    atomic_uint stage;
    qword_t result;
};

static const struct fd_ops metadata_fd_ops = {0};

static void put_u16(byte_t *bytes, word_t value) {
    bytes[0] = (byte_t) value;
    bytes[1] = (byte_t) (value >> 8);
}

static void put_u32(byte_t *bytes, dword_t value) {
    for (byte_t index = 0; index < 4; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
}

static void put_u64(byte_t *bytes, qword_t value) {
    for (byte_t index = 0; index < 8; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
}

static void put_program_header(byte_t *bytes, dword_t type,
        dword_t flags, qword_t offset, qword_t address,
        qword_t file_size, qword_t memory_size, qword_t alignment) {
    put_u32(bytes, type);
    put_u32(bytes + 4, flags);
    put_u64(bytes + 8, offset);
    put_u64(bytes + 16, address);
    put_u64(bytes + 32, file_size);
    put_u64(bytes + 40, memory_size);
    put_u64(bytes + 48, alignment);
}

static void append_move(dword_t *program, size_t *count,
        byte_t reg, qword_t value) {
    program[(*count)++] = UINT32_C(0xd2800000) |
            ((dword_t) (value & UINT64_C(0xffff)) << 5) | reg;
    for (dword_t half = 1; half < 4; half++) {
        dword_t immediate = (dword_t) (value >> (half * 16)) &
                UINT32_C(0xffff);
        if (immediate != 0) {
            program[(*count)++] = UINT32_C(0xf2800000) |
                    (half << 21) | (immediate << 5) | reg;
        }
    }
}

static void append_mmap(dword_t *program, size_t *count,
        qword_t address, qword_t protection, qword_t flags) {
    append_move(program, count, 0, address);
    append_move(program, count, 1, GUEST_MEMORY_PAGE_SIZE);
    append_move(program, count, 2, protection);
    append_move(program, count, 3, flags);
    append_move(program, count, 4, UINT64_MAX);
    append_move(program, count, 5, 0);
    append_move(program, count, 8, 222);
    program[(*count)++] = UINT32_C(0xd4000001);
}

static void append_mprotect(dword_t *program, size_t *count,
        qword_t address, qword_t protection) {
    append_move(program, count, 0, address);
    append_move(program, count, 1, GUEST_MEMORY_PAGE_SIZE);
    append_move(program, count, 2, protection);
    append_move(program, count, 8, 226);
    program[(*count)++] = UINT32_C(0xd4000001);
}

static size_t make_image(byte_t file[IMAGE_SIZE],
        size_t *setup_steps) {
    dword_t program[96];
    size_t instruction_count = 0;
    append_mmap(program, &instruction_count, SHARED_BASE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_mmap(program, &instruction_count, PRIVATE_BASE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_mmap(program, &instruction_count, READ_ONLY_PRIVATE_BASE,
            GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_mmap(program, &instruction_count, READ_ONLY_SHARED_BASE,
            GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_mprotect(program, &instruction_count,
            READ_ONLY_STACK_BASE, GUEST_LINUX_PROT_READ);
    append_move(program, &instruction_count, 0, RUNTIME_COW_BASE);
    append_move(program, &instruction_count, 1, UINT32_C(0x12345678));
    program[instruction_count++] = UINT32_C(0xb9000001);
    append_mprotect(program, &instruction_count,
            RUNTIME_COW_BASE, GUEST_LINUX_PROT_READ);
    append_mprotect(program, &instruction_count,
            PADZERO_BASE, GUEST_LINUX_PROT_READ);
    *setup_steps = instruction_count;
    append_mprotect(program, &instruction_count,
            PIN_COW_BASE, GUEST_LINUX_PROT_READ);

    memset(file, 0, IMAGE_SIZE);
    file[0] = 0x7f;
    file[1] = 'E';
    file[2] = 'L';
    file[3] = 'F';
    file[4] = 2;
    file[5] = 1;
    file[6] = 1;
    put_u16(file + 16, 2);
    put_u16(file + 18, 183);
    put_u32(file + 20, 1);
    put_u64(file + 24, TEXT_BASE + ENTRY_OFFSET);
    put_u64(file + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(file + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(file + 56, 5);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t headers_size = 5 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4, AARCH64_ELF64_HEADER_SIZE,
            TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            headers_size, headers_size, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, IMAGE_SIZE,
            IMAGE_SIZE, GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, RUNTIME_COW_BASE, 4, 4,
            GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, PADZERO_BASE, 4, 8,
            GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 4 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, PIN_COW_BASE, 4, 4,
            GUEST_MEMORY_PAGE_SIZE);
    if (instruction_count >
            (IMAGE_SIZE - ENTRY_OFFSET) / sizeof(*program))
        abort();
    for (size_t index = 0; index < instruction_count; index++)
        put_u32(file + ENTRY_OFFSET + index * sizeof(*program),
                program[index]);
    return instruction_count;
}

static struct task *make_parent(struct tgroup *group) {
    struct task *parent = task_create_(NULL);
    if (parent == NULL)
        return NULL;
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = parent;
    group->sid = parent->pid;
    group->pgid = parent->pid;
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    parent->group = group;
    parent->tgid = parent->pid;

    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(1);
    struct fs_info *fs = fs_info_new();
    struct sighand *sighand = sighand_new();
    struct fd *metadata = fd_create(&metadata_fd_ops);
    if (mm == NULL || IS_ERR(files) || fs == NULL ||
            sighand == NULL || metadata == NULL)
        return NULL;
    parent->files = files;
    if (f_install_task(parent, fd_retain(metadata), 0) != 0)
        return NULL;
    mm->exefile = fd_retain(metadata);
    fs->root = fd_retain(metadata);
    fs->pwd = fd_retain(metadata);
    fd_close(metadata);
    task_set_mm(parent, mm);
    parent->fs = fs;
    parent->sighand = sighand;
    task_thread_store(parent, pthread_self());
    task_publish(parent);
    current = parent;
    return parent;
}

static struct aarch64_linux_process *make_process(
        struct task *task, size_t *setup_steps,
        size_t *pin_cow_protect_steps) {
    byte_t file[IMAGE_SIZE];
    size_t total_steps = make_image(file, setup_steps);
    *pin_cow_protect_steps = total_steps - *setup_steps;
    const char *arguments[] = {"futex-waitv-test"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x800000),
        .executable = "/bin/futex-waitv-test",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
        .tid = task->pid,
        .task_opaque = task,
        .syscalls = &ish_aarch64_linux_syscall_service,
        .signals = &ish_aarch64_linux_signal_service,
    };
    return aarch64_linux_process_create(&config, NULL);
}

static bool init_fixture(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->parent = make_parent(&fixture->group);
    if (fixture->parent == NULL)
        return false;
    size_t setup_steps;
    struct aarch64_linux_process *process =
            make_process(fixture->parent, &setup_steps,
                    &fixture->pin_cow_protect_steps);
    if (process == NULL)
        return false;
    for (size_t step = 0; step < setup_steps; step++) {
        if (aarch64_linux_process_run_one(process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    return task_attach_aarch64_process(fixture->parent, process);
}

static bool protect_pin_cow_page(struct fixture *fixture) {
    for (size_t step = 0;
            step < fixture->pin_cow_protect_steps; step++) {
        if (aarch64_linux_process_run_one(
                fixture->parent->aarch64_process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    fixture->pin_cow_protect_steps = 0;
    return true;
}

static void destroy_fixture(struct fixture *fixture) {
    struct task *parent = fixture->parent;
    task_release_aarch64_process(parent);
    signal_flush_pending(parent);
    mm_release(parent->mm);
    fdtable_release(parent->files);
    fs_info_release(parent->fs);
    sighand_release(parent->sighand);
    parent->mm = NULL;
    parent->files = NULL;
    parent->fs = NULL;
    parent->sighand = NULL;
    current = NULL;
    cond_destroy(&parent->pause);
    cond_destroy(&parent->ptrace.cond);
    lock(&pids_lock);
    lock(&fixture->group.lock);
    list_remove(&parent->group_links);
    list_remove(&fixture->group.session);
    list_remove(&fixture->group.pgroup);
    task_destroy(parent);
    unlock(&fixture->group.lock);
    unlock(&pids_lock);
    cond_destroy(&fixture->group.stopped_cond);
    cond_destroy(&fixture->group.child_exit);
}

static struct task *make_related_task(struct task *parent, bool thread) {
    struct task *task = task_create_(parent);
    if (task == NULL)
        return NULL;
    struct aarch64_linux_process *process;
    if (thread) {
        const struct aarch64_linux_process_thread_config config = {
            .tid = task->pid,
            .task_opaque = task,
        };
        process = aarch64_linux_process_clone_thread(
                parent->aarch64_process, &config, NULL);
    } else {
        const struct aarch64_linux_process_fork_config config = {
            .tid = task->pid,
            .task_opaque = task,
        };
        process = aarch64_linux_process_fork(
                parent->aarch64_process, &config, NULL);
    }
    if (process == NULL || !task_attach_aarch64_process(task, process)) {
        aarch64_linux_process_destroy(process);
        task_abort_create(task);
        return NULL;
    }
    return task;
}

static void destroy_related_task(struct task *parent, struct task *task) {
    current = parent;
    signal_flush_pending(task);
    task_abort_create(task);
}

static bool write_u32(struct task *task, qword_t address, dword_t value) {
    struct guest_linux_user_fault fault = {0};
    return aarch64_linux_process_write_u32(
            task->aarch64_process, address, value, &fault);
}

static bool write_u64(struct task *task, qword_t address, qword_t value) {
    return write_u32(task, address, (dword_t) value) &&
            write_u32(task, address + 4, (dword_t) (value >> 32));
}

static bool write_waiter(struct task *task, size_t index,
        qword_t value, qword_t address, dword_t flags,
        dword_t reserved) {
    qword_t base = WAITERS_BASE +
            index * sizeof(struct guest_linux_futex_waitv);
    return write_u64(task, base, value) &&
            write_u64(task, base + 8, address) &&
            write_u32(task, base + 16, flags) &&
            write_u32(task, base + 20, reserved);
}

static bool write_timespec(struct task *task,
        sqword_t sec, sqword_t nsec) {
    return write_u64(task, TIMEOUT_BASE, (qword_t) sec) &&
            write_u64(task, TIMEOUT_BASE + 8, (qword_t) nsec);
}

static bool range_contains(qword_t address, dword_t size,
        qword_t target) {
    return target >= address && target - address < size;
}

static void reset_probe(struct user_probe *probe, struct task *task) {
    *probe = (struct user_probe) {
        .task = task,
        .fail_read_at = UINT64_MAX,
    };
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->read_calls++;
    probe->last_read_address = address;
    probe->last_read_size = size;
    if (probe->fail_read_at != UINT64_MAX &&
            range_contains(address, size, probe->fail_read_at)) {
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_read_at,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    return aarch64_linux_process_read_memory(
            probe->task->aarch64_process, address,
            destination, size, fault);
}

static qword_t invoke_waitv(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault, qword_t waiters,
        qword_t count, qword_t flags, qword_t timeout, qword_t clock) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {
            .opaque = probe,
            .read = read_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = FUTEX_WAITV_SYSCALL,
        .arguments = {waiters, count, flags, timeout, clock},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static bool returned(qword_t result, int error) {
    return (sqword_t) result == error;
}

static int wake_word(struct task *task, qword_t address,
        bool private_mapping, dword_t count) {
    struct task *saved = current;
    current = task;
    struct guest_linux_user_fault fault = {0};
    dword_t result = sys_futex_aarch64(address,
            FUTEX_WAKE_ | (private_mapping ?
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG : 0),
            count, 0, 0, 0, &fault);
    current = saved;
    return (sdword_t) result;
}

static int requeue_word(struct task *task, qword_t source,
        qword_t target, dword_t count) {
    struct task *saved = current;
    current = task;
    struct guest_linux_user_fault fault = {0};
    dword_t result = sys_futex_aarch64(source, FUTEX_REQUEUE_,
            0, count, target, 0, &fault);
    current = saved;
    return (sdword_t) result;
}

static void *wait_thread_main(void *opaque) {
    struct wait_thread *waiter = opaque;
    current = waiter->task;
    task_thread_store(waiter->task, pthread_self());
    atomic_store_explicit(&waiter->stage, 1, memory_order_release);
    waiter->result = invoke_waitv(waiter->task, &waiter->probe,
            &waiter->fault, waiter->waiters, waiter->count,
            waiter->flags, waiter->timeout, waiter->clock);
    atomic_store_explicit(&waiter->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool start_wait_thread(struct wait_thread *waiter,
        struct task *task, dword_t count, qword_t timeout,
        sdword_t clock) {
    *waiter = (struct wait_thread) {
        .task = task,
        .waiters = WAITERS_BASE,
        .count = count,
        .timeout = timeout,
        .clock = (qword_t) clock,
    };
    reset_probe(&waiter->probe, task);
    atomic_init(&waiter->stage, 0);
    return pthread_create(
            &waiter->thread, NULL, wait_thread_main, waiter) == 0;
}

static bool wait_until_registered(struct task *task) {
    for (unsigned attempt = 0; attempt < 200000; attempt++) {
        lock(&task->waiting_cond_lock);
        bool registered = task->waiting_cond != NULL &&
                task->waiting_lock != NULL;
        unlock(&task->waiting_cond_lock);
        if (registered)
            return true;
        sched_yield();
    }
    return false;
}

static bool join_wait_thread(struct wait_thread *waiter, int result) {
    return pthread_join(waiter->thread, NULL) == 0 &&
            returned(waiter->result, result);
}

static bool test_dispatch_and_copy(struct fixture *fixture) {
    struct task *task = fixture->parent;
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    CHECK(write_u32(task, SHARED_WORD_BASE, 7) &&
            write_waiter(task, 0, 8, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备高地址 waiter");

    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    qword_t result = invoke_waitv(task, &probe, &fault, WAITERS_BASE,
            (UINT64_C(0x12345678) << 32) | 1,
            UINT64_C(0xabcdef01) << 32, 0,
            (UINT64_C(0xfedcba98) << 32) | UINT32_C(99));
    CHECK(returned(result, _EAGAIN) && probe.read_calls == 1,
            "dispatcher 只取标量低 32 位且保留完整 64 位地址");

    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    0, 1, 0, 0, 0), _EINVAL) &&
            returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 0, 0, 0, 0), _EINVAL) &&
            returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 129, 0, 0, 0), _EINVAL) &&
            returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 1, 0, 0), _EINVAL) &&
            probe.read_calls == 0,
            "先校验 syscall 级参数且不触碰用户内存");

    const struct {
        qword_t value;
        dword_t flags;
        dword_t reserved;
    } invalid[] = {
        {8, 0, 0},
        {8, GUEST_LINUX_FUTEX2_SIZE_U32 | UINT32_C(4), 0},
        {8, GUEST_LINUX_FUTEX2_SIZE_U32, 1},
        {UINT64_C(1) << 32, GUEST_LINUX_FUTEX2_SIZE_U32, 0},
    };
    for (size_t index = 0; index < array_size(invalid); index++) {
        CHECK(write_waiter(task, 0, invalid[index].value,
                        SHARED_WORD_BASE, invalid[index].flags,
                        invalid[index].reserved),
                "写入非法 waiter ABI");
        reset_probe(&probe, task);
        CHECK(returned(invoke_waitv(task, &probe, &fault,
                        WAITERS_BASE, 1, 0, 0, 0), _EINVAL) &&
                probe.read_calls == 1,
                "逐项拒绝未实现 size、标志、reserved 和高位值");
    }

    for (size_t index = 0; index < 3; index++)
        CHECK(write_waiter(task, index, 8, SHARED_WORD_BASE,
                        GUEST_LINUX_FUTEX2_SIZE_U32, 0),
                "准备逐项 copy fault waiter");
    for (size_t index = 0; index < 3; index++) {
        reset_probe(&probe, task);
        probe.fail_read_at = WAITERS_BASE +
                index * sizeof(struct guest_linux_futex_waitv) + 8;
        fault = (struct guest_linux_user_fault) {0};
        CHECK(returned(invoke_waitv(task, &probe, &fault,
                        WAITERS_BASE, 3, 0, 0, 0), _EFAULT) &&
                probe.read_calls == index + 1 &&
                fault.address == probe.fail_read_at &&
                fault.access == GUEST_MEMORY_READ &&
                fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
                "逐个元素 copy fault 保留精确故障元数据");
    }

    CHECK(write_timespec(task, 0, 0), "准备 timeout wire");
    reset_probe(&probe, task);
    probe.fail_read_at = TIMEOUT_BASE + 7;
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, TIMEOUT_BASE,
                    CLOCK_MONOTONIC_), _EFAULT) &&
            probe.read_calls == 1 &&
            fault.address == TIMEOUT_BASE + 7 &&
            fault.access == GUEST_MEMORY_READ,
            "timeout copy fault 先于 waiter 且保留地址");
    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, TIMEOUT_BASE, 77),
                    _EINVAL) && probe.read_calls == 0,
            "非空 timeout 的非法 clock 在 copy 前失败");
    return true;
}

static bool test_two_phase_order(struct fixture *fixture) {
    struct task *task = fixture->parent;
    struct user_probe probe;
    struct guest_linux_user_fault fault = {0};
    CHECK(write_u32(task, SHARED_WORD_BASE, 10) &&
            write_waiter(task, 0, 11, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0) &&
            write_waiter(task, 1, 0, UNMAPPED_ADDRESS,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备共享键阶段故障");
    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EFAULT) &&
            fault.address == UNMAPPED_ADDRESS &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "共享键解析 EFAULT 先于较早元素值不匹配");

    CHECK(write_waiter(task, 1, 0, UNMAPPED_ADDRESS,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "切换为 PRIVATE 延迟取值");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "PRIVATE 后项取值故障不能越过前项 EAGAIN");
    CHECK(write_waiter(task, 0, 10, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "让前项匹配以继续第二阶段");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EFAULT) &&
            fault.address == UNMAPPED_ADDRESS,
            "前项匹配后报告 PRIVATE 后项 EFAULT");

    CHECK(write_waiter(task, 0, 11, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0) &&
            write_waiter(task, 1, 0, READ_ONLY_PRIVATE_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备只读匿名私有页的 SHARED 键故障");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EFAULT) &&
            fault.address == READ_ONLY_PRIVATE_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "只读匿名私有页键故障先于前项值 mismatch");
    CHECK(write_waiter(task, 1, 0, READ_ONLY_PRIVATE_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "将只读匿名私有页切换为 PRIVATE 键");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE &&
            write_waiter(task, 0, 10, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0) &&
            write_waiter(task, 1, 1, READ_ONLY_PRIVATE_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "PRIVATE 保留前项优先并允许读取只读匿名私有页");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "PRIVATE 只读页在值不匹配时返回 EAGAIN");
    CHECK(write_waiter(task, 1, 1, READ_ONLY_SHARED_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备只读匿名共享页控制项");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 2, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "只读匿名共享页仍可形成 SHARED 稳定键");

    CHECK(write_waiter(task, 0, 0, READ_ONLY_STACK_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备只读初始栈的 SHARED 键故障");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EFAULT) &&
            fault.address == READ_ONLY_STACK_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "只读初始栈保留匿名私有页来源");
    CHECK(write_waiter(task, 0, 1, READ_ONLY_STACK_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "将只读初始栈切换为 PRIVATE 键");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "PRIVATE waitv 仍可读取只读初始栈");

    CHECK(write_waiter(task, 0, 0, TEXT_BASE + ENTRY_OFFSET,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备只读 ELF 文件页控制项");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "只读 ELF 文件页通过来源检查并到达值比较");

    CHECK(write_waiter(task, 0, 0, RUNTIME_COW_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备运行期 COW 页的 SHARED 键故障");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EFAULT) &&
            fault.address == RUNTIME_COW_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "运行期写入后降权的 ELF 私有页已经转为匿名来源");
    CHECK(write_waiter(task, 0, 0, RUNTIME_COW_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "将运行期 COW 页切换为 PRIVATE 键");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "PRIVATE waitv 仍可读取运行期 COW 页");

    CHECK(write_waiter(task, 0, 0, PADZERO_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备 PT_LOAD 文件尾补零页的 SHARED 键故障");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EFAULT) &&
            fault.address == PADZERO_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "可写 PT_LOAD 的文件尾补零页保留匿名 COW 来源");
    CHECK(write_waiter(task, 0, 0, PADZERO_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "将文件尾补零页切换为 PRIVATE 键");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "PRIVATE waitv 仍可读取文件尾补零页");

    CHECK(write_waiter(task, 0, 0, PIN_COW_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备仍可写纯文件页的 SHARED waitv 写固定");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE &&
            protect_pin_cow_page(fixture),
            "waitv 共享取键私有化纯文件页后再由 guest 降权");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EFAULT) &&
            fault.address == PIN_COW_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            write_waiter(task, 0, 0, PIN_COW_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "仅由 waitv 取键触发的文件页 COW 降权后拒绝 SHARED 键");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EAGAIN) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "PRIVATE waitv 仍可读取由共享取键私有化的文件页");

    CHECK(write_waiter(task, 0, 0, SIGNAL_TRAMPOLINE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备特殊信号跳板键故障");
    reset_probe(&probe, task);
    fault = (struct guest_linux_user_fault) {0};
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, 0), _EFAULT) &&
            fault.address == SIGNAL_TRAMPOLINE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "特殊信号跳板不能伪装成文件后备 SHARED 键");
    return true;
}

static bool test_private_shared_and_signal(struct fixture *fixture) {
    struct task *parent = fixture->parent;
    CHECK(write_u32(parent, PRIVATE_WORD, 21) &&
            write_waiter(parent, 0, 21, PRIVATE_WORD,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "准备单项 PRIVATE 等待");
    struct task *thread = make_related_task(parent, true);
    struct wait_thread waiter;
    CHECK(thread != NULL && start_wait_thread(
                    &waiter, thread, 1, 0, CLOCK_MONOTONIC_),
            "启动单项 PRIVATE 等待者");
    CHECK(wait_until_registered(thread) &&
            wake_word(parent, PRIVATE_WORD, false, 1) == 0 &&
            wake_word(parent, PRIVATE_WORD, true, 1) == 1 &&
            join_wait_thread(&waiter, 0),
            "mm-shared 键不命中同 VA 的 PRIVATE waitv 队列");
    destroy_related_task(parent, thread);

    CHECK(write_waiter(parent, 0, 21, PRIVATE_WORD,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备私有匿名页上的 mm-shared 等待");
    thread = make_related_task(parent, true);
    CHECK(thread != NULL && start_wait_thread(
                    &waiter, thread, 1, 0, CLOCK_MONOTONIC_) &&
            wait_until_registered(thread) &&
            wake_word(parent, PRIVATE_WORD, true, 1) == 0 &&
            wake_word(parent, PRIVATE_WORD, false, 1) == 1 &&
            join_wait_thread(&waiter, 0),
            "PRIVATE 键不命中同 VA 的 mm-shared waitv 队列");
    destroy_related_task(parent, thread);

    CHECK(write_u32(parent, SHARED_WORD_BASE, 22) &&
            write_waiter(parent, 0, 22, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备 fork 共享等待");
    struct task *child = make_related_task(parent, false);
    CHECK(child != NULL && start_wait_thread(
                    &waiter, child, 1, 0, CLOCK_MONOTONIC_),
            "启动 fork 子进程共享等待者");
    CHECK(wait_until_registered(child) &&
            wake_word(parent, SHARED_WORD_BASE, false, 1) == 1 &&
            join_wait_thread(&waiter, 0),
            "fork 父子通过共享 backing 唤醒 waitv");
    destroy_related_task(parent, child);

    CHECK(write_waiter(parent, 0, 22, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "准备 fork PRIVATE 隔离等待");
    child = make_related_task(parent, false);
    CHECK(child != NULL && start_wait_thread(
                    &waiter, child, 1, 0, CLOCK_MONOTONIC_) &&
            wait_until_registered(child),
            "启动可中断的 fork PRIVATE 等待者");
    CHECK(wake_word(parent, SHARED_WORD_BASE, true, 1) == 0,
            "fork 后相同 VA 的 PRIVATE 键保持隔离");
    send_signal(child, SIGUSR1_, (struct siginfo_) {.code = SI_USER_});
    CHECK(join_wait_thread(&waiter, _EINTR),
            "guest 信号中断多地址等待并返回 EINTR");
    destroy_related_task(parent, child);
    return true;
}

static bool test_maximum_and_duplicates(struct fixture *fixture) {
    struct task *parent = fixture->parent;
    unsigned baseline = futex_test_live_count();
    for (size_t index = 0; index < GUEST_LINUX_FUTEX_WAITV_MAX;
            index++) {
        CHECK(write_u32(parent, SHARED_WORD_BASE + index * 4,
                        UINT32_C(0x1000) + (dword_t) index) &&
                write_waiter(parent, index,
                        UINT32_C(0x1000) + (dword_t) index,
                        SHARED_WORD_BASE + index * 4,
                        GUEST_LINUX_FUTEX2_SIZE_U32, 0),
                "准备 128 项 waitv");
    }
    struct task *thread = make_related_task(parent, true);
    struct wait_thread waiter;
    const size_t wake_index = 93;
    CHECK(thread != NULL && start_wait_thread(&waiter, thread,
                    GUEST_LINUX_FUTEX_WAITV_MAX, 0,
                    CLOCK_MONOTONIC_) && wait_until_registered(thread),
            "原子登记 128 项等待");
    CHECK(wake_word(parent, SHARED_WORD_BASE + wake_index * 4,
                    false, 1) == 1 &&
            join_wait_thread(&waiter, (int) wake_index) &&
            futex_test_live_count() == baseline,
            "任意索引唤醒返回该索引并清理其余 127 项");
    destroy_related_task(parent, thread);

    CHECK(write_u32(parent, SHARED_WORD_BASE, 31) &&
            write_waiter(parent, 0, 31, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0) &&
            write_waiter(parent, 1, 31, SHARED_WORD_BASE,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备重复地址 waitv");
    thread = make_related_task(parent, true);
    CHECK(thread != NULL && start_wait_thread(
                    &waiter, thread, 2, 0, CLOCK_MONOTONIC_) &&
            wait_until_registered(thread),
            "登记同一键的两个独立等待节点");
    CHECK(wake_word(parent, SHARED_WORD_BASE, false, 1) == 1 &&
            join_wait_thread(&waiter, 0) &&
            wake_word(parent, SHARED_WORD_BASE, false, 2) == 0 &&
            futex_test_live_count() == baseline,
            "重复地址消耗一个 wake 名额并统一撤销剩余节点");
    destroy_related_task(parent, thread);

    const qword_t untouched = SHARED_WORD_BASE + UINT64_C(0x80);
    const qword_t source = SHARED_WORD_BASE + UINT64_C(0x84);
    const qword_t target = SHARED_WORD_BASE + UINT64_C(0x88);
    CHECK(write_u32(parent, untouched, 51) &&
            write_u32(parent, source, 52) &&
            write_u32(parent, target, 0) &&
            write_waiter(parent, 0, 51, untouched,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0) &&
            write_waiter(parent, 1, 52, source,
                    GUEST_LINUX_FUTEX2_SIZE_U32, 0),
            "准备 classic REQUEUE 与 waitv 共享节点");
    thread = make_related_task(parent, true);
    CHECK(thread != NULL && start_wait_thread(
                    &waiter, thread, 2, 0, CLOCK_MONOTONIC_) &&
            wait_until_registered(thread),
            "登记待迁移的向量索引一");
    CHECK(requeue_word(parent, source, target, 1) == 1 &&
            wake_word(parent, source, false, 1) == 0 &&
            wake_word(parent, target, false, 1) == 1 &&
            join_wait_thread(&waiter, 1) &&
            wake_word(parent, untouched, false, 1) == 0 &&
            futex_test_live_count() == baseline,
            "REQUEUE 更新节点所属键且目标 wake 返回原向量索引");
    destroy_related_task(parent, thread);
    return true;
}

static bool test_timeouts_and_oom(struct fixture *fixture) {
    struct task *task = fixture->parent;
    struct user_probe probe;
    struct guest_linux_user_fault fault = {0};
    CHECK(write_u32(task, PRIVATE_WORD, 41) &&
            write_waiter(task, 0, 41, PRIVATE_WORD,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0) &&
            write_timespec(task, 0, 0),
            "准备绝对超时等待");
    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, TIMEOUT_BASE,
                    CLOCK_MONOTONIC_), _ETIMEDOUT),
            "过去的 MONOTONIC 绝对期限立即超时");

    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0,
            "读取 host MONOTONIC");
    sqword_t deadline_sec = now.tv_sec;
    sqword_t deadline_nsec = now.tv_nsec + 30000000;
    if (deadline_nsec >= 1000000000) {
        deadline_sec++;
        deadline_nsec -= 1000000000;
    }
    CHECK(write_timespec(task, deadline_sec, deadline_nsec),
            "写入未来绝对期限");
    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, TIMEOUT_BASE,
                    (UINT64_C(0x76543210) << 32) |
                    CLOCK_MONOTONIC_), _ETIMEDOUT),
            "dispatcher 截低 clock 且按绝对期限等待");
    clock_gettime(CLOCK_MONOTONIC, &finished);
    qword_t elapsed_ns = (qword_t) (finished.tv_sec - started.tv_sec) *
            UINT64_C(1000000000) + finished.tv_nsec - started.tv_nsec;
    CHECK(elapsed_ns >= UINT64_C(5000000) &&
            elapsed_ns < UINT64_C(2000000000),
            "未来绝对期限既非立即返回也未被当成相对时长");

    CHECK(clock_gettime(CLOCK_REALTIME, &now) == 0 &&
            write_timespec(task, now.tv_sec - 1, now.tv_nsec),
            "准备 REALTIME 过去期限");
    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, TIMEOUT_BASE,
                    CLOCK_REALTIME_), _ETIMEDOUT),
            "支持 REALTIME 绝对期限");

    const sqword_t invalid_times[][2] = {
        {-1, 0}, {0, -1}, {0, 1000000000},
    };
    for (size_t index = 0; index < array_size(invalid_times); index++) {
        CHECK(write_timespec(task, invalid_times[index][0],
                        invalid_times[index][1]),
                "写入非法 timespec");
        reset_probe(&probe, task);
        CHECK(returned(invoke_waitv(task, &probe, &fault,
                        WAITERS_BASE, 1, 0, TIMEOUT_BASE,
                        CLOCK_MONOTONIC_), _EINVAL),
                "拒绝负字段和越界纳秒");
    }
    CHECK(write_waiter(task, 0, 42, PRIVATE_WORD,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG, 0),
            "准备无 timeout 的值不匹配");
    reset_probe(&probe, task);
    CHECK(returned(invoke_waitv(task, &probe, &fault,
                    WAITERS_BASE, 1, 0, 0, UINT32_C(77)),
                    _EAGAIN),
            "空 timeout 时忽略非法 clock");

    unsigned baseline = futex_test_live_count();
    for (size_t index = 0; index < 3; index++) {
        CHECK(write_u32(task, SHARED_WORD_BASE + index * 4,
                        UINT32_C(0x2000) + (dword_t) index) &&
                write_waiter(task, index,
                        UINT32_C(0x2000) + (dword_t) index,
                        SHARED_WORD_BASE + index * 4,
                        GUEST_LINUX_FUTEX2_SIZE_U32, 0),
                "准备 OOM 原子回滚项");
    }
    const size_t failures[] = {0, 1, 2, 3};
    for (size_t index = 0; index < array_size(failures); index++) {
        futex_test_fail_allocation_at(failures[index]);
        reset_probe(&probe, task);
        qword_t result = invoke_waitv(task, &probe, &fault,
                WAITERS_BASE, 3, 0, 0, CLOCK_MONOTONIC_);
        futex_test_fail_allocation_at(SIZE_MAX);
        CHECK(returned(result, _ENOMEM) &&
                futex_test_live_count() == baseline,
                "入口及中途 OOM 均回滚全部 futex 对象");
        if (failures[index] == 0)
            CHECK(probe.read_calls == 0,
                    "waiter 向量分配失败发生在逐项 copy 前");
        for (size_t word = 0; word < 3; word++)
            CHECK(wake_word(task, SHARED_WORD_BASE + word * 4,
                            false, 1) == 0,
                    "OOM 后没有残留队列节点");
    }
    return true;
}

int main(void) {
    signal(SIGUSR1, SIG_IGN);
    alarm(30);
    struct fixture fixture;
    bool passed = init_fixture(&fixture);
    if (!passed) {
        fprintf(stderr, "AArch64 futex_waitv 测试失败：建立真实进程夹具\n");
        return 1;
    }
    passed = test_dispatch_and_copy(&fixture) &&
            test_two_phase_order(&fixture) &&
            test_private_shared_and_signal(&fixture) &&
            test_maximum_and_duplicates(&fixture) &&
            test_timeouts_and_oom(&fixture);
    unsigned live_futexes = futex_test_live_count();
    destroy_fixture(&fixture);
    alarm(0);
    if (live_futexes != 0) {
        fprintf(stderr, "AArch64 futex_waitv 测试遗留 %u 个 futex 对象\n",
                live_futexes);
        passed = false;
    }
    return passed ? 0 : 1;
}

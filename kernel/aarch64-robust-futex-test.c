#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-futex-abi.h"
#include "guest/aarch64/linux-process.h"
#include "guest/linux/mman.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-file-mapping-service.h"
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

#define IMAGE_SIZE 1024
#define TEXT_BASE UINT64_C(0x400000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define ELF_FILE_IDENTITY UINT64_C(0x524f42555354454c)
#define ROBUST_BASE UINT64_C(0x10000000)
#define ROBUST_PAGES UINT64_C(10)
#define READONLY_ROBUST_BASE \
    (ROBUST_BASE + ROBUST_PAGES * GUEST_MEMORY_PAGE_SIZE)
#define INVALID_ADDRESS (UINT64_C(1) << 48)

#define AARCH64_LINUX_SYS_FUTEX 98
#define AARCH64_LINUX_SYS_SET_ROBUST_LIST 99
#define AARCH64_LINUX_SYS_GET_ROBUST_LIST 100
#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 robust futex 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct robust_fixture {
    struct task *parent;
    struct tgroup group;
    size_t readonly_steps;
    size_t restore_steps;
};

struct robust_waiter {
    pthread_t thread;
    struct task *task;
    qword_t address;
    dword_t value;
    atomic_uint stage;
    dword_t result;
};

struct user_probe {
    unsigned reads;
    unsigned writes;
    unsigned fail_write_at;
    qword_t addresses[4];
    qword_t values[4];
    dword_t sizes[4];
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
        dword_t immediate =
                (dword_t) (value >> (half * 16)) & UINT32_C(0xffff);
        if (immediate != 0) {
            program[(*count)++] = UINT32_C(0xf2800000) |
                    (half << 21) | (immediate << 5) | reg;
        }
    }
}

static void append_mmap(dword_t *program, size_t *count,
        qword_t address, qword_t length,
        qword_t protection, qword_t flags) {
    append_move(program, count, 0, address);
    append_move(program, count, 1, length);
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

static size_t make_image(byte_t file[IMAGE_SIZE], size_t *setup_steps,
        size_t *readonly_steps) {
    dword_t program[64];
    size_t instruction_count = 0;
    append_mmap(program, &instruction_count, ROBUST_BASE,
            ROBUST_PAGES * GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_mmap(program, &instruction_count, READONLY_ROBUST_BASE,
            GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    *setup_steps = instruction_count;
    append_mprotect(program, &instruction_count,
            READONLY_ROBUST_BASE, GUEST_LINUX_PROT_READ);
    *readonly_steps = instruction_count - *setup_steps;
    append_mprotect(program, &instruction_count,
            READONLY_ROBUST_BASE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE);

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
    put_u16(file + 56, 2);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size = 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, IMAGE_SIZE,
            IMAGE_SIZE, GUEST_MEMORY_PAGE_SIZE);
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
        size_t *readonly_steps, size_t *restore_steps) {
    byte_t file[IMAGE_SIZE];
    size_t total_steps = make_image(
            file, setup_steps, readonly_steps);
    *restore_steps = total_steps - *setup_steps - *readonly_steps;
    struct guest_file_source *file_source = guest_file_source_create(
            ELF_FILE_IDENTITY, NULL, NULL);
    if (file_source == NULL)
        return NULL;
    const char *arguments[] = {"robust-futex-test"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .elf_file_source = file_source,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x800000),
        .executable = "/bin/robust-futex-test",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .random = random,
        .tid = task->pid,
        .task_opaque = task,
        .syscalls = &ish_aarch64_linux_syscall_service,
        .signals = &ish_aarch64_linux_signal_service,
        .file_mappings = &ish_aarch64_linux_file_mapping_service,
    };
    struct aarch64_linux_process *process =
            aarch64_linux_process_create(&config, NULL);
    guest_file_source_release(file_source);
    return process;
}

static bool init_fixture(struct robust_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->parent = make_parent(&fixture->group);
    if (fixture->parent == NULL)
        return false;
    size_t setup_steps;
    struct aarch64_linux_process *process =
            make_process(fixture->parent, &setup_steps,
                    &fixture->readonly_steps,
                    &fixture->restore_steps);
    if (process == NULL)
        return false;
    for (size_t step = 0; step < setup_steps; step++) {
        if (aarch64_linux_process_run_one(process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    return task_attach_aarch64_process(fixture->parent, process);
}

static bool protect_readonly_robust_page(
        struct robust_fixture *fixture) {
    for (size_t step = 0; step < fixture->readonly_steps; step++) {
        if (aarch64_linux_process_run_one(
                fixture->parent->aarch64_process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    fixture->readonly_steps = 0;
    return true;
}

static bool restore_writable_robust_page(
        struct robust_fixture *fixture) {
    for (size_t step = 0; step < fixture->restore_steps; step++) {
        if (aarch64_linux_process_run_one(
                fixture->parent->aarch64_process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    fixture->restore_steps = 0;
    return true;
}

static void destroy_fixture(struct robust_fixture *fixture) {
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

static struct task *make_thread(struct task *parent) {
    struct task *task = task_create_(parent);
    if (task == NULL)
        return NULL;
    const struct aarch64_linux_process_thread_config config = {
        .tid = task->pid,
        .task_opaque = task,
    };
    struct aarch64_linux_process *process =
            aarch64_linux_process_clone_thread(
                    parent->aarch64_process, &config, NULL);
    if (process == NULL || !task_attach_aarch64_process(task, process)) {
        aarch64_linux_process_destroy(process);
        task_abort_create(task);
        return NULL;
    }
    return task;
}

static bool configure_clear_child_tid(
        struct robust_fixture *fixture, qword_t address) {
    struct aarch64_linux_process *old =
            fixture->parent->aarch64_process;
    const struct aarch64_linux_process_thread_config config = {
        .tid = fixture->parent->pid,
        .task_opaque = fixture->parent,
        .clear_child_tid = address,
    };
    struct aarch64_linux_process *replacement =
            aarch64_linux_process_clone_thread(old, &config, NULL);
    if (replacement == NULL)
        return false;
    if (task_take_aarch64_process(fixture->parent) != old)
        abort();
    if (!task_attach_aarch64_process(
            fixture->parent, replacement)) {
        if (!task_attach_aarch64_process(fixture->parent, old))
            abort();
        aarch64_linux_process_destroy(replacement);
        return false;
    }
    aarch64_linux_process_destroy(old);
    return true;
}

static void destroy_thread(struct task *parent, struct task *task) {
    current = parent;
    signal_flush_pending(task);
    task_abort_create(task);
}

static bool write_u32(struct robust_fixture *fixture,
        qword_t address, dword_t value) {
    return aarch64_linux_process_write_u32(
            fixture->parent->aarch64_process,
            address, value, NULL);
}

static bool read_u32(struct robust_fixture *fixture,
        qword_t address, dword_t *value) {
    return aarch64_linux_process_read_u32(
            fixture->parent->aarch64_process,
            address, value, NULL);
}

static bool write_u64(struct robust_fixture *fixture,
        qword_t address, qword_t value) {
    return write_u32(fixture, address, (dword_t) value) &&
            write_u32(fixture, address + 4, (dword_t) (value >> 32));
}

static bool write_head(struct robust_fixture *fixture,
        qword_t head_address, qword_t first,
        sqword_t offset, qword_t pending) {
    return write_u64(fixture, head_address, first) &&
            write_u64(fixture, head_address + 8, (qword_t) offset) &&
            write_u64(fixture, head_address + 16, pending);
}

static dword_t call_futex(struct task *task, qword_t address,
        dword_t operation, dword_t value,
        qword_t timeout_or_value2, qword_t second_address) {
    struct task *saved = current;
    current = task;
    dword_t result = sys_futex_aarch64(address, operation, value,
            timeout_or_value2, second_address, 0, NULL);
    current = saved;
    return result;
}

static void *waiter_main(void *opaque) {
    struct robust_waiter *waiter = opaque;
    current = waiter->task;
    task_thread_store(waiter->task, pthread_self());
    atomic_store_explicit(&waiter->stage, 1, memory_order_release);
    waiter->result = sys_futex_aarch64(waiter->address,
            FUTEX_WAIT_, waiter->value, 0, 0, 0, NULL);
    atomic_store_explicit(&waiter->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool start_waiter(struct robust_waiter *waiter,
        struct task *task, qword_t address, dword_t value) {
    *waiter = (struct robust_waiter) {
        .task = task,
        .address = address,
        .value = value,
    };
    atomic_init(&waiter->stage, 0);
    if (pthread_create(&waiter->thread, NULL,
            waiter_main, waiter) != 0)
        return false;
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (atomic_load_explicit(
                &waiter->stage, memory_order_acquire) != 0)
            return true;
        sched_yield();
    }
    return false;
}

static bool wait_until_queued(struct task *observer,
        qword_t address) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        dword_t result = call_futex(observer, address,
                FUTEX_REQUEUE_, 0, 1, address);
        if (result == 1)
            return true;
        if ((sdword_t) result < 0 || result > 1)
            return false;
        sched_yield();
    }
    return false;
}

static unsigned waiter_stage(const struct robust_waiter *waiter) {
    return atomic_load_explicit(
            &waiter->stage, memory_order_acquire);
}

static bool wait_for_stage(
        const struct robust_waiter *waiter, unsigned expected) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (waiter_stage(waiter) == expected)
            return true;
        sched_yield();
    }
    return false;
}

static bool wait_for_exactly_one(
        const struct robust_waiter waiters[2]) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        unsigned completed =
                (waiter_stage(&waiters[0]) == 2) +
                (waiter_stage(&waiters[1]) == 2);
        if (completed != 0)
            return completed == 1;
        sched_yield();
    }
    return false;
}

static bool join_waiter(struct robust_waiter *waiter) {
    return pthread_join(waiter->thread, NULL) == 0 &&
            waiter->result == 0;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) destination;
    (void) size;
    (void) fault;
    struct user_probe *probe = opaque;
    probe->reads++;
    return false;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    unsigned index = probe->writes++;
    if (index < array_size(probe->addresses)) {
        probe->addresses[index] = address;
        probe->sizes[index] = size;
        if (size == sizeof(qword_t))
            memcpy(&probe->values[index], source, sizeof(qword_t));
    }
    if (index != probe->fail_write_at)
        return true;
    if (fault != NULL) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
    }
    return false;
}

static qword_t invoke_robust_syscall(struct robust_fixture *fixture,
        struct user_probe *probe, qword_t number,
        qword_t argument0, qword_t argument1, qword_t argument2,
        struct guest_linux_user_fault *fault) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = fixture->parent,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {argument0, argument1, argument2},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static qword_t syscall_error(int_t error) {
    return (qword_t) (sqword_t) error;
}

static bool test_registration_syscalls(void) {
    struct robust_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 robust syscall 夹具");
    const qword_t registered = UINT64_C(0xfffffffffffffff0);
    struct user_probe probe = {.fail_write_at = UINT_MAX};
    struct guest_linux_user_fault fault = {0};

    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_SET_ROBUST_LIST,
            registered, sizeof(struct aarch64_linux_robust_list_head),
            0, &fault) == 0 &&
            probe.reads == 0 && probe.writes == 0,
            "set_robust_list 接受完整 64 位指针且不探测用户内存");

    qword_t observed = 0;
    TEST_CHECK(sys_get_robust_list_aarch64(
            0, &observed) == 0 && observed == registered,
            "pid 0 查询当前任务的完整 64 位注册");
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_SET_ROBUST_LIST,
            ROBUST_BASE, 23, 0, &fault) == syscall_error(_EINVAL) &&
            sys_get_robust_list_aarch64(0, &observed) == 0 &&
            observed == registered,
            "错误长度返回 EINVAL 且不覆盖旧注册");
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_SET_ROBUST_LIST,
            ROBUST_BASE, 25, 0, &fault) == syscall_error(_EINVAL) &&
            probe.reads == 0 && probe.writes == 0,
            "set_robust_list 只接受精确 LP64 结构长度");

    probe = (struct user_probe) {.fail_write_at = UINT_MAX};
    fault = (struct guest_linux_user_fault) {0};
    const qword_t head_output = UINT64_C(0x2000);
    const qword_t length_output = UINT64_C(0x3000);
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_GET_ROBUST_LIST,
            UINT64_C(0xdeadbeef00000000) |
                    (dword_t) fixture.parent->pid,
            head_output, length_output, &fault) == 0 &&
            probe.reads == 0 && probe.writes == 2 &&
            probe.addresses[0] == length_output &&
            probe.values[0] ==
                    sizeof(struct aarch64_linux_robust_list_head) &&
            probe.sizes[0] == sizeof(qword_t) &&
            probe.addresses[1] == head_output &&
            probe.values[1] == registered &&
            probe.sizes[1] == sizeof(qword_t),
            "get_robust_list 先写长度再写头指针");

    probe = (struct user_probe) {.fail_write_at = UINT_MAX};
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_GET_ROBUST_LIST,
            0, head_output, INVALID_ADDRESS, &fault) ==
                    syscall_error(_EFAULT) &&
            probe.writes == 0 && fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "长度地址越界时不产生部分输出");

    probe = (struct user_probe) {.fail_write_at = UINT_MAX};
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_GET_ROBUST_LIST,
            0, INVALID_ADDRESS, length_output, &fault) ==
                    syscall_error(_EFAULT) &&
            probe.writes == 1 &&
            probe.addresses[0] == length_output &&
            probe.values[0] ==
                    sizeof(struct aarch64_linux_robust_list_head) &&
            fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "头指针地址越界时保留 Linux 的先写长度副作用");

    probe = (struct user_probe) {.fail_write_at = 0};
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_GET_ROBUST_LIST,
            0, head_output, length_output, &fault) ==
                    syscall_error(_EFAULT) &&
            probe.writes == 1 && probe.addresses[0] == length_output &&
            fault.address == length_output,
            "长度写故障立即返回 EFAULT");

    probe = (struct user_probe) {.fail_write_at = 1};
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_GET_ROBUST_LIST,
            0, head_output, length_output, &fault) ==
                    syscall_error(_EFAULT) &&
            probe.writes == 2 &&
            probe.addresses[0] == length_output &&
            probe.addresses[1] == head_output &&
            fault.address == head_output,
            "头指针写故障发生在长度成功写出之后");

    probe = (struct user_probe) {.fail_write_at = UINT_MAX};
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_GET_ROBUST_LIST,
            MAX_PID + 1, head_output, length_output, &fault) ==
                    syscall_error(_ESRCH) && probe.writes == 0,
            "不存在的目标任务返回 ESRCH 且不写输出");
    struct task *child = task_create_(fixture.parent);
    TEST_CHECK(child != NULL && child->aarch64_robust_list == 0 &&
            fixture.parent->aarch64_robust_list == registered,
            "新任务不继承父任务的 robust 注册");
    child->uid = child->euid = child->suid = 1001;
    child->gid = child->egid = child->sgid = 2001;
    current = child;
    TEST_CHECK(sys_get_robust_list_aarch64(
            fixture.parent->pid, &observed) == 0 &&
            observed == registered,
            "同一线程组可按精确 TID 查询 robust 注册");
    current = fixture.parent;
    task_abort_create(child);

    probe = (struct user_probe) {.fail_write_at = UINT_MAX};
    TEST_CHECK(invoke_robust_syscall(&fixture, &probe,
            AARCH64_LINUX_SYS_SET_ROBUST_LIST,
            0, sizeof(struct aarch64_linux_robust_list_head),
            0, &fault) == 0 &&
            sys_get_robust_list_aarch64(0, &observed) == 0 &&
            observed == 0,
            "NULL 头指针显式清除 robust 注册");

    destroy_fixture(&fixture);
    return true;
}

static bool test_listed_pending_and_pi_cleanup(void) {
    struct robust_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 robust 清理夹具");
    const qword_t head = ROBUST_BASE;
    const qword_t listed = ROBUST_BASE + UINT64_C(0x40);
    const qword_t pi_entry = ROBUST_BASE + UINT64_C(0x80);
    const qword_t pending = ROBUST_BASE + UINT64_C(0xc0);
    const qword_t mismatch = ROBUST_BASE + UINT64_C(0x100);
    const sqword_t offset = 8;
    const dword_t owner = (dword_t) fixture.parent->pid;
    const dword_t listed_value =
            owner | AARCH64_LINUX_FUTEX_WAITERS;
    const dword_t pi_value = listed_value;
    const dword_t pending_value = AARCH64_LINUX_FUTEX_WAITERS;
    const dword_t mismatch_value =
            ((owner + 7) & AARCH64_LINUX_FUTEX_TID_MASK) |
            AARCH64_LINUX_FUTEX_WAITERS;

    TEST_CHECK(write_head(&fixture, head, listed, offset, pending) &&
            write_u64(&fixture, listed,
                    pi_entry | AARCH64_LINUX_ROBUST_LIST_PI) &&
            write_u64(&fixture, pi_entry, pending) &&
            write_u64(&fixture, pending, mismatch) &&
            write_u64(&fixture, mismatch, head) &&
            write_u32(&fixture, listed + offset, listed_value) &&
            write_u32(&fixture, pi_entry + offset, pi_value) &&
            write_u32(&fixture, pending + offset, pending_value) &&
            write_u32(&fixture, mismatch + offset, mismatch_value),
            "准备普通、PI、pending 与 owner mismatch 节点");

    struct task *tasks[5];
    struct robust_waiter waiters[5];
    const qword_t addresses[5] = {
        listed + offset,
        listed + offset,
        pi_entry + offset,
        pending + offset,
        mismatch + offset,
    };
    const dword_t values[5] = {
        listed_value, listed_value, pi_value,
        pending_value, mismatch_value,
    };
    for (size_t index = 0; index < array_size(tasks); index++) {
        tasks[index] = make_thread(fixture.parent);
        TEST_CHECK(tasks[index] != NULL &&
                tasks[index]->aarch64_robust_list == 0 &&
                start_waiter(&waiters[index], tasks[index],
                        addresses[index], values[index]) &&
                wait_until_queued(fixture.parent, addresses[index]),
                "启动并确认 robust futex 等待者入队");
    }

    TEST_CHECK(sys_set_robust_list_aarch64(
            head, sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "注册 robust 链表");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);

    dword_t listed_after;
    dword_t pi_after;
    dword_t pending_after;
    dword_t mismatch_after;
    qword_t registration = UINT64_MAX;
    TEST_CHECK(read_u32(&fixture, listed + offset, &listed_after) &&
            read_u32(&fixture, pi_entry + offset, &pi_after) &&
            read_u32(&fixture, pending + offset, &pending_after) &&
            read_u32(&fixture, mismatch + offset, &mismatch_after) &&
            listed_after == (AARCH64_LINUX_FUTEX_WAITERS |
                    AARCH64_LINUX_FUTEX_OWNER_DIED) &&
            pi_after == (AARCH64_LINUX_FUTEX_WAITERS |
                    AARCH64_LINUX_FUTEX_OWNER_DIED) &&
            pending_after == pending_value &&
            mismatch_after == mismatch_value &&
            sys_get_robust_list_aarch64(0, &registration) == 0 &&
            registration == 0,
            "清理按 owner、PI 与 pending 规则更新 futex 并取走注册");
    TEST_CHECK(wait_for_exactly_one(waiters) &&
            wait_for_stage(&waiters[3], 2) &&
            waiter_stage(&waiters[2]) == 1 &&
            waiter_stage(&waiters[4]) == 1,
            "普通 futex 只唤醒一个，pending 唤醒且 PI 与 owner mismatch 不误唤醒");
    TEST_CHECK(call_futex(fixture.parent, addresses[0],
            FUTEX_WAKE_, 1, 0, 0) == 1 &&
            call_futex(fixture.parent, addresses[2],
                    FUTEX_WAKE_, 1, 0, 0) == 1 &&
            call_futex(fixture.parent, addresses[4],
                    FUTEX_WAKE_, 1, 0, 0) == 1,
            "手动回收未被 robust 规则唤醒的等待者");
    for (size_t index = 0; index < array_size(waiters); index++) {
        TEST_CHECK(join_waiter(&waiters[index]),
                "等待者以成功结果退出");
        destroy_thread(fixture.parent, tasks[index]);
    }

    destroy_fixture(&fixture);
    return true;
}

static bool test_readonly_private_cleanup_continues(void) {
    struct robust_fixture fixture;
    TEST_CHECK(init_fixture(&fixture),
            "建立只读匿名私有 robust 清理夹具");
    const qword_t head = ROBUST_BASE + UINT64_C(0x600);
    const qword_t owned = ROBUST_BASE + UINT64_C(0x680);
    const qword_t mismatch = READONLY_ROBUST_BASE + UINT64_C(0x100);
    const qword_t pending = READONLY_ROBUST_BASE + UINT64_C(0x180);
    const sqword_t offset = 8;
    const dword_t owner = (dword_t) fixture.parent->pid;
    const dword_t mismatch_value =
            ((owner + 7) & AARCH64_LINUX_FUTEX_TID_MASK) |
            AARCH64_LINUX_FUTEX_WAITERS;
    const dword_t owned_value = owner | AARCH64_LINUX_FUTEX_WAITERS;
    const dword_t pending_value = AARCH64_LINUX_FUTEX_WAITERS;
    TEST_CHECK(write_head(&fixture, head, mismatch, offset, pending) &&
            write_u64(&fixture, mismatch, owned) &&
            write_u64(&fixture, owned, head) &&
            write_u32(&fixture, mismatch + offset, mismatch_value) &&
            write_u32(&fixture, owned + offset, owned_value) &&
            write_u32(&fixture, pending + offset, pending_value),
            "准备 owner mismatch、后续 owned 与 pending 节点");

    struct task *waiter_task = make_thread(fixture.parent);
    struct robust_waiter waiter;
    TEST_CHECK(waiter_task != NULL &&
            start_waiter(&waiter, waiter_task,
                    pending + offset, pending_value) &&
            wait_until_queued(fixture.parent, pending + offset) &&
            protect_readonly_robust_page(&fixture) &&
            sys_set_robust_list_aarch64(head,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "等待者入队后把匿名私有页降为只读并注册 robust 头");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);

    dword_t mismatch_after;
    dword_t owned_after;
    dword_t pending_after;
    qword_t registration = UINT64_MAX;
    TEST_CHECK(read_u32(&fixture, mismatch + offset, &mismatch_after) &&
            read_u32(&fixture, owned + offset, &owned_after) &&
            read_u32(&fixture, pending + offset, &pending_after) &&
            mismatch_after == mismatch_value &&
            owned_after == (AARCH64_LINUX_FUTEX_WAITERS |
                    AARCH64_LINUX_FUTEX_OWNER_DIED) &&
            pending_after == pending_value &&
            sys_get_robust_list_aarch64(0, &registration) == 0 &&
            registration == 0,
            "只读 owner mismatch 不终止遍历且 pending 不被错误修改");
    TEST_CHECK(call_futex(fixture.parent, pending + offset,
                    FUTEX_WAKE_ | GUEST_LINUX_FUTEX_PRIVATE_FLAG,
                    1, 0, 0) == 0 &&
            waiter_stage(&waiter) == 1 &&
            restore_writable_robust_page(&fixture) &&
            call_futex(fixture.parent, pending + offset,
                    FUTEX_WAKE_, 1, 0, 0) == 1 &&
            join_waiter(&waiter),
            "只读 pending 不误唤醒且恢复写权限后由 mm-shared 键回收");
    destroy_thread(fixture.parent, waiter_task);

    destroy_fixture(&fixture);
    return true;
}

static bool test_negative_offset_and_fault_boundaries(void) {
    struct robust_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 robust 边界夹具");
    const qword_t head = ROBUST_BASE + UINT64_C(0x200);
    const qword_t entry = ROBUST_BASE + UINT64_C(0x280);
    const qword_t futex = entry - 8;
    const dword_t owner = (dword_t) fixture.parent->pid;

    TEST_CHECK(write_head(&fixture, head, entry, -8, 0) &&
            write_u64(&fixture, entry, head) &&
            write_u32(&fixture, futex, owner) &&
            sys_set_robust_list_aarch64(
                    head,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "准备负 futex_offset 节点");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);
    dword_t after;
    TEST_CHECK(read_u32(&fixture, futex, &after) &&
            after == AARCH64_LINUX_FUTEX_OWNER_DIED,
            "负 futex_offset 正确定位并修复 owner");

    const qword_t malformed_head = ROBUST_BASE + UINT64_C(0x300);
    const qword_t malformed_entry = ROBUST_BASE + UINT64_C(0x380);
    const qword_t pending = ROBUST_BASE + UINT64_C(0x400);
    TEST_CHECK(write_head(&fixture, malformed_head,
                    malformed_entry, INT64_MIN, pending) &&
            write_u64(&fixture, malformed_entry, malformed_head) &&
            write_u32(&fixture, pending, owner) &&
            sys_set_robust_list_aarch64(malformed_head,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "准备 offset 下溢与 pending 边界");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);
    TEST_CHECK(read_u32(&fixture, pending, &after) && after == owner,
            "主链故障静默终止且不越过故障处理 pending");

    const qword_t fault_head = ROBUST_BASE + UINT64_C(0x480);
    const qword_t fault_entry = ROBUST_BASE +
            ROBUST_PAGES * GUEST_MEMORY_PAGE_SIZE - 4;
    const qword_t fault_futex = fault_entry - 4;
    const qword_t skipped_pending = ROBUST_BASE + UINT64_C(0x500);
    TEST_CHECK(write_head(&fixture, fault_head,
                    fault_entry, -4, skipped_pending) &&
            write_u32(&fixture, fault_futex, owner) &&
            write_u32(&fixture, skipped_pending - 4, owner) &&
            sys_set_robust_list_aarch64(fault_head,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "准备 next 跨越未映射页的当前节点");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);
    dword_t pending_after;
    TEST_CHECK(read_u32(&fixture, fault_futex, &after) &&
            read_u32(&fixture, skipped_pending - 4, &pending_after) &&
            after == AARCH64_LINUX_FUTEX_OWNER_DIED &&
            pending_after == owner,
            "next 读取故障前仍修复当前节点，随后停止且不处理 pending");

    TEST_CHECK(sys_set_robust_list_aarch64(
            INVALID_ADDRESS,
            sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "允许注册尚未探测的坏地址");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);
    qword_t registration = UINT64_MAX;
    TEST_CHECK(sys_get_robust_list_aarch64(
            0, &registration) == 0 && registration == 0,
            "坏头指针退出时静默丢弃且注册只消费一次");

    destroy_fixture(&fixture);
    return true;
}

static bool test_traversal_limit(void) {
    struct robust_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 robust 遍历上限夹具");
    const qword_t head = ROBUST_BASE;
    const qword_t first = ROBUST_BASE + UINT64_C(0x100);
    const qword_t stride = 16;
    const sqword_t offset = 8;
    const dword_t owner = (dword_t) fixture.parent->pid;
    const qword_t pending = first +
            (AARCH64_LINUX_ROBUST_LIST_LIMIT + 2) * stride;

    TEST_CHECK(write_head(&fixture, head, first, offset, pending),
            "写入遍历上限测试头");
    for (dword_t index = 0;
            index <= AARCH64_LINUX_ROBUST_LIST_LIMIT; index++) {
        qword_t entry = first + (qword_t) index * stride;
        qword_t next = index == AARCH64_LINUX_ROBUST_LIST_LIMIT ?
                head : entry + stride;
        TEST_CHECK(write_u64(&fixture, entry, next) &&
                write_u32(&fixture, entry + offset, owner),
                "构造超出 Linux robust 遍历上限的链表");
    }
    TEST_CHECK(write_u32(&fixture, pending + offset, owner) &&
            sys_set_robust_list_aarch64(
                    head,
                    sizeof(struct aarch64_linux_robust_list_head)) == 0,
            "准备遍历上限后的 pending 节点");
    futex_cleanup_robust_list_aarch64(
            fixture.parent, fixture.parent->aarch64_process);

    dword_t first_after;
    dword_t last_processed_after;
    dword_t capped_after;
    dword_t pending_after;
    qword_t last_processed = first +
            (AARCH64_LINUX_ROBUST_LIST_LIMIT - 1) * stride;
    qword_t capped = first +
            (qword_t) AARCH64_LINUX_ROBUST_LIST_LIMIT * stride;
    TEST_CHECK(read_u32(&fixture, first + offset, &first_after) &&
            read_u32(&fixture, last_processed + offset,
                    &last_processed_after) &&
            read_u32(&fixture, capped + offset, &capped_after) &&
            read_u32(&fixture, pending + offset, &pending_after) &&
            first_after == AARCH64_LINUX_FUTEX_OWNER_DIED &&
            last_processed_after == AARCH64_LINUX_FUTEX_OWNER_DIED &&
            capped_after == owner &&
            pending_after == AARCH64_LINUX_FUTEX_OWNER_DIED,
            "最多修复 2048 个主链节点并仍单独处理 pending");

    destroy_fixture(&fixture);
    return true;
}

static bool test_clear_child_tid_release(void) {
    struct robust_fixture fixture;
    TEST_CHECK(init_fixture(&fixture),
            "建立 clear-child-tid 清理夹具");
    const qword_t writable = ROBUST_BASE + UINT64_C(0x700);
    const dword_t marker = UINT32_C(0x6a09e667);
    TEST_CHECK(write_u32(&fixture, writable, marker) &&
            configure_clear_child_tid(&fixture, writable),
            "登记单独持有地址空间的 clear-child-tid");
    const struct aarch64_linux_process_thread_config observer_config = {
        .tid = fixture.parent->pid + 1,
        .task_opaque = fixture.parent,
    };
    struct aarch64_linux_process *observer =
            aarch64_linux_process_clone_thread(
                    fixture.parent->aarch64_process,
                    &observer_config, NULL);
    TEST_CHECK(observer != NULL,
            "建立不持有 metadata mm 的地址空间观察者");
    futex_cleanup_task_aarch64(
            fixture.parent, fixture.parent->aarch64_process);
    dword_t observed;
    TEST_CHECK(read_u32(&fixture, writable, &observed) &&
            observed == marker &&
            aarch64_linux_process_take_clear_child_tid(
                    fixture.parent->aarch64_process) == 0,
            "opaque 观察者不冒充 mm 用户触发清零");
    aarch64_linux_process_destroy(observer);

    const qword_t readonly = TEXT_BASE + ENTRY_OFFSET;
    TEST_CHECK(read_u32(&fixture, readonly, &observed) &&
            configure_clear_child_tid(&fixture, readonly),
            "把 clear-child-tid 指向只读代码页");
    const dword_t instruction = observed;
    struct task *waiter_task = make_thread(fixture.parent);
    struct robust_waiter waiter;
    TEST_CHECK(waiter_task != NULL,
            "创建共享旧 mm 的 clear-child-tid 等待任务");
    mm_retain(fixture.parent->mm);
    TEST_CHECK(start_waiter(
                    &waiter, waiter_task, readonly, instruction) &&
            wait_until_queued(fixture.parent, readonly),
            "在共享旧 mm 的只读地址上排入等待者");
    futex_cleanup_task_aarch64(
            fixture.parent, fixture.parent->aarch64_process);
    TEST_CHECK(wait_for_stage(&waiter, 2) && join_waiter(&waiter) &&
            read_u32(&fixture, readonly, &observed) &&
            observed == instruction &&
            aarch64_linux_process_take_clear_child_tid(
                    fixture.parent->aarch64_process) == 0,
            "清零故障后仍以共享键唤醒且静默消费注册");
    destroy_thread(fixture.parent, waiter_task);
    mm_release(fixture.parent->mm);
    destroy_fixture(&fixture);
    return true;
}

typedef bool (*scenario_function)(void);

static bool run_isolated(const char *name, scenario_function scenario) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "创建 %s 隔离进程失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (child == 0) {
        alarm(20);
        bool passed = scenario();
        alarm(0);
        exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        fprintf(stderr, "等待 %s 隔离进程失败：%s\n",
                name, strerror(errno));
        return false;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s 被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s 返回状态 %d\n", name,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

int main(void) {
    const struct {
        const char *name;
        scenario_function function;
    } scenarios[] = {
        {"AArch64 robust 注册系统调用", test_registration_syscalls},
        {"AArch64 robust 链表与 pending 清理",
                test_listed_pending_and_pi_cleanup},
        {"AArch64 robust 只读匿名私有页继续遍历",
                test_readonly_private_cleanup_continues},
        {"AArch64 robust 负偏移与故障边界",
                test_negative_offset_and_fault_boundaries},
        {"AArch64 robust 遍历上限", test_traversal_limit},
        {"AArch64 clear-child-tid 释放", test_clear_child_tid_release},
    };
    for (size_t index = 0; index < array_size(scenarios); index++) {
        if (!run_isolated(
                scenarios[index].name, scenarios[index].function))
            return 1;
    }
    return 0;
}

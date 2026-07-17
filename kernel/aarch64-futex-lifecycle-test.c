#include <errno.h>
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
#include "guest/memory/page-backing.h"
#include "kernel/aarch64-file-mapping-service.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/futex.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define IMAGE_SIZE 2048
#define TEXT_BASE UINT64_C(0x400000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define ELF_FILE_IDENTITY UINT64_C(0x46555445584c4946)
#define STATUS_BASE UINT64_C(0x10000000)
#define TRANSIENT_BASE (STATUS_BASE + GUEST_MEMORY_PAGE_SIZE)
#define CHILD_STACK (STACK_TOP - UINT64_C(0x400))

#define ROBUST_HEAD (STATUS_BASE + UINT64_C(0x40))
#define ROBUST_LISTED (STATUS_BASE + UINT64_C(0x80))
#define ROBUST_LISTED_WORD (ROBUST_LISTED + UINT64_C(8))
#define ROBUST_PENDING (STATUS_BASE + UINT64_C(0xc0))
#define ROBUST_PENDING_WORD (ROBUST_PENDING + UINT64_C(8))
#define CLONE_CHILD_TID (STATUS_BASE + UINT64_C(0x100))
#define RUNTIME_CHILD_TID (STATUS_BASE + UINT64_C(0x104))
#define PARENT_REQUEUE_RESULT (STATUS_BASE + UINT64_C(0x20))
#define PARENT_MUNMAP_RESULT (STATUS_BASE + UINT64_C(0x24))
#define PARENT_REMAP_RESULT (STATUS_BASE + UINT64_C(0x28))
#define PARENT_KILL_RESULT (STATUS_BASE + UINT64_C(0x30))
#define CHILD_ROBUST_RESULT (STATUS_BASE + UINT64_C(0x34))
#define CHILD_TID_RESULT (STATUS_BASE + UINT64_C(0x38))
#define REQUEUE_SOURCE (TRANSIENT_BASE + UINT64_C(0x40))
#define REQUEUE_TARGET (TRANSIENT_BASE + UINT64_C(0x44))

#define CLONE_VM_ UINT32_C(0x00000100)
#define CLONE_CHILD_CLEARTID_ UINT32_C(0x00200000)
#define CLONE_CHILD_SETTID_ UINT32_C(0x01000000)
#define TEST_CLONE_FLAGS (CLONE_VM_ | CLONE_CHILD_CLEARTID_ | \
        CLONE_CHILD_SETTID_ | SIGCHLD_)

#define AARCH64_SYS_SET_TID_ADDRESS 96
#define AARCH64_SYS_FUTEX 98
#define AARCH64_SYS_SET_ROBUST_LIST 99
#define AARCH64_SYS_KILL 129
#define AARCH64_SYS_MUNMAP 215
#define AARCH64_SYS_CLONE 220
#define AARCH64_SYS_MMAP 222

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 futex 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct lifecycle_fixture {
    struct task *parent;
    struct tgroup group;
};

struct lifecycle_waiter {
    pthread_t thread;
    struct task *task;
    qword_t address;
    dword_t expected;
    atomic_uint stage;
    dword_t result;
};

struct exit_observation {
    atomic_bool called;
    atomic_bool resources_released;
    atomic_int pid;
    atomic_int status;
};

static const struct fd_ops metadata_fd_ops = {0};
static struct exit_observation observed_exit;

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

static void append_svc(dword_t *program, size_t *count,
        qword_t syscall_number) {
    append_move(program, count, 8, syscall_number);
    program[(*count)++] = UINT32_C(0xd4000001);
}

static void append_store_w(dword_t *program, size_t *count,
        byte_t source, byte_t base, qword_t offset) {
    if ((offset & 3) != 0 || offset / 4 > UINT32_C(0xfff))
        abort();
    program[(*count)++] = UINT32_C(0xb9000000) |
            ((dword_t) (offset / 4) << 10) |
            ((dword_t) base << 5) | source;
}

static void append_store_x(dword_t *program, size_t *count,
        byte_t source, byte_t base, qword_t offset) {
    if ((offset & 7) != 0 || offset / 8 > UINT32_C(0xfff))
        abort();
    program[(*count)++] = UINT32_C(0xf9000000) |
            ((dword_t) (offset / 8) << 10) |
            ((dword_t) base << 5) | source;
}

static void append_mmap(dword_t *program, size_t *count,
        qword_t address) {
    append_move(program, count, 0, address);
    append_move(program, count, 1, GUEST_MEMORY_PAGE_SIZE);
    append_move(program, count, 2,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE);
    append_move(program, count, 3,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_move(program, count, 4, UINT64_MAX);
    append_move(program, count, 5, 0);
    append_svc(program, count, AARCH64_SYS_MMAP);
}

static void patch_cbz_x0(dword_t *program,
        size_t branch_index, size_t target_index) {
    size_t distance = target_index - branch_index;
    if (target_index <= branch_index || distance >= (1U << 18))
        abort();
    program[branch_index] = UINT32_C(0xb4000000) |
            ((dword_t) distance << 5);
}

static void make_image(byte_t file[IMAGE_SIZE]) {
    dword_t program[128];
    size_t count = 0;
    append_mmap(program, &count, STATUS_BASE);
    append_mmap(program, &count, TRANSIENT_BASE);
    append_move(program, &count, 9, STATUS_BASE);

    append_move(program, &count, 0, TEST_CLONE_FLAGS);
    append_move(program, &count, 1, CHILD_STACK);
    append_move(program, &count, 2, 0);
    append_move(program, &count, 3, 0);
    append_move(program, &count, 4, CLONE_CHILD_TID);
    append_svc(program, &count, AARCH64_SYS_CLONE);
    size_t child_branch = count++;

    // 父路径保留子 PID，由 guest 串起重排、重映射与终止生命周期。
    program[count++] = UINT32_C(0xaa0003f3); // mov x19, x0
    append_move(program, &count, 0, REQUEUE_SOURCE);
    append_move(program, &count, 1, FUTEX_REQUEUE_);
    append_move(program, &count, 2, 0);
    append_move(program, &count, 3, 1);
    append_move(program, &count, 4, REQUEUE_TARGET);
    append_move(program, &count, 5, 0);
    append_svc(program, &count, AARCH64_SYS_FUTEX);
    append_store_w(program, &count, 0, 9,
            PARENT_REQUEUE_RESULT - STATUS_BASE);
    append_move(program, &count, 0, TRANSIENT_BASE);
    append_move(program, &count, 1, GUEST_MEMORY_PAGE_SIZE);
    append_svc(program, &count, AARCH64_SYS_MUNMAP);
    append_store_w(program, &count, 0, 9,
            PARENT_MUNMAP_RESULT - STATUS_BASE);
    append_mmap(program, &count, TRANSIENT_BASE);
    append_store_x(program, &count, 0, 9,
            PARENT_REMAP_RESULT - STATUS_BASE);
    program[count++] = UINT32_C(0xaa1303e0); // mov x0, x19
    append_move(program, &count, 1, SIGKILL_);
    append_svc(program, &count, AARCH64_SYS_KILL);
    append_store_w(program, &count, 0, 9,
            PARENT_KILL_RESULT - STATUS_BASE);
    program[count++] = UINT32_C(0x14000000); // b .

    size_t child_path = count;
    append_move(program, &count, 0, ROBUST_HEAD);
    append_move(program, &count, 1,
            sizeof(struct aarch64_linux_robust_list_head));
    append_svc(program, &count, AARCH64_SYS_SET_ROBUST_LIST);
    append_store_w(program, &count, 0, 9,
            CHILD_ROBUST_RESULT - STATUS_BASE);
    append_move(program, &count, 0, RUNTIME_CHILD_TID);
    append_svc(program, &count, AARCH64_SYS_SET_TID_ADDRESS);
    append_store_w(program, &count, 0, 9,
            CHILD_TID_RESULT - STATUS_BASE);
    append_move(program, &count, 0, REQUEUE_SOURCE);
    append_move(program, &count, 1, FUTEX_WAIT_);
    append_move(program, &count, 2, 0);
    append_move(program, &count, 3, 0);
    append_move(program, &count, 4, 0);
    append_move(program, &count, 5, 0);
    append_svc(program, &count, AARCH64_SYS_FUTEX);
    program[count++] = UINT32_C(0x14000000); // b .
    patch_cbz_x0(program, child_branch, child_path);

    if (count > (IMAGE_SIZE - ENTRY_OFFSET) / sizeof(*program))
        abort();
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
    for (size_t index = 0; index < count; index++)
        put_u32(file + ENTRY_OFFSET + index * sizeof(*program),
                program[index]);
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

static struct aarch64_linux_process *make_process(struct task *task) {
    byte_t file[IMAGE_SIZE];
    make_image(file);
    struct guest_file_source *file_source = guest_file_source_create(
            ELF_FILE_IDENTITY, NULL, NULL);
    if (file_source == NULL)
        return NULL;
    const char *arguments[] = {"futex-lifecycle-test"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .elf_file_source = file_source,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x800000),
        .executable = "/bin/futex-lifecycle-test",
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

static bool init_fixture(struct lifecycle_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->parent = make_parent(&fixture->group);
    if (fixture->parent == NULL)
        return false;
    struct aarch64_linux_process *process =
            make_process(fixture->parent);
    return process != NULL &&
            task_attach_aarch64_process(fixture->parent, process);
}

static void destroy_fixture(struct lifecycle_fixture *fixture) {
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

static bool write_u32(struct task *task,
        qword_t address, dword_t value) {
    return aarch64_linux_process_write_u32(
            task->aarch64_process, address, value, NULL);
}

static bool read_u32(struct task *task,
        qword_t address, dword_t *value) {
    return aarch64_linux_process_read_u32(
            task->aarch64_process, address, value, NULL);
}

static bool read_u64(struct task *task,
        qword_t address, qword_t *value) {
    dword_t low;
    dword_t high;
    if (!read_u32(task, address, &low) ||
            !read_u32(task, address + 4, &high))
        return false;
    *value = (qword_t) low | ((qword_t) high << 32);
    return true;
}

static bool write_u64(struct task *task,
        qword_t address, qword_t value) {
    return write_u32(task, address, (dword_t) value) &&
            write_u32(task, address + 4, (dword_t) (value >> 32));
}

static bool snapshot_word(struct task *task, qword_t address,
        struct aarch64_linux_futex_word_snapshot *snapshot) {
    const qword_t addresses[] = {address};
    return aarch64_linux_process_snapshot_futex_words(
            task->aarch64_process, addresses, 1, true,
            snapshot, NULL, NULL);
}

static dword_t call_futex(struct task *task, qword_t address,
        dword_t operation, dword_t value,
        qword_t value2, qword_t second_address) {
    struct task *saved = current;
    current = task;
    dword_t result = sys_futex_aarch64(address, operation, value,
            value2, second_address, 0, NULL);
    current = saved;
    return result;
}

static bool wait_until_queued(
        struct task *observer, qword_t address) {
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

static struct task *make_waiter_task(struct task *parent) {
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

static void destroy_waiter_task(
        struct task *parent, struct task *task) {
    current = parent;
    signal_flush_pending(task);
    task_abort_create(task);
}

static void *waiter_main(void *opaque) {
    struct lifecycle_waiter *waiter = opaque;
    current = waiter->task;
    task_thread_store(waiter->task, pthread_self());
    atomic_store_explicit(&waiter->stage, 1, memory_order_release);
    waiter->result = sys_futex_aarch64(
            waiter->address, FUTEX_WAIT_, waiter->expected,
            0, 0, 0, NULL);
    atomic_store_explicit(&waiter->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool start_waiter(struct lifecycle_waiter *waiter,
        struct task *task, qword_t address, dword_t expected) {
    *waiter = (struct lifecycle_waiter) {
        .task = task,
        .address = address,
        .expected = expected,
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

static bool join_waiter(struct lifecycle_waiter *waiter) {
    return pthread_join(waiter->thread, NULL) == 0 &&
            waiter->result == 0;
}

static void reset_exit_observation(void) {
    atomic_store_explicit(
            &observed_exit.called, false, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.resources_released,
            false, memory_order_relaxed);
    atomic_store_explicit(
            &observed_exit.pid, 0, memory_order_relaxed);
    atomic_store_explicit(
            &observed_exit.status, 0, memory_order_relaxed);
}

static void observe_exit(struct task *task, int status) {
    bool resources_released = !task_has_aarch64_process(task) &&
            task->mm == NULL && task->files == NULL &&
            task->fs == NULL && task->sighand == NULL;
    atomic_store_explicit(
            &observed_exit.pid, task->pid, memory_order_relaxed);
    atomic_store_explicit(
            &observed_exit.status, status, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.resources_released,
            resources_released, memory_order_relaxed);
    atomic_store_explicit(
            &observed_exit.called, true, memory_order_release);
}

static bool exit_observation_matches(pid_t_ pid, int status) {
    return atomic_load_explicit(
                    &observed_exit.called, memory_order_acquire) &&
            atomic_load_explicit(&observed_exit.resources_released,
                    memory_order_relaxed) &&
            atomic_load_explicit(
                    &observed_exit.pid, memory_order_relaxed) == pid &&
            atomic_load_explicit(
                    &observed_exit.status, memory_order_relaxed) == status;
}

static bool run_until_child(struct task *parent, pid_t_ *pid_out) {
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        struct aarch64_task_event event =
                aarch64_task_run_one(parent);
        if (event.action != AARCH64_TASK_EVENT_CONTINUE)
            return false;
        lock(&pids_lock);
        unsigned long child_count = list_size(&parent->children);
        struct task *child = child_count == 1 ?
                list_first_entry(&parent->children,
                        struct task, siblings) : NULL;
        bool valid = child != NULL && child->parent == parent &&
                child->group != parent->group &&
                child->group->leader == child &&
                child->tgid == child->pid &&
                child->exit_signal == SIGCHLD_ &&
                child->mm == parent->mm &&
                child->files != parent->files &&
                child->fs != parent->fs &&
                child->sighand != parent->sighand &&
                task_has_aarch64_process(child) &&
                aarch64_linux_process_memory_identity(
                        child->aarch64_process) ==
                aarch64_linux_process_memory_identity(
                        parent->aarch64_process) &&
                atomic_load_explicit(
                        &child->start_ready, memory_order_acquire);
        pid_t_ pid = valid ? child->pid : 0;
        unlock(&pids_lock);
        if (child_count != 0 && !valid)
            return false;
        if (pid != 0) {
            *pid_out = pid;
            return true;
        }
    }
    return false;
}

static bool child_registered_robust_list(pid_t_ pid) {
    lock(&pids_lock);
    struct task *child = pid_get_task((dword_t) pid);
    bool registered = child != NULL &&
            child->aarch64_robust_list == ROBUST_HEAD;
    unlock(&pids_lock);
    return registered;
}

static bool run_until_u32_result(struct task *parent,
        qword_t address, dword_t sentinel, dword_t expected) {
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        dword_t value;
        if (!read_u32(parent, address, &value))
            return false;
        if (value != sentinel)
            return value == expected;
        if (aarch64_task_run_one(parent).action !=
                AARCH64_TASK_EVENT_CONTINUE)
            return false;
    }
    return false;
}

static bool run_until_u64_result(struct task *parent,
        qword_t address, qword_t sentinel, qword_t expected) {
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        qword_t value;
        if (!read_u64(parent, address, &value))
            return false;
        if (value != sentinel)
            return value == expected;
        if (aarch64_task_run_one(parent).action !=
                AARCH64_TASK_EVENT_CONTINUE)
            return false;
    }
    return false;
}

static bool run_until_mapping_state(struct task *parent,
        bool mapped,
        struct aarch64_linux_futex_word_snapshot *snapshot) {
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        struct aarch64_task_event event =
                aarch64_task_run_one(parent);
        if (event.action != AARCH64_TASK_EVENT_CONTINUE)
            return false;
        struct aarch64_linux_futex_word_snapshot observed;
        bool present = snapshot_word(parent, REQUEUE_TARGET, &observed);
        if (present == mapped) {
            if (present && snapshot != NULL)
                *snapshot = observed;
            return true;
        }
    }
    return false;
}

static bool run_until_child_exits(
        struct task *parent, pid_t_ child_pid) {
    extern void (*exit_hook)(struct task *task, int code);
    reset_exit_observation();
    exit_hook = observe_exit;
    bool exited = false;
    for (unsigned attempt = 0; attempt < 100000 && !exited; attempt++) {
        struct aarch64_task_event event =
                aarch64_task_run_one(parent);
        if (event.action != AARCH64_TASK_EVENT_CONTINUE)
            break;
        exited = atomic_load_explicit(
                &observed_exit.called, memory_order_acquire);
        if (!exited)
            sched_yield();
    }
    exit_hook = NULL;
    return exited &&
            exit_observation_matches(child_pid, SIGKILL_);
}

static bool test_shared_exit_lifecycle(void) {
    unsigned backing_baseline =
            guest_page_backing_test_live_count();
    unsigned futex_baseline = futex_test_live_count();
    struct lifecycle_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立生产生命周期夹具");
    pid_t_ child_pid;
    TEST_CHECK(run_until_child(fixture.parent, &child_pid),
            "真实 clone SVC 发布共享 mm 的独立子进程");
    TEST_CHECK(wait_until_queued(fixture.parent, REQUEUE_SOURCE) &&
            child_registered_robust_list(child_pid),
            "子进程依次完成 robust、set_tid_address 与 futex WAIT");
    dword_t child_robust_result;
    dword_t child_tid_result;
    TEST_CHECK(read_u32(fixture.parent,
                    CHILD_ROBUST_RESULT, &child_robust_result) &&
            read_u32(fixture.parent,
                    CHILD_TID_RESULT, &child_tid_result) &&
            child_robust_result == 0 &&
            child_tid_result == (dword_t) child_pid,
            "子进程 syscall 返回值与注册状态一致");

    dword_t listed_value = (dword_t) child_pid |
            AARCH64_LINUX_FUTEX_WAITERS;
    dword_t clone_tid;
    TEST_CHECK(write_u64(fixture.parent, ROBUST_HEAD, ROBUST_LISTED) &&
            write_u64(fixture.parent, ROBUST_HEAD + 8, 8) &&
            write_u64(fixture.parent,
                    ROBUST_HEAD + 16, ROBUST_PENDING) &&
            write_u64(fixture.parent, ROBUST_LISTED, ROBUST_HEAD) &&
            write_u32(fixture.parent,
                    ROBUST_LISTED_WORD, listed_value) &&
            write_u64(fixture.parent, ROBUST_PENDING, 0) &&
            write_u32(fixture.parent, ROBUST_PENDING_WORD, 0) &&
            write_u32(fixture.parent,
                    RUNTIME_CHILD_TID, (dword_t) child_pid) &&
            write_u32(fixture.parent,
                    PARENT_REQUEUE_RESULT, UINT32_MAX) &&
            write_u32(fixture.parent,
                    PARENT_MUNMAP_RESULT, UINT32_MAX) &&
            write_u64(fixture.parent,
                    PARENT_REMAP_RESULT, UINT64_MAX) &&
            write_u32(fixture.parent,
                    PARENT_KILL_RESULT, UINT32_MAX) &&
            read_u32(fixture.parent, CLONE_CHILD_TID, &clone_tid) &&
            clone_tid == (dword_t) child_pid,
            "准备 listed、pending 与覆盖后的 clear-child-tid 状态");

    const qword_t waiter_addresses[] = {
        ROBUST_LISTED_WORD,
        ROBUST_PENDING_WORD,
        RUNTIME_CHILD_TID,
    };
    const dword_t waiter_values[] = {
        listed_value,
        0,
        (dword_t) child_pid,
    };
    struct task *waiter_tasks[array_size(waiter_addresses)];
    struct lifecycle_waiter waiters[array_size(waiter_addresses)];
    for (size_t index = 0;
            index < array_size(waiter_addresses); index++) {
        waiter_tasks[index] = make_waiter_task(fixture.parent);
        TEST_CHECK(waiter_tasks[index] != NULL &&
                start_waiter(&waiters[index], waiter_tasks[index],
                        waiter_addresses[index], waiter_values[index]) &&
                wait_until_queued(
                        fixture.parent, waiter_addresses[index]),
                "把退出观察者排入稳定共享页");
    }

    unsigned mapped_backings =
            guest_page_backing_test_live_count();
    unsigned queued_futexes = futex_test_live_count();
    TEST_CHECK(mapped_backings >= backing_baseline + 2 &&
            queued_futexes == futex_baseline + 4,
            "两个共享页与四个等待键均已建立");
    struct aarch64_linux_futex_word_snapshot old_target;
    TEST_CHECK(snapshot_word(
                    fixture.parent, REQUEUE_TARGET, &old_target) &&
            old_target.shared_identity != 0 &&
            run_until_u32_result(fixture.parent,
                    PARENT_REQUEUE_RESULT, UINT32_MAX, 1) &&
            call_futex(fixture.parent, REQUEUE_SOURCE,
                    FUTEX_WAKE_, 1, 0, 0) == 0 &&
            futex_test_live_count() == queued_futexes,
            "真实 REQUEUE SVC 把等待迁到旧物理 target 并释放 source");

    TEST_CHECK(run_until_mapping_state(fixture.parent, false, NULL) &&
            run_until_u32_result(fixture.parent,
                    PARENT_MUNMAP_RESULT, UINT32_MAX, 0) &&
            guest_page_backing_test_live_count() ==
                    mapped_backings - 1 &&
            futex_test_live_count() == queued_futexes,
            "munmap 释放最后一份旧 backing 且保留重排等待对象");
    struct aarch64_linux_futex_word_snapshot new_target;
    TEST_CHECK(run_until_mapping_state(
                    fixture.parent, true, &new_target) &&
            run_until_u64_result(fixture.parent,
                    PARENT_REMAP_RESULT, UINT64_MAX,
                    TRANSIENT_BASE) &&
            new_target.shared_identity != 0 &&
            new_target.shared_identity != old_target.shared_identity &&
            guest_page_backing_test_live_count() == mapped_backings &&
            futex_test_live_count() == queued_futexes &&
            call_futex(fixture.parent, REQUEUE_TARGET,
                    FUTEX_WAKE_, 1, 0, 0) == 0,
            "MAP_FIXED 建立新物理键且不会误唤醒旧 target");

    TEST_CHECK(run_until_child_exits(fixture.parent, child_pid),
            "父 guest 的 kill SVC 中断旧键等待并完成真实 do_exit");
    TEST_CHECK(run_until_u32_result(fixture.parent,
                    PARENT_KILL_RESULT, UINT32_MAX, 0),
            "父 guest 的 kill SVC 返回成功");
    for (size_t index = 0; index < array_size(waiters); index++) {
        TEST_CHECK(join_waiter(&waiters[index]),
                "退出清理观察者被对应共享键唤醒");
        destroy_waiter_task(fixture.parent, waiter_tasks[index]);
    }
    dword_t listed_after;
    dword_t pending_after;
    dword_t runtime_tid_after;
    dword_t clone_tid_after;
    TEST_CHECK(read_u32(fixture.parent,
                    ROBUST_LISTED_WORD, &listed_after) &&
            read_u32(fixture.parent,
                    ROBUST_PENDING_WORD, &pending_after) &&
            read_u32(fixture.parent,
                    RUNTIME_CHILD_TID, &runtime_tid_after) &&
            read_u32(fixture.parent,
                    CLONE_CHILD_TID, &clone_tid_after) &&
            listed_after == (AARCH64_LINUX_FUTEX_WAITERS |
                    AARCH64_LINUX_FUTEX_OWNER_DIED) &&
            pending_after == 0 && runtime_tid_after == 0 &&
            clone_tid_after == (dword_t) child_pid,
            "robust、pending 与被 set_tid_address 覆盖的 ctid 均正确清理");

    struct wait4_result wait_result;
    TEST_CHECK(do_wait4(child_pid, 0, &wait_result) == child_pid &&
            wait_result.status == SIGKILL_,
            "wait4 回收由 SIGKILL 终止的独立子进程");
    lock(&pids_lock);
    bool reaped = pid_get_task_zombie((dword_t) child_pid) == NULL &&
            list_empty(&fixture.parent->children);
    unlock(&pids_lock);
    TEST_CHECK(reaped &&
            atomic_load_explicit(&fixture.parent->mm->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture.parent->files->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture.parent->fs->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture.parent->sighand->refcount,
                    memory_order_relaxed) == 1 &&
            futex_test_live_count() == futex_baseline,
            "子任务与全部 futex 对象回收后父资源引用恢复基线");

    destroy_fixture(&fixture);
    TEST_CHECK(guest_page_backing_test_live_count() ==
                    backing_baseline &&
            futex_test_live_count() == futex_baseline,
            "销毁父映像后所有 page backing 与 futex 回到基线");
    return true;
}

static bool run_isolated(void) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "创建 AArch64 futex 生命周期隔离进程失败：%s\n",
                strerror(errno));
        return false;
    }
    if (child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(30);
        bool passed = test_shared_exit_lifecycle();
        alarm(0);
        exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        fprintf(stderr, "等待 AArch64 futex 生命周期进程失败：%s\n",
                strerror(errno));
        return false;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "AArch64 futex 生命周期进程被 host 信号 %d 终止\n",
                WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "AArch64 futex 生命周期进程返回状态 %d\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

int main(void) {
    return run_isolated() ? 0 : 1;
}

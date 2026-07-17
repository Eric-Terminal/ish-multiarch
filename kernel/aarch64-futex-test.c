#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/linux/futex-abi.h"
#include "guest/linux/mman.h"
#include "guest/linux/syscall-service.h"
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
#define BSS_BASE UINT64_C(0x600000)
#define RUNTIME_COW_BASE UINT64_C(0x700000)
#define PADZERO_BASE UINT64_C(0x710000)
#define PIN_COW_BASE UINT64_C(0x720000)
#define CAS_COW_BASE UINT64_C(0x730000)
#define FILE_ALIAS_BASE UINT64_C(0x740000)
#define LAZY_FILE_BASE UINT64_C(0x750000)
#define LAZY_WAITV_BASE UINT64_C(0x760000)
#define LAZY_FILE_FD 1
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define SHARED_BASE UINT64_C(0x10000000)
#define UNRELATED_SHARED_BASE (SHARED_BASE + GUEST_MEMORY_PAGE_SIZE)
#define PRIVATE_BASE (UNRELATED_SHARED_BASE + GUEST_MEMORY_PAGE_SIZE)
#define READ_ONLY_PRIVATE_BASE (PRIVATE_BASE + GUEST_MEMORY_PAGE_SIZE)
#define READ_ONLY_SHARED_BASE \
    (READ_ONLY_PRIVATE_BASE + GUEST_MEMORY_PAGE_SIZE)
#define READ_ONLY_STACK_BASE (STACK_TOP - 2 * GUEST_MEMORY_PAGE_SIZE)
#define LONG_TIMEOUT (PRIVATE_BASE + UINT64_C(0xf00))
#define SHORT_TIMEOUT (PRIVATE_BASE + UINT64_C(0xf20))
#define WAITV_WIRE (PRIVATE_BASE + UINT64_C(0x100))
#define UNMAPPED_ADDRESS UINT64_C(0x20000000)
#define INVALID_ADDRESS (UINT64_C(1) << 48)
#define TEST_FILE_IDENTITY UINT64_C(0x7a64000000000001)
#define OTHER_FILE_IDENTITY UINT64_C(0x7a64000000000002)

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3
#define FUTEX_PRIVATE_FLAG_ UINT32_C(128)

#define TEST_CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 futex 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct futex_fixture {
    struct task *parent;
    struct tgroup group;
    struct mount lazy_mount;
    struct {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        byte_t bytes[GUEST_MEMORY_PAGE_SIZE * 2];
        bool block_read;
        bool read_entered;
        bool release_read;
        unsigned pread_calls;
        unsigned close_calls;
    } lazy_file;
    size_t text_cow_steps;
    size_t cas_cow_protect_steps;
    size_t pin_cow_protect_steps;
};

struct waiter {
    pthread_t thread;
    struct task *task;
    qword_t address;
    dword_t operation;
    dword_t value;
    qword_t timeout;
    atomic_uint stage;
    dword_t result;
};

struct immediate_futex_call {
    pthread_t thread;
    struct task *task;
    qword_t address;
    dword_t operation;
    atomic_bool completed;
    dword_t result;
};

struct waitv_futex_call {
    pthread_t thread;
    struct task *task;
    struct guest_linux_user_access user;
    struct guest_linux_user_fault fault;
    int_t result;
};

static const struct fd_ops metadata_fd_ops = {0};

static ssize_t lazy_file_pread(struct fd *fd, void *buffer,
        size_t size, off_t offset) {
    struct futex_fixture *fixture = fd->data;
    assert(pthread_mutex_lock(&fixture->lazy_file.mutex) == 0);
    fixture->lazy_file.pread_calls++;
    fixture->lazy_file.read_entered = true;
    assert(pthread_cond_broadcast(&fixture->lazy_file.cond) == 0);
    while (fixture->lazy_file.block_read &&
            !fixture->lazy_file.release_read) {
        assert(pthread_cond_wait(&fixture->lazy_file.cond,
                &fixture->lazy_file.mutex) == 0);
    }
    assert(pthread_mutex_unlock(&fixture->lazy_file.mutex) == 0);

    if (offset < 0 || (qword_t) offset >= sizeof(fixture->lazy_file.bytes))
        return 0;
    size_t available = sizeof(fixture->lazy_file.bytes) -
            (size_t) offset;
    if (size > available)
        size = available;
    memcpy(buffer, fixture->lazy_file.bytes + (size_t) offset, size);
    return (ssize_t) size;
}

static int lazy_file_close(struct fd *fd) {
    struct futex_fixture *fixture = fd->data;
    fixture->lazy_file.close_calls++;
    return 0;
}

static int lazy_file_fstat(struct fd *fd, struct statbuf *stat) {
    *stat = (struct statbuf) {
        .inode = fd->inode->number,
        .mode = S_IFREG | 0444,
        .size = sizeof(((struct futex_fixture *) 0)->lazy_file.bytes),
        .inode_device = fd->inode->device,
    };
    return 0;
}

static const struct fd_ops lazy_file_fd_ops = {
    .page_cacheable = true,
    .pread = lazy_file_pread,
    .close = lazy_file_close,
};

static const struct fs_ops lazy_file_fs = {
    .fstat = lazy_file_fstat,
};

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

static void append_file_mmap(dword_t *program, size_t *count,
        qword_t address, qword_t protection,
        qword_t fd, qword_t offset) {
    append_move(program, count, 0, address);
    append_move(program, count, 1, GUEST_MEMORY_PAGE_SIZE);
    append_move(program, count, 2, protection);
    append_move(program, count, 3,
            GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED);
    append_move(program, count, 4, fd);
    append_move(program, count, 5, offset);
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
        size_t *setup_steps, size_t *text_cow_steps,
        size_t *cas_cow_protect_steps) {
    dword_t program[128];
    size_t instruction_count = 0;
    append_mmap(program, &instruction_count, SHARED_BASE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS);
    append_mmap(program, &instruction_count, UNRELATED_SHARED_BASE,
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
    append_file_mmap(program, &instruction_count, LAZY_FILE_BASE,
            GUEST_LINUX_PROT_READ, LAZY_FILE_FD, 0);
    append_file_mmap(program, &instruction_count, LAZY_WAITV_BASE,
            GUEST_LINUX_PROT_READ, LAZY_FILE_FD,
            GUEST_MEMORY_PAGE_SIZE);
    append_mprotect(program, &instruction_count,
            READ_ONLY_STACK_BASE, GUEST_LINUX_PROT_READ);
    append_mprotect(program, &instruction_count,
            BSS_BASE, GUEST_LINUX_PROT_READ);
    append_move(program, &instruction_count, 0, RUNTIME_COW_BASE);
    append_move(program, &instruction_count, 1, UINT32_C(0x12345678));
    program[instruction_count++] = UINT32_C(0xb9000001);
    append_mprotect(program, &instruction_count,
            RUNTIME_COW_BASE, GUEST_LINUX_PROT_READ);
    append_mprotect(program, &instruction_count,
            PADZERO_BASE, GUEST_LINUX_PROT_READ);
    *setup_steps = instruction_count;
    append_mprotect(program, &instruction_count, TEXT_BASE,
            GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE |
            GUEST_LINUX_PROT_EXEC);
    *text_cow_steps = instruction_count - *setup_steps;
    append_mprotect(program, &instruction_count,
            CAS_COW_BASE, GUEST_LINUX_PROT_READ);
    *cas_cow_protect_steps =
            instruction_count - *setup_steps - *text_cow_steps;
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
    put_u16(file + 56, 8);

    byte_t *headers = file + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size = 8 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            TEXT_BASE + AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, TEXT_BASE, IMAGE_SIZE,
            IMAGE_SIZE, GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, BSS_BASE, 0,
            GUEST_MEMORY_PAGE_SIZE, GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 3 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, RUNTIME_COW_BASE, 4, 4,
            GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 4 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, PADZERO_BASE, 4, 8,
            GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 5 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, PIN_COW_BASE, 4, 4,
            GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 6 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 6, 0, CAS_COW_BASE, 4, 4,
            GUEST_MEMORY_PAGE_SIZE);
    put_program_header(headers + 7 * AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 4, 0, FILE_ALIAS_BASE, IMAGE_SIZE, IMAGE_SIZE,
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

static struct aarch64_linux_process *make_process(struct task *task,
        size_t *setup_steps, size_t *text_cow_steps,
        size_t *cas_cow_protect_steps,
        size_t *pin_cow_protect_steps, qword_t file_identity) {
    byte_t file[IMAGE_SIZE];
    size_t total_steps = make_image(file, setup_steps,
            text_cow_steps, cas_cow_protect_steps);
    *pin_cow_protect_steps =
            total_steps - *setup_steps - *text_cow_steps -
            *cas_cow_protect_steps;
    const char *arguments[] = {"futex-test"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    struct guest_file_source *file_source = guest_file_source_create(
            file_identity, NULL, NULL);
    if (file_source == NULL)
        return NULL;
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .elf_file_source = file_source,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x800000),
        .executable = "/bin/futex-test",
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

static bool install_lazy_file(struct futex_fixture *fixture) {
    fixture->lazy_mount = (struct mount) {
        .fs = &lazy_file_fs,
    };
    if (pthread_mutex_init(&fixture->lazy_file.mutex, NULL) != 0)
        return false;
    if (pthread_cond_init(&fixture->lazy_file.cond, NULL) != 0) {
        pthread_mutex_destroy(&fixture->lazy_file.mutex);
        return false;
    }
    put_u32(fixture->lazy_file.bytes, 7);
    put_u32(fixture->lazy_file.bytes + GUEST_MEMORY_PAGE_SIZE, 9);

    struct fd *fd = fd_create(&lazy_file_fd_ops);
    if (fd == NULL)
        return false;
    fd->data = fixture;
    fd->type = S_IFREG;
    fd->flags = O_RDONLY_;
    fd->mount = &fixture->lazy_mount;
    mount_retain(&fixture->lazy_mount);
    fd->inode = inode_get(&fixture->lazy_mount,
            UINT64_C(0xfeed0001), 1);
    return f_install_task(fixture->parent, fd, 0) == LAZY_FILE_FD;
}

static bool init_fixture(struct futex_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->parent = make_parent(&fixture->group);
    if (fixture->parent == NULL)
        return false;
    if (!install_lazy_file(fixture))
        return false;
    size_t setup_steps;
    struct aarch64_linux_process *process =
            make_process(fixture->parent, &setup_steps,
                    &fixture->text_cow_steps,
                    &fixture->cas_cow_protect_steps,
                    &fixture->pin_cow_protect_steps,
                    TEST_FILE_IDENTITY);
    if (process == NULL)
        return false;
    for (size_t step = 0; step < setup_steps; step++) {
        if (aarch64_linux_process_run_one(process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    if (!task_attach_aarch64_process(fixture->parent, process))
        return false;
    struct guest_linux_user_fault fault = {0};
    return aarch64_linux_process_write_u32(
                    process, SHARED_BASE, 0, &fault) &&
            aarch64_linux_process_write_u32(
                    process, PRIVATE_BASE, 0, &fault);
}

static bool run_delayed_steps(
        struct futex_fixture *fixture, size_t *remaining) {
    for (size_t step = 0; step < *remaining; step++) {
        if (aarch64_linux_process_run_one(
                fixture->parent->aarch64_process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    *remaining = 0;
    return true;
}

static void destroy_fixture(struct futex_fixture *fixture) {
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
    assert(fixture->lazy_file.close_calls == 1 &&
            fixture->lazy_mount.refcount == 0);
    assert(pthread_cond_destroy(&fixture->lazy_file.cond) == 0);
    assert(pthread_mutex_destroy(&fixture->lazy_file.mutex) == 0);
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

static struct task *make_related_task(struct task *parent,
        bool share_memory) {
    struct task *task = task_create_(parent);
    if (task == NULL)
        return NULL;
    struct aarch64_linux_process *process;
    if (share_memory) {
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

static struct task *make_independent_task(
        struct task *parent, qword_t file_identity) {
    struct task *task = task_create_(parent);
    if (task == NULL)
        return NULL;
    size_t setup_steps;
    size_t text_cow_steps;
    size_t cas_cow_protect_steps;
    size_t pin_cow_protect_steps;
    struct aarch64_linux_process *process = make_process(
            task, &setup_steps, &text_cow_steps,
            &cas_cow_protect_steps, &pin_cow_protect_steps,
            file_identity);
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

static bool write_word(struct task *task, qword_t address,
        dword_t value) {
    struct guest_linux_user_fault fault = {0};
    return aarch64_linux_process_write_u32(
            task->aarch64_process, address, value, &fault);
}

static bool write_timespec(struct task *task, qword_t address,
        qword_t seconds, qword_t nanoseconds) {
    return write_word(task, address, (dword_t) seconds) &&
            write_word(task, address + 4, (dword_t) (seconds >> 32)) &&
            write_word(task, address + 8, (dword_t) nanoseconds) &&
            write_word(task, address + 12,
                    (dword_t) (nanoseconds >> 32));
}

static dword_t call_futex(struct task *task, qword_t address,
        dword_t operation, dword_t value, qword_t timeout_or_value2,
        qword_t second_address, struct guest_linux_user_fault *fault) {
    struct task *saved = current;
    current = task;
    dword_t result = sys_futex_aarch64(address, operation, value,
            timeout_or_value2, second_address, 0, fault);
    current = saved;
    return result;
}

static void *immediate_futex_main(void *opaque) {
    struct immediate_futex_call *call = opaque;
    current = call->task;
    struct guest_linux_user_fault fault = {0};
    call->result = sys_futex_aarch64(call->address,
            call->operation, 1, 0, 0, 0, &fault);
    atomic_store_explicit(
            &call->completed, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool start_immediate_futex_call(
        struct immediate_futex_call *call, struct task *task,
        qword_t address, dword_t operation) {
    *call = (struct immediate_futex_call) {
        .task = task,
        .address = address,
        .operation = operation,
    };
    atomic_init(&call->completed, false);
    return pthread_create(&call->thread, NULL,
            immediate_futex_main, call) == 0;
}

static bool wait_for_immediate_futex_call(
        struct immediate_futex_call *call) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (atomic_load_explicit(
                &call->completed, memory_order_acquire))
            return true;
        sched_yield();
    }
    return false;
}

static bool futex_test_read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    return aarch64_linux_process_read_memory(opaque,
            address, destination, size, fault);
}

static void *waitv_futex_main(void *opaque) {
    struct waitv_futex_call *call = opaque;
    current = call->task;
    task_thread_store(call->task, pthread_self());
    call->result = sys_futex_waitv_aarch64(
            WAITV_WIRE, 1, 0, 0, 0,
            &call->user, &call->fault);
    current = NULL;
    return NULL;
}

static bool start_waitv_futex_call(
        struct waitv_futex_call *call, struct task *task) {
    *call = (struct waitv_futex_call) {
        .task = task,
        .user = {
            .opaque = task->aarch64_process,
            .read = futex_test_read_user,
        },
    };
    return pthread_create(&call->thread, NULL,
            waitv_futex_main, call) == 0;
}

static void block_lazy_file_read(struct futex_fixture *fixture) {
    assert(pthread_mutex_lock(&fixture->lazy_file.mutex) == 0);
    fixture->lazy_file.block_read = true;
    fixture->lazy_file.read_entered = false;
    fixture->lazy_file.release_read = false;
    assert(pthread_mutex_unlock(&fixture->lazy_file.mutex) == 0);
}

static bool wait_for_lazy_file_read(struct futex_fixture *fixture) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        assert(pthread_mutex_lock(&fixture->lazy_file.mutex) == 0);
        bool entered = fixture->lazy_file.read_entered;
        assert(pthread_mutex_unlock(&fixture->lazy_file.mutex) == 0);
        if (entered)
            return true;
        sched_yield();
    }
    return false;
}

static void release_lazy_file_read(struct futex_fixture *fixture) {
    assert(pthread_mutex_lock(&fixture->lazy_file.mutex) == 0);
    fixture->lazy_file.release_read = true;
    assert(pthread_cond_broadcast(&fixture->lazy_file.cond) == 0);
    assert(pthread_mutex_unlock(&fixture->lazy_file.mutex) == 0);
}

static void *waiter_main(void *opaque) {
    struct waiter *waiter = opaque;
    current = waiter->task;
    task_thread_store(waiter->task, pthread_self());
    atomic_store_explicit(&waiter->stage, 1, memory_order_release);
    struct guest_linux_user_fault fault = {0};
    waiter->result = sys_futex_aarch64(waiter->address,
            waiter->operation, waiter->value, waiter->timeout,
            0, 0, &fault);
    atomic_store_explicit(&waiter->stage, 2, memory_order_release);
    current = NULL;
    return NULL;
}

static bool start_waiter(struct waiter *waiter, struct task *task,
        qword_t address, dword_t operation, dword_t value,
        qword_t timeout) {
    *waiter = (struct waiter) {
        .task = task,
        .address = address,
        .operation = operation,
        .value = value,
        .timeout = timeout,
    };
    atomic_init(&waiter->stage, 0);
    if (pthread_create(&waiter->thread, NULL, waiter_main, waiter) != 0)
        return false;
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        if (atomic_load_explicit(
                &waiter->stage, memory_order_acquire) != 0)
            return true;
        sched_yield();
    }
    return false;
}

static unsigned completed_waiters(
        const struct waiter *waiters, size_t count) {
    unsigned completed = 0;
    for (size_t index = 0; index < count; index++) {
        if (atomic_load_explicit(
                &waiters[index].stage, memory_order_acquire) == 2)
            completed++;
    }
    return completed;
}

static bool wait_for_completed(struct waiter *waiters,
        size_t count, unsigned expected) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        unsigned completed = completed_waiters(waiters, count);
        if (completed >= expected)
            return completed == expected;
        sched_yield();
    }
    return false;
}

static bool wait_until_queued(struct task *observer,
        qword_t address, dword_t flags, dword_t count) {
    struct guest_linux_user_fault fault = {0};
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        dword_t result = call_futex(observer, address,
                FUTEX_REQUEUE_ | flags, 0, count,
                address, &fault);
        if (result == count)
            return true;
        if ((sdword_t) result < 0 || result > count)
            return false;
        sched_yield();
    }
    return false;
}

static bool wait_until_interruptible_wait_is_registered(struct task *task) {
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
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

static bool join_waiter(struct waiter *waiter, dword_t expected) {
    return pthread_join(waiter->thread, NULL) == 0 &&
            waiter->result == expected;
}

static bool test_edges_and_thread_private(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立边界测试夹具");
    struct task *thread = make_related_task(fixture.parent, true);
    TEST_CHECK(thread != NULL, "克隆共享地址空间线程");
    const qword_t word = PRIVATE_BASE + UINT64_C(0x20);
    const qword_t empty = PRIVATE_BASE + UINT64_C(0x24);
    TEST_CHECK(write_word(fixture.parent, word, 7) &&
            write_timespec(fixture.parent, LONG_TIMEOUT, 5, 0),
            "准备私有 futex 与超时");

    struct guest_linux_user_fault fault = {.address = UINT64_MAX};
    TEST_CHECK(call_futex(fixture.parent, word,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 8, 0, 0,
            &fault) == (dword_t) _EAGAIN,
            "值不匹配返回 EAGAIN");
    TEST_CHECK(call_futex(fixture.parent, word + 1,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 7, 0, 0,
            &fault) == (dword_t) _EINVAL,
            "未对齐地址返回 EINVAL");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, INVALID_ADDRESS,
            FUTEX_WAIT_, 0, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == INVALID_ADDRESS,
            "坏地址返回 EFAULT 并报告地址");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, INVALID_ADDRESS,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == (dword_t) _EFAULT &&
            fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "私有 WAKE 拒绝越界用户地址");
    TEST_CHECK(call_futex(fixture.parent, UNMAPPED_ADDRESS,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 0, "私有 WAKE 不要求地址已经映射");
    TEST_CHECK(call_futex(fixture.parent, UNMAPPED_ADDRESS,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 0, 1,
            UNMAPPED_ADDRESS + sizeof(dword_t), &fault) == 0,
            "私有 REQUEUE 不要求有效范围内的地址已经映射");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, empty,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 0, 1,
            INVALID_ADDRESS, &fault) == (dword_t) _EFAULT &&
            fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "私有 REQUEUE 在改动队列前校验目标范围");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, INVALID_ADDRESS,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 0, 1,
            empty, &fault) == (dword_t) _EFAULT &&
            fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "私有 REQUEUE 优先校验源地址范围");

    futex_test_fail_allocation_at(0);
    TEST_CHECK(call_futex(fixture.parent, empty,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 0, "空 WAKE 不分配对象");
    futex_test_fail_allocation_at(SIZE_MAX);

    struct waiter waiter;
    TEST_CHECK(start_waiter(&waiter, thread, word,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 7, LONG_TIMEOUT),
            "启动 clone_thread 私有等待者");
    TEST_CHECK(wait_until_queued(fixture.parent, word,
            FUTEX_PRIVATE_FLAG_, 1), "观察 clone_thread 等待入队");
    TEST_CHECK(call_futex(fixture.parent, word,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 1, "共享 mm identity 可唤醒私有 futex");
    TEST_CHECK(join_waiter(&waiter, 0), "回收私有 futex 等待者");

    struct waiter interrupted_waiter;
    TEST_CHECK(start_waiter(&interrupted_waiter, thread, word,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 7, LONG_TIMEOUT),
            "启动可中断 futex 等待者");
    TEST_CHECK(wait_until_interruptible_wait_is_registered(thread),
            "等待 futex 发布可中断 condition");
    send_signal(thread, SIGUSR1_, (struct siginfo_) {.code = SI_USER_});
    TEST_CHECK(join_waiter(&interrupted_waiter, (dword_t) _EINTR),
            "登记后的 guest 信号可靠中断 futex 等待");

    destroy_related_task(fixture.parent, thread);
    destroy_fixture(&fixture);
    return true;
}

static bool test_lazy_file_prefault_outside_global_lock(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 lazy 文件 futex 夹具");
    TEST_CHECK(fixture.lazy_file.pread_calls == 0,
            "文件 mmap 建立时不提前读取后备页");
    struct task *thread = make_related_task(fixture.parent, true);
    TEST_CHECK(thread != NULL, "建立共享 lazy VMA 的线程");

    block_lazy_file_read(&fixture);
    struct waiter lazy_waiter;
    TEST_CHECK(start_waiter(&lazy_waiter, thread, LAZY_FILE_BASE,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 8, 0),
            "启动会触发 lazy page-in 的 futex waiter");
    TEST_CHECK(wait_for_lazy_file_read(&fixture),
            "观察 provider 进入阻塞式 pread");

    struct immediate_futex_call independent_wake;
    TEST_CHECK(start_immediate_futex_call(&independent_wake,
            fixture.parent, PRIVATE_BASE,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_),
            "并发启动无关 futex wake");
    bool completed_while_io_blocked =
            wait_for_immediate_futex_call(&independent_wake);
    release_lazy_file_read(&fixture);
    TEST_CHECK(pthread_join(independent_wake.thread, NULL) == 0 &&
            completed_while_io_blocked && independent_wake.result == 0,
            "文件 I/O 阻塞期间 futex 全局锁仍可服务无关操作");
    TEST_CHECK(join_waiter(&lazy_waiter, (dword_t) _EAGAIN) &&
            fixture.lazy_file.pread_calls == 1,
            "prefault 完成后锁内重新读取并比较 futex 值");

    TEST_CHECK(write_word(fixture.parent, WAITV_WIRE, 10) &&
            write_word(fixture.parent, WAITV_WIRE + 4, 0) &&
            write_word(fixture.parent, WAITV_WIRE + 8,
                    (dword_t) LAZY_WAITV_BASE) &&
            write_word(fixture.parent, WAITV_WIRE + 12,
                    (dword_t) (LAZY_WAITV_BASE >> 32)) &&
            write_word(fixture.parent, WAITV_WIRE + 16,
                    GUEST_LINUX_FUTEX2_SIZE_U32 |
                    GUEST_LINUX_FUTEX_PRIVATE_FLAG) &&
            write_word(fixture.parent, WAITV_WIRE + 20, 0),
            "准备指向第二个 lazy 文件页的 waitv wire");
    block_lazy_file_read(&fixture);
    struct waitv_futex_call waitv_call;
    TEST_CHECK(start_waitv_futex_call(&waitv_call, thread),
            "启动会触发第二次 lazy page-in 的 futex_waitv");
    TEST_CHECK(wait_for_lazy_file_read(&fixture),
            "观察 waitv provider 进入阻塞式 pread");
    TEST_CHECK(start_immediate_futex_call(&independent_wake,
            fixture.parent, PRIVATE_BASE,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_),
            "waitv I/O 期间并发启动无关 futex wake");
    completed_while_io_blocked =
            wait_for_immediate_futex_call(&independent_wake);
    release_lazy_file_read(&fixture);
    TEST_CHECK(pthread_join(independent_wake.thread, NULL) == 0 &&
            pthread_join(waitv_call.thread, NULL) == 0 &&
            completed_while_io_blocked && independent_wake.result == 0 &&
            waitv_call.result == _EAGAIN &&
            fixture.lazy_file.pread_calls == 2,
            "waitv page-in 同样不持有 futex 全局锁并保留值比较语义");

    struct guest_linux_user_fault fault = {0};
    TEST_CHECK(call_futex(fixture.parent, LAZY_FILE_BASE,
            FUTEX_WAIT_, 8, 0, 0, &fault) == (dword_t) _EAGAIN &&
            fault.kind == GUEST_MEMORY_FAULT_NONE &&
            fixture.lazy_file.pread_calls == 2,
            "共享 futex 复用已载入文件页且不重复 I/O");
    const qword_t addresses[] = {LAZY_FILE_BASE};
    struct aarch64_linux_futex_word_snapshot snapshot;
    dword_t value;
    TEST_CHECK(aarch64_linux_process_snapshot_futex_words(
            fixture.parent->aarch64_process,
            addresses, 1, true, &snapshot, &value, &fault) &&
            value == 7 && snapshot.file_identity != 0 &&
            snapshot.file_offset == 0,
            "只读 MAP_PRIVATE 文件页形成稳定 inode/offset futex 键");

    destroy_related_task(fixture.parent, thread);
    destroy_fixture(&fixture);
    return true;
}

static bool test_read_only_shared_key_permissions(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立只读 futex 键测试夹具");
    struct guest_linux_user_fault fault = {0};

    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_PRIVATE_BASE,
            FUTEX_WAIT_, 1, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == READ_ONLY_PRIVATE_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "共享 WAIT 拒绝只读匿名私有页并报告写权限故障");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_PRIVATE_BASE,
            FUTEX_WAKE_, 1, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == READ_ONLY_PRIVATE_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "共享 WAKE 拒绝只读匿名私有页");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_PRIVATE_BASE,
            FUTEX_REQUEUE_, 0, 1, READ_ONLY_SHARED_BASE,
            &fault) == (dword_t) _EFAULT &&
            fault.address == READ_ONLY_PRIVATE_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "共享 REQUEUE 在源键阶段拒绝只读匿名私有页");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_SHARED_BASE,
            FUTEX_REQUEUE_, 0, 1, READ_ONLY_PRIVATE_BASE,
            &fault) == (dword_t) _EFAULT &&
            fault.address == READ_ONLY_PRIVATE_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "共享 REQUEUE 在队列副作用前校验只读私有目标键");

    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_PRIVATE_BASE,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == (dword_t) _EAGAIN &&
            fault.kind == GUEST_MEMORY_FAULT_NONE &&
            call_futex(fixture.parent, READ_ONLY_PRIVATE_BASE,
                    FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_,
                    1, 0, 0, &fault) == 0 &&
            call_futex(fixture.parent, READ_ONLY_PRIVATE_BASE,
                    FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 0, 1,
                    READ_ONLY_SHARED_BASE, &fault) == 0,
            "PRIVATE 操作保留只读页的虚拟键语义");

    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_SHARED_BASE,
            FUTEX_WAIT_, 1, 0, 0, &fault) == (dword_t) _EAGAIN &&
            fault.kind == GUEST_MEMORY_FAULT_NONE &&
            call_futex(fixture.parent, READ_ONLY_SHARED_BASE,
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            call_futex(fixture.parent, READ_ONLY_SHARED_BASE,
                    FUTEX_REQUEUE_, 0, 1,
                    READ_ONLY_SHARED_BASE, &fault) == 0,
            "只读匿名共享页仍可形成稳定物理键");

    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, READ_ONLY_STACK_BASE,
            FUTEX_WAIT_, 1, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == READ_ONLY_STACK_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            call_futex(fixture.parent, READ_ONLY_STACK_BASE,
                    FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
                    1, 0, 0, &fault) == (dword_t) _EAGAIN,
            "只读初始栈保留匿名私有来源并允许 PRIVATE 读取");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, TEXT_BASE + ENTRY_OFFSET,
            FUTEX_WAIT_, 0, 0, 0, &fault) == (dword_t) _EAGAIN &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "只读 ELF 文件页通过来源检查并到达值比较");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, BSS_BASE,
            FUTEX_WAIT_, 1, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == BSS_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            call_futex(fixture.parent, BSS_BASE,
                    FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
                    1, 0, 0, &fault) == (dword_t) _EAGAIN,
            "只读 ELF 纯 BSS 页保留匿名来源");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, RUNTIME_COW_BASE,
            FUTEX_WAIT_, 0, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == RUNTIME_COW_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            call_futex(fixture.parent, RUNTIME_COW_BASE,
                    FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
                    0, 0, 0, &fault) == (dword_t) _EAGAIN,
            "运行期写入后降权的 ELF 私有页已经转为匿名来源");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, PADZERO_BASE,
            FUTEX_WAIT_, 0, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == PADZERO_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            call_futex(fixture.parent, PADZERO_BASE,
                    FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
                    0, 0, 0, &fault) == (dword_t) _EAGAIN,
            "可写 PT_LOAD 的文件尾补零页保留匿名 COW 来源");

    const qword_t text_word = TEXT_BASE + ENTRY_OFFSET;
    dword_t text_value;
    struct task *file_waiter_task = make_related_task(
            fixture.parent, true);
    struct waiter file_waiter;
    TEST_CHECK(file_waiter_task != NULL &&
            aarch64_linux_process_read_u32(
                    fixture.parent->aarch64_process,
                    text_word, &text_value, &fault) &&
            write_timespec(fixture.parent, SHORT_TIMEOUT,
                    0, 500000000) &&
            start_waiter(&file_waiter, file_waiter_task,
                    text_word, FUTEX_WAIT_, text_value,
                    SHORT_TIMEOUT) &&
            wait_until_queued(fixture.parent, text_word, 0, 1),
            "在只读文件页键上登记等待者");
    TEST_CHECK(run_delayed_steps(
                    &fixture, &fixture.text_cow_steps) &&
            call_futex(fixture.parent, text_word,
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            completed_waiters(&file_waiter, 1) == 0 &&
            join_waiter(&file_waiter, (dword_t) _ETIMEDOUT),
            "文件页写固定后的匿名键不得误唤醒旧文件页等待者");
    destroy_related_task(fixture.parent, file_waiter_task);

    dword_t cas_observed = UINT32_MAX;
    struct aarch64_linux_futex_word_snapshot cas_snapshot = {
        .shared_identity = UINT64_MAX,
        .file_identity = UINT64_MAX,
        .page_offset = UINT64_MAX,
        .file_offset = UINT64_MAX,
    };
    TEST_CHECK(aarch64_linux_process_compare_exchange_futex_u32(
                    fixture.parent->aarch64_process,
                    CAS_COW_BASE, 1, 2, &cas_observed,
                    &cas_snapshot, &fault) ==
                            AARCH64_LINUX_PROCESS_COMPARE_EXCHANGE_MISMATCH &&
            cas_observed == UINT32_C(0x464c457f) &&
            cas_snapshot.shared_identity == 0 &&
            cas_snapshot.file_identity == 0 &&
            cas_snapshot.page_offset == 0 &&
            cas_snapshot.file_offset == 0 &&
            run_delayed_steps(
                    &fixture, &fixture.cas_cow_protect_steps),
            "原子写比较不匹配仍完成私有文件页 COW 后再降权");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, CAS_COW_BASE,
                    FUTEX_WAIT_, 1, 0, 0, &fault) ==
                            (dword_t) _EFAULT &&
            fault.address == CAS_COW_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            call_futex(fixture.parent, CAS_COW_BASE,
                    FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
                    1, 0, 0, &fault) == (dword_t) _EAGAIN,
            "CAS mismatch 后的只读匿名页拒绝共享键但保留 PRIVATE 读取");

    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, PIN_COW_BASE,
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE &&
            run_delayed_steps(
                    &fixture, &fixture.pin_cow_protect_steps),
            "共享 futex 写固定私有化仍可写的纯文件页后再降权");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, PIN_COW_BASE,
                    FUTEX_WAKE_, 1, 0, 0, &fault) ==
                            (dword_t) _EFAULT &&
            fault.address == PIN_COW_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION &&
            call_futex(fixture.parent, PIN_COW_BASE,
                    FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
                    0, 0, 0, &fault) == (dword_t) _EAGAIN,
            "仅由共享取键触发的文件页 COW 降权后拒绝共享键");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, SIGNAL_TRAMPOLINE,
            FUTEX_WAIT_, 0, 0, 0, &fault) == (dword_t) _EFAULT &&
            fault.address == SIGNAL_TRAMPOLINE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_PERMISSION,
            "特殊信号跳板不能伪装成文件后备共享键");

    destroy_fixture(&fixture);
    return true;
}

static bool test_independent_file_key_domains(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture),
            "建立独立文件来源 futex 键测试夹具");
    struct task *same_file = make_independent_task(
            fixture.parent, TEST_FILE_IDENTITY);
    struct task *other_file = make_independent_task(
            fixture.parent, OTHER_FILE_IDENTITY);
    TEST_CHECK(same_file != NULL && other_file != NULL,
            "建立同文件与异文件的独立 AArch64 进程");

    dword_t parent_value;
    dword_t same_value;
    dword_t other_value;
    struct guest_linux_user_fault fault = {0};
    TEST_CHECK(aarch64_linux_process_read_u32(
                    fixture.parent->aarch64_process,
                    TEXT_BASE, &parent_value, &fault) &&
            aarch64_linux_process_read_u32(
                    same_file->aarch64_process,
                    TEXT_BASE, &same_value, &fault) &&
            aarch64_linux_process_read_u32(
                    other_file->aarch64_process,
                    TEXT_BASE, &other_value, &fault) &&
            parent_value == UINT32_C(0x464c457f) &&
            same_value == parent_value && other_value == parent_value,
            "三个独立映像观察相同的只读文件字节");

    struct waiter same_waiter;
    TEST_CHECK(start_waiter(&same_waiter, same_file,
                    TEXT_BASE, FUTEX_WAIT_, same_value, 0) &&
            wait_until_queued(same_file, TEXT_BASE, 0, 1),
            "同文件独立进程在原虚拟地址登记等待");
    TEST_CHECK(call_futex(fixture.parent,
                    FILE_ALIAS_BASE + sizeof(dword_t),
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            completed_waiters(&same_waiter, 1) == 0,
            "同文件不同绝对偏移不得串键");
    TEST_CHECK(call_futex(fixture.parent, FILE_ALIAS_BASE,
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 1 &&
            join_waiter(&same_waiter, 0),
            "同文件同偏移可跨来源对象、进程与虚拟地址唤醒");

    struct waiter other_waiter;
    TEST_CHECK(start_waiter(&other_waiter, other_file,
                    TEXT_BASE, FUTEX_WAIT_, other_value, 0) &&
            wait_until_queued(other_file, TEXT_BASE, 0, 1),
            "不同文件身份的独立进程登记等待");
    TEST_CHECK(call_futex(fixture.parent, FILE_ALIAS_BASE,
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            completed_waiters(&other_waiter, 1) == 0,
            "不同文件身份的相同偏移不得误唤醒");
    TEST_CHECK(call_futex(other_file, TEXT_BASE,
                    FUTEX_WAKE_, 1, 0, 0, &fault) == 1 &&
            join_waiter(&other_waiter, 0),
            "不同文件身份仍能在自身键域完成唤醒");

    destroy_related_task(fixture.parent, other_file);
    destroy_related_task(fixture.parent, same_file);
    destroy_fixture(&fixture);
    return true;
}

static bool test_fork_key_domains(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 fork 键域测试夹具");
    const qword_t private_word = PRIVATE_BASE + UINT64_C(0x40);
    const qword_t fallback_word = PRIVATE_BASE + UINT64_C(0x44);
    const qword_t shared_word = SHARED_BASE + UINT64_C(0x44);
    const qword_t shared_other = SHARED_BASE + UINT64_C(0x40);
    const qword_t unrelated_shared_word =
            UNRELATED_SHARED_BASE + UINT64_C(0x44);
    TEST_CHECK(write_word(fixture.parent, private_word, 11) &&
            write_word(fixture.parent, fallback_word, 12) &&
            write_word(fixture.parent, shared_word, 21) &&
            write_word(fixture.parent, shared_other, 22) &&
            write_timespec(fixture.parent, LONG_TIMEOUT, 5, 0),
            "准备 fork 前内存");
    struct task *thread = make_related_task(fixture.parent, true);
    struct task *child = make_related_task(fixture.parent, false);
    TEST_CHECK(thread != NULL && child != NULL,
            "建立同 mm 线程与独立 fork 子进程");
    struct guest_linux_user_fault fault = {0};

    struct waiter private_waiter;
    TEST_CHECK(start_waiter(&private_waiter, fixture.parent,
            private_word, FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_,
            11, LONG_TIMEOUT), "启动 fork 父进程私有等待者");
    TEST_CHECK(wait_until_queued(thread, private_word,
            FUTEX_PRIVATE_FLAG_, 1), "私有等待者入队");
    TEST_CHECK(call_futex(thread, private_word,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            call_futex(thread, private_word,
                    FUTEX_REQUEUE_, 0, 1,
                    private_word + sizeof(dword_t), &fault) == 0 &&
            completed_waiters(&private_waiter, 1) == 0,
            "同一 mm 与 VA 的 mm-shared 操作不命中 PRIVATE 队列");
    TEST_CHECK(call_futex(child, private_word,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 0 && completed_waiters(&private_waiter, 1) == 0,
            "fork 后相同 VA 不共享 PRIVATE 键");
    TEST_CHECK(call_futex(thread, private_word,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 1 && join_waiter(&private_waiter, 0),
            "同 mm 线程唤醒 fork 父进程");

    struct waiter fallback_waiter;
    TEST_CHECK(start_waiter(&fallback_waiter, fixture.parent,
            fallback_word, FUTEX_WAIT_, 12, LONG_TIMEOUT),
            "启动私有映射非 PRIVATE 等待者");
    TEST_CHECK(wait_until_queued(thread, fallback_word, 0, 1),
            "私有映射回退键等待入队");
    TEST_CHECK(call_futex(thread, fallback_word,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 0 &&
            call_futex(thread, fallback_word,
                    FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 0, 1,
                    fallback_word + sizeof(dword_t), &fault) == 0 &&
            completed_waiters(&fallback_waiter, 1) == 0,
            "同一 mm 与 VA 的 PRIVATE 操作不命中 mm-shared 队列");
    TEST_CHECK(call_futex(child, fallback_word,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            completed_waiters(&fallback_waiter, 1) == 0,
            "非共享 backing 回退为 mm 与 VA 键");
    TEST_CHECK(call_futex(thread, fallback_word,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 1 &&
            join_waiter(&fallback_waiter, 0),
            "回退键仍在同 mm 内工作");

    struct waiter shared_waiter;
    TEST_CHECK(start_waiter(&shared_waiter, child, shared_word,
            FUTEX_WAIT_, 21, LONG_TIMEOUT),
            "启动共享匿名 backing 等待者");
    TEST_CHECK(wait_until_queued(fixture.parent, shared_word, 0, 1),
            "父进程通过共享 backing 观察等待者");
    TEST_CHECK(call_futex(fixture.parent, shared_other,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            completed_waiters(&shared_waiter, 1) == 0,
            "同页不同 offset 不串键");
    TEST_CHECK(call_futex(fixture.parent, unrelated_shared_word,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 0 &&
            completed_waiters(&shared_waiter, 1) == 0,
            "不同共享 backing 的相同 offset 不串键");
    TEST_CHECK(write_word(fixture.parent, shared_word, 23),
            "父进程更新共享 futex 值");
    TEST_CHECK(call_futex(fixture.parent, shared_word,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 1 &&
            join_waiter(&shared_waiter, 0),
            "fork 父子通过共享 backing 唤醒");

    const qword_t shared_requeue_source =
            SHARED_BASE + UINT64_C(0x80);
    const qword_t shared_requeue_target =
            SHARED_BASE + UINT64_C(0x84);
    TEST_CHECK(write_word(fixture.parent, shared_requeue_source, 31) &&
            write_word(fixture.parent, shared_requeue_target, 0),
            "准备共享物理 REQUEUE 键");
    struct waiter shared_requeue_waiters[2];
    TEST_CHECK(start_waiter(&shared_requeue_waiters[0], child,
                    shared_requeue_source, FUTEX_WAIT_, 31, LONG_TIMEOUT) &&
            start_waiter(&shared_requeue_waiters[1], thread,
                    shared_requeue_source, FUTEX_WAIT_, 31, LONG_TIMEOUT),
            "启动跨 fork 的共享 REQUEUE 等待者");
    TEST_CHECK(wait_until_queued(fixture.parent,
            shared_requeue_source, 0, 2),
            "父进程通过物理键观察两个共享等待者");
    TEST_CHECK(call_futex(fixture.parent, shared_requeue_source,
            FUTEX_REQUEUE_, 0, 2, shared_requeue_target, &fault) == 2,
            "非 PRIVATE REQUEUE 迁移共享物理键等待者");
    TEST_CHECK(call_futex(fixture.parent, shared_requeue_source,
            FUTEX_WAKE_, 2, 0, 0, &fault) == 0 &&
            completed_waiters(shared_requeue_waiters, 2) == 0,
            "共享 REQUEUE 后源物理键为空");
    TEST_CHECK(call_futex(fixture.parent, shared_requeue_target,
            FUTEX_WAKE_, 2, 0, 0, &fault) == 2 &&
            join_waiter(&shared_requeue_waiters[0], 0) &&
            join_waiter(&shared_requeue_waiters[1], 0),
            "目标物理键唤醒跨地址空间等待者");

    destroy_related_task(fixture.parent, child);
    destroy_related_task(fixture.parent, thread);
    destroy_fixture(&fixture);
    return true;
}

static bool make_thread_waiters(struct task *parent,
        struct task **tasks, struct waiter *waiters, size_t count,
        qword_t address, dword_t operation, dword_t value,
        qword_t timeout) {
    for (size_t index = 0; index < count; index++) {
        tasks[index] = make_related_task(parent, true);
        if (tasks[index] == NULL || !start_waiter(&waiters[index],
                tasks[index], address, operation, value, timeout))
            return false;
    }
    return true;
}

static bool join_and_destroy_waiters(struct task *parent,
        struct task **tasks, struct waiter *waiters, size_t count,
        dword_t expected) {
    for (size_t index = 0; index < count; index++) {
        if (!join_waiter(&waiters[index], expected))
            return false;
        destroy_related_task(parent, tasks[index]);
    }
    return true;
}

static bool test_requeue_semantics(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 REQUEUE 测试夹具");
    TEST_CHECK(write_timespec(fixture.parent, LONG_TIMEOUT, 5, 0),
            "准备 REQUEUE 长超时");
    struct guest_linux_user_fault fault = {0};
    struct task *tasks[3];
    struct waiter waiters[3];

    const qword_t source = PRIVATE_BASE + UINT64_C(0x100);
    const qword_t target = PRIVATE_BASE + UINT64_C(0x104);
    TEST_CHECK(write_word(fixture.parent, source, 31) &&
            write_word(fixture.parent, target, 0),
            "准备不同 REQUEUE 键");
    TEST_CHECK(make_thread_waiters(fixture.parent, tasks, waiters, 3,
            source, FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 31,
            LONG_TIMEOUT), "启动不同键 REQUEUE 等待者");
    TEST_CHECK(wait_until_queued(fixture.parent, source,
            FUTEX_PRIVATE_FLAG_, 3), "三个不同键等待者均已入队");
    TEST_CHECK(call_futex(fixture.parent, source,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 1, 2,
            target, &fault) == 3, "REQUEUE 返回唤醒与迁移总数");
    TEST_CHECK(wait_for_completed(waiters, 3, 1),
            "只有直接唤醒的一个等待者完成");
    TEST_CHECK(call_futex(fixture.parent, source,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 3, 0, 0,
            &fault) == 0, "迁移后源队列为空");
    TEST_CHECK(call_futex(fixture.parent, target,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 2, 0, 0,
            &fault) == 2, "目标键唤醒迁移后的等待者");
    TEST_CHECK(join_and_destroy_waiters(fixture.parent,
            tasks, waiters, 3, 0), "回收不同键等待者");

    const qword_t same = PRIVATE_BASE + UINT64_C(0x120);
    TEST_CHECK(write_word(fixture.parent, same, 41),
            "准备相同 REQUEUE 键");
    TEST_CHECK(make_thread_waiters(fixture.parent, tasks, waiters, 3,
            same, FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 41,
            LONG_TIMEOUT), "启动相同键 REQUEUE 等待者");
    TEST_CHECK(wait_until_queued(fixture.parent, same,
            FUTEX_PRIVATE_FLAG_, 3), "三个相同键等待者均已入队");
    TEST_CHECK(call_futex(fixture.parent, same,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 1, 7,
            same, &fault) == 3, "相同键只计数而不循环迁移");
    TEST_CHECK(wait_for_completed(waiters, 3, 1),
            "相同键仅直接唤醒一个等待者");
    TEST_CHECK(call_futex(fixture.parent, same,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 2, 0, 0,
            &fault) == 2, "相同键剩余等待者仍留在源队列");
    TEST_CHECK(join_and_destroy_waiters(fixture.parent,
            tasks, waiters, 3, 0), "回收相同键等待者");

    destroy_fixture(&fixture);
    return true;
}

static bool test_requeue_failures(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 REQUEUE 失败测试夹具");
    TEST_CHECK(write_timespec(fixture.parent, LONG_TIMEOUT, 5, 0),
            "准备失败路径长超时");
    struct guest_linux_user_fault fault = {0};
    const qword_t empty_source = PRIVATE_BASE + UINT64_C(0x140);
    const qword_t empty_target = PRIVATE_BASE + UINT64_C(0x144);
    futex_test_fail_allocation_at(0);
    TEST_CHECK(call_futex(fixture.parent, empty_source,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 1, 1,
            empty_target, &fault) == 0,
            "空 REQUEUE source 不分配任何对象");
    futex_test_fail_allocation_at(SIZE_MAX);

    const qword_t oom_source = PRIVATE_BASE + UINT64_C(0x160);
    const qword_t oom_target = PRIVATE_BASE + UINT64_C(0x164);
    TEST_CHECK(write_word(fixture.parent, oom_source, 51) &&
            write_word(fixture.parent, oom_target, 0),
            "准备 REQUEUE OOM 键");
    struct task *oom_tasks[2];
    struct waiter oom_waiters[2];
    TEST_CHECK(make_thread_waiters(fixture.parent,
            oom_tasks, oom_waiters, 2, oom_source,
            FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 51, LONG_TIMEOUT),
            "启动 OOM source 等待者");
    TEST_CHECK(wait_until_queued(fixture.parent, oom_source,
            FUTEX_PRIVATE_FLAG_, 2), "OOM source 等待者均已入队");
    futex_test_fail_allocation_at(0);
    TEST_CHECK(call_futex(fixture.parent, oom_source,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 1, 1,
            oom_target, &fault) == (dword_t) _ENOMEM,
            "目标对象分配失败返回 ENOMEM");
    futex_test_fail_allocation_at(SIZE_MAX);
    for (unsigned attempt = 0; attempt < 10000; attempt++)
        sched_yield();
    TEST_CHECK(completed_waiters(oom_waiters, 2) == 0,
            "目标 OOM 对源队列零副作用");
    TEST_CHECK(call_futex(fixture.parent, oom_source,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 2, 0, 0,
            &fault) == 2, "OOM 后源队列仍可完整唤醒");
    TEST_CHECK(join_and_destroy_waiters(fixture.parent,
            oom_tasks, oom_waiters, 2, 0), "回收 OOM 等待者");

    const qword_t valid_source = PRIVATE_BASE + UINT64_C(0x180);
    const qword_t valid_target = PRIVATE_BASE + UINT64_C(0x184);
    TEST_CHECK(write_word(fixture.parent, valid_source, 61) &&
            write_word(fixture.parent, valid_target, 62),
            "准备坏映射 REQUEUE 键");
    struct task *source_task = make_related_task(fixture.parent, true);
    struct waiter source_waiter;
    TEST_CHECK(source_task != NULL && start_waiter(&source_waiter,
            source_task, valid_source, FUTEX_WAIT_, 61,
            LONG_TIMEOUT), "启动坏 target 场景等待者");
    TEST_CHECK(wait_until_queued(
            fixture.parent, valid_source, 0, 1),
            "坏 target 场景等待者入队");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, valid_source,
            FUTEX_REQUEUE_, 0, 1, INVALID_ADDRESS,
            &fault) == (dword_t) _EFAULT &&
            fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            completed_waiters(&source_waiter, 1) == 0,
            "坏 target 返回 EFAULT 且不改 source");
    TEST_CHECK(call_futex(fixture.parent, valid_source,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 1 &&
            join_waiter(&source_waiter, 0),
            "坏 target 后 source 仍可唤醒");
    destroy_related_task(fixture.parent, source_task);

    struct task *target_task = make_related_task(fixture.parent, true);
    struct waiter target_waiter;
    TEST_CHECK(target_task != NULL && start_waiter(&target_waiter,
            target_task, valid_target, FUTEX_WAIT_, 62,
            LONG_TIMEOUT), "启动坏 source 场景目标等待者");
    TEST_CHECK(wait_until_queued(
            fixture.parent, valid_target, 0, 1),
            "坏 source 场景目标等待者入队");
    fault = (struct guest_linux_user_fault) {0};
    TEST_CHECK(call_futex(fixture.parent, INVALID_ADDRESS,
            FUTEX_REQUEUE_, 0, 1, valid_target,
            &fault) == (dword_t) _EFAULT &&
            fault.address == INVALID_ADDRESS &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            completed_waiters(&target_waiter, 1) == 0,
            "坏 source 返回 EFAULT 且不改另一队列");
    TEST_CHECK(call_futex(fixture.parent, valid_target,
            FUTEX_WAKE_, 1, 0, 0, &fault) == 1 &&
            join_waiter(&target_waiter, 0),
            "坏 source 后既有队列仍可唤醒");
    destroy_related_task(fixture.parent, target_task);

    destroy_fixture(&fixture);
    return true;
}

static bool test_requeue_timeout(void) {
    struct futex_fixture fixture;
    TEST_CHECK(init_fixture(&fixture), "建立 REQUEUE 超时测试夹具");
    const qword_t source = PRIVATE_BASE + UINT64_C(0x1c0);
    const qword_t target = PRIVATE_BASE + UINT64_C(0x1c4);
    TEST_CHECK(write_word(fixture.parent, source, 71) &&
            write_word(fixture.parent, target, 0) &&
            write_timespec(fixture.parent, SHORT_TIMEOUT,
                    0, 200000000), "准备短 deadline");
    struct task *thread = make_related_task(fixture.parent, true);
    struct waiter waiter;
    TEST_CHECK(thread != NULL && start_waiter(&waiter, thread,
            source, FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_, 71,
            SHORT_TIMEOUT), "启动短 deadline 等待者");
    TEST_CHECK(wait_until_queued(fixture.parent, source,
            FUTEX_PRIVATE_FLAG_, 1), "短 deadline 等待者入队");
    struct guest_linux_user_fault fault = {0};
    TEST_CHECK(call_futex(fixture.parent, source,
            FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_, 0, 1,
            target, &fault) == 1, "短 deadline 等待者迁移到目标");
    TEST_CHECK(join_waiter(&waiter, (dword_t) _ETIMEDOUT),
            "迁移后从目标队列安全超时退出");
    TEST_CHECK(call_futex(fixture.parent, target,
            FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_, 1, 0, 0,
            &fault) == 0, "超时退出后目标队列为空");

    destroy_related_task(fixture.parent, thread);
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
        signal(SIGUSR1, SIG_IGN);
        alarm(20);
        bool passed = scenario();
        unsigned live_futexes = futex_test_live_count();
        if (live_futexes != 0) {
            fprintf(stderr, "%s 遗留 %u 个 futex 对象\n",
                    name, live_futexes);
            passed = false;
        }
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
        {"AArch64 futex 边界与线程私有键",
                test_edges_and_thread_private},
        {"AArch64 futex lazy 文件锁外预取",
                test_lazy_file_prefault_outside_global_lock},
        {"AArch64 futex 只读共享键权限",
                test_read_only_shared_key_permissions},
        {"AArch64 futex 独立文件键域",
                test_independent_file_key_domains},
        {"AArch64 futex fork 键域", test_fork_key_domains},
        {"AArch64 futex REQUEUE 语义", test_requeue_semantics},
        {"AArch64 futex REQUEUE 失败原子性", test_requeue_failures},
        {"AArch64 futex REQUEUE 超时", test_requeue_timeout},
    };
    for (size_t index = 0; index < array_size(scenarios); index++) {
        if (!run_isolated(
                scenarios[index].name, scenarios[index].function))
            return 1;
    }
    return 0;
}

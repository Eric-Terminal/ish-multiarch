#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-resource-wire.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define IMAGE_SIZE 1024
#define TEXT_BASE UINT64_C(0x400000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)
#define CLONE_SVC_STEP_COUNT 10
#define WNOHANG_CHECK_STEP_COUNT 37
#define PARENT_COMPLETION_STEP_LIMIT 48
#define WAITID_P_PID 1
#define WAIT_OPTION_WNOHANG UINT32_C(1)
#define WAIT_OPTION_WUNTRACED UINT32_C(2)
#define WAIT_OPTION_WAITID_EXITED UINT32_C(4)
#define WAIT_OPTION_WCONTINUED UINT32_C(8)
#define WAIT_OPTION_WNOWAIT UINT32_C(0x01000000)
#define THREAD_CLONE_FLAGS (UINT32_C(0x00000100) | \
        UINT32_C(0x00000200) | UINT32_C(0x00000400) | \
        UINT32_C(0x00000800) | UINT32_C(0x00010000) | \
        UINT32_C(0x00080000) | UINT32_C(0x00100000) | \
        UINT32_C(0x00200000) | UINT32_C(0x01000000))

struct exit_observation {
    atomic_bool called;
    atomic_bool resources_released;
    atomic_int pid;
    atomic_int status;
};

struct failed_write_observation {
    unsigned calls;
    qword_t address;
    dword_t size;
    dword_t status;
};

static struct exit_observation observed_exit;
static const struct fd_ops metadata_fd_ops = {0};

static void reset_exit_observation(void) {
    atomic_store_explicit(&observed_exit.called,
            false, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.resources_released,
            false, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.pid,
            0, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.status,
            0, memory_order_relaxed);
}

static bool exit_observation_matches(pid_t_ pid, int status) {
    return atomic_load_explicit(&observed_exit.called,
                    memory_order_acquire) &&
            atomic_load_explicit(&observed_exit.resources_released,
                    memory_order_relaxed) &&
            atomic_load_explicit(&observed_exit.pid,
                    memory_order_relaxed) == pid &&
            atomic_load_explicit(&observed_exit.status,
                    memory_order_relaxed) == status;
}

static bool wait_for_continue_notification(struct task *task) {
    for (unsigned i = 0; i < 100000; i++) {
        lock(&task->group->lock);
        bool pending = task->group->continue_notification_pending;
        unlock(&task->group->lock);
        if (!pending)
            return true;
        sched_yield();
    }
    return false;
}

static bool fail_status_write(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct failed_write_observation *observation = opaque;
    observation->calls++;
    observation->address = address;
    observation->size = size;
    if (size == sizeof(observation->status))
        memcpy(&observation->status, source, sizeof(observation->status));
    *fault = (struct guest_linux_user_fault) {
        .address = address + 2,
        .access = GUEST_MEMORY_WRITE,
        .kind = GUEST_MEMORY_FAULT_PERMISSION,
    };
    return false;
}

static bool resource_wire_is_fixed_width(void) {
    const struct rusage_ source = {
        .utime = {UINT32_C(0x7fffffff), UINT32_C(0x80000000)},
        .stime = {UINT32_C(0xffffffff), UINT32_C(0x12345678)},
        .maxrss = UINT32_C(0x80000001),
        .ixrss = 2,
        .idrss = 3,
        .isrss = 4,
        .minflt = 5,
        .majflt = 6,
        .nswap = 7,
        .inblock = 8,
        .oublock = 9,
        .msgsnd = 10,
        .msgrcv = 11,
        .nsignals = 12,
        .nvcsw = 13,
        .nivcsw = UINT32_C(0xfffffffe),
    };
    struct aarch64_linux_rusage wire =
            aarch64_linux_pack_rusage(&source);
    return wire.utime.sec == INT32_MAX &&
            wire.utime.usec == INT32_MIN &&
            wire.stime.sec == -1 &&
            wire.stime.usec == UINT32_C(0x12345678) &&
            wire.maxrss == (sqword_t) INT32_MIN + 1 &&
            wire.ixrss == 2 && wire.idrss == 3 &&
            wire.isrss == 4 && wire.minflt == 5 &&
            wire.majflt == 6 && wire.nswap == 7 &&
            wire.inblock == 8 && wire.oublock == 9 &&
            wire.msgsnd == 10 && wire.msgrcv == 11 &&
            wire.nsignals == 12 && wire.nvcsw == 13 &&
            wire.nivcsw == -2;
}

static void put_u16(byte_t *bytes, word_t value) {
    bytes[0] = (byte_t) value;
    bytes[1] = (byte_t) (value >> 8);
}

static void put_u32(byte_t *bytes, dword_t value) {
    for (byte_t i = 0; i < 4; i++)
        bytes[i] = (byte_t) (value >> (i * 8));
}

static void put_u64(byte_t *bytes, qword_t value) {
    for (byte_t i = 0; i < 8; i++)
        bytes[i] = (byte_t) (value >> (i * 8));
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

static void make_image(byte_t file[IMAGE_SIZE],
        const dword_t *program, size_t instruction_count) {
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

    assert(instruction_count <=
            (IMAGE_SIZE - ENTRY_OFFSET) / sizeof(*program));
    for (size_t i = 0; i < instruction_count; i++)
        put_u32(file + ENTRY_OFFSET + i * sizeof(*program), program[i]);
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
    // 子进程自旋；父进程先无阻塞探测，再用高位栈地址回收并核验 ABI 写回。
    static const dword_t program[] = {
        UINT32_C(0xd2800220), UINT32_C(0xf2fbd5a0),
        UINT32_C(0xd2800001), UINT32_C(0xd2844442),
        UINT32_C(0xf2f55542), UINT32_C(0x92800003),
        UINT32_C(0xd2888884), UINT32_C(0xf2d77764),
        UINT32_C(0xd2801b88), UINT32_C(0xd4000001),
        UINT32_C(0xb4000a40), UINT32_C(0xaa0003f3),
        UINT32_C(0xd102c3ff), UINT32_C(0xd28ef104),
        UINT32_C(0xf2aaacc4), UINT32_C(0xf2c66884),
        UINT32_C(0xf2e22444), UINT32_C(0xf90003e4),
        UINT32_C(0x92800005), UINT32_C(0xf9000be5),
        UINT32_C(0xf9001be5), UINT32_C(0xf9004fe5),
        UINT32_C(0xf90053e5), UINT32_C(0x92800000),
        UINT32_C(0xf2e24680), UINT32_C(0x910003e1),
        UINT32_C(0xd2800022), UINT32_C(0xf2f7dde2),
        UINT32_C(0x910043e3), UINT32_C(0xd2802088),
        UINT32_C(0xd4000001), UINT32_C(0xb5000720),
        UINT32_C(0xf94003e6), UINT32_C(0xeb0400df),
        UINT32_C(0x540006c1), UINT32_C(0xf9400be6),
        UINT32_C(0xeb0500df), UINT32_C(0x54000661),
        UINT32_C(0xf9401be6), UINT32_C(0xeb0500df),
        UINT32_C(0x54000601), UINT32_C(0xf9404fe6),
        UINT32_C(0xeb0500df), UINT32_C(0x540005a1),
        UINT32_C(0xf94053e6), UINT32_C(0xeb0500df),
        UINT32_C(0x54000541), UINT32_C(0x92800000),
        UINT32_C(0xf2eacf00), UINT32_C(0x910003e1),
        UINT32_C(0xd2800002), UINT32_C(0xf2f95fc2),
        UINT32_C(0x910043e3), UINT32_C(0xd2802088),
        UINT32_C(0xd4000001), UINT32_C(0xeb13001f),
        UINT32_C(0x54000401), UINT32_C(0xb94003e6),
        UINT32_C(0x710024df), UINT32_C(0x540003a1),
        UINT32_C(0xb94007e6), UINT32_C(0x52866887),
        UINT32_C(0x72a22447), UINT32_C(0x6b0700df),
        UINT32_C(0x54000301), UINT32_C(0xf9400be6),
        UINT32_C(0xeb0500df), UINT32_C(0x540002a0),
        UINT32_C(0xf9401be6), UINT32_C(0xb5000266),
        UINT32_C(0xf9404fe6), UINT32_C(0xb5000226),
        UINT32_C(0xf94053e6), UINT32_C(0xeb0500df),
        UINT32_C(0x540001c1), UINT32_C(0x92800000),
        UINT32_C(0xd2800001), UINT32_C(0xd2800022),
        UINT32_C(0xd2800003), UINT32_C(0xd2802088),
        UINT32_C(0xd4000001), UINT32_C(0x92800127),
        UINT32_C(0xeb07001f), UINT32_C(0x540000a1),
        UINT32_C(0xd2800540), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001), UINT32_C(0x14000006),
        UINT32_C(0xd2800c60), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001), UINT32_C(0x14000000),
        UINT32_C(0x14000000),
        // 仅目标 TLS 的线程进入 futex 等待；普通 fork 子进程继续自旋。
        UINT32_C(0xd53bd041), UINT32_C(0xd2824682),
        UINT32_C(0xeb02003f), UINT32_C(0x54000001),
        UINT32_C(0x910013e0), UINT32_C(0xd2801001),
        UINT32_C(0xd2800002), UINT32_C(0xd2800003),
        UINT32_C(0xd2800004), UINT32_C(0xd2800005),
        UINT32_C(0xd2800c48), UINT32_C(0xd4000001),
        UINT32_C(0xd2800000), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001),
    };
    byte_t file[IMAGE_SIZE];
    make_image(file, program, array_size(program));
    const char *arguments[] = {"clone-test"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    const struct aarch64_linux_process_config config = {
        .elf_data = file,
        .elf_size = sizeof(file),
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x600000),
        .executable = "/bin/clone-test",
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

static void observe_exit(struct task *task, int status) {
    bool resources_released = !task_has_aarch64_process(task) &&
            task->mm == NULL && task->files == NULL &&
            task->fs == NULL && task->sighand == NULL;
    atomic_store_explicit(&observed_exit.pid,
            task->pid, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.status,
            status, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.resources_released,
            resources_released, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.called,
            true, memory_order_release);
}

static bool write_guest_timespec(struct aarch64_linux_process *process,
        qword_t address, sqword_t sec, sqword_t nsec) {
    qword_t sec_bits = (qword_t) sec;
    qword_t nsec_bits = (qword_t) nsec;
    return aarch64_linux_process_write_u32(
            process, address, (dword_t) sec_bits, NULL) &&
            aarch64_linux_process_write_u32(
                    process, address + 4,
                    (dword_t) (sec_bits >> 32), NULL) &&
            aarch64_linux_process_write_u32(
                    process, address + 8,
                    (dword_t) nsec_bits, NULL) &&
            aarch64_linux_process_write_u32(
                    process, address + 12,
                    (dword_t) (nsec_bits >> 32), NULL);
}

static unsigned exercise_futex_edges(struct task *parent) {
    const qword_t word = STACK_TOP - UINT64_C(0x300);
    const qword_t timeout = word + 16;
    struct guest_linux_user_fault fault = {0};
    if (!aarch64_linux_process_write_u32(
            parent->aarch64_process, word, 7, &fault))
        return 1;
    if (sys_futex_aarch64(word, 128, 8, 0, 0, 0, &fault) !=
            (dword_t) _EAGAIN)
        return 2;

    const qword_t invalid = UINT64_C(1) << 48;
    fault = (struct guest_linux_user_fault) {0};
    if (sys_futex_aarch64(invalid, 128, 0, 0, 0, 0, &fault) !=
                    (dword_t) _EFAULT ||
            fault.address != invalid ||
            fault.access != GUEST_MEMORY_READ ||
            fault.kind != GUEST_MEMORY_FAULT_ADDRESS_SIZE)
        return 3;

    if (!write_guest_timespec(
            parent->aarch64_process, timeout, -1, 0) ||
            sys_futex_aarch64(word, 128, 7,
                    timeout, 0, 0, &fault) != (dword_t) _EINVAL)
        return 4;
    if (!write_guest_timespec(
            parent->aarch64_process, timeout, 0, 1000000) ||
            sys_futex_aarch64(word, 128, 7,
                    timeout, 0, 0, &fault) != (dword_t) _ETIMEDOUT)
        return 5;
    return sys_futex_aarch64(
            word, 129, 1, 0, 0, 0, &fault) == 0 ? 0 : 6;
}

static bool exercise_thread_clone(struct task *parent) {
    const qword_t child_stack = STACK_TOP - UINT64_C(0x200);
    const qword_t child_tid = child_stack;
    const qword_t child_ready = child_stack + sizeof(dword_t);
    const qword_t parent_tid = child_ready + sizeof(dword_t);
    const qword_t tls = UINT64_C(0x1234);
    struct guest_linux_user_fault fault = {0};
    if (!aarch64_linux_process_write_u32(
            parent->aarch64_process, child_tid, 0, &fault) ||
            !aarch64_linux_process_write_u32(
                    parent->aarch64_process, child_ready, 0, &fault) ||
            !aarch64_linux_process_write_u32(
                    parent->aarch64_process, parent_tid, 0, &fault))
        return false;

    dword_t encoded_pid = sys_clone_aarch64(
            THREAD_CLONE_FLAGS, child_stack,
            parent_tid, tls, child_tid, &fault);
    if ((sdword_t) encoded_pid <= 0)
        return false;
    pid_t_ pid = (pid_t_) encoded_pid;
    lock(&pids_lock);
    struct task *child = pid_get_task(pid);
    bool published = child != NULL && child->group == parent->group &&
            child->tgid == parent->tgid && child->mm == parent->mm &&
            child->files == parent->files && child->fs == parent->fs &&
            child->sighand == parent->sighand &&
            task_has_aarch64_process(child) &&
            child->aarch64_process != parent->aarch64_process &&
            aarch64_linux_process_uses_services(
                    child->aarch64_process, pid, child,
                    &ish_aarch64_linux_syscall_service,
                    &ish_aarch64_linux_signal_service) &&
            atomic_load_explicit(
                    &child->start_ready, memory_order_acquire);
    unlock(&pids_lock);
    dword_t observed_child_tid;
    dword_t observed_parent_tid;
    if (!published || !aarch64_linux_process_read_u32(
            parent->aarch64_process, child_tid,
            &observed_child_tid, &fault) ||
            !aarch64_linux_process_read_u32(
                    parent->aarch64_process, parent_tid,
                    &observed_parent_tid, &fault) ||
            observed_child_tid != (dword_t) pid ||
            observed_parent_tid != (dword_t) pid)
        return false;

    bool woken = false;
    for (unsigned i = 0; i < 100000 && !woken; i++) {
        dword_t wake_result = sys_futex_aarch64(
                child_ready, 129, 1, 0, 0, 0, &fault);
        if ((sdword_t) wake_result < 0)
            return false;
        woken = wake_result == 1;
        if (!woken)
            sched_yield();
    }
    if (!woken || !aarch64_linux_process_write_u32(
            parent->aarch64_process, child_ready, 1, &fault))
        return false;
    bool reaped = false;
    for (unsigned i = 0; i < 100000 && !reaped; i++) {
        lock(&pids_lock);
        reaped = pid_get_task_zombie(pid) == NULL;
        unlock(&pids_lock);
        if (!reaped)
            sched_yield();
    }
    dword_t cleared_child_tid;
    return reaped && aarch64_linux_process_read_u32(
            parent->aarch64_process, child_tid,
            &cleared_child_tid, &fault) && cleared_child_tid == 0;
}

static void clear_pending_signals(struct task *task) {
    struct sigqueue *queued;
    struct sigqueue *temporary;
    list_for_each_entry_safe(&task->queue,
            queued, temporary, queue) {
        list_remove(&queued->queue);
        free(queued);
    }
    task->pending = 0;
}

static void destroy_parent(struct task *parent,
        struct tgroup *group) {
    task_release_aarch64_process(parent);
    clear_pending_signals(parent);
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
    lock(&group->lock);
    list_remove(&parent->group_links);
    list_remove(&group->session);
    list_remove(&group->pgroup);
    task_destroy(parent);
    unlock(&group->lock);
    unlock(&pids_lock);
    cond_destroy(&group->stopped_cond);
    cond_destroy(&group->child_exit);
}

static int run_clone_scenario(void) {
    struct tgroup parent_group;
    struct task *parent = make_parent(&parent_group);
    if (parent == NULL)
        return 21;
    struct aarch64_linux_process *process = make_process(parent);
    if (process == NULL ||
            !task_attach_aarch64_process(parent, process))
        return 22;
    unsigned futex_edge_error = exercise_futex_edges(parent);
    if (futex_edge_error != 0)
        return 45 + (int) futex_edge_error;
    extern void (*exit_hook)(struct task *task, int code);
    reset_exit_observation();
    exit_hook = observe_exit;
    // 第十步执行真实 SVC；runtime 在返回前把父进程 x0 改为子 PID。
    for (unsigned i = 0; i < CLONE_SVC_STEP_COUNT; i++) {
        if (aarch64_task_run_one(parent).action !=
                AARCH64_TASK_EVENT_CONTINUE)
            return 23;
    }

    lock(&pids_lock);
    struct task *child = list_size(&parent->children) == 1 ?
            list_first_entry(&parent->children,
                    struct task, siblings) : NULL;
    pid_t_ child_pid = child == NULL ? 0 : child->pid;
    bool published = child != NULL && child_pid > 0 &&
            child->parent == parent &&
            child->pid == child_pid && child->tgid == child_pid &&
            child->group != parent->group &&
            child->group->leader == child &&
            child->exit_signal == SIGCHLD_ &&
            task_has_aarch64_process(child) &&
            child->aarch64_process != parent->aarch64_process &&
            aarch64_linux_process_uses_services(
                    child->aarch64_process, child_pid, child,
                    &ish_aarch64_linux_syscall_service,
                    &ish_aarch64_linux_signal_service) &&
            child->mm != parent->mm &&
            child->files != parent->files &&
            child->fs != parent->fs &&
            child->sighand != parent->sighand &&
            atomic_load_explicit(&child->start_ready,
                    memory_order_acquire) &&
            list_size(&parent->children) == 1;
    unlock(&pids_lock);
    if (!published)
        return 24;

    // 先走完 WNOHANG 及 canary 检查；此时子进程仍在自旋，结果必为 0。
    for (unsigned i = 0; i < WNOHANG_CHECK_STEP_COUNT; i++) {
        if (aarch64_task_run_one(parent).action !=
                AARCH64_TASK_EVENT_CONTINUE)
            return 25;
    }
    struct guest_linux_syscall_completion kill_completion = {0};
    const struct guest_linux_syscall_context kill_context = {
        .task_opaque = parent,
        .completion = &kill_completion,
    };
    const struct guest_linux_syscall kill_syscall = {
        .number = 129,
        .arguments = {(qword_t) (dword_t) child_pid, SIGKILL_},
    };
    struct guest_linux_user_fault kill_fault = {0};
    if (ish_aarch64_linux_syscall_service.dispatch(
            &kill_context, &kill_syscall, &kill_fault) != 0)
        return 26;
    struct aarch64_task_event parent_event = {
        .action = AARCH64_TASK_EVENT_CONTINUE,
    };
    for (unsigned i = 0;
            i < PARENT_COMPLETION_STEP_LIMIT && parent_event.action ==
                    AARCH64_TASK_EVENT_CONTINUE; i++)
        parent_event = aarch64_task_run_one(parent);
    exit_hook = NULL;
    if (parent_event.action != AARCH64_TASK_EVENT_EXIT ||
            parent_event.status != UINT32_C(42) << 8 ||
            !exit_observation_matches(child_pid, SIGKILL_))
        return 27;

    lock(&pids_lock);
    bool reaped = pid_get_task_zombie(child_pid) == NULL &&
            list_empty(&parent->children);
    unlock(&pids_lock);
    if (!reaped)
        return 28;

    struct wait4_result edge_result;
    if (do_wait4(INT32_MIN, 0, &edge_result) != _ESRCH ||
            do_wait4(-1, WAIT_OPTION_WAITID_EXITED,
                    &edge_result) != _EINVAL)
        return 29;

    dword_t encoded_fault_pid = sys_clone(SIGCHLD_, 0, 0, 0, 0);
    if ((sdword_t) encoded_fault_pid <= 0)
        return 30;
    pid_t_ fault_pid = (pid_t_) encoded_fault_pid;
    lock(&pids_lock);
    struct task *fault_child = pid_get_task(fault_pid);
    bool fault_child_published = fault_child != NULL &&
            fault_child->parent == parent &&
            task_has_aarch64_process(fault_child);
    unlock(&pids_lock);
    if (!fault_child_published)
        return 31;
    reset_exit_observation();
    exit_hook = observe_exit;
    send_signal(fault_child, SIGKILL_, SIGINFO_NIL);

    const qword_t failed_status_address = STACK_TOP - 4;
    struct failed_write_observation failed_write = {0};
    struct guest_linux_syscall_completion completion = {0};
    const struct guest_linux_syscall_context context = {
        .task_opaque = parent,
        .completion = &completion,
        .user = {
            .opaque = &failed_write,
            .write = fail_status_write,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = 260,
        .arguments = {
            (qword_t) (dword_t) fault_pid,
            failed_status_address,
            0,
            STACK_TOP - 160,
        },
    };
    struct guest_linux_user_fault fault = {0};
    qword_t fault_result = ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, &fault);
    exit_hook = NULL;
    if (fault_result != (qword_t) (sqword_t) _EFAULT ||
            failed_write.calls != 1 ||
            failed_write.address != failed_status_address ||
            failed_write.size != sizeof(dword_t) ||
            failed_write.status != SIGKILL_ ||
            fault.address != failed_status_address + 2 ||
            fault.access != GUEST_MEMORY_WRITE ||
            fault.kind != GUEST_MEMORY_FAULT_PERMISSION ||
            !exit_observation_matches(fault_pid, SIGKILL_) ||
            do_wait4(fault_pid, WAIT_OPTION_WNOHANG,
                    &edge_result) != _ECHILD)
        return 32;

    lock(&pids_lock);
    bool fault_child_reaped = pid_get_task_zombie(fault_pid) == NULL &&
            list_empty(&parent->children);
    unlock(&pids_lock);
    if (!fault_child_reaped)
        return 33;

    dword_t encoded_job_pid = sys_clone(SIGCHLD_, 0, 0, 0, 0);
    if ((sdword_t) encoded_job_pid <= 0)
        return 34;
    pid_t_ job_pid = (pid_t_) encoded_job_pid;
    lock(&pids_lock);
    struct task *job_child = pid_get_task(job_pid);
    bool job_child_published = job_child != NULL &&
            job_child->parent == parent &&
            task_has_aarch64_process(job_child);
    unlock(&pids_lock);
    if (!job_child_published)
        return 35;

    send_signal(job_child, SIGTSTP_, SIGINFO_NIL);
    dword_t stop_peek = sys_waitid(WAITID_P_PID, job_pid, 0,
            WAIT_OPTION_WUNTRACED | WAIT_OPTION_WNOWAIT);
    sdword_t stopped = do_wait4(
            job_pid, WAIT_OPTION_WUNTRACED, &edge_result);
    if (stop_peek != 0 || stopped != job_pid || edge_result.status !=
            ((dword_t) SIGTSTP_ << 8 | UINT32_C(0x7f)))
        return 36;
    send_signal(job_child, SIGCONT_, SIGINFO_NIL);
    dword_t continue_peek = sys_waitid(WAITID_P_PID, job_pid, 0,
            WAIT_OPTION_WCONTINUED | WAIT_OPTION_WNOWAIT);
    sdword_t continued = do_wait4(
            job_pid, WAIT_OPTION_WCONTINUED, &edge_result);
    bool continue_notified =
            wait_for_continue_notification(job_child);
    if (continue_peek != 0 || !continue_notified ||
            continued != job_pid ||
            edge_result.status != UINT32_C(0xffff) ||
            do_wait4(job_pid,
                    WAIT_OPTION_WNOHANG | WAIT_OPTION_WCONTINUED,
                    &edge_result) != 0)
        return 37;

    send_signal(job_child, SIGTSTP_, SIGINFO_NIL);
    stopped = do_wait4(
            job_pid, WAIT_OPTION_WUNTRACED, &edge_result);
    if (stopped != job_pid || edge_result.status !=
            ((dword_t) SIGTSTP_ << 8 | UINT32_C(0x7f)))
        return 38;
    send_signal(job_child, SIGCONT_, SIGINFO_NIL);
    if (!wait_for_continue_notification(job_child))
        return 39;
    send_signal(job_child, SIGTSTP_, SIGINFO_NIL);
    stopped = do_wait4(
            job_pid, WAIT_OPTION_WUNTRACED, &edge_result);
    if (stopped != job_pid || edge_result.status !=
            ((dword_t) SIGTSTP_ << 8 | UINT32_C(0x7f)) ||
            do_wait4(job_pid,
                    WAIT_OPTION_WNOHANG | WAIT_OPTION_WCONTINUED,
                    &edge_result) != 0)
        return 40;

    send_signal(job_child, SIGCONT_, SIGINFO_NIL);
    if (!wait_for_continue_notification(job_child))
        return 41;
    reset_exit_observation();
    exit_hook = observe_exit;
    send_signal(job_child, SIGKILL_, SIGINFO_NIL);
    bool job_exited = false;
    for (unsigned i = 0; i < 100000 && !job_exited; i++) {
        job_exited = atomic_load_explicit(
                &observed_exit.called, memory_order_acquire);
        if (!job_exited)
            sched_yield();
    }
    sdword_t killed = do_wait4(
            job_pid, WAIT_OPTION_WCONTINUED, &edge_result);
    exit_hook = NULL;
    if (!job_exited || killed != job_pid ||
            edge_result.status != SIGKILL_ ||
            !exit_observation_matches(job_pid, SIGKILL_))
        return 42;

    lock(&pids_lock);
    bool job_child_reaped = pid_get_task_zombie(job_pid) == NULL &&
            list_empty(&parent->children);
    unlock(&pids_lock);
    bool parent_unchanged = job_child_reaped &&
            atomic_load_explicit(&parent->mm->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&parent->files->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&parent->fs->refcount,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&parent->sighand->refcount,
                    memory_order_relaxed) == 1 &&
            aarch64_linux_process_uses_services(
                    parent->aarch64_process, parent->pid, parent,
                    &ish_aarch64_linux_syscall_service,
                    &ish_aarch64_linux_signal_service);
    if (!parent_unchanged)
        return 43;
    if (!exercise_thread_clone(parent))
        return 44;
    if (atomic_load_explicit(&parent->mm->refcount,
                    memory_order_relaxed) != 1 ||
            atomic_load_explicit(&parent->files->refcount,
                    memory_order_relaxed) != 1 ||
            atomic_load_explicit(&parent->fs->refcount,
                    memory_order_relaxed) != 1 ||
            atomic_load_explicit(&parent->sighand->refcount,
                    memory_order_relaxed) != 1)
        return 45;

    destroy_parent(parent, &parent_group);
    return 0;
}

static bool run_isolated_clone_scenario(void) {
    pid_t host_child = fork();
    if (host_child < 0)
        return false;
    if (host_child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(30);
        int result = run_clone_scenario();
        alarm(0);
        _exit(result);
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(host_child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != host_child) {
        fprintf(stderr, "等待 AArch64 clone 场景失败：%s\n",
                strerror(errno));
        return false;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "AArch64 clone 场景被 host 信号 %d 终止\n",
                WTERMSIG(status));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "AArch64 clone 场景返回状态 %d\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

int main(void) {
    if (!resource_wire_is_fixed_width()) {
        fprintf(stderr, "AArch64 rusage 固定宽度转换测试失败\n");
        return 1;
    }
    if (!run_isolated_clone_scenario()) {
        fprintf(stderr, "AArch64 clone 集成测试失败\n");
        return 1;
    }
    return 0;
}

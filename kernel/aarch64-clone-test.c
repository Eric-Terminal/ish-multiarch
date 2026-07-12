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
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/aarch64-task-runner.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define IMAGE_SIZE 1024
#define TEXT_BASE UINT64_C(0x400000)
#define ENTRY_OFFSET UINT64_C(0x200)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)

struct exit_observation {
    atomic_bool called;
    atomic_bool resources_released;
    atomic_int pid;
    atomic_int status;
};

static struct exit_observation observed_exit;
static const struct fd_ops metadata_fd_ops = {0};

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
    files->files[0] = fd_retain(metadata);
    mm->exefile = fd_retain(metadata);
    fs->root = fd_retain(metadata);
    fs->pwd = fd_retain(metadata);
    fd_close(metadata);
    task_set_mm(parent, mm);
    parent->files = files;
    parent->fs = fs;
    parent->sighand = sighand;
    task_thread_store(parent, pthread_self());
    task_publish(parent);
    current = parent;
    return parent;
}

static struct aarch64_linux_process *make_process(struct task *task) {
    // clone 的高位 flags 与 x2-x4 都是垃圾；父返回 PID 后退出，子进入自旋。
    static const dword_t program[] = {
        UINT32_C(0xd2800220), UINT32_C(0xf2fbd5a0),
        UINT32_C(0xd2800001), UINT32_C(0xd2844442),
        UINT32_C(0xf2f55542), UINT32_C(0x92800003),
        UINT32_C(0xd2888884), UINT32_C(0xf2d77764),
        UINT32_C(0xd2801b88), UINT32_C(0xd4000001),
        UINT32_C(0xb4000080), UINT32_C(0xd2800ba8),
        UINT32_C(0xd4000001), UINT32_C(0x14000000),
        UINT32_C(0x14000000),
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
    extern void (*exit_hook)(struct task *task, int code);
    atomic_store_explicit(&observed_exit.called,
            false, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.resources_released,
            false, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.pid,
            0, memory_order_relaxed);
    atomic_store_explicit(&observed_exit.status,
            0, memory_order_relaxed);
    exit_hook = observe_exit;
    // 第十步执行真实 SVC；runtime 在返回前把父进程 x0 改为子 PID。
    for (unsigned i = 0; i < 10; i++) {
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

    struct aarch64_task_event parent_event =
            aarch64_task_run_one(parent);
    if (parent_event.action != AARCH64_TASK_EVENT_CONTINUE ||
            aarch64_task_run_one(parent).action !=
                    AARCH64_TASK_EVENT_CONTINUE)
        return 25;
    parent_event = aarch64_task_run_one(parent);
    if (parent_event.action != AARCH64_TASK_EVENT_EXIT ||
            parent_event.status !=
                    ((dword_t) child_pid & UINT32_C(0xff)) << 8)
        return 26;

    send_signal(child, SIGKILL_, SIGINFO_NIL);
    dword_t waited = sys_wait4(child_pid, 0, 0, 0);
    exit_hook = NULL;
    if (waited != (dword_t) child_pid ||
            !atomic_load_explicit(&observed_exit.called,
                    memory_order_acquire) ||
            !atomic_load_explicit(&observed_exit.resources_released,
                    memory_order_relaxed) ||
            atomic_load_explicit(&observed_exit.pid,
                    memory_order_relaxed) != child_pid ||
            atomic_load_explicit(&observed_exit.status,
                    memory_order_relaxed) != SIGKILL_)
        return 27;

    lock(&pids_lock);
    bool reaped = pid_get_task_zombie(child_pid) == NULL &&
            list_empty(&parent->children);
    unlock(&pids_lock);
    bool parent_unchanged = reaped &&
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
        return 28;

    destroy_parent(parent, &parent_group);
    return 0;
}

static bool run_isolated_clone_scenario(void) {
    pid_t host_child = fork();
    if (host_child < 0)
        return false;
    if (host_child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(15);
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
    if (!run_isolated_clone_scenario()) {
        fprintf(stderr, "AArch64 clone 集成测试失败\n");
        return 1;
    }
    return 0;
}

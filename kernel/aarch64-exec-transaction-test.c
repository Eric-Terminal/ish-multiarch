#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/real.h"
#include "guest/aarch64/elf64.h"
#include "guest/aarch64/linux-process.h"
#include "kernel/aarch64-signal-service.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 exec 事务测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

#define IMAGE_SIZE 512
#define IMAGE_ENTRY_OFFSET UINT64_C(0x100)
#define IMAGE_ENTRY_ADDRESS UINT64_C(0x400100)
#define IMAGE_BASE UINT64_C(0x400000)
#define WAIT_MASK_OFFSET UINT64_C(0x180)
#define RT_SIGSUSPEND_SYSCALL 133
#define EXECVE_SYSCALL 221
#define EXEC_PATH_OFFSET UINT64_C(0x180)
#define EXEC_ARGUMENT_OFFSET UINT64_C(0x188)
#define EXEC_ARGV_OFFSET UINT64_C(0x190)
#define EXEC_ENVP_OFFSET UINT64_C(0x1a0)
#define STACK_TOP UINT64_C(0x00007fff00000000)
#define SIGNAL_TRAMPOLINE UINT64_C(0x00007ffe00000000)

#define CUSTOM_HANDLER UINT64_C(0x0000000000400200)
#define CUSTOM_RESTORER UINT64_C(0x0000000000400300)
#define IGNORE_RESTORER UINT64_C(0x0000000000400400)

struct exec_fixture {
    struct tgroup observer_group;
    struct tgroup exec_group;
    struct task *observer;
    struct task *leader;
};

struct peer_thread {
    struct task *task;
    pthread_t thread;
    atomic_bool ready;
};

struct close_order_probe {
    struct task *task;
    struct fdtable *private_files;
    struct sighand *private_sighand;
    struct mm *new_mm;
    struct aarch64_linux_process *new_process;
    struct signal_action unreset_action;
    struct signal_altstack old_altstack;
    uid_t_ old_euid;
    uid_t_ old_egid;
    char old_comm[16];
    int task_timer_signal;
    int group_timer_signal;
    bool called;
    bool state_valid;
};

static atomic_int ponr_exit_calls;
static atomic_int ponr_exit_code;
static atomic_size_t ponr_exit_buffer_sets;
static struct task *ponr_exit_task;

static int close_order_marker_close(struct fd *fd);
static struct aarch64_linux_process *make_old_process(
        struct task *task, bool execve_caller);

static const struct fd_ops marker_fd_ops = {0};
static const struct fd_ops close_order_marker_fd_ops = {
    .close = close_order_marker_close,
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

static dword_t movz_x(unsigned reg, word_t immediate) {
    return UINT32_C(0xd2800000) |
            (dword_t) immediate << 5 | (dword_t) reg;
}

static dword_t movk_x_lsl16(unsigned reg, word_t immediate) {
    return UINT32_C(0xf2a00000) |
            (dword_t) immediate << 5 | (dword_t) reg;
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

static void make_runnable_image(byte_t image[IMAGE_SIZE]) {
    memset(image, 0, IMAGE_SIZE);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2;
    image[5] = 1;
    image[6] = 1;
    image[7] = 3;
    put_u16(image + 16, 2);
    put_u16(image + 18, 183);
    put_u32(image + 20, 1);
    put_u64(image + 24, IMAGE_ENTRY_ADDRESS);
    put_u64(image + 32, AARCH64_ELF64_HEADER_SIZE);
    put_u16(image + 52, AARCH64_ELF64_HEADER_SIZE);
    put_u16(image + 54, AARCH64_ELF64_PROGRAM_HEADER_SIZE);
    put_u16(image + 56, 2);

    byte_t *headers = image + AARCH64_ELF64_HEADER_SIZE;
    qword_t header_size =
            2 * AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    put_program_header(headers, 6, 4,
            AARCH64_ELF64_HEADER_SIZE,
            IMAGE_BASE + AARCH64_ELF64_HEADER_SIZE,
            header_size, header_size, 8);
    put_program_header(headers + AARCH64_ELF64_PROGRAM_HEADER_SIZE,
            1, 5, 0, IMAGE_BASE,
            IMAGE_SIZE, IMAGE_SIZE, UINT64_C(0x1000));

    // mov x0, #42; mov x8, #93; svc #0
    put_u32(image + IMAGE_ENTRY_OFFSET, UINT32_C(0xd2800540));
    put_u32(image + IMAGE_ENTRY_OFFSET + 4, UINT32_C(0xd2800ba8));
    put_u32(image + IMAGE_ENTRY_OFFSET + 8, UINT32_C(0xd4000001));
}

static void make_blocking_image(byte_t image[IMAGE_SIZE]) {
    make_runnable_image(image);
    qword_t mask_address = IMAGE_BASE + WAIT_MASK_OFFSET;
    byte_t *program = image + IMAGE_ENTRY_OFFSET;

    // 在真实 guest syscall 中休眠，确保 exec 的内部 SIGKILL 走 AArch64 信号桥。
    put_u32(program, movz_x(0, (word_t) mask_address));
    put_u32(program + 4,
            movk_x_lsl16(0, (word_t) (mask_address >> 16)));
    put_u32(program + 8, movz_x(1, sizeof(sigset_t_)));
    put_u32(program + 12,
            movz_x(8, RT_SIGSUSPEND_SYSCALL));
    put_u32(program + 16, UINT32_C(0xd4000001));
    put_u32(program + 20, UINT32_C(0x17fffffb));
    put_u64(image + WAIT_MASK_OFFSET, 0);
}

static void make_execve_image(byte_t image[IMAGE_SIZE]) {
    make_runnable_image(image);
    qword_t path = IMAGE_BASE + EXEC_PATH_OFFSET;
    qword_t argument = IMAGE_BASE + EXEC_ARGUMENT_OFFSET;
    qword_t argv = IMAGE_BASE + EXEC_ARGV_OFFSET;
    qword_t envp = IMAGE_BASE + EXEC_ENVP_OFFSET;
    byte_t *program = image + IMAGE_ENTRY_OFFSET;

    qword_t addresses[] = {path, argv, envp};
    for (unsigned reg = 0; reg < array_size(addresses); reg++) {
        put_u32(program, movz_x(reg, (word_t) addresses[reg]));
        put_u32(program + 4,
                movk_x_lsl16(reg, (word_t) (addresses[reg] >> 16)));
        program += 8;
    }
    put_u32(program, movz_x(8, EXECVE_SYSCALL));
    put_u32(program + 4, UINT32_C(0xd4000001));
    // 失败若错误返回旧映像，就以 99 退出，不能伪装成 PONR 致命退出。
    put_u32(program + 8, movz_x(0, 99));
    put_u32(program + 12, movz_x(8, 93));
    put_u32(program + 16, UINT32_C(0xd4000001));

    memcpy(image + EXEC_PATH_OFFSET, "/new", sizeof("/new"));
    memcpy(image + EXEC_ARGUMENT_OFFSET, "/new", sizeof("/new"));
    put_u64(image + EXEC_ARGV_OFFSET, argument);
    put_u64(image + EXEC_ARGV_OFFSET + 8, 0);
    put_u64(image + EXEC_ENVP_OFFSET, 0);
}

static bool write_all(int fd, const void *buffer, size_t size) {
    const byte_t *cursor = buffer;
    while (size != 0) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        cursor += written;
        size -= (size_t) written;
    }
    return true;
}

static bool create_exec_image(char directory[static 64],
        char path[static 256], byte_t image[static IMAGE_SIZE]) {
    strcpy(directory, "/tmp/aarch64-exec-transaction-XXXXXX");
    if (mkdtemp(directory) == NULL ||
            snprintf(path, 256, "%s/new", directory) >= 256)
        return false;
    make_runnable_image(image);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (fd < 0)
        return false;
    bool written = write_all(fd, image, IMAGE_SIZE);
    if (close(fd) != 0)
        written = false;
    if (written && chmod(path, 06755) != 0)
        written = false;
    return written;
}

static void init_group(struct tgroup *group, struct task *leader,
        pid_t_ sid, pid_t_ pgid) {
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    list_init(&group->shared_queue);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = leader;
    group->sid = sid;
    group->pgid = pgid;
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {32, 32};
}

static struct task *make_observer(struct exec_fixture *fixture) {
    struct task *observer = task_create_(NULL);
    if (observer == NULL)
        return NULL;
    init_group(&fixture->observer_group, observer,
            observer->pid, observer->pid);
    observer->group = &fixture->observer_group;
    observer->tgid = observer->pid;
    observer->exit_signal = SIGCHLD_;

    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(4);
    struct fs_info *fs = fs_info_new();
    struct sighand *sighand = sighand_new();
    if (mm == NULL || IS_ERR(files) || fs == NULL || sighand == NULL)
        return NULL;
    task_set_mm(observer, mm);
    observer->files = files;
    observer->fs = fs;
    observer->sighand = sighand;
    task_thread_store(observer, pthread_self());
    task_publish(observer);
    current = observer;
    return observer;
}

static void retain_task_resources(struct task *task) {
    mm_retain(task->mm);
    atomic_fetch_add_explicit(
            &task->files->refcount, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(
            &task->fs->refcount, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(
            &task->sighand->refcount, 1, memory_order_relaxed);
}

static struct task *make_exec_leader(
        struct exec_fixture *fixture, bool old_aarch64) {
    struct task *leader = task_create_(fixture->observer);
    if (leader == NULL)
        return NULL;
    init_group(&fixture->exec_group, leader,
            fixture->observer_group.sid,
            fixture->observer_group.pgid);
    leader->group = &fixture->exec_group;
    leader->tgid = leader->pid;
    leader->exit_signal = SIGCHLD_;
    if (old_aarch64) {
        struct aarch64_linux_process *process =
                make_old_process(leader, false);
        if (process == NULL ||
                !task_attach_aarch64_process(leader, process)) {
            aarch64_linux_process_destroy(process);
            task_abort_create(leader);
            return NULL;
        }
    }
    retain_task_resources(leader);
    task_publish(leader);
    fixture->leader = leader;
    return leader;
}

static struct task *make_exec_thread(struct exec_fixture *fixture) {
    struct task *thread = task_create_(fixture->observer);
    if (thread == NULL)
        return NULL;
    thread->group = &fixture->exec_group;
    thread->tgid = fixture->leader->tgid;
    thread->exit_signal = 0;
    if (task_has_aarch64_process(fixture->leader)) {
        const struct aarch64_linux_process_thread_config config = {
            .tid = thread->pid,
            .task_opaque = thread,
        };
        struct aarch64_linux_process *process =
                aarch64_linux_process_clone_thread(
                        fixture->leader->aarch64_process,
                        &config, NULL);
        if (process == NULL ||
                !task_attach_aarch64_process(thread, process)) {
            aarch64_linux_process_destroy(process);
            task_abort_create(thread);
            return NULL;
        }
    }
    retain_task_resources(thread);
    task_publish(thread);
    return thread;
}

static bool setup_fixture(struct exec_fixture *fixture,
        const char *directory, bool old_aarch64) {
    *fixture = (struct exec_fixture) {0};
    fixture->observer = make_observer(fixture);
    if (fixture->observer == NULL ||
            mount_root(&realfs, directory) < 0)
        return false;
    current = fixture->observer;
    struct fd *root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(root))
        return false;
    fixture->observer->fs->root = root;
    fixture->observer->fs->pwd = fd_retain(root);
    return make_exec_leader(fixture, old_aarch64) != NULL;
}

static void *run_peer(void *opaque) {
    struct peer_thread *peer = opaque;
    current = peer->task;
    task_thread_store(current, pthread_self());
    atomic_store_explicit(&peer->ready, true, memory_order_release);
    if (task_has_aarch64_process(current)) {
        task_run_current();
    } else {
        sys_pause();
        receive_signals();
    }
    return (void *) 1;
}

static bool task_waits_on_pause(struct task *task) {
    lock(&task->waiting_cond_lock);
    bool waiting = task->waiting_cond == &task->pause;
    unlock(&task->waiting_cond_lock);
    return waiting;
}

static bool start_peer(struct peer_thread *peer, struct task *task) {
    *peer = (struct peer_thread) {
        .task = task,
        .ready = ATOMIC_VAR_INIT(false),
    };
    if (pthread_create(&peer->thread, NULL, run_peer, peer) != 0)
        return false;
    while (!atomic_load_explicit(&peer->ready, memory_order_acquire))
        sched_yield();
    while (!task_waits_on_pause(task))
        sched_yield();
    return true;
}

static bool join_peer(struct peer_thread *peer) {
    void *result = (void *) 1;
    return pthread_join(peer->thread, &result) == 0 && result == NULL;
}

static struct aarch64_linux_process *make_old_process(
        struct task *task, bool execve_caller) {
    byte_t image[IMAGE_SIZE];
    if (execve_caller)
        make_execve_image(image);
    else
        make_blocking_image(image);
    static const char *arguments[] = {"old-aarch64"};
    byte_t random[AARCH64_LINUX_PROCESS_RANDOM_SIZE] = {0};
    const struct aarch64_linux_process_config config = {
        .elf_data = image,
        .elf_size = IMAGE_SIZE,
        .stack_top = STACK_TOP,
        .stack_size = 2 * GUEST_MEMORY_PAGE_SIZE,
        .signal_trampoline_page = SIGNAL_TRAMPOLINE,
        .brk_limit = UINT64_C(0x600000),
        .executable = "/old-aarch64",
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

static bool prepare_shared_files(struct task *observer,
        fd_t *cloexec_number, fd_t *kept_number,
        struct fd **cloexec_marker, struct fd **kept_marker) {
    *cloexec_marker = fd_create(&marker_fd_ops);
    *kept_marker = fd_create(&marker_fd_ops);
    if (*cloexec_marker == NULL || *kept_marker == NULL)
        return false;
    *cloexec_number = f_install_task(
            observer, *cloexec_marker, O_CLOEXEC_);
    *kept_number = f_install_task(observer, *kept_marker, 0);
    return *cloexec_number >= 0 && *kept_number >= 0;
}

static bool prepare_shared_signals(struct task *observer) {
    const struct signal_action custom = {
        .handler = CUSTOM_HANDLER,
        .flags = SA_SIGINFO_ | SA_ONSTACK_,
        .restorer = CUSTOM_RESTORER,
        .mask = sig_mask(SIGTERM_),
    };
    const struct signal_action ignored = {
        .handler = SIG_IGN_,
        .flags = SA_NODEFER_,
        .restorer = IGNORE_RESTORER,
        .mask = sig_mask(SIGCHLD_),
    };
    return task_sigaction(observer, SIGUSR1_, &custom, NULL) == 0 &&
            task_sigaction(observer, SIGUSR2_, &ignored, NULL) == 0 &&
            task_sigaction(observer, SIGTRAP_, &ignored, NULL) == 0;
}

static bool action_is(const struct signal_action *action,
        qword_t handler, qword_t flags,
        qword_t restorer, sigset_t_ mask) {
    return action->handler == handler && action->flags == flags &&
            action->restorer == restorer && action->mask == mask;
}

static bool posix_timers_are_empty(const struct tgroup *group) {
    for (size_t index = 0; index < array_size(group->posix_timers); index++)
        if (group->posix_timers[index].timer != NULL ||
                group->posix_timers[index].deleting)
            return false;
    return true;
}

static int close_order_marker_close(struct fd *fd) {
    struct close_order_probe *probe = fd->data;
    struct task *task = probe->task;
    probe->called = true;
    probe->state_valid = task == current &&
            task->files == probe->private_files &&
            task->sighand == probe->private_sighand &&
            task->mm == probe->new_mm &&
            task->aarch64_process == probe->new_process &&
            posix_timers_are_empty(task->group) &&
            !sigset_has(task->pending, probe->task_timer_signal) &&
            !sigset_has(task->pending_timer_bit_only,
                    probe->task_timer_signal) &&
            !sigset_has(task->group->shared_pending,
                    probe->group_timer_signal) &&
            !sigset_has(task->group->shared_timer_bit_only,
                    probe->group_timer_signal) &&
            !sigset_has(task->pending, SIGTRAP_) &&
            !sigset_has(task->group->shared_pending, SIGTRAP_) &&
            task->altstack.stack == probe->old_altstack.stack &&
            task->altstack.size == probe->old_altstack.size &&
            task->altstack.flags == probe->old_altstack.flags &&
            task->euid == probe->old_euid &&
            task->suid == probe->old_euid &&
            task->egid == probe->old_egid &&
            task->sgid == probe->old_egid &&
            strcmp(task->comm, probe->old_comm) == 0 &&
            !task->did_exec &&
            action_is(&task->sighand->action[SIGUSR1_],
                    probe->unreset_action.handler,
                    probe->unreset_action.flags,
                    probe->unreset_action.restorer,
                    probe->unreset_action.mask);
    return 0;
}

static void unused_timer_callback(void *opaque) {
    (void) opaque;
}

static bool process_uses_final_identity(struct task *task) {
    return task_has_aarch64_process(task) &&
            aarch64_linux_process_uses_services(
                    task->aarch64_process, task->pid, task,
                    &ish_aarch64_linux_syscall_service,
                    &ish_aarch64_linux_signal_service);
}

static bool process_runs_to_exit(
        struct aarch64_linux_process *process) {
    for (unsigned step = 0; step < 2; step++) {
        if (aarch64_linux_process_run_one(process).status !=
                AARCH64_LINUX_PROCESS_RUNNABLE)
            return false;
    }
    struct aarch64_linux_process_result result =
            aarch64_linux_process_run_one(process);
    return result.status == AARCH64_LINUX_PROCESS_EXIT &&
            result.exit_status == 42;
}

static int verify_resource_isolation(struct exec_fixture *fixture,
        struct task *execer, struct fdtable *shared_files,
        struct sighand *shared_sighand,
        fd_t cloexec_number, fd_t kept_number,
        struct fd *cloexec_marker, struct fd *kept_marker) {
    struct signal_action exec_late;
    struct signal_action exec_ignored;
    struct signal_action observer_late;
    struct signal_action observer_ignored;
    CHECK(execer->files != shared_files &&
            fixture->observer->files == shared_files &&
            execer->sighand != shared_sighand &&
            fixture->observer->sighand == shared_sighand,
            "execer 获得私有 files 与 sighand，旁观进程保留共享对象");
    CHECK(f_get_task(execer, cloexec_number) == NULL &&
            f_get_task(fixture->observer,
                    cloexec_number) == cloexec_marker &&
            f_get_task(execer, kept_number) == kept_marker &&
            f_get_task(fixture->observer, kept_number) == kept_marker,
            "CLOEXEC 只关闭 execer 私有表项，普通表项保留");

    CHECK(task_sigaction(execer, SIGUSR1_, NULL, &exec_late) == 0 &&
            task_sigaction(execer, SIGUSR2_, NULL, &exec_ignored) == 0 &&
            task_sigaction(fixture->observer,
                    SIGUSR1_, NULL, &observer_late) == 0 &&
            task_sigaction(fixture->observer,
                    SIGUSR2_, NULL, &observer_ignored) == 0,
            "读取 execer 与旁观进程的信号动作");
    CHECK(action_is(&exec_late, SIG_IGN_, 0, 0, 0) &&
            action_is(&exec_ignored, SIG_IGN_, 0, 0, 0),
            "exec 使用安全点快照并仅保留忽略 disposition");
    CHECK(action_is(&observer_late, SIG_IGN_,
                    SA_NODEFER_ | SA_ONSTACK_, CUSTOM_RESTORER,
                    sig_mask(SIGINT_)) &&
            action_is(&observer_ignored, SIG_IGN_,
                    SA_NODEFER_, IGNORE_RESTORER,
                    sig_mask(SIGCHLD_)),
            "跨线程组共享 sighand 不受 handler reset 污染");
    CHECK(execer->altstack.stack == 0 && execer->altstack.size == 0 &&
            execer->altstack.flags == SS_DISABLE_,
            "exec 清空备用信号栈");

    CHECK(f_close_task(execer, kept_number) == 0 &&
            f_get_task(fixture->observer, kept_number) == kept_marker,
            "execer 关闭普通 fd 不影响旁观进程");
    struct fd *exec_only = fd_create(&marker_fd_ops);
    CHECK(exec_only != NULL, "创建 execer 私有 fd 标记");
    fd_t exec_only_number = f_install_task(execer, exec_only, 0);
    CHECK(exec_only_number >= 0 &&
            f_get_task(execer, exec_only_number) == exec_only &&
            f_get_task(fixture->observer,
                    exec_only_number) != exec_only,
            "execer 安装新 fd 不写入旁观进程表");

    const struct signal_action exec_only_action = {
        .handler = UINT64_C(0x500100),
        .flags = SA_NODEFER_,
        .mask = sig_mask(SIGINT_),
    };
    CHECK(task_sigaction(execer, SIGTERM_,
                    &exec_only_action, NULL) == 0,
            "在 execer 私有 sighand 写入新动作");
    struct signal_action observer_term;
    CHECK(task_sigaction(fixture->observer,
                    SIGTERM_, NULL, &observer_term) == 0 &&
            action_is(&observer_term, SIG_DFL_, 0, 0, 0),
            "execer 的后续信号动作不回写旁观进程");

    const struct signal_action observer_only_action = {
        .handler = CUSTOM_HANDLER,
        .flags = SA_SIGINFO_,
        .restorer = CUSTOM_RESTORER,
        .mask = sig_mask(SIGTERM_),
    };
    CHECK(task_sigaction(fixture->observer, SIGUSR1_,
                    &observer_only_action, NULL) == 0,
            "在旁观进程共享 sighand 写入新动作");
    CHECK(task_sigaction(execer,
                    SIGUSR1_, NULL, &exec_late) == 0 &&
            action_is(&exec_late, SIG_IGN_, 0, 0, 0),
            "旁观进程的后续信号动作不回写 execer");
    CHECK(f_close_task(execer, exec_only_number) == 0,
            "清理 execer 私有 fd 标记");
    return 0;
}

static int run_exec_transaction(bool old_aarch64,
        bool nonleader) {
    char directory[64];
    char path[256];
    byte_t image[IMAGE_SIZE];
    CHECK(create_exec_image(directory, path, image),
            "创建真实 AArch64 ELF 映像");
    struct stat executable_stat;
    CHECK(stat(path, &executable_stat) == 0,
            "读取 set-id AArch64 ELF 元数据");

    struct exec_fixture fixture;
    CHECK(setup_fixture(&fixture, directory, old_aarch64),
            "建立两个线程组与真实文件系统");

    fd_t cloexec_number;
    fd_t kept_number;
    struct fd *cloexec_marker;
    struct fd *kept_marker;
    CHECK(prepare_shared_files(fixture.observer,
                    &cloexec_number, &kept_number,
                    &cloexec_marker, &kept_marker) &&
            prepare_shared_signals(fixture.observer),
            "建立跨线程组共享 files 与 sighand 状态");
    struct fdtable *shared_files = fixture.observer->files;
    struct sighand *shared_sighand = fixture.observer->sighand;

    struct task *execer = fixture.leader;
    struct task *ordinary_peer = make_exec_thread(&fixture);
    CHECK(ordinary_peer != NULL, "创建普通同组线程");
    struct task *nonleader_task = NULL;
    if (nonleader) {
        nonleader_task = make_exec_thread(&fixture);
        CHECK(nonleader_task != NULL, "创建非 leader execer");
        execer = nonleader_task;
    }
    CHECK(task_has_aarch64_process(fixture.leader) == old_aarch64 &&
            task_has_aarch64_process(ordinary_peer) == old_aarch64 &&
            task_has_aarch64_process(execer) == old_aarch64,
            "旧 exec 线程组使用同一种 guest 架构");
    if (old_aarch64) {
        qword_t memory_identity =
                aarch64_linux_process_memory_identity(
                        fixture.leader->aarch64_process);
        CHECK(aarch64_linux_process_memory_identity(
                        ordinary_peer->aarch64_process) ==
                            memory_identity &&
                aarch64_linux_process_memory_identity(
                        execer->aarch64_process) == memory_identity &&
                process_uses_final_identity(fixture.leader) &&
                process_uses_final_identity(ordinary_peer) &&
                process_uses_final_identity(execer),
                "旧 AArch64 线程共享映像并绑定各自 TID 与 task");
    }

    uid_t_ old_euid = (uid_t_) executable_stat.st_uid ^ 1;
    uid_t_ old_egid = (uid_t_) executable_stat.st_gid ^ 1;
    execer->euid = execer->suid = old_euid;
    execer->egid = execer->sgid = old_egid;
    strcpy(execer->comm, "old-exec");
    execer->did_exec = false;
    execer->blocked = sig_mask(SIGWINCH_);
    execer->altstack = (struct signal_altstack) {
        .stack = UINT64_C(0x700000),
        .size = UINT64_C(0x8000),
    };
    lock(&execer->ptrace.lock);
    execer->ptrace.traced = true;
    unlock(&execer->ptrace.lock);

    pid_t_ old_tgid = fixture.leader->pid;
    pid_t_ caller_tid = execer->pid;
    pid_t_ ordinary_tid = ordinary_peer->pid;
    struct mm *shared_mm = fixture.observer->mm;
    struct peer_thread peers[2];
    size_t peer_count = 0;
    CHECK(start_peer(&peers[peer_count++], ordinary_peer),
            "启动普通同组 host 线程");
    if (nonleader) {
        CHECK(start_peer(&peers[peer_count++], fixture.leader),
                "启动旧 leader host 线程");
    }

    current = execer;
    task_thread_store(execer, pthread_self());
    static const char arguments[] = "/new\0";
    static const char environment[] = "\0";
    int exec_error = do_execve(
            "/new", 1, arguments, environment);
    if (exec_error != 0)
        fprintf(stderr, "真实 do_execve 返回 %d\n", exec_error);
    CHECK(exec_error == 0,
            "多线程真实 do_execve 建立 AArch64 候选");
    CHECK(task_has_aarch64_exec_candidate(execer),
            "真实入口只在安全点前保留候选映像");
    for (size_t index = 0; index < peer_count; index++)
        CHECK(join_peer(&peers[index]),
                "被 exec 清理的同组线程正常退出");

    struct fdtable *private_files = execer->files;
    struct sighand *reserved_sighand =
            execer->exec_transition.sighand;
    struct mm *new_mm = execer->exec_transition.mm;
    struct aarch64_linux_process *new_process =
            execer->exec_transition.process;
    uid_t_ target_euid = execer->exec_transition.euid;
    uid_t_ target_egid = execer->exec_transition.egid;
    struct signal_action before_commit_action;
    CHECK(execer->exec_transition.begun &&
            execer->exec_transition.ready &&
            private_files != shared_files &&
            fixture.observer->files == shared_files &&
            f_get_task(execer, cloexec_number) == cloexec_marker &&
            f_get_task(fixture.observer,
                    cloexec_number) == cloexec_marker,
            "PONR 后 files 已私有但尚未执行 CLOEXEC");
    CHECK(execer->sighand == shared_sighand &&
            fixture.observer->sighand == shared_sighand &&
            reserved_sighand != NULL &&
            !sigset_has(execer->pending, SIGTRAP_) &&
            !sigset_has(fixture.exec_group.shared_pending, SIGTRAP_) &&
            task_sigaction(execer, SIGUSR1_, NULL,
                    &before_commit_action) == 0 &&
            action_is(&before_commit_action, CUSTOM_HANDLER,
                    SA_SIGINFO_ | SA_ONSTACK_, CUSTOM_RESTORER,
                    sig_mask(SIGTERM_)),
            "安全点前保留共享 sighand 与原信号动作");
    CHECK(execer->altstack.stack == UINT64_C(0x700000) &&
            execer->altstack.size == UINT64_C(0x8000) &&
            execer->altstack.flags == 0 &&
            execer->euid == old_euid && execer->suid == old_euid &&
            execer->egid == old_egid && execer->sgid == old_egid &&
            strcmp(execer->comm, "old-exec") == 0 &&
            !execer->did_exec && target_euid != old_euid,
            "安全点前不发布信号栈、凭据、comm 与 did_exec");
    CHECK(atomic_load_explicit(&shared_files->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&private_files->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&shared_sighand->refcount,
                    memory_order_acquire) == 2 &&
            atomic_load_explicit(&reserved_sighand->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&shared_mm->refcount,
                    memory_order_acquire) == 2 &&
            atomic_load_explicit(&new_mm->refcount,
                    memory_order_acquire) == 1,
            "PONR 后释放 peer 引用并保留活动映像引用");

    const struct signal_action late_ignored = {
        .handler = SIG_IGN_,
        .flags = SA_NODEFER_ | SA_ONSTACK_,
        .restorer = CUSTOM_RESTORER,
        .mask = sig_mask(SIGINT_),
    };
    CHECK(task_sigaction(fixture.observer, SIGUSR1_,
                    &late_ignored, NULL) == 0 &&
            task_sigaction(execer, SIGUSR1_, NULL,
                    &before_commit_action) == 0 &&
            action_is(&before_commit_action, SIG_IGN_,
                    SA_NODEFER_ | SA_ONSTACK_, CUSTOM_RESTORER,
                    sig_mask(SIGINT_)),
            "旁观进程在安全点前更新最终信号 disposition");

    struct timer *exec_timer = timer_new(
            CLOCK_MONOTONIC, unused_timer_callback, NULL);
    CHECK(exec_timer != NULL, "创建 exec 顺序探针 POSIX timer");
    fixture.exec_group.posix_timers[0] = (struct posix_timer) {
        .timer = exec_timer,
        .timer_id = 0,
        .tgroup = &fixture.exec_group,
        .signal = SIGALRM_,
    };
    lock(&execer->sighand->lock);
    sigset_add(&execer->pending, SIGALRM_);
    sigset_add(&execer->pending_timer_bit_only, SIGALRM_);
    sigset_add(&fixture.exec_group.shared_pending, SIGVTALRM_);
    sigset_add(&fixture.exec_group.shared_timer_bit_only, SIGVTALRM_);
    unlock(&execer->sighand->lock);

    struct close_order_probe close_probe = {
        .task = execer,
        .private_files = private_files,
        .private_sighand = reserved_sighand,
        .new_mm = new_mm,
        .new_process = new_process,
        .unreset_action = late_ignored,
        .old_altstack = execer->altstack,
        .old_euid = old_euid,
        .old_egid = old_egid,
        .old_comm = "old-exec",
        .task_timer_signal = SIGALRM_,
        .group_timer_signal = SIGVTALRM_,
    };
    struct fd *close_order_marker =
            fd_create(&close_order_marker_fd_ops);
    CHECK(close_order_marker != NULL,
            "创建 CLOEXEC close 顺序探针");
    close_order_marker->data = &close_probe;
    fd_t close_order_number = f_install_task(
            execer, close_order_marker, O_CLOEXEC_);
    CHECK(close_order_number >= 0 &&
            f_get_task(execer, close_order_number) ==
                    close_order_marker &&
            f_get_task(fixture.observer, close_order_number) !=
                    close_order_marker,
            "把 close 顺序探针安装到 execer 私有 CLOEXEC 表项");

    task_commit_aarch64_exec(execer);
    CHECK(close_probe.called && close_probe.state_valid &&
            f_get_task(execer, close_order_number) == NULL,
            "CLOEXEC close 位于映像发布与公共 exec 副作用之间");
    CHECK(!task_has_aarch64_exec_candidate(execer) &&
            process_uses_final_identity(execer),
            "候选提交后的服务 TID 等于最终 leader PID");
    struct signal_action exec_trap_action;
    struct siginfo_ exec_trap_info;
    lock(&execer->sighand->lock);
    bool task_trap_pending = sigset_has(execer->pending, SIGTRAP_);
    bool group_trap_pending = sigset_has(
            fixture.exec_group.shared_pending, SIGTRAP_);
    bool took_exec_trap = signal_take_unblocked_locked(
            execer, 0, &exec_trap_info);
    unlock(&execer->sighand->lock);
    lock(&execer->ptrace.lock);
    bool still_traced = execer->ptrace.traced;
    unlock(&execer->ptrace.lock);
    CHECK(task_trap_pending && !group_trap_pending && took_exec_trap &&
            exec_trap_info.sig == SIGTRAP_ &&
            exec_trap_info.code == SI_USER_ &&
            exec_trap_info.payload_kind == SIGNAL_INFO_PAYLOAD_KILL &&
            exec_trap_info.kill.pid == old_tgid &&
            exec_trap_info.kill.uid == execer->uid && still_traced &&
            task_sigaction(execer, SIGTRAP_, NULL,
                    &exec_trap_action) == 0 &&
            action_is(&exec_trap_action, SIG_IGN_, 0, 0, 0),
            "ptrace exec trap 在公共状态完成后仍无视 SIG_IGN 精确排队");
    lock(&execer->ptrace.lock);
    execer->ptrace.traced = false;
    unlock(&execer->ptrace.lock);
    CHECK(execer->pid == old_tgid && execer->tgid == old_tgid &&
            fixture.exec_group.leader == execer &&
            fixture.exec_group.exec_task == NULL,
            "execer 成为唯一 leader 并保留原 TGID");
    lock(&pids_lock);
    bool only_execer = list_size(&fixture.exec_group.threads) == 1 &&
            pid_get_task(old_tgid) == execer &&
            pid_get_task_zombie(ordinary_tid) == NULL &&
            (!nonleader || pid_get_task_zombie(caller_tid) == NULL);
    unlock(&pids_lock);
    CHECK(only_execer,
            "线程组最终只剩 execer，所有已清理 peer 的旧 TID 消失");
    CHECK(execer->blocked == sig_mask(SIGWINCH_),
            "exec 保留线程信号屏蔽字");
    CHECK(execer->files == private_files &&
            execer->sighand == reserved_sighand &&
            execer->mm == new_mm &&
            atomic_load_explicit(&shared_files->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&private_files->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&shared_sighand->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&reserved_sighand->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&shared_mm->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&new_mm->refcount,
                    memory_order_acquire) == 1,
            "提交释放旧 files、sighand 与 mm 引用且不泄漏新对象");
    CHECK(execer->euid == target_euid && execer->suid == target_euid &&
            execer->egid == target_egid && execer->sgid == target_egid &&
            strcmp(execer->comm, "new") == 0 && execer->did_exec,
            "安全点统一发布 set-id 凭据、comm 与 did_exec");
    CHECK(verify_resource_isolation(&fixture, execer,
                    shared_files, shared_sighand,
                    cloexec_number, kept_number,
                    cloexec_marker, kept_marker) == 0,
            "验证 exec 事务资源隔离");
    CHECK(process_runs_to_exit(execer->aarch64_process),
            "提交后的真实 AArch64 映像运行到退出码 42");
    CHECK(unlink(path) == 0 && rmdir(directory) == 0,
            "清理真实 exec 测试映像");
    return 0;
}

static int run_i386_leader_exec(void) {
    return run_exec_transaction(false, false);
}

static int run_i386_nonleader_exec(void) {
    return run_exec_transaction(false, true);
}

static int run_aarch64_leader_exec(void) {
    return run_exec_transaction(true, false);
}

static int run_aarch64_nonleader_exec(void) {
    return run_exec_transaction(true, true);
}

static void observe_ponr_exit(struct task *task, int code) {
    ponr_exit_task = task;
    atomic_store_explicit(&ponr_exit_buffer_sets,
            ish_aarch64_execve_test_live_host_buffer_sets(),
            memory_order_release);
    atomic_store_explicit(&ponr_exit_code, code, memory_order_release);
    atomic_fetch_add_explicit(&ponr_exit_calls, 1, memory_order_release);
}

static void *run_ponr_failure_exec(void *opaque) {
    current = opaque;
    task_thread_store(current, pthread_self());
    task_run_current();
    return (void *) 1;
}

static int run_ponr_sighand_failure(void) {
    char directory[64];
    char path[256];
    byte_t image[IMAGE_SIZE];
    CHECK(create_exec_image(directory, path, image),
            "创建 PONR 失败路径的真实 AArch64 ELF");

    struct exec_fixture fixture;
    CHECK(setup_fixture(&fixture, directory, true),
            "建立旧 AArch64 执行映像与共享资源");
    task_release_aarch64_process(fixture.leader);
    struct aarch64_linux_process *execve_caller =
            make_old_process(fixture.leader, true);
    CHECK(execve_caller != NULL &&
            task_attach_aarch64_process(fixture.leader, execve_caller),
            "安装经真实 svc 发起 execve 的旧 AArch64 映像");
    CHECK(task_has_aarch64_process(fixture.leader) &&
            atomic_load_explicit(&fixture.leader->sighand->refcount,
                    memory_order_acquire) > 1 &&
            ish_aarch64_execve_test_live_host_buffer_sets() == 0,
            "确保 sighand 预留故障与 host 参数清理具有稳定前置条件");

    atomic_store_explicit(&ponr_exit_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&ponr_exit_code, 0, memory_order_relaxed);
    atomic_store_explicit(&ponr_exit_buffer_sets, 0, memory_order_relaxed);
    ponr_exit_task = NULL;
    exit_hook = observe_ponr_exit;
    task_exec_test_fail_sighand_reservation_once();
    current = fixture.observer;
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL,
                    run_ponr_failure_exec, fixture.leader) == 0,
            "启动 PONR 失败路径 exec worker");
    void *worker_result = (void *) 1;
    int join_error = pthread_join(worker, &worker_result);
    exit_hook = NULL;

    CHECK(join_error == 0 && worker_result == NULL &&
            atomic_load_explicit(
                    &ponr_exit_calls, memory_order_acquire) == 1 &&
            atomic_load_explicit(
                    &ponr_exit_code, memory_order_acquire) == SIGSEGV_ &&
            atomic_load_explicit(&ponr_exit_buffer_sets,
                    memory_order_acquire) == 1 &&
            ish_aarch64_execve_test_live_host_buffer_sets() == 0 &&
            ponr_exit_task == fixture.leader &&
            fixture.exec_group.doing_group_exit &&
            fixture.exec_group.group_exit_code == SIGSEGV_ &&
            fixture.leader->exiting &&
            !task_has_aarch64_exec_candidate(fixture.leader) &&
            !task_has_aarch64_process(fixture.leader) &&
            fixture.leader->exec_transition.mm == NULL &&
            fixture.leader->exec_transition.sighand == NULL &&
            fixture.leader->mm == NULL && fixture.leader->files == NULL &&
            fixture.leader->fs == NULL && fixture.leader->sighand == NULL &&
            atomic_load_explicit(&fixture.observer->mm->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&fixture.observer->files->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&fixture.observer->fs->refcount,
                    memory_order_acquire) == 1 &&
            atomic_load_explicit(&fixture.observer->sighand->refcount,
                    memory_order_acquire) == 1,
            "真实 svc 的 PONR ENOMEM 致命退出并在线程退场时释放 host 参数");
    CHECK(unlink(path) == 0 && rmdir(directory) == 0,
            "清理 PONR 失败路径测试映像");
    return 0;
}

static int run_isolated(const char *name, int (*scenario)(void)) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "%s 无法 fork：%s\n", name, strerror(errno));
        return 1;
    }
    if (child == 0) {
        alarm(20);
        signal(SIGUSR1, SIG_IGN);
        _exit(scenario());
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0)
        return 0;
    if (waited == child && WIFSIGNALED(status)) {
        fprintf(stderr, "%s 被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
    } else {
        fprintf(stderr, "%s 返回状态 %d\n", name,
                waited == child && WIFEXITED(status) ?
                        WEXITSTATUS(status) : -1);
    }
    return 1;
}

int main(void) {
    if (run_isolated("i386 leader 到 AArch64 exec 事务",
                run_i386_leader_exec) != 0)
        return 1;
    if (run_isolated("i386 非 leader 到 AArch64 exec 事务",
                run_i386_nonleader_exec) != 0)
        return 1;
    if (run_isolated("AArch64 leader 到 AArch64 exec 事务",
                run_aarch64_leader_exec) != 0)
        return 1;
    if (run_isolated("AArch64 非 leader 到 AArch64 exec 事务",
                run_aarch64_nonleader_exec) != 0)
        return 1;
    return run_isolated("AArch64 PONR sighand ENOMEM 不返回",
            run_ponr_sighand_failure);
}

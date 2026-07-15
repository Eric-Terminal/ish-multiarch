#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/elf.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 多线程 exec 收敛测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct exec_fixture {
    struct tgroup parent_group;
    struct tgroup process_group;
    struct task *parent;
    struct task *leader;
};

struct peer_thread {
    struct task *task;
    atomic_bool ready;
};

enum committed_exec_entry {
    COMMITTED_EXEC_HOST,
    COMMITTED_EXEC_SYSCALL,
};

struct committed_exec_request {
    struct task *task;
    enum committed_exec_entry entry;
    addr_t filename;
    addr_t argv;
    addr_t envp;
};

static atomic_int committed_exit_calls;
static atomic_int committed_exit_code;
static struct task *committed_exit_task;

static void init_group(
        struct tgroup *group, struct task *leader,
        pid_t_ sid, pid_t_ pgid) {
    *group = (struct tgroup) {0};
    list_init(&group->threads);
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

static struct task *make_parent(struct exec_fixture *fixture) {
    struct task *parent = task_create_(NULL);
    if (parent == NULL)
        return NULL;

    init_group(&fixture->parent_group, parent,
            parent->pid, parent->pid);
    parent->group = &fixture->parent_group;
    parent->tgid = parent->pid;
    parent->exit_signal = SIGCHLD_;

    struct mm *mm = mm_new();
    struct fdtable *files = fdtable_new(1);
    struct fs_info *fs = fs_info_new();
    struct sighand *sighand = sighand_new();
    if (mm == NULL || IS_ERR(files) || fs == NULL || sighand == NULL)
        return NULL;
    task_set_mm(parent, mm);
    parent->files = files;
    parent->fs = fs;
    parent->sighand = sighand;
    task_thread_store(parent, pthread_self());
    task_publish(parent);
    current = parent;
    return parent;
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

static struct task *make_process_leader(
        struct exec_fixture *fixture) {
    struct task *leader = task_create_(fixture->parent);
    if (leader == NULL)
        return NULL;

    init_group(&fixture->process_group, leader,
            fixture->parent_group.sid, fixture->parent_group.pgid);
    leader->group = &fixture->process_group;
    leader->tgid = leader->pid;
    leader->exit_signal = SIGCHLD_;
    retain_task_resources(leader);
    task_publish(leader);
    fixture->leader = leader;
    return leader;
}

static struct task *make_group_thread(
        struct exec_fixture *fixture) {
    struct task *thread = task_create_(fixture->parent);
    if (thread == NULL)
        return NULL;
    thread->group = &fixture->process_group;
    thread->tgid = fixture->leader->tgid;
    thread->exit_signal = 0;
    retain_task_resources(thread);
    task_publish(thread);
    return thread;
}

static bool setup_fixture(struct exec_fixture *fixture) {
    *fixture = (struct exec_fixture) {0};
    fixture->parent = make_parent(fixture);
    return fixture->parent != NULL &&
            make_process_leader(fixture) != NULL;
}

static void *run_exec_peer(void *opaque) {
    struct peer_thread *peer = opaque;
    current = peer->task;
    task_thread_store(current, pthread_self());
    atomic_store_explicit(&peer->ready, true, memory_order_release);

    /* SIGKILL 会唤醒 pause，并经 do_exit_group 命中 exec 同组退场路径。 */
    sys_pause();
    receive_signals();
    return (void *) 1;
}

static bool start_exec_peer(
        struct peer_thread *peer, struct task *task,
        pthread_t *host_thread) {
    *peer = (struct peer_thread) {
        .task = task,
        .ready = ATOMIC_VAR_INIT(false),
    };
    if (pthread_create(host_thread, NULL, run_exec_peer, peer) != 0)
        return false;
    while (!atomic_load_explicit(&peer->ready, memory_order_acquire))
        sched_yield();
    return true;
}

static bool join_exec_peer(pthread_t host_thread) {
    void *result = (void *) 1;
    return pthread_join(host_thread, &result) == 0 && result == NULL;
}

static bool only_child_is(
        struct task *parent, struct task *expected) {
    if (list_size(&parent->children) != 1)
        return false;
    struct task *child = list_first_entry(
            &parent->children, struct task, siblings);
    return child == expected && expected->parent == parent &&
            expected->parent != expected &&
            expected->siblings.next != &expected->siblings &&
            expected->siblings.prev != &expected->siblings;
}

static int run_leader_exec(void) {
    struct exec_fixture fixture;
    CHECK(setup_fixture(&fixture), "创建 leader exec 测试夹具");
    struct task *leader = fixture.leader;
    struct task *peer_task = make_group_thread(&fixture);
    CHECK(peer_task != NULL, "创建 leader 的同组线程");

    pid_t_ leader_pid = leader->pid;
    pid_t_ peer_tid = peer_task->pid;

    struct peer_thread peer;
    pthread_t host_thread;
    CHECK(start_exec_peer(&peer, peer_task, &host_thread),
            "启动 leader 的同组 host 线程");

    current = leader;
    task_thread_store(leader, pthread_self());
    CHECK(task_exec_dethread(leader) == 0,
            "leader 等待并收敛同组线程");
    CHECK(join_exec_peer(host_thread),
            "同组线程经 exec 专用退出路径结束");

    lock(&pids_lock);
    bool identity_ok = pid_get_task(leader_pid) == leader &&
            pid_get_task_zombie(peer_tid) == NULL &&
            list_size(&fixture.process_group.threads) == 1 &&
            fixture.process_group.leader == leader &&
            fixture.process_group.exec_task == NULL;
    bool parent_chain_ok = only_child_is(fixture.parent, leader);
    unlock(&pids_lock);
    CHECK(identity_ok, "leader 保持 PID 并清除旧 peer TID");
    CHECK(parent_chain_ok, "leader 收敛后父子链保持单项且无自环");
    CHECK(!fixture.process_group.doing_group_exit,
            "peer 的 do_exit_group 不把 execer 标成组退出");
    return 0;
}

static int run_nonleader_exec(void) {
    struct exec_fixture fixture;
    CHECK(setup_fixture(&fixture), "创建非 leader exec 测试夹具");
    struct task *old_leader = fixture.leader;
    struct task *execer = make_group_thread(&fixture);
    struct task *peer_task = make_group_thread(&fixture);
    CHECK(execer != NULL && peer_task != NULL,
            "创建非 leader execer 与普通 peer");

    pid_t_ old_tgid = old_leader->pid;
    pid_t_ caller_tid = execer->pid;
    pid_t_ peer_tid = peer_task->pid;
    pid_t_ sid = fixture.process_group.sid;
    pid_t_ pgid = fixture.process_group.pgid;

    old_leader->blocked |= sig_mask(SIGUSR1_);
    execer->blocked |= sig_mask(SIGUSR1_);
    peer_task->blocked |= sig_mask(SIGUSR1_);
    struct siginfo_ process_info = {
        .code = SI_QUEUE_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_QUEUE,
        .queue.pid = fixture.parent->pid,
        .queue.uid = fixture.parent->uid,
        .queue.value = UINT64_C(0x123456789abcdef0),
    };
    lock(&pids_lock);
    send_process_signal(old_leader, SIGUSR1_, process_info);
    unlock(&pids_lock);
    CHECK(sigset_has(fixture.process_group.shared_pending, SIGUSR1_) &&
            list_size(&fixture.process_group.shared_queue) == 1,
            "在 de-thread 前建立进程定向共享 pending");

    struct peer_thread leader_peer;
    struct peer_thread ordinary_peer;
    pthread_t leader_host_thread;
    pthread_t ordinary_host_thread;
    CHECK(start_exec_peer(&leader_peer,
                    old_leader, &leader_host_thread),
            "启动旧 leader host 线程");
    CHECK(start_exec_peer(&ordinary_peer,
                    peer_task, &ordinary_host_thread),
            "启动普通 peer host 线程");

    current = execer;
    task_thread_store(execer, pthread_self());
    CHECK(task_exec_dethread(execer) == 0,
            "非 leader 等待 peer 并接管旧 TGID");
    CHECK(join_exec_peer(leader_host_thread) &&
                    join_exec_peer(ordinary_host_thread),
            "旧 leader 与普通 peer 均完成退出");

    lock(&pids_lock);
    bool identity_ok = execer->pid == old_tgid &&
            execer->tgid == old_tgid &&
            execer->exit_signal == SIGCHLD_ &&
            fixture.process_group.leader == execer &&
            fixture.process_group.exec_task == NULL &&
            pid_get_task(old_tgid) == execer &&
            pid_get_task_zombie(caller_tid) == NULL &&
            pid_get_task_zombie(peer_tid) == NULL &&
            list_size(&fixture.process_group.threads) == 1;
    bool parent_chain_ok = only_child_is(fixture.parent, execer);
    unlock(&pids_lock);
    CHECK(identity_ok,
            "非 leader 接管旧 TGID 且旧调用者 TID 完全消失");
    CHECK(parent_chain_ok,
            "非 leader 接管身份后父子链保持单项且无自环");
    CHECK(fixture.process_group.sid == sid &&
                    fixture.process_group.pgid == pgid,
            "身份接管不改变会话与进程组编号");
    CHECK(!fixture.process_group.doing_group_exit,
            "多个 peer 的 do_exit_group 不误杀 execer");

    lock(&execer->sighand->lock);
    struct siginfo_ preserved_info;
    bool preserved = signal_take_unblocked_locked(
            execer, 0, &preserved_info);
    unlock(&execer->sighand->lock);
    CHECK(preserved && preserved_info.sig == SIGUSR1_ &&
            preserved_info.code == SI_QUEUE_ &&
            preserved_info.queue.value == process_info.queue.value &&
            fixture.process_group.shared_pending == 0 &&
            list_empty(&fixture.process_group.shared_queue),
            "peer 退出与 leader PID 换位不丢失进程定向 pending");
    return 0;
}

static bool write_all_host(int fd, const void *buffer, size_t size) {
    const char *cursor = buffer;
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

static bool create_committed_failure_image(
        char directory[static 1], char path[static 1]) {
    strcpy(directory, "/tmp/i386-exec-wrapper-XXXXXX");
    if (mkdtemp(directory) == NULL)
        return false;
    if (snprintf(path, 256, "%s/bad", directory) >= 256)
        return false;

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (fd < 0)
        return false;
    struct elf_header header = {
        .bitness = ELF_32BIT,
        .endian = ELF_LITTLEENDIAN,
        .elfversion1 = 1,
        .type = ELF_EXECUTABLE,
        .machine = ELF_X86,
        .elfversion2 = 1,
        .entry_point = UINT32_C(0x08048000),
        .prghead_off = sizeof(struct elf_header),
        .header_size = sizeof(struct elf_header),
        .phent_size = sizeof(struct prg_header),
        .phent_count = 1,
    };
    memcpy(&header.magic, ELF_MAGIC, sizeof(header.magic));
    const struct prg_header segment = {
        .type = PT_LOAD,
        .vaddr = UINT32_C(0x08048000),
        .flags = PH_R | PH_X,
        .alignment = PAGE_SIZE,
        // 零长度映射只在旧 mm 退休之后失败，稳定命中 committed-fatal。
    };
    bool written = write_all_host(fd, &header, sizeof(header)) &&
            write_all_host(fd, &segment, sizeof(segment));
    if (close(fd) != 0)
        written = false;
    return written;
}

static bool attach_fixture_root(
        struct exec_fixture *fixture, const char *directory) {
    if (mount_root(&realfs, directory) < 0)
        return false;
    current = fixture->parent;
    struct fd *root = generic_open("/", O_RDONLY_, 0);
    if (IS_ERR(root))
        return false;
    fixture->parent->fs->root = root;
    fixture->parent->fs->pwd = fd_retain(root);
    return true;
}

#define EXEC_ARGUMENT_PAGE UINT32_C(0x00100000)
#define EXEC_FILENAME_ADDRESS (EXEC_ARGUMENT_PAGE + 32)
#define EXEC_ARGUMENT_ADDRESS (EXEC_ARGUMENT_PAGE + 64)
#define EXEC_ARGV_ADDRESS (EXEC_ARGUMENT_PAGE + 128)
#define EXEC_ENVP_ADDRESS (EXEC_ARGUMENT_PAGE + 160)

static bool prepare_sys_exec_arguments(struct task *task) {
    current = task;
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(
            task->mem, PAGE(EXEC_ARGUMENT_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    if (error < 0)
        return false;

    static const char filename[] = "/bad";
    addr_t argv[] = {EXEC_ARGUMENT_ADDRESS, 0};
    addr_t envp[] = {0};
    return user_write(EXEC_FILENAME_ADDRESS,
                    filename, sizeof(filename)) == 0 &&
            user_write(EXEC_ARGUMENT_ADDRESS,
                    filename, sizeof(filename)) == 0 &&
            user_write(EXEC_ARGV_ADDRESS, argv, sizeof(argv)) == 0 &&
            user_write(EXEC_ENVP_ADDRESS, envp, sizeof(envp)) == 0;
}

static void observe_committed_exit(struct task *task, int code) {
    committed_exit_task = task;
    atomic_store_explicit(
            &committed_exit_code, code, memory_order_release);
    atomic_fetch_add_explicit(
            &committed_exit_calls, 1, memory_order_release);
}

static void *run_committed_exec(void *opaque) {
    struct committed_exec_request *request = opaque;
    current = request->task;
    task_thread_store(current, pthread_self());
    if (request->entry == COMMITTED_EXEC_SYSCALL) {
        (void) sys_execve(
                request->filename, request->argv, request->envp);
    } else {
        static const char arguments[] = "/bad\0\0";
        static const char environment[] = "\0";
        (void) do_execve("/bad", 1, arguments, environment);
    }
    return (void *) 1;
}

static int run_committed_exec_failure(
        enum committed_exec_entry entry) {
    char directory[64];
    char path[256];
    CHECK(create_committed_failure_image(directory, path),
            "创建提交后失败的 i386 ELF");

    struct exec_fixture fixture;
    CHECK(setup_fixture(&fixture), "创建 committed-fatal 测试夹具");
    CHECK(attach_fixture_root(&fixture, directory),
            "挂载 committed-fatal 测试根文件系统");
    if (entry == COMMITTED_EXEC_SYSCALL)
        CHECK(prepare_sys_exec_arguments(fixture.leader),
                "映射 sys_execve 的 guest 参数");

    atomic_store_explicit(&committed_exit_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&committed_exit_code, 0, memory_order_relaxed);
    committed_exit_task = NULL;
    exit_hook = observe_committed_exit;
    struct committed_exec_request request = {
        .task = fixture.leader,
        .entry = entry,
        .filename = EXEC_FILENAME_ADDRESS,
        .argv = EXEC_ARGV_ADDRESS,
        .envp = EXEC_ENVP_ADDRESS,
    };
    current = fixture.parent;
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL,
                    run_committed_exec, &request) == 0,
            "启动 committed-fatal exec worker");
    void *worker_result = (void *) 1;
    CHECK(pthread_join(worker, &worker_result) == 0,
            "等待 committed-fatal exec worker");
    exit_hook = NULL;

    CHECK(worker_result == NULL &&
            atomic_load_explicit(
                    &committed_exit_calls, memory_order_acquire) == 1 &&
            atomic_load_explicit(
                    &committed_exit_code, memory_order_acquire) == SIGSEGV_ &&
            committed_exit_task == fixture.leader &&
            fixture.process_group.doing_group_exit &&
            fixture.process_group.group_exit_code == SIGSEGV_,
            "公开 exec 包装层消费内部哨兵并以 SIGSEGV 终止进程");
    CHECK(unlink(path) == 0 && rmdir(directory) == 0,
            "清理 committed-fatal 测试映像");
    return 0;
}

static int run_host_committed_exec_failure(void) {
    return run_committed_exec_failure(COMMITTED_EXEC_HOST);
}

static int run_syscall_committed_exec_failure(void) {
    return run_committed_exec_failure(COMMITTED_EXEC_SYSCALL);
}

static int run_isolated(
        const char *name, int (*scenario)(void)) {
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "%s 无法 fork：%s\n", name, strerror(errno));
        return 1;
    }
    if (child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(15);
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
    if (run_isolated("leader exec 收敛测试", run_leader_exec) != 0)
        return 1;
    if (run_isolated(
                "非 leader exec 收敛测试", run_nonleader_exec) != 0)
        return 1;
    if (run_isolated("do_execve committed-fatal 测试",
                run_host_committed_exec_failure) != 0)
        return 1;
    return run_isolated("sys_execve committed-fatal 测试",
            run_syscall_committed_exec_failure);
}

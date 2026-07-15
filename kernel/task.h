#ifndef TASK_H
#define TASK_H

#include <pthread.h>
#include <stdatomic.h>
#include "emu/cpu.h"
#include "kernel/mm.h"
#include "kernel/fs.h"
#include "kernel/signal.h"
#include "kernel/resource.h"
#include "fs/sockrestart.h"
#include "util/list.h"
#include "util/timer.h"
#include "util/sync.h"

struct aarch64_linux_process;

// exec 候选映像在 guest 执行栈返回安全点后统一提交。
struct task_exec_transition {
    struct aarch64_linux_process *process;
    struct mm *mm;
    struct sighand *sighand;
    uid_t_ euid, egid;
    char comm[16] __strncpy_safe;
    bool begun;
    bool ready;
};

// everything here is private to the thread executing this task and needs no
// locking, unless otherwise specified
struct task {
    struct cpu_state cpu;
    // 非空时由 task 独占；AArch64 CPU 与页表封装在 opaque process 中。
    struct aarch64_linux_process *aarch64_process;
    // exec 候选在当前 guest 执行栈返回安全点前不得替换或销毁活动映像。
    struct task_exec_transition exec_transition;
    struct mm *mm; // locked by general_lock
    struct mem *mem; // pointer to mm.mem, for convenience
    pthread_t thread; // 并发访问必须使用 task_thread_load/store。
    atomic_bool start_ready; // host 线程在完整发布前等待。
    uint64_t threadid;

    struct tgroup *group; // immutable
    struct list group_links;
    // pid/tgid 通常不可变；非 leader exec 在 pids_lock 下接管 leader PID。
    pid_t_ pid, tgid;
    uid_t_ uid, gid;
    uid_t_ euid, egid;
    uid_t_ suid, sgid;
#define MAX_GROUPS 32
    unsigned ngroups;
    uid_t_ groups[MAX_GROUPS];
    char comm[16] __strncpy_safe; // locked by general_lock
    bool did_exec; // for that one annoying setsid edge case

    struct fdtable *files;
    struct fs_info *fs;

    // locked by sighand->lock
    struct sighand *sighand;
    sigset_t_ blocked;
    sigset_t_ pending;
    // 无队列节点的 pending 仍需记录来源，供 exec 只清理 POSIX timer。
    sigset_t_ pending_bit_only;
    sigset_t_ pending_timer_bit_only;
    sigset_t_ waiting; // if nonzero, an ongoing call to sigtimedwait is waiting on these
    struct list queue;
    cond_t pause; // please don't signal this
    // private
    sigset_t_ saved_mask;
    bool has_saved_mask;
    // guest 地址宽度独立于 arm64_32 等 host ABI。
    struct signal_altstack altstack;

    struct {
        // Locks all ptrace-related things
        lock_t lock;
        cond_t cond;

        bool traced;
        bool stopped;
        int signal;
        struct siginfo_ info;
        int trap_event;
    } ptrace;

    // locked by pids_lock
    struct task *parent;
    struct list children;
    struct list siblings;

    addr_t clear_tid;
    addr_t robust_list;
    // AArch64 guest 始终使用 LP64 robust-list 指针，与 host ABI 无关。
    qword_t aarch64_robust_list; // 由 pids_lock 保护。

    // locked by pids_lock
    dword_t exit_code;
    bool zombie;
    bool exiting;

    // this structure is allocated on the stack of the parent's clone() call
    struct vfork_info {
        bool done;
        cond_t cond;
        lock_t lock;
    } *vfork;
    int exit_signal;

    // lock for anything that needs locking but is not covered by some other lock
    // specifically: comm, mm
    lock_t general_lock;

    struct task_sockrestart sockrestart;

    // current condition/lock, so it can be notified in case of a signal
    cond_t *waiting_cond;
    lock_t *waiting_lock;
    // 仅供单等待者 poll 使用；发送信号时写入非阻塞通知管道。
    bool waiting_poll_active;
    int waiting_poll_notify_fd;
    lock_t waiting_cond_lock;
};

static inline pthread_t task_thread_load(const struct task *task) {
    return __atomic_load_n(&task->thread, __ATOMIC_ACQUIRE);
}

static inline void task_thread_store(struct task *task, pthread_t thread) {
    __atomic_store_n(&task->thread, thread, __ATOMIC_RELEASE);
}

// current will always give the process that is currently executing
// if I have to stop using __thread, current will become a macro
extern __thread struct task *current;

static inline void task_set_mm(struct task *task, struct mm *mm) {
    task->mm = mm;
    task->mem = &task->mm->mem;
    task->cpu.mmu = &task->mem->mmu;
}

bool task_has_aarch64_process(const struct task *task);
// 这些入口仅由未发布 task 的构造线程或 task 自己的执行线程调用。
// attach 仅在生产服务闭包匹配时接管所有权；失败时所有权仍归调用方。
// stage 同时接管 process 与预分配 metadata mm，commit 只能在执行栈安全点调用。
// take 将所有权交还调用方且不销毁。
bool task_attach_aarch64_process(struct task *task,
        struct aarch64_linux_process *process);
bool task_stage_aarch64_exec(struct task *task,
        struct aarch64_linux_process *process, struct mm *mm,
        uid_t_ euid, uid_t_ egid, const char *executable);
// stage 仍可回滚；begin 置位 PONR，之后的错误只能终止当前映像。
int task_begin_aarch64_exec(struct task *task);
bool task_has_aarch64_exec_candidate(const struct task *task);
void task_commit_aarch64_exec(struct task *task);
void task_discard_aarch64_exec(struct task *task);
struct aarch64_linux_process *task_take_aarch64_process(
        struct task *task);
void task_release_aarch64_process(struct task *task);

// Creates a new process, initializes most fields from the parent. Specify
// parent as NULL to create the init process. Returns NULL if out of memory.
// Ends with an underscore because there's a mach function by the same name
struct task *task_create_(struct task *parent);
// 调用方必须依次持有 pids_lock 与 task->group->lock。
void task_publish_locked(struct task *task);
void task_publish(struct task *task);
void task_abort_create(struct task *task);
// Removes the process from the process table and frees it. Must be called with pids_lock.
void task_destroy(struct task *task);
// 收敛 exec 线程组；EINTR 表示执行者同时收到 SIGKILL。
int task_exec_dethread(struct task *task);
// 仅在 PONR 后调用；返回失败时旧映像不得继续运行。
int task_exec_unshare_files(struct task *task);
// 常驻的一次性故障注入仅用于验证 PONR 后的 sighand 分配失败路径。
void task_exec_test_fail_sighand_reservation_once(void);
// 完成两种 guest 共用的 exec 身份、文件、信号与通知副作用。
// 接管 retired_mm，并在公共状态提交后、ptrace 通知前退休旧映像。
void task_finish_exec(struct task *task, uid_t_ euid, uid_t_ egid,
        const char *executable, struct mm *retired_mm);

// misc
pid_t_ task_getpid(const struct task *task);
pid_t_ task_gettid(const struct task *task);
pid_t_ task_getppid(const struct task *task);
void vfork_notify(struct task *task);
pid_t_ task_setsid(struct task *task);
void task_leave_session(struct task *task);

struct posix_timer {
    struct timer *timer;
    bool deleting;
    int_t timer_id;
    struct tgroup *tgroup;
    pid_t_ thread_pid;
    int_t signal;
    union sigval_ sig_value;
};

// struct thread_group is way too long to type comfortably
struct tgroup {
    struct list threads; // locked by pids_lock, by majority vote
    // 非 leader exec 只可在 pids_lock 下接管 leader 身份。
    struct task *leader;
    // 非空时禁止发布新线程，并让被清理线程只退出自身。
    struct task *exec_task; // locked by pids_lock
    // 已生成的未阻塞默认致死信号；内部 exec zap 不写入。
    atomic_int external_fatal_signal;
    // 进程定向 pending 属于线程组，不随单个 peer 退出丢失。
    sigset_t_ shared_pending; // locked by the shared sighand
    struct list shared_queue; // locked by the shared sighand
    sigset_t_ shared_bit_only; // locked by the shared sighand
    sigset_t_ shared_timer_bit_only; // locked by the shared sighand
    struct rusage_ rusage;

    // locked by pids_lock
    pid_t_ sid, pgid;
    struct list session;
    struct list pgroup;

    bool stopped;
    dword_t stop_code;
    bool continued;
    bool continue_notification_pending;
    cond_t stopped_cond;

    struct tty *tty;
    struct timer *itimer;
#define TIMERS_MAX 16
    struct posix_timer posix_timers[TIMERS_MAX];

    struct rlimit_ limits[RLIMIT_NLIMITS_];

    // From https://twitter.com/tblodt/status/957706819236904960
    // > there are two distinct ways for a p̶r̶o̶c̶e̶s̶s̶ thread group to exit:
    // > 
    // > - each thread calls exit
    // > wait will return the exit code for the group leader
    // > 
    // > - any thread calls exit_group
    // > the SIGNAL_GROUP_EXIT flag will be set and wait will return the status passed to exit_group
    //
    // TODO locking
    bool doing_group_exit;
    dword_t group_exit_code;

    struct rusage_ children_rusage;
    cond_t child_exit;

    dword_t personality;

    // for everything in this struct not locked by something else
    lock_t lock;
};

static inline bool task_is_leader(struct task *task) {
    return task->group->leader == task;
}

int task_group_fatal_signal(const struct task *task);

struct pid {
    dword_t id;
    bool reserved;
    struct task *task;
    struct list session;
    struct list pgroup;
};

// synchronizes obtaining a pointer to a task and freeing that task
extern lock_t pids_lock;
// these functions must be called with pids_lock
struct pid *pid_get(dword_t pid);
struct task *pid_get_task(dword_t pid);
struct task *pid_get_task_zombie(dword_t id); // don't return null if the task exists as a zombie
struct task *task_process_representative_locked(struct task *task);
struct task *pid_get_process_task(dword_t id);

#define MAX_PID (1 << 15) // oughta be enough

// 建立 host 线程但暂不允许其执行 guest；用于完整发布与 ptrace 通知。
void task_start_suspended(struct task *task);
// 放行已建立的 host 线程；调用返回后不得再假定 task 仍然存活。
void task_release_start(struct task *task);
// 建立并立即放行 host 线程。
void task_start(struct task *task);
void task_run_current(void);

extern void (*exit_hook)(struct task *task, int code);

#define superuser() (current != NULL && current->euid == 0)

// Update the thread name to match the current task, in the format "comm-pid".
// Will ensure that the -pid part always fits, then will fit as much of comm as possible.
void update_thread_name(void);

#endif

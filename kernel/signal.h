#ifndef SIGNAL_H
#define SIGNAL_H

#include "misc.h"
#include "kernel/signal-info.h"
#include "util/list.h"
#include "util/sync.h"
struct task;
struct tgroup;

typedef qword_t sigset_t_;

#define SIG_ERR_ -1
#define SIG_DFL_ 0
#define SIG_IGN_ 1

#define SA_NOCLDSTOP_ 1
#define SA_NOCLDWAIT_ 2
#define SA_SIGINFO_ 4
#define SA_ONSTACK_ UINT64_C(0x08000000)
#define SA_NODEFER_ 0x40000000
#define SA_RESETHAND_ 0x80000000

struct signal_action {
    qword_t handler;
    qword_t flags;
    qword_t restorer;
    sigset_t_ mask;
};

struct i386_sigaction {
    dword_t handler;
    dword_t flags;
    dword_t restorer;
    sigset_t_ mask;
} __attribute__((packed));
_Static_assert(sizeof(struct i386_sigaction) == 20 &&
        __builtin_offsetof(struct i386_sigaction, handler) == 0 &&
        __builtin_offsetof(struct i386_sigaction, flags) == 4 &&
        __builtin_offsetof(struct i386_sigaction, restorer) == 8 &&
        __builtin_offsetof(struct i386_sigaction, mask) == 12,
        "i386 rt_sigaction wire 必须固定为 20 字节");
_Static_assert(sizeof(struct signal_action) == 32,
        "内部信号动作必须完整保存四个 64 位字段");

#define NUM_SIGS 64

#define	SIGHUP_    1
#define	SIGINT_    2
#define	SIGQUIT_   3
#define	SIGILL_    4
#define	SIGTRAP_   5
#define	SIGABRT_   6
#define	SIGIOT_    6
#define	SIGBUS_    7
#define	SIGFPE_    8
#define	SIGKILL_   9
#define	SIGUSR1_   10
#define	SIGSEGV_   11
#define	SIGUSR2_   12
#define	SIGPIPE_   13
#define	SIGALRM_   14
#define	SIGTERM_   15
#define	SIGSTKFLT_ 16
#define	SIGCHLD_   17
#define	SIGCONT_   18
#define	SIGSTOP_   19
#define	SIGTSTP_   20
#define	SIGTTIN_   21
#define	SIGTTOU_   22
#define	SIGURG_    23
#define	SIGXCPU_   24
#define	SIGXFSZ_   25
#define	SIGVTALRM_ 26
#define	SIGPROF_   27
#define	SIGWINCH_  28
#define	SIGIO_     29
#define	SIGPWR_    30
#define SIGSYS_    31
#define SIGRTMIN_  32
#define SIGRTMAX_  NUM_SIGS

#define SI_USER_ 0
#define SI_QUEUE_ -1
#define SI_TIMER_ -2
#define SI_TKILL_ -6
#define SI_KERNEL_ 128
#define TRAP_TRACE_ 2
#define ILL_ILLOPC_ 1
#define BUS_ADRALN_ 1
#define SEGV_MAPERR_ 1
#define SEGV_ACCERR_ 2

union sigval_ {
    sdword_t sv_int;
    dword_t sv_ptr;
};
_Static_assert(sizeof(union sigval_) == 4,
        "i386 sigevent 的 sigval wire 必须固定为 4 字节");

// a reasonable default siginfo
static const struct siginfo_ SIGINFO_NIL = {
    .code = SI_KERNEL_,
    .payload_kind = SIGNAL_INFO_PAYLOAD_NONE,
};

struct sigqueue {
    struct list queue;
    struct siginfo_ info;
    struct signal_pending_account *account;
};

enum signal_queue_policy {
    SIGNAL_QUEUE_FORCE,
    SIGNAL_QUEUE_LEGACY,
    SIGNAL_QUEUE_EXPLICIT,
};

enum signal_enqueue_result {
    SIGNAL_ENQUEUE_COALESCED,
    SIGNAL_ENQUEUE_QUEUED,
    SIGNAL_ENQUEUE_BIT_ONLY,
};

// 调用方持有目标 sighand 锁；uid 与 limit 必须在拿锁前快照。
int signal_enqueue_locked(struct task *task, int signal,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, qword_t limit);
int signal_enqueue_process_locked(struct task *representative, int signal,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, qword_t limit);
// 调用方持有任务共享的 sighand 锁；返回线程与进程两级 pending 并集。
sigset_t_ signal_pending_mask_locked(struct task *task);
// 释放节点时同步归还其 real UID pending 配额。
void signal_queue_release(struct sigqueue *queued);
// 调用方保证队列不被并发修改；所有节点经统一扣账路径释放。
void signal_flush_pending(struct task *task);
void signal_flush_group_pending(struct tgroup *group);
// POSIX timer 已同步退休后，清除其私有与进程级 SI_TIMER 排队节点。
void signal_flush_exec_timer_pending(struct task *task);
// 调用方持有 sighand 锁；清空一个信号的节点与 bit-only pending 位。
void signal_discard_pending_locked(struct task *task, int signal);
void signal_discard_group_pending_locked(
        struct tgroup *group, int signal);
void signal_group_pending_init(struct tgroup *group);

// 仅供故障注入测试；SIZE_MAX 表示恢复正常分配。
void signal_queue_test_fail_allocation_at(size_t index);

// 删除一个队列节点，并仅在同号节点全部消费后清除 pending 位。
void signal_dequeue_locked(
        struct task *task, struct sigqueue *queued);
// 选择并消费一个信号；没有队列节点的 pending 位会生成 Linux 兼容信息。
bool signal_take_unblocked_locked(struct task *task, sigset_t_ blocked,
        struct siginfo_ *info);

struct sigevent_ {
    union sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
};

// send a signal
// you better make sure the task isn't gonna get freed under me (pids_lock or current)
void send_signal(struct task *task, int sig, struct siginfo_ info);
// 调用方持有 pids_lock；用于已经固定目标生命周期的内部路径。
void send_signal_locked(struct task *task, int sig, struct siginfo_ info);
// 调用方持有 pids_lock；在 exec 换位窗口内保留进程定向 pending。
void send_process_signal(
        struct task *suggested, int sig, struct siginfo_ info);
// 已持有 pids_lock 与共享 sighand 锁时，把当前任务不再接收的共享信号重定向。
void signal_retarget_shared_pending_locked(
        struct task *task, sigset_t_ which);
// 无锁入口；用于任务修改自身阻塞掩码后的重新唤醒。
void signal_retarget_shared_pending(
        struct task *task, sigset_t_ which);
// 读取不可阻塞的 SIGKILL pending 状态；内部负责 sighand 锁。
bool task_sigkill_pending(struct task *task);
// 调用方持有目标 sighand 锁；返回必须抢占普通派送的退出信号。
int signal_forced_exit_locked(struct task *task);
// 子进程从作业控制停止恢复后，在无 pids_lock 的运行安全点通知父组。
void signal_notify_group_continue(struct task *task);
// 子进程停止或继续后唤醒父组；调用方不得持有 pids、group 或 sighand 锁。
void signal_notify_parent_child_state(struct task *task);
// 调用方持有 pids_lock；返回是否自动回收，并写出是否生成退出信号。
bool signal_parent_child_exit_policy_locked(
        struct task *parent, int exit_signal, bool *send_exit_signal);
// send a signal without regard for whether the signal is blocked or ignored
void deliver_signal(struct task *task, int sig, struct siginfo_ info);
// 调用方持有 pids_lock；强制排队但仍执行 stop/continue 的进程级副作用。
void deliver_signal_locked(
        struct task *task, int sig, struct siginfo_ info);
// send a signal to current if it's not blocked or ignored, return whether that worked
// exists specifically for sending SIGTTIN/SIGTTOU
bool try_self_signal(int sig);
// send a signal to all processes in a group, could return ESRCH
int send_group_signal(dword_t pgid, int sig, struct siginfo_ info);
// check for and deliver pending signals on current
// must be called without pids_lock, current->group->lock, or current->sighand->lock
void receive_signals(void);
// set the signal mask, restore it to what it was before on the next receive_signals call
void sigmask_set_temp_task(struct task *task, sigset_t_ mask);
void sigmask_set_temp(sigset_t_ mask);
// 无信号结束原子等待时，立即撤销尚未被投递路径消费的临时掩码。
void sigmask_restore_temp_task(struct task *task);
// 调用方持有 sighand 锁；返回等待期间用于选择中断信号的掩码，
// 同时把任务可见掩码恢复为写入 handler 帧的原值。
sigset_t_ signal_prepare_delivery_locked(struct task *task);
// 返回 EINTR 后保留临时掩码，下一次信号投递会恢复原掩码并写入信号帧。
int_t task_sigsuspend(struct task *task, sigset_t_ mask);

struct sighand {
    atomic_uint refcount;
    struct signal_action action[NUM_SIGS + 1];
    lock_t lock;
};
struct sighand *sighand_new(void);
struct sighand *sighand_copy(struct sighand *sighand);
void sighand_release(struct sighand *sighand);
int task_sigaction(struct task *task, int signal,
        const struct signal_action *action,
        struct signal_action *oldaction);
// exec 在 comm 更新后重置 disposition；替代栈由调用方按 Linux 顺序单独清理。
void task_signal_exec_reset_actions(struct task *task);
void task_signal_exec_reset(struct task *task);

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size);
dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr);
dword_t sys_rt_sigreturn(void);
dword_t sys_sigreturn(void);

#define SIG_BLOCK_ 0
#define SIG_UNBLOCK_ 1
#define SIG_SETMASK_ 2
int task_sigprocmask(struct task *task, dword_t how,
        const sigset_t_ *set, sigset_t_ *oldset);
sigset_t_ task_sigpending(struct task *task);
dword_t sys_rt_sigprocmask(dword_t how, addr_t set, addr_t oldset, dword_t size);
int_t sys_rt_sigpending(addr_t set_addr, uint_t size);

static inline sigset_t_ sig_mask(int sig) {
    assert(sig >= 1 && sig <= NUM_SIGS);
    return UINT64_C(1) << (sig - 1);
}

static inline bool sigset_has(sigset_t_ set, int sig) {
    return !!(set & sig_mask(sig));
}
static inline void sigset_add(sigset_t_ *set, int sig) {
    *set |= sig_mask(sig);
}
static inline void sigset_del(sigset_t_ *set, int sig) {
    *set &= ~sig_mask(sig);
}

struct stack_t_ {
    addr_t stack;
    dword_t flags;
    dword_t size;
};
_Static_assert(sizeof(struct stack_t_) == 12 &&
        __builtin_offsetof(struct stack_t_, stack) == 0 &&
        __builtin_offsetof(struct stack_t_, flags) == 4 &&
        __builtin_offsetof(struct stack_t_, size) == 8,
        "i386 sigaltstack wire 必须固定为 12 字节");

struct signal_altstack {
    qword_t stack;
    qword_t size;
    dword_t flags;
};

#define SS_ONSTACK_ 1
#define SS_DISABLE_ 2
#define MINSIGSTKSZ_ 2048
bool task_on_altstack(const struct task *task, qword_t stack_pointer);
bool i386_signal_frame_pointer(const struct task *task,
        const struct signal_action *action, dword_t original_sp,
        size_t frame_size, dword_t *frame_pointer);
int task_sigaltstack(struct task *task, qword_t stack_pointer,
        const struct signal_altstack *new_stack,
        struct signal_altstack *old_stack,
        qword_t minimum_size, qword_t address_limit);
void task_altstack_reset(struct task *task);
void task_altstack_on_clone(
        struct task *task, bool shares_vm, bool is_vfork);
dword_t sys_sigaltstack(addr_t ss, addr_t old_ss);

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size);
int_t sys_pause(void);
int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size);

dword_t sys_kill(pid_t_ pid, dword_t sig);
dword_t sys_tkill(pid_t_ tid, dword_t sig);
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig);
int task_rt_sigqueueinfo(pid_t_ pid, int signal,
        const struct siginfo_ *info);
int task_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid, int signal,
        const struct siginfo_ *info);
dword_t sys_rt_sigqueueinfo(pid_t_ pid, dword_t signal, addr_t info_addr);
dword_t sys_rt_tgsigqueueinfo(pid_t_ tgid, pid_t_ tid,
        dword_t signal, addr_t info_addr);

// signal frame structs. There's a good chance this should go in its own header file

// thanks kernel for giving me something to copy/paste
struct sigcontext_ {
    word_t gs, __gsh;
    word_t fs, __fsh;
    word_t es, __esh;
    word_t ds, __dsh;
    dword_t di;
    dword_t si;
    dword_t bp;
    dword_t sp;
    dword_t bx;
    dword_t dx;
    dword_t cx;
    dword_t ax;
    dword_t trapno;
    dword_t err;
    dword_t ip;
    word_t cs, __csh;
    dword_t flags;
    dword_t sp_at_signal;
    word_t ss, __ssh;

    dword_t fpstate;
    dword_t oldmask;
    dword_t cr2;
};

struct ucontext_ {
    uint_t flags;
    uint_t link;
    struct stack_t_ stack;
    struct sigcontext_ mcontext;
    sigset_t_ sigmask;
} __attribute__((packed));

struct fpreg_ {
    word_t significand[4];
    word_t exponent;
};

struct fpxreg_ {
    word_t significand[4];
    word_t exponent;
    word_t padding[3];
};

struct xmmreg_ {
    uint32_t element[4];
};

struct fpstate_ {
    /* Regular FPU environment.  */
    dword_t cw;
    dword_t sw;
    dword_t tag;
    dword_t ipoff;
    dword_t cssel;
    dword_t dataoff;
    dword_t datasel;
    struct fpreg_ st[8];
    word_t status;
    word_t magic;

    /* FXSR FPU environment.  */
    dword_t _fxsr_env[6];
    dword_t mxcsr;
    dword_t reserved;
    struct fpxreg_ fxsr_st[8];
    struct xmmreg_ xmm[8];
    dword_t padding[56];
};

struct sigframe_ {
    addr_t restorer;
    dword_t sig;
    struct sigcontext_ sc;
    struct fpstate_ fpstate;
    dword_t extramask;
    char retcode[8];
};

struct rt_sigframe_ {
    addr_t restorer;
    int_t sig;
    addr_t pinfo;
    addr_t puc;
    struct i386_siginfo info;
    struct ucontext_ uc;
    char retcode[8];
};
_Static_assert(sizeof(struct rt_sigframe_) == 268 &&
        __builtin_offsetof(struct rt_sigframe_, info) == 16 &&
        __builtin_offsetof(struct rt_sigframe_, uc) == 144 &&
        __builtin_offsetof(struct rt_sigframe_, retcode) == 260,
        "i386 实时信号帧布局不得随内部信息模型变化");

// On a 64-bit system with 32-bit emulation, the fpu state is stored in extra
// space at the end of the frame, not in the frame itself. We store the fpu
// state in the frame where it should be, and ptraceomatic will set this. If
// they are set we'll add some padding to the bottom to the frame to make
// everything align.
extern int xsave_extra;
extern int fxsave_extra;

#endif

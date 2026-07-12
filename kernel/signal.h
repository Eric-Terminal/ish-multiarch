#ifndef SIGNAL_H
#define SIGNAL_H

#include "misc.h"
#include "kernel/signal-info.h"
#include "util/list.h"
#include "util/sync.h"
struct task;

typedef qword_t sigset_t_;

#define SIG_ERR_ -1
#define SIG_DFL_ 0
#define SIG_IGN_ 1

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
};

// 调用方持有 sighand->lock；普通信号优先，
// 实时信号按编号与入队顺序选择。
struct sigqueue *signal_select_unblocked_locked(
        struct task *task, sigset_t_ blocked);
// 删除一个队列节点，并仅在同号节点全部消费后清除 pending 位。
void signal_dequeue_locked(
        struct task *task, struct sigqueue *queued);

struct sigevent_ {
    union sigval_ value;
    int_t signo;
    int_t method;
    pid_t_ tid;
};

// send a signal
// you better make sure the task isn't gonna get freed under me (pids_lock or current)
void send_signal(struct task *task, int sig, struct siginfo_ info);
// 子进程从作业控制停止恢复后，在无 pids_lock 的运行安全点通知父组。
void signal_notify_group_continue(struct task *task);
// send a signal without regard for whether the signal is blocked or ignored
void deliver_signal(struct task *task, int sig, struct siginfo_ info);
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

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_PAGE UINT32_C(0x00100000)
#define INPUT_ADDRESS (USER_PAGE + 3)
#define OUTPUT_ADDRESS (USER_PAGE + 67)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 信号测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static int fill_user_page(byte_t value) {
    byte_t bytes[PAGE_SIZE];
    memset(bytes, value, sizeof(bytes));
    return user_write(USER_PAGE, bytes, sizeof(bytes));
}

static byte_t load_user_byte(addr_t address) {
    byte_t value = 0;
    int error = user_read(address, &value, sizeof(value));
    assert(error == 0);
    return value;
}

static void clear_pending_signals(struct task *task) {
    struct sigqueue *queued, *queued_tmp;
    list_for_each_entry_safe(&task->queue, queued, queued_tmp, queue) {
        list_remove(&queued->queue);
        free(queued);
    }
    task->pending = 0;
}

struct async_signal_sender {
    struct task *task;
    sigset_t_ waiting;
    int sig;
    struct siginfo_ info;
    atomic_bool cancel;
    atomic_bool sent;
};

static void *send_signal_when_waiting(void *opaque) {
    struct async_signal_sender *sender = opaque;
    while (!atomic_load(&sender->cancel)) {
        lock(&sender->task->sighand->lock);
        bool ready = sender->task->waiting == sender->waiting;
        unlock(&sender->task->sighand->lock);
        if (ready) {
            atomic_store(&sender->sent, true);
            send_signal(sender->task, sender->sig, sender->info);
            return NULL;
        }
        sched_yield();
    }
    return NULL;
}

int main(void) {
    struct task task = {0};
    struct sighand sighand = {0};
    lock_init(&sighand.lock);
    task.sighand = &sighand;
    list_init(&task.queue);
    cond_init(&task.pause);
    lock_init(&task.waiting_cond_lock);
    lock_init(&task.ptrace.lock);
    cond_init(&task.ptrace.cond);
    task_thread_store(&task, pthread_self());
    task_set_mm(&task, mm_new());
    CHECK(task.mm != NULL, "创建 i386 用户地址空间");
    current = &task;

    struct sigaction host_ignore = {.sa_handler = SIG_IGN};
    struct sigaction old_host_action;
    sigemptyset(&host_ignore.sa_mask);
    CHECK(sigaction(SIGUSR1, &host_ignore, &old_host_action) == 0,
            "忽略测试期间的 host 唤醒信号");

    write_wrlock(&task.mem->lock);
    int map_error = pt_map_nothing(
            task.mem, PAGE(USER_PAGE), 1, P_RWX);
    write_wrunlock(&task.mem->lock);
    CHECK(map_error == 0 && fill_user_page(0xa5) == 0,
            "映射并初始化用户测试页");

    const struct i386_sigaction wire_action = {
        .handler = UINT32_C(0x81234567),
        .flags = UINT32_C(0xfedcba98),
        .restorer = UINT32_C(0x87654321),
        .mask = sig_mask(NUM_SIGS) |
                sig_mask(SIGKILL_) | sig_mask(SIGSTOP_),
    };
    CHECK(user_put(INPUT_ADDRESS, wire_action) == 0,
            "写入未对齐的 i386 动作 wire");

    dword_t result = sys_rt_sigaction(NUM_SIGS,
            INPUT_ADDRESS, OUTPUT_ADDRESS, sizeof(sigset_t_));
    struct i386_sigaction old_wire;
    CHECK(result == 0 && user_get(OUTPUT_ADDRESS, old_wire) == 0 &&
            old_wire.handler == SIG_DFL_ && old_wire.flags == 0 &&
            old_wire.restorer == 0 && old_wire.mask == 0,
            "信号 64 安装同时写回默认旧动作");
    CHECK(sighand.action[NUM_SIGS].handler == wire_action.handler &&
            sighand.action[NUM_SIGS].flags == wire_action.flags &&
            sighand.action[NUM_SIGS].restorer == wire_action.restorer &&
            sighand.action[NUM_SIGS].mask == sig_mask(NUM_SIGS),
            "i386 wire 零扩展到内部动作并清除不可阻塞掩码");
    CHECK(load_user_byte(OUTPUT_ADDRESS - 1) == 0xa5 &&
            load_user_byte(OUTPUT_ADDRESS + sizeof(old_wire)) == 0xa5,
            "旧动作写回严格限制为 20 字节");

    memset(&old_wire, 0xcc, sizeof(old_wire));
    CHECK(user_put(OUTPUT_ADDRESS, old_wire) == 0,
            "重置动作查询区域");
    result = sys_rt_sigaction(NUM_SIGS,
            0, OUTPUT_ADDRESS, sizeof(sigset_t_));
    CHECK(result == 0 && user_get(OUTPUT_ADDRESS, old_wire) == 0 &&
            old_wire.handler == wire_action.handler &&
            old_wire.flags == wire_action.flags &&
            old_wire.restorer == wire_action.restorer &&
            old_wire.mask == sig_mask(NUM_SIGS),
            "内部 64 位动作正确压回 i386 20 字节 wire");

    CHECK(fill_user_page(0x5a) == 0 &&
            user_put(INPUT_ADDRESS, wire_action) == 0,
            "准备输入输出别名用例");
    result = sys_rt_sigaction(SIGUSR1_,
            INPUT_ADDRESS, INPUT_ADDRESS, sizeof(sigset_t_));
    CHECK(result == 0 && user_get(INPUT_ADDRESS, old_wire) == 0 &&
            old_wire.handler == SIG_DFL_ && old_wire.flags == 0 &&
            old_wire.restorer == 0 && old_wire.mask == 0 &&
            sighand.action[SIGUSR1_].handler == wire_action.handler,
            "输入输出别名先完整读取新动作再写回旧动作");
    CHECK(load_user_byte(INPUT_ADDRESS - 1) == 0x5a &&
            load_user_byte(INPUT_ADDRESS + sizeof(old_wire)) == 0x5a,
            "别名写回不越过 i386 wire 边界");

    CHECK(sys_rt_sigaction(SIGKILL_, 0, OUTPUT_ADDRESS,
                    sizeof(sigset_t_)) == 0 &&
            sys_rt_sigaction(SIGSTOP_, INPUT_ADDRESS, 0,
                    sizeof(sigset_t_)) == (dword_t) _EINVAL &&
            sys_rt_sigaction(0, 0, OUTPUT_ADDRESS,
                    sizeof(sigset_t_)) == (dword_t) _EINVAL &&
            sys_rt_sigaction(NUM_SIGS + 1, 0, OUTPUT_ADDRESS,
                    sizeof(sigset_t_)) == (dword_t) _EINVAL,
            "查询不可捕获信号并拒绝安装与非法信号边界");

    addr_t boundary_address =
            USER_PAGE + PAGE_SIZE - sizeof(struct i386_sigaction);
    CHECK(user_put(boundary_address, wire_action) == 0 &&
            sys_rt_sigaction(63, boundary_address, boundary_address,
                    sizeof(sigset_t_)) == 0 &&
            user_get(boundary_address, old_wire) == 0 &&
            old_wire.handler == SIG_DFL_ &&
            sighand.action[63].handler == wire_action.handler,
            "页尾恰好容纳的 20 字节 wire 可完成读取与写回");

    addr_t crossing_address = USER_PAGE + PAGE_SIZE - 10;
    CHECK(sys_rt_sigaction(SIGUSR2_, crossing_address, 0,
                    sizeof(sigset_t_)) == (dword_t) _EFAULT &&
            sighand.action[SIGUSR2_].handler == SIG_DFL_,
            "跨越未映射页的输入故障不安装部分动作");
    CHECK(sys_rt_sigaction(SIGUSR2_, crossing_address, crossing_address,
                    1) == (dword_t) _EINVAL &&
            sighand.action[SIGUSR2_].handler == SIG_DFL_,
            "错误 sigset 大小优先于用户地址访问");

    CHECK(user_put(INPUT_ADDRESS, wire_action) == 0,
            "准备旧动作部分写回故障用例");
    result = sys_rt_sigaction(SIGUSR2_, INPUT_ADDRESS,
            crossing_address, sizeof(sigset_t_));
    CHECK(result == (dword_t) _EFAULT &&
            sighand.action[SIGUSR2_].handler == wire_action.handler &&
            sighand.action[SIGUSR2_].flags == wire_action.flags &&
            sighand.action[SIGUSR2_].restorer == wire_action.restorer &&
            sighand.action[SIGUSR2_].mask == sig_mask(NUM_SIGS),
            "旧动作部分写回故障不回滚已经安装的新动作");
    for (addr_t address = crossing_address;
            address < USER_PAGE + PAGE_SIZE; address++)
        CHECK(load_user_byte(address) == 0,
                "旧动作写回故障保留已复制的默认动作前缀");

    const addr_t wait_set_address = USER_PAGE + 128;
    const addr_t wait_info_address = USER_PAGE + 160;
    const addr_t wait_timeout_address = USER_PAGE + 224;
    const sigset_t_ wait_set = sig_mask(SIGALRM_);
    const sigset_t_ ordered_wait_set = wait_set | sig_mask(SIGTERM_);
    CHECK(user_put(wait_set_address, ordered_wait_set) == 0,
            "写入多信号 sigtimedwait 等待集合");
    const struct siginfo_ pending_info = {
        .code = SI_USER_,
        .kill = {.pid = 1234, .uid = 5678},
    };
    task.blocked = ordered_wait_set;
    deliver_signal(&task, SIGTERM_, (struct siginfo_) {.code = SI_USER_});
    deliver_signal(&task, SIGUSR1_, (struct siginfo_) {.code = SI_USER_});
    deliver_signal(&task, SIGALRM_, pending_info);
    CHECK(sigset_has(task.pending, SIGTERM_) &&
            sigset_has(task.pending, SIGALRM_) &&
            sigset_has(task.pending, SIGUSR1_) &&
            list_size(&task.queue) == 3,
            "按高号、无关、低号顺序预排信号");

    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, 0, sizeof(sigset_t_));
    struct siginfo_ waited_info;
    CHECK(result == SIGALRM_ &&
            user_get(wait_info_address, waited_info) == 0 &&
            waited_info.sig == SIGALRM_ &&
            waited_info.code == pending_info.code &&
            waited_info.kill.pid == pending_info.kill.pid &&
            waited_info.kill.uid == pending_info.kill.uid &&
            task.waiting == 0 && task.waiting_cond == NULL &&
            !sigset_has(task.pending, SIGALRM_) &&
            sigset_has(task.pending, SIGTERM_) &&
            sigset_has(task.pending, SIGUSR1_) &&
            list_size(&task.queue) == 2,
            "sigtimedwait 优先消费最低编号目标并保留其他节点");

    clear_pending_signals(&task);
    const sigset_t_ synchronous_wait_set =
            sig_mask(SIGUSR1_) | sig_mask(SIGSEGV_);
    task.blocked = synchronous_wait_set;
    CHECK(user_put(wait_set_address, synchronous_wait_set) == 0,
            "写入同步信号优先级等待集合");
    deliver_signal(&task, SIGUSR1_, (struct siginfo_) {.code = SI_USER_});
    deliver_signal(&task, SIGSEGV_, (struct siginfo_) {.code = SI_USER_});
    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, 0, sizeof(sigset_t_));
    CHECK(result == SIGSEGV_ && sigset_has(task.pending, SIGUSR1_) &&
            !sigset_has(task.pending, SIGSEGV_) &&
            list_size(&task.queue) == 1,
            "同步故障信号优先于编号更低的普通信号");

    clear_pending_signals(&task);
    task.blocked = wait_set;
    CHECK(user_put(wait_set_address, wait_set) == 0,
            "切换为单信号异步等待集合");
    const struct timespec_ one_second = {.sec = 1};
    CHECK(user_put(wait_timeout_address, one_second) == 0,
            "写入异步等待超时");
    struct async_signal_sender target_sender = {
        .task = &task,
        .waiting = wait_set,
        .sig = SIGALRM_,
        .info = {
            .code = SI_USER_,
            .kill = {.pid = 2468, .uid = 1357},
        },
    };
    pthread_t sender_thread;
    CHECK(pthread_create(&sender_thread, NULL,
                    send_signal_when_waiting, &target_sender) == 0,
            "创建异步目标信号线程");
    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, wait_timeout_address, sizeof(sigset_t_));
    atomic_store(&target_sender.cancel, true);
    CHECK(pthread_join(sender_thread, NULL) == 0,
            "回收异步目标信号线程");
    CHECK(atomic_load(&target_sender.sent) && result == SIGALRM_ &&
            user_get(wait_info_address, waited_info) == 0 &&
            waited_info.sig == SIGALRM_ &&
            waited_info.code == target_sender.info.code &&
            waited_info.kill.pid == target_sender.info.kill.pid &&
            waited_info.kill.uid == target_sender.info.kill.uid &&
            task.pending == 0 && list_empty(&task.queue) &&
            task.waiting == 0 && task.waiting_cond == NULL,
            "等待期间到达的被阻塞目标会唤醒并被完整消费");

    struct siginfo_ info_sentinel;
    memset(&info_sentinel, 0x5c, sizeof(info_sentinel));
    CHECK(user_put(wait_info_address, info_sentinel) == 0,
            "写入 EINTR 输出哨兵");
    struct async_signal_sender interrupt_sender = {
        .task = &task,
        .waiting = wait_set,
        .sig = SIGUSR1_,
        .info = {
            .code = SI_USER_,
            .kill = {.pid = 9753, .uid = 8642},
        },
    };
    CHECK(pthread_create(&sender_thread, NULL,
                    send_signal_when_waiting, &interrupt_sender) == 0,
            "创建异步中断信号线程");
    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, wait_timeout_address, sizeof(sigset_t_));
    atomic_store(&interrupt_sender.cancel, true);
    CHECK(pthread_join(sender_thread, NULL) == 0,
            "回收异步中断信号线程");
    CHECK(user_get(wait_info_address, waited_info) == 0,
            "读取 EINTR 输出哨兵");
    CHECK(atomic_load(&interrupt_sender.sent) &&
            result == (dword_t) _EINTR &&
            memcmp(&waited_info, &info_sentinel, sizeof(waited_info)) == 0 &&
            sigset_has(task.pending, SIGUSR1_) &&
            list_size(&task.queue) == 1 &&
            task.waiting == 0 && task.waiting_cond == NULL,
            "无关未阻塞信号中断等待且不写输出或消费节点");

    clear_pending_signals(&task);
    const struct timespec_ zero_timeout = {0};
    CHECK(user_put(wait_timeout_address, zero_timeout) == 0 &&
            user_put(wait_info_address, info_sentinel) == 0,
            "写入零超时与输出哨兵");
    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, wait_timeout_address, sizeof(sigset_t_));
    CHECK(user_get(wait_info_address, waited_info) == 0 &&
            result == (dword_t) _EAGAIN &&
            memcmp(&waited_info, &info_sentinel, sizeof(waited_info)) == 0 &&
            task.pending == 0 && list_empty(&task.queue) &&
            task.waiting == 0 && task.waiting_cond == NULL,
            "零超时轮询空集合立即返回且不改写输出");

    const struct timespec_ short_timeout = {
        .nsec = UINT32_C(20000000),
    };
    CHECK(user_put(wait_timeout_address, short_timeout) == 0,
            "写入非零有限超时");
    struct timespec timeout_start = timespec_now(CLOCK_MONOTONIC);
    result = sys_rt_sigtimedwait(wait_set_address, wait_info_address,
            wait_timeout_address, sizeof(sigset_t_));
    struct timespec timeout_elapsed = timespec_subtract(
            timespec_now(CLOCK_MONOTONIC), timeout_start);
    CHECK(result == (dword_t) _EAGAIN &&
            (timeout_elapsed.tv_sec > 0 ||
             timeout_elapsed.tv_nsec >= 10000000) &&
            task.pending == 0 && list_empty(&task.queue) &&
            task.waiting == 0 && task.waiting_cond == NULL,
            "非零有限等待通过条件变量超时并清理等待状态");

    const struct timespec_ negative_timeout = {.sec = UINT32_MAX};
    const struct timespec_ invalid_nsec = {.nsec = UINT32_C(1000000000)};
    CHECK(user_put(wait_timeout_address, negative_timeout) == 0 &&
            sys_rt_sigtimedwait(wait_set_address, wait_info_address,
                    wait_timeout_address, sizeof(sigset_t_)) == _EINVAL &&
            user_put(wait_timeout_address, invalid_nsec) == 0 &&
            sys_rt_sigtimedwait(wait_set_address, wait_info_address,
                    wait_timeout_address, sizeof(sigset_t_)) == _EINVAL,
            "拒绝负超时和越界纳秒");

    CHECK(user_put(wait_timeout_address, zero_timeout) == 0,
            "恢复零超时以测试 siginfo 写回故障");
    deliver_signal(&task, SIGALRM_, pending_info);
    result = sys_rt_sigtimedwait(wait_set_address,
            crossing_address, wait_timeout_address, sizeof(sigset_t_));
    CHECK(result == (dword_t) _EFAULT && task.pending == 0 &&
            list_empty(&task.queue),
            "siginfo 写回故障仍消费已经选中的信号");

    const sigset_t_ unblockable_set =
            sig_mask(SIGKILL_) | sig_mask(SIGSTOP_);
    CHECK(user_put(wait_set_address, unblockable_set) == 0 &&
            user_put(wait_timeout_address, zero_timeout) == 0,
            "写入不可等待信号集合");
    deliver_signal(&task, SIGKILL_, SIGINFO_NIL);
    deliver_signal(&task, SIGSTOP_, SIGINFO_NIL);
    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, wait_timeout_address, sizeof(sigset_t_));
    CHECK(result == (dword_t) _EAGAIN &&
            sigset_has(task.pending, SIGKILL_) &&
            sigset_has(task.pending, SIGSTOP_) &&
            list_size(&task.queue) == 2,
            "sigtimedwait 静默排除且不消费 SIGKILL 与 SIGSTOP");

    clear_pending_signals(&task);
    const sigset_t_ ignored_set = sig_mask(SIGUSR2_);
    CHECK(user_put(wait_set_address, ignored_set) == 0,
            "写入显式忽略信号集合");
    sighand.action[SIGUSR2_].handler = SIG_IGN_;
    task.blocked = 0;
    send_signal(&task, SIGUSR2_, (struct siginfo_) {.code = SI_USER_});
    CHECK(task.pending == 0 && list_empty(&task.queue),
            "未阻塞的显式忽略信号在生成阶段丢弃");
    task.blocked = ignored_set;
    send_signal(&task, SIGUSR2_, (struct siginfo_) {
        .code = SI_USER_, .kill = {.pid = 111, .uid = 222},
    });
    CHECK(sigset_has(task.pending, SIGUSR2_) &&
            list_size(&task.queue) == 1 &&
            sys_rt_sigtimedwait(wait_set_address, wait_info_address,
                    wait_timeout_address, sizeof(sigset_t_)) == SIGUSR2_ &&
            task.pending == 0 && list_empty(&task.queue),
            "被阻塞的显式忽略信号仍可排队并被等待消费");

    const sigset_t_ default_ignored_set = sig_mask(SIGCHLD_);
    CHECK(user_put(wait_set_address, default_ignored_set) == 0,
            "写入默认忽略信号集合");
    task.blocked = 0;
    send_signal(&task, SIGCHLD_, (struct siginfo_) {.code = SI_USER_});
    CHECK(task.pending == 0 && list_empty(&task.queue),
            "未阻塞的默认忽略信号在生成阶段丢弃");
    task.blocked = default_ignored_set;
    send_signal(&task, SIGCHLD_, (struct siginfo_) {
        .code = SI_USER_, .kill = {.pid = 333, .uid = 444},
    });
    CHECK(sigset_has(task.pending, SIGCHLD_) &&
            list_size(&task.queue) == 1 &&
            sys_rt_sigtimedwait(wait_set_address, wait_info_address,
                    wait_timeout_address, sizeof(sigset_t_)) == SIGCHLD_ &&
            task.pending == 0 && list_empty(&task.queue),
            "被阻塞的默认忽略信号仍可排队并被等待消费");

    task.blocked = 0;
    clear_pending_signals(&task);
    CHECK(sigaction(SIGUSR1, &old_host_action, NULL) == 0,
            "恢复 host 唤醒信号动作");

    current = NULL;
    cond_destroy(&task.ptrace.cond);
    cond_destroy(&task.pause);
    mm_release(task.mm);
    return 0;
}

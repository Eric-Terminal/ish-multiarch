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
#include "kernel/vdso.h"

#define USER_PAGE UINT32_C(0x00100000)
#define ALTSTACK_PAGE UINT32_C(0x00200000)
#define NORMAL_STACK_PAGE UINT32_C(0x00300000)
#define VDSO_BASE UINT32_C(0x00400000)
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
    task_altstack_reset(&task);
    task_thread_store(&task, pthread_self());
    task_set_mm(&task, mm_new());
    CHECK(task.mm != NULL, "创建 i386 用户地址空间");
    task.mm->vdso = VDSO_BASE;
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

    const addr_t crossing_sigset_address =
            USER_PAGE + PAGE_SIZE - sizeof(dword_t);
    const sigset_t_ process_mask = sig_mask(SIGUSR1_);
    CHECK(user_put(INPUT_ADDRESS, process_mask) == 0,
            "写入进程信号掩码输入");
    CHECK(sys_rt_sigprocmask(SIG_BLOCK_, INPUT_ADDRESS,
                    crossing_sigset_address, sizeof(sigset_t_)) ==
                    (dword_t) _EFAULT &&
            task.blocked == process_mask,
            "rt_sigprocmask 的旧掩码写故障不回滚新掩码");
    CHECK(fill_user_page(0xa5) == 0 &&
            sys_rt_sigprocmask(SIG_BLOCK_, crossing_sigset_address,
                    OUTPUT_ADDRESS, sizeof(sigset_t_)) ==
                    (dword_t) _EFAULT &&
            task.blocked == process_mask &&
            load_user_byte(OUTPUT_ADDRESS) == 0xa5,
            "rt_sigprocmask 的输入故障不写旧掩码");
    CHECK(user_put(INPUT_ADDRESS, process_mask) == 0 &&
            sys_rt_sigprocmask(3, INPUT_ADDRESS,
                    OUTPUT_ADDRESS, sizeof(sigset_t_)) ==
                    (dword_t) _EINVAL &&
            task.blocked == process_mask &&
            load_user_byte(OUTPUT_ADDRESS) == 0xa5,
            "rt_sigprocmask 的非法操作不写旧掩码");

    task.blocked = sig_mask(SIGUSR1_) | sig_mask(NUM_SIGS);
    task.pending = sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_) |
            sig_mask(NUM_SIGS);
    CHECK(sys_rt_sigpending(OUTPUT_ADDRESS,
                    sizeof(sigset_t_) + 1) == _EINVAL &&
            load_user_byte(OUTPUT_ADDRESS) == 0xa5,
            "rt_sigpending 按完整参数拒绝错误 sigset 大小");
    CHECK(sys_rt_sigpending(UINT32_MAX, 0) == 0,
            "rt_sigpending 的零长度查询不访问用户地址");
    CHECK(sys_rt_sigpending(
                    UINT32_MAX - sizeof(sigset_t_) + 2,
                    sizeof(sigset_t_)) == _EFAULT,
            "rt_sigpending 拒绝回绕 32 位地址空间的写回范围");
    CHECK(sys_rt_sigpending(OUTPUT_ADDRESS, sizeof(dword_t)) == 0,
            "rt_sigpending 接受 sigset 的前缀长度");
    dword_t pending_prefix;
    CHECK(user_get(OUTPUT_ADDRESS, pending_prefix) == 0 &&
            pending_prefix == (dword_t) sig_mask(SIGUSR1_) &&
            load_user_byte(OUTPUT_ADDRESS + sizeof(pending_prefix)) == 0xa5,
            "rt_sigpending 仅写入请求的 sigset 前缀");
    sigset_t_ pending_snapshot;
    CHECK(sys_rt_sigpending(OUTPUT_ADDRESS, sizeof(sigset_t_)) == 0 &&
            user_get(OUTPUT_ADDRESS, pending_snapshot) == 0 &&
            pending_snapshot ==
                    (sig_mask(SIGUSR1_) | sig_mask(NUM_SIGS)),
            "rt_sigpending 在 sighand 锁内取得阻塞 pending 快照");
    struct sighand other_sighand = {0};
    lock_init(&other_sighand.lock);
    struct task other_task = {
        .sighand = &other_sighand,
        .blocked = sig_mask(SIGUSR2_),
        .pending = sig_mask(SIGUSR1_) | sig_mask(SIGUSR2_),
    };
    CHECK(task_sigpending(&other_task) == sig_mask(SIGUSR2_),
            "task_sigpending 查询显式任务而非当前任务");
    task.blocked = 0;
    task.pending = 0;

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

    const addr_t altstack_input_address = USER_PAGE + 320;
    const addr_t altstack_output_address = USER_PAGE + 352;
    struct stack_t_ queried_stack;
    CHECK(sys_sigaltstack(0, altstack_output_address) == 0 &&
            user_get(altstack_output_address, queried_stack) == 0 &&
            queried_stack.stack == 0 &&
            queried_stack.flags == SS_DISABLE_ &&
            queried_stack.size == 0,
            "未配置的线程替代栈查询为禁用状态");

    struct stack_t_ stack_sentinel;
    memset(&stack_sentinel, 0xcc, sizeof(stack_sentinel));
    const struct stack_t_ small_stack = {
        .stack = ALTSTACK_PAGE,
        .size = MINSIGSTKSZ_ - 1,
    };
    CHECK(user_put(altstack_input_address, small_stack) == 0 &&
            user_put(altstack_output_address, stack_sentinel) == 0 &&
            sys_sigaltstack(altstack_input_address,
                    altstack_output_address) == (dword_t) _ENOMEM &&
            user_get(altstack_output_address, queried_stack) == 0 &&
            memcmp(&queried_stack, &stack_sentinel,
                    sizeof(queried_stack)) == 0 &&
            task.altstack.size == 0,
            "过小替代栈不改状态也不写旧状态");
    CHECK(trylock(&sighand.lock) == 0,
            "替代栈 ENOMEM 路径不遗留 sighand 锁");
    unlock(&sighand.lock);

    const struct stack_t_ invalid_flags_stack = {
        .stack = ALTSTACK_PAGE,
        .flags = SS_ONSTACK_ | SS_DISABLE_,
        .size = PAGE_SIZE,
    };
    CHECK(user_put(altstack_input_address, invalid_flags_stack) == 0 &&
            sys_sigaltstack(altstack_input_address,
                    altstack_output_address) == (dword_t) _EINVAL &&
            task.altstack.size == 0,
            "替代栈拒绝组合模式标志");

    const struct stack_t_ configured_stack = {
        .stack = ALTSTACK_PAGE,
        .size = PAGE_SIZE,
    };
    CHECK(user_put(altstack_input_address, configured_stack) == 0 &&
            sys_sigaltstack(altstack_input_address,
                    altstack_input_address) == 0 &&
            user_get(altstack_input_address, queried_stack) == 0 &&
            queried_stack.stack == 0 &&
            queried_stack.flags == SS_DISABLE_ &&
            queried_stack.size == 0 &&
            task.altstack.stack == configured_stack.stack &&
            task.altstack.size == configured_stack.size &&
            task.altstack.flags == 0,
            "替代栈输入输出同址时先读取新状态再写回旧状态");

    const struct stack_t_ zero_based_stack = {
        .stack = 0,
        .size = MINSIGSTKSZ_,
    };
    CHECK(user_put(altstack_input_address, zero_based_stack) == 0 &&
            sys_sigaltstack(altstack_input_address, 0) == 0 &&
            task.altstack.stack == 0 &&
            task.altstack.size == MINSIGSTKSZ_ &&
            !task_on_altstack(&task, 0) &&
            task_on_altstack(&task, MINSIGSTKSZ_),
            "地址零的非空替代栈保持启用且使用无回绕边界判断");

    const struct stack_t_ restored_onstack_mode = {
        .stack = ALTSTACK_PAGE,
        .flags = SS_ONSTACK_,
        .size = PAGE_SIZE,
    };
    CHECK(user_put(altstack_input_address, restored_onstack_mode) == 0 &&
            sys_sigaltstack(altstack_input_address, 0) == 0 &&
            task.altstack.flags == SS_ONSTACK_ &&
            sys_sigaltstack(0, altstack_output_address) == 0 &&
            user_get(altstack_output_address, queried_stack) == 0 &&
            queried_stack.flags == 0,
            "替代栈接受恢复用 ONSTACK 模式并动态查询当前位置");

    const struct stack_t_ wrapping_stack = {
        .stack = UINT32_MAX - MINSIGSTKSZ_ + 1,
        .size = MINSIGSTKSZ_,
    };
    CHECK(user_put(altstack_input_address, wrapping_stack) == 0 &&
            user_put(altstack_output_address, stack_sentinel) == 0 &&
            sys_sigaltstack(altstack_input_address,
                    altstack_output_address) == (dword_t) _ENOMEM &&
            task.altstack.stack == ALTSTACK_PAGE &&
            task.altstack.size == PAGE_SIZE &&
            task.altstack.flags == SS_ONSTACK_ &&
            user_get(altstack_output_address, queried_stack) == 0 &&
            memcmp(&queried_stack, &stack_sentinel,
                    sizeof(queried_stack)) == 0,
            "替代栈拒绝越过 i386 地址上界的范围");

    const struct stack_t_ disable_stack = {
        .stack = UINT32_MAX,
        .flags = SS_DISABLE_,
        .size = 1,
    };
    CHECK(user_put(altstack_input_address, disable_stack) == 0 &&
            sys_sigaltstack(altstack_input_address, 0) == 0 &&
            task.altstack.stack == 0 && task.altstack.size == 0 &&
            task.altstack.flags == SS_DISABLE_,
            "禁用替代栈忽略输入范围并清空全部状态");

    struct task peer_task = {
        .sighand = &sighand,
        .altstack = {
            .stack = UINT64_C(0x0000400012345000),
            .size = UINT64_C(0x8000),
            .flags = SS_ONSTACK_,
        },
    };
    CHECK(user_put(altstack_input_address, configured_stack) == 0 &&
            sys_sigaltstack(altstack_input_address, 0) == 0 &&
            peer_task.altstack.stack == UINT64_C(0x0000400012345000) &&
            peer_task.altstack.size == UINT64_C(0x8000) &&
            peer_task.altstack.flags == SS_ONSTACK_ &&
            !task_on_altstack(&peer_task,
                    UINT64_C(0x0000400012345000)) &&
            task_on_altstack(&peer_task,
                    UINT64_C(0x000040001234d000)),
            "共享 sighand 的线程拥有互不影响的 64 位替代栈");

    struct task clone_altstack = {.altstack = task.altstack};
    task_altstack_on_clone(&clone_altstack, false, false);
    CHECK(clone_altstack.altstack.stack == task.altstack.stack &&
            clone_altstack.altstack.size == task.altstack.size,
            "普通 fork 继承线程替代栈");
    clone_altstack.altstack = task.altstack;
    task_altstack_on_clone(&clone_altstack, true, true);
    CHECK(clone_altstack.altstack.stack == task.altstack.stack &&
            clone_altstack.altstack.size == task.altstack.size,
            "vfork 共享地址空间时保留替代栈");
    task_altstack_on_clone(&clone_altstack, true, false);
    CHECK(clone_altstack.altstack.stack == 0 &&
            clone_altstack.altstack.size == 0 &&
            clone_altstack.altstack.flags == SS_DISABLE_,
            "普通 CLONE_VM 子任务从禁用替代栈开始");

    task.cpu.esp = ALTSTACK_PAGE + PAGE_SIZE;
    CHECK(sys_sigaltstack(0, altstack_output_address) == 0 &&
            user_get(altstack_output_address, queried_stack) == 0 &&
            queried_stack.flags == SS_ONSTACK_ &&
            user_put(altstack_input_address, disable_stack) == 0 &&
            user_put(altstack_output_address, stack_sentinel) == 0 &&
            sys_sigaltstack(altstack_input_address,
                    altstack_output_address) == (dword_t) _EPERM &&
            task.altstack.stack == ALTSTACK_PAGE &&
            task.altstack.size == PAGE_SIZE &&
            user_get(altstack_output_address, queried_stack) == 0 &&
            memcmp(&queried_stack, &stack_sentinel,
                    sizeof(queried_stack)) == 0,
            "位于替代栈时允许查询但拒绝修改且不写旧状态");

    task.cpu.esp = NORMAL_STACK_PAGE + PAGE_SIZE;
    CHECK(user_put(altstack_output_address, stack_sentinel) == 0 &&
            sys_sigaltstack(crossing_address,
                    altstack_output_address) == (dword_t) _EFAULT &&
            task.altstack.stack == ALTSTACK_PAGE &&
            load_user_byte(altstack_output_address) == 0xcc,
            "替代栈输入故障优先且不写旧状态");
    const struct stack_t_ second_stack = {
        .stack = ALTSTACK_PAGE + PAGE_SIZE,
        .size = PAGE_SIZE,
    };
    CHECK(user_put(altstack_input_address, second_stack) == 0 &&
            sys_sigaltstack(altstack_input_address,
                    crossing_address) == (dword_t) _EFAULT &&
            task.altstack.stack == second_stack.stack &&
            task.altstack.size == second_stack.size,
            "替代栈旧状态写回故障不回滚已应用的新状态");
    task_altstack_reset(&task);

    const addr_t wait_set_address = USER_PAGE + 128;
    const addr_t wait_info_address = USER_PAGE + 160;
    const addr_t wait_timeout_address = USER_PAGE + 288;
    const sigset_t_ wait_set = sig_mask(SIGALRM_);
    const sigset_t_ ordered_wait_set = wait_set | sig_mask(SIGTERM_);
    CHECK(user_put(wait_set_address, ordered_wait_set) == 0,
            "写入多信号 sigtimedwait 等待集合");
    const struct siginfo_ pending_info = {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
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

    const byte_t info_boundary_sentinel = UINT8_C(0x6d);
    CHECK(user_put(wait_info_address - 1, info_boundary_sentinel) == 0 &&
            user_put(wait_info_address + sizeof(struct i386_siginfo),
                    info_boundary_sentinel) == 0,
            "写入 siginfo 输出边界哨兵");
    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, 0, sizeof(sigset_t_));
    struct i386_siginfo waited_info;
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
            list_size(&task.queue) == 2 &&
            load_user_byte(wait_info_address - 1) ==
                    info_boundary_sentinel &&
            load_user_byte(wait_info_address +
                    sizeof(struct i386_siginfo)) == info_boundary_sentinel,
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
            .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
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

    struct i386_siginfo info_sentinel;
    memset(&info_sentinel, 0x5c, sizeof(info_sentinel));
    CHECK(user_put(wait_info_address, info_sentinel) == 0,
            "写入 EINTR 输出哨兵");
    struct async_signal_sender interrupt_sender = {
        .task = &task,
        .waiting = wait_set,
        .sig = SIGUSR1_,
        .info = {
            .code = SI_USER_,
            .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
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
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
        .kill = {.pid = 111, .uid = 222},
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
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
        .kill = {.pid = 333, .uid = 444},
    });
    CHECK(sigset_has(task.pending, SIGCHLD_) &&
            list_size(&task.queue) == 1 &&
            sys_rt_sigtimedwait(wait_set_address, wait_info_address,
                    wait_timeout_address, sizeof(sigset_t_)) == SIGCHLD_ &&
            task.pending == 0 && list_empty(&task.queue),
            "被阻塞的默认忽略信号仍可排队并被等待消费");

    task.blocked = 0;
    clear_pending_signals(&task);

    write_wrlock(&task.mem->lock);
    int altstack_map_error = pt_map_nothing(
            task.mem, PAGE(ALTSTACK_PAGE), 1, P_RWX);
    int normal_stack_map_error = pt_map_nothing(
            task.mem, PAGE(NORMAL_STACK_PAGE), 1, P_RWX);
    write_wrunlock(&task.mem->lock);
    CHECK(altstack_map_error == 0 && normal_stack_map_error == 0,
            "映射普通栈与替代信号栈测试页");

    struct tgroup signal_group = {0};
    lock_init(&signal_group.lock);
    cond_init(&signal_group.stopped_cond);
    signal_group.leader = &task;
    task.group = &signal_group;
    CHECK(user_put(altstack_input_address, configured_stack) == 0 &&
            sys_sigaltstack(altstack_input_address, 0) == 0,
            "为信号帧派送配置线程替代栈");

    const dword_t normal_stack_top = NORMAL_STACK_PAGE + PAGE_SIZE;
    const dword_t original_eip = UINT32_C(0x08049000);
    const dword_t original_eax = UINT32_C(0x13579bdf);
    const struct signal_action normal_stack_action = {
        .handler = UINT32_C(0x0804a000),
        .mask = sig_mask(SIGUSR2_),
        .flags = SA_SIGINFO_,
    };
    CHECK(task_sigaction(&task, SIGUSR1_,
                    &normal_stack_action, NULL) == 0,
            "安装不请求替代栈的信号动作");
    task.cpu.esp = normal_stack_top;
    task.cpu.eip = original_eip;
    task.cpu.eax = original_eax;
    task.blocked = 0;
    const struct siginfo_ normal_frame_info = {
        .code = SEGV_MAPERR_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = UINT64_C(0x12345678abcdef01),
    };
    deliver_signal(&task, SIGUSR1_, normal_frame_info);
    receive_signals();
    addr_t normal_frame_address = task.cpu.esp;
    struct rt_sigframe_ delivered_frame;
    CHECK(user_get(normal_frame_address, delivered_frame) == 0,
            "读取普通栈实时信号帧");
    bool frame_info_tail_zero = true;
    for (size_t index = 1;
            index < array_size(delivered_frame.info.payload_words); index++) {
        if (delivered_frame.info.payload_words[index] != 0) {
            frame_info_tail_zero = false;
            break;
        }
    }
    CHECK(normal_frame_address >= NORMAL_STACK_PAGE &&
            normal_frame_address < normal_stack_top &&
            task.cpu.eip == normal_stack_action.handler &&
            task.cpu.edx == normal_frame_address +
                    offsetof(struct rt_sigframe_, info) &&
            delivered_frame.pinfo == task.cpu.edx &&
            delivered_frame.info.sig == SIGUSR1_ &&
            delivered_frame.info.code == normal_frame_info.code &&
            delivered_frame.info.fault.addr == UINT32_C(0xabcdef01) &&
            frame_info_tail_zero &&
            delivered_frame.uc.mcontext.sp == normal_stack_top &&
            delivered_frame.restorer == VDSO_BASE +
                    (addr_t) vdso_symbol("__kernel_rt_sigreturn") &&
            delivered_frame.uc.stack.stack == ALTSTACK_PAGE &&
            delivered_frame.uc.stack.size == PAGE_SIZE &&
            delivered_frame.uc.stack.flags == 0,
            "未设置 SA_ONSTACK 时信号帧留在普通栈并保存替代栈配置");
    delivered_frame.uc.stack = (struct stack_t_) {
        .stack = ALTSTACK_PAGE,
        .size = MINSIGSTKSZ_ - 1,
    };
    CHECK(user_put(normal_frame_address, delivered_frame) == 0,
            "把过小替代栈状态写入普通栈返回帧");
    task.cpu.esp = normal_frame_address +
            offsetof(struct rt_sigframe_, sig);
    CHECK(sys_rt_sigreturn() == original_eax &&
            task.cpu.esp == normal_stack_top &&
            task.cpu.eip == original_eip && task.blocked == 0 &&
            task.altstack.stack == ALTSTACK_PAGE &&
            task.altstack.size == PAGE_SIZE,
            "rt_sigreturn 忽略 ENOMEM 并恢复普通栈帧上下文");

    const struct signal_action altstack_action = {
        .handler = UINT32_C(0x0804b000),
        .flags = SA_SIGINFO_ | SA_ONSTACK_,
    };
    CHECK(task_sigaction(&task, SIGUSR2_,
                    &altstack_action, NULL) == 0,
            "安装请求替代栈的信号动作");
    task.cpu.esp = normal_stack_top;
    task.cpu.eip = original_eip;
    task.cpu.eax = original_eax;
    deliver_signal(&task, SIGUSR2_, (struct siginfo_) {.code = SI_USER_});
    receive_signals();
    addr_t altstack_frame_address = task.cpu.esp;
    CHECK(altstack_frame_address > ALTSTACK_PAGE &&
            altstack_frame_address < ALTSTACK_PAGE + PAGE_SIZE &&
            task.cpu.eip == altstack_action.handler &&
            user_get(altstack_frame_address, delivered_frame) == 0 &&
            delivered_frame.uc.mcontext.sp == normal_stack_top &&
            delivered_frame.uc.stack.stack == ALTSTACK_PAGE &&
            delivered_frame.uc.stack.size == PAGE_SIZE &&
            delivered_frame.uc.stack.flags == 0,
            "SA_ONSTACK 从替代栈顶向下建立信号帧");
    delivered_frame.uc.stack = (struct stack_t_) {
        .flags = SS_DISABLE_,
    };
    CHECK(user_put(altstack_frame_address, delivered_frame) == 0,
            "把禁用替代栈状态写入返回帧");
    task.cpu.esp = altstack_frame_address +
            offsetof(struct rt_sigframe_, sig);
    CHECK(sys_rt_sigreturn() == original_eax &&
            task.cpu.esp == normal_stack_top &&
            task.cpu.eip == original_eip &&
            task.altstack.stack == 0 && task.altstack.size == 0 &&
            task.altstack.flags == SS_DISABLE_,
            "rt_sigreturn 复用线程私有状态机恢复禁用状态");

    task.cpu.esp = normal_stack_top;
    task.cpu.eip = original_eip;
    task.cpu.eax = original_eax;
    deliver_signal(&task, SIGUSR2_, (struct siginfo_) {.code = SI_USER_});
    receive_signals();
    addr_t disabled_frame_address = task.cpu.esp;
    CHECK(disabled_frame_address >= NORMAL_STACK_PAGE &&
            disabled_frame_address < normal_stack_top &&
            user_get(disabled_frame_address, delivered_frame) == 0 &&
            delivered_frame.uc.stack.flags == SS_DISABLE_,
            "禁用替代栈时 SA_ONSTACK 明确回落到普通栈");
    task.cpu.esp = disabled_frame_address +
            offsetof(struct rt_sigframe_, sig);
    CHECK(sys_rt_sigreturn() == original_eax &&
            task.cpu.esp == normal_stack_top,
            "从替代栈禁用状态的普通帧返回");

    CHECK(user_put(altstack_input_address, configured_stack) == 0 &&
            sys_sigaltstack(altstack_input_address, 0) == 0,
            "为嵌套派送重新启用替代栈");

    const struct signal_action legacy_altstack_action = {
        .handler = UINT32_C(0x0804c000),
        .flags = SA_ONSTACK_,
    };
    CHECK(task_sigaction(&task, SIGTERM_,
                    &legacy_altstack_action, NULL) == 0,
            "安装非 SA_SIGINFO 的替代栈动作");
    task.cpu.esp = normal_stack_top;
    task.cpu.eip = original_eip;
    task.cpu.eax = original_eax;
    deliver_signal(&task, SIGTERM_, (struct siginfo_) {.code = SI_USER_});
    receive_signals();
    addr_t legacy_frame_address = task.cpu.esp;
    struct sigframe_ legacy_frame;
    CHECK(legacy_frame_address > ALTSTACK_PAGE &&
            legacy_frame_address < ALTSTACK_PAGE + PAGE_SIZE &&
            user_get(legacy_frame_address, legacy_frame) == 0 &&
            legacy_frame.sc.sp == normal_stack_top,
            "非 SA_SIGINFO 动作也在替代栈建立传统信号帧");
    task.cpu.esp = legacy_frame_address +
            offsetof(struct sigframe_, sc);
    CHECK(sys_sigreturn() == original_eax &&
            task.cpu.esp == normal_stack_top &&
            task.cpu.eip == original_eip,
            "传统 sigreturn 从替代栈恢复上下文");

    dword_t nested_stack_pointer = ALTSTACK_PAGE + PAGE_SIZE / 2;
    task.cpu.esp = nested_stack_pointer;
    task.cpu.eip = original_eip;
    task.cpu.eax = original_eax;
    deliver_signal(&task, SIGUSR2_, (struct siginfo_) {.code = SI_USER_});
    receive_signals();
    addr_t nested_frame_address = task.cpu.esp;
    CHECK(nested_frame_address > ALTSTACK_PAGE &&
            nested_frame_address < nested_stack_pointer &&
            user_get(nested_frame_address, delivered_frame) == 0 &&
            delivered_frame.uc.mcontext.sp == nested_stack_pointer,
            "已在替代栈上的嵌套派送沿当前栈指针继续向下建帧");
    delivered_frame.uc.stack = (struct stack_t_) {
        .stack = ALTSTACK_PAGE,
        .size = MINSIGSTKSZ_ - 1,
    };
    CHECK(user_put(nested_frame_address, delivered_frame) == 0,
            "把非法替代栈状态写入嵌套返回帧");
    task.cpu.esp = nested_frame_address +
            offsetof(struct rt_sigframe_, sig);
    CHECK(sys_rt_sigreturn() == original_eax &&
            task.cpu.esp == nested_stack_pointer &&
            task.cpu.eip == original_eip &&
            task.altstack.stack == ALTSTACK_PAGE &&
            task.altstack.size == PAGE_SIZE,
            "rt_sigreturn 忽略仍在替代栈上的 EPERM 并恢复上下文");

    dword_t rejected_frame = UINT32_MAX;
    CHECK(!i386_signal_frame_pointer(&task, &altstack_action,
                    ALTSTACK_PAGE + 8, sizeof(struct rt_sigframe_),
                    &rejected_frame) &&
            rejected_frame == UINT32_MAX,
            "栈底空间不足时拒绝越界信号帧且不写输出");

    const dword_t unmapped_stack_top = UINT32_C(0x00600000);
    const dword_t original_ecx = UINT32_C(0x2468ace0);
    const dword_t original_edx = UINT32_C(0x0badc0de);
    const struct signal_action segv_altstack_action = {
        .handler = UINT32_C(0x0804d000),
        .mask = sig_mask(SIGALRM_),
        .flags = SA_SIGINFO_ | SA_ONSTACK_,
    };
    CHECK(task_sigaction(&task, SIGSEGV_,
                    &segv_altstack_action, NULL) == 0,
            "为建帧故障安装可捕获的替代栈 SIGSEGV 动作");
    task.cpu.esp = unmapped_stack_top;
    task.cpu.eip = original_eip;
    task.cpu.eax = original_eax;
    task.cpu.ecx = original_ecx;
    task.cpu.edx = original_edx;
    task.blocked = sig_mask(SIGTERM_);
    deliver_signal(&task, SIGUSR1_, (struct siginfo_) {.code = SI_USER_});
    deliver_signal(&task, SIGALRM_, (struct siginfo_) {.code = SI_USER_});
    receive_signals();
    addr_t segv_frame_address = task.cpu.esp;
    CHECK(segv_frame_address > ALTSTACK_PAGE &&
            segv_frame_address < ALTSTACK_PAGE + PAGE_SIZE &&
            task.cpu.eip == segv_altstack_action.handler &&
            task.pending == sig_mask(SIGALRM_) &&
            list_size(&task.queue) == 1 &&
            user_get(segv_frame_address, delivered_frame) == 0 &&
            delivered_frame.info.sig == SIGSEGV_ &&
            delivered_frame.uc.mcontext.sp == unmapped_stack_top &&
            delivered_frame.uc.mcontext.ip == original_eip &&
            delivered_frame.uc.mcontext.ax == original_eax &&
            delivered_frame.uc.mcontext.cx == original_ecx &&
            delivered_frame.uc.mcontext.dx == original_edx &&
            delivered_frame.uc.sigmask == sig_mask(SIGTERM_) &&
            task.blocked == (sig_mask(SIGTERM_) |
                    sig_mask(SIGSEGV_) | sig_mask(SIGALRM_)) &&
            sighand.action[SIGSEGV_].handler ==
                    segv_altstack_action.handler,
            "建帧故障立即转入替代栈 SIGSEGV 并应用 handler 掩码");
    CHECK(trylock(&sighand.lock) == 0,
            "信号帧用户写故障不会递归锁死 sighand");
    unlock(&sighand.lock);
    clear_pending_signals(&task);
    task.cpu.esp = normal_stack_top;
    cond_destroy(&signal_group.stopped_cond);

    CHECK(sigaction(SIGUSR1, &old_host_action, NULL) == 0,
            "恢复 host 唤醒信号动作");

    current = NULL;
    cond_destroy(&task.ptrace.cond);
    cond_destroy(&task.pause);
    mm_release(task.mm);
    return 0;
}

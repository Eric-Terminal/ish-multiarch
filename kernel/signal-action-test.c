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

int main(void) {
    struct task task = {0};
    struct sighand sighand = {0};
    lock_init(&sighand.lock);
    task.sighand = &sighand;
    list_init(&task.queue);
    task_set_mm(&task, mm_new());
    CHECK(task.mm != NULL, "创建 i386 用户地址空间");
    current = &task;

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
    const sigset_t_ wait_set = sig_mask(SIGALRM_);
    CHECK(user_put(wait_set_address, wait_set) == 0,
            "写入 sigtimedwait 等待集合");
    deliver_signal(&task, SIGALRM_, (struct siginfo_) {.code = SI_USER_});
    deliver_signal(&task, SIGUSR1_, (struct siginfo_) {.code = SI_USER_});
    CHECK(sigset_has(task.pending, SIGALRM_) &&
            sigset_has(task.pending, SIGUSR1_) &&
            list_size(&task.queue) == 2,
            "预排等待目标与无关信号");

    result = sys_rt_sigtimedwait(wait_set_address,
            wait_info_address, 0, sizeof(sigset_t_));
    struct siginfo_ waited_info;
    CHECK(result == SIGALRM_ &&
            user_get(wait_info_address, waited_info) == 0 &&
            waited_info.sig == SIGALRM_ && task.waiting == 0 &&
            !sigset_has(task.pending, SIGALRM_) &&
            sigset_has(task.pending, SIGUSR1_) &&
            list_size(&task.queue) == 1,
            "sigtimedwait 消费目标节点并只清对应 pending 位");

    deliver_signal(&task, SIGALRM_, (struct siginfo_) {.code = SI_USER_});
    CHECK(sigset_has(task.pending, SIGALRM_) &&
            list_size(&task.queue) == 2,
            "被等待消费的同号信号可以再次入队");

    struct sigqueue *queued, *queued_tmp;
    list_for_each_entry_safe(&task.queue, queued, queued_tmp, queue) {
        list_remove(&queued->queue);
        free(queued);
    }
    task.pending = 0;

    current = NULL;
    mm_release(task.mm);
    return 0;
}

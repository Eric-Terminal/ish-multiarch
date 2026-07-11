#include <stdio.h>

#include "guest/linux/signal-service-test.h"

#define TEST_NORMAL_SIGNAL 10
#define TEST_FORCED_SIGNAL 11

_Static_assert(sizeof(guest_addr_t) == 8,
        "signal service 调用方必须在 AArch64 guest 类型域编译");

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Linux signal service 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct installer_state {
    dword_t mode;
    dword_t calls;
    bool valid;
};

static bool common_delivery_is_valid(
        const struct guest_linux_signal_delivery *delivery) {
    return delivery->info.error == -7 && delivery->info.code == -2 &&
            delivery->action.handler == UINT64_C(0x1122334455667788) &&
            delivery->action.flags == UINT64_C(0x8877665544332211) &&
            delivery->action.restorer == UINT64_C(0x123456789abcdef0) &&
            delivery->action.mask == UINT64_C(0xfedcba9876543210) &&
            delivery->blocked_mask == UINT64_C(0x8000000000000040) &&
            delivery->altstack.base == UINT64_C(0x0000700012345000) &&
            delivery->altstack.size == UINT64_C(0x0000000123456000) &&
            delivery->altstack.flags == 1 &&
            delivery->altstack.reserved == 0;
}

static bool payload_is_valid(dword_t mode,
        const struct guest_linux_signal_info *info) {
    switch (mode) {
        case GUEST_LINUX_SIGNAL_TEST_KILL:
            return info->payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_KILL &&
                    info->kill.pid == 1234 && info->kill.uid == 5678;
        case GUEST_LINUX_SIGNAL_TEST_TIMER:
            return info->payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_TIMER &&
                    info->timer.timer == 9 && info->timer.overrun == 3 &&
                    info->timer.value == UINT64_C(0xabcdef0123456789) &&
                    info->timer.private_value == -5;
        case GUEST_LINUX_SIGNAL_TEST_CHILD:
            return info->payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_CHILD &&
                    info->child.pid == 2468 && info->child.uid == 1357 &&
                    info->child.status == -4 &&
                    info->child.utime == INT64_C(0x1122334412345678) &&
                    info->child.stime == INT64_C(0x2233445523456789);
        case GUEST_LINUX_SIGNAL_TEST_FAULT:
        case GUEST_LINUX_SIGNAL_TEST_FORCE_HANDLER:
        case GUEST_LINUX_SIGNAL_TEST_FORCE_TERMINATE:
            return info->payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_FAULT &&
                    info->fault.address == UINT64_C(0x12345678abcdef01);
        case GUEST_LINUX_SIGNAL_TEST_SIGSYS:
            return info->payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS &&
                    info->sigsys.address == UINT64_C(0x2345678912345678) &&
                    info->sigsys.syscall == 439 &&
                    info->sigsys.architecture == UINT32_C(0xc00000b7);
        default:
            return false;
    }
}

static enum guest_linux_signal_install_status test_installer(
        void *opaque,
        const struct guest_linux_signal_delivery *delivery) {
    struct installer_state *state = opaque;
    state->calls++;
    state->valid = state->valid && common_delivery_is_valid(delivery) &&
            payload_is_valid(state->mode, &delivery->info);

    if (state->mode == GUEST_LINUX_SIGNAL_TEST_FORCE_HANDLER ||
            state->mode == GUEST_LINUX_SIGNAL_TEST_FORCE_TERMINATE) {
        sdword_t expected_signal = state->calls == 1 ?
                TEST_NORMAL_SIGNAL : TEST_FORCED_SIGNAL;
        state->valid = state->valid &&
                delivery->info.signal == expected_signal;
        if (state->calls == 1 ||
                state->mode == GUEST_LINUX_SIGNAL_TEST_FORCE_TERMINATE)
            return GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT;
    } else {
        state->valid = state->valid &&
                delivery->info.signal == TEST_NORMAL_SIGNAL;
    }
    return GUEST_LINUX_SIGNAL_INSTALL_COMPLETE;
}

static struct guest_linux_signal_poll_result poll_mode(
        dword_t mode, struct installer_state *state, void *task_opaque) {
    guest_linux_signal_test_configure(mode, task_opaque);
    *state = (struct installer_state) {
        .mode = mode,
        .valid = true,
    };
    const struct guest_linux_signal_context context = {
        .runtime_opaque = guest_linux_signal_test_service.runtime_opaque,
        .task_opaque = task_opaque,
    };
    return guest_linux_signal_test_service.poll(
            &context, test_installer, state);
}

int main(void) {
    byte_t task_cookie;
    struct installer_state state;
    const dword_t handler_modes[] = {
        GUEST_LINUX_SIGNAL_TEST_KILL,
        GUEST_LINUX_SIGNAL_TEST_TIMER,
        GUEST_LINUX_SIGNAL_TEST_CHILD,
        GUEST_LINUX_SIGNAL_TEST_FAULT,
        GUEST_LINUX_SIGNAL_TEST_SIGSYS,
    };
    for (size_t index = 0; index < array_size(handler_modes); index++) {
        struct guest_linux_signal_poll_result result = poll_mode(
                handler_modes[index], &state, &task_cookie);
        CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
                result.signal == TEST_NORMAL_SIGNAL &&
                state.calls == 1 && state.valid &&
                guest_linux_signal_test_poll_count() == 1,
                "完整 handler DTO 跨 guest 类型域同步安装");
    }

    struct guest_linux_signal_poll_result result = poll_mode(
            GUEST_LINUX_SIGNAL_TEST_IDLE, &state, &task_cookie);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_IDLE &&
            result.signal == 0 && state.calls == 0 &&
            guest_linux_signal_test_poll_count() == 1,
            "无可派送信号时不调用 installer");

    result = poll_mode(GUEST_LINUX_SIGNAL_TEST_STOP,
            &state, &task_cookie);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_STOP &&
            result.signal == 19 && state.calls == 0,
            "默认停止动作不构造 handler 帧");

    result = poll_mode(GUEST_LINUX_SIGNAL_TEST_TERMINATE,
            &state, &task_cookie);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            result.signal == 15 && state.calls == 0,
            "默认终止动作不构造 handler 帧");

    result = poll_mode(GUEST_LINUX_SIGNAL_TEST_FORCE_HANDLER,
            &state, &task_cookie);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_HANDLER &&
            result.signal == TEST_FORCED_SIGNAL &&
            state.calls == 2 && state.valid,
            "建帧失败在同一次 poll 内改装强制 SIGSEGV handler");

    result = poll_mode(GUEST_LINUX_SIGNAL_TEST_FORCE_TERMINATE,
            &state, &task_cookie);
    CHECK(result.status == GUEST_LINUX_SIGNAL_POLL_TERMINATE &&
            result.signal == TEST_FORCED_SIGNAL &&
            state.calls == 2 && state.valid,
            "强制 SIGSEGV 再次建帧失败时直接终止");
    return 0;
}

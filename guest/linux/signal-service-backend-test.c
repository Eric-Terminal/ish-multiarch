#include <assert.h>

#include "guest/linux/signal-service-test.h"

#define TEST_NORMAL_SIGNAL 10
#define TEST_FORCED_SIGNAL 11

_Static_assert(sizeof(guest_addr_t) == 4,
        "signal service 假 backend 必须在 i386 guest 类型域编译");

static byte_t runtime_cookie;
static dword_t test_mode;
static void *test_task_opaque;
static dword_t poll_count;

void guest_linux_signal_test_configure(
        dword_t mode, void *expected_task_opaque) {
    test_mode = mode;
    test_task_opaque = expected_task_opaque;
    poll_count = 0;
}

dword_t guest_linux_signal_test_poll_count(void) {
    return poll_count;
}

static struct guest_linux_signal_delivery make_delivery(
        dword_t mode, sdword_t signal) {
    struct guest_linux_signal_delivery delivery = {
        .info = {
            .signal = signal,
            .error = -7,
            .code = -2,
        },
        .action = {
            .handler = UINT64_C(0x1122334455667788),
            .flags = UINT64_C(0x8877665544332211),
            .restorer = UINT64_C(0x123456789abcdef0),
            .mask = UINT64_C(0xfedcba9876543210),
        },
        .blocked_mask = UINT64_C(0x8000000000000040),
        .altstack = {
            .base = UINT64_C(0x0000700012345000),
            .size = UINT64_C(0x0000000123456000),
            .flags = 1,
        },
    };
    switch (mode) {
        case GUEST_LINUX_SIGNAL_TEST_KILL:
            delivery.info.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_KILL;
            delivery.info.kill.pid = 1234;
            delivery.info.kill.uid = 5678;
            break;
        case GUEST_LINUX_SIGNAL_TEST_TIMER:
            delivery.info.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_TIMER;
            delivery.info.timer.timer = 9;
            delivery.info.timer.overrun = 3;
            delivery.info.timer.value = UINT64_C(0xabcdef0123456789);
            delivery.info.timer.private_value = -5;
            break;
        case GUEST_LINUX_SIGNAL_TEST_CHILD:
            delivery.info.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_CHILD;
            delivery.info.child.pid = 2468;
            delivery.info.child.uid = 1357;
            delivery.info.child.status = -4;
            delivery.info.child.utime = INT64_C(0x1122334412345678);
            delivery.info.child.stime = INT64_C(0x2233445523456789);
            break;
        case GUEST_LINUX_SIGNAL_TEST_FAULT:
        case GUEST_LINUX_SIGNAL_TEST_FORCE_HANDLER:
        case GUEST_LINUX_SIGNAL_TEST_FORCE_TERMINATE:
            delivery.info.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_FAULT;
            delivery.info.fault.address = UINT64_C(0x12345678abcdef01);
            break;
        case GUEST_LINUX_SIGNAL_TEST_SIGSYS:
            delivery.info.payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS;
            delivery.info.sigsys.address = UINT64_C(0x2345678912345678);
            delivery.info.sigsys.syscall = 439;
            delivery.info.sigsys.architecture = UINT32_C(0xc00000b7);
            break;
        default:
            break;
    }
    return delivery;
}

static struct guest_linux_signal_poll_result test_poll(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque) {
    assert(context != NULL && installer != NULL);
    assert(context->runtime_opaque == &runtime_cookie);
    assert(context->task_opaque == test_task_opaque);
    poll_count++;

    if (test_mode == GUEST_LINUX_SIGNAL_TEST_IDLE)
        return (struct guest_linux_signal_poll_result) {
            .status = GUEST_LINUX_SIGNAL_POLL_IDLE,
        };
    if (test_mode == GUEST_LINUX_SIGNAL_TEST_STOP ||
            test_mode == GUEST_LINUX_SIGNAL_TEST_TERMINATE) {
        return (struct guest_linux_signal_poll_result) {
            .status = test_mode == GUEST_LINUX_SIGNAL_TEST_STOP ?
                    GUEST_LINUX_SIGNAL_POLL_STOP :
                    GUEST_LINUX_SIGNAL_POLL_TERMINATE,
            .signal = test_mode == GUEST_LINUX_SIGNAL_TEST_STOP ? 19 : 15,
        };
    }

    struct guest_linux_signal_delivery delivery = make_delivery(
            test_mode, TEST_NORMAL_SIGNAL);
    enum guest_linux_signal_install_status status =
            installer(installer_opaque, &delivery);
    if (status == GUEST_LINUX_SIGNAL_INSTALL_COMPLETE) {
        return (struct guest_linux_signal_poll_result) {
            .status = GUEST_LINUX_SIGNAL_POLL_HANDLER,
            .signal = delivery.info.signal,
        };
    }

    delivery = make_delivery(test_mode, TEST_FORCED_SIGNAL);
    status = installer(installer_opaque, &delivery);
    return (struct guest_linux_signal_poll_result) {
        .status = status == GUEST_LINUX_SIGNAL_INSTALL_COMPLETE ?
                GUEST_LINUX_SIGNAL_POLL_HANDLER :
                GUEST_LINUX_SIGNAL_POLL_TERMINATE,
        .signal = TEST_FORCED_SIGNAL,
    };
}

const struct guest_linux_signal_service guest_linux_signal_test_service = {
    .runtime_opaque = &runtime_cookie,
    .poll = test_poll,
};

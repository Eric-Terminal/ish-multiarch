#include <stdio.h>
#include <string.h>

#include "guest/aarch64/linux-signal-info.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 siginfo 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static void put_dword(byte_t *bytes, size_t offset, dword_t value) {
    for (byte_t index = 0; index < 4; index++)
        bytes[offset + index] = (byte_t) (value >> (index * 8));
}

static void put_qword(byte_t *bytes, size_t offset, qword_t value) {
    for (byte_t index = 0; index < 8; index++)
        bytes[offset + index] = (byte_t) (value >> (index * 8));
}

static void make_expected(const struct guest_linux_signal_info *info,
        byte_t expected[128]) {
    memset(expected, 0, 128);
    put_dword(expected, 0, (dword_t) info->signal);
    put_dword(expected, 4, (dword_t) info->error);
    put_dword(expected, 8, (dword_t) info->code);
    switch (info->payload_kind) {
        case GUEST_LINUX_SIGNAL_PAYLOAD_KILL:
            put_dword(expected, 16, (dword_t) info->kill.pid);
            put_dword(expected, 20, info->kill.uid);
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_TIMER:
            put_dword(expected, 16, (dword_t) info->timer.timer);
            put_dword(expected, 20, (dword_t) info->timer.overrun);
            put_qword(expected, 24, info->timer.value);
            put_dword(expected, 32,
                    (dword_t) info->timer.private_value);
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_CHILD:
            put_dword(expected, 16, (dword_t) info->child.pid);
            put_dword(expected, 20, info->child.uid);
            put_dword(expected, 24, (dword_t) info->child.status);
            put_qword(expected, 32, (qword_t) info->child.utime);
            put_qword(expected, 40, (qword_t) info->child.stime);
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_FAULT:
            put_qword(expected, 16, info->fault.address);
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS:
            put_qword(expected, 16, info->sigsys.address);
            put_dword(expected, 24, (dword_t) info->sigsys.syscall);
            put_dword(expected, 28, info->sigsys.architecture);
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_QUEUE:
            put_dword(expected, 16, (dword_t) info->queue.pid);
            put_dword(expected, 20, info->queue.uid);
            put_qword(expected, 24, info->queue.value);
            break;
        case GUEST_LINUX_SIGNAL_PAYLOAD_NONE:
        default:
            break;
    }
}

int main(void) {
    const struct guest_linux_signal_info cases[] = {
        {
            .signal = 5, .error = -1, .code = 128,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_NONE,
        },
        {
            .signal = 10, .error = -2, .code = 0,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_KILL,
            .kill = {.pid = -1234, .uid = UINT32_C(0x89abcdef)},
        },
        {
            .signal = 14, .error = -3, .code = -2,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_TIMER,
            .timer = {
                .timer = -9,
                .overrun = 7,
                .value = UINT64_C(0xabcdef0123456789),
                .private_value = -5,
            },
        },
        {
            .signal = 17, .error = -4, .code = 1,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_CHILD,
            .child = {
                .pid = 2468,
                .uid = UINT32_C(0xfedcba98),
                .status = -6,
                .utime = INT64_C(0x1122334412345678),
                .stime = -INT64_C(0x1234567890),
            },
        },
        {
            .signal = 11, .error = -5, .code = 2,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_FAULT,
            .fault.address = UINT64_C(0x12345678abcdef01),
        },
        {
            .signal = 31, .error = -6, .code = 1,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS,
            .sigsys = {
                .address = UINT64_C(0x2345678912345678),
                .syscall = 439,
                .architecture = UINT32_C(0xc00000b7),
            },
        },
        {
            .signal = 12, .error = -7, .code = -1,
            .payload_kind = GUEST_LINUX_SIGNAL_PAYLOAD_QUEUE,
            .queue = {
                .pid = -1357,
                .uid = UINT32_C(0x89abcdef),
                .value = UINT64_C(0xfedcba9876543210),
            },
        },
        {
            .signal = 13, .error = -8, .code = -9,
            .payload_kind = UINT32_MAX,
        },
    };

    for (size_t index = 0; index < array_size(cases); index++) {
        struct aarch64_linux_siginfo wire =
                aarch64_linux_pack_siginfo(&cases[index]);
        byte_t expected[128];
        make_expected(&cases[index], expected);
        CHECK(memcmp(&wire, expected, sizeof(expected)) == 0,
                "完整 128 字节 wire 与固定 payload 偏移一致");
    }

    struct aarch64_linux_siginfo queue_wire = {
        .signo = 63,
        .error = -11,
        .code = -1,
        .reserved = UINT32_MAX,
        .queue = {
            .pid = -9753,
            .uid = UINT32_C(0xa1b2c3d4),
            .value = UINT64_C(0x0123456789abcdef),
        },
    };
    struct guest_linux_signal_info unpacked =
            aarch64_linux_unpack_sigqueueinfo(32, &queue_wire);
    CHECK(unpacked.signal == 32 && unpacked.error == -11 &&
            unpacked.code == -1 &&
            unpacked.payload_kind == GUEST_LINUX_SIGNAL_PAYLOAD_QUEUE &&
            unpacked.queue.pid == -9753 &&
            unpacked.queue.uid == UINT32_C(0xa1b2c3d4) &&
            unpacked.queue.value == UINT64_C(0x0123456789abcdef),
            "AArch64 queue 解包忽略 wire signo 并保留完整 64 位 sigval");
    return 0;
}

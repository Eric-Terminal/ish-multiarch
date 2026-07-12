#include <stdio.h>
#include <string.h>

#include "kernel/signal-info.h"

#define TEST_SIGTRAP 5
#define TEST_SIGUSR1 10
#define TEST_SIGSEGV 11
#define TEST_SIGUSR2 12
#define TEST_SIGALRM 14
#define TEST_SIGSYS 31
#define TEST_SI_USER 0
#define TEST_SI_QUEUE -1
#define TEST_SI_TIMER -2
#define TEST_SI_KERNEL 128
#define TEST_SEGV_MAPERR 1

static addr_t captured_address;
static byte_t captured_bytes[128];
static size_t captured_size;

int user_write(addr_t address, const void *source, size_t size) {
    captured_address = address;
    captured_size = size;
    if (size <= sizeof(captured_bytes))
        memcpy(captured_bytes, source, size);
    return 0;
}

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 siginfo 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    const struct siginfo_ none = {
        .sig = TEST_SIGTRAP,
        .sig_errno = -2,
        .code = TEST_SI_KERNEL,
    };
    const struct i386_siginfo expected_none = {
        .sig = TEST_SIGTRAP,
        .sig_errno = -2,
        .code = TEST_SI_KERNEL,
    };
    struct i386_siginfo wire = pack_i386_siginfo(&none);
    CHECK(memcmp(&wire, &expected_none, sizeof(wire)) == 0,
            "无 payload 时只序列化前导字段并清零剩余空间");

    const struct siginfo_ kill = {
        .sig = TEST_SIGUSR1,
        .code = TEST_SI_USER,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
        .kill = {.pid = 1234, .uid = 5678},
    };
    const struct i386_siginfo expected_kill = {
        .sig = TEST_SIGUSR1,
        .code = TEST_SI_USER,
        .kill = {.pid = 1234, .uid = 5678},
    };
    wire = pack_i386_siginfo(&kill);
    CHECK(memcmp(&wire, &expected_kill, sizeof(wire)) == 0,
            "kill payload 使用 i386 固定偏移");

    const struct siginfo_ queued = {
        .sig = TEST_SIGUSR2,
        .sig_errno = -7,
        .code = TEST_SI_QUEUE,
        .payload_kind = SIGNAL_INFO_PAYLOAD_QUEUE,
        .queue = {
            .pid = -2468,
            .uid = UINT32_C(0x89abcdef),
            .value = UINT64_C(0x1122334455667788),
        },
    };
    const struct i386_siginfo expected_queue = {
        .sig = TEST_SIGUSR2,
        .sig_errno = -7,
        .code = TEST_SI_QUEUE,
        .queue = {
            .pid = -2468,
            .uid = UINT32_C(0x89abcdef),
            .value = UINT32_C(0x55667788),
        },
    };
    wire = pack_i386_siginfo(&queued);
    struct siginfo_ unpacked = unpack_i386_sigqueueinfo(
            TEST_SIGUSR1, &expected_queue);
    CHECK(memcmp(&wire, &expected_queue, sizeof(wire)) == 0 &&
            unpacked.sig == TEST_SIGUSR1 &&
            unpacked.sig_errno == -7 && unpacked.code == TEST_SI_QUEUE &&
            unpacked.payload_kind == SIGNAL_INFO_PAYLOAD_QUEUE &&
            unpacked.queue.pid == -2468 &&
            unpacked.queue.uid == UINT32_C(0x89abcdef) &&
            unpacked.queue.value == UINT64_C(0x55667788),
            "i386 queue wire 使用系统调用信号号并把 sigval 限定为 32 位");

    const struct siginfo_ timer = {
        .sig = TEST_SIGALRM,
        .code = TEST_SI_TIMER,
        .payload_kind = SIGNAL_INFO_PAYLOAD_TIMER,
        .timer = {
            .timer = 7,
            .overrun = 3,
            .value = UINT64_C(0x123456786bcdef01),
            ._private = 9,
        },
    };
    const struct i386_siginfo expected_timer = {
        .sig = TEST_SIGALRM,
        .code = TEST_SI_TIMER,
        .timer = {
            .timer = 7,
            .overrun = 3,
            .value = UINT32_C(0x6bcdef01),
            ._private = 9,
        },
    };
    wire = pack_i386_siginfo(&timer);
    CHECK(memcmp(&wire, &expected_timer, sizeof(wire)) == 0 &&
            timer.timer.value == UINT64_C(0x123456786bcdef01),
            "timer 原始值只在 i386 wire 边界截断");

    const struct siginfo_ child = {
        .sig = TEST_SIGUSR2,
        .code = TEST_SI_KERNEL,
        .payload_kind = SIGNAL_INFO_PAYLOAD_CHILD,
        .child = {
            .pid = 2468,
            .uid = 1357,
            .status = -3,
            .utime = INT64_C(0x1122334412345678),
            .stime = INT64_C(0x2233445523456789),
        },
    };
    const struct i386_siginfo expected_child = {
        .sig = TEST_SIGUSR2,
        .code = TEST_SI_KERNEL,
        .child = {
            .pid = 2468,
            .uid = 1357,
            .status = -3,
            .utime = INT32_C(0x12345678),
            .stime = INT32_C(0x23456789),
        },
    };
    wire = pack_i386_siginfo(&child);
    CHECK(memcmp(&wire, &expected_child, sizeof(wire)) == 0 &&
            child.child.utime == INT64_C(0x1122334412345678),
            "非 SIGCHLD 的 CHILD payload 仍按 tag 序列化 64 位时钟");

    const struct siginfo_ fault = {
        .sig = TEST_SIGSEGV,
        .code = TEST_SEGV_MAPERR,
        .payload_kind = SIGNAL_INFO_PAYLOAD_FAULT,
        .fault.addr = UINT64_C(0x12345678abcdef01),
    };
    const struct i386_siginfo expected_fault = {
        .sig = TEST_SIGSEGV,
        .code = TEST_SEGV_MAPERR,
        .fault.addr = UINT32_C(0xabcdef01),
    };
    wire = pack_i386_siginfo(&fault);
    CHECK(memcmp(&wire, &expected_fault, sizeof(wire)) == 0 &&
            fault.fault.addr == UINT64_C(0x12345678abcdef01),
            "内部故障地址保持 64 位并只在 i386 wire 截断");
    const dword_t output_address = UINT32_C(0x12345678);
    CHECK(write_i386_siginfo(output_address, &fault) == 0 &&
            captured_address == output_address &&
            captured_size == sizeof(struct i386_siginfo) &&
            memcmp(captured_bytes, &expected_fault,
                    sizeof(expected_fault)) == 0,
            "共享写出边界恰好提交 128 字节 i386 wire");

    const struct siginfo_ sigsys = {
        .sig = TEST_SIGSYS,
        .code = TEST_SI_KERNEL,
        .payload_kind = SIGNAL_INFO_PAYLOAD_SIGSYS,
        .sigsys = {
            .addr = UINT64_C(0x2345678912345678),
            .syscall = 439,
            .arch = UINT32_C(0xc00000b7),
        },
    };
    const struct i386_siginfo expected_sigsys = {
        .sig = TEST_SIGSYS,
        .code = TEST_SI_KERNEL,
        .sigsys = {
            .addr = UINT32_C(0x12345678),
            .syscall = 439,
            .arch = UINT32_C(0xc00000b7),
        },
    };
    wire = pack_i386_siginfo(&sigsys);
    CHECK(memcmp(&wire, &expected_sigsys, sizeof(wire)) == 0,
            "SIGSYS 地址、系统调用号与架构号按固定偏移序列化");
    return 0;
}

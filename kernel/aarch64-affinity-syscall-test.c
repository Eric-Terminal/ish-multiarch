#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 256
#define MASK_ADDRESS (USER_BASE + 5)
#define SCHED_GETAFFINITY_SYSCALL 123
#define CPUSET_MAX_CPUS 1024

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 sched_getaffinity 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    bool fail_write;
    unsigned writes;
    qword_t last_write_address;
    dword_t last_write_size;
};

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    probe->last_write_address = address;
    probe->last_write_size = size;
    if (probe->fail_write || address < USER_BASE ||
            address - USER_BASE > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - (address - USER_BASE)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    memcpy(probe->bytes + (address - USER_BASE), source, size);
    return true;
}

static void reset_probe(struct user_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    memset(probe->bytes, 0xa5, sizeof(probe->bytes));
}

static dword_t expected_cpu_count(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1)
        cpus = 1;
    if (cpus > CPUSET_MAX_CPUS)
        cpus = CPUSET_MAX_CPUS;
    return (dword_t) cpus;
}

static dword_t expected_mask_size(void) {
    return ((expected_cpu_count() + 63) / 64) * 8;
}

static qword_t invoke(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault,
        qword_t pid, qword_t size, qword_t address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {
            .opaque = probe,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = SCHED_GETAFFINITY_SYSCALL,
        .arguments = {pid, size, address},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static int test_online_mask(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    dword_t cpus = expected_cpu_count();
    dword_t mask_size = expected_mask_size();
    qword_t result = invoke(task, &probe, &fault,
            0, 128, MASK_ADDRESS);
    CHECK(result == mask_size && probe.writes == 1 &&
            probe.last_write_address == MASK_ADDRESS &&
            probe.last_write_size == mask_size &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "宽缓冲区应只写回主机在线 CPU 掩码的有效长度");

    size_t offset = MASK_ADDRESS - USER_BASE;
    for (dword_t cpu = 0; cpu < mask_size * 8; cpu++) {
        bool enabled = (probe.bytes[offset + cpu / 8] &
                (byte_t) (1u << (cpu % 8))) != 0;
        CHECK(enabled == (cpu < cpus),
                "掩码应连续标记所有在线 CPU 且清零尾部位");
    }
    CHECK(probe.bytes[offset - 1] == 0xa5 &&
            probe.bytes[offset + mask_size] == 0xa5,
            "写回不得越过返回的掩码长度");

    reset_probe(&probe);
    result = invoke(task, &probe, &fault,
            0, mask_size, MASK_ADDRESS);
    CHECK(result == mask_size && probe.writes == 1,
            "恰好容纳掩码的对齐缓冲区应成功");
    return 0;
}

static int test_size_and_pid_errors(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    dword_t mask_size = expected_mask_size();
    reset_probe(&probe);
    qword_t result = invoke(task, &probe, &fault,
            0, mask_size - 8, MASK_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.writes == 0,
            "过小的 CPU 集合缓冲区应返回 EINVAL");

    reset_probe(&probe);
    result = invoke(task, &probe, &fault,
            0, mask_size + 1, MASK_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.writes == 0,
            "未按 AArch64 unsigned long 对齐的长度应返回 EINVAL");

    reset_probe(&probe);
    result = invoke(task, &probe, &fault,
            MAX_PID + 1, mask_size, MASK_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _ESRCH &&
            probe.writes == 0,
            "不存在的 PID 应返回 ESRCH 且不触碰输出缓冲区");
    return 0;
}

static int test_output_faults(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    dword_t mask_size = expected_mask_size();
    reset_probe(&probe);
    qword_t crossing = AARCH64_LINUX_USER_ADDRESS_MAX - mask_size + 2;
    qword_t result = invoke(task, &probe, &fault,
            0, mask_size, crossing);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 0 && fault.address == crossing &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "跨越 AArch64 用户地址上限时应在回调前返回 EFAULT");

    reset_probe(&probe);
    probe.fail_write = true;
    result = invoke(task, &probe, &fault,
            0, mask_size, MASK_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 1 && fault.address == MASK_ADDRESS &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "用户内存写入失败时应传播精确 EFAULT");
    return 0;
}

int main(void) {
    struct task task = {0};
    current = &task;
    int result = test_online_mask(&task);
    if (result == 0)
        result = test_size_and_pid_errors(&task);
    if (result == 0)
        result = test_output_faults(&task);
    current = NULL;
    return result;
}

#include <stdio.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-system-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 160
#define INFO_ADDRESS (USER_BASE + 3)
#define SYSINFO_SYSCALL 179

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 sysinfo 测试失败：%s（第 %d 行）\n", \
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

static qword_t invoke(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault, qword_t address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {
            .opaque = probe,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = SYSINFO_SYSCALL,
        .arguments = {address},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static int test_resource_snapshot(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    qword_t result = invoke(task, &probe, &fault, INFO_ADDRESS);
    struct aarch64_linux_sysinfo info;
    memcpy(&info, probe.bytes + (INFO_ADDRESS - USER_BASE), sizeof(info));
    CHECK(result == 0 && probe.writes == 1 &&
            probe.last_write_address == INFO_ADDRESS &&
            probe.last_write_size == sizeof(info) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "sysinfo 应一次写回完整的 112 字节 LP64 快照");
    CHECK(info.uptime >= 0 && info.totalram > 0 &&
            info.freeram <= info.totalram && info.sharedram == 0 &&
            info.bufferram == 0 && info.totalswap == 0 &&
            info.freeswap == 0 && info.procs == 1 &&
            info.totalhigh == 0 && info.freehigh == 0 &&
            info.mem_unit == 1,
            "资源字段应使用 64 位字节值并报告当前无交换区");
    CHECK(info.padding == 0 && info.alignment_padding == 0 &&
            info.tail_padding == 0,
            "所有 ABI 填充字段必须清零，避免泄露宿主栈数据");

    size_t offset = INFO_ADDRESS - USER_BASE;
    CHECK(probe.bytes[offset - 1] == 0xa5 &&
            probe.bytes[offset + sizeof(info)] == 0xa5,
            "资源快照写回不得越过 LP64 结构边界");
    return 0;
}

static int test_output_faults(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    qword_t crossing = AARCH64_LINUX_USER_ADDRESS_MAX -
            sizeof(struct aarch64_linux_sysinfo) + 2;
    qword_t result = invoke(task, &probe, &fault, crossing);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 0 && fault.address == crossing &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "跨越 AArch64 用户地址上限时应在读取宿主资源前失败");

    reset_probe(&probe);
    probe.fail_write = true;
    result = invoke(task, &probe, &fault, INFO_ADDRESS);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 1 && fault.address == INFO_ADDRESS &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "用户内存写入失败时应传播精确 EFAULT");
    return 0;
}

int main(void) {
    struct task task = {0};
    current = &task;
    int result = test_resource_snapshot(&task);
    if (result == 0)
        result = test_output_faults(&task);
    current = NULL;
    return result;
}

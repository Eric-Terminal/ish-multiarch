#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 256
#define NEW_STACK_ADDRESS (USER_BASE + 32)
#define OLD_STACK_ADDRESS (USER_BASE + 96)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 sigaltstack 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned reads;
    unsigned writes;
};

static bool user_range(qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE)
        return false;
    qword_t relative = address - USER_BASE;
    if (relative > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
}

static bool probe_read(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->reads++;
    size_t offset;
    if (!user_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->fail_read_at >= address &&
            probe->fail_read_at - address < size) {
        dword_t prefix = (dword_t) (probe->fail_read_at - address);
        memcpy(destination, probe->bytes + offset, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_read_at,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    memcpy(destination, probe->bytes + offset, size);
    return true;
}

static bool probe_write(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    size_t offset;
    if (!user_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->fail_write_at >= address &&
            probe->fail_write_at - address < size) {
        dword_t prefix = (dword_t) (probe->fail_write_at - address);
        memcpy(probe->bytes + offset, source, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_write_at,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    memcpy(probe->bytes + offset, source, size);
    return true;
}

static void reset_probe(struct user_probe *probe, byte_t fill) {
    memset(probe->bytes, fill, sizeof(probe->bytes));
    probe->fail_read_at = UINT64_MAX;
    probe->fail_write_at = UINT64_MAX;
    probe->reads = 0;
    probe->writes = 0;
}

static void store_stack(struct user_probe *probe, qword_t address,
        struct aarch64_linux_stack stack) {
    size_t offset;
    assert(user_range(address, sizeof(stack), &offset));
    memcpy(probe->bytes + offset, &stack, sizeof(stack));
}

static struct aarch64_linux_stack load_stack(
        const struct user_probe *probe, qword_t address) {
    assert(address >= USER_BASE && address - USER_BASE <=
            sizeof(probe->bytes) - sizeof(struct aarch64_linux_stack));
    struct aarch64_linux_stack stack;
    memcpy(&stack, probe->bytes + (size_t) (address - USER_BASE),
            sizeof(stack));
    return stack;
}

static qword_t invoke(struct task *task, struct user_probe *probe,
        qword_t stack_pointer, qword_t new_address, qword_t old_address,
        struct guest_linux_user_fault *fault) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .stack_pointer = stack_pointer,
        .user = {
            .opaque = probe,
            .read = probe_read,
            .write = probe_write,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = 132,
        .arguments = {new_address, old_address},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

int main(void) {
    struct task task = {0};
    task_altstack_reset(&task);
    current = &task;
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    const qword_t outside_sp = UINT64_C(0x0000600012345000);
    const qword_t first_base = UINT64_C(0x0000700012345000);
    const qword_t second_base = UINT64_C(0x0000700023456000);

    reset_probe(&probe, 0);
    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = first_base,
                .flags = 0,
                .reserved = UINT32_C(0xa5a5a5a5),
                .size = UINT64_C(8192),
            });
    qword_t result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == 0 && probe.reads == 1 && probe.writes == 0 &&
            task.altstack.stack == first_base &&
            task.altstack.size == UINT64_C(8192) &&
            task.altstack.flags == 0,
            "安装完整 64 位替代栈并忽略 wire 填充");

    reset_probe(&probe, 0xa5);
    result = invoke(&task, &probe, outside_sp,
            0, OLD_STACK_ADDRESS, &fault);
    struct aarch64_linux_stack old = load_stack(
            &probe, OLD_STACK_ADDRESS);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
            old.sp == first_base && old.size == UINT64_C(8192) &&
            old.flags == 0 && old.reserved == 0,
            "查询替代栈时写回固定布局并清零 reserved");

    reset_probe(&probe, 0);
    result = invoke(&task, &probe, first_base + 128,
            0, OLD_STACK_ADDRESS, &fault);
    old = load_stack(&probe, OLD_STACK_ADDRESS);
    CHECK(result == 0 && old.flags == SS_ONSTACK_,
            "guest SP 位于替代栈时报告 SS_ONSTACK");

    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = second_base,
                .size = UINT64_C(8192),
            });
    result = invoke(&task, &probe, first_base + 128,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == encoded_error(_EPERM) && probe.reads == 1 &&
            task.altstack.stack == first_base,
            "正在替代栈上执行时拒绝修改配置");

    reset_probe(&probe, 0);
    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = second_base,
                .flags = SS_ONSTACK_,
                .size = UINT64_C(8192),
            });
    result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == encoded_error(_EINVAL) &&
            task.altstack.stack == first_base,
            "系统调用边界拒绝只供查询使用的 SS_ONSTACK");

    reset_probe(&probe, 0);
    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = second_base,
                .size = AARCH64_LINUX_MINSIGSTKSZ - 1,
            });
    result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == encoded_error(_ENOMEM) &&
            task.altstack.stack == first_base,
            "拒绝小于 AArch64 MINSIGSTKSZ 的替代栈");

    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = AARCH64_LINUX_USER_ADDRESS_MAX - 4096,
                .size = UINT64_C(8192),
            });
    result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == encoded_error(_ENOMEM) &&
            task.altstack.stack == first_base,
            "拒绝栈顶越过 48 位用户地址上界");

    reset_probe(&probe, 0);
    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = second_base,
                .size = UINT64_C(8192),
            });
    probe.fail_read_at = NEW_STACK_ADDRESS + 8;
    result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == encoded_error(_EFAULT) && probe.reads == 1 &&
            probe.writes == 0 && task.altstack.stack == first_base &&
            fault.address == probe.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "输入部分读取故障不修改替代栈");

    reset_probe(&probe, 0xcc);
    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = second_base,
                .size = UINT64_C(16384),
            });
    probe.fail_write_at = OLD_STACK_ADDRESS + 12;
    result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, OLD_STACK_ADDRESS, &fault);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.reads == 1 && probe.writes == 1 &&
            task.altstack.stack == second_base &&
            task.altstack.size == UINT64_C(16384) &&
            fault.address == probe.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "旧配置部分写失败不回滚已提交的新替代栈");

    reset_probe(&probe, 0);
    store_stack(&probe, NEW_STACK_ADDRESS,
            (struct aarch64_linux_stack) {
                .sp = UINT64_MAX,
                .flags = SS_DISABLE_,
                .size = UINT64_MAX,
            });
    result = invoke(&task, &probe, outside_sp,
            NEW_STACK_ADDRESS, 0, &fault);
    CHECK(result == 0 && task.altstack.stack == 0 &&
            task.altstack.size == 0 &&
            task.altstack.flags == SS_DISABLE_,
            "SS_DISABLE 忽略地址与大小并清除配置");

    reset_probe(&probe, 0xa5);
    result = invoke(&task, &probe, outside_sp,
            0, OLD_STACK_ADDRESS, &fault);
    old = load_stack(&probe, OLD_STACK_ADDRESS);
    CHECK(result == 0 && old.sp == 0 && old.size == 0 &&
            old.flags == SS_DISABLE_ && old.reserved == 0,
            "禁用状态查询结果保持规范化");

    current = NULL;
    return 0;
}

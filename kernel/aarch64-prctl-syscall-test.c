#include <stdio.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 prctl 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct user_memory {
    byte_t bytes[64];
    unsigned reads;
    unsigned writes;
};

static bool check_range(qword_t address, dword_t size,
        enum guest_memory_access access, struct guest_linux_user_fault *fault) {
    if (address < USER_BASE || address - USER_BASE > 64 ||
            size > 64 - (address - USER_BASE)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = access,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    return true;
}

static bool read_user(void *opaque, qword_t address, void *destination,
        dword_t size, struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    memory->reads++;
    if (!check_range(address, size, GUEST_MEMORY_READ, fault))
        return false;
    // guest 内存回调不能在持有线程名称锁时执行。
    lock(&current->general_lock);
    memcpy(destination, memory->bytes + (address - USER_BASE), size);
    unlock(&current->general_lock);
    return true;
}

static bool write_user(void *opaque, qword_t address, const void *source,
        dword_t size, struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    memory->writes++;
    if (!check_range(address, size, GUEST_MEMORY_WRITE, fault))
        return false;
    lock(&current->general_lock);
    memcpy(memory->bytes + (address - USER_BASE), source, size);
    unlock(&current->general_lock);
    return true;
}

static qword_t invoke(struct user_memory *memory,
        struct guest_linux_user_fault *fault, qword_t option, qword_t address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = current,
        .user = {.opaque = memory, .read = read_user, .write = write_user},
    };
    const struct guest_linux_syscall syscall = {
        .number = 167,
        // 名称操作忽略未使用的参数，option 遵循 Linux int ABI。
        .arguments = {option, address, UINT64_MAX, UINT64_MAX, UINT64_MAX},
    };
    return ish_aarch64_linux_syscall_service.dispatch(&context, &syscall, fault);
}

static bool test_names(void) {
    struct user_memory memory = {0};
    struct guest_linux_user_fault fault;
    memcpy(memory.bytes + 1, "uv-worker", sizeof("uv-worker"));
    CHECK(invoke(&memory, &fault, UINT64_C(0x10000000f), USER_BASE + 1) == 0 &&
            strcmp(current->comm, "uv-worker") == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "高地址非对齐指针设置当前线程名称，保留 64 位指针");
    memset(memory.bytes, 0xcc, sizeof(memory.bytes));
    CHECK(invoke(&memory, &fault, 16, USER_BASE + 1) == 0,
            "读取线程名称成功");
    const char expected[16] = "uv-worker";
    CHECK(memcmp(memory.bytes + 1, expected, sizeof(expected)) == 0 &&
            memory.bytes[0] == 0xcc && memory.bytes[17] == 0xcc,
            "GET 写出完整 16 字节且零填充，不覆盖缓冲区外字节");

    memset(memory.bytes, 'x', sizeof(memory.bytes));
    memory.reads = 0;
    CHECK(invoke(&memory, &fault, 15, USER_BASE + 49) == 0 &&
            strcmp(current->comm, "xxxxxxxxxxxxxxx") == 0 && memory.reads == 15,
            "长名称截断到 15 字节，无需读取第 16 字节");
    memory.bytes[63] = 0;
    memory.reads = 0;
    CHECK(invoke(&memory, &fault, 15, USER_BASE + 63) == 0 &&
            current->comm[0] == 0 && memory.reads == 1,
            "映射末尾的空名称有效，遇到 NUL 停止读取");
    memory.bytes[62] = 'u';
    CHECK(invoke(&memory, &fault, 15, USER_BASE + 62) == 0 &&
            strcmp(current->comm, "u") == 0,
            "映射末尾的短名称无需完整 16 字节可读区间");
    return true;
}

static bool test_faults(void) {
    struct user_memory memory = {0};
    struct guest_linux_user_fault fault;
    strcpy(current->comm, "unchanged");
    memset(memory.bytes, 'x', sizeof(memory.bytes));
    CHECK(invoke(&memory, &fault, 15, USER_BASE + 60) == (qword_t) (sqword_t) _EFAULT &&
            strcmp(current->comm, "unchanged") == 0,
            "读取中途失败时名称保持原样");
    const qword_t invalid[] = {0, USER_BASE + 64, UINT64_MAX,
            AARCH64_LINUX_USER_ADDRESS_MAX + 1};
    for (unsigned index = 0; index < array_size(invalid); index++) {
        for (unsigned option = 15; option <= 16; option++) {
            CHECK(invoke(&memory, &fault, option, invalid[index]) ==
                        (qword_t) (sqword_t) _EFAULT &&
                    fault.address == invalid[index] &&
                    fault.access == (option == 15 ? GUEST_MEMORY_READ : GUEST_MEMORY_WRITE),
                    "空指针、未映射地址、地址溢出均返回 EFAULT");
        }
    }
    CHECK(invoke(&memory, &fault, 16, USER_BASE + 49) == (qword_t) (sqword_t) _EFAULT,
            "GET 要求完整 16 字节可写区间");
    CHECK(invoke(&memory, &fault, 16, AARCH64_LINUX_USER_ADDRESS_MAX - 7) ==
                (qword_t) (sqword_t) _EFAULT && fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "GET 在进入用户内存回调前拒绝越过地址空间边界的写入");
    unsigned reads = memory.reads, writes = memory.writes;
    CHECK(invoke(&memory, &fault, UINT32_MAX, USER_BASE) == (qword_t) (sqword_t) _EINVAL &&
            memory.reads == reads && memory.writes == writes &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "未实现的选项返回 EINVAL，不伪造成功或访问用户指针");
    CHECK(strcmp(current->comm, "unchanged") == 0, "失败调用不改名");
    return true;
}

int main(void) {
    struct task task = {0};
    lock_init(&task.general_lock);
    current = &task;
    bool success = test_names() && test_faults();
    current = NULL;
    lock_destroy(&task.general_lock);
    return success ? 0 : 1;
}

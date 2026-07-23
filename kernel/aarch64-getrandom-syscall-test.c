#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 8192
#define GETRANDOM_SYSCALL 278
#define RANDOM_CHUNK_SIZE 4096

#define GRND_NONBLOCK UINT32_C(0x0001)
#define GRND_RANDOM UINT32_C(0x0002)
#define GRND_INSECURE UINT32_C(0x0004)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 getrandom 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct random_source_probe {
    unsigned calls;
    unsigned fail_call;
    bool write_pattern;
    dword_t first_sizes[4];
    dword_t last_size;
};

static struct random_source_probe random_source;

// 测试目标单独编译 syscall service，以可控源覆盖成功、短路与失败边界。
int ish_aarch64_getrandom_test_source(char *buffer, size_t size) {
    unsigned call = random_source.calls++;
    if (call < array_size(random_source.first_sizes))
        random_source.first_sizes[call] = (dword_t) size;
    random_source.last_size = (dword_t) size;
    if (random_source.fail_call != 0 &&
            call + 1 == random_source.fail_call)
        return 1;
    if (random_source.write_pattern)
        memset(buffer, (byte_t) (0x41 + call % 31), size);
    return 0;
}

static void reset_random_source(void) {
    memset(&random_source, 0, sizeof(random_source));
    random_source.write_pattern = true;
}

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    unsigned reads;
    unsigned writes;
    unsigned fail_write_call;
    dword_t partial_fail_size;
    bool accept_all_writes;
    qword_t first_write_addresses[4];
    dword_t first_write_sizes[4];
    qword_t last_write_address;
    dword_t last_write_size;
};

static bool probe_range(
        qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE ||
            address - USER_BASE > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - (address - USER_BASE))
        return false;
    *offset = (size_t) (address - USER_BASE);
    return true;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) destination;
    (void) size;
    (void) fault;
    struct user_probe *probe = opaque;
    probe->reads++;
    return false;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    unsigned call = probe->writes++;
    if (call < array_size(probe->first_write_addresses)) {
        probe->first_write_addresses[call] = address;
        probe->first_write_sizes[call] = size;
    }
    probe->last_write_address = address;
    probe->last_write_size = size;

    if (probe->fail_write_call != 0 &&
            call + 1 == probe->fail_write_call) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->accept_all_writes) {
        *fault = (struct guest_linux_user_fault) {0};
        return true;
    }

    size_t offset;
    if (!probe_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->partial_fail_size != 0) {
        dword_t copied = probe->partial_fail_size < size ?
                probe->partial_fail_size : size;
        memcpy(probe->bytes + offset, source, copied);
        *fault = (struct guest_linux_user_fault) {
            .address = address + copied,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    memcpy(probe->bytes + offset, source, size);
    *fault = (struct guest_linux_user_fault) {0};
    return true;
}

static void reset_probe(struct user_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    memset(probe->bytes, 0xa5, sizeof(probe->bytes));
}

static bool bytes_equal(const byte_t *bytes,
        size_t size, byte_t expected) {
    for (size_t index = 0; index < size; index++) {
        if (bytes[index] != expected)
            return false;
    }
    return true;
}

static qword_t invoke(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault,
        qword_t address, qword_t count, qword_t flags) {
    *fault = (struct guest_linux_user_fault) {0};
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = GETRANDOM_SYSCALL,
        .arguments = {address, count, flags},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static int test_random_bytes_and_flags(struct task *task) {
    static const dword_t valid_flags[] = {
        0,
        GRND_NONBLOCK,
        GRND_RANDOM,
        GRND_INSECURE,
        GRND_NONBLOCK | GRND_RANDOM,
        GRND_NONBLOCK | GRND_INSECURE,
    };
    for (size_t index = 0; index < array_size(valid_flags); index++) {
        struct user_probe probe;
        struct guest_linux_user_fault fault;
        reset_probe(&probe);
        reset_random_source();
        qword_t flags = UINT64_C(0xa5a5a5a500000000) |
                valid_flags[index];
        qword_t result = invoke(task, &probe, &fault,
                USER_BASE + 32, 64, flags);
        CHECK(result == 64 && probe.reads == 0 && probe.writes == 1 &&
                probe.first_write_addresses[0] == USER_BASE + 32 &&
                probe.first_write_sizes[0] == 64 &&
                random_source.calls == 1 &&
                random_source.first_sizes[0] == 64 &&
                bytes_equal(probe.bytes + 32, 64, 0x41) &&
                fault.kind == GUEST_MEMORY_FAULT_NONE,
                "所有合法 flag 应返回完整的可控随机字节");
    }
    return 0;
}

static int test_invalid_flags_and_zero_length(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    reset_random_source();
    qword_t result = invoke(task, &probe, &fault,
            UINT64_MAX, 0, UINT32_C(0x8));
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 0 && probe.writes == 0 &&
            random_source.calls == 0,
            "未知 flag 即使长度为零也应返回 EINVAL");

    reset_probe(&probe);
    reset_random_source();
    result = invoke(task, &probe, &fault,
            USER_BASE, 16, GRND_RANDOM | GRND_INSECURE);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.writes == 0 && random_source.calls == 0,
            "RANDOM 与 INSECURE 的矛盾组合应返回 EINVAL");

    reset_probe(&probe);
    reset_random_source();
    result = invoke(task, &probe, &fault, UINT64_MAX, 0, 0);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 0 &&
            random_source.calls == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "零长度请求不得检查或访问输出地址");
    return 0;
}

static int test_chunking_and_limit(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    reset_random_source();
    qword_t result = invoke(task, &probe, &fault,
            USER_BASE, RANDOM_CHUNK_SIZE + 37, 0);
    CHECK(result == RANDOM_CHUNK_SIZE + 37 && probe.writes == 2 &&
            probe.first_write_addresses[0] == USER_BASE &&
            probe.first_write_sizes[0] == RANDOM_CHUNK_SIZE &&
            probe.first_write_addresses[1] ==
                    USER_BASE + RANDOM_CHUNK_SIZE &&
            probe.first_write_sizes[1] == 37 &&
            random_source.calls == 2 &&
            bytes_equal(probe.bytes, RANDOM_CHUNK_SIZE, 0x41) &&
            bytes_equal(probe.bytes + RANDOM_CHUNK_SIZE, 37, 0x42) &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "大请求应按有界缓冲分块并返回完整字节数");

    reset_probe(&probe);
    probe.accept_all_writes = true;
    reset_random_source();
    random_source.write_pattern = false;
    result = invoke(task, &probe, &fault,
            USER_BASE, UINT64_MAX, GRND_NONBLOCK);
    const qword_t expected_calls =
            ((qword_t) INT_MAX + RANDOM_CHUNK_SIZE - 1) /
            RANDOM_CHUNK_SIZE;
    CHECK(result == INT_MAX && probe.writes == expected_calls &&
            random_source.calls == expected_calls &&
            probe.last_write_address == USER_BASE +
                    (expected_calls - 1) * RANDOM_CHUNK_SIZE &&
            probe.last_write_size ==
                    (dword_t) (INT_MAX % RANDOM_CHUNK_SIZE) &&
            random_source.last_size ==
                    (dword_t) (INT_MAX % RANDOM_CHUNK_SIZE),
            "超大请求应精确截到 INT_MAX 并保留末块长度");
    return 0;
}

static int test_random_source_failures(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    reset_random_source();
    random_source.fail_call = 1;
    qword_t result = invoke(task, &probe, &fault,
            USER_BASE, 32, 0);
    CHECK(result == (qword_t) (sqword_t) _EIO &&
            random_source.calls == 1 && probe.writes == 0 &&
            bytes_equal(probe.bytes, sizeof(probe.bytes), 0xa5),
            "首块随机源失败应返回 EIO 且不写用户内存");

    reset_probe(&probe);
    reset_random_source();
    random_source.fail_call = 2;
    result = invoke(task, &probe, &fault,
            USER_BASE, RANDOM_CHUNK_SIZE + 37, 0);
    CHECK(result == (qword_t) (sqword_t) _EIO &&
            random_source.calls == 2 && probe.writes == 1 &&
            bytes_equal(probe.bytes, RANDOM_CHUNK_SIZE, 0x41) &&
            bytes_equal(probe.bytes + RANDOM_CHUNK_SIZE, 37, 0xa5),
            "后续随机源失败应返回 EIO 并保留此前已写内存");
    return 0;
}

static int test_output_faults(struct task *task) {
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    reset_random_source();
    qword_t address = UINT64_MAX - 7;
    qword_t result = invoke(task, &probe, &fault, address, 16, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 0 && random_source.calls == 0 &&
            fault.address == address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "越过用户地址边界的首块应在随机源调用前返回 EFAULT");

    reset_probe(&probe);
    reset_random_source();
    probe.fail_write_call = 1;
    result = invoke(task, &probe, &fault, USER_BASE, 32, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            random_source.calls == 1 && probe.writes == 1 &&
            fault.address == USER_BASE &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "首个用户写回故障应返回 EFAULT 与精确访存信息");

    reset_probe(&probe);
    reset_random_source();
    probe.fail_write_call = 2;
    result = invoke(task, &probe, &fault,
            USER_BASE, RANDOM_CHUNK_SIZE + 37, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            random_source.calls == 2 && probe.writes == 2 &&
            bytes_equal(probe.bytes, RANDOM_CHUNK_SIZE, 0x41) &&
            bytes_equal(probe.bytes + RANDOM_CHUNK_SIZE, 37, 0xa5) &&
            fault.address == USER_BASE + RANDOM_CHUNK_SIZE,
            "后续写故障应返回 EFAULT 并保留此前已写内存");

    reset_probe(&probe);
    reset_random_source();
    probe.partial_fail_size = 19;
    result = invoke(task, &probe, &fault, USER_BASE, 64, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            random_source.calls == 1 && probe.writes == 1 &&
            bytes_equal(probe.bytes, 19, 0x41) &&
            bytes_equal(probe.bytes + 19, 45, 0xa5) &&
            fault.address == USER_BASE + 19,
            "单次用户回调部分写入后仍应返回 EFAULT");

    reset_probe(&probe);
    probe.accept_all_writes = true;
    reset_random_source();
    random_source.write_pattern = false;
    address = AARCH64_LINUX_USER_ADDRESS_MAX -
            (RANDOM_CHUNK_SIZE - 1);
    result = invoke(task, &probe, &fault,
            address, RANDOM_CHUNK_SIZE + 37, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            random_source.calls == 1 && probe.writes == 1 &&
            fault.address == AARCH64_LINUX_USER_ADDRESS_MAX + 1 &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "后续块越过 AArch64 用户上限时应返回 EFAULT");
    return 0;
}

int main(void) {
    struct task task = {0};
    current = &task;
    int result = test_random_bytes_and_flags(&task);
    if (result == 0)
        result = test_invalid_flags_and_zero_length(&task);
    if (result == 0)
        result = test_chunking_and_limit(&task);
    if (result == 0)
        result = test_random_source_failures(&task);
    if (result == 0)
        result = test_output_faults(&task);
    current = NULL;
    return result;
}

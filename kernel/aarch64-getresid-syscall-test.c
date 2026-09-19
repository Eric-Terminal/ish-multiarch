#include <stdio.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 64

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 凭据读取测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_write_at;
    unsigned writes;
    bool change_credentials;
};

static const qword_t output_addresses[] = {
    USER_BASE + 3, USER_BASE + 19, USER_BASE + 43,
};

static const struct {
    qword_t number;
    dword_t ids[3];
} identity_calls[] = {
    {148, {UINT32_C(0x89abcdef), UINT32_C(0xa1b2c3d4), UINT32_MAX}},
    {150, {UINT32_C(0x76543210), UINT32_C(0x10203040), UINT32_C(0xfedcba98)}},
};

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    if (address < USER_BASE || address - USER_BASE > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - (address - USER_BASE) ||
            address == probe->fail_write_at) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    memcpy(probe->bytes + (address - USER_BASE), source, size);
    if (probe->change_credentials && probe->writes == 1) {
        // 在第一次写回后改变凭据，验证后续值仍来自同一次快照；同时确保写 guest 内存时已释放锁。
        lock(&pids_lock);
        current->uid = current->euid = current->suid = 7;
        current->gid = current->egid = current->sgid = 8;
        unlock(&pids_lock);
    }
    return true;
}

static void reset_fixture(struct task *task, struct user_probe *probe) {
    *task = (struct task) {
        .uid = identity_calls[0].ids[0],
        .euid = identity_calls[0].ids[1],
        .suid = identity_calls[0].ids[2],
        .gid = identity_calls[1].ids[0],
        .egid = identity_calls[1].ids[1],
        .sgid = identity_calls[1].ids[2],
    };
    *probe = (struct user_probe) {.fail_write_at = UINT64_MAX};
    memset(probe->bytes, 0xcc, sizeof(probe->bytes));
    current = task;
}

static qword_t invoke(struct task *task, struct user_probe *probe,
        struct guest_linux_user_fault *fault, qword_t number,
        const qword_t addresses[3]) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {.opaque = probe, .write = write_user},
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {addresses[0], addresses[1], addresses[2]},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static bool check_outputs(const struct user_probe *probe,
        const dword_t ids[3], unsigned count) {
    byte_t expected[USER_MEMORY_SIZE];
    memset(expected, 0xcc, sizeof(expected));
    for (unsigned i = 0; i < count; i++)
        memcpy(expected + (output_addresses[i] - USER_BASE),
                &ids[i], sizeof(ids[i]));
    CHECK(memcmp(probe->bytes, expected, sizeof(expected)) == 0,
            "仅写入已完成的 32 位 ID，并保留其他字节");
    return true;
}

static bool test_success(void) {
    struct task task;
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    for (unsigned call = 0; call < array_size(identity_calls); call++) {
        reset_fixture(&task, &probe);
        qword_t result = invoke(&task, &probe, &fault,
                identity_calls[call].number, output_addresses);
        CHECK(result == 0 && probe.writes == 3 &&
                fault.kind == GUEST_MEMORY_FAULT_NONE,
                "非 root 可通过高地址、非对齐指针读取真实、有效和保存 ID");
        CHECK(check_outputs(&probe, identity_calls[call].ids, 3),
                "保留每个 ID 的全部 32 位");

        reset_fixture(&task, &probe);
        task.uid = task.euid = task.suid = 0;
        task.gid = task.egid = task.sgid = 0;
        result = invoke(&task, &probe, &fault,
                identity_calls[call].number, output_addresses);
        const dword_t root_ids[] = {0, 0, 0};
        CHECK(result == 0 && check_outputs(&probe, root_ids, 3),
                "root 凭据正确返回零值");

        reset_fixture(&task, &probe);
        const qword_t aliases[] = {
            output_addresses[0], output_addresses[0], output_addresses[0],
        };
        result = invoke(&task, &probe, &fault,
                identity_calls[call].number, aliases);
        const dword_t aliased_ids[] = {identity_calls[call].ids[2], 0, 0};
        CHECK(result == 0 && probe.writes == 3 &&
                check_outputs(&probe, aliased_ids, 1),
                "重叠指针按真实、有效、保存 ID 的顺序覆盖");

        reset_fixture(&task, &probe);
        probe.change_credentials = true;
        result = invoke(&task, &probe, &fault,
                identity_calls[call].number, output_addresses);
        CHECK(result == 0 && check_outputs(&probe, identity_calls[call].ids, 3),
                "写回期间凭据变化不混合不同快照的 ID");
    }
    return true;
}

static bool test_faults(void) {
    const qword_t invalid_addresses[] = {
        0,
        USER_BASE + USER_MEMORY_SIZE,
        AARCH64_LINUX_USER_ADDRESS_MAX - 1,
        UINT64_MAX,
    };
    struct task task;
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    for (unsigned call = 0; call < array_size(identity_calls); call++) {
        for (unsigned failed = 0; failed < 3; failed++) {
            for (unsigned invalid = 0;
                    invalid < array_size(invalid_addresses); invalid++) {
                reset_fixture(&task, &probe);
                qword_t addresses[3];
                memcpy(addresses, output_addresses, sizeof(addresses));
                addresses[failed] = invalid_addresses[invalid];
                qword_t result = invoke(&task, &probe, &fault,
                        identity_calls[call].number, addresses);
                CHECK(result == (qword_t) (sqword_t) _EFAULT &&
                        fault.address == addresses[failed] &&
                        fault.access == GUEST_MEMORY_WRITE,
                        "空指针、未映射地址、越界或溢出地址返回 EFAULT");
                bool range_error = invalid >= 2;
                CHECK(fault.kind == (range_error ?
                            GUEST_MEMORY_FAULT_ADDRESS_SIZE :
                            GUEST_MEMORY_FAULT_UNMAPPED) &&
                        probe.writes == failed + (range_error ? 0 : 1),
                        "失败后立即停止，地址越界不进入用户内存回调");
                CHECK(check_outputs(&probe, identity_calls[call].ids, failed),
                        "后续指针无效时保留此前成功写回的 ID");
            }

            reset_fixture(&task, &probe);
            probe.fail_write_at = output_addresses[failed];
            qword_t result = invoke(&task, &probe, &fault,
                    identity_calls[call].number, output_addresses);
            CHECK(result == (qword_t) (sqword_t) _EFAULT &&
                    probe.writes == failed + 1 &&
                    fault.address == output_addresses[failed] &&
                    check_outputs(&probe, identity_calls[call].ids, failed),
                    "用户内存拒绝写入时返回 EFAULT 并停止后续写回");
        }
    }
    return true;
}

int main(void) {
    if (!test_success() || !test_faults())
        return 1;
    current = NULL;
    return 0;
}

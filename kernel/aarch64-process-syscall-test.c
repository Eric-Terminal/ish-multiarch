#include <stdio.h>

#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 进程系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct identity_fixture {
    struct task task;
    struct task leader;
    struct task parent;
    struct task decoy_parent;
    struct tgroup group;
};

struct user_probe {
    unsigned reads;
    unsigned writes;
};

static bool reject_user_read(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->reads++;
    (void) address;
    (void) destination;
    (void) size;
    (void) fault;
    return false;
}

static bool reject_user_write(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    (void) address;
    (void) source;
    (void) size;
    (void) fault;
    return false;
}

static qword_t invoke(struct identity_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t number, qword_t argument0, qword_t argument1,
        qword_t argument2, qword_t argument3, qword_t argument4,
        qword_t argument5) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = reject_user_read,
            .write = reject_user_write,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {
            argument0, argument1, argument2,
            argument3, argument4, argument5,
        },
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static void init_fixture(struct identity_fixture *fixture) {
    *fixture = (struct identity_fixture) {0};
    fixture->task.group = &fixture->group;
    fixture->task.pid = 12345;
    fixture->task.tgid = 12000;
    fixture->task.uid = UINT32_C(0x89abcdef);
    fixture->task.euid = UINT32_C(0xa1b2c3d4);
    fixture->task.gid = UINT32_C(0x76543210);
    fixture->task.egid = UINT32_C(0x10203040);

    fixture->leader.pid = 12000;
    fixture->leader.tgid = 12000;
    fixture->leader.parent = &fixture->parent;
    fixture->group.leader = &fixture->leader;
    fixture->parent.pid = 5432;
    fixture->parent.tgid = 5000;

    // task 自身的 parent 是诱饵，防止实现绕过线程组 leader。
    fixture->decoy_parent.tgid = 9999;
    fixture->task.parent = &fixture->decoy_parent;
    current = &fixture->task;
}

int main(void) {
    struct identity_fixture fixture;
    struct user_probe probe = {0};
    struct guest_linux_user_fault fault;
    init_fixture(&fixture);

    static const struct {
        qword_t number;
        qword_t expected;
    } identity_calls[] = {
        {172, 12000},
        {173, 5000},
        {174, UINT32_C(0x89abcdef)},
        {175, UINT32_C(0xa1b2c3d4)},
        {176, UINT32_C(0x76543210)},
        {177, UINT32_C(0x10203040)},
    };
    for (size_t i = 0; i < array_size(identity_calls); i++) {
        fault = (struct guest_linux_user_fault) {
            .address = UINT64_MAX,
            .access = UINT32_MAX,
            .kind = UINT32_MAX,
        };
        qword_t result = invoke(&fixture, &probe, &fault,
                identity_calls[i].number,
                UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 2,
                UINT64_MAX - 3, UINT64_MAX - 4, UINT64_MAX - 5);
        CHECK(result == identity_calls[i].expected &&
                fault.address == 0 && fault.access == 0 && fault.kind == 0,
                "身份调用返回目标 task 的完整宽度字段并清零故障");
    }
    CHECK(probe.reads == 0 && probe.writes == 0,
            "身份调用不访问 guest 用户内存");
    CHECK(task_gettid(&fixture.task) == 12345,
            "显式 task 的 gettid 保留线程 PID");

    qword_t result = invoke(&fixture, &probe, &fault, 178,
            UINT64_MAX, UINT64_MAX, UINT64_MAX,
            UINT64_MAX, UINT64_MAX, UINT64_MAX);
    CHECK(result == (qword_t) (sqword_t) _ENOSYS,
            "gettid 仍由 AArch64 runtime 负责");

    lock(&pids_lock);
    fixture.leader.parent = NULL;
    unlock(&pids_lock);
    result = invoke(&fixture, &probe, &fault, 173,
            UINT64_MAX, UINT64_MAX, UINT64_MAX,
            UINT64_MAX, UINT64_MAX, UINT64_MAX);
    CHECK(result == 0, "无父进程的 getppid 返回零");

    current = NULL;
    return 0;
}

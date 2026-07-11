#include <stdio.h>
#include <string.h>

#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 512

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
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_write_at;
    unsigned reads;
    unsigned writes;
    qword_t last_write_address;
    dword_t last_write_size;
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

static bool probe_user_write(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->writes++;
    probe->last_write_address = address;
    probe->last_write_size = size;

    if (address < USER_BASE) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    qword_t offset = address - USER_BASE;
    if (offset > sizeof(probe->bytes) ||
            size > sizeof(probe->bytes) - offset) {
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
        memcpy(&probe->bytes[offset], source, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_write_at,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    memcpy(&probe->bytes[offset], source, size);
    return true;
}

static void reset_user_probe(struct user_probe *probe, byte_t fill) {
    memset(probe->bytes, fill, sizeof(probe->bytes));
    probe->fail_write_at = UINT64_MAX;
    probe->reads = 0;
    probe->writes = 0;
    probe->last_write_address = 0;
    probe->last_write_size = 0;
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
            .write = probe_user_write,
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
    struct user_probe probe;
    struct guest_linux_user_fault fault;
    init_fixture(&fixture);
    reset_user_probe(&probe, 0);

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

    extern const char *uname_version;
    extern const char *uname_hostname_override;
    const char *saved_version = uname_version;
    const char *saved_hostname = uname_hostname_override;
    char long_hostname[UNAME_LENGTH + 17];
    memset(long_hostname, 'h', sizeof(long_hostname) - 1);
    long_hostname[sizeof(long_hostname) - 1] = '\0';
    uname_version = "test-version";
    uname_hostname_override = long_hostname;

    qword_t uts_address = USER_BASE + 1;
    reset_user_probe(&probe, 0x5a);
    result = invoke(&fixture, &probe, &fault, 160,
            uts_address, UINT64_MAX, UINT64_MAX - 1,
            UINT64_MAX - 2, UINT64_MAX - 3, UINT64_MAX - 4);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
            probe.last_write_address == uts_address &&
            probe.last_write_size == sizeof(struct uname),
            "uname 向未对齐地址单次写出完整 new_utsname");
    CHECK(probe.bytes[0] == 0x5a &&
            probe.bytes[1 + sizeof(struct uname)] == 0x5a,
            "uname 不修改 wire 结构前后的哨兵");

    struct uname uts;
    memcpy(&uts, &probe.bytes[1], sizeof(uts));
    CHECK(strcmp(uts.system, "Linux") == 0 &&
            strcmp(uts.release, "4.20.69-ish") == 0 &&
            strncmp(uts.version, "test-version ",
                    strlen("test-version ")) == 0 &&
            strcmp(uts.arch, "aarch64") == 0 &&
            strcmp(uts.domain, "(none)") == 0,
            "uname 保持 Linux 字段并报告 AArch64 guest machine");
    CHECK(strncmp(uts.hostname, long_hostname, UNAME_LENGTH - 1) == 0 &&
            uts.hostname[UNAME_LENGTH - 1] == '\0' &&
            uts.release[0] == '4',
            "超长 hostname 在字段边界截断且不污染 release");

    reset_user_probe(&probe, 0xcc);
    probe.fail_write_at = uts_address + 200;
    result = invoke(&fixture, &probe, &fault, 160,
            uts_address, 1, 2, 3, 4, 5);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 && probe.writes == 1 &&
            fault.address == probe.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "uname 保留用户写回回调给出的精确故障");
    CHECK(probe.bytes[1] == 'L' &&
            probe.bytes[1 + 200] == 0xcc,
            "uname 写回失败时保留已复制前缀且不越过故障点");

    reset_user_probe(&probe, 0x33);
    qword_t wrapping_address =
            UINT64_MAX - (qword_t) sizeof(struct uname) + 2;
    result = invoke(&fixture, &probe, &fault, 160,
            wrapping_address, 1, 2, 3, 4, 5);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.writes == 0 && fault.address == wrapping_address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "uname 在回调前拒绝 64 位地址回绕");

    struct uname i386_uts;
    do_uname(&i386_uts, "i686");
    CHECK(strcmp(i386_uts.arch, "i686") == 0,
            "官方 i386 uname 继续报告 i686");
    uname_version = saved_version;
    uname_hostname_override = saved_hostname;

    current = NULL;
    return 0;
}

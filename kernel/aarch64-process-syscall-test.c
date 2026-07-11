#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest/aarch64/linux-signal-abi.h"
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

struct process_fixture {
    struct task task;
    struct task leader;
    struct task parent;
    struct task decoy_parent;
    struct tgroup group;
    struct sighand sighand;
};

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned reads;
    unsigned writes;
    qword_t last_read_address;
    dword_t last_read_size;
    qword_t last_write_address;
    dword_t last_write_size;
};

static bool probe_user_read(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    probe->reads++;
    probe->last_read_address = address;
    probe->last_read_size = size;

    if (address < USER_BASE) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    qword_t offset = address - USER_BASE;
    if (offset > sizeof(probe->bytes) ||
            size > sizeof(probe->bytes) - offset) {
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
        memcpy(destination, &probe->bytes[offset], prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = probe->fail_read_at,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }

    memcpy(destination, &probe->bytes[offset], size);
    return true;
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
    probe->fail_read_at = UINT64_MAX;
    probe->fail_write_at = UINT64_MAX;
    probe->reads = 0;
    probe->writes = 0;
    probe->last_read_address = 0;
    probe->last_read_size = 0;
    probe->last_write_address = 0;
    probe->last_write_size = 0;
}

static void store_user_sigset(struct user_probe *probe,
        qword_t address, sigset_t_ set) {
    assert(address >= USER_BASE &&
            address - USER_BASE <= sizeof(probe->bytes) - sizeof(set));
    memcpy(&probe->bytes[address - USER_BASE], &set, sizeof(set));
}

static sigset_t_ load_user_sigset(
        const struct user_probe *probe, qword_t address) {
    assert(address >= USER_BASE &&
            address - USER_BASE <= sizeof(probe->bytes) - sizeof(sigset_t_));
    sigset_t_ set;
    memcpy(&set, &probe->bytes[address - USER_BASE], sizeof(set));
    return set;
}

static void store_user_sigaction(struct user_probe *probe,
        qword_t address, struct aarch64_linux_sigaction action) {
    assert(address >= USER_BASE &&
            address - USER_BASE <= sizeof(probe->bytes) - sizeof(action));
    memcpy(&probe->bytes[address - USER_BASE], &action, sizeof(action));
}

static struct aarch64_linux_sigaction load_user_sigaction(
        const struct user_probe *probe, qword_t address) {
    assert(address >= USER_BASE && address - USER_BASE <=
            sizeof(probe->bytes) - sizeof(struct aarch64_linux_sigaction));
    struct aarch64_linux_sigaction action;
    memcpy(&action, &probe->bytes[address - USER_BASE], sizeof(action));
    return action;
}

static struct sigqueue *queue_signal(struct task *task, int signal) {
    struct sigqueue *queued = malloc(sizeof(*queued));
    assert(queued != NULL);
    *queued = (struct sigqueue) {
        .info.sig = signal,
    };
    list_add_tail(&task->queue, &queued->queue);
    sigset_add(&task->pending, signal);
    return queued;
}

static void remove_queued_signal(struct task *task,
        struct sigqueue *queued) {
    sigset_del(&task->pending, queued->info.sig);
    list_remove(&queued->queue);
    free(queued);
}

static qword_t invoke(struct process_fixture *fixture,
        struct user_probe *probe, struct guest_linux_user_fault *fault,
        qword_t number, qword_t argument0, qword_t argument1,
        qword_t argument2, qword_t argument3, qword_t argument4,
        qword_t argument5) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = probe,
            .read = probe_user_read,
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

static void init_fixture(struct process_fixture *fixture) {
    *fixture = (struct process_fixture) {0};
    list_init(&fixture->group.threads);
    list_init(&fixture->task.queue);
    list_init(&fixture->leader.queue);
    fixture->task.group = &fixture->group;
    fixture->task.pid = 12345;
    fixture->task.tgid = 12000;
    fixture->task.uid = UINT32_C(0x89abcdef);
    fixture->task.euid = UINT32_C(0xa1b2c3d4);
    fixture->task.gid = UINT32_C(0x76543210);
    fixture->task.egid = UINT32_C(0x10203040);
    lock_init(&fixture->sighand.lock);
    fixture->task.sighand = &fixture->sighand;

    fixture->leader.pid = 12000;
    fixture->leader.tgid = 12000;
    fixture->leader.group = &fixture->group;
    fixture->leader.sighand = &fixture->sighand;
    fixture->leader.parent = &fixture->parent;
    fixture->group.leader = &fixture->leader;
    list_add_tail(&fixture->group.threads, &fixture->leader.group_links);
    list_add_tail(&fixture->group.threads, &fixture->task.group_links);
    fixture->parent.pid = 5432;
    fixture->parent.tgid = 5000;

    // task 自身的 parent 是诱饵，防止实现绕过线程组 leader。
    fixture->decoy_parent.tgid = 9999;
    fixture->task.parent = &fixture->decoy_parent;
    current = &fixture->task;
}

int main(void) {
    struct process_fixture fixture;
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

    const qword_t set_address = USER_BASE + 3;
    const qword_t oldset_address = USER_BASE + 29;
    const sigset_t_ high_signal_bit = sig_mask(NUM_SIGS);
    const sigset_t_ initial_mask = sig_mask(SIGUSR1_) | high_signal_bit;
    CHECK(sig_mask(63) == (UINT64_C(1) << 62) &&
            sig_mask(64) == (UINT64_C(1) << 63),
            "信号位计算在 ILP32 宿主仍保持 64 位");

    struct sighand explicit_sighand = {0};
    lock_init(&explicit_sighand.lock);
    struct task explicit_task = {
        .sighand = &explicit_sighand,
        .blocked = sig_mask(SIGUSR1_),
    };
    sigset_t_ explicit_set = high_signal_bit | sig_mask(SIGUSR2_);
    sigset_t_ explicit_old;
    CHECK(task_sigprocmask(&explicit_task, SIG_SETMASK_,
            &explicit_set, &explicit_old) == 0 &&
            explicit_old == sig_mask(SIGUSR1_) &&
            explicit_task.blocked == explicit_set &&
            fixture.task.blocked == 0,
            "掩码 helper 只修改显式目标 task");

    const struct signal_action full_action = {
        .handler = UINT64_C(0x0000400012345678),
        .flags = UINT64_C(0xfedcba9876543210),
        .restorer = UINT64_C(0x0000400087654321),
        .mask = high_signal_bit |
                sig_mask(SIGKILL_) | sig_mask(SIGSTOP_),
    };
    struct signal_action old_action;
    CHECK(task_sigaction(&explicit_task, NUM_SIGS,
            &full_action, &old_action) == 0 &&
            old_action.handler == SIG_DFL_,
            "显式动作 helper 接受 Linux 信号 64");
    struct signal_action queried_action;
    CHECK(task_sigaction(&explicit_task, NUM_SIGS,
            NULL, &queried_action) == 0 &&
            queried_action.handler == full_action.handler &&
            queried_action.flags == full_action.flags &&
            queried_action.restorer == full_action.restorer &&
            queried_action.mask == high_signal_bit,
            "内部动作完整保存 64 位字段并清除不可阻塞掩码");
    CHECK(task_sigaction(&explicit_task, SIGKILL_,
            NULL, &old_action) == 0 &&
            task_sigaction(&explicit_task, SIGSTOP_,
                    &full_action, NULL) == _EINVAL &&
            task_sigaction(&explicit_task, 0,
                    NULL, &old_action) == _EINVAL &&
            task_sigaction(&explicit_task, NUM_SIGS + 1,
                    NULL, &old_action) == _EINVAL,
            "动作边界允许查询不可捕获信号并拒绝非法安装");
    CHECK(fixture.sighand.action[NUM_SIGS].handler == SIG_DFL_,
            "动作 helper 不依赖 TLS current");

    explicit_sighand.altstack = UINT32_C(0x12345000);
    explicit_sighand.altstack_size = UINT32_C(0x8000);
    struct sighand *copied_sighand = sighand_copy(&explicit_sighand);
    CHECK(copied_sighand != NULL &&
            copied_sighand->action[NUM_SIGS].handler == full_action.handler &&
            copied_sighand->action[NUM_SIGS].flags == full_action.flags &&
            copied_sighand->action[NUM_SIGS].restorer == full_action.restorer &&
            copied_sighand->action[NUM_SIGS].mask == high_signal_bit &&
            copied_sighand->altstack == explicit_sighand.altstack &&
            copied_sighand->altstack_size == explicit_sighand.altstack_size,
            "sighand 在同一快照中复制完整动作与替代栈状态");
    sighand_release(copied_sighand);

    explicit_sighand.action[SIGUSR1_] = full_action;
    explicit_sighand.action[SIGUSR2_] = (struct signal_action) {
        .handler = SIG_IGN_,
        .flags = UINT64_MAX,
        .restorer = UINT64_MAX,
        .mask = UINT64_MAX,
    };
    task_signal_exec_reset(&explicit_task);
    CHECK(explicit_sighand.action[SIGUSR1_].handler == SIG_DFL_ &&
            explicit_sighand.action[SIGUSR1_].flags == 0 &&
            explicit_sighand.action[SIGUSR1_].restorer == 0 &&
            explicit_sighand.action[SIGUSR1_].mask == 0 &&
            explicit_sighand.action[SIGUSR2_].handler == SIG_IGN_ &&
            explicit_sighand.action[SIGUSR2_].flags == 0 &&
            explicit_sighand.action[SIGUSR2_].restorer == 0 &&
            explicit_sighand.action[SIGUSR2_].mask == 0 &&
            explicit_sighand.action[NUM_SIGS].handler == SIG_DFL_ &&
            explicit_sighand.altstack == 0 &&
            explicit_sighand.altstack_size == 0,
            "exec 重置只保留忽略处置并清除全部附属状态");

    const qword_t action_address = USER_BASE + 101;
    const qword_t oldaction_address = USER_BASE + 181;
    const struct aarch64_linux_sigaction wide_action = {
        .handler = UINT64_C(0x0000400012345678),
        .flags = UINT64_C(0x12345678dc000c0f),
        .restorer = UINT64_C(0xffff000087654321),
        .mask = high_signal_bit |
                sig_mask(SIGKILL_) | sig_mask(SIGSTOP_),
    };

    reset_user_probe(&probe, 0x33);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            0, UINT64_MAX, UINT64_MAX - 31,
            UINT64_C(0x100000008), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 0 && probe.writes == 0 &&
            fixture.sighand.action[NUM_SIGS].handler == SIG_DFL_,
            "rt_sigaction 按完整参数拒绝非 8 字节 sigset 且不访问指针");

    reset_user_probe(&probe, 0x44);
    store_user_sigaction(&probe, action_address, wide_action);
    probe.fail_read_at = action_address + 17;
    result = invoke(&fixture, &probe, &fault, 134,
            SIGKILL_, action_address, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == probe.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fixture.sighand.action[SIGKILL_].handler == SIG_DFL_,
            "动作输入故障优先于 SIGKILL 安装校验且不改变状态");

    qword_t wrapping_action_address =
            UINT64_MAX - sizeof(struct aarch64_linux_sigaction) + 2;
    reset_user_probe(&probe, 0);
    result = invoke(&fixture, &probe, &fault, 134,
            0, wrapping_action_address, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == wrapping_action_address &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "动作地址回绕在 signal 校验与用户回调前失败");

    reset_user_probe(&probe, 0xa5);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            UINT64_C(0xfeedface00000040),
            action_address, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    struct aarch64_linux_sigaction wire_oldaction =
            load_user_sigaction(&probe, oldaction_address);
    CHECK(result == 0 && probe.reads == 1 && probe.writes == 1 &&
            probe.last_read_address == action_address &&
            probe.last_read_size == sizeof(wide_action) &&
            probe.last_write_address == oldaction_address &&
            probe.last_write_size == sizeof(wire_oldaction) &&
            wire_oldaction.handler == SIG_DFL_ &&
            wire_oldaction.flags == 0 &&
            wire_oldaction.restorer == 0 && wire_oldaction.mask == 0,
            "rt_sigaction 以单次 32 字节读写安装信号 64 并返回旧动作");
    CHECK(fixture.sighand.action[NUM_SIGS].handler == wide_action.handler &&
            fixture.sighand.action[NUM_SIGS].flags ==
                    AARCH64_LINUX_SA_SUPPORTED_FLAGS &&
            fixture.sighand.action[NUM_SIGS].restorer ==
                    wide_action.restorer &&
            fixture.sighand.action[NUM_SIGS].mask == high_signal_bit,
            "AArch64 动作保留 64 位字段并过滤未知 flags 与不可阻塞掩码");
    size_t oldaction_offset = (size_t) (oldaction_address - USER_BASE);
    CHECK(probe.bytes[oldaction_offset - 1] == 0xa5 &&
            probe.bytes[oldaction_offset + sizeof(wire_oldaction)] == 0xa5,
            "32 字节旧动作写回不修改结构外哨兵");

    reset_user_probe(&probe, 0xcc);
    result = invoke(&fixture, &probe, &fault, 134,
            UINT64_C(0x100000040), 0, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    wire_oldaction = load_user_sigaction(&probe, oldaction_address);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1 &&
            wire_oldaction.handler == wide_action.handler &&
            wire_oldaction.flags == AARCH64_LINUX_SA_SUPPORTED_FLAGS &&
            wire_oldaction.restorer == wide_action.restorer &&
            wire_oldaction.mask == high_signal_bit,
            "查询忽略 signal 参数高 32 位并完整回读 AArch64 动作");

    fixture.sighand.action[63] = (struct signal_action) {
        .handler = UINT64_C(0x00004000aaaabbbb),
        .flags = UINT64_MAX,
        .restorer = UINT64_C(0x00004000ccccdddd),
        .mask = high_signal_bit,
    };
    reset_user_probe(&probe, 0x5a);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            63, action_address, action_address,
            sizeof(sigset_t_), 0, 0);
    wire_oldaction = load_user_sigaction(&probe, action_address);
    CHECK(result == 0 &&
            wire_oldaction.handler == UINT64_C(0x00004000aaaabbbb) &&
            wire_oldaction.flags == AARCH64_LINUX_SA_SUPPORTED_FLAGS &&
            wire_oldaction.restorer == UINT64_C(0x00004000ccccdddd) &&
            wire_oldaction.mask == high_signal_bit &&
            fixture.sighand.action[63].handler == wide_action.handler &&
            fixture.sighand.action[63].flags ==
                    AARCH64_LINUX_SA_SUPPORTED_FLAGS,
            "输入输出别名先读取新动作并过滤旧动作的内部 flags");

    fixture.sighand.action[62] = (struct signal_action) {
        .handler = UINT64_C(0x0000400011112222),
        .flags = AARCH64_LINUX_SA_RESTART,
        .restorer = UINT64_C(0x0000400033334444),
        .mask = sig_mask(SIGUSR1_),
    };
    reset_user_probe(&probe, 0x66);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            62, action_address, action_address + 8,
            sizeof(sigset_t_), 0, 0);
    wire_oldaction = load_user_sigaction(&probe, action_address + 8);
    CHECK(result == 0 &&
            wire_oldaction.handler == UINT64_C(0x0000400011112222) &&
            wire_oldaction.restorer == UINT64_C(0x0000400033334444) &&
            fixture.sighand.action[62].handler == wide_action.handler &&
            fixture.sighand.action[62].restorer == wide_action.restorer,
            "部分重叠的输出不会污染已读取的新动作");

    reset_user_probe(&probe, 0x77);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            0, action_address, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 1 && probe.writes == 0,
            "成功读取动作后拒绝 signal 0 且不写旧动作");
    reset_user_probe(&probe, 0x77);
    result = invoke(&fixture, &probe, &fault, 134,
            65, 0, oldaction_address, sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 0 && probe.writes == 0,
            "查询拒绝 signal 65 且不访问旧动作");
    result = invoke(&fixture, &probe, &fault, 134,
            UINT64_C(0xffffffffffffffff), 0, 0,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EINVAL,
            "signal 参数低 32 位按有符号整数解释");

    reset_user_probe(&probe, 0x88);
    result = invoke(&fixture, &probe, &fault, 134,
            SIGKILL_, 0, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1,
            "允许查询 SIGKILL 动作");
    reset_user_probe(&probe, 0x88);
    result = invoke(&fixture, &probe, &fault, 134,
            SIGSTOP_, 0, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && probe.reads == 0 && probe.writes == 1,
            "允许查询 SIGSTOP 动作");
    reset_user_probe(&probe, 0x88);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            SIGSTOP_, action_address, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            probe.reads == 1 && probe.writes == 0,
            "读取新动作后拒绝安装 SIGSTOP");

    qword_t wrapping_oldaction_address =
            UINT64_MAX - sizeof(struct aarch64_linux_sigaction) + 2;
    reset_user_probe(&probe, 0x99);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            61, action_address, wrapping_oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == wrapping_oldaction_address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            fixture.sighand.action[61].handler == wide_action.handler,
            "旧动作地址回绕不回滚已经安装的新动作");

    struct sigqueue *task_other = queue_signal(&fixture.task, SIGUSR2_);
    struct sigqueue *leader_other = queue_signal(&fixture.leader, SIGUSR2_);
    reset_user_probe(&probe, 0);
    store_user_sigaction(&probe, action_address, wide_action);
    result = invoke(&fixture, &probe, &fault, 134,
            SIGUSR2_, action_address, 0, sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && sigset_has(fixture.task.pending, SIGUSR2_) &&
            sigset_has(fixture.leader.pending, SIGUSR2_) &&
            list_size(&fixture.task.queue) == 1 &&
            list_size(&fixture.leader.queue) == 1,
            "安装用户 handler 不清理同组线程的 pending");

    queue_signal(&fixture.task, SIGTERM_);
    queue_signal(&fixture.leader, SIGTERM_);
    fixture.task.blocked |= sig_mask(SIGTERM_);
    fixture.leader.blocked |= sig_mask(SIGTERM_);
    const struct aarch64_linux_sigaction ignore_action = {
        .handler = SIG_IGN_,
        .flags = UINT64_MAX,
        .restorer = UINT64_C(0x00004000deadbeef),
        .mask = high_signal_bit,
    };
    reset_user_probe(&probe, 0xcc);
    store_user_sigaction(&probe, action_address, ignore_action);
    probe.fail_write_at = oldaction_address + 17;
    result = invoke(&fixture, &probe, &fault, 134,
            SIGTERM_, action_address, oldaction_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            fixture.sighand.action[SIGTERM_].handler == SIG_IGN_ &&
            !sigset_has(fixture.task.pending, SIGTERM_) &&
            !sigset_has(fixture.leader.pending, SIGTERM_) &&
            sigset_has(fixture.task.pending, SIGUSR2_) &&
            sigset_has(fixture.leader.pending, SIGUSR2_) &&
            sigset_has(fixture.task.blocked, SIGTERM_) &&
            sigset_has(fixture.leader.blocked, SIGTERM_) &&
            list_size(&fixture.task.queue) == 1 &&
            list_size(&fixture.leader.queue) == 1,
            "SIG_IGN 清理全线程组 pending 且旧动作故障不回滚副作用");
    CHECK(probe.reads == 1 && probe.writes == 1 &&
            fault.address == probe.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE &&
            probe.bytes[oldaction_offset] == 0 &&
            probe.bytes[oldaction_offset + 16] == 0 &&
            probe.bytes[oldaction_offset + 17] == 0xcc,
            "旧动作部分写故障保留已复制前缀与精确故障位置");
    remove_queued_signal(&fixture.task, task_other);
    remove_queued_signal(&fixture.leader, leader_other);

    queue_signal(&fixture.task, SIGCHLD_);
    queue_signal(&fixture.leader, SIGCHLD_);
    const struct aarch64_linux_sigaction default_action = {0};
    reset_user_probe(&probe, 0);
    store_user_sigaction(&probe, action_address, default_action);
    result = invoke(&fixture, &probe, &fault, 134,
            SIGCHLD_, action_address, 0, sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 &&
            !sigset_has(fixture.task.pending, SIGCHLD_) &&
            !sigset_has(fixture.leader.pending, SIGCHLD_) &&
            list_empty(&fixture.task.queue) &&
            list_empty(&fixture.leader.queue),
            "默认忽略的 SIGCHLD 安装 SIG_DFL 时清理全组 pending");

    struct sigqueue *task_sigio = queue_signal(&fixture.task, SIGIO_);
    struct sigqueue *leader_sigio = queue_signal(&fixture.leader, SIGIO_);
    reset_user_probe(&probe, 0);
    store_user_sigaction(&probe, action_address, default_action);
    result = invoke(&fixture, &probe, &fault, 134,
            SIGIO_, action_address, 0, sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && sigset_has(fixture.task.pending, SIGIO_) &&
            sigset_has(fixture.leader.pending, SIGIO_) &&
            list_size(&fixture.task.queue) == 1 &&
            list_size(&fixture.leader.queue) == 1,
            "默认终止的 SIGIO 安装 SIG_DFL 时保留 pending");
    remove_queued_signal(&fixture.task, task_sigio);
    remove_queued_signal(&fixture.leader, leader_sigio);

    fixture.task.blocked = initial_mask;
    reset_user_probe(&probe, 0);
    store_user_sigset(&probe, set_address, sig_mask(SIGUSR2_));
    result = invoke(&fixture, &probe, &fault, 135,
            SIG_BLOCK_, set_address, oldset_address,
            UINT64_C(0x100000008), UINT64_MAX, UINT64_MAX);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            fixture.task.blocked == initial_mask &&
            probe.reads == 0 && probe.writes == 0,
            "rt_sigprocmask 按完整参数拒绝非 8 字节 sigset");

    reset_user_probe(&probe, 0x77);
    result = invoke(&fixture, &probe, &fault, 135,
            UINT64_MAX, 0, oldset_address, sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && fixture.task.blocked == initial_mask &&
            probe.reads == 0 && probe.writes == 1 &&
            load_user_sigset(&probe, oldset_address) == initial_mask,
            "NULL set 忽略 how 并输出调用前的完整掩码");

    fixture.task.blocked = sig_mask(SIGUSR1_);
    sigset_t_ block_mask = sig_mask(SIGUSR2_) | sig_mask(SIGKILL_) |
            sig_mask(SIGSTOP_) | high_signal_bit;
    reset_user_probe(&probe, 0x55);
    store_user_sigset(&probe, set_address, block_mask);
    result = invoke(&fixture, &probe, &fault, 135,
            UINT64_C(0xfeedface00000000) | SIG_BLOCK_,
            set_address, set_address, sizeof(sigset_t_), 0, 0);
    sigset_t_ blocked_mask = sig_mask(SIGUSR1_) |
            sig_mask(SIGUSR2_) | high_signal_bit;
    CHECK(result == 0 && fixture.task.blocked == blocked_mask &&
            probe.reads == 1 && probe.writes == 1 &&
            load_user_sigset(&probe, set_address) == sig_mask(SIGUSR1_),
            "BLOCK 支持输入输出别名、高位 how 与 64 位 signal wire");
    CHECK(!sigset_has(fixture.task.blocked, SIGKILL_) &&
            !sigset_has(fixture.task.blocked, SIGSTOP_),
            "SIGKILL 与 SIGSTOP 永远不能被阻塞");

    reset_user_probe(&probe, 0);
    store_user_sigset(&probe, set_address,
            sig_mask(SIGUSR1_) | high_signal_bit);
    result = invoke(&fixture, &probe, &fault, 135,
            SIG_UNBLOCK_, set_address, 0, sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && fixture.task.blocked == sig_mask(SIGUSR2_),
            "UNBLOCK 只清除输入集合中的信号");

    reset_user_probe(&probe, 0);
    store_user_sigset(&probe, set_address,
            high_signal_bit | sig_mask(SIGKILL_) | sig_mask(SIGSTOP_));
    result = invoke(&fixture, &probe, &fault, 135,
            SIG_SETMASK_, set_address, 0, sizeof(sigset_t_), 0, 0);
    CHECK(result == 0 && fixture.task.blocked == high_signal_bit,
            "SETMASK 保留最高位并剔除不可阻塞信号");

    reset_user_probe(&probe, 0x44);
    store_user_sigset(&probe, set_address, sig_mask(SIGUSR1_));
    result = invoke(&fixture, &probe, &fault, 135,
            3, set_address, oldset_address, sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EINVAL &&
            fixture.task.blocked == high_signal_bit &&
            probe.reads == 1 && probe.writes == 0,
            "非法 how 在读取 set 后拒绝且不写 oldset");

    reset_user_probe(&probe, 0x22);
    store_user_sigset(&probe, set_address, sig_mask(SIGUSR1_));
    probe.fail_read_at = set_address + 4;
    result = invoke(&fixture, &probe, &fault, 135,
            3, set_address, oldset_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            fixture.task.blocked == high_signal_bit &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == probe.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "set 读取故障优先于 how、oldset 与状态变更");

    fixture.task.blocked = high_signal_bit | sig_mask(SIGUSR1_);
    reset_user_probe(&probe, 0xcc);
    store_user_sigset(&probe, set_address, sig_mask(SIGUSR1_));
    probe.fail_write_at = oldset_address + 4;
    result = invoke(&fixture, &probe, &fault, 135,
            SIG_UNBLOCK_, set_address, oldset_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            fixture.task.blocked == high_signal_bit &&
            probe.reads == 1 && probe.writes == 1 &&
            fault.address == probe.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "oldset 部分写故障不回滚已生效的新掩码");

    qword_t wrapping_sigset_address =
            UINT64_MAX - (qword_t) sizeof(sigset_t_) + 2;
    reset_user_probe(&probe, 0);
    result = invoke(&fixture, &probe, &fault, 135,
            SIG_BLOCK_, wrapping_sigset_address, oldset_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            fixture.task.blocked == high_signal_bit &&
            probe.reads == 0 && probe.writes == 0 &&
            fault.address == wrapping_sigset_address &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "set 地址回绕在用户回调与状态变更前失败");

    reset_user_probe(&probe, 0);
    store_user_sigset(&probe, set_address, sig_mask(SIGUSR2_));
    result = invoke(&fixture, &probe, &fault, 135,
            SIG_SETMASK_, set_address, wrapping_sigset_address,
            sizeof(sigset_t_), 0, 0);
    CHECK(result == (qword_t) (sqword_t) _EFAULT &&
            fixture.task.blocked == sig_mask(SIGUSR2_) &&
            probe.reads == 1 && probe.writes == 0 &&
            fault.address == wrapping_sigset_address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "oldset 地址回绕保留已应用的 SETMASK 副作用");

    current = NULL;
    return 0;
}

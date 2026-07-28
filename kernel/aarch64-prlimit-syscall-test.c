#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "guest/aarch64/linux-resource-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define AARCH64_PRLIMIT64_SYSCALL UINT64_C(261)
#define HIGH_ARGUMENT UINT64_C(0xa5a5a5a500000000)
#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE 512
#define NEW_ADDRESS (USER_BASE + 3)
#define OLD_ADDRESS (USER_BASE + 131)
#define ALIAS_ADDRESS (USER_BASE + 259)
#define ACCESS_LOG_CAPACITY 8
#define I386_PAGE UINT32_C(0x10000000)
#define I386_INPUT (I386_PAGE + UINT32_C(0x100))
#define I386_OUTPUT (I386_PAGE + UINT32_C(0x140))
#define I386_UNMAPPED (I386_PAGE + UINT32_C(0x1100))
#define TEST_RESOURCE RLIMIT_RTTIME_
#define CONCURRENT_WRITERS 4
#define CONCURRENT_ITERATIONS 2000

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 prlimit64 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

_Static_assert(sizeof(struct aarch64_linux_rlimit64) == 16 &&
        _Alignof(struct aarch64_linux_rlimit64) == 8,
        "测试必须使用固定的 AArch64 Linux rlimit64 wire");
_Static_assert(sizeof(struct rlimit32_) == 8 &&
        sizeof(struct rlimit_) == 16,
        "i386 与 rlimit64 wire 必须保持各自固定宽度");

struct published_process {
    struct task *leader;
    struct tgroup group;
};

struct user_access {
    qword_t address;
    dword_t size;
    enum guest_memory_access access;
};

struct user_probe {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    struct user_access accesses[ACCESS_LOG_CAPACITY];
    unsigned access_count;
};

struct concurrent_context {
    struct task *caller;
    pid_t_ target_pid;
    struct rlimit_ values[CONCURRENT_WRITERS];
    unsigned value_index;
    atomic_bool *failed;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static bool limits_equal(
        struct rlimit_ left, struct rlimit_ right) {
    return left.cur == right.cur && left.max == right.max;
}

static bool range_contains(
        qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
}

static bool probe_range(
        qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE)
        return false;
    qword_t relative = address - USER_BASE;
    if (relative > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
}

static void record_access(struct user_probe *probe,
        qword_t address, dword_t size,
        enum guest_memory_access access) {
    if (probe->access_count < ACCESS_LOG_CAPACITY) {
        probe->accesses[probe->access_count] =
                (struct user_access) {
                    .address = address,
                    .size = size,
                    .access = access,
                };
    }
    probe->access_count++;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    record_access(probe, address, size, GUEST_MEMORY_READ);
    size_t offset;
    if (!probe_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->fail_read_at != UINT64_MAX &&
            range_contains(address, size, probe->fail_read_at)) {
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

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_probe *probe = opaque;
    record_access(probe, address, size, GUEST_MEMORY_WRITE);
    size_t offset;
    if (!probe_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    if (probe->fail_write_at != UINT64_MAX &&
            range_contains(address, size, probe->fail_write_at)) {
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

static void reset_probe(struct user_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->fail_read_at = UINT64_MAX;
    probe->fail_write_at = UINT64_MAX;
}

static void reset_access(struct user_probe *probe) {
    probe->fail_read_at = UINT64_MAX;
    probe->fail_write_at = UINT64_MAX;
    probe->access_count = 0;
    memset(probe->accesses, 0, sizeof(probe->accesses));
}

static size_t probe_offset(qword_t address) {
    return (size_t) (address - USER_BASE);
}

static void store_wire(struct user_probe *probe, qword_t address,
        struct aarch64_linux_rlimit64 wire) {
    memcpy(probe->bytes + probe_offset(address), &wire, sizeof(wire));
}

static struct aarch64_linux_rlimit64 load_wire(
        const struct user_probe *probe, qword_t address) {
    struct aarch64_linux_rlimit64 wire;
    memcpy(&wire, probe->bytes + probe_offset(address), sizeof(wire));
    return wire;
}

static void init_group(struct tgroup *group, struct task *leader) {
    *group = (struct tgroup) {0};
    list_init(&group->threads);
    list_init(&group->session);
    list_init(&group->pgroup);
    lock_init(&group->lock);
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = leader;
    group->sid = leader->pid;
    group->pgid = leader->pid;
    for (unsigned resource = 0;
            resource < RLIMIT_NLIMITS_; resource++) {
        group->limits[resource] =
                (struct rlimit_) {RLIM_INFINITY_, RLIM_INFINITY_};
    }
}

static bool map_i386_page(struct mm *mm) {
    write_wrlock(&mm->mem.lock);
    int error = pt_map_nothing(
            &mm->mem, PAGE(I386_PAGE), 1, P_RWX);
    write_wrunlock(&mm->mem.lock);
    return error == 0;
}

static bool init_process(
        struct published_process *process, bool with_i386_memory) {
    memset(process, 0, sizeof(*process));
    struct task *leader = task_create_(NULL);
    if (leader == NULL)
        return false;
    init_group(&process->group, leader);
    leader->group = &process->group;
    leader->tgid = leader->pid;
    if (with_i386_memory) {
        struct mm *mm = mm_new();
        if (mm == NULL || !map_i386_page(mm)) {
            if (mm != NULL)
                mm_release(mm);
            task_abort_create(leader);
            cond_destroy(&process->group.stopped_cond);
            cond_destroy(&process->group.child_exit);
            return false;
        }
        task_set_mm(leader, mm);
    }
    process->leader = leader;
    task_publish(leader);
    return true;
}

static void destroy_process(struct published_process *process) {
    struct task *leader = process->leader;
    if (leader == NULL)
        return;
    if (current == leader)
        current = NULL;
    struct mm *mm = leader->mm;
    cond_destroy(&leader->pause);
    cond_destroy(&leader->ptrace.cond);
    lock(&pids_lock);
    lock(&process->group.lock);
    list_remove(&leader->group_links);
    list_remove(&process->group.session);
    list_remove(&process->group.pgroup);
    task_destroy(leader);
    unlock(&process->group.lock);
    unlock(&pids_lock);
    if (mm != NULL)
        mm_release(mm);
    cond_destroy(&process->group.stopped_cond);
    cond_destroy(&process->group.child_exit);
    process->leader = NULL;
}

static void set_credentials(struct task *task,
        struct task_credentials credentials) {
    lock(&pids_lock);
    task->uid = credentials.uid;
    task->gid = credentials.gid;
    task->euid = credentials.euid;
    task->egid = credentials.egid;
    task->suid = credentials.suid;
    task->sgid = credentials.sgid;
    unlock(&pids_lock);
}

static void set_limit(
        struct task *task, dword_t resource, struct rlimit_ limit) {
    lock(&task->group->lock);
    task->group->limits[resource] = limit;
    unlock(&task->group->lock);
}

static struct rlimit_ get_limit(
        struct task *task, dword_t resource) {
    lock(&task->group->lock);
    struct rlimit_ limit = task->group->limits[resource];
    unlock(&task->group->lock);
    return limit;
}

static qword_t invoke_prlimit64(struct task *caller,
        struct user_probe *probe,
        struct guest_linux_user_fault *fault,
        qword_t pid, qword_t resource,
        qword_t new_address, qword_t old_address) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = caller,
        .user = {
            .opaque = probe,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = AARCH64_PRLIMIT64_SYSCALL,
        .arguments = {
            pid, resource, new_address, old_address,
            UINT64_C(0x1122334455667788),
        },
    };
    current = caller;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static bool test_wire_self_and_explicit_context(void) {
    struct published_process caller_process;
    struct published_process decoy_process;
    CHECK(init_process(&caller_process, false) &&
            init_process(&decoy_process, false),
            "建立已发布 caller 与 current decoy");
    struct task *caller = caller_process.leader;
    struct task *decoy = decoy_process.leader;
    set_credentials(caller, (struct task_credentials) {
        .uid = 1001, .euid = 1001, .suid = 1001,
        .gid = 2001, .egid = 2001, .sgid = 2001,
    });

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    struct rlimit_ wide = {
        UINT64_C(0x100000002), UINT64_MAX,
    };
    set_limit(caller, RLIMIT_NOFILE_, wide);
    qword_t result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | RLIMIT_NOFILE_,
            0, OLD_ADDRESS);
    struct aarch64_linux_rlimit64 observed =
            load_wire(&probe, OLD_ADDRESS);
    CHECK(result == 0 && probe.access_count == 1 &&
            probe.accesses[0].access == GUEST_MEMORY_WRITE &&
            probe.accesses[0].address == OLD_ADDRESS &&
            probe.accesses[0].size == 16 &&
            observed.cur == wide.cur && observed.max == wide.max,
            "pid/resource 高位被忽略且未对齐 wire 保留完整 64 位值");

    struct rlimit_ previous = {1001, 2002};
    struct rlimit_ replacement = {501, 1502};
    set_limit(caller, TEST_RESOURCE, previous);
    store_wire(&probe, ALIAS_ADDRESS,
            (struct aarch64_linux_rlimit64) {
                replacement.cur, replacement.max,
            });
    reset_access(&probe);
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (dword_t) caller->pid,
            HIGH_ARGUMENT | TEST_RESOURCE,
            ALIAS_ADDRESS, ALIAS_ADDRESS);
    observed = load_wire(&probe, ALIAS_ADDRESS);
    CHECK(result == 0 && probe.access_count == 2 &&
            probe.accesses[0].access == GUEST_MEMORY_READ &&
            probe.accesses[1].access == GUEST_MEMORY_WRITE &&
            probe.accesses[0].address == ALIAS_ADDRESS &&
            probe.accesses[1].address == ALIAS_ADDRESS &&
            observed.cur == previous.cur &&
            observed.max == previous.max &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    replacement),
            "显式 self PID 的同址输入输出先读新值再写回旧值");

    set_limit(caller, RLIMIT_RTPRIO_,
            (struct rlimit_) {3, 4});
    set_limit(decoy, RLIMIT_RTPRIO_,
            (struct rlimit_) {9, 10});
    struct rlimit_ lowered = {1, 2};
    struct rlimit_ old_limit;
    current = decoy;
    CHECK(resource_prlimit_task(caller, 0, RLIMIT_RTPRIO_,
                    &lowered, &old_limit) == 0 &&
            limits_equal(old_limit, (struct rlimit_) {3, 4}) &&
            limits_equal(get_limit(caller, RLIMIT_RTPRIO_),
                    lowered) &&
            limits_equal(get_limit(decoy, RLIMIT_RTPRIO_),
                    (struct rlimit_) {9, 10}),
            "共享核心以显式 caller 解析 pid 0 而不依赖 TLS current");

    destroy_process(&decoy_process);
    destroy_process(&caller_process);
    return true;
}

static void mismatch_credential(
        struct task_credentials *credentials, unsigned index) {
    switch (index) {
        case 0: credentials->uid++; break;
        case 1: credentials->euid++; break;
        case 2: credentials->suid++; break;
        case 3: credentials->gid++; break;
        case 4: credentials->egid++; break;
        case 5: credentials->sgid++; break;
    }
}

static bool test_error_order_and_validation(void) {
    struct published_process caller_process;
    struct published_process target_process;
    CHECK(init_process(&caller_process, false) &&
            init_process(&target_process, false),
            "建立错误顺序 caller 与 target");
    struct task *caller = caller_process.leader;
    struct task *target = target_process.leader;
    const struct task_credentials caller_credentials = {
        .uid = 1001, .euid = 3001, .suid = 5001,
        .gid = 2001, .egid = 4001, .sgid = 6001,
    };
    const struct task_credentials matching_target = {
        .uid = 1001, .euid = 1001, .suid = 1001,
        .gid = 2001, .egid = 2001, .sgid = 2001,
    };
    set_credentials(caller, caller_credentials);
    set_credentials(target, matching_target);
    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {10, 20});

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {5, 15});
    probe.fail_read_at = NEW_ADDRESS + 8;
    qword_t result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (MAX_PID + 1),
            HIGH_ARGUMENT | RLIMIT_NLIMITS_,
            NEW_ADDRESS, OLD_ADDRESS);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.access_count == 1 &&
            probe.accesses[0].access == GUEST_MEMORY_READ &&
            fault.address == NEW_ADDRESS + 8 &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {10, 20}),
            "新值中途 EFAULT 优先于 PID、资源与旧值故障");

    reset_access(&probe);
    qword_t crossing_address =
            AARCH64_LINUX_USER_ADDRESS_MAX - 7;
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (MAX_PID + 1),
            HIGH_ARGUMENT | RLIMIT_NLIMITS_,
            crossing_address, OLD_ADDRESS);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.access_count == 0 &&
            fault.address == crossing_address &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "越过 AArch64 用户上界的新值在查 PID 前结构性失败");

    reset_access(&probe);
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (MAX_PID + 1),
            HIGH_ARGUMENT | RLIMIT_NLIMITS_,
            0, OLD_ADDRESS);
    CHECK(result == encoded_error(_ESRCH) &&
            probe.access_count == 0,
            "不存在 PID 优先于资源与旧值输出故障");
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | UINT32_MAX,
            HIGH_ARGUMENT | TEST_RESOURCE, 0, OLD_ADDRESS);
    CHECK(result == encoded_error(_ESRCH) &&
            probe.access_count == 0,
            "负 PID 按有符号低 32 位解析并返回 ESRCH");

    struct task_credentials mismatched = matching_target;
    mismatched.euid++;
    set_credentials(target, mismatched);
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (dword_t) target->pid,
            HIGH_ARGUMENT | RLIMIT_NLIMITS_,
            0, OLD_ADDRESS);
    CHECK(result == encoded_error(_EPERM) &&
            probe.access_count == 0,
            "跨任务权限拒绝优先于资源与旧值输出故障");
    set_credentials(target, matching_target);

    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | RLIMIT_NLIMITS_,
            0, OLD_ADDRESS);
    CHECK(result == encoded_error(_EINVAL) &&
            probe.access_count == 0,
            "self 无效资源返回 EINVAL 且不写旧值");

    lock(&pids_lock);
    target->exiting = true;
    unlock(&pids_lock);
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (dword_t) target->pid,
            HIGH_ARGUMENT | RLIMIT_NLIMITS_, 0, 0);
    CHECK(result == encoded_error(_EINVAL),
            "已退出目标仍先执行资源编号校验");
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (dword_t) target->pid,
            HIGH_ARGUMENT | TEST_RESOURCE, 0, 0);
    CHECK(result == encoded_error(_ESRCH),
            "合法资源访问已退出目标返回 ESRCH");
    lock(&pids_lock);
    target->exiting = false;
    unlock(&pids_lock);

    reset_access(&probe);
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {11, 10});
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | TEST_RESOURCE,
            NEW_ADDRESS, OLD_ADDRESS);
    CHECK(result == encoded_error(_EINVAL) &&
            probe.access_count == 1 &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {10, 20}),
            "cur 大于 max 返回 EINVAL 且不更新或输出");

    reset_access(&probe);
    set_limit(caller, RLIMIT_NOFILE_,
            (struct rlimit_) {100, UINT64_C(2000000)});
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {
                100, UINT64_C(1048577),
            });
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | RLIMIT_NOFILE_,
            NEW_ADDRESS, 0);
    CHECK(result == encoded_error(_EPERM) &&
            limits_equal(get_limit(caller, RLIMIT_NOFILE_),
                    (struct rlimit_) {100, UINT64_C(2000000)}),
            "RLIMIT_NOFILE hard 超过 Linux nr_open 默认上限时拒绝");

    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {10, 20});
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {10, 21});
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | TEST_RESOURCE,
            NEW_ADDRESS, 0);
    CHECK(result == encoded_error(_EPERM) &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {10, 20}),
            "非 root 不能提高 hard limit");
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {5, 15});
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | TEST_RESOURCE,
            NEW_ADDRESS, 0);
    CHECK(result == 0 &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {5, 15}),
            "非 root 可以降低 soft 与 hard limit");

    reset_access(&probe);
    CHECK(invoke_prlimit64(caller, &probe, &fault,
                    HIGH_ARGUMENT, HIGH_ARGUMENT | TEST_RESOURCE,
                    0, 0) == 0 &&
            probe.access_count == 0 &&
            invoke_prlimit64(caller, &probe, &fault,
                    HIGH_ARGUMENT, HIGH_ARGUMENT | RLIMIT_NLIMITS_,
                    0, 0) == encoded_error(_EINVAL) &&
            probe.access_count == 0,
            "双空指针对合法资源成功、对越界资源返回 EINVAL");

    destroy_process(&target_process);
    destroy_process(&caller_process);
    return true;
}

static bool test_old_output_fault_does_not_rollback(void) {
    struct published_process process;
    CHECK(init_process(&process, false),
            "建立旧值故障 caller");
    struct task *caller = process.leader;
    set_credentials(caller, (struct task_credentials) {
        .uid = 1001, .euid = 1001, .suid = 1001,
        .gid = 2001, .egid = 2001, .sgid = 2001,
    });
    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {80, 90});

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {40, 50});
    memset(probe.bytes + probe_offset(OLD_ADDRESS),
            0xa5, sizeof(struct aarch64_linux_rlimit64));
    probe.fail_write_at = OLD_ADDRESS + 8;
    qword_t result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | TEST_RESOURCE,
            NEW_ADDRESS, OLD_ADDRESS);
    qword_t first;
    qword_t second;
    memcpy(&first, probe.bytes + probe_offset(OLD_ADDRESS),
            sizeof(first));
    memcpy(&second, probe.bytes + probe_offset(OLD_ADDRESS) + 8,
            sizeof(second));
    CHECK(result == encoded_error(_EFAULT) &&
            probe.access_count == 2 &&
            probe.accesses[0].access == GUEST_MEMORY_READ &&
            probe.accesses[1].access == GUEST_MEMORY_WRITE &&
            fault.address == OLD_ADDRESS + 8 &&
            first == 80 &&
            second == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {40, 50}),
            "旧值中途写 EFAULT 保留部分输出且不回滚新限制");

    reset_access(&probe);
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {20, 30});
    qword_t crossing_address =
            AARCH64_LINUX_USER_ADDRESS_MAX - 7;
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT, HIGH_ARGUMENT | TEST_RESOURCE,
            NEW_ADDRESS, crossing_address);
    CHECK(result == encoded_error(_EFAULT) &&
            probe.access_count == 1 &&
            probe.accesses[0].access == GUEST_MEMORY_READ &&
            fault.address == crossing_address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {20, 30}),
            "旧值结构性地址故障同样发生在新限制提交之后");

    destroy_process(&process);
    return true;
}

static bool test_cross_pid_permissions(void) {
    struct published_process caller_process;
    struct published_process target_process;
    CHECK(init_process(&caller_process, false) &&
            init_process(&target_process, false),
            "建立跨 PID caller 与 target");
    struct task *caller = caller_process.leader;
    struct task *target = target_process.leader;
    const struct task_credentials caller_credentials = {
        .uid = 1001, .euid = 3001, .suid = 5001,
        .gid = 2001, .egid = 4001, .sgid = 6001,
    };
    const struct task_credentials matching_target = {
        .uid = 1001, .euid = 1001, .suid = 1001,
        .gid = 2001, .egid = 2001, .sgid = 2001,
    };
    set_credentials(caller, caller_credentials);
    set_credentials(target, matching_target);
    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {111, 222});
    set_limit(target, TEST_RESOURCE,
            (struct rlimit_) {333, 444});

    struct user_probe probe;
    struct guest_linux_user_fault fault;
    reset_probe(&probe);
    qword_t result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (dword_t) target->pid,
            HIGH_ARGUMENT | TEST_RESOURCE, 0, OLD_ADDRESS);
    struct aarch64_linux_rlimit64 observed =
            load_wire(&probe, OLD_ADDRESS);
    CHECK(result == 0 && observed.cur == 333 &&
            observed.max == 444 &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {111, 222}),
            "匹配 real UID/GID 时跨 PID 读取目标限制而非 caller");

    for (unsigned field = 0; field < 6; field++) {
        struct task_credentials mismatched = matching_target;
        mismatch_credential(&mismatched, field);
        set_credentials(target, mismatched);
        reset_access(&probe);
        CHECK(invoke_prlimit64(caller, &probe, &fault,
                        HIGH_ARGUMENT | (dword_t) target->pid,
                        HIGH_ARGUMENT | TEST_RESOURCE,
                        0, OLD_ADDRESS) ==
                        encoded_error(_EPERM) &&
                probe.access_count == 0,
                "目标任一 UID/GID 字段不匹配时拒绝且无输出");
    }

    set_credentials(target, (struct task_credentials) {
        .uid = 9001, .euid = 9002, .suid = 9003,
        .gid = 9101, .egid = 9102, .sgid = 9103,
    });
    struct task_credentials root = caller_credentials;
    root.euid = 0;
    set_credentials(caller, root);
    store_wire(&probe, NEW_ADDRESS,
            (struct aarch64_linux_rlimit64) {477, 888});
    reset_access(&probe);
    result = invoke_prlimit64(caller, &probe, &fault,
            HIGH_ARGUMENT | (dword_t) target->pid,
            HIGH_ARGUMENT | TEST_RESOURCE,
            NEW_ADDRESS, OLD_ADDRESS);
    observed = load_wire(&probe, OLD_ADDRESS);
    CHECK(result == 0 && observed.cur == 333 &&
            observed.max == 444 &&
            limits_equal(get_limit(target, TEST_RESOURCE),
                    (struct rlimit_) {477, 888}) &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {111, 222}),
            "effective root 近似 CAP_SYS_RESOURCE 提高目标 hard limit");

    destroy_process(&target_process);
    destroy_process(&caller_process);
    return true;
}

static bool test_i386_compatibility(void) {
    struct published_process caller_process;
    struct published_process target_process;
    CHECK(init_process(&caller_process, true) &&
            init_process(&target_process, false),
            "建立 i386 wire caller 与跨 PID target");
    struct task *caller = caller_process.leader;
    struct task *target = target_process.leader;
    const struct task_credentials credentials = {
        .uid = 1001, .euid = 1001, .suid = 1001,
        .gid = 2001, .egid = 2001, .sgid = 2001,
    };
    set_credentials(caller, credentials);
    set_credentials(target, credentials);
    current = caller;

    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {0, 1000});
    struct {
        struct rlimit32_ wire;
        qword_t canary;
    } input = {
        .wire = {123, 456},
        .canary = UINT64_C(0x1122334455667788),
    };
    CHECK(user_write_task(caller, I386_INPUT,
                    &input, sizeof(input)) == 0 &&
            sys_setrlimit32(TEST_RESOURCE, I386_INPUT) == 0 &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {123, 456}),
            "i386 setrlimit 只读取 8 字节 wire 并设置限制");
    qword_t observed_canary = 0;
    CHECK(user_read_task(caller,
                    I386_INPUT + sizeof(struct rlimit32_),
                    &observed_canary, sizeof(observed_canary)) == 0 &&
            observed_canary == input.canary,
            "i386 setrlimit 不触碰相邻八字节");

    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {
                UINT64_C(0x100000010), RLIM_INFINITY_,
            });
    CHECK(sys_getrlimit32(TEST_RESOURCE, I386_OUTPUT) == 0,
            "i386 getrlimit 回读高值");
    struct rlimit32_ output;
    CHECK(user_read_task(caller, I386_OUTPUT,
                    &output, sizeof(output)) == 0 &&
            output.cur == UINT32_MAX &&
            output.max == UINT32_MAX,
            "超过 32 位的限制和值 infinity 均饱和为 UINT32_MAX");

    input.wire = (struct rlimit32_) {
        UINT32_MAX, UINT32_MAX,
    };
    set_limit(caller, TEST_RESOURCE,
            (struct rlimit_) {RLIM_INFINITY_, RLIM_INFINITY_});
    CHECK(user_write_task(caller, I386_INPUT,
                    &input.wire, sizeof(input.wire)) == 0 &&
            sys_setrlimit32(TEST_RESOURCE, I386_INPUT) == 0 &&
            limits_equal(get_limit(caller, TEST_RESOURCE),
                    (struct rlimit_) {
                        RLIM_INFINITY_, RLIM_INFINITY_,
                    }),
            "i386 UINT32_MAX 输入映射为内部 64 位 infinity");

    const qword_t boundary_values[] = {
        UINT32_MAX - UINT64_C(1),
        UINT32_MAX,
        UINT32_MAX + UINT64_C(1),
        UINT64_MAX,
    };
    for (unsigned index = 0;
            index < array_size(boundary_values); index++) {
        struct rlimit_ boundary_wire = {
            boundary_values[index], boundary_values[index],
        };
        set_limit(caller, RLIMIT_RTPRIO_,
                (struct rlimit_) {
                    RLIM_INFINITY_, RLIM_INFINITY_,
                });
        CHECK(user_write_task(caller, I386_INPUT,
                        &boundary_wire, sizeof(boundary_wire)) == 0 &&
                sys_prlimit64(0, RLIMIT_RTPRIO_,
                        I386_INPUT, 0) == 0,
                "i386 prlimit64 接受四个 infinity 边界输入");
        rlim_t_ expected = boundary_values[index] < UINT32_MAX ?
                boundary_values[index] : RLIM_INFINITY_;
        CHECK(limits_equal(get_limit(caller, RLIMIT_RTPRIO_),
                        (struct rlimit_) {expected, expected}),
                "i386 rlimit64 输入按 32 位内核规则归一");
    }
    set_limit(caller, RLIMIT_RTPRIO_,
            (struct rlimit_) {
                UINT32_MAX - UINT64_C(1),
                UINT32_MAX + UINT64_C(1),
            });
    CHECK(sys_prlimit64(0, RLIMIT_RTPRIO_,
                    0, I386_OUTPUT) == 0,
            "i386 prlimit64 投影有限边界与内部高值");
    struct rlimit_ projected;
    CHECK(user_read_task(caller, I386_OUTPUT,
                    &projected, sizeof(projected)) == 0 &&
            projected.cur == UINT32_MAX - UINT64_C(1) &&
            projected.max == UINT64_MAX,
            "i386 输出保留最大有限值并把高值扩成 UINT64_MAX");

    struct rlimit_ old = {100, 200};
    struct rlimit_ replacement = {50, 150};
    set_limit(caller, RLIMIT_RTPRIO_, old);
    CHECK(user_write_task(caller, I386_INPUT,
                    &replacement, sizeof(replacement)) == 0 &&
            sys_prlimit64(0, RLIMIT_RTPRIO_,
                    I386_INPUT, I386_INPUT) == 0,
            "i386 prlimit64 接受同址输入输出");
    struct rlimit_ observed_old;
    CHECK(user_read_task(caller, I386_INPUT,
                    &observed_old, sizeof(observed_old)) == 0 &&
            limits_equal(observed_old, old) &&
            limits_equal(get_limit(caller, RLIMIT_RTPRIO_),
                    replacement),
            "i386 prlimit64 同址先读新值再覆盖旧值");

    CHECK(sys_prlimit64(MAX_PID + 1, RLIMIT_NLIMITS_,
                    I386_UNMAPPED, I386_UNMAPPED) ==
                    (dword_t) _EFAULT,
            "i386 prlimit64 新值 EFAULT 优先于 PID 与资源错误");
    replacement = (struct rlimit_) {20, 30};
    CHECK(user_write_task(caller, I386_INPUT,
                    &replacement, sizeof(replacement)) == 0 &&
            sys_prlimit64(0, RLIMIT_RTPRIO_,
                    I386_INPUT, I386_UNMAPPED) ==
                    (dword_t) _EFAULT &&
            limits_equal(get_limit(caller, RLIMIT_RTPRIO_),
                    replacement),
            "i386 prlimit64 旧值 EFAULT 不回滚新限制");

    set_limit(target, RLIMIT_RTPRIO_,
            (struct rlimit_) {300, 400});
    replacement = (struct rlimit_) {200, 300};
    CHECK(user_write_task(caller, I386_INPUT,
                    &replacement, sizeof(replacement)) == 0 &&
            sys_prlimit64(target->pid, RLIMIT_RTPRIO_,
                    I386_INPUT, I386_OUTPUT) == 0 &&
            user_read_task(caller, I386_OUTPUT,
                    &observed_old, sizeof(observed_old)) == 0 &&
            limits_equal(observed_old,
                    (struct rlimit_) {300, 400}) &&
            limits_equal(get_limit(target, RLIMIT_RTPRIO_),
                    replacement),
            "i386 prlimit64 与 AArch64 共享跨 PID 原子核心");

    destroy_process(&target_process);
    destroy_process(&caller_process);
    return true;
}

static bool limit_is_allowed(const struct concurrent_context *context,
        struct rlimit_ limit) {
    if (limits_equal(limit, (struct rlimit_) {1, 100}))
        return true;
    for (unsigned index = 0; index < CONCURRENT_WRITERS; index++) {
        if (limits_equal(limit, context->values[index]))
            return true;
    }
    return false;
}

static void *concurrent_writer(void *opaque) {
    struct concurrent_context *context = opaque;
    for (unsigned iteration = 0;
            iteration < CONCURRENT_ITERATIONS; iteration++) {
        struct rlimit_ old;
        if (resource_prlimit_task(context->caller,
                    context->target_pid, TEST_RESOURCE,
                    &context->values[context->value_index],
                    &old) != 0 ||
                !limit_is_allowed(context, old)) {
            atomic_store_explicit(
                    context->failed, true, memory_order_release);
            break;
        }
    }
    return NULL;
}

static bool test_concurrent_atomic_exchange(void) {
    struct published_process caller_process;
    struct published_process target_process;
    CHECK(init_process(&caller_process, false) &&
            init_process(&target_process, false),
            "建立并发交换 caller 与 target");
    struct task *caller = caller_process.leader;
    struct task *target = target_process.leader;
    set_credentials(caller, (struct task_credentials) {
        .uid = 1001, .euid = 0, .suid = 1001,
        .gid = 2001, .egid = 2001, .sgid = 2001,
    });
    set_credentials(target, (struct task_credentials) {
        .uid = 9001, .euid = 9002, .suid = 9003,
        .gid = 9101, .egid = 9102, .sgid = 9103,
    });
    set_limit(target, TEST_RESOURCE,
            (struct rlimit_) {1, 100});

    atomic_bool failed;
    atomic_init(&failed, false);
    struct concurrent_context contexts[CONCURRENT_WRITERS];
    pthread_t threads[CONCURRENT_WRITERS];
    for (unsigned index = 0;
            index < CONCURRENT_WRITERS; index++) {
        contexts[index] = (struct concurrent_context) {
            .caller = caller,
            .target_pid = target->pid,
            .value_index = index,
            .failed = &failed,
        };
        for (unsigned value = 0;
                value < CONCURRENT_WRITERS; value++) {
            contexts[index].values[value] =
                    (struct rlimit_) {
                        10 + value, 1000 + value,
                    };
        }
        CHECK(pthread_create(&threads[index], NULL,
                        concurrent_writer, &contexts[index]) == 0,
                "建立并发 prlimit writer");
    }
    for (unsigned index = 0;
            index < CONCURRENT_WRITERS; index++) {
        CHECK(pthread_join(threads[index], NULL) == 0,
                "回收并发 prlimit writer");
    }
    CHECK(!atomic_load_explicit(
                    &failed, memory_order_acquire) &&
            limit_is_allowed(&contexts[0],
                    get_limit(target, TEST_RESOURCE)),
            "并发旧值快照与新值安装不产生撕裂 pair");

    destroy_process(&target_process);
    destroy_process(&caller_process);
    return true;
}

int main(void) {
    if (!test_wire_self_and_explicit_context() ||
            !test_error_order_and_validation() ||
            !test_old_output_fault_does_not_rollback() ||
            !test_cross_pid_permissions() ||
            !test_i386_compatibility() ||
            !test_concurrent_atomic_exchange())
        return 1;
    puts("AArch64 prlimit64 与共享资源限制测试通过");
    return 0;
}

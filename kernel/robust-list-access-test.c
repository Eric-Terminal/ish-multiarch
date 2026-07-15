#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "guest/aarch64/linux-futex-abi.h"
#include "guest/linux/futex-abi.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/futex.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define AARCH64_LINUX_SYS_GET_ROBUST_LIST 100

#define I386_OUTPUT_PAGE UINT32_C(0x10000000)
#define I386_HEAD_OUTPUT (I386_OUTPUT_PAGE + UINT32_C(0x100))
#define I386_LENGTH_OUTPUT (I386_OUTPUT_PAGE + UINT32_C(0x104))
#define AARCH64_HEAD_OUTPUT UINT64_C(0x20000000)
#define AARCH64_LENGTH_OUTPUT UINT64_C(0x20000008)

#define I386_HEAD_A UINT32_C(0x13579bdf)
#define I386_HEAD_B UINT32_C(0x2468ace0)
#define AARCH64_HEAD_A UINT64_C(0x0123456789abcdef)
#define AARCH64_HEAD_B UINT64_C(0xfedcba9876543210)
#define OUTPUT_MARKER UINT32_C(0x6a09e667)
#define I386_ROBUST_HEAD_SIZE \
    ((dword_t) sizeof(struct i386_linux_robust_list_head))
#define AARCH64_ROBUST_HEAD_SIZE \
    ((qword_t) sizeof(struct aarch64_linux_robust_list_head))

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "robust-list 跨任务测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct published_process {
    struct task *leader;
    struct tgroup group;
};

struct aarch64_output_probe {
    unsigned writes;
    qword_t addresses[2];
    qword_t values[2];
    dword_t sizes[2];
};

struct robust_writer {
    struct task *task;
    atomic_bool ready;
    atomic_bool start;
    atomic_bool reader_done;
    size_t iterations;
};

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
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
}

static bool init_process(struct published_process *process, bool with_mm) {
    memset(process, 0, sizeof(*process));
    struct task *leader = task_create_(NULL);
    if (leader == NULL)
        return false;
    init_group(&process->group, leader);
    leader->group = &process->group;
    leader->tgid = leader->pid;
    if (with_mm) {
        struct mm *mm = mm_new();
        if (mm == NULL) {
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

static bool map_i386_outputs(struct task *task) {
    if (task->mem == NULL)
        return false;
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(task->mem,
            PAGE(I386_OUTPUT_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error == 0;
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

static struct task *make_peer(struct task *leader) {
    struct task *peer = task_create_(leader);
    if (peer == NULL)
        return NULL;
    peer->group = leader->group;
    peer->tgid = leader->tgid;
    task_publish(peer);
    return peer;
}

static void destroy_peer(struct task *peer) {
    struct tgroup *group = peer->group;
    cond_destroy(&peer->pause);
    cond_destroy(&peer->ptrace.cond);
    lock(&pids_lock);
    lock(&group->lock);
    list_remove(&peer->group_links);
    task_destroy(peer);
    unlock(&group->lock);
    unlock(&pids_lock);
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

static void set_robust_slots(
        struct task *task, addr_t i386, qword_t aarch64) {
    lock(&pids_lock);
    task->robust_list = i386;
    task->aarch64_robust_list = aarch64;
    unlock(&pids_lock);
}

static bool write_aarch64_output(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) fault;
    struct aarch64_output_probe *probe = opaque;
    if (probe->writes >= 2 || size != sizeof(qword_t))
        return false;
    qword_t value;
    memcpy(&value, source, sizeof(value));
    probe->addresses[probe->writes] = address;
    probe->values[probe->writes] = value;
    probe->sizes[probe->writes] = size;
    probe->writes++;
    return true;
}

static qword_t invoke_aarch64_get(struct task *caller, pid_t_ pid,
        struct aarch64_output_probe *probe) {
    memset(probe, 0, sizeof(*probe));
    struct guest_linux_user_fault fault;
    const struct guest_linux_syscall_context context = {
        .task_opaque = caller,
        .user = {
            .opaque = probe,
            .write = write_aarch64_output,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = AARCH64_LINUX_SYS_GET_ROBUST_LIST,
        .arguments = {
            (qword_t) (sqword_t) pid,
            AARCH64_HEAD_OUTPUT,
            AARCH64_LENGTH_OUTPUT,
        },
    };
    current = caller;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, &fault);
}

static qword_t syscall_error(int_t error) {
    return (qword_t) (sqword_t) error;
}

static bool aarch64_output_matches(
        const struct aarch64_output_probe *probe, qword_t head) {
    return probe->writes == 2 &&
            probe->addresses[0] == AARCH64_LENGTH_OUTPUT &&
            probe->values[0] == AARCH64_ROBUST_HEAD_SIZE &&
            probe->sizes[0] == sizeof(qword_t) &&
            probe->addresses[1] == AARCH64_HEAD_OUTPUT &&
            probe->values[1] == head &&
            probe->sizes[1] == sizeof(qword_t);
}

static bool prepare_i386_outputs(
        struct task *caller, dword_t marker) {
    return user_write_task(caller, I386_HEAD_OUTPUT,
                    &marker, sizeof(marker)) == 0 &&
            user_write_task(caller, I386_LENGTH_OUTPUT,
                    &marker, sizeof(marker)) == 0;
}

static bool read_i386_outputs(struct task *caller,
        dword_t *head, dword_t *length) {
    return user_read_task(caller, I386_HEAD_OUTPUT,
                    head, sizeof(*head)) == 0 &&
            user_read_task(caller, I386_LENGTH_OUTPUT,
                    length, sizeof(*length)) == 0;
}

static int_t invoke_i386_get(struct task *caller, pid_t_ pid,
        dword_t *head, dword_t *length) {
    current = caller;
    int_t result = sys_get_robust_list(
            pid, I386_HEAD_OUTPUT, I386_LENGTH_OUTPUT);
    // 直接调用 helper 时补齐 dispatcher 负责的 strace 行尾。
    STRACE(" = %#x\n", (dword_t) result);
    if (!read_i386_outputs(caller, head, length))
        return _EFAULT;
    return result;
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

static void *robust_writer_main(void *opaque) {
    struct robust_writer *writer = opaque;
    current = writer->task;
    atomic_store_explicit(&writer->ready, true, memory_order_release);
    while (!atomic_load_explicit(
            &writer->start, memory_order_acquire))
        sched_yield();

    do {
        bool alternate = (writer->iterations & 1) != 0;
        int_t i386_result = sys_set_robust_list(
                alternate ? I386_HEAD_A : I386_HEAD_B,
                I386_ROBUST_HEAD_SIZE);
        STRACE(" = %#x\n", (dword_t) i386_result);
        if (i386_result != 0 || sys_set_robust_list_aarch64(
                    alternate ? AARCH64_HEAD_A : AARCH64_HEAD_B,
                    AARCH64_ROBUST_HEAD_SIZE) != 0)
            break;
        writer->iterations++;
    } while (!atomic_load_explicit(
            &writer->reader_done, memory_order_acquire));
    current = NULL;
    return NULL;
}

static bool test_concurrent_snapshots(
        struct task *caller, struct task *target) {
    struct robust_writer writer = {
        .task = target,
    };
    atomic_init(&writer.ready, false);
    atomic_init(&writer.start, false);
    atomic_init(&writer.reader_done, false);
    pthread_t thread;
    if (pthread_create(
            &thread, NULL, robust_writer_main, &writer) != 0)
        return false;
    while (!atomic_load_explicit(&writer.ready, memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&writer.start, true, memory_order_release);

    bool valid = true;
    for (unsigned iteration = 0; iteration < 10000; iteration++) {
        dword_t i386_head;
        dword_t i386_length;
        qword_t aarch64_head;
        if (sys_get_robust_list_aarch64(
                    target->pid, &aarch64_head) != 0 ||
                (aarch64_head != AARCH64_HEAD_A &&
                    aarch64_head != AARCH64_HEAD_B) ||
                invoke_i386_get(caller, target->pid,
                    &i386_head, &i386_length) != 0 ||
                (i386_head != I386_HEAD_A &&
                    i386_head != I386_HEAD_B) ||
                i386_length != I386_ROBUST_HEAD_SIZE) {
            valid = false;
            break;
        }
    }
    atomic_store_explicit(
            &writer.reader_done, true, memory_order_release);
    bool joined = pthread_join(thread, NULL) == 0;
    current = caller;
    return joined && valid && writer.iterations != 0;
}

int main(void) {
    struct published_process caller_process;
    CHECK(init_process(&caller_process, true) &&
            map_i386_outputs(caller_process.leader),
            "建立已发布调用任务与 i386 输出页");
    struct task *caller = caller_process.leader;
    const struct task_credentials caller_credentials = {
        .uid = 1001,
        .euid = 3001,
        .suid = 5001,
        .gid = 2001,
        .egid = 4001,
        .sgid = 6001,
    };
    set_credentials(caller, caller_credentials);
    set_robust_slots(caller, I386_HEAD_A, AARCH64_HEAD_A);
    current = caller;

    qword_t observed_aarch64 = 0;
    dword_t observed_i386;
    dword_t observed_length;
    CHECK(sys_get_robust_list_aarch64(
                    0, &observed_aarch64) == 0 &&
            observed_aarch64 == AARCH64_HEAD_A &&
            sys_get_robust_list_aarch64(
                    caller->pid, &observed_aarch64) == 0 &&
            observed_aarch64 == AARCH64_HEAD_A,
            "pid 0 与显式正 PID 都查询当前 AArch64 槽");
    CHECK(prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, 0,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == I386_HEAD_A &&
            observed_length == I386_ROBUST_HEAD_SIZE &&
            invoke_i386_get(caller, caller->pid,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == I386_HEAD_A &&
            observed_length == I386_ROBUST_HEAD_SIZE,
            "pid 0 与显式正 PID 都查询当前 i386 槽");

    struct task *peer = make_peer(caller);
    CHECK(peer != NULL, "创建并发布同线程组 peer");
    lock(&pids_lock);
    bool fresh_peer_slots_are_zero =
            peer->robust_list == 0 && peer->aarch64_robust_list == 0;
    unlock(&pids_lock);
    CHECK(fresh_peer_slots_are_zero,
            "新任务不继承任何 guest ABI 的 robust 注册");
    set_credentials(peer, (struct task_credentials) {
        .uid = 7001, .euid = 7002, .suid = 7003,
        .gid = 8001, .egid = 8002, .sgid = 8003,
    });
    set_robust_slots(peer, I386_HEAD_B, AARCH64_HEAD_B);
    struct aarch64_output_probe probe;
    CHECK(invoke_aarch64_get(caller, peer->pid, &probe) == 0 &&
            aarch64_output_matches(&probe, AARCH64_HEAD_B),
            "同组权限忽略凭据并按精确 peer TID 读取 AArch64 槽");
    CHECK(prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, peer->pid,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == I386_HEAD_B &&
            observed_length == I386_ROBUST_HEAD_SIZE,
            "i386 调用者按精确 peer TID 读取兼容槽而非 leader 槽");
    destroy_peer(peer);

    struct published_process target_process;
    CHECK(init_process(&target_process, false),
            "建立独立线程组目标任务");
    struct task *target = target_process.leader;
    const struct task_credentials matching_target = {
        .uid = caller_credentials.uid,
        .euid = caller_credentials.uid,
        .suid = caller_credentials.uid,
        .gid = caller_credentials.gid,
        .egid = caller_credentials.gid,
        .sgid = caller_credentials.gid,
    };
    set_credentials(target, matching_target);
    set_robust_slots(target, I386_HEAD_B, AARCH64_HEAD_B);

    CHECK(invoke_aarch64_get(caller, target->pid, &probe) == 0 &&
            aarch64_output_matches(&probe, AARCH64_HEAD_B),
            "跨组匹配 caller real UID/GID 时读取 AArch64 槽");
    CHECK(prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, target->pid,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == I386_HEAD_B &&
            observed_length == I386_ROBUST_HEAD_SIZE,
            "跨组匹配 caller real UID/GID 时读取 i386 槽");

    for (unsigned field = 0; field < 6; field++) {
        struct task_credentials mismatched = matching_target;
        mismatch_credential(&mismatched, field);
        set_credentials(target, mismatched);
        CHECK(invoke_aarch64_get(caller,
                        target->pid, &probe) == syscall_error(_EPERM) &&
                probe.writes == 0,
                "目标任一 UID/GID 字段不匹配时 AArch64 无输出拒绝");
        CHECK(prepare_i386_outputs(caller, OUTPUT_MARKER) &&
                invoke_i386_get(caller, target->pid,
                        &observed_i386, &observed_length) == _EPERM &&
                observed_i386 == OUTPUT_MARKER &&
                observed_length == OUTPUT_MARKER,
                "目标任一 UID/GID 字段不匹配时 i386 无输出拒绝");
    }

    set_credentials(target, (struct task_credentials) {
        .uid = 9001, .euid = 9002, .suid = 9003,
        .gid = 9101, .egid = 9102, .sgid = 9103,
    });
    struct task_credentials root_caller = caller_credentials;
    root_caller.euid = 0;
    set_credentials(caller, root_caller);
    CHECK(invoke_aarch64_get(caller, target->pid, &probe) == 0 &&
            aarch64_output_matches(&probe, AARCH64_HEAD_B) &&
            prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, target->pid,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == I386_HEAD_B &&
            observed_length == I386_ROBUST_HEAD_SIZE,
            "有效 root 身份近似 CAP_SYS_PTRACE 跨组旁路");
    set_credentials(caller, caller_credentials);

    CHECK(invoke_aarch64_get(caller,
                    -1, &probe) == syscall_error(_ESRCH) &&
            probe.writes == 0 &&
            invoke_aarch64_get(caller,
                    MAX_PID + 1, &probe) == syscall_error(_ESRCH) &&
            probe.writes == 0,
            "负 PID 与越界 PID 返回 ESRCH 且 AArch64 不写输出");
    CHECK(prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, -1,
                    &observed_i386, &observed_length) == _ESRCH &&
            observed_i386 == OUTPUT_MARKER &&
            observed_length == OUTPUT_MARKER,
            "负 PID 返回 ESRCH 且 i386 不写输出");

    struct task *reserved = task_create_(caller);
    CHECK(reserved != NULL, "建立保留但未发布的 PID");
    pid_t_ reserved_pid = reserved->pid;
    CHECK(invoke_aarch64_get(caller,
                    reserved_pid, &probe) == syscall_error(_ESRCH) &&
            probe.writes == 0 &&
            prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, reserved_pid,
                    &observed_i386, &observed_length) == _ESRCH &&
            observed_i386 == OUTPUT_MARKER &&
            observed_length == OUTPUT_MARKER,
            "保留 PID 在发布前返回 ESRCH 且两种 ABI 都无输出");
    task_abort_create(reserved);

    set_credentials(target, matching_target);
    set_robust_slots(target, 0, 0);
    lock(&pids_lock);
    target->exiting = true;
    unlock(&pids_lock);
    CHECK(invoke_aarch64_get(caller, target->pid, &probe) == 0 &&
            aarch64_output_matches(&probe, 0) &&
            prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, target->pid,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == 0 &&
            observed_length == I386_ROBUST_HEAD_SIZE,
            "退出中但仍保留 PID 的任务可查询已消费的零槽");
    lock(&pids_lock);
    target->exiting = false;
    target->zombie = true;
    unlock(&pids_lock);
    CHECK(invoke_aarch64_get(caller, target->pid, &probe) == 0 &&
            aarch64_output_matches(&probe, 0) &&
            prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, target->pid,
                    &observed_i386, &observed_length) == 0 &&
            observed_i386 == 0 &&
            observed_length == I386_ROBUST_HEAD_SIZE,
            "保留 zombie 继续按精确 PID 返回两种 ABI 的零注册");
    lock(&pids_lock);
    target->zombie = false;
    unlock(&pids_lock);

    set_robust_slots(target, I386_HEAD_A, AARCH64_HEAD_A);
    CHECK(test_concurrent_snapshots(caller, target),
            "并发 set/get 只观察完整的 ABI 对应标量快照");

    pid_t_ reaped_pid = target->pid;
    destroy_process(&target_process);
    CHECK(invoke_aarch64_get(caller,
                    reaped_pid, &probe) == syscall_error(_ESRCH) &&
            probe.writes == 0 &&
            prepare_i386_outputs(caller, OUTPUT_MARKER) &&
            invoke_i386_get(caller, reaped_pid,
                    &observed_i386, &observed_length) == _ESRCH &&
            observed_i386 == OUTPUT_MARKER &&
            observed_length == OUTPUT_MARKER,
            "任务回收后精确 PID 返回 ESRCH 且两种 ABI 都无输出");

    current = NULL;
    destroy_process(&caller_process);
    return 0;
}

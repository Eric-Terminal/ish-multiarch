#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define PUBLISH_ITERATIONS 256
#define INHERIT_OBSERVATIONS 32

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "凭据原子快照测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

enum credential_family {
    CREDENTIAL_UID,
    CREDENTIAL_GID,
};

enum race_failure {
    RACE_OK,
    RACE_PTHREAD_FAILED,
    RACE_SYSCALL_FAILED,
    RACE_TASK_CREATE_FAILED,
    RACE_MIXED_SNAPSHOT,
};

struct credential_triple {
    uid_t_ real;
    uid_t_ effective;
    uid_t_ saved;
};

struct race_context {
    struct task *task;
    enum credential_family family;
    struct credential_triple first;
    struct credential_triple second;
    atomic_bool start;
    atomic_bool first_published;
    atomic_bool first_observed;
    atomic_bool done;
    atomic_uint inherit_observations;
    atomic_int failure;
};

static bool credentials_equal(
        const struct task_credentials *left,
        const struct task_credentials *right) {
    return left->uid == right->uid && left->euid == right->euid &&
            left->suid == right->suid && left->gid == right->gid &&
            left->egid == right->egid && left->sgid == right->sgid;
}

static bool snapshot_matches(const struct race_context *context,
        const struct task_credentials *credentials,
        const struct credential_triple *triple) {
    if (context->family == CREDENTIAL_UID) {
        return credentials->uid == triple->real &&
                credentials->euid == triple->effective &&
                credentials->suid == triple->saved;
    }
    return credentials->gid == triple->real &&
            credentials->egid == triple->effective &&
            credentials->sgid == triple->saved;
}

static dword_t publish_credentials(const struct race_context *context,
        const struct credential_triple *triple) {
    if (context->family == CREDENTIAL_UID) {
        return sys_setresuid(
                triple->real, triple->effective, triple->saved);
    }
    return sys_setresgid(
            triple->real, triple->effective, triple->saved);
}

static void *publish_credentials_repeatedly(void *opaque) {
    struct race_context *context = opaque;
    current = context->task;

    while (!atomic_load_explicit(&context->start, memory_order_acquire))
        sched_yield();

    if (publish_credentials(context, &context->second) != 0)
        atomic_store_explicit(&context->failure,
                RACE_SYSCALL_FAILED, memory_order_release);
    atomic_store_explicit(
            &context->first_published, true, memory_order_release);

    // 先让读取方完成一次快照，确保并发阶段不是空跑。
    while (!atomic_load_explicit(
            &context->first_observed, memory_order_acquire))
        sched_yield();

    for (int iteration = 0;
            (iteration < PUBLISH_ITERATIONS ||
                    atomic_load_explicit(&context->inherit_observations,
                            memory_order_acquire) < INHERIT_OBSERVATIONS) &&
                    atomic_load_explicit(
                            &context->failure, memory_order_acquire) == RACE_OK;
            iteration++) {
        const struct credential_triple *next =
                iteration % 2 == 0 ? &context->first : &context->second;
        if (publish_credentials(context, next) != 0) {
            atomic_store_explicit(&context->failure,
                    RACE_SYSCALL_FAILED, memory_order_release);
            break;
        }
        if (iteration % 64 == 0)
            sched_yield();
    }

    atomic_store_explicit(&context->done, true, memory_order_release);
    current = NULL;
    return NULL;
}

static enum race_failure run_atomic_publish_test(
        struct task *task, enum credential_family family,
        struct credential_triple first,
        struct credential_triple second) {
    struct race_context context = {
        .task = task,
        .family = family,
        .first = first,
        .second = second,
    };
    atomic_init(&context.start, false);
    atomic_init(&context.first_published, false);
    atomic_init(&context.first_observed, false);
    atomic_init(&context.done, false);
    atomic_init(&context.inherit_observations, 0);
    atomic_init(&context.failure, RACE_OK);

    pthread_t writer;
    if (pthread_create(&writer, NULL,
            publish_credentials_repeatedly, &context) != 0)
        return RACE_PTHREAD_FAILED;

    atomic_store_explicit(&context.start, true, memory_order_release);
    while (!atomic_load_explicit(
            &context.first_published, memory_order_acquire))
        sched_yield();

    while (!atomic_load_explicit(&context.done, memory_order_acquire)) {
        struct task_credentials credentials;
        task_credentials_snapshot(task, &credentials);
        if (!snapshot_matches(&context, &credentials, &first) &&
                !snapshot_matches(&context, &credentials, &second)) {
            atomic_store_explicit(&context.failure,
                    RACE_MIXED_SNAPSHOT, memory_order_release);
        }
        atomic_store_explicit(
                &context.first_observed, true, memory_order_release);

        struct task *child = task_create_(task);
        if (child == NULL) {
            atomic_store_explicit(&context.failure,
                    RACE_TASK_CREATE_FAILED, memory_order_release);
        } else {
            struct task_credentials inherited;
            task_credentials_snapshot(child, &inherited);
            if (!snapshot_matches(&context, &inherited, &first) &&
                    !snapshot_matches(&context, &inherited, &second)) {
                atomic_store_explicit(&context.failure,
                        RACE_MIXED_SNAPSHOT, memory_order_release);
            }
            task_abort_create(child);
            atomic_fetch_add_explicit(&context.inherit_observations,
                    1, memory_order_release);
        }
        sched_yield();
    }

    // done 只描述写线程已退出循环；最后再读取一次最终发布状态。
    struct task_credentials credentials;
    task_credentials_snapshot(task, &credentials);
    if (!snapshot_matches(&context, &credentials, &first) &&
            !snapshot_matches(&context, &credentials, &second)) {
        atomic_store_explicit(&context.failure,
                RACE_MIXED_SNAPSHOT, memory_order_release);
    }

    pthread_join(writer, NULL);
    return atomic_load_explicit(&context.failure, memory_order_acquire);
}

int main(void) {
    struct task task = {
        .uid = 100,
        .euid = 101,
        .suid = 102,
        .gid = 200,
        .egid = 201,
        .sgid = 202,
    };
    current = &task;

    struct task_credentials before;
    struct task_credentials after;
    task_credentials_snapshot(&task, &before);
    CHECK(sys_setresuid(100, 101, 999) == (dword_t) _EPERM,
            "非 root 的 setresuid 拒绝未知保存 UID");
    task_credentials_snapshot(&task, &after);
    CHECK(credentials_equal(&before, &after),
            "setresuid 拒绝路径不改变任一凭据");

    CHECK(sys_setresgid(200, 201, 999) == (dword_t) _EPERM,
            "非 root 的 setresgid 拒绝未知保存 GID");
    task_credentials_snapshot(&task, &after);
    CHECK(credentials_equal(&before, &after),
            "setresgid 拒绝路径不改变任一凭据");

    CHECK(sys_setuid(999) == _EPERM && sys_setgid(999) == _EPERM,
            "非 root 的 setuid 与 setgid 拒绝未知身份");
    task_credentials_snapshot(&task, &after);
    CHECK(credentials_equal(&before, &after),
            "setuid 与 setgid 拒绝路径不改变任一凭据");

    task = (struct task) {
        .uid = 10,
        .euid = 0,
        .suid = 11,
        .gid = 20,
        .egid = 21,
        .sgid = 22,
    };
    CHECK(sys_setuid(700) == 0,
            "root 的 setuid 成功提交身份");
    task_credentials_snapshot(&task, &after);
    CHECK(after.uid == 700 && after.euid == 700 && after.suid == 700 &&
            after.gid == 20 && after.egid == 21 && after.sgid == 22,
            "root 的 setuid 原子更新 UID 三元组且不影响 GID");

    task = (struct task) {
        .uid = 10,
        .euid = 0,
        .suid = 11,
        .gid = 20,
        .egid = 21,
        .sgid = 22,
    };
    CHECK(sys_setgid(800) == 0,
            "root 的 setgid 成功提交身份");
    task_credentials_snapshot(&task, &after);
    CHECK(after.gid == 800 && after.egid == 800 && after.sgid == 800 &&
            after.uid == 10 && after.euid == 0 && after.suid == 11,
            "root 的 setgid 原子更新 GID 三元组且不影响 UID");
    current = NULL;

    // UID 与 GID 分轮发布；Linux 并不保证两个 syscall 组成跨族事务。
    task = (struct task) {
        .uid = 1001,
        .euid = 0,
        .suid = 1002,
        .gid = 3001,
        .egid = 3002,
        .sgid = 3003,
    };
    CHECK(run_atomic_publish_test(&task, CREDENTIAL_UID,
            (struct credential_triple) {1001, 0, 1002},
            (struct credential_triple) {2001, 0, 2002}) == RACE_OK,
            "setresuid 并发发布时只出现完整 UID 三元组");

    task = (struct task) {
        .uid = 4001,
        .euid = 0,
        .suid = 4002,
        .gid = 5001,
        .egid = 5002,
        .sgid = 5003,
    };
    CHECK(run_atomic_publish_test(&task, CREDENTIAL_GID,
            (struct credential_triple) {5001, 5002, 5003},
            (struct credential_triple) {6001, 6002, 6003}) == RACE_OK,
            "setresgid 并发发布时只出现完整 GID 三元组");

    return 0;
}

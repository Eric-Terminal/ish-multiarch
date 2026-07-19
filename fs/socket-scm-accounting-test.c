#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_SOCKETPAIR UINT32_C(0x1000)
#define MAX_ACCOUNT_ACTORS 4

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "SCM inflight 记账测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        passed = false; \
        goto cleanup; \
    } \
} while (0)

struct account_actor {
    struct task task;
    struct tgroup group;
    fd_t probe_number;
    bool group_lock_initialized;
};

struct fixture {
    struct task io_task;
    struct tgroup io_group;
    struct account_actor actors[MAX_ACCOUNT_ACTORS];
    unsigned actor_count;
    bool io_locks_initialized;
};

struct received_message {
    ssize_t result;
    byte_t payload;
    dword_t flags;
    struct scm *scm;
};

static int probe_close(struct fd *fd) {
    (void) fd;
    return 0;
}

static const struct fd_ops probe_ops = {
    .close = probe_close,
};

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->io_task;
    if (fixture->io_task.files != NULL &&
            !IS_ERR(fixture->io_task.files)) {
        fdtable_release(fixture->io_task.files);
        fixture->io_task.files = NULL;
    }
    for (unsigned index = 0; index < fixture->actor_count; index++) {
        struct account_actor *actor = &fixture->actors[index];
        if (actor->task.files != NULL && !IS_ERR(actor->task.files)) {
            fdtable_release(actor->task.files);
            actor->task.files = NULL;
        }
    }
    if (fixture->io_task.mm != NULL) {
        mm_release(fixture->io_task.mm);
        fixture->io_task.mm = NULL;
    }
    for (unsigned index = 0; index < fixture->actor_count; index++) {
        struct account_actor *actor = &fixture->actors[index];
        if (actor->group_lock_initialized)
            pthread_mutex_destroy(&actor->group.lock.m);
    }
    if (fixture->io_locks_initialized) {
        pthread_mutex_destroy(&fixture->io_task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->io_group.lock.m);
    }
    current = NULL;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->io_group.lock);
    lock_init(&fixture->io_task.waiting_cond_lock);
    fixture->io_locks_initialized = true;
    list_init(&fixture->io_group.threads);
    fixture->io_group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {64, 64};
    fixture->io_task.group = &fixture->io_group;
    fixture->io_task.uid = 9000;
    fixture->io_task.euid = 9000;
    fixture->io_task.suid = 9000;
    fixture->io_task.waiting_poll_notify_fd = -1;
    fixture->io_task.files = fdtable_new(16);
    if (IS_ERR(fixture->io_task.files)) {
        fprintf(stderr, "SCM inflight 记账测试失败：创建 I/O fd 表\n");
        return false;
    }

    struct mm *memory = mm_new();
    if (memory == NULL) {
        fprintf(stderr, "SCM inflight 记账测试失败：创建用户地址空间\n");
        return false;
    }
    task_set_mm(&fixture->io_task, memory);
    write_wrlock(&fixture->io_task.mem->lock);
    int error = pt_map_nothing(
            fixture->io_task.mem, PAGE(USER_SOCKETPAIR), 1, P_RWX);
    write_wrunlock(&fixture->io_task.mem->lock);
    if (error < 0) {
        fprintf(stderr, "SCM inflight 记账测试失败：映射 socketpair 写回页\n");
        return false;
    }
    current = &fixture->io_task;
    return true;
}

static struct account_actor *fixture_add_actor(
        struct fixture *fixture, uid_t_ uid, uid_t_ euid) {
    if (fixture->actor_count == MAX_ACCOUNT_ACTORS)
        return NULL;
    struct account_actor *actor =
            &fixture->actors[fixture->actor_count++];
    memset(actor, 0, sizeof(*actor));
    actor->probe_number = -1;
    lock_init(&actor->group.lock);
    actor->group_lock_initialized = true;
    list_init(&actor->group.threads);
    actor->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {16, 16};
    actor->task.group = &actor->group;
    actor->task.uid = uid;
    actor->task.euid = euid;
    actor->task.suid = euid;
    actor->task.files = fdtable_new(4);
    if (IS_ERR(actor->task.files))
        return NULL;

    struct fd *probe = fd_create(&probe_ops);
    if (probe == NULL)
        return NULL;
    actor->probe_number = f_install_task(&actor->task, probe, 0);
    if (actor->probe_number < 0)
        return NULL;
    return actor;
}

static void actor_set_identity_and_limit(struct account_actor *actor,
        uid_t_ uid, uid_t_ euid, rlim_t_ nofile) {
    actor->task.uid = uid;
    actor->task.euid = euid;
    actor->task.suid = euid;
    actor->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {nofile, nofile};
}

static bool fixture_create_pair(struct fixture *fixture, fd_t pair[2]) {
    current = &fixture->io_task;
    return sys_socketpair(AF_LOCAL_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(fd_t) * 2) == 0;
}

static int create_repeated_rights(struct account_actor *actor,
        unsigned count, struct scm **scm) {
    fd_t numbers[SOCKET_SCM_MAX_FDS];
    for (unsigned index = 0; index < count; index++)
        numbers[index] = actor->probe_number;
    return socket_scm_create_task(&actor->task, numbers, count, scm);
}

static ssize_t send_rights(struct fixture *fixture, fd_t sender,
        byte_t payload, struct scm **scm) {
    struct socket_ref socket = {0};
    current = &fixture->io_task;
    int error = socket_ref_get_task(&fixture->io_task, sender, &socket);
    if (error < 0)
        return error;
    ssize_t result = socket_sendmsg_ref(&socket,
            &payload, sizeof(payload), MSG_DONTWAIT_ | MSG_NOSIGNAL_,
            NULL, scm);
    socket_ref_release(&socket);
    return result;
}

static struct received_message receive_rights(
        struct fixture *fixture, fd_t receiver, dword_t flags) {
    struct received_message message = {
        .result = _EIO,
        .flags = UINT32_MAX,
    };
    struct socket_ref socket = {0};
    current = &fixture->io_task;
    int error = socket_ref_get_task(
            &fixture->io_task, receiver, &socket);
    if (error < 0) {
        message.result = error;
        return message;
    }
    message.result = socket_recvmsg_ref(&socket,
            &message.payload, sizeof(message.payload),
            flags | MSG_DONTWAIT_, NULL, &message.flags, &message.scm);
    socket_ref_release(&socket);
    return message;
}

static bool receive_and_release(struct fixture *fixture, fd_t receiver,
        dword_t flags, byte_t payload, unsigned rights_count) {
    struct received_message message =
            receive_rights(fixture, receiver, flags);
    bool matched = message.result == 1 &&
            message.payload == payload && message.flags == 0 &&
            message.scm != NULL &&
            message.scm->num_fds == rights_count;
    if (message.scm != NULL)
        socket_scm_release(message.scm);
    if (!matched)
        fprintf(stderr,
                "SCM inflight 记账测试失败：接收的 payload 或 rights 数量不匹配\n");
    return matched;
}

static bool test_zero_limit_boundary(void) {
    bool passed = true;
    struct fixture fixture;
    struct scm *first = NULL;
    struct scm *second = NULL;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return false;
    }
    fd_t pair[2];
    struct account_actor *actor =
            fixture_add_actor(&fixture, 1001, 1001);
    REQUIRE(actor != NULL && fixture_create_pair(&fixture, pair),
            "创建零额度边界夹具");
    actor_set_identity_and_limit(actor, 1001, 1001, 0);

    REQUIRE(create_repeated_rights(actor, 1, &first) == 0 &&
                    send_rights(&fixture, pair[0], 'A', &first) == 1 &&
                    first == NULL,
            "RLIMIT_NOFILE 为零时允许第一包 rights");
    REQUIRE(create_repeated_rights(actor, 1, &second) == 0 &&
                    send_rights(&fixture, pair[0], 'B', &second) ==
                            _ETOOMANYREFS &&
                    second != NULL,
            "已有一项 inflight 后下一包返回 ETOOMANYREFS");

cleanup:
    if (first != NULL)
        socket_scm_release(first);
    if (second != NULL)
        socket_scm_release(second);
    fixture_destroy(&fixture);
    return passed;
}

static bool test_slots_are_counted_individually(void) {
    bool passed = true;
    struct fixture fixture;
    struct scm *two_slots = NULL;
    struct scm *third_slot = NULL;
    struct scm *over_limit = NULL;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return false;
    }
    fd_t pair[2];
    struct account_actor *actor =
            fixture_add_actor(&fixture, 1010, 1010);
    REQUIRE(actor != NULL && fixture_create_pair(&fixture, pair),
            "创建逐槽记账夹具");
    actor_set_identity_and_limit(actor, 1010, 1010, 2);

    REQUIRE(create_repeated_rights(actor, 2, &two_slots) == 0 &&
                    send_rights(&fixture, pair[0], 'C', &two_slots) == 1 &&
                    two_slots == NULL,
            "同一非 Unix fd 重复出现时按两个槽发布");
    REQUIRE(create_repeated_rights(actor, 1, &third_slot) == 0 &&
                    send_rights(&fixture, pair[0], 'D', &third_slot) == 1 &&
                    third_slot == NULL,
            "inflight 等于额度时仍允许下一槽");
    REQUIRE(create_repeated_rights(actor, 1, &over_limit) == 0 &&
                    send_rights(&fixture, pair[0], 'E', &over_limit) ==
                            _ETOOMANYREFS &&
                    over_limit != NULL,
            "重复槽与普通 fd 槽都进入额度总数");
    REQUIRE(receive_and_release(&fixture, pair[1], 0, 'C', 2) &&
                    receive_and_release(&fixture, pair[1], 0, 'D', 1),
            "逐槽记账消息可按原 rights 数量接收");

cleanup:
    if (two_slots != NULL)
        socket_scm_release(two_slots);
    if (third_slot != NULL)
        socket_scm_release(third_slot);
    if (over_limit != NULL)
        socket_scm_release(over_limit);
    fixture_destroy(&fixture);
    return passed;
}

static bool test_real_uid_accounting_scope(void) {
    bool passed = true;
    struct fixture fixture;
    struct scm *first = NULL;
    struct scm *same_uid = NULL;
    struct scm *different_uid = NULL;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return false;
    }
    fd_t pair[2];
    struct account_actor *first_actor =
            fixture_add_actor(&fixture, 1020, 1020);
    struct account_actor *same_actor =
            fixture_add_actor(&fixture, 1020, 1021);
    struct account_actor *different_actor =
            fixture_add_actor(&fixture, 1022, 1021);
    REQUIRE(first_actor != NULL && same_actor != NULL &&
                    different_actor != NULL &&
                    fixture_create_pair(&fixture, pair),
            "创建 real uid 聚合夹具");
    actor_set_identity_and_limit(first_actor, 1020, 1020, 0);
    actor_set_identity_and_limit(same_actor, 1020, 1021, 0);
    actor_set_identity_and_limit(different_actor, 1022, 1021, 0);

    REQUIRE(create_repeated_rights(first_actor, 1, &first) == 0 &&
                    send_rights(&fixture, pair[0], 'F', &first) == 1 &&
                    first == NULL,
            "第一个 real uid 发布一项 inflight");
    REQUIRE(create_repeated_rights(same_actor, 1, &same_uid) == 0 &&
                    send_rights(&fixture, pair[0], 'G', &same_uid) ==
                            _ETOOMANYREFS &&
                    same_uid != NULL,
            "不同 task 的相同 real uid 共享额度");
    REQUIRE(create_repeated_rights(
                    different_actor, 1, &different_uid) == 0 &&
                    send_rights(&fixture, pair[0], 'H',
                            &different_uid) == 1 &&
                    different_uid == NULL,
            "不同 real uid 的额度彼此隔离");
    REQUIRE(receive_and_release(&fixture, pair[1], 0, 'F', 1) &&
                    receive_and_release(&fixture, pair[1], 0, 'H', 1),
            "清理 real uid 聚合测试消息");

cleanup:
    if (first != NULL)
        socket_scm_release(first);
    if (same_uid != NULL)
        socket_scm_release(same_uid);
    if (different_uid != NULL)
        socket_scm_release(different_uid);
    fixture_destroy(&fixture);
    return passed;
}

static bool test_effective_root_exemption(void) {
    bool passed = true;
    struct fixture fixture;
    struct scm *exempt_first = NULL;
    struct scm *exempt_second = NULL;
    struct scm *counted = NULL;
    struct scm *real_root_first = NULL;
    struct scm *real_root_second = NULL;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return false;
    }
    fd_t pair[2];
    struct account_actor *effective_root =
            fixture_add_actor(&fixture, 1030, 0);
    struct account_actor *same_real_uid =
            fixture_add_actor(&fixture, 1030, 1030);
    struct account_actor *real_root_only =
            fixture_add_actor(&fixture, 0, 1031);
    REQUIRE(effective_root != NULL && same_real_uid != NULL &&
                    real_root_only != NULL &&
                    fixture_create_pair(&fixture, pair),
            "创建有效 root 豁免夹具");
    actor_set_identity_and_limit(effective_root, 1030, 0, 0);
    actor_set_identity_and_limit(same_real_uid, 1030, 1030, 0);
    actor_set_identity_and_limit(real_root_only, 0, 1031, 0);

    REQUIRE(create_repeated_rights(
                    effective_root, 1, &exempt_first) == 0 &&
                    send_rights(&fixture, pair[0], 'I',
                            &exempt_first) == 1 &&
                    exempt_first == NULL &&
                    create_repeated_rights(
                            effective_root, 1, &exempt_second) == 0 &&
                    send_rights(&fixture, pair[0], 'J',
                            &exempt_second) == 1 &&
                    exempt_second == NULL,
            "euid 为零时可以越过 inflight 限额");
    REQUIRE(create_repeated_rights(same_real_uid, 1, &counted) == 0 &&
                    send_rights(&fixture, pair[0], 'K', &counted) ==
                            _ETOOMANYREFS &&
                    counted != NULL,
            "豁免发送仍计入相同 real uid 的 inflight");
    REQUIRE(create_repeated_rights(
                    real_root_only, 1, &real_root_first) == 0 &&
                    send_rights(&fixture, pair[0], 'L',
                            &real_root_first) == 1 &&
                    real_root_first == NULL &&
                    create_repeated_rights(
                            real_root_only, 1, &real_root_second) == 0 &&
                    send_rights(&fixture, pair[0], 'M',
                            &real_root_second) == _ETOOMANYREFS &&
                    real_root_second != NULL,
            "real uid 为零但 euid 非零时不享受豁免");
    REQUIRE(receive_and_release(&fixture, pair[1], 0, 'I', 1) &&
                    receive_and_release(&fixture, pair[1], 0, 'J', 1) &&
                    receive_and_release(&fixture, pair[1], 0, 'L', 1),
            "清理有效 root 豁免测试消息");

cleanup:
    if (exempt_first != NULL)
        socket_scm_release(exempt_first);
    if (exempt_second != NULL)
        socket_scm_release(exempt_second);
    if (counted != NULL)
        socket_scm_release(counted);
    if (real_root_first != NULL)
        socket_scm_release(real_root_first);
    if (real_root_second != NULL)
        socket_scm_release(real_root_second);
    fixture_destroy(&fixture);
    return passed;
}

static bool test_receive_and_close_unaccount(void) {
    bool passed = true;
    struct fixture fixture;
    struct scm *received_first = NULL;
    struct scm *received_retry = NULL;
    struct scm *closed_first = NULL;
    struct scm *closed_retry = NULL;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return false;
    }
    fd_t receive_pair[2];
    fd_t close_pair[2];
    fd_t retry_pair[2];
    struct account_actor *actor =
            fixture_add_actor(&fixture, 1040, 1040);
    REQUIRE(actor != NULL &&
                    fixture_create_pair(&fixture, receive_pair) &&
                    fixture_create_pair(&fixture, close_pair) &&
                    fixture_create_pair(&fixture, retry_pair),
            "创建接收与关闭解账夹具");
    actor_set_identity_and_limit(actor, 1040, 1040, 0);

    REQUIRE(create_repeated_rights(actor, 1, &received_first) == 0 &&
                    send_rights(&fixture, receive_pair[0], 'N',
                            &received_first) == 1 &&
                    received_first == NULL &&
                    create_repeated_rights(actor, 1, &received_retry) == 0 &&
                    send_rights(&fixture, receive_pair[0], 'O',
                            &received_retry) == _ETOOMANYREFS,
            "正式接收前额度保持占用");
    REQUIRE(receive_and_release(
                    &fixture, receive_pair[1], 0, 'N', 1) &&
                    send_rights(&fixture, receive_pair[0], 'O',
                            &received_retry) == 1 &&
                    received_retry == NULL &&
                    receive_and_release(
                            &fixture, receive_pair[1], 0, 'O', 1),
            "正式接收后立即解账并允许再次发送");

    REQUIRE(create_repeated_rights(actor, 1, &closed_first) == 0 &&
                    send_rights(&fixture, close_pair[0], 'P',
                            &closed_first) == 1 &&
                    closed_first == NULL &&
                    create_repeated_rights(actor, 1, &closed_retry) == 0 &&
                    send_rights(&fixture, retry_pair[0], 'Q',
                            &closed_retry) == _ETOOMANYREFS,
            "关闭接收端前额度保持占用");
    REQUIRE(f_close_task(&fixture.io_task, close_pair[1]) == 0,
            "关闭含未接收 rights 的接收端");
    close_pair[1] = -1;
    REQUIRE(send_rights(&fixture, retry_pair[0], 'Q',
                    &closed_retry) == 1 &&
                    closed_retry == NULL &&
                    receive_and_release(
                            &fixture, retry_pair[1], 0, 'Q', 1),
            "接收端关闭后解账并允许其他 socket 再次发送");

cleanup:
    if (received_first != NULL)
        socket_scm_release(received_first);
    if (received_retry != NULL)
        socket_scm_release(received_retry);
    if (closed_first != NULL)
        socket_scm_release(closed_first);
    if (closed_retry != NULL)
        socket_scm_release(closed_retry);
    fixture_destroy(&fixture);
    return passed;
}

static bool test_peek_keeps_accounting(void) {
    bool passed = true;
    struct fixture fixture;
    struct scm *first = NULL;
    struct scm *retry = NULL;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return false;
    }
    fd_t pair[2];
    struct account_actor *actor =
            fixture_add_actor(&fixture, 1050, 1050);
    REQUIRE(actor != NULL && fixture_create_pair(&fixture, pair),
            "创建 MSG_PEEK 记账夹具");
    actor_set_identity_and_limit(actor, 1050, 1050, 0);

    REQUIRE(create_repeated_rights(actor, 1, &first) == 0 &&
                    send_rights(&fixture, pair[0], 'R', &first) == 1 &&
                    first == NULL &&
                    receive_and_release(
                            &fixture, pair[1], MSG_PEEK_, 'R', 1),
            "MSG_PEEK 克隆 rights 但不消费原消息");
    REQUIRE(create_repeated_rights(actor, 1, &retry) == 0 &&
                    send_rights(&fixture, pair[0], 'S', &retry) ==
                            _ETOOMANYREFS &&
                    retry != NULL,
            "MSG_PEEK 后原 inflight 额度仍被占用");
    REQUIRE(receive_and_release(&fixture, pair[1], 0, 'R', 1) &&
                    send_rights(&fixture, pair[0], 'S', &retry) == 1 &&
                    retry == NULL &&
                    receive_and_release(&fixture, pair[1], 0, 'S', 1),
            "正式接收后解除 MSG_PEEK 保留的记账");

cleanup:
    if (first != NULL)
        socket_scm_release(first);
    if (retry != NULL)
        socket_scm_release(retry);
    fixture_destroy(&fixture);
    return passed;
}

int main(void) {
    bool passed = true;
    passed = test_zero_limit_boundary() && passed;
    passed = test_slots_are_counted_individually() && passed;
    passed = test_real_uid_accounting_scope() && passed;
    passed = test_effective_root_exemption() && passed;
    passed = test_receive_and_close_unaccount() && passed;
    passed = test_peek_keeps_accounting() && passed;
    return passed ? 0 : 1;
}

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_SOCKETPAIR UINT32_C(0x1000)
#define MAX_NODES 6

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "SCM 环回收测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        passed = false; \
        goto cleanup; \
    } \
} while (0)

struct cycle_node {
    fd_t sender_number;
    fd_t receiver_number;
    struct fd *receiver_root;
    int receiver_host_fd;
};

struct fixture {
    struct task task;
    struct tgroup group;
    struct cycle_node nodes[MAX_NODES];
    unsigned node_count;
    bool locks_initialized;
};

struct close_probe {
    unsigned closes;
};

static int probe_close(struct fd *fd) {
    struct close_probe *probe = fd->data;
    probe->closes++;
    return 0;
}

static const struct fd_ops probe_ops = {
    .close = probe_close,
};

static void node_drop_root(struct cycle_node *node) {
    if (node->receiver_root != NULL) {
        fd_close(node->receiver_root);
        node->receiver_root = NULL;
    }
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    for (unsigned index = 0; index < fixture->node_count; index++)
        node_drop_root(&fixture->nodes[index]);
    if (fixture->task.files != NULL &&
            !IS_ERR(fixture->task.files)) {
        fdtable_release(fixture->task.files);
        fixture->task.files = NULL;
    }
    socket_scm_collect_now();
    if (fixture->task.mm != NULL) {
        mm_release(fixture->task.mm);
        fixture->task.mm = NULL;
    }
    if (fixture->locks_initialized) {
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
    }
    current = NULL;
}

static bool fixture_init(struct fixture *fixture, uid_t_ uid) {
    memset(fixture, 0, sizeof(*fixture));
    for (unsigned index = 0; index < MAX_NODES; index++) {
        fixture->nodes[index].sender_number = -1;
        fixture->nodes[index].receiver_number = -1;
        fixture->nodes[index].receiver_host_fd = -1;
    }

    lock_init(&fixture->group.lock);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->locks_initialized = true;
    list_init(&fixture->group.threads);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {512, 512};
    fixture->task.group = &fixture->group;
    fixture->task.uid = uid;
    fixture->task.euid = uid;
    fixture->task.suid = uid;
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->task.files = fdtable_new(32);
    if (IS_ERR(fixture->task.files)) {
        fprintf(stderr, "SCM 环回收测试失败：创建 fd 表\n");
        return false;
    }

    struct mm *memory = mm_new();
    if (memory == NULL) {
        fprintf(stderr, "SCM 环回收测试失败：创建用户地址空间\n");
        return false;
    }
    task_set_mm(&fixture->task, memory);
    write_wrlock(&fixture->task.mem->lock);
    int error = pt_map_nothing(
            fixture->task.mem, PAGE(USER_SOCKETPAIR), 1, P_RWX);
    write_wrunlock(&fixture->task.mem->lock);
    if (error < 0) {
        fprintf(stderr, "SCM 环回收测试失败：映射 socketpair 写回页\n");
        return false;
    }
    current = &fixture->task;
    return true;
}

static struct cycle_node *fixture_add_node(
        struct fixture *fixture, dword_t socket_type) {
    if (fixture->node_count == MAX_NODES)
        return NULL;
    struct cycle_node *node =
            &fixture->nodes[fixture->node_count++];
    fd_t pair[2];
    current = &fixture->task;
    if (sys_socketpair(AF_LOCAL_, socket_type | SOCK_NONBLOCK_, 0,
                    USER_SOCKETPAIR) < 0 ||
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) != 0)
        return NULL;
    node->sender_number = pair[0];
    node->receiver_number = pair[1];
    node->receiver_root = f_get_task_retain(
            &fixture->task, node->receiver_number);
    if (node->receiver_root == NULL)
        return NULL;
    node->receiver_host_fd = node->receiver_root->real_fd;
    return node;
}

static bool send_rights(struct fixture *fixture,
        const struct cycle_node *source,
        const fd_t *numbers, unsigned count, byte_t payload) {
    struct scm *scm = NULL;
    int error = socket_scm_create_task(&fixture->task,
            numbers, count, &scm);
    if (error < 0)
        return false;

    struct socket_ref sender = {0};
    error = socket_ref_get_task(
            &fixture->task, source->sender_number, &sender);
    if (error < 0) {
        socket_scm_release(scm);
        return false;
    }
    ssize_t result = socket_sendmsg_ref(&sender,
            &payload, sizeof(payload),
            MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL, &scm);
    socket_ref_release(&sender);
    if (scm != NULL)
        socket_scm_release(scm);
    return result == (ssize_t) sizeof(payload) && scm == NULL;
}

static bool send_edge(struct fixture *fixture,
        const struct cycle_node *source,
        const struct cycle_node *target, byte_t payload) {
    return send_rights(fixture, source,
            &target->receiver_number, 1, payload);
}

static bool close_guest_number(
        struct fixture *fixture, fd_t *number) {
    if (*number < 0)
        return true;
    int error = f_close_task(&fixture->task, *number);
    *number = -1;
    return error == 0;
}

static bool close_all_guest_references(struct fixture *fixture) {
    bool closed = true;
    for (unsigned index = 0; index < fixture->node_count; index++) {
        if (!close_guest_number(fixture,
                &fixture->nodes[index].sender_number))
            closed = false;
    }
    for (unsigned index = 0; index < fixture->node_count; index++) {
        if (!close_guest_number(fixture,
                &fixture->nodes[index].receiver_number))
            closed = false;
    }
    return closed;
}

static void drop_all_roots(struct fixture *fixture) {
    for (unsigned index = 0; index < fixture->node_count; index++)
        node_drop_root(&fixture->nodes[index]);
}

static bool host_fd_is_open(int host_fd) {
    return host_fd >= 0 && fcntl(host_fd, F_GETFD) >= 0;
}

static bool host_fd_is_closed(int host_fd) {
    if (host_fd < 0)
        return false;
    errno = 0;
    return fcntl(host_fd, F_GETFD) < 0 && errno == EBADF;
}

static bool all_receiver_host_fds_closed(
        const struct fixture *fixture) {
    for (unsigned index = 0; index < fixture->node_count; index++) {
        if (!host_fd_is_closed(
                fixture->nodes[index].receiver_host_fd))
            return false;
    }
    return true;
}

static bool all_receiver_host_fds_open(
        const struct fixture *fixture) {
    for (unsigned index = 0; index < fixture->node_count; index++) {
        if (!host_fd_is_open(
                fixture->nodes[index].receiver_host_fd))
            return false;
    }
    return true;
}

static bool test_self_cycle(dword_t socket_type, uid_t_ uid) {
    bool passed = true;
    struct fixture fixture;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *node =
            fixture_add_node(&fixture, socket_type);
    REQUIRE(node != NULL, "创建自环 socketpair");
    REQUIRE(send_edge(&fixture, node, node, 'A'),
            "发送指向接收端自身的 SCM_RIGHTS");
    REQUIRE(socket_scm_inflight_count(uid) == 1,
            "自环发布后 inflight 恰好增加一个槽");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭自环的 guest 描述符");
    drop_all_roots(&fixture);
    REQUIRE(socket_scm_inflight_count(uid) == 1,
            "安全点收集前自环仍由消息边持有");

    socket_scm_collect_checkpoint();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "最后外部根下降会请求安全点解除 inflight 记账");
    REQUIRE(all_receiver_host_fds_closed(&fixture),
            "安全点析构自环接收 socket");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool test_two_node_cycle(dword_t socket_type, uid_t_ uid) {
    bool passed = true;
    struct fixture fixture;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *left =
            fixture_add_node(&fixture, socket_type);
    struct cycle_node *right =
            fixture_add_node(&fixture, socket_type);
    REQUIRE(left != NULL && right != NULL, "创建双节点环");
    REQUIRE(send_edge(&fixture, left, right, 'B') &&
                    send_edge(&fixture, right, left, 'C'),
            "发布双节点环的两条消息边");
    REQUIRE(socket_scm_inflight_count(uid) == 2,
            "双节点环包含两个 inflight 槽");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭双节点环的 guest 描述符");
    drop_all_roots(&fixture);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "收集双节点环的全部消息边");
    REQUIRE(all_receiver_host_fds_closed(&fixture),
            "双节点环的两个接收 socket 均被析构");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool test_three_node_cycle_with_repeated_edge(void) {
    const uid_t_ uid = 6103;
    bool passed = true;
    struct fixture fixture;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *first =
            fixture_add_node(&fixture, SOCK_DGRAM_);
    struct cycle_node *second =
            fixture_add_node(&fixture, SOCK_DGRAM_);
    struct cycle_node *third =
            fixture_add_node(&fixture, SOCK_DGRAM_);
    REQUIRE(first != NULL && second != NULL && third != NULL,
            "创建带重复边的三节点环");
    REQUIRE(send_edge(&fixture, first, second, 'D') &&
                    send_edge(&fixture, first, second, 'E') &&
                    send_edge(&fixture, second, third, 'F') &&
                    send_edge(&fixture, third, first, 'G'),
            "发布三节点环及重复消息边");
    REQUIRE(socket_scm_inflight_count(uid) == 4,
            "重复边按独立 SCM 槽进入 inflight");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭三节点环的 guest 描述符");
    drop_all_roots(&fixture);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "一次收集清除三节点环及重复边");
    REQUIRE(all_receiver_host_fds_closed(&fixture),
            "三节点环的全部接收 socket 均被析构");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool test_two_disjoint_cycles(void) {
    const uid_t_ uid = 6107;
    bool passed = true;
    struct fixture fixture;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *first = fixture_add_node(&fixture, SOCK_STREAM_);
    struct cycle_node *second = fixture_add_node(&fixture, SOCK_STREAM_);
    struct cycle_node *third = fixture_add_node(&fixture, SOCK_DGRAM_);
    struct cycle_node *fourth = fixture_add_node(&fixture, SOCK_DGRAM_);
    REQUIRE(first != NULL && second != NULL &&
                    third != NULL && fourth != NULL,
            "创建两个互不相交的消息环");
    REQUIRE(send_edge(&fixture, first, second, 'K') &&
                    send_edge(&fixture, second, first, 'L') &&
                    send_edge(&fixture, third, fourth, 'M') &&
                    send_edge(&fixture, fourth, third, 'N'),
            "发布两个互不相交环的四条边");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭两个不相交环的 guest 描述符");
    drop_all_roots(&fixture);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "一次收集清除两个互不相交环");
    REQUIRE(all_receiver_host_fds_closed(&fixture),
            "两个不相交环的全部 socket 均被析构");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool test_external_root_propagates_through_chain(void) {
    const uid_t_ uid = 6108;
    bool passed = true;
    struct fixture fixture;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *root = fixture_add_node(&fixture, SOCK_STREAM_);
    struct cycle_node *middle = fixture_add_node(&fixture, SOCK_STREAM_);
    struct cycle_node *left = fixture_add_node(&fixture, SOCK_STREAM_);
    struct cycle_node *right = fixture_add_node(&fixture, SOCK_STREAM_);
    REQUIRE(root != NULL && middle != NULL &&
                    left != NULL && right != NULL,
            "创建外部根、链与下游环");
    REQUIRE(send_edge(&fixture, root, middle, 'O') &&
                    send_edge(&fixture, middle, left, 'P') &&
                    send_edge(&fixture, left, right, 'Q') &&
                    send_edge(&fixture, right, left, 'R'),
            "发布根到链及下游环的可达边");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭根传播拓扑的 guest 描述符");
    node_drop_root(middle);
    node_drop_root(left);
    node_drop_root(right);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 4 &&
                    all_receiver_host_fds_open(&fixture),
            "外部根经多跳链保活下游消息环");
    node_drop_root(root);
    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0 &&
                    all_receiver_host_fds_closed(&fixture),
            "释放链首外部根后回收整张不可达子图");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool test_mixed_rights_container_is_released_once(void) {
    const uid_t_ uid = 6109;
    bool passed = true;
    struct fixture fixture;
    struct close_probe probe = {0};
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *node = fixture_add_node(&fixture, SOCK_DGRAM_);
    REQUIRE(node != NULL, "创建混合 rights 自环");
    struct fd *ordinary = fd_create(&probe_ops);
    REQUIRE(ordinary != NULL, "创建混合 rights 普通 fd");
    ordinary->data = &probe;
    fd_t ordinary_number = f_install_task(
            &fixture.task, ordinary, 0);
    REQUIRE(ordinary_number >= 0, "安装混合 rights 普通 fd");
    fd_t numbers[] = {
        node->receiver_number,
        ordinary_number,
        node->receiver_number,
    };
    REQUIRE(send_rights(&fixture, node, numbers,
                    array_size(numbers), 'S'),
            "在同一 SCM 容器发送重复 Unix fd 与普通 fd");
    REQUIRE(socket_scm_inflight_count(uid) == array_size(numbers),
            "混合容器按三个 fd 槽计入 inflight");
    REQUIRE(f_close_task(&fixture.task, ordinary_number) == 0 &&
                    probe.closes == 0,
            "普通 fd 的 guest 根释放后仍由混合容器保活");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭混合容器自环的 guest socket 描述符");
    drop_all_roots(&fixture);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "回收混合容器时逐槽清除 inflight");
    REQUIRE(probe.closes == 1 &&
                    all_receiver_host_fds_closed(&fixture),
            "混合容器只析构一次普通 fd 并释放重复 Unix 引用");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool test_external_root_preserves_cycle(void) {
    const uid_t_ uid = 6104;
    bool passed = true;
    struct fixture fixture;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *rooted =
            fixture_add_node(&fixture, SOCK_STREAM_);
    struct cycle_node *dependent =
            fixture_add_node(&fixture, SOCK_STREAM_);
    REQUIRE(rooted != NULL && dependent != NULL,
            "创建带外部根的双节点环");
    REQUIRE(send_edge(&fixture, rooted, dependent, 'H') &&
                    send_edge(&fixture, dependent, rooted, 'I'),
            "发布带外部根的双节点环");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭带外部根环的 guest 描述符");
    node_drop_root(dependent);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 2,
            "可达外部根保留整个消息环");
    REQUIRE(host_fd_is_open(rooted->receiver_host_fd) &&
                    host_fd_is_open(dependent->receiver_host_fd),
            "外部根保活环内全部接收 socket");

    node_drop_root(rooted);
    REQUIRE(socket_scm_inflight_count(uid) == 2,
            "释放外部根尚未隐式摘除消息边");
    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "释放外部根后回收整个消息环");
    REQUIRE(all_receiver_host_fds_closed(&fixture),
            "释放外部根后析构环内全部接收 socket");

cleanup:
    fixture_destroy(&fixture);
    return passed;
}

static bool peek_rights(struct fixture *fixture,
        const struct cycle_node *node, struct scm **clone) {
    struct socket_ref receiver = {0};
    int error = socket_ref_get_task(
            &fixture->task, node->receiver_number, &receiver);
    if (error < 0)
        return false;
    byte_t payload = 0;
    dword_t message_flags = UINT32_MAX;
    ssize_t result = socket_recvmsg_ref(&receiver,
            &payload, sizeof(payload), MSG_PEEK_ | MSG_DONTWAIT_,
            NULL, &message_flags, clone);
    socket_ref_release(&receiver);
    return result == 1 && payload == 'J' && message_flags == 0 &&
            *clone != NULL && (*clone)->num_fds == 1;
}

static bool test_peek_clone_is_temporary_root(void) {
    const uid_t_ uid = 6105;
    bool passed = true;
    struct fixture fixture;
    struct scm *clone = NULL;
    if (!fixture_init(&fixture, uid)) {
        fixture_destroy(&fixture);
        return false;
    }
    struct cycle_node *node =
            fixture_add_node(&fixture, SOCK_STREAM_);
    REQUIRE(node != NULL, "创建 MSG_PEEK 自环");
    REQUIRE(send_edge(&fixture, node, node, 'J'),
            "向 MSG_PEEK 自环发布 rights");
    REQUIRE(peek_rights(&fixture, node, &clone),
            "MSG_PEEK 返回独立 rights 克隆");
    REQUIRE(close_all_guest_references(&fixture),
            "关闭 MSG_PEEK 自环的 guest 描述符");
    drop_all_roots(&fixture);

    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 1,
            "未释放的 MSG_PEEK 克隆作为外部根保留原消息");
    REQUIRE(host_fd_is_open(node->receiver_host_fd),
            "MSG_PEEK 克隆保活接收 socket");

    socket_scm_release(clone);
    clone = NULL;
    socket_scm_collect_now();
    REQUIRE(socket_scm_inflight_count(uid) == 0,
            "释放 MSG_PEEK 克隆后回收自环");
    REQUIRE(all_receiver_host_fds_closed(&fixture),
            "释放 MSG_PEEK 克隆后析构接收 socket");

cleanup:
    if (clone != NULL)
        socket_scm_release(clone);
    fixture_destroy(&fixture);
    return passed;
}

int main(void) {
    bool passed = true;
    passed = test_self_cycle(SOCK_STREAM_, 6100) && passed;
    passed = test_self_cycle(SOCK_DGRAM_, 6101) && passed;
    passed = test_two_node_cycle(SOCK_STREAM_, 6102) && passed;
    passed = test_two_node_cycle(SOCK_DGRAM_, 6106) && passed;
    passed = test_three_node_cycle_with_repeated_edge() && passed;
    passed = test_two_disjoint_cycles() && passed;
    passed = test_external_root_propagates_through_chain() && passed;
    passed = test_mixed_rights_container_is_released_once() && passed;
    passed = test_external_root_preserves_cycle() && passed;
    passed = test_peek_clone_is_temporary_root() && passed;
    return passed ? 0 : 1;
}

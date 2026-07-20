#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define TEST_TIMEOUT_SECONDS 30
#define USER_PAGE UINT32_C(0x1000)
#define USER_SOCKETPAIR (USER_PAGE + UINT32_C(0x20))
#define USER_PAYLOAD (USER_PAGE + UINT32_C(0x40))
#define USER_RECEIVE (USER_PAGE + UINT32_C(0x60))
#define USER_IOV (USER_PAGE + UINT32_C(0x80))
#define USER_HEADER (USER_PAGE + UINT32_C(0xa0))
#define USER_ADDRESS (USER_PAGE + UINT32_C(0xe0))
#define USER_RETURNED_ADDRESS (USER_PAGE + UINT32_C(0x180))
#define UNMAPPED_ADDRESS UINT32_C(0x3000)
#define PEER_EVENT_TOKEN UINT64_C(0x7365717061636b65)
#define SEQPACKET_STATE_EVENTS (POLL_READ | POLL_ERR | POLL_HUP)

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define THREAD_WAIT_MS 10000
#else
#define THREAD_WAIT_MS 3000
#endif

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "SEQPACKET socketpair 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        passed = false; \
        goto cleanup; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
    bool group_lock_initialized;
    bool waiting_lock_initialized;
    bool ptrace_lock_initialized;
};

struct poll_call {
    struct fixture *fixture;
    struct poll *poll;
    atomic_bool finished;
    int result;
    int event_types;
    uint64_t event_token;
};

struct scan_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool entered;
    bool released;
};

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] =
            "SEQPACKET socketpair 测试失败：超过硬超时\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static int open_host_fds(void) {
    int count = 0;
    int limit = getdtablesize();
    for (int number = 0; number < limit; number++) {
        errno = 0;
        if (fcntl(number, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group_lock_initialized = true;
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {16, 16};

    lock_init(&fixture->task.waiting_cond_lock);
    fixture->waiting_lock_initialized = true;
    fixture->task.waiting_poll_notify_fd = -1;
    lock_init(&fixture->task.ptrace.lock);
    fixture->ptrace_lock_initialized = true;
    list_init(&fixture->task.queue);
    fixture->task.pid = 7301;
    fixture->task.uid = 1000;
    fixture->task.gid = 1000;
    fixture->task.euid = 1000;
    fixture->task.egid = 1000;
    fixture->task.group = &fixture->group;
    fixture->task.sighand = sighand_new();
    if (fixture->task.sighand == NULL)
        return false;
    fixture->task.files = fdtable_new(8);
    if (IS_ERR(fixture->task.files)) {
        fixture->task.files = NULL;
        return false;
    }

    struct mm *memory = mm_new();
    if (memory == NULL)
        return false;
    task_set_mm(&fixture->task, memory);
    write_wrlock(&fixture->task.mem->lock);
    int map_error = pt_map_nothing(
            fixture->task.mem, PAGE(USER_PAGE), 1, P_RWX);
    write_wrunlock(&fixture->task.mem->lock);
    if (map_error < 0)
        return false;

    current = &fixture->task;
    task_thread_store(&fixture->task, pthread_self());
    return true;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL)
        fdtable_release(fixture->task.files);
    if (fixture->task.mm != NULL)
        mm_release(fixture->task.mm);
    if (fixture->task.sighand != NULL)
        sighand_release(fixture->task.sighand);
    if (fixture->ptrace_lock_initialized)
        pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
    if (fixture->waiting_lock_initialized)
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    if (fixture->group_lock_initialized)
        pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static bool wait_for_flag(atomic_bool *flag) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < THREAD_WAIT_MS; elapsed++) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(flag, memory_order_acquire);
}

static bool create_guest_pair(
        dword_t protocol, fd_t pair[2]) {
    return sys_socketpair(AF_LOCAL_, SOCK_SEQPACKET_,
                    protocol, USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(fd_t) * 2) == 0;
}

static ssize_t seqpacket_send(
        struct fd *fd, const void *data, size_t length) {
    const struct socket_ref socket = {.fd = fd};
    return socket_sendto_ref(
            &socket, data, length, MSG_NOSIGNAL_, NULL);
}

static ssize_t seqpacket_recv(
        struct fd *fd, void *data, size_t length, dword_t flags) {
    const struct socket_ref socket = {.fd = fd};
    return socket_recvfrom_ref(
            &socket, data, length, flags | MSG_DONTWAIT_, NULL);
}

static bool seqpacket_take_error(
        struct fd *fd, sdword_t *error) {
    const struct socket_ref socket = {.fd = fd};
    struct socket_option_result result;
    if (socket_getsockopt_ref(&socket,
            SOL_SOCKET_, SO_ERROR_, sizeof(*error),
            SOCKET_GUEST_I386, &result) != 0 ||
            result.length != sizeof(*error))
        return false;
    memcpy(error, result.value, sizeof(*error));
    return true;
}

static bool seqpacket_close_fd(struct fd **fd_pointer) {
    if (*fd_pointer == NULL)
        return true;
    struct fd *fd = *fd_pointer;
    *fd_pointer = NULL;
    return fd_close(fd) == 0;
}

static bool seqpacket_close_pair(struct fd *pair[2]) {
    bool first = seqpacket_close_fd(&pair[0]);
    bool second = seqpacket_close_fd(&pair[1]);
    return first && second;
}

static bool test_seqpacket_pair(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};
    struct scm *scm = NULL;

    REQUIRE(socket_pair_create_task(task,
                    AF_LOCAL_, SOCK_SEQPACKET_, PF_LOCAL_, pair) == 0 &&
                    pair[0] != NULL && pair[1] != NULL,
            "创建本地有序记录对");
    REQUIRE(pair[0]->socket.type == SOCK_SEQPACKET_ &&
                    pair[1]->socket.type == SOCK_SEQPACKET_,
            "两端保留 Linux guest 类型");
    REQUIRE(pair[0]->socket.protocol == 0 &&
                    pair[1]->socket.protocol == 0 &&
                    pair[0]->socket.guest_protocol == 0 &&
                    pair[1]->socket.guest_protocol == 0,
            "PF_LOCAL 输入按 Linux 语义归一化为协议零");

    static const byte_t first_record[] = {0x11, 0x12, 0x13};
    static const byte_t second_record[] = {0x21, 0x22, 0x23, 0x24};
    REQUIRE(pair[0]->ops->write(pair[0],
                    first_record, sizeof(first_record)) ==
                    (ssize_t) sizeof(first_record) &&
                    pair[0]->ops->write(pair[0],
                    second_record, sizeof(second_record)) ==
                    (ssize_t) sizeof(second_record),
            "分别发送两个记录");

    struct socket_ref receiver = {.fd = pair[1]};
    byte_t short_record[2] = {0};
    dword_t message_flags = 0;
    REQUIRE(socket_recvmsg_ref(&receiver,
                    short_record, sizeof(short_record), 0,
                    NULL, &message_flags, &scm) ==
                    (ssize_t) sizeof(short_record) &&
                    scm == NULL && (message_flags & MSG_TRUNC_) != 0 &&
                    memcmp(short_record, first_record,
                    sizeof(short_record)) == 0,
            "短缓冲截断当前记录并报告 MSG_TRUNC");

    byte_t complete_record[sizeof(second_record)] = {0};
    REQUIRE(pair[1]->ops->read(pair[1],
                    complete_record, sizeof(complete_record)) ==
                    (ssize_t) sizeof(complete_record) &&
                    memcmp(complete_record, second_record,
                    sizeof(complete_record)) == 0,
            "截断不会吞掉或拼接下一条记录");

    const byte_t final_record = 0x31;
    REQUIRE(seqpacket_send(pair[0],
                    &final_record, sizeof(final_record)) ==
                    (ssize_t) sizeof(final_record),
            "干净关闭前为存活端保留一条已发送记录");
    REQUIRE(seqpacket_close_fd(&pair[0]),
            "关闭发送端");
    int events = pair[1]->ops->poll(pair[1]);
    int repeated_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((events & SEQPACKET_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP) &&
                    (repeated_events & SEQPACKET_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP),
            "干净关闭持续呈现可读与挂断但不报告错误");

    sdword_t socket_error = -1;
    REQUIRE(seqpacket_take_error(pair[1], &socket_error) &&
                    socket_error == 0 &&
                    seqpacket_take_error(pair[1], &socket_error) &&
                    socket_error == 0,
            "干净关闭不会泄漏 host reset 到 SO_ERROR");

    byte_t received_final_record = 0;
    REQUIRE(seqpacket_recv(pair[1], &received_final_record,
                    sizeof(received_final_record), 0) ==
                    (ssize_t) sizeof(received_final_record) &&
                    received_final_record == final_record,
            "干净关闭保留存活端已经排队的记录");

    byte_t eof = 0;
    REQUIRE(pair[1]->ops->read(pair[1], &eof, sizeof(eof)) == 0 &&
                    pair[1]->ops->read(pair[1], &eof, sizeof(eof)) == 0,
            "记录排空后重复读取稳定返回 EOF");
    struct socket_address eof_address = {
        .length = sizeof(eof_address.storage),
    };
    REQUIRE(socket_recvfrom_ref(&receiver, &eof, sizeof(eof),
                    MSG_DONTWAIT_, &eof_address) == 0 &&
                    eof_address.length == 0,
            "EOF 不会伪造来源地址长度");
    REQUIRE(socket_sendmsg_ref(&receiver,
                    &eof, sizeof(eof), MSG_NOSIGNAL_, NULL, NULL) == _EPIPE,
            "对端关闭后的写入返回 EPIPE");
    REQUIRE(seqpacket_take_error(pair[1], &socket_error) &&
                    socket_error == 0,
            "干净关闭后的 EPIPE 不会写入 SO_ERROR");

cleanup:
    if (scm != NULL)
        socket_scm_release(scm);
    seqpacket_close_pair(pair);
    return passed;
}

static bool test_seqpacket_shutdown_semantics(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};
    sdword_t socket_error = -1;

    REQUIRE(socket_pair_create_task(task,
                    AF_LOCAL_, SOCK_SEQPACKET_, 0, pair) == 0,
            "为 SHUT_RD 创建有序记录对");
    const byte_t read_queued = 0x41;
    REQUIRE(seqpacket_send(pair[0],
                    &read_queued, sizeof(read_queued)) ==
                    (ssize_t) sizeof(read_queued),
            "SHUT_RD 前排入接收记录");
    const struct socket_ref read_shutdown = {.fd = pair[1]};
    REQUIRE(socket_shutdown_ref(&read_shutdown, SHUT_RD) == 0,
            "SEQPACKET 接收端执行 SHUT_RD");

    byte_t received = 0;
    REQUIRE(seqpacket_recv(pair[1], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == read_queued &&
                    seqpacket_recv(pair[1], &received,
                    sizeof(received), 0) == 0,
            "SHUT_RD 保留已有记录并在排空后返回 EOF");
    const byte_t rejected = 0x42;
    REQUIRE(seqpacket_send(pair[0], &rejected, sizeof(rejected)) == _EPIPE,
            "SHUT_RD 向对端传播写关闭");
    const byte_t read_reverse = 0x43;
    REQUIRE(seqpacket_send(pair[1],
                    &read_reverse, sizeof(read_reverse)) ==
                    (ssize_t) sizeof(read_reverse) &&
                    seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == read_reverse,
            "SHUT_RD 不关闭反向发送与接收");
    int left_events = pair[0]->ops->poll(pair[0]);
    int right_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((left_events & SEQPACKET_STATE_EVENTS) == 0 &&
                    (right_events & SEQPACKET_STATE_EVENTS) == POLL_READ,
            "SHUT_RD 只让本端 EOF 可读且不产生完整挂断");
    REQUIRE(seqpacket_take_error(pair[0], &socket_error) &&
                    socket_error == 0 &&
                    seqpacket_take_error(pair[1], &socket_error) &&
                    socket_error == 0,
            "SHUT_RD 不产生异步 socket 错误");
    REQUIRE(seqpacket_close_pair(pair),
            "清理 SHUT_RD 有序记录对");

    REQUIRE(socket_pair_create_task(task,
                    AF_LOCAL_, SOCK_SEQPACKET_, 0, pair) == 0,
            "为 SHUT_WR 创建有序记录对");
    const byte_t write_queued = 0x51;
    REQUIRE(seqpacket_send(pair[1],
                    &write_queued, sizeof(write_queued)) ==
                    (ssize_t) sizeof(write_queued),
            "SHUT_WR 前排入对端接收记录");
    const struct socket_ref write_shutdown = {.fd = pair[1]};
    REQUIRE(socket_shutdown_ref(&write_shutdown, SHUT_WR) == 0,
            "SEQPACKET 发送端执行 SHUT_WR");
    REQUIRE(seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == write_queued &&
                    seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == 0,
            "SHUT_WR 保留已发送记录并向对端交付 EOF");
    REQUIRE(seqpacket_send(pair[1], &rejected, sizeof(rejected)) == _EPIPE,
            "SHUT_WR 关闭本端后续发送");
    const byte_t write_reverse = 0x53;
    REQUIRE(seqpacket_send(pair[0],
                    &write_reverse, sizeof(write_reverse)) ==
                    (ssize_t) sizeof(write_reverse) &&
                    seqpacket_recv(pair[1], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == write_reverse,
            "SHUT_WR 不关闭反向发送与接收");
    left_events = pair[0]->ops->poll(pair[0]);
    right_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((left_events & SEQPACKET_STATE_EVENTS) == POLL_READ &&
                    (right_events & SEQPACKET_STATE_EVENTS) == 0,
            "SHUT_WR 只让对端 EOF 可读且不产生完整挂断");
    REQUIRE(seqpacket_take_error(pair[0], &socket_error) &&
                    socket_error == 0 &&
                    seqpacket_take_error(pair[1], &socket_error) &&
                    socket_error == 0,
            "SHUT_WR 不产生异步 socket 错误");
    REQUIRE(seqpacket_close_pair(pair),
            "清理 SHUT_WR 有序记录对");

    REQUIRE(socket_pair_create_task(task,
                    AF_LOCAL_, SOCK_SEQPACKET_, 0, pair) == 0,
            "为 SHUT_RDWR 创建有序记录对");
    const byte_t both_left = 0x61;
    const byte_t both_right = 0x62;
    REQUIRE(seqpacket_send(pair[0],
                    &both_left, sizeof(both_left)) ==
                    (ssize_t) sizeof(both_left) &&
                    seqpacket_send(pair[1],
                    &both_right, sizeof(both_right)) ==
                    (ssize_t) sizeof(both_right),
            "SHUT_RDWR 前为两个方向各排入一条记录");
    const struct socket_ref both_shutdown = {.fd = pair[1]};
    REQUIRE(socket_shutdown_ref(&both_shutdown, SHUT_RDWR) == 0,
            "SEQPACKET 端点执行 SHUT_RDWR");
    REQUIRE(seqpacket_recv(pair[1], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == both_left &&
                    seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == both_right,
            "SHUT_RDWR 保留两个方向已经排队的记录");
    REQUIRE(seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == 0 &&
                    seqpacket_recv(pair[1], &received,
                    sizeof(received), 0) == 0 &&
                    seqpacket_send(pair[0], &rejected,
                    sizeof(rejected)) == _EPIPE &&
                    seqpacket_send(pair[1], &rejected,
                    sizeof(rejected)) == _EPIPE,
            "SHUT_RDWR 在排空后向两端交付 EOF 与 EPIPE");
    left_events = pair[0]->ops->poll(pair[0]);
    right_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((left_events & SEQPACKET_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP) &&
                    (right_events & SEQPACKET_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP),
            "SHUT_RDWR 让两端持续呈现可读与完整挂断");
    REQUIRE(seqpacket_take_error(pair[0], &socket_error) &&
                    socket_error == 0 &&
                    seqpacket_take_error(pair[1], &socket_error) &&
                    socket_error == 0,
            "SHUT_RDWR 不产生 ECONNRESET");
    REQUIRE(seqpacket_close_pair(pair),
            "清理 SHUT_RDWR 有序记录对");

cleanup:
    seqpacket_close_pair(pair);
    return passed;
}

enum seqpacket_reset_consumer {
    SEQPACKET_RESET_BY_RECV,
    SEQPACKET_RESET_BY_SEND,
    SEQPACKET_RESET_BY_SO_ERROR,
};

static bool test_seqpacket_unread_close_error(struct task *task,
        enum seqpacket_reset_consumer consumer) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};

    REQUIRE(socket_pair_create_task(task,
                    AF_LOCAL_, SOCK_SEQPACKET_, 0, pair) == 0,
            "为未读关闭创建有序记录对");
    const byte_t unread_by_closer = 0x71;
    const byte_t queued_for_survivor = 0x72;
    REQUIRE(seqpacket_send(pair[0],
                    &unread_by_closer, sizeof(unread_by_closer)) ==
                    (ssize_t) sizeof(unread_by_closer) &&
                    seqpacket_send(pair[1],
                    &queued_for_survivor, sizeof(queued_for_survivor)) ==
                    (ssize_t) sizeof(queued_for_survivor),
            "关闭前让关闭端与存活端各有一条待读记录");
    REQUIRE(seqpacket_close_fd(&pair[1]),
            "关闭带未读记录的对端");

    int first_events = pair[0]->ops->poll(pair[0]);
    int repeated_events = pair[0]->ops->poll(pair[0]);
    REQUIRE((first_events & SEQPACKET_STATE_EVENTS) ==
                    SEQPACKET_STATE_EVENTS &&
                    (repeated_events & SEQPACKET_STATE_EVENTS) ==
                    SEQPACKET_STATE_EVENTS,
            "未读关闭的 poll 重复观察错误但不会消费它");

    sdword_t socket_error = -1;
    byte_t attempted = 0xa5;
    switch (consumer) {
        case SEQPACKET_RESET_BY_RECV:
            REQUIRE(seqpacket_recv(pair[0], &attempted,
                            sizeof(attempted), MSG_PEEK_) == _ECONNRESET &&
                            attempted == 0xa5,
                    "MSG_PEEK recv 优先消费一次 ECONNRESET 且不读取记录");
            break;
        case SEQPACKET_RESET_BY_SEND:
            REQUIRE(seqpacket_send(pair[0],
                            &attempted, sizeof(attempted)) == _ECONNRESET,
                    "send 优先消费一次 ECONNRESET");
            break;
        case SEQPACKET_RESET_BY_SO_ERROR:
            REQUIRE(seqpacket_take_error(pair[0], &socket_error) &&
                            socket_error == -_ECONNRESET,
                    "SO_ERROR 返回并消费一次 ECONNRESET");
            break;
    }

    int consumed_events = pair[0]->ops->poll(pair[0]);
    REQUIRE((consumed_events & SEQPACKET_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP),
            "消费 reset 后 poll 只保留可读与挂断");
    REQUIRE(seqpacket_take_error(pair[0], &socket_error) &&
                    socket_error == 0 &&
                    seqpacket_take_error(pair[0], &socket_error) &&
                    socket_error == 0,
            "ECONNRESET 只能由一个接口消费一次");

    byte_t received = 0;
    REQUIRE(seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == (ssize_t) sizeof(received) &&
                    received == queued_for_survivor,
            "reset 不会销毁存活端已经排队的记录");
    REQUIRE(seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == 0 &&
                    seqpacket_recv(pair[0], &received,
                    sizeof(received), 0) == 0,
            "reset 消费且记录排空后稳定返回 EOF");
    REQUIRE(seqpacket_send(pair[0],
                    &attempted, sizeof(attempted)) == _EPIPE,
            "reset 消费后的后续发送返回 EPIPE");
    REQUIRE(seqpacket_take_error(pair[0], &socket_error) &&
                    socket_error == 0,
            "后续 EPIPE 不会重新写入 SO_ERROR");

cleanup:
    seqpacket_close_pair(pair);
    return passed;
}

static bool receive_guest_byte(fd_t receiver, byte_t expected) {
    byte_t received = 0;
    return sys_recvfrom(receiver, USER_RECEIVE, sizeof(received),
                    MSG_DONTWAIT_, 0, 0) == sizeof(received) &&
            user_read(USER_RECEIVE, &received, sizeof(received)) == 0 &&
            received == expected;
}

static bool test_i386_message_addresses_and_eof(void) {
    bool passed = true;
    fd_t pair[2] = {-1, -1};
    byte_t payload = 0x5a;
    struct sockaddr_ family_only = {.family = AF_LOCAL_};
    struct iovec_ vector = {
        .base = USER_PAYLOAD,
        .len = sizeof(payload),
    };
    struct msghdr_ header = {
        .msg_name = USER_ADDRESS,
        .msg_namelen = sizeof(family_only.family),
        .msg_iov = USER_IOV,
        .msg_iovlen = 1,
    };

    REQUIRE(create_guest_pair(PF_LOCAL_, pair),
            "为 i386 消息接口创建 PF_LOCAL 有序记录对");
    REQUIRE(f_get_task(current, pair[0])->socket.guest_protocol == 0 &&
                    f_get_task(current, pair[1])->socket.guest_protocol == 0,
            "已安装 PF_LOCAL socketpair 对 guest 报告协议零");
    REQUIRE(user_write(USER_PAYLOAD, &payload, sizeof(payload)) == 0 &&
                    user_write(USER_IOV, &vector, sizeof(vector)) == 0 &&
                    user_write(USER_ADDRESS, &family_only,
                    sizeof(family_only.family)) == 0 &&
                    user_write(USER_HEADER, &header, sizeof(header)) == 0,
            "准备 family-only sendmsg 参数");
    REQUIRE(sys_sendmsg(pair[0], USER_HEADER, 0) == sizeof(payload) &&
                    receive_guest_byte(pair[1], payload),
            "已连接 SEQPACKET 的 sendmsg 忽略 family-only 地址");

    payload = 0x6b;
    REQUIRE(user_write(USER_PAYLOAD, &payload, sizeof(payload)) == 0 &&
                    sys_sendto(pair[0], USER_PAYLOAD, sizeof(payload), 0,
                    USER_ADDRESS, sizeof(family_only.family)) ==
                    sizeof(payload) &&
                    receive_guest_byte(pair[1], payload),
            "已连接 SEQPACKET 的 sendto 忽略 family-only 地址");

    header.msg_name = UNMAPPED_ADDRESS;
    header.msg_namelen = sizeof(family_only.family);
    REQUIRE(user_write(USER_HEADER, &header, sizeof(header)) == 0 &&
                    sys_sendmsg(pair[0], USER_HEADER, 0) == _EFAULT,
            "sendmsg 在忽略地址前仍复制用户内存");
    header.msg_name = USER_ADDRESS;
    header.msg_namelen = UINT32_MAX;
    REQUIRE(user_write(USER_HEADER, &header, sizeof(header)) == 0 &&
                    sys_sendmsg(pair[0], USER_HEADER, 0) == _EINVAL,
            "sendmsg 拒绝负数语义的地址长度");
    header.msg_namelen = sizeof(struct sockaddr_storage) + 1;
    REQUIRE(user_write(USER_HEADER, &header, sizeof(header)) == 0 &&
                    sys_sendmsg(pair[0], USER_HEADER, 0) ==
                    sizeof(payload) && receive_guest_byte(pair[1], payload),
            "sendmsg 截断过长地址后仍按 SEQPACKET 语义忽略内容");

    REQUIRE(sys_sendto(pair[0], USER_PAYLOAD, sizeof(payload), 0,
                    UNMAPPED_ADDRESS, sizeof(family_only.family)) == _EFAULT,
            "sendto 在忽略地址前仍复制用户内存");
    REQUIRE(sys_sendto(pair[0], USER_PAYLOAD, sizeof(payload), 0,
                    USER_ADDRESS, UINT32_MAX) == _EINVAL &&
                    sys_sendto(pair[0], USER_PAYLOAD, sizeof(payload), 0,
                    USER_ADDRESS,
                    sizeof(struct sockaddr_storage) + 1) == _EINVAL,
            "sendto 拒绝负数与过长地址长度");

    REQUIRE(f_close_task(current, pair[0]) == 0,
            "关闭 i386 消息发送端");
    pair[0] = -1;
    vector.base = USER_RECEIVE;
    memset(&header, 0, sizeof(header));
    header.msg_name = USER_RETURNED_ADDRESS;
    header.msg_namelen = sizeof(struct sockaddr_storage);
    header.msg_iov = USER_IOV;
    header.msg_iovlen = 1;
    REQUIRE(user_write(USER_IOV, &vector, sizeof(vector)) == 0 &&
                    user_write(USER_HEADER, &header, sizeof(header)) == 0 &&
                    sys_recvmsg(pair[1], USER_HEADER, MSG_DONTWAIT_) == 0 &&
                    user_read(USER_HEADER, &header, sizeof(header)) == 0 &&
                    header.msg_namelen == 0 && header.msg_controllen == 0 &&
                    header.msg_flags == 0,
            "i386 recvmsg 在 peer close 后返回无来源地址的 EOF");

    header.msg_namelen = sizeof(struct sockaddr_storage);
    header.msg_flags = INT32_MAX;
    REQUIRE(user_write(USER_HEADER, &header, sizeof(header)) == 0 &&
                    sys_recvmsg(pair[1], USER_HEADER, MSG_DONTWAIT_) == 0 &&
                    user_read(USER_HEADER, &header, sizeof(header)) == 0 &&
                    header.msg_namelen == 0 && header.msg_controllen == 0 &&
                    header.msg_flags == 0,
            "i386 recvmsg 连续观察到稳定 EOF");

cleanup:
    if (pair[0] >= 0)
        f_close_task(current, pair[0]);
    if (pair[1] >= 0)
        f_close_task(current, pair[1]);
    return passed;
}

static void scan_gate_init(struct scan_gate *gate) {
    memset(gate, 0, sizeof(*gate));
    pthread_mutex_init(&gate->lock, NULL);
    pthread_cond_init(&gate->cond, NULL);
}

static void scan_gate_release(struct scan_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->released = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static bool scan_gate_wait_until_entered(struct scan_gate *gate) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += THREAD_WAIT_MS / 1000 + 1;

    pthread_mutex_lock(&gate->lock);
    int error = 0;
    while (!gate->entered && error == 0)
        error = pthread_cond_timedwait(
                &gate->cond, &gate->lock, &deadline);
    bool entered = gate->entered;
    pthread_mutex_unlock(&gate->lock);
    return entered;
}

static void scan_gate_destroy(struct scan_gate *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

static int scan_gate_poll(struct fd *fd) {
    struct scan_gate *gate = fd->data;
    pthread_mutex_lock(&gate->lock);
    gate->entered = true;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->released)
        pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
    return 0;
}

static const struct fd_ops scan_gate_ops = {
    .poll = scan_gate_poll,
};

static int capture_peer_event(
        void *opaque, int types, union poll_fd_info info) {
    struct poll_call *call = opaque;
    call->event_types = types;
    call->event_token = info.num;
    return info.num == PEER_EVENT_TOKEN ? 1 : 0;
}

static void *run_blocking_poll(void *opaque) {
    struct poll_call *call = opaque;
    current = &call->fixture->task;
    task_thread_store(&call->fixture->task, pthread_self());
    struct timespec timeout = {
        .tv_sec = THREAD_WAIT_MS / 1000 + 1,
    };
    call->result = poll_wait(call->poll,
            capture_peer_event, call, &timeout);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool test_project_peer_close_poll_wakeup(
        struct fixture *fixture) {
    bool passed = true;
    fd_t pair[2] = {-1, -1};
    int held_host_endpoint = -1;
    struct poll *poll = NULL;
    struct fd *gate_fd = NULL;
    struct scan_gate gate;
    bool gate_initialized = false;
    pthread_t thread;
    bool thread_created = false;
    struct poll_call call = {.fixture = fixture};
    atomic_init(&call.finished, false);

    REQUIRE(create_guest_pair(0, pair),
            "为阻塞 poll 创建有序记录对");
    struct fd *sender = f_get_task(&fixture->task, pair[0]);
    struct fd *receiver = f_get_task(&fixture->task, pair[1]);
    REQUIRE(sender != NULL &&
                    receiver != NULL &&
                    (held_host_endpoint = dup(sender->real_fd)) >= 0,
            "复制 host 发送端以隔离宿主关闭通知");

    poll = poll_create();
    if (IS_ERR(poll))
        poll = NULL;
    REQUIRE(poll != NULL,
            "创建项目 poll 后端");
    scan_gate_init(&gate);
    gate_initialized = true;
    gate_fd = fd_create(&scan_gate_ops);
    REQUIRE(gate_fd != NULL,
            "创建首次扫描闸门");
    gate_fd->data = &gate;
    // poll 采用头插登记；先放闸门、后放接收端，确保首次扫描先检查 peer。
    REQUIRE(poll_add_fd(poll, gate_fd, POLL_READ,
                    (union poll_fd_info) {.num = 0}) == 0 &&
                    poll_add_fd(poll, receiver, POLL_READ,
                    (union poll_fd_info) {
                        .num = PEER_EVENT_TOKEN,
                    }) == 0,
            "登记接收端与首次扫描闸门");
    call.poll = poll;
    REQUIRE(pthread_create(&thread, NULL,
                    run_blocking_poll, &call) == 0,
            "启动阻塞 poll 线程");
    thread_created = true;

    bool scanned_open_peer = scan_gate_wait_until_entered(&gate);
    if (scanned_open_peer)
        scan_gate_release(&gate);
    int close_result = scanned_open_peer ?
            f_close_task(&fixture->task, pair[0]) : _EIO;
    if (close_result == 0)
        pair[0] = -1;
    bool woke_before_host_close =
            scanned_open_peer && close_result == 0 &&
            wait_for_flag(&call.finished);

    // 救援路径只负责让失败测试可回收线程；成功判据在关闭 dup 之前取样。
    if (gate_initialized)
        scan_gate_release(&gate);
    if (!woke_before_host_close && held_host_endpoint >= 0) {
        close(held_host_endpoint);
        held_host_endpoint = -1;
    }
    if (!woke_before_host_close && pair[0] >= 0) {
        f_close_task(&fixture->task, pair[0]);
        pair[0] = -1;
    }
    bool rescued = woke_before_host_close || wait_for_flag(&call.finished);
    int join_result = rescued ? pthread_join(thread, NULL) : -1;
    if (join_result == 0)
        thread_created = false;
    if (held_host_endpoint >= 0) {
        close(held_host_endpoint);
        held_host_endpoint = -1;
    }
    task_thread_store(&fixture->task, pthread_self());
    current = &fixture->task;

    REQUIRE(scanned_open_peer,
            "首次扫描在逻辑 peer 仍连接时完成");
    REQUIRE(close_result == 0,
            "首次扫描后关闭逻辑发送端");
    REQUIRE(woke_before_host_close,
            "host endpoint 保活时由项目 poll_wakeup 唤醒等待者");
    REQUIRE(join_result == 0 && call.result == 1 &&
                    call.event_token == PEER_EVENT_TOKEN &&
                    (call.event_types & (POLL_READ | POLL_HUP)) ==
                    (POLL_READ | POLL_HUP),
            "阻塞 poll 返回接收端的可读与挂断事件");

cleanup:
    if (gate_initialized)
        scan_gate_release(&gate);
    if (held_host_endpoint >= 0)
        close(held_host_endpoint);
    if (pair[0] >= 0)
        f_close_task(&fixture->task, pair[0]);
    if (thread_created) {
        wait_for_flag(&call.finished);
        pthread_join(thread, NULL);
        task_thread_store(&fixture->task, pthread_self());
        current = &fixture->task;
    }
    if (poll != NULL)
        poll_destroy(poll);
    if (gate_fd != NULL)
        fd_close(gate_fd);
    if (gate_initialized)
        scan_gate_destroy(&gate);
    if (pair[1] >= 0)
        f_close_task(&fixture->task, pair[1]);
    return passed;
}

int main(void) {
    struct sigaction action = {
        .sa_handler = timeout_handler,
    };
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    alarm(TEST_TIMEOUT_SECONDS);

    int baseline = open_host_fds();
    struct fixture fixture;
    bool initialized = fixture_init(&fixture);
    bool passed = initialized;
    if (!initialized) {
        fprintf(stderr,
                "SEQPACKET socketpair 测试失败：初始化完整任务夹具\n");
    } else {
        passed = test_seqpacket_pair(&fixture.task) &&
                test_seqpacket_shutdown_semantics(&fixture.task) &&
                test_seqpacket_unread_close_error(&fixture.task,
                        SEQPACKET_RESET_BY_RECV) &&
                test_seqpacket_unread_close_error(&fixture.task,
                        SEQPACKET_RESET_BY_SEND) &&
                test_seqpacket_unread_close_error(&fixture.task,
                        SEQPACKET_RESET_BY_SO_ERROR) &&
                test_i386_message_addresses_and_eof() &&
                test_project_peer_close_poll_wakeup(&fixture);
    }
    fixture_destroy(&fixture);
    if (open_host_fds() != baseline) {
        fprintf(stderr,
                "SEQPACKET socketpair 测试失败：host fd 未回到基线\n");
        passed = false;
    }

    alarm(0);
    return passed ? 0 : 1;
}

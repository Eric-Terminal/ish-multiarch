#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define TEST_TIMEOUT_SECONDS 30
#define THREAD_WAIT_MS 2000
#define BLOCKED_SEND_ROUNDS 16
#define SEND_BUFFER_SIZE 4096
#define SEND_FILL_LIMIT (16 * 1024 * 1024)
#define STREAM_STATE_EVENTS (POLL_READ | POLL_ERR | POLL_HUP)

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AF_UNIX stream shutdown 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        passed = false; \
        goto cleanup; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
};

struct blocking_send_call {
    struct fixture *fixture;
    struct fd *sender;
    atomic_bool started;
    atomic_bool finished;
    ssize_t result;
};

static const byte_t send_payload[SEND_BUFFER_SIZE] = {0xa5};

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] =
            "AF_UNIX stream shutdown 测试失败：超过硬超时\n";
    (void) write(STDERR_FILENO, message, sizeof(message) - 1);
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
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {16, 16};

    fixture->task.group = &fixture->group;
    fixture->task.pid = 7401;
    fixture->task.uid = 1000;
    fixture->task.gid = 1000;
    fixture->task.euid = 1000;
    fixture->task.egid = 1000;
    list_init(&fixture->task.queue);
    list_init(&fixture->task.sockrestart.listen);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->task.sighand = sighand_new();
    if (fixture->task.sighand == NULL)
        return false;
    fixture->task.files = fdtable_new(4);
    if (IS_ERR(fixture->task.files)) {
        fixture->task.files = NULL;
        return false;
    }
    task_thread_store(&fixture->task, pthread_self());
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL)
        fdtable_release(fixture->task.files);
    if (fixture->task.sighand != NULL)
        sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static bool stream_create_pair(
        struct task *task, struct fd *pair[2]) {
    return socket_pair_create_task(
            task, AF_LOCAL_, SOCK_STREAM_, 0, pair) == 0 &&
            pair[0] != NULL && pair[1] != NULL;
}

static ssize_t stream_send(
        struct fd *fd, const void *data, size_t length) {
    const struct socket_ref socket = {.fd = fd};
    return socket_sendmsg_ref(&socket, data, length,
            MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL, NULL);
}

static ssize_t stream_recv(
        struct fd *fd, void *data, size_t length) {
    const struct socket_ref socket = {.fd = fd};
    return socket_recvfrom_ref(
            &socket, data, length, MSG_DONTWAIT_, NULL);
}

static bool stream_take_error(
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

static bool stream_close_fd(struct fd **fd_pointer) {
    if (*fd_pointer == NULL)
        return true;
    struct fd *fd = *fd_pointer;
    *fd_pointer = NULL;
    return fd_close(fd) == 0;
}

static bool stream_close_pair(struct fd *pair[2]) {
    bool first = stream_close_fd(&pair[0]);
    bool second = stream_close_fd(&pair[1]);
    return first && second;
}

static bool stream_errors_clear(struct fd *pair[2]) {
    sdword_t error = -1;
    return stream_take_error(pair[0], &error) && error == 0 &&
            stream_take_error(pair[1], &error) && error == 0;
}

static bool wait_for_flag(atomic_bool *flag, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(flag, memory_order_acquire);
}

static bool wait_for_poll_registration(
        struct task *task, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        lock(&task->waiting_cond_lock);
        bool registered = task->waiting_poll_active &&
                task->waiting_poll_notify_fd >= 0;
        unlock(&task->waiting_cond_lock);
        if (registered)
            return true;
        nanosleep(&interval, NULL);
    }
    return false;
}

static bool poll_registration_cleared(struct task *task) {
    lock(&task->waiting_cond_lock);
    bool cleared = !task->waiting_poll_active &&
            task->waiting_poll_notify_fd == -1;
    unlock(&task->waiting_cond_lock);
    return cleared;
}

static bool fill_stream_send_buffer(struct fd *sender) {
    int buffer_size = SEND_BUFFER_SIZE;
    if (setsockopt(sender->real_fd, SOL_SOCKET, SO_SNDBUF,
            &buffer_size, sizeof(buffer_size)) < 0)
        return false;

    int original_flags = fcntl(sender->real_fd, F_GETFL);
    if (original_flags < 0 || fcntl(sender->real_fd,
            F_SETFL, original_flags | O_NONBLOCK) < 0)
        return false;

    size_t total = 0;
    bool filled = false;
    while (total < SEND_FILL_LIMIT) {
        ssize_t result = send(sender->real_fd,
                send_payload, sizeof(send_payload), 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            filled = total != 0;
            break;
        }
        if (result <= 0)
            break;
        total += (size_t) result;
    }
    if (fcntl(sender->real_fd, F_SETFL, original_flags) < 0)
        return false;
    return filled;
}

static void *run_blocking_send(void *opaque) {
    struct blocking_send_call *call = opaque;
    current = &call->fixture->task;
    task_thread_store(&call->fixture->task, pthread_self());
    atomic_store_explicit(&call->started, true, memory_order_release);
    const struct socket_ref sender = {.fd = call->sender};
    call->result = socket_sendmsg_ref(&sender,
            send_payload, sizeof(send_payload),
            MSG_NOSIGNAL_, NULL, NULL);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool test_shutdown_read(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};
    byte_t received = 0;

    REQUIRE(stream_create_pair(task, pair),
            "为 SHUT_RD 创建 stream pair");
    const byte_t queued = 0x41;
    REQUIRE(stream_send(pair[0], &queued, sizeof(queued)) ==
                    (ssize_t) sizeof(queued),
            "SHUT_RD 前排入接收字节");
    const struct socket_ref shutdown_socket = {.fd = pair[1]};
    REQUIRE(socket_shutdown_ref(&shutdown_socket, SHUT_RD) == 0,
            "stream 接收端执行 SHUT_RD");
    REQUIRE(stream_recv(pair[1], &received, sizeof(received)) ==
                    (ssize_t) sizeof(received) && received == queued &&
                    stream_recv(pair[1], &received,
                            sizeof(received)) == 0,
            "SHUT_RD 保留已排队字节并在排空后返回 EOF");

    const byte_t rejected = 0x42;
    REQUIRE(stream_send(pair[0], &rejected,
                    sizeof(rejected)) == _EPIPE,
            "SHUT_RD 反向传播为 peer 写关闭");
    const byte_t reverse = 0x43;
    REQUIRE(stream_send(pair[1], &reverse, sizeof(reverse)) ==
                    (ssize_t) sizeof(reverse) &&
                    stream_recv(pair[0], &received,
                            sizeof(received)) ==
                    (ssize_t) sizeof(received) && received == reverse,
            "SHUT_RD 不影响反向发送与接收");

    int left_events = pair[0]->ops->poll(pair[0]);
    int right_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((left_events & STREAM_STATE_EVENTS) == 0 &&
                    (right_events & STREAM_STATE_EVENTS) == POLL_READ,
            "SHUT_RD 只让本端持续报告 EOF 可读");
    REQUIRE(stream_errors_clear(pair),
            "SHUT_RD 不产生 SO_ERROR");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool test_shutdown_write(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};
    byte_t received = 0;

    REQUIRE(stream_create_pair(task, pair),
            "为 SHUT_WR 创建 stream pair");
    const byte_t queued = 0x51;
    REQUIRE(stream_send(pair[1], &queued, sizeof(queued)) ==
                    (ssize_t) sizeof(queued),
            "SHUT_WR 前排入对端接收字节");
    const struct socket_ref shutdown_socket = {.fd = pair[1]};
    REQUIRE(socket_shutdown_ref(&shutdown_socket, SHUT_WR) == 0,
            "stream 发送端执行 SHUT_WR");
    REQUIRE(stream_recv(pair[0], &received, sizeof(received)) ==
                    (ssize_t) sizeof(received) && received == queued &&
                    stream_recv(pair[0], &received,
                            sizeof(received)) == 0,
            "SHUT_WR 保留已发送字节并向 peer 交付 EOF");

    const byte_t rejected = 0x52;
    REQUIRE(stream_send(pair[1], &rejected,
                    sizeof(rejected)) == _EPIPE,
            "SHUT_WR 关闭本端后续发送");
    const byte_t reverse = 0x53;
    REQUIRE(stream_send(pair[0], &reverse, sizeof(reverse)) ==
                    (ssize_t) sizeof(reverse) &&
                    stream_recv(pair[1], &received,
                            sizeof(received)) ==
                    (ssize_t) sizeof(received) && received == reverse,
            "SHUT_WR 不影响反向发送与接收");

    int left_events = pair[0]->ops->poll(pair[0]);
    int right_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((left_events & STREAM_STATE_EVENTS) == POLL_READ &&
                    (right_events & STREAM_STATE_EVENTS) == 0,
            "SHUT_WR 只让 peer 持续报告 EOF 可读");
    REQUIRE(stream_errors_clear(pair),
            "SHUT_WR 不产生 SO_ERROR");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool test_shutdown_both(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};
    byte_t received = 0;

    REQUIRE(stream_create_pair(task, pair),
            "为 SHUT_RDWR 创建 stream pair");
    const byte_t queued_for_right = 0x61;
    const byte_t queued_for_left = 0x62;
    REQUIRE(stream_send(pair[0], &queued_for_right,
                    sizeof(queued_for_right)) ==
                    (ssize_t) sizeof(queued_for_right) &&
                    stream_send(pair[1], &queued_for_left,
                            sizeof(queued_for_left)) ==
                    (ssize_t) sizeof(queued_for_left),
            "SHUT_RDWR 前为两个方向各排入字节");
    const struct socket_ref shutdown_socket = {.fd = pair[1]};
    REQUIRE(socket_shutdown_ref(&shutdown_socket, SHUT_RDWR) == 0,
            "stream 端点执行 SHUT_RDWR");
    REQUIRE(stream_recv(pair[1], &received, sizeof(received)) ==
                    (ssize_t) sizeof(received) &&
                    received == queued_for_right &&
                    stream_recv(pair[0], &received,
                            sizeof(received)) ==
                    (ssize_t) sizeof(received) &&
                    received == queued_for_left,
            "SHUT_RDWR 保留两个方向已经排队的字节");

    const byte_t rejected = 0x63;
    REQUIRE(stream_recv(pair[0], &received, sizeof(received)) == 0 &&
                    stream_recv(pair[1], &received,
                            sizeof(received)) == 0 &&
                    stream_send(pair[0], &rejected,
                            sizeof(rejected)) == _EPIPE &&
                    stream_send(pair[1], &rejected,
                            sizeof(rejected)) == _EPIPE,
            "SHUT_RDWR 排空后向两端交付 EOF 与 EPIPE");
    int left_events = pair[0]->ops->poll(pair[0]);
    int right_events = pair[1]->ops->poll(pair[1]);
    REQUIRE((left_events & STREAM_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP) &&
                    (right_events & STREAM_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP),
            "SHUT_RDWR 让两端持续报告可读与完整挂断");
    REQUIRE(stream_errors_clear(pair),
            "SHUT_RDWR 不产生 ECONNRESET");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool test_clean_close(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};

    REQUIRE(stream_create_pair(task, pair),
            "为 clean close 创建 stream pair");
    const byte_t queued = 0x71;
    REQUIRE(stream_send(pair[1], &queued, sizeof(queued)) ==
                    (ssize_t) sizeof(queued),
            "clean close 前为 survivor 排入字节");
    REQUIRE(stream_close_fd(&pair[1]),
            "关闭没有未读数据的 peer");
    int first_events = pair[0]->ops->poll(pair[0]);
    int repeated_events = pair[0]->ops->poll(pair[0]);
    REQUIRE((first_events & STREAM_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP) &&
                    (repeated_events & STREAM_STATE_EVENTS) ==
                    (POLL_READ | POLL_HUP),
            "clean close 持续报告 EOF/HUP 而不报告 reset");

    sdword_t error = -1;
    REQUIRE(stream_take_error(pair[0], &error) && error == 0 &&
                    stream_take_error(pair[0], &error) && error == 0,
            "clean close 的 SO_ERROR 始终为零");
    byte_t received = 0;
    REQUIRE(stream_recv(pair[0], &received, sizeof(received)) ==
                    (ssize_t) sizeof(received) && received == queued &&
                    stream_recv(pair[0], &received,
                            sizeof(received)) == 0 &&
                    stream_recv(pair[0], &received,
                            sizeof(received)) == 0,
            "clean close 先保留已排队字节再稳定返回 EOF");
    const byte_t rejected = 0x72;
    REQUIRE(stream_send(pair[0], &rejected,
                    sizeof(rejected)) == _EPIPE &&
                    stream_take_error(pair[0], &error) && error == 0,
            "clean close 后 EPIPE 不会生成 SO_ERROR");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool setup_unread_close(
        struct task *task, struct fd *pair[2]) {
    if (!stream_create_pair(task, pair))
        return false;
    // 两个方向同时留数据：关闭端的未读字节决定 reset，反向字节验证 survivor 顺序。
    const byte_t unread_by_closer = 0x81;
    const byte_t queued_for_survivor = 0x82;
    return stream_send(pair[0],
                    &unread_by_closer, sizeof(unread_by_closer)) ==
                    (ssize_t) sizeof(unread_by_closer) &&
            stream_send(pair[1],
                    &queued_for_survivor, sizeof(queued_for_survivor)) ==
                    (ssize_t) sizeof(queued_for_survivor) &&
            stream_close_fd(&pair[1]);
}

static bool expect_pending_reset_poll(struct fd *survivor) {
    int first_events = survivor->ops->poll(survivor);
    int repeated_events = survivor->ops->poll(survivor);
    return (first_events & STREAM_STATE_EVENTS) ==
                    STREAM_STATE_EVENTS &&
            (repeated_events & STREAM_STATE_EVENTS) ==
                    STREAM_STATE_EVENTS;
}

static bool expect_consumed_reset_poll(struct fd *survivor) {
    return (survivor->ops->poll(survivor) & STREAM_STATE_EVENTS) ==
            (POLL_READ | POLL_HUP);
}

static bool receive_survivor_byte(struct fd *survivor) {
    byte_t received = 0;
    return stream_recv(survivor, &received, sizeof(received)) ==
                    (ssize_t) sizeof(received) &&
            received == 0x82;
}

static bool test_unread_close_reset_by_recv(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};

    REQUIRE(setup_unread_close(task, pair),
            "为 recv 消费 reset 建立独立 pair");
    REQUIRE(expect_pending_reset_poll(pair[0]),
            "recv 消费前 poll 可重复观察 ECONNRESET");
    REQUIRE(receive_survivor_byte(pair[0]),
            "recv 消费 reset 前先交付 survivor 已排队字节");
    REQUIRE(expect_pending_reset_poll(pair[0]),
            "读取已排队字节不会提前消费 reset");

    byte_t received = 0xa5;
    REQUIRE(stream_recv(pair[0], &received,
                    sizeof(received)) == _ECONNRESET &&
                    received == 0xa5,
            "队列排空后的 recv 单次消费 ECONNRESET");
    REQUIRE(expect_consumed_reset_poll(pair[0]),
            "recv 消费 reset 后 poll 只保留 EOF/HUP");
    sdword_t error = -1;
    REQUIRE(stream_take_error(pair[0], &error) && error == 0 &&
                    stream_take_error(pair[0], &error) && error == 0,
            "recv 消费后 SO_ERROR 不会再次交付 reset");
    REQUIRE(stream_recv(pair[0], &received,
                    sizeof(received)) == 0 &&
                    stream_recv(pair[0], &received,
                            sizeof(received)) == 0,
            "reset 消费后稳定返回 EOF");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool test_unread_close_reset_by_so_error(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};

    REQUIRE(setup_unread_close(task, pair),
            "为 SO_ERROR 消费 reset 建立独立 pair");
    REQUIRE(expect_pending_reset_poll(pair[0]),
            "SO_ERROR 消费前 poll 可重复观察 ECONNRESET");
    REQUIRE(receive_survivor_byte(pair[0]),
            "SO_ERROR 消费前先交付 survivor 已排队字节");

    sdword_t error = -1;
    REQUIRE(stream_take_error(pair[0], &error) &&
                    error == -_ECONNRESET,
            "SO_ERROR 单次返回并消费 ECONNRESET");
    REQUIRE(expect_consumed_reset_poll(pair[0]),
            "SO_ERROR 消费 reset 后 poll 只保留 EOF/HUP");
    REQUIRE(stream_take_error(pair[0], &error) && error == 0 &&
                    stream_take_error(pair[0], &error) && error == 0,
            "SO_ERROR 后续读取稳定为零");
    byte_t received = 0;
    REQUIRE(stream_recv(pair[0], &received,
                    sizeof(received)) == 0,
            "SO_ERROR 消费 reset 后 recv 返回 EOF");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool test_send_preserves_pending_reset(struct task *task) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};

    REQUIRE(setup_unread_close(task, pair),
            "为 send/reset 顺序建立独立 pair");
    const byte_t attempted = 0x91;
    // STREAM 写路径只报告断管；一次性 reset 必须留给 recv 或 SO_ERROR。
    REQUIRE(stream_send(pair[0], &attempted,
                    sizeof(attempted)) == _EPIPE,
            "reset 未消费时 stream send 仍优先返回 EPIPE");
    REQUIRE(expect_pending_reset_poll(pair[0]),
            "EPIPE 不会消费待交付 reset");
    REQUIRE(receive_survivor_byte(pair[0]),
            "EPIPE 后仍先读取 survivor 已排队字节");
    REQUIRE(stream_send(pair[0], &attempted,
                    sizeof(attempted)) == _EPIPE,
            "队列排空但 reset 未消费时 send 仍返回 EPIPE");

    sdword_t error = -1;
    REQUIRE(stream_take_error(pair[0], &error) &&
                    error == -_ECONNRESET &&
                    stream_take_error(pair[0], &error) && error == 0,
            "两次 EPIPE 后 SO_ERROR 仍单次交付 ECONNRESET");
    REQUIRE(expect_consumed_reset_poll(pair[0]),
            "消费保留的 reset 后清除 POLL_ERR");

cleanup:
    stream_close_pair(pair);
    return passed;
}

static bool test_blocked_send_wakes_on_peer_close(
        struct fixture *fixture, unsigned round) {
    bool passed = true;
    struct fd *pair[2] = {NULL, NULL};
    pthread_t thread;
    bool thread_created = false;
    struct blocking_send_call call = {
        .fixture = fixture,
        .result = _EIO,
    };
    atomic_init(&call.started, false);
    atomic_init(&call.finished, false);

    REQUIRE(stream_create_pair(&fixture->task, pair),
            "为阻塞 send/close 创建 stream pair");
    REQUIRE(fill_stream_send_buffer(pair[0]),
            "通过 host 非阻塞 send 将宿主发送缓冲填至 EAGAIN");
    call.sender = pair[0];
    REQUIRE(pthread_create(&thread, NULL,
                    run_blocking_send, &call) == 0,
            "启动不带 MSG_DONTWAIT 的 send 线程");
    thread_created = true;
    REQUIRE(wait_for_flag(&call.started, THREAD_WAIT_MS),
            "阻塞 send 线程开始运行");
    REQUIRE(wait_for_poll_registration(
                    &fixture->task, THREAD_WAIT_MS) &&
                    !atomic_load_explicit(
                            &call.finished, memory_order_acquire),
            "send 在 peer close 前确实阻塞于项目 poll");

    // poll 登记完成时发送路径已释放临时 peer 引用；这里确实关闭最后 guest 引用。
    REQUIRE(stream_close_fd(&pair[1]),
            "主线程只关闭带未读数据的 peer 最后引用");
    if (!wait_for_flag(&call.finished, THREAD_WAIT_MS)) {
        static const char message[] =
                "AF_UNIX stream shutdown 测试失败：peer close 未唤醒 send\n";
        (void) write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(125);
    }
    REQUIRE(pthread_join(thread, NULL) == 0,
            "回收被 peer close 唤醒的 send 线程");
    thread_created = false;
    task_thread_store(&fixture->task, pthread_self());
    current = &fixture->task;

    REQUIRE(call.result == _EPIPE,
            "STREAM 阻塞 send 被 close 唤醒后稳定返回 EPIPE");
    REQUIRE(poll_registration_cleared(&fixture->task),
            "阻塞 send 返回后清理 poll 登记");
    REQUIRE((pair[0]->ops->poll(pair[0]) & STREAM_STATE_EVENTS) ==
                    STREAM_STATE_EVENTS,
            "EPIPE 后 poll 仍报告尚未消费的 ECONNRESET");
    sdword_t error = -1;
    REQUIRE(stream_take_error(pair[0], &error) &&
                    error == -_ECONNRESET &&
                    stream_take_error(pair[0], &error) && error == 0,
            "阻塞 send 的 EPIPE 不消费 close 产生的一次性 reset");
    REQUIRE(expect_consumed_reset_poll(pair[0]),
            "消费 reset 后 poll 清除错误且保留 EOF/HUP");

cleanup:
    if (thread_created) {
        (void) stream_close_fd(&pair[1]);
        if (!wait_for_flag(&call.finished, THREAD_WAIT_MS))
            _exit(125);
        (void) pthread_join(thread, NULL);
        task_thread_store(&fixture->task, pthread_self());
        current = &fixture->task;
    }
    stream_close_pair(pair);
    if (!passed)
        fprintf(stderr,
                "AF_UNIX stream shutdown 阻塞回归失败轮次：%u\n",
                round);
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
                "AF_UNIX stream shutdown 测试失败：初始化任务夹具\n");
    } else {
        if (!test_shutdown_read(&fixture.task))
            passed = false;
        if (!test_shutdown_write(&fixture.task))
            passed = false;
        if (!test_shutdown_both(&fixture.task))
            passed = false;
        if (!test_clean_close(&fixture.task))
            passed = false;
        if (!test_unread_close_reset_by_recv(&fixture.task))
            passed = false;
        if (!test_unread_close_reset_by_so_error(&fixture.task))
            passed = false;
        if (!test_send_preserves_pending_reset(&fixture.task))
            passed = false;
        for (unsigned round = 0;
                round < BLOCKED_SEND_ROUNDS; round++) {
            if (!test_blocked_send_wakes_on_peer_close(
                    &fixture, round))
                passed = false;
        }
    }
    fixture_destroy(&fixture);
    if (open_host_fds() != baseline) {
        fprintf(stderr,
                "AF_UNIX stream shutdown 测试失败：host fd 未回到基线\n");
        passed = false;
    }

    alarm(0);
    return passed ? 0 : 1;
}

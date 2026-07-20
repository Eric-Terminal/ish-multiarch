#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
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

static int failures;
static atomic_uint unique_name;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AF_UNIX listener shutdown 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        EXPECT(false, message); \
        goto cleanup; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
};

struct guest_unix_address {
    struct sockaddr_max_ wire;
    size_t length;
};

struct accept_call {
    struct fixture *fixture;
    fd_t listener;
    atomic_bool started;
    atomic_bool finished;
    int_t result;
};

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] =
            "AF_UNIX listener shutdown 测试失败：超过硬超时\n";
    (void) write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {32, 32};

    fixture->task.group = &fixture->group;
    list_init(&fixture->task.queue);
    list_init(&fixture->task.sockrestart.listen);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->task.sighand = sighand_new();
    if (fixture->task.sighand == NULL)
        return false;
    fixture->task.files = fdtable_new(8);
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

static struct guest_unix_address make_address(void) {
    struct guest_unix_address address = {0};
    address.wire.family = AF_LOCAL_;
    unsigned sequence = atomic_fetch_add_explicit(
            &unique_name, 1, memory_order_relaxed);
    snprintf(address.wire.data + 1, sizeof(address.wire.data) - 1,
            "listener-shutdown-%ld-%u", (long) getpid(), sequence);
    address.length = offsetof(struct sockaddr_max_, data) + 1 +
            strlen(address.wire.data + 1);
    return address;
}

static int bind_socket(struct fixture *fixture, fd_t number,
        const struct guest_unix_address *address) {
    struct socket_ref socket = {0};
    int result = socket_ref_get_task(
            &fixture->task, number, &socket);
    if (result == 0)
        result = socket_bind_ref_task(&fixture->task, &socket,
                &address->wire, address->length);
    if (socket.fd != NULL)
        socket_ref_release(&socket);
    return result;
}

static int connect_socket(struct fixture *fixture, fd_t number,
        const struct guest_unix_address *address) {
    struct socket_ref socket = {0};
    int result = socket_ref_get_task(
            &fixture->task, number, &socket);
    if (result == 0)
        result = socket_connect_ref_task(&fixture->task, &socket,
                &address->wire, address->length);
    if (socket.fd != NULL)
        socket_ref_release(&socket);
    return result;
}

static fd_t create_stream(struct fixture *fixture) {
    return socket_create_task(
            &fixture->task, AF_LOCAL_, SOCK_STREAM_, 0);
}

static bool setup_listener(struct fixture *fixture,
        struct guest_unix_address *address, fd_t *listener) {
    *address = make_address();
    *listener = create_stream(fixture);
    return *listener >= 0 &&
            bind_socket(fixture, *listener, address) == 0 &&
            sys_listen(*listener, 4) == 0;
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

static void *run_accept(void *opaque) {
    struct accept_call *call = opaque;
    current = &call->fixture->task;
    task_thread_store(&call->fixture->task, pthread_self());
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = sys_accept(call->listener, 0, 0);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void test_preaccept_connector_is_not_eof(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "预 accept connector 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    fd_t connector = -1;
    fd_t accepted = -1;
    struct socket_ref connector_ref = {0};
    struct socket_ref accepted_ref = {0};

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建预 accept 状态的 listener");
    connector = create_stream(&fixture);
    REQUIRE(connector >= 0 &&
                    connect_socket(&fixture, connector, &address) == 0 &&
                    socket_ref_get_task(&fixture.task,
                            connector, &connector_ref) == 0,
            "connector 成功连接但暂不 accept");
    unsigned char received = 0;
    REQUIRE(socket_recvfrom_ref(&connector_ref,
                    &received, sizeof(received),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "尚未 accept 的空 connector 非阻塞读取返回 EAGAIN");
    int events = connector_ref.fd->ops->poll(connector_ref.fd);
    REQUIRE((events & (POLL_READ | POLL_HUP)) == 0,
            "尚未 accept 的 connector 不伪造 EOF 或 HUP");

    accepted = sys_accept(listener, 0, 0);
    REQUIRE(accepted >= 0 &&
                    socket_ref_get_task(&fixture.task,
                            accepted, &accepted_ref) == 0,
            "server accept 完成 pending 握手");
    const unsigned char to_server = 0x31;
    REQUIRE(socket_sendmsg_ref(&connector_ref,
                    &to_server, sizeof(to_server),
                    MSG_DONTWAIT_ | MSG_NOSIGNAL_,
                    NULL, NULL) == sizeof(to_server) &&
                    socket_recvfrom_ref(&accepted_ref,
                            &received, sizeof(received),
                            MSG_DONTWAIT_, NULL) == sizeof(received) &&
                    received == to_server,
            "accept 后 connector 可向 server 正常发送");
    const unsigned char to_connector = 0x32;
    REQUIRE(socket_sendmsg_ref(&accepted_ref,
                    &to_connector, sizeof(to_connector),
                    MSG_DONTWAIT_ | MSG_NOSIGNAL_,
                    NULL, NULL) == sizeof(to_connector) &&
                    socket_recvfrom_ref(&connector_ref,
                            &received, sizeof(received),
                            MSG_DONTWAIT_, NULL) == sizeof(received) &&
                    received == to_connector,
            "accept 后 server 可向 connector 正常发送");

cleanup:
    if (accepted_ref.fd != NULL)
        socket_ref_release(&accepted_ref);
    if (connector_ref.fd != NULL)
        socket_ref_release(&connector_ref);
    fixture_destroy(&fixture);
}

static void test_pending_drain_and_closed_gate(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "排空夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    fd_t pending[2] = {-1, -1};
    fd_t rejected_before_listen = -1;
    fd_t rejected_after_listen = -1;

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建并启动 listener");
    for (size_t index = 0; index < 2; index++) {
        pending[index] = create_stream(&fixture);
        REQUIRE(pending[index] >= 0 &&
                        connect_socket(&fixture, pending[index],
                                &address) == 0,
                "shutdown 前连接进入待 accept 队列");
    }

    REQUIRE(sys_shutdown(listener, SHUT_RD) == 0,
            "listener 执行 SHUT_RD");
    rejected_before_listen = create_stream(&fixture);
    REQUIRE(rejected_before_listen >= 0,
            "创建 shutdown 后的新 connector");
    EXPECT(connect_socket(&fixture, rejected_before_listen,
                    &address) == _ECONNREFUSED,
            "shutdown 后的新 connect 被拒绝");

    REQUIRE(sys_listen(listener, 8) == 0,
            "已 shutdown 的 listener 可重复 listen");
    rejected_after_listen = create_stream(&fixture);
    REQUIRE(rejected_after_listen >= 0,
            "创建重复 listen 后的新 connector");
    EXPECT(connect_socket(&fixture, rejected_after_listen,
                    &address) == _ECONNREFUSED,
            "重复 listen 不会重新开放 connect 入口");

    for (size_t index = 0; index < 2; index++) {
        fd_t accepted = sys_accept(listener, 0, 0);
        REQUIRE(accepted >= 0,
                "shutdown 前已经排队的连接仍可 accept");
        EXPECT(f_close_task(&fixture.task, accepted) == 0,
                "关闭排空得到的 accepted fd");
    }
    EXPECT(sys_accept(listener, 0, 0) == _EINVAL,
            "阻塞 listener 排空后 accept 返回 EINVAL");

    struct fd *listener_fd = f_get_task(&fixture.task, listener);
    REQUIRE(listener_fd != NULL,
            "取得 listener 以切换非阻塞状态");
    int flags = fd_getflags(listener_fd);
    REQUIRE(flags >= 0 &&
                    fd_setflags(listener_fd, flags | O_NONBLOCK_) == 0,
            "listener 切换为非阻塞状态");
    EXPECT(sys_accept(listener, 0, 0) == _EAGAIN,
            "非阻塞 listener 排空后 accept 返回 EAGAIN");

cleanup:
    fixture_destroy(&fixture);
}

static void test_blocked_accept_wakes_on_shutdown(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "阻塞 accept 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    pthread_t thread;
    bool thread_started = false;
    struct accept_call call = {
        .fixture = &fixture,
        .listener = -1,
        .result = _EIO,
    };
    atomic_init(&call.started, false);
    atomic_init(&call.finished, false);

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建空队列 listener");
    call.listener = listener;
    REQUIRE(pthread_create(&thread, NULL, run_accept, &call) == 0,
            "启动阻塞 accept 线程");
    thread_started = true;
    REQUIRE(wait_for_flag(&call.started, THREAD_WAIT_MS),
            "阻塞 accept 线程开始运行");
    bool registered = wait_for_poll_registration(
            &fixture.task, THREAD_WAIT_MS);
    EXPECT(registered && !atomic_load_explicit(
                    &call.finished, memory_order_acquire),
            "accept 在 shutdown 前确实阻塞于 poll");

    REQUIRE(sys_shutdown(listener, SHUT_RD) == 0,
            "shutdown 唤醒阻塞 accept");
    if (!wait_for_flag(&call.finished, THREAD_WAIT_MS)) {
        static const char message[] =
                "AF_UNIX listener shutdown 测试失败：accept 未被唤醒\n";
        (void) write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(125);
    }
    REQUIRE(pthread_join(thread, NULL) == 0,
            "回收阻塞 accept 线程");
    thread_started = false;
    task_thread_store(&fixture.task, pthread_self());
    EXPECT(call.result == _EINVAL,
            "被 shutdown 唤醒的阻塞 accept 返回 EINVAL");
    EXPECT(poll_registration_cleared(&fixture.task),
            "accept 返回后清理 poll 登记");

cleanup:
    if (thread_started) {
        (void) sys_shutdown(listener, SHUT_RD);
        if (!wait_for_flag(&call.finished, THREAD_WAIT_MS))
            _exit(125);
        (void) pthread_join(thread, NULL);
        task_thread_store(&fixture.task, pthread_self());
    }
    fixture_destroy(&fixture);
}

static void test_preaccept_write_shutdown_propagates(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "预 accept SHUT_WR 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    fd_t connector = -1;
    fd_t accepted = -1;
    struct socket_ref connector_ref = {0};
    struct socket_ref accepted_ref = {0};
    const unsigned char payload = 0x5a;

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建 SHUT_WR listener");
    connector = create_stream(&fixture);
    REQUIRE(connector >= 0 &&
                    connect_socket(&fixture, connector, &address) == 0,
            "建立 accept 前的 SHUT_WR connector");
    REQUIRE(socket_ref_get_task(&fixture.task,
                    connector, &connector_ref) == 0,
            "取得 SHUT_WR connector 引用");
    REQUIRE(socket_sendmsg_ref(&connector_ref,
                    &payload, sizeof(payload), MSG_NOSIGNAL_,
                    NULL, NULL) == sizeof(payload),
            "SHUT_WR 前排队一字节数据");
    REQUIRE(sys_shutdown(connector, SHUT_WR) == 0,
            "connector 在 accept 前执行 SHUT_WR");

    accepted = sys_accept(listener, 0, 0);
    REQUIRE(accepted >= 0 &&
                    socket_ref_get_task(&fixture.task,
                            accepted, &accepted_ref) == 0,
            "accept 取得预先关闭写方向的 connector");
    unsigned char received = 0;
    EXPECT(socket_recvfrom_ref(&accepted_ref,
                    &received, sizeof(received), MSG_DONTWAIT_, NULL) ==
                    sizeof(received) && received == payload,
            "accepted peer 先读取 shutdown 前已排队的数据");
    EXPECT(socket_recvfrom_ref(&accepted_ref,
                    &received, sizeof(received), MSG_DONTWAIT_, NULL) == 0,
            "connector 的 SHUT_WR 在 accept 后传播为 peer EOF");

cleanup:
    if (accepted_ref.fd != NULL)
        socket_ref_release(&accepted_ref);
    if (connector_ref.fd != NULL)
        socket_ref_release(&connector_ref);
    fixture_destroy(&fixture);
}

static void test_preaccept_read_shutdown_propagates(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "预 accept SHUT_RD 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    fd_t connector = -1;
    fd_t accepted = -1;
    struct socket_ref connector_ref = {0};
    struct socket_ref accepted_ref = {0};
    const unsigned char payload[] = {0x6a, 0x6b};

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建 SHUT_RD listener");
    connector = create_stream(&fixture);
    REQUIRE(connector >= 0 &&
                    connect_socket(&fixture, connector, &address) == 0,
            "建立 accept 前的 SHUT_RD connector");
    REQUIRE(sys_shutdown(connector, SHUT_RD) == 0,
            "connector 在 accept 前执行 SHUT_RD");
    REQUIRE(socket_ref_get_task(&fixture.task,
                    connector, &connector_ref) == 0,
            "取得 accept 前 SHUT_RD connector 引用");
    struct scm *zero_length_scm = NULL;
    REQUIRE(socket_scm_create_task(&fixture.task,
                    &connector, 1, &zero_length_scm) == 0 &&
                    socket_sendto_ref(&connector_ref,
                            &payload[0], 1, MSG_NOSIGNAL_, NULL) == 1 &&
                    socket_sendmsg_ref(&connector_ref,
                            &payload[1], 1, MSG_NOSIGNAL_,
                            NULL, NULL) == 1 &&
                    socket_sendmsg_ref(&connector_ref,
                            NULL, 0, MSG_NOSIGNAL_,
                            NULL, &zero_length_scm) == 0 &&
                    zero_length_scm == NULL,
            "SHUT_RD 后 accept 前的 scalar、sendmsg 与零长度 SCM 仍可发送");

    accepted = sys_accept(listener, 0, 0);
    REQUIRE(accepted >= 0 &&
                    socket_ref_get_task(&fixture.task,
                            accepted, &accepted_ref) == 0,
            "accept 取得预先关闭读方向的 connector");
    unsigned char received[sizeof(payload)] = {0};
    EXPECT(socket_recvfrom_ref(&accepted_ref,
                    received, sizeof(received), MSG_DONTWAIT_, NULL) ==
                    sizeof(received) &&
                    memcmp(received, payload, sizeof(payload)) == 0,
            "accepted peer 完整读取 accept 前排队的数据");
    EXPECT(socket_sendmsg_ref(&accepted_ref,
                    &payload[0], 1,
                    MSG_DONTWAIT_ | MSG_NOSIGNAL_,
                    NULL, NULL) == _EPIPE,
            "connector 的 SHUT_RD 在 accept 后传播为 peer EPIPE");

cleanup:
    if (connector_ref.fd != NULL)
        socket_ref_release(&connector_ref);
    if (accepted_ref.fd != NULL)
        socket_ref_release(&accepted_ref);
    fixture_destroy(&fixture);
}

static void test_preconnect_shutdown_is_not_propagated(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "connect 前 shutdown 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    fd_t connector = -1;
    fd_t accepted = -1;
    struct socket_ref connector_ref = {0};
    struct socket_ref accepted_ref = {0};
    const unsigned char payload = 0x7c;

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建 connect 前 shutdown listener");
    connector = create_stream(&fixture);
    REQUIRE(connector >= 0 &&
                    sys_shutdown(connector, SHUT_WR) == 0 &&
                    connect_socket(&fixture, connector, &address) == 0,
            "写关闭发生在 connect 之前");
    accepted = sys_accept(listener, 0, 0);
    REQUIRE(accepted >= 0 &&
                    socket_ref_get_task(&fixture.task,
                            connector, &connector_ref) == 0 &&
                    socket_ref_get_task(&fixture.task,
                            accepted, &accepted_ref) == 0,
            "accept 取得 connect 前已写关闭的 connector");
    EXPECT(socket_sendmsg_ref(&connector_ref,
                    &payload, sizeof(payload),
                    MSG_DONTWAIT_ | MSG_NOSIGNAL_,
                    NULL, NULL) == _EPIPE,
            "connect 前 SHUT_WR 仍关闭 connector 本端写方向");
    unsigned char received = 0;
    EXPECT(socket_recvfrom_ref(&accepted_ref,
                    &received, sizeof(received),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "connect 前 SHUT_WR 不向未来 accepted peer 传播 EOF");

cleanup:
    if (accepted_ref.fd != NULL)
        socket_ref_release(&accepted_ref);
    if (connector_ref.fd != NULL)
        socket_ref_release(&connector_ref);
    fixture_destroy(&fixture);
}

static void test_repeated_shutdown_after_connect_is_propagated(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "connect 后重复 shutdown 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct guest_unix_address address;
    fd_t listener = -1;
    fd_t connector = -1;
    fd_t accepted = -1;
    struct socket_ref accepted_ref = {0};

    REQUIRE(setup_listener(&fixture, &address, &listener),
            "创建 connect 后重复 shutdown listener");
    connector = create_stream(&fixture);
    REQUIRE(connector >= 0 &&
                    sys_shutdown(connector, SHUT_WR) == 0 &&
                    connect_socket(&fixture, connector, &address) == 0 &&
                    sys_shutdown(connector, SHUT_WR) == 0,
            "connect 后重复 SHUT_WR 形成一次可传播调用");
    accepted = sys_accept(listener, 0, 0);
    REQUIRE(accepted >= 0 &&
                    socket_ref_get_task(&fixture.task,
                            accepted, &accepted_ref) == 0,
            "accept 取得重复写关闭的 connector");
    unsigned char received = 0;
    EXPECT(socket_recvfrom_ref(&accepted_ref,
                    &received, sizeof(received),
                    MSG_DONTWAIT_, NULL) == 0,
            "connect 后重复 SHUT_WR 向 accepted peer 传播 EOF");

cleanup:
    if (accepted_ref.fd != NULL)
        socket_ref_release(&accepted_ref);
    fixture_destroy(&fixture);
}

int main(void) {
    signal(SIGALRM, timeout_handler);
    alarm(TEST_TIMEOUT_SECONDS);

    test_preaccept_connector_is_not_eof();
    test_pending_drain_and_closed_gate();
    test_blocked_accept_wakes_on_shutdown();
    test_preaccept_write_shutdown_propagates();
    test_preaccept_read_shutdown_propagates();
    test_preconnect_shutdown_is_not_propagated();
    test_repeated_shutdown_after_connect_is_propagated();

    alarm(0);
    if (failures != 0) {
        fprintf(stderr,
                "AF_UNIX listener shutdown 测试共有 %d 项失败\n",
                failures);
        return 1;
    }
    puts("AF_UNIX listener shutdown 测试通过");
    return 0;
}

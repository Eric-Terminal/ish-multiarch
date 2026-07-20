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

    REQUIRE(fd_close(pair[0]) == 0,
            "关闭发送端");
    pair[0] = NULL;
    int events = pair[1]->ops->poll(pair[1]);
    REQUIRE((events & (POLL_READ | POLL_HUP)) ==
                    (POLL_READ | POLL_HUP),
            "对端关闭持续呈现可读与挂断");

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

cleanup:
    if (scm != NULL)
        socket_scm_release(scm);
    if (pair[0] != NULL)
        fd_close(pair[0]);
    if (pair[1] != NULL)
        fd_close(pair[1]);
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

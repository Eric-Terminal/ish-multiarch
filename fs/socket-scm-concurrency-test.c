#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_SOCKETPAIR UINT32_C(0x1000)
#define STRESS_ITERATIONS 128
#define PAYLOAD_LENGTH 4

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define THREAD_WAIT_MS 10000
#define QUICK_WAIT_MS 5000
#else
#define THREAD_WAIT_MS 3000
#define QUICK_WAIT_MS 1000
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AF_UNIX SCM 并发测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct probe {
    atomic_uint close_calls;
};

struct fixture {
    struct task task;
    struct tgroup group;
    fd_t sockets[2];
    fd_t probe_numbers[2];
    struct fd *probes[2];
    struct probe probe_state[2];
    bool locks_initialized;
};

struct start_gate {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    unsigned ready;
    bool open;
};

struct receive_call {
    struct fixture *fixture;
    struct start_gate *gate;
    struct socket_ref socket;
    byte_t payload[PAYLOAD_LENGTH];
    dword_t message_flags;
    struct scm *scm;
    atomic_bool finished;
    ssize_t result;
};

struct blocking_receive_call {
    struct fixture *fixture;
    struct socket_ref socket;
    dword_t flags;
    bool signal_target;
    atomic_bool started;
    atomic_bool finished;
    byte_t payload;
    ssize_t result;
};

struct send_call {
    struct fixture *fixture;
    struct socket_ref socket;
    struct scm *scm;
    struct start_gate *gate;
    dword_t flags;
    bool signal_target;
    atomic_bool started;
    atomic_bool finished;
    byte_t payload[1024];
    size_t length;
    ssize_t result;
};

static int probe_close(struct fd *fd) {
    struct probe *probe = fd->data;
    atomic_fetch_add_explicit(
            &probe->close_calls, 1, memory_order_relaxed);
    return 0;
}

static const struct fd_ops probe_ops = {
    .close = probe_close,
};

static void host_wakeup_handler(int signal) {
    (void) signal;
}

static void fail_stuck_thread(const char *operation) {
    fprintf(stderr,
            "AF_UNIX SCM 并发测试失败：%s线程在救援后仍未退出\n",
            operation);
    fflush(stderr);
    _Exit(1);
}

static void clear_pending_signals(struct fixture *fixture) {
    lock(&fixture->task.sighand->lock);
    signal_flush_pending(&fixture->task);
    signal_flush_group_pending(&fixture->group);
    unlock(&fixture->task.sighand->lock);
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL &&
            !IS_ERR(fixture->task.files))
        fdtable_release(fixture->task.files);
    if (fixture->task.mm != NULL)
        mm_release(fixture->task.mm);
    if (fixture->task.sighand != NULL) {
        clear_pending_signals(fixture);
        sighand_release(fixture->task.sighand);
    }
    if (fixture->locks_initialized) {
        pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
    }
    current = NULL;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->sockets[0] = -1;
    fixture->sockets[1] = -1;
    fixture->probe_numbers[0] = -1;
    fixture->probe_numbers[1] = -1;
    lock_init(&fixture->group.lock);
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    lock_init(&fixture->task.waiting_cond_lock);
    lock_init(&fixture->task.ptrace.lock);
    list_init(&fixture->task.queue);
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->locks_initialized = true;
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {32, 32};
    fixture->task.group = &fixture->group;
    fixture->task.sighand = sighand_new();
    CHECK(fixture->task.sighand != NULL, "创建测试信号处理表");
    fixture->task.sighand->action[SIGUSR1_].handler =
            UINT32_C(0x1000);
    fixture->task.files = fdtable_new(8);
    CHECK(!IS_ERR(fixture->task.files), "创建测试 fd 表");

    struct mm *memory = mm_new();
    CHECK(memory != NULL, "创建测试地址空间");
    task_set_mm(&fixture->task, memory);
    write_wrlock(&fixture->task.mem->lock);
    int map_error = pt_map_nothing(
            fixture->task.mem, PAGE(USER_SOCKETPAIR), 1, P_RWX);
    write_wrunlock(&fixture->task.mem->lock);
    CHECK(map_error == 0, "映射 socketpair 写回页");

    current = &fixture->task;
    task_thread_store(&fixture->task, pthread_self());
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, fixture->sockets,
                    sizeof(fixture->sockets)) == 0,
            "创建 AF_UNIX 流 socketpair");

    for (unsigned index = 0; index < 2; index++) {
        atomic_init(&fixture->probe_state[index].close_calls, 0);
        struct fd *probe = fd_create(&probe_ops);
        CHECK(probe != NULL, "创建 SCM fd 身份探针");
        probe->data = &fixture->probe_state[index];
        fd_t number = f_install_task(&fixture->task, probe, 0);
        CHECK(number >= 0, "安装 SCM fd 身份探针");
        fixture->probes[index] = probe;
        fixture->probe_numbers[index] = number;
    }
    return true;
}

static void make_payload(
        byte_t payload[PAYLOAD_LENGTH], uint16_t sequence, byte_t marker) {
    payload[0] = marker;
    payload[1] = (byte_t) sequence;
    payload[2] = (byte_t) (sequence >> 8);
    payload[3] = payload[0] ^ payload[1] ^ payload[2];
}

static bool send_rights(struct fixture *fixture,
        const struct socket_ref *sender, unsigned probe_index,
        uint16_t sequence, byte_t marker) {
    byte_t payload[PAYLOAD_LENGTH];
    make_payload(payload, sequence, marker);
    struct scm *scm = NULL;
    int error = socket_scm_create_task(&fixture->task,
            &fixture->probe_numbers[probe_index], 1, &scm);
    if (error < 0)
        return false;
    ssize_t sent = socket_sendmsg_ref(sender,
            payload, sizeof(payload), 0, NULL, &scm);
    if (scm != NULL)
        socket_scm_release(scm);
    return sent == (ssize_t) sizeof(payload) && scm == NULL;
}

static bool start_gate_init(struct start_gate *gate) {
    memset(gate, 0, sizeof(*gate));
    if (pthread_mutex_init(&gate->lock, NULL) != 0)
        return false;
    if (pthread_cond_init(&gate->condition, NULL) != 0) {
        pthread_mutex_destroy(&gate->lock);
        return false;
    }
    return true;
}

static void start_gate_destroy(struct start_gate *gate) {
    pthread_cond_destroy(&gate->condition);
    pthread_mutex_destroy(&gate->lock);
}

static void start_gate_wait(struct start_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->ready++;
    pthread_cond_broadcast(&gate->condition);
    while (!gate->open)
        pthread_cond_wait(&gate->condition, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
}

static bool start_gate_open(struct start_gate *gate) {
    const struct timespec interval = {.tv_nsec = 1000000};
    bool ready = false;
    for (unsigned elapsed = 0; elapsed < THREAD_WAIT_MS; elapsed++) {
        pthread_mutex_lock(&gate->lock);
        ready = gate->ready == 2;
        pthread_mutex_unlock(&gate->lock);
        if (ready)
            break;
        nanosleep(&interval, NULL);
    }
    pthread_mutex_lock(&gate->lock);
    gate->open = true;
    pthread_cond_broadcast(&gate->condition);
    pthread_mutex_unlock(&gate->lock);
    return ready;
}

static void *receive_rights(void *opaque) {
    struct receive_call *call = opaque;
    current = &call->fixture->task;
    start_gate_wait(call->gate);
    call->result = socket_recvmsg_ref(&call->socket,
            call->payload, sizeof(call->payload), MSG_DONTWAIT_,
            NULL, &call->message_flags, &call->scm);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *receive_one_byte(void *opaque) {
    struct blocking_receive_call *call = opaque;
    current = &call->fixture->task;
    if (call->signal_target)
        task_thread_store(&call->fixture->task, pthread_self());
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = socket_recvfrom_ref(&call->socket,
            &call->payload, sizeof(call->payload), call->flags, NULL);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *send_one_right(void *opaque) {
    struct send_call *call = opaque;
    current = &call->fixture->task;
    if (call->signal_target)
        task_thread_store(&call->fixture->task, pthread_self());
    atomic_store_explicit(&call->started, true, memory_order_release);
    if (call->gate != NULL)
        start_gate_wait(call->gate);
    call->result = socket_sendmsg_ref(&call->socket,
            call->payload, call->length, call->flags,
            NULL, &call->scm);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
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

static void wait_for_nonblocking_thread(pthread_t thread,
        atomic_bool *finished, const char *operation) {
    if (wait_for_flag(finished, THREAD_WAIT_MS))
        return;
    (void) pthread_kill(thread, SIGUSR1);
    if (!wait_for_flag(finished, THREAD_WAIT_MS))
        fail_stuck_thread(operation);
}

static bool wait_for_poll_registration(
        struct task *task, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        lock(&task->waiting_cond_lock);
        bool registered = task->waiting_poll_active;
        unlock(&task->waiting_cond_lock);
        if (registered)
            return true;
        nanosleep(&interval, NULL);
    }
    lock(&task->waiting_cond_lock);
    bool registered = task->waiting_poll_active;
    unlock(&task->waiting_cond_lock);
    return registered;
}

static bool poll_registration_cleared(struct task *task) {
    lock(&task->waiting_cond_lock);
    bool cleared = !task->waiting_poll_active &&
            task->waiting_poll_notify_fd == -1;
    unlock(&task->waiting_cond_lock);
    return cleared;
}

static bool guest_signal_pending(struct fixture *fixture, int signal) {
    lock(&fixture->task.sighand->lock);
    bool pending = sigset_has(
            signal_pending_mask_locked(&fixture->task), signal);
    unlock(&fixture->task.sighand->lock);
    return pending;
}

static void queue_guest_signal_from_sender(
        struct fixture *fixture, int signal) {
    struct task *saved = current;
    current = NULL;
    deliver_signal(&fixture->task, signal, (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
    });
    current = saved;
}

static bool receive_matches(const struct receive_call *call,
        const struct fixture *fixture, uint16_t sequence) {
    if (call->result != PAYLOAD_LENGTH ||
            call->message_flags != 0 || call->scm == NULL ||
            call->scm->num_fds != 1)
        return false;
    unsigned probe_index;
    if (call->payload[0] == 'A')
        probe_index = 0;
    else if (call->payload[0] == 'B')
        probe_index = 1;
    else
        return false;
    byte_t expected[PAYLOAD_LENGTH];
    make_payload(expected, sequence, call->payload[0]);
    return memcmp(call->payload, expected, sizeof(expected)) == 0 &&
            call->scm->fds[0] == fixture->probes[probe_index];
}

static bool test_concurrent_pairing(struct fixture *fixture) {
    struct socket_ref sender;
    CHECK(socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &sender) == 0,
            "取得并发发送 socket 强引用");

    for (uint16_t sequence = 0;
            sequence < STRESS_ITERATIONS; sequence++) {
        unsigned first = sequence & 1;
        unsigned second = first ^ 1;
        byte_t markers[2] = {'A', 'B'};
        bool sent = send_rights(fixture, &sender, first,
                    sequence, markers[first]) &&
                send_rights(fixture, &sender, second,
                    sequence, markers[second]);
        if (!sent) {
            socket_ref_release(&sender);
            CHECK(false, "发送并发接收所需 SCM 消息");
        }

        struct start_gate gate;
        if (!start_gate_init(&gate)) {
            socket_ref_release(&sender);
            CHECK(false, "初始化并发接收闸门");
        }
        struct receive_call calls[2] = {
            {.fixture = fixture, .gate = &gate},
            {.fixture = fixture, .gate = &gate},
        };
        atomic_init(&calls[0].finished, false);
        atomic_init(&calls[1].finished, false);
        bool retained = socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &calls[0].socket) == 0 &&
                socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &calls[1].socket) == 0;
        if (!retained) {
            if (calls[0].socket.fd != NULL)
                socket_ref_release(&calls[0].socket);
            if (calls[1].socket.fd != NULL)
                socket_ref_release(&calls[1].socket);
            start_gate_destroy(&gate);
            socket_ref_release(&sender);
            CHECK(false, "取得两个并发接收 socket 强引用");
        }

        pthread_t threads[2];
        int first_error = pthread_create(
                &threads[0], NULL, receive_rights, &calls[0]);
        int second_error = first_error == 0 ? pthread_create(
                &threads[1], NULL, receive_rights, &calls[1]) : -1;
        if (first_error != 0 || second_error != 0) {
            pthread_mutex_lock(&gate.lock);
            gate.open = true;
            pthread_cond_broadcast(&gate.condition);
            pthread_mutex_unlock(&gate.lock);
            if (first_error == 0) {
                wait_for_nonblocking_thread(
                        threads[0], &calls[0].finished, "并发接收");
                pthread_join(threads[0], NULL);
            }
            if (second_error == 0) {
                wait_for_nonblocking_thread(
                        threads[1], &calls[1].finished, "并发接收");
                pthread_join(threads[1], NULL);
            }
            socket_ref_release(&calls[1].socket);
            socket_ref_release(&calls[0].socket);
            start_gate_destroy(&gate);
            socket_ref_release(&sender);
            CHECK(false, "创建两个并发接收线程");
        }

        bool gate_ready = start_gate_open(&gate);
        wait_for_nonblocking_thread(
                threads[0], &calls[0].finished, "并发接收");
        wait_for_nonblocking_thread(
                threads[1], &calls[1].finished, "并发接收");
        pthread_join(threads[0], NULL);
        pthread_join(threads[1], NULL);
        socket_ref_release(&calls[1].socket);
        socket_ref_release(&calls[0].socket);
        start_gate_destroy(&gate);

        bool matched = gate_ready &&
                receive_matches(&calls[0], fixture, sequence) &&
                receive_matches(&calls[1], fixture, sequence) &&
                calls[0].payload[0] != calls[1].payload[0];
        if (calls[0].scm != NULL)
            socket_scm_release(calls[0].scm);
        if (calls[1].scm != NULL)
            socket_scm_release(calls[1].scm);
        if (!matched) {
            socket_ref_release(&sender);
            CHECK(false, "并发接收始终保持 payload 与 SCM fd 一一对应");
        }
    }

    socket_ref_release(&sender);
    return true;
}

static bool test_concurrent_sends_keep_pairing(struct fixture *fixture) {
    struct socket_ref senders[2] = {0};
    struct socket_ref receiver = {0};
    CHECK(socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &senders[0]) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &senders[1]) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &receiver) == 0,
            "取得两个并发发送与同一接收端的强引用");

    struct start_gate gate;
    CHECK(start_gate_init(&gate), "初始化并发发送闸门");
    const uint16_t sequence = UINT16_C(0xfffd);
    struct send_call calls[2] = {
        {
            .fixture = fixture,
            .socket = senders[0],
            .gate = &gate,
            .flags = MSG_DONTWAIT_ | MSG_NOSIGNAL_,
            .length = PAYLOAD_LENGTH,
            .result = _EIO,
        },
        {
            .fixture = fixture,
            .socket = senders[1],
            .gate = &gate,
            .flags = MSG_DONTWAIT_ | MSG_NOSIGNAL_,
            .length = PAYLOAD_LENGTH,
            .result = _EIO,
        },
    };
    make_payload(calls[0].payload, sequence, 'A');
    make_payload(calls[1].payload, sequence, 'B');
    for (unsigned index = 0; index < 2; index++) {
        atomic_init(&calls[index].started, false);
        atomic_init(&calls[index].finished, false);
    }
    bool scm_created = socket_scm_create_task(&fixture->task,
                    &fixture->probe_numbers[0], 1, &calls[0].scm) == 0 &&
            socket_scm_create_task(&fixture->task,
                    &fixture->probe_numbers[1], 1, &calls[1].scm) == 0;

    pthread_t threads[2];
    int first_error = scm_created ? pthread_create(
            &threads[0], NULL, send_one_right, &calls[0]) : -1;
    int second_error = first_error == 0 ? pthread_create(
            &threads[1], NULL, send_one_right, &calls[1]) : -1;
    bool gate_ready = false;
    if (first_error == 0 && second_error == 0) {
        gate_ready = start_gate_open(&gate);
    } else {
        pthread_mutex_lock(&gate.lock);
        gate.open = true;
        pthread_cond_broadcast(&gate.condition);
        pthread_mutex_unlock(&gate.lock);
    }
    if (first_error == 0) {
        wait_for_nonblocking_thread(
                threads[0], &calls[0].finished, "并发发送");
        pthread_join(threads[0], NULL);
    }
    if (second_error == 0) {
        wait_for_nonblocking_thread(
                threads[1], &calls[1].finished, "并发发送");
        pthread_join(threads[1], NULL);
    }

    struct receive_call received[2] = {
        {.fixture = fixture, .socket = receiver},
        {.fixture = fixture, .socket = receiver},
    };
    for (unsigned index = 0; index < 2; index++) {
        received[index].result = socket_recvmsg_ref(
                &received[index].socket,
                received[index].payload, sizeof(received[index].payload),
                MSG_DONTWAIT_, NULL, &received[index].message_flags,
                &received[index].scm);
    }
    bool matched = scm_created && gate_ready &&
            first_error == 0 && second_error == 0 &&
            calls[0].result == PAYLOAD_LENGTH &&
            calls[1].result == PAYLOAD_LENGTH &&
            calls[0].scm == NULL && calls[1].scm == NULL &&
            receive_matches(&received[0], fixture, sequence) &&
            receive_matches(&received[1], fixture, sequence) &&
            received[0].payload[0] != received[1].payload[0];
    for (unsigned index = 0; index < 2; index++) {
        if (received[index].scm != NULL)
            socket_scm_release(received[index].scm);
        if (calls[index].scm != NULL)
            socket_scm_release(calls[index].scm);
    }
    start_gate_destroy(&gate);
    socket_ref_release(&receiver);
    socket_ref_release(&senders[1]);
    socket_ref_release(&senders[0]);
    CHECK(matched,
            "同一接收端的两个并发 SCM send 保持 payload 与 fd 配对");
    return true;
}

static bool test_discard_does_not_pollute_next_message(
        struct fixture *fixture) {
    struct socket_ref sender;
    struct socket_ref receiver;
    CHECK(socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &sender) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &receiver) == 0,
            "取得 ancillary 丢弃测试 socket 强引用");

    const uint16_t sequence = UINT16_C(0xfffe);
    byte_t expected_discarded[PAYLOAD_LENGTH];
    byte_t discarded[PAYLOAD_LENGTH] = {0};
    make_payload(expected_discarded, sequence, 'A');
    CHECK(send_rights(fixture, &sender, 0, sequence, 'A') &&
            socket_recvfrom_ref(&receiver,
                    discarded, sizeof(discarded), MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(discarded) &&
            memcmp(discarded, expected_discarded,
                    sizeof(expected_discarded)) == 0,
            "普通 recv 消费并丢弃内部 SCM");

    CHECK(send_rights(fixture, &sender, 1, sequence, 'B'),
            "发送 ancillary 丢弃后的下一条 SCM 消息");
    struct receive_call received = {
        .fixture = fixture,
        .socket = receiver,
    };
    received.result = socket_recvmsg_ref(&received.socket,
            received.payload, sizeof(received.payload), MSG_DONTWAIT_,
            NULL, &received.message_flags, &received.scm);
    bool matched = receive_matches(&received, fixture, sequence) &&
            received.payload[0] == 'B';
    if (received.scm != NULL)
        socket_scm_release(received.scm);

    byte_t unexpected = 0;
    bool drained = socket_recvfrom_ref(&receiver,
            &unexpected, sizeof(unexpected), MSG_DONTWAIT_, NULL) ==
            _EAGAIN;
    socket_ref_release(&receiver);
    socket_ref_release(&sender);
    CHECK(matched && drained,
            "丢弃的 SCM 不会串入下一条 recvmsg 且队列已排空");
    return true;
}

static bool test_blocking_receive_does_not_block_nonblocking_peer(
        struct fixture *fixture) {
    struct socket_ref sender;
    struct blocking_receive_call calls[2] = {
        {
            .fixture = fixture,
            .flags = 0,
            .signal_target = true,
            .result = _EIO,
        },
        {.fixture = fixture, .flags = MSG_DONTWAIT_, .result = _EIO},
    };
    atomic_init(&calls[0].started, false);
    atomic_init(&calls[0].finished, false);
    atomic_init(&calls[1].started, false);
    atomic_init(&calls[1].finished, false);
    CHECK(socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &sender) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &calls[0].socket) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &calls[1].socket) == 0,
            "取得阻塞与非阻塞接收 socket 强引用");

    pthread_t threads[2];
    bool blocking_started = pthread_create(
            &threads[0], NULL, receive_one_byte, &calls[0]) == 0;
    bool blocking_waiting = blocking_started &&
            wait_for_poll_registration(&fixture->task, THREAD_WAIT_MS);
    bool nonblocking_started = blocking_waiting && pthread_create(
            &threads[1], NULL, receive_one_byte, &calls[1]) == 0;
    bool returned_before_send = nonblocking_started &&
            wait_for_flag(&calls[1].finished, QUICK_WAIT_MS);

    const byte_t wake_payload = 0x7d;
    ssize_t sent = blocking_started ? socket_sendto_ref(&sender,
            &wake_payload, sizeof(wake_payload), 0, NULL) : _EIO;
    bool blocking_finished = !blocking_started ||
            wait_for_flag(&calls[0].finished, THREAD_WAIT_MS);
    if (blocking_started && !blocking_finished) {
        queue_guest_signal_from_sender(fixture, SIGUSR1_);
        blocking_finished = wait_for_flag(
                &calls[0].finished, THREAD_WAIT_MS);
    }
    if (blocking_started && !blocking_finished) {
        (void) shutdown(calls[0].socket.fd->real_fd, SHUT_RDWR);
        blocking_finished = wait_for_flag(
                &calls[0].finished, THREAD_WAIT_MS);
    }
    if (blocking_started && !blocking_finished)
        fail_stuck_thread("阻塞接收");
    if (nonblocking_started &&
            !wait_for_flag(&calls[1].finished, THREAD_WAIT_MS)) {
        (void) shutdown(calls[1].socket.fd->real_fd, SHUT_RDWR);
        if (!wait_for_flag(&calls[1].finished, THREAD_WAIT_MS))
            fail_stuck_thread("非阻塞接收");
    }
    if (blocking_started) {
        pthread_join(threads[0], NULL);
        task_thread_store(&fixture->task, pthread_self());
    }
    if (nonblocking_started)
        pthread_join(threads[1], NULL);
    clear_pending_signals(fixture);
    socket_ref_release(&calls[1].socket);
    socket_ref_release(&calls[0].socket);
    socket_ref_release(&sender);

    CHECK(blocking_waiting && returned_before_send && blocking_finished &&
                    calls[1].result == _EAGAIN &&
                    sent == 1 && calls[0].result == 1 &&
                    calls[0].payload == wake_payload,
            "阻塞 recv 等待期间 MSG_DONTWAIT 立即返回 EAGAIN");
    return true;
}

static bool test_guest_signal_interrupts_blocking_receive(
        struct fixture *fixture) {
    struct socket_ref sender = {0};
    struct blocking_receive_call blocked = {
        .fixture = fixture,
        .flags = 0,
        .signal_target = true,
        .result = _EIO,
    };
    atomic_init(&blocked.started, false);
    atomic_init(&blocked.finished, false);
    CHECK(socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &sender) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &blocked.socket) == 0,
            "取得 guest 信号中断接收所需 socket 强引用");

    unsigned references_before[2] = {
        atomic_load_explicit(
                &fixture->probes[0]->refcount, memory_order_relaxed),
        atomic_load_explicit(
                &fixture->probes[1]->refcount, memory_order_relaxed),
    };
    pthread_t thread;
    bool started = pthread_create(
            &thread, NULL, receive_one_byte, &blocked) == 0;
    bool waiting = started && wait_for_poll_registration(
            &fixture->task, THREAD_WAIT_MS);
    if (started)
        queue_guest_signal_from_sender(fixture, SIGUSR1_);
    bool finished = started &&
            wait_for_flag(&blocked.finished, THREAD_WAIT_MS);
    if (started && !finished) {
        (void) shutdown(blocked.socket.fd->real_fd, SHUT_RDWR);
        finished = wait_for_flag(&blocked.finished, THREAD_WAIT_MS);
    }
    if (started && !finished)
        fail_stuck_thread("guest 信号中断接收");
    if (started) {
        pthread_join(thread, NULL);
        task_thread_store(&fixture->task, pthread_self());
    }

    bool pending = guest_signal_pending(fixture, SIGUSR1_);
    bool registration_cleared = poll_registration_cleared(&fixture->task);
    bool references_unchanged =
            atomic_load_explicit(&fixture->probes[0]->refcount,
                    memory_order_relaxed) == references_before[0] &&
            atomic_load_explicit(&fixture->probes[1]->refcount,
                    memory_order_relaxed) == references_before[1];
    clear_pending_signals(fixture);

    const uint16_t sequence = UINT16_C(0xfffc);
    bool followup_sent = send_rights(
            fixture, &sender, 0, sequence, 'A');
    struct receive_call received = {
        .fixture = fixture,
        .socket = blocked.socket,
    };
    received.result = socket_recvmsg_ref(&received.socket,
            received.payload, sizeof(received.payload), MSG_DONTWAIT_,
            NULL, &received.message_flags, &received.scm);
    bool followup_matched = followup_sent &&
            receive_matches(&received, fixture, sequence) &&
            received.payload[0] == 'A';
    if (received.scm != NULL)
        socket_scm_release(received.scm);
    bool followup_released =
            atomic_load_explicit(&fixture->probes[0]->refcount,
                    memory_order_relaxed) == references_before[0];
    socket_ref_release(&blocked.socket);
    socket_ref_release(&sender);

    CHECK(started && waiting && finished && blocked.result == _EINTR &&
                    pending && registration_cleared && references_unchanged &&
                    followup_matched && followup_released,
            "guest 信号令阻塞 recv 返回 EINTR 并清理等待与 SCM 状态");
    return true;
}

static bool fill_host_send_buffer(
        int socket, const byte_t *filler, size_t length,
        size_t *filled_bytes) {
    *filled_bytes = 0;
    for (unsigned attempt = 0; attempt < 65536; attempt++) {
        ssize_t sent = send(socket, filler, length, MSG_DONTWAIT);
        if (sent > 0) {
            *filled_bytes += (size_t) sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            attempt--;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ENOBUFS)) {
            sent = send(socket, filler, 1, MSG_DONTWAIT);
            if (sent > 0) {
                *filled_bytes += (size_t) sent;
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                attempt--;
                continue;
            }
            return sent < 0 && (errno == EAGAIN ||
                    errno == EWOULDBLOCK || errno == ENOBUFS);
        }
        return false;
    }
    return false;
}

static bool drain_host_bytes(int socket, size_t expected) {
    size_t drained = 0;
    byte_t buffer[1024];
    while (drained < expected) {
        size_t remaining = expected - drained;
        size_t capacity = remaining < sizeof(buffer) ?
                remaining : sizeof(buffer);
        ssize_t received = recv(socket, buffer, capacity, MSG_DONTWAIT);
        if (received > 0) {
            drained += (size_t) received;
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool test_guest_signal_interrupts_blocking_send(
        struct fixture *fixture) {
    fd_t pair[2] = {-1, -1};
    current = &fixture->task;
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 guest 信号中断发送专用 AF_UNIX 流 socketpair");

    struct socket_ref sender = {0};
    struct socket_ref receiver = {0};
    CHECK(socket_ref_get_task(&fixture->task, pair[0], &sender) == 0 &&
            socket_ref_get_task(&fixture->task, pair[1], &receiver) == 0,
            "取得 guest 信号中断发送所需 socket 强引用");
    int send_buffer_size = 8192;
    int original_status = fcntl(sender.fd->real_fd, F_GETFL);
    bool buffer_configured = original_status >= 0 &&
            fcntl(sender.fd->real_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0 &&
            setsockopt(sender.fd->real_fd,
                    SOL_SOCKET, SO_SNDBUF,
                    &send_buffer_size, sizeof(send_buffer_size)) == 0;
    byte_t filler[1024];
    memset(filler, 0x69, sizeof(filler));
    size_t filler_bytes = 0;
    bool buffer_full = buffer_configured && fill_host_send_buffer(
            sender.fd->real_fd, filler, sizeof(filler), &filler_bytes);
    bool blocking_restored = original_status >= 0 &&
            fcntl(sender.fd->real_fd, F_SETFL, original_status) == 0;

    unsigned references_before[2] = {
        atomic_load_explicit(
                &fixture->probes[0]->refcount, memory_order_relaxed),
        atomic_load_explicit(
                &fixture->probes[1]->refcount, memory_order_relaxed),
    };
    struct send_call blocked = {
        .fixture = fixture,
        .socket = sender,
        .flags = MSG_NOSIGNAL_,
        .signal_target = true,
        .length = sizeof(filler),
        .result = _EIO,
    };
    memset(blocked.payload, 0x96, blocked.length);
    atomic_init(&blocked.started, false);
    atomic_init(&blocked.finished, false);
    bool scm_created = buffer_full && blocking_restored &&
            socket_scm_create_task(&fixture->task,
                    &fixture->probe_numbers[0], 1, &blocked.scm) == 0;
    bool reference_held = scm_created &&
            atomic_load_explicit(&fixture->probes[0]->refcount,
                    memory_order_relaxed) == references_before[0] + 1;

    pthread_t thread;
    bool started = scm_created && pthread_create(
            &thread, NULL, send_one_right, &blocked) == 0;
    bool waiting = started && wait_for_poll_registration(
            &fixture->task, THREAD_WAIT_MS);
    if (started)
        queue_guest_signal_from_sender(fixture, SIGUSR1_);
    bool finished = started &&
            wait_for_flag(&blocked.finished, THREAD_WAIT_MS);
    if (started && !finished) {
        (void) shutdown(sender.fd->real_fd, SHUT_RDWR);
        finished = wait_for_flag(&blocked.finished, THREAD_WAIT_MS);
    }
    if (started && !finished)
        fail_stuck_thread("guest 信号中断发送");
    if (started) {
        pthread_join(thread, NULL);
        task_thread_store(&fixture->task, pthread_self());
    }

    bool pending = guest_signal_pending(fixture, SIGUSR1_);
    bool registration_cleared = poll_registration_cleared(&fixture->task);
    bool ownership_retained = blocked.scm != NULL &&
            atomic_load_explicit(&fixture->probes[0]->refcount,
                    memory_order_relaxed) == references_before[0] + 1;
    clear_pending_signals(fixture);
    if (blocked.scm != NULL) {
        socket_scm_release(blocked.scm);
        blocked.scm = NULL;
    }
    bool interrupted_reference_released =
            atomic_load_explicit(&fixture->probes[0]->refcount,
                    memory_order_relaxed) == references_before[0];
    bool drained_filler = drain_host_bytes(
            receiver.fd->real_fd, filler_bytes);

    const uint16_t sequence = UINT16_C(0xfffb);
    bool followup_sent = drained_filler && send_rights(
            fixture, &sender, 1, sequence, 'B');
    struct receive_call received = {
        .fixture = fixture,
        .socket = receiver,
    };
    received.result = followup_sent ? socket_recvmsg_ref(
            &received.socket, received.payload, sizeof(received.payload),
            MSG_DONTWAIT_, NULL, &received.message_flags,
            &received.scm) : _EIO;
    bool followup_matched = followup_sent &&
            receive_matches(&received, fixture, sequence) &&
            received.payload[0] == 'B';
    if (received.scm != NULL)
        socket_scm_release(received.scm);
    bool followup_released =
            atomic_load_explicit(&fixture->probes[1]->refcount,
                    memory_order_relaxed) == references_before[1];

    socket_ref_release(&receiver);
    socket_ref_release(&sender);
    bool pair_closed = f_close_task(&fixture->task, pair[1]) == 0 &&
            f_close_task(&fixture->task, pair[0]) == 0;
    CHECK(buffer_full && filler_bytes != 0 && blocking_restored &&
                    scm_created && reference_held && started && waiting &&
                    finished && blocked.result == _EINTR && pending &&
                    registration_cleared && ownership_retained &&
                    interrupted_reference_released && drained_filler &&
                    followup_matched && followup_released && pair_closed,
            "guest 信号令阻塞 send 返回 EINTR 并把 SCM 所有权交还调用方");
    return true;
}

static bool test_blocking_send_releases_global_scm_lock(
        struct fixture *fixture) {
    fd_t blocked_pair[2] = {-1, -1};
    current = &fixture->task;
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, blocked_pair,
                    sizeof(blocked_pair)) == 0,
            "创建阻塞发送专用 AF_UNIX 流 socketpair");

    struct socket_ref blocked_sender = {0};
    struct socket_ref blocked_receiver = {0};
    struct socket_ref independent_sender = {0};
    struct socket_ref independent_receiver = {0};
    bool retained = socket_ref_get_task(&fixture->task,
                    blocked_pair[0], &blocked_sender) == 0 &&
            socket_ref_get_task(&fixture->task,
                    blocked_pair[1], &blocked_receiver) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[0], &independent_sender) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->sockets[1], &independent_receiver) == 0;
    if (!retained) {
        if (blocked_sender.fd != NULL)
            socket_ref_release(&blocked_sender);
        if (blocked_receiver.fd != NULL)
            socket_ref_release(&blocked_receiver);
        if (independent_sender.fd != NULL)
            socket_ref_release(&independent_sender);
        if (independent_receiver.fd != NULL)
            socket_ref_release(&independent_receiver);
        CHECK(false, "取得阻塞与独立发送 socket 强引用");
    }

    int send_buffer_size = 8192;
    int original_status = fcntl(blocked_sender.fd->real_fd, F_GETFL);
    bool buffer_configured = original_status >= 0 &&
            fcntl(blocked_sender.fd->real_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0 &&
            setsockopt(blocked_sender.fd->real_fd,
                    SOL_SOCKET, SO_SNDBUF,
                    &send_buffer_size, sizeof(send_buffer_size)) == 0;
    byte_t filler[1024];
    memset(filler, 0xa5, sizeof(filler));
    size_t filler_bytes = 0;
    bool buffer_full = buffer_configured && fill_host_send_buffer(
            blocked_sender.fd->real_fd, filler, sizeof(filler),
            &filler_bytes);
    bool blocking_restored = original_status >= 0 &&
            fcntl(blocked_sender.fd->real_fd,
                    F_SETFL, original_status) == 0;

    bool apple_nonblocking_eagain = true;
#ifdef __APPLE__
    // Darwin 的流 socket 单次 MSG_DONTWAIT 不能作为防挂死边界，探针同时
    // 设置宿主 O_NONBLOCK；被测接口仍须精确翻译为 Linux EAGAIN。
    const byte_t extra = 0x3a;
    bool probe_nonblocking = blocking_restored &&
            fcntl(blocked_sender.fd->real_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0;
    ssize_t nonblocking_result = buffer_full && probe_nonblocking ?
            socket_sendto_ref(
            &blocked_sender, &extra, sizeof(extra),
            MSG_DONTWAIT_ | MSG_NOSIGNAL_, NULL) : _EIO;
    bool probe_restored = original_status >= 0 &&
            fcntl(blocked_sender.fd->real_fd,
                    F_SETFL, original_status) == 0;
    apple_nonblocking_eagain = probe_nonblocking && probe_restored &&
            nonblocking_result == _EAGAIN;
    if (nonblocking_result > 0)
        filler_bytes += (size_t) nonblocking_result;
#endif

    struct send_call blocked = {
        .fixture = fixture,
        .socket = blocked_sender,
        .flags = MSG_NOSIGNAL_,
        .signal_target = true,
        .length = sizeof(filler),
        .result = _EIO,
    };
    struct send_call independent = {
        .fixture = fixture,
        .socket = independent_sender,
        .flags = MSG_DONTWAIT_ | MSG_NOSIGNAL_,
        .payload = {0xc2},
        .length = 1,
        .result = _EIO,
    };
    memset(blocked.payload, 0xb1, blocked.length);
    atomic_init(&blocked.started, false);
    atomic_init(&blocked.finished, false);
    atomic_init(&independent.started, false);
    atomic_init(&independent.finished, false);
    bool scm_created = buffer_full && blocking_restored &&
            socket_scm_create_task(&fixture->task,
                    &fixture->probe_numbers[0], 1, &blocked.scm) == 0 &&
            socket_scm_create_task(&fixture->task,
                    &fixture->probe_numbers[1], 1, &independent.scm) == 0;

    pthread_t blocked_thread;
    pthread_t independent_thread;
    bool blocked_started = scm_created && pthread_create(
            &blocked_thread, NULL, send_one_right, &blocked) == 0;
    bool blocked_waiting = blocked_started &&
            wait_for_poll_registration(&fixture->task, THREAD_WAIT_MS);
    bool independent_started = blocked_waiting && pthread_create(
            &independent_thread, NULL, send_one_right, &independent) == 0;
    bool independent_finished_before_drain = independent_started &&
            wait_for_flag(&independent.finished, QUICK_WAIT_MS);

    bool drained_filler = drain_host_bytes(
            blocked_receiver.fd->real_fd, filler_bytes);
    bool blocked_finished = blocked_started &&
            wait_for_flag(&blocked.finished, THREAD_WAIT_MS);
    if (!blocked_finished) {
        queue_guest_signal_from_sender(fixture, SIGUSR1_);
        blocked_finished = wait_for_flag(
                &blocked.finished, THREAD_WAIT_MS);
    }
    if (!blocked_finished) {
        (void) shutdown(blocked_sender.fd->real_fd, SHUT_RDWR);
        blocked_finished = wait_for_flag(
                &blocked.finished, THREAD_WAIT_MS);
    }
    if (blocked_started && !blocked_finished)
        fail_stuck_thread("阻塞发送");
    bool independent_finished = !independent_started ||
            wait_for_flag(&independent.finished, THREAD_WAIT_MS);
    if (independent_started && !independent_finished) {
        (void) shutdown(independent_sender.fd->real_fd, SHUT_RDWR);
        independent_finished = wait_for_flag(
                &independent.finished, THREAD_WAIT_MS);
    }
    if (independent_started && !independent_finished)
        fail_stuck_thread("独立非阻塞发送");
    if (blocked_started) {
        pthread_join(blocked_thread, NULL);
        task_thread_store(&fixture->task, pthread_self());
    }
    if (independent_started)
        pthread_join(independent_thread, NULL);
    clear_pending_signals(fixture);

    byte_t received_payload[sizeof(filler)] = {0};
    ssize_t blocked_received = blocked.result == (ssize_t) blocked.length ?
            socket_recvfrom_ref(&blocked_receiver,
                    received_payload, sizeof(received_payload),
                    MSG_DONTWAIT_, NULL) : _EIO;
    bool blocked_payload_matches = blocked_received ==
            (ssize_t) blocked.length &&
            memcmp(received_payload, blocked.payload, blocked.length) == 0;
    memset(received_payload, 0, sizeof(received_payload));
    ssize_t independent_received = independent.result == 1 ?
            socket_recvfrom_ref(&independent_receiver,
                    received_payload, 1,
                    MSG_DONTWAIT_, NULL) : _EIO;
    bool independent_payload_matches = independent_received == 1 &&
            received_payload[0] == independent.payload[0];

    if (blocked.scm != NULL)
        socket_scm_release(blocked.scm);
    if (independent.scm != NULL)
        socket_scm_release(independent.scm);
    socket_ref_release(&independent_receiver);
    socket_ref_release(&independent_sender);
    socket_ref_release(&blocked_receiver);
    socket_ref_release(&blocked_sender);
    bool pair_closed = f_close_task(&fixture->task, blocked_pair[1]) == 0 &&
            f_close_task(&fixture->task, blocked_pair[0]) == 0;

    CHECK(buffer_full && filler_bytes != 0 && blocking_restored &&
                    apple_nonblocking_eagain && scm_created &&
                    blocked_waiting && independent_finished_before_drain &&
                    drained_filler && independent_finished &&
                    blocked_finished && blocked.result ==
                            (ssize_t) blocked.length &&
                    independent.result == 1 && blocked_payload_matches &&
                    independent_payload_matches && pair_closed,
            "阻塞 SCM send 等待期间无关 socket 可立即发送并保持消息配对");
    return true;
}

static bool close_descriptors_without_retained_scm(
        struct fixture *fixture) {
    CHECK(f_close_task(&fixture->task,
                    fixture->probe_numbers[0]) == 0 &&
            f_close_task(&fixture->task,
                    fixture->probe_numbers[1]) == 0,
            "关闭原始身份探针描述符");
    fixture->probe_numbers[0] = -1;
    fixture->probe_numbers[1] = -1;
    CHECK(atomic_load_explicit(&fixture->probe_state[0].close_calls,
                    memory_order_relaxed) == 1 &&
            atomic_load_explicit(&fixture->probe_state[1].close_calls,
                    memory_order_relaxed) == 1,
            "并发与丢弃路径均未遗留 SCM 强引用");
    CHECK(f_close_task(&fixture->task, fixture->sockets[1]) == 0 &&
            f_close_task(&fixture->task, fixture->sockets[0]) == 0,
            "关闭 AF_UNIX 流 socketpair");
    fixture->sockets[0] = -1;
    fixture->sockets[1] = -1;
    return true;
}

int main(void) {
    struct sigaction host_action = {0};
    struct sigaction old_host_action;
    host_action.sa_handler = host_wakeup_handler;
    sigemptyset(&host_action.sa_mask);
    if (sigaction(SIGUSR1, &host_action, &old_host_action) != 0) {
        fprintf(stderr,
                "AF_UNIX SCM 并发测试失败：安装 host 唤醒信号处理器\n");
        return 1;
    }

    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        sigaction(SIGUSR1, &old_host_action, NULL);
        return 1;
    }

    bool passed = test_concurrent_pairing(&fixture) &&
            test_concurrent_sends_keep_pairing(&fixture) &&
            test_discard_does_not_pollute_next_message(&fixture) &&
            test_blocking_receive_does_not_block_nonblocking_peer(&fixture) &&
            test_guest_signal_interrupts_blocking_receive(&fixture) &&
            test_guest_signal_interrupts_blocking_send(&fixture) &&
            test_blocking_send_releases_global_scm_lock(&fixture) &&
            close_descriptors_without_retained_scm(&fixture);
    fixture_destroy(&fixture);
    sigaction(SIGUSR1, &old_host_action, NULL);
    if (!passed)
        return 1;
    if (atomic_load_explicit(&fixture.probe_state[0].close_calls,
                    memory_order_relaxed) != 1 ||
            atomic_load_explicit(&fixture.probe_state[1].close_calls,
                    memory_order_relaxed) != 1) {
        fprintf(stderr, "AF_UNIX SCM 并发测试失败：身份探针析构次数异常\n");
        return 1;
    }
    return 0;
}

#include "platform/apple-watch-runtime.h"

#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>

#include "debug.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/sock.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "platform/apple-resolver.h"

#define WATCH_OUTPUT_CAPACITY (64 * 1024)
#define WATCH_CONSOLE_NUMBER 1
#define WATCH_DEFAULT_COLUMNS 40
#define WATCH_DEFAULT_ROWS 18
#define WATCH_COMMAND_LIMIT 4096
#define WATCH_NO_TTY (-1)

enum watch_session_internal_phase {
    WATCH_SESSION_FREE = 0,
};

struct watch_output_ring {
    unsigned char bytes[WATCH_OUTPUT_CAPACITY];
    size_t head;
    size_t count;
    uint64_t dropped;
};

struct watch_session {
    ish_watch_session_id id;
    int32_t phase;
    int32_t wait_status;
    struct tgroup *leader_group;
    int tty_number;
    bool client_closed;
    struct watch_output_ring output;
};

struct watch_session_transport {
    ish_watch_session_id id;
    struct watch_session *session;
    int tty_number;
};

static _Atomic int runtime_phase = ISH_WATCH_RUNTIME_IDLE;
static _Atomic int runtime_error;
static _Atomic bool runtime_accepts_sessions;

static struct watch_output_ring console_output;
static lock_t output_lock = LOCK_INITIALIZER;

static struct watch_session sessions[ISH_WATCH_SESSION_LIMIT];
static ish_watch_session_id next_session_id = 1;
static lock_t sessions_lock = LOCK_INITIALIZER;
static lock_t session_create_lock = LOCK_INITIALIZER;
static _Thread_local struct watch_session *opening_session;

static void output_note_dropped(
        struct watch_output_ring *output, size_t length) {
    uint64_t increment = (uint64_t) length;
    if (UINT64_MAX - output->dropped < increment)
        output->dropped = UINT64_MAX;
    else
        output->dropped += increment;
}

static void output_append_locked(
        struct watch_output_ring *output,
        const void *bytes,
        size_t length) {
    if (length == 0)
        return;
    const unsigned char *source = bytes;

    if (length >= sizeof(output->bytes)) {
        output_note_dropped(output, output->count);
        output_note_dropped(output, length - sizeof(output->bytes));
        source += length - sizeof(output->bytes);
        length = sizeof(output->bytes);
        memcpy(output->bytes, source, length);
        output->head = 0;
        output->count = length;
        return;
    }

    size_t overflow = output->count + length > sizeof(output->bytes) ?
            output->count + length - sizeof(output->bytes) : 0;
    output_note_dropped(output, overflow);
    output->head = (output->head + overflow) % sizeof(output->bytes);
    output->count -= overflow;

    size_t tail = (output->head + output->count) % sizeof(output->bytes);
    size_t first = sizeof(output->bytes) - tail;
    if (first > length)
        first = length;
    memcpy(output->bytes + tail, source, first);
    memcpy(output->bytes, source + first, length - first);
    output->count += length;
}

static size_t output_read_locked(
        struct watch_output_ring *output,
        void *buffer,
        size_t capacity,
        uint64_t *dropped_bytes) {
    if (dropped_bytes != NULL) {
        *dropped_bytes = output->dropped;
        output->dropped = 0;
    }
    if (buffer == NULL || capacity == 0)
        return 0;

    size_t length = output->count < capacity ?
            output->count : capacity;
    size_t first = sizeof(output->bytes) - output->head;
    if (first > length)
        first = length;
    memcpy(buffer, output->bytes + output->head, first);
    memcpy((unsigned char *) buffer + first, output->bytes, length - first);
    output->head = (output->head + length) % sizeof(output->bytes);
    output->count -= length;
    return length;
}

static void output_append(const void *bytes, size_t length) {
    lock(&output_lock);
    output_append_locked(&console_output, bytes, length);
    unlock(&output_lock);
}

size_t ish_watch_runtime_read_output(
        void *buffer, size_t capacity, uint64_t *dropped_bytes) {
    lock(&output_lock);
    size_t length = output_read_locked(
            &console_output, buffer, capacity, dropped_bytes);
    unlock(&output_lock);
    return length;
}

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
void ish_watch_runtime_test_append_output(const void *bytes, size_t length) {
    output_append(bytes, length);
}
#endif

static struct watch_session *session_find_locked(
        ish_watch_session_id session_id) {
    if (session_id == 0)
        return NULL;
    for (size_t index = 0; index < ISH_WATCH_SESSION_LIMIT; index++) {
        struct watch_session *session = &sessions[index];
        if (session->phase != WATCH_SESSION_FREE &&
                session->id == session_id)
            return session;
    }
    return NULL;
}

static void session_reset_locked(struct watch_session *session) {
    session->id = 0;
    session->phase = WATCH_SESSION_FREE;
    session->wait_status = 0;
    session->leader_group = NULL;
    session->tty_number = WATCH_NO_TTY;
    session->client_closed = false;
    session->output.head = 0;
    session->output.count = 0;
    session->output.dropped = 0;
}

static void session_release_if_finished_locked(
        struct watch_session *session) {
    if (session->client_closed &&
            session->phase == ISH_WATCH_SESSION_EXITED &&
            session->tty_number == WATCH_NO_TTY)
        session_reset_locked(session);
}

static void session_mark_exited_locked(
        struct watch_session *session, int32_t wait_status) {
    session->leader_group = NULL;
    session->phase = ISH_WATCH_SESSION_EXITED;
    session->wait_status = wait_status;
    session_release_if_finished_locked(session);
}

static int session_reserve_locked(
        int32_t phase, struct watch_session **reserved_session) {
    if (next_session_id == UINT64_MAX)
        return _EOVERFLOW;

    struct watch_session *session = NULL;
    for (size_t index = 0; index < ISH_WATCH_SESSION_LIMIT; index++) {
        session_release_if_finished_locked(&sessions[index]);
        if (sessions[index].phase == WATCH_SESSION_FREE) {
            session = &sessions[index];
            break;
        }
    }
    if (session == NULL)
        return _EMFILE;

    session->id = next_session_id++;
    session->phase = phase;
    session->wait_status = 0;
    session->leader_group = NULL;
    session->tty_number = WATCH_NO_TTY;
    session->client_closed = false;
    session->output.head = 0;
    session->output.count = 0;
    session->output.dropped = 0;
    *reserved_session = session;
    return 0;
}

static int watch_session_pty_init(struct tty *tty) {
    struct watch_session *session = opening_session;
    if (session == NULL)
        return _EIO;

    tty->winsize.col = WATCH_DEFAULT_COLUMNS;
    tty->winsize.row = WATCH_DEFAULT_ROWS;
    tty->data = session;

    // pty_open_fake 持有 ttys_lock；驱动侧只允许按此方向进入 sessions_lock。
    lock(&sessions_lock);
    if (session->phase != ISH_WATCH_SESSION_STARTING ||
            session->client_closed ||
            session->tty_number != WATCH_NO_TTY) {
        unlock(&sessions_lock);
        tty->data = NULL;
        return _EIO;
    }
    session->tty_number = tty->num;
    unlock(&sessions_lock);
    return 0;
}

static int watch_session_pty_write(
        struct tty *tty, const void *bytes,
        size_t length, bool UNUSED(blocking)) {
    size_t accepted = length > (size_t) INT_MAX ?
            (size_t) INT_MAX : length;
    struct watch_session *session = tty->data;
    if (session == NULL)
        return _EIO;

    lock(&sessions_lock);
    if (session->phase != WATCH_SESSION_FREE &&
            !session->client_closed &&
            session->tty_number == tty->num)
        output_append_locked(&session->output, bytes, accepted);
    unlock(&sessions_lock);
    return (int) accepted;
}

static void watch_session_pty_cleanup(struct tty *tty) {
    struct watch_session *session = tty->data;
    tty->data = NULL;
    if (session == NULL)
        return;

    // tty_release 持有 ttys_lock；这里不得再取得进程表或 creation 锁。
    lock(&sessions_lock);
    if (session->phase != WATCH_SESSION_FREE &&
            session->tty_number == tty->num) {
        session->tty_number = WATCH_NO_TTY;
        session_release_if_finished_locked(session);
    }
    unlock(&sessions_lock);
}

static const struct tty_driver_ops watch_session_pty_ops = {
    .init = watch_session_pty_init,
    .write = watch_session_pty_write,
    .cleanup = watch_session_pty_cleanup,
};
static struct tty_driver watch_session_pty_driver = {
    .ops = &watch_session_pty_ops,
};

// 调用方持有 sessions_lock，且通过 ttys_lock 固定 tty 的发布代际。
static bool session_transport_matches_locked(
        struct watch_session_transport transport,
        const struct tty *tty,
        bool allow_closed) {
    struct watch_session *current_session =
            session_find_locked(transport.id);
    return tty != NULL &&
            tty->driver == &watch_session_pty_driver &&
            tty->data == transport.session &&
            current_session == transport.session &&
            current_session->tty_number == transport.tty_number &&
            (allow_closed || !current_session->client_closed);
}

static struct tty *session_tty_acquire(
        struct watch_session_transport transport,
        bool allow_closed) {
    if (transport.tty_number < 0)
        return NULL;

    lock(&ttys_lock);
    struct tty *tty = NULL;
    unsigned index = (unsigned) transport.tty_number;
    if (watch_session_pty_driver.ttys != NULL &&
            index < watch_session_pty_driver.limit &&
            !watch_session_pty_driver.reserved[index]) {
        tty = watch_session_pty_driver.ttys[index];
        lock(&sessions_lock);
        if (session_transport_matches_locked(
                transport, tty, allow_closed)) {
            lock(&tty->lock);
            tty->refcount++;
            unlock(&tty->lock);
        } else {
            tty = NULL;
        }
        unlock(&sessions_lock);
    }
    unlock(&ttys_lock);
    return tty;
}

static void session_tty_release(struct tty *tty) {
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
}

int ish_watch_session_status(
        ish_watch_session_id session_id,
        struct ish_watch_session_status *status) {
    if (status == NULL)
        return _EINVAL;

    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session == NULL || session->client_closed) {
        unlock(&sessions_lock);
        return _ESTALE;
    }
    *status = (struct ish_watch_session_status) {
        .phase = session->phase,
        .wait_status = session->wait_status,
    };
    unlock(&sessions_lock);
    return 0;
}

ssize_t ish_watch_session_read_output(
        ish_watch_session_id session_id,
        void *buffer,
        size_t capacity,
        uint64_t *dropped_bytes) {
    if (buffer == NULL && capacity != 0)
        return _EINVAL;
    if (capacity > (size_t) SSIZE_MAX)
        return _EMSGSIZE;

    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session == NULL || session->client_closed) {
        unlock(&sessions_lock);
        return _ESTALE;
    }
    size_t length = output_read_locked(
            &session->output, buffer, capacity, dropped_bytes);
    unlock(&sessions_lock);
    return (ssize_t) length;
}

static int session_transport_snapshot(
        ish_watch_session_id session_id,
        struct watch_session_transport *transport) {
    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session == NULL || session->client_closed) {
        unlock(&sessions_lock);
        return _ESTALE;
    }
    if (session->phase == ISH_WATCH_SESSION_STARTING) {
        unlock(&sessions_lock);
        return _EAGAIN;
    }
    if (session->phase == ISH_WATCH_SESSION_EXITED) {
        unlock(&sessions_lock);
        return _ESHUTDOWN;
    }
    *transport = (struct watch_session_transport) {
        .id = session->id,
        .session = session,
        .tty_number = session->tty_number,
    };
    unlock(&sessions_lock);

    int phase = atomic_load_explicit(
            &runtime_phase, memory_order_acquire);
    return phase == ISH_WATCH_RUNTIME_RUNNING ? 0 : _ESHUTDOWN;
}

ssize_t ish_watch_session_send_input(
        ish_watch_session_id session_id,
        const void *bytes,
        size_t length) {
    if (bytes == NULL && length != 0)
        return _EINVAL;
    if (length > (size_t) SSIZE_MAX)
        return _EMSGSIZE;

    struct watch_session_transport transport;
    int error = session_transport_snapshot(session_id, &transport);
    if (error < 0)
        return error;
    if (length == 0)
        return 0;

    struct tty *tty = session_tty_acquire(transport, false);
    if (tty == NULL)
        return _ESHUTDOWN;
    ssize_t consumed = tty_input(tty, bytes, length, false);
    session_tty_release(tty);
    return consumed;
}

int ish_watch_session_set_window_size(
        ish_watch_session_id session_id,
        uint16_t columns,
        uint16_t rows) {
    if (columns == 0 || rows == 0)
        return _EINVAL;

    struct watch_session_transport transport;
    int error = session_transport_snapshot(session_id, &transport);
    if (error < 0)
        return error;

    struct tty *tty = session_tty_acquire(transport, false);
    if (tty == NULL)
        return _ESHUTDOWN;
    lock(&tty->lock);
    tty_set_winsize(tty, (struct winsize_) {
        .col = columns,
        .row = rows,
    });
    unlock(&tty->lock);
    session_tty_release(tty);
    return 0;
}

int ish_watch_session_close(ish_watch_session_id session_id) {
    struct watch_session_transport transport;

    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session == NULL || session->client_closed) {
        unlock(&sessions_lock);
        return _ESTALE;
    }
    transport = (struct watch_session_transport) {
        .id = session->id,
        .session = session,
        .tty_number = session->tty_number,
    };
    session->client_closed = true;
    session_release_if_finished_locked(session);
    unlock(&sessions_lock);

    struct tty *tty = session_tty_acquire(transport, true);
    if (tty != NULL) {
        // 强引用的 tty 是不可复用身份，不依赖可能发生 ABA 的 PID 或 tgroup 地址。
        (void) task_kill_controlling_tty(tty);
        lock(&tty->lock);
        tty_hangup(tty);
        unlock(&tty->lock);
        session_tty_release(tty);
    }
    return 0;
}

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
int ish_watch_runtime_test_add_session(
        int32_t phase, ish_watch_session_id *session_id) {
    if (session_id == NULL ||
            (phase != ISH_WATCH_SESSION_STARTING &&
            phase != ISH_WATCH_SESSION_RUNNING &&
            phase != ISH_WATCH_SESSION_EXITED))
        return _EINVAL;

    lock(&sessions_lock);
    struct watch_session *session;
    int error = session_reserve_locked(phase, &session);
    if (error == 0)
        *session_id = session->id;
    unlock(&sessions_lock);
    return error;
}

void ish_watch_runtime_test_append_session_output(
        ish_watch_session_id session_id,
        const void *bytes,
        size_t length) {
    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session != NULL && !session->client_closed)
        output_append_locked(&session->output, bytes, length);
    unlock(&sessions_lock);
}

void ish_watch_runtime_test_mark_session_exited(
        ish_watch_session_id session_id,
        int32_t wait_status) {
    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session != NULL)
        session_mark_exited_locked(session, wait_status);
    unlock(&sessions_lock);
}

int ish_watch_runtime_test_recycled_transport(void) {
    lock(&sessions_lock);
    struct watch_session *original;
    int error = session_reserve_locked(
            ISH_WATCH_SESSION_RUNNING, &original);
    if (error < 0) {
        unlock(&sessions_lock);
        return error;
    }
    original->tty_number = 17;
    struct watch_session_transport stale = {
        .id = original->id,
        .session = original,
        .tty_number = original->tty_number,
    };

    session_reset_locked(original);
    struct watch_session *replacement;
    error = session_reserve_locked(
            ISH_WATCH_SESSION_RUNNING, &replacement);
    if (error < 0) {
        unlock(&sessions_lock);
        return error;
    }
    replacement->tty_number = stale.tty_number;
    struct tty replacement_tty = {
        .driver = &watch_session_pty_driver,
        .data = replacement,
    };
    bool rejected = !session_transport_matches_locked(
            stale, &replacement_tty, false);
    session_reset_locked(replacement);
    unlock(&sessions_lock);
    return rejected ? 0 : _EIO;
}
#endif

static int watch_console_init(struct tty *tty) {
    tty->winsize.col = WATCH_DEFAULT_COLUMNS;
    tty->winsize.row = WATCH_DEFAULT_ROWS;
    return 0;
}

static int watch_console_write(
        struct tty *tty, const void *bytes,
        size_t length, bool UNUSED(blocking)) {
    size_t accepted = length > (size_t) INT_MAX ?
            (size_t) INT_MAX : length;
    if (tty->num == WATCH_CONSOLE_NUMBER)
        output_append(bytes, accepted);
    return (int) accepted;
}

static const struct tty_driver_ops watch_console_ops = {
    .init = watch_console_init,
    .write = watch_console_write,
};
DEFINE_TTY_DRIVER(
        watch_console_driver, &watch_console_ops, TTY_CONSOLE_MAJOR, 64);

static struct tty *console_acquire(void) {
    lock(&ttys_lock);
    struct tty *tty = NULL;
    if (!watch_console_driver.reserved[WATCH_CONSOLE_NUMBER]) {
        tty = watch_console_driver.ttys[WATCH_CONSOLE_NUMBER];
        if (tty != NULL && tty->driver == &watch_console_driver) {
            lock(&tty->lock);
            tty->refcount++;
            unlock(&tty->lock);
        } else {
            tty = NULL;
        }
    }
    unlock(&ttys_lock);
    return tty;
}

static void console_release(struct tty *tty) {
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
}

ssize_t ish_watch_runtime_send_input(const void *bytes, size_t length) {
    if (bytes == NULL && length != 0)
        return _EINVAL;
    if (length > (size_t) SSIZE_MAX)
        return _EMSGSIZE;
    if (length == 0)
        return 0;
    if (atomic_load_explicit(&runtime_phase, memory_order_acquire) !=
            ISH_WATCH_RUNTIME_RUNNING)
        return _EAGAIN;

    struct tty *tty = console_acquire();
    if (tty == NULL)
        return _EAGAIN;
    ssize_t consumed = tty_input(tty, bytes, length, false);
    console_release(tty);
    return consumed;
}

int ish_watch_runtime_set_window_size(uint16_t columns, uint16_t rows) {
    if (columns == 0 || rows == 0)
        return _EINVAL;
    if (atomic_load_explicit(&runtime_phase, memory_order_acquire) !=
            ISH_WATCH_RUNTIME_RUNNING)
        return _EAGAIN;

    struct tty *tty = console_acquire();
    if (tty == NULL)
        return _EAGAIN;
    lock(&tty->lock);
    tty_set_winsize(tty, (struct winsize_) {
        .col = columns,
        .row = rows,
    });
    unlock(&tty->lock);
    console_release(tty);
    return 0;
}

int ish_watch_runtime_current_phase(void) {
    return atomic_load_explicit(&runtime_phase, memory_order_acquire);
}

int ish_watch_runtime_last_error(void) {
    return atomic_load_explicit(&runtime_error, memory_order_acquire);
}

static int runtime_fail(int error) {
    assert(error < 0);
    atomic_store_explicit(
            &runtime_accepts_sessions, false, memory_order_release);
    atomic_store_explicit(&runtime_error, error, memory_order_release);
    atomic_store_explicit(
            &runtime_phase, ISH_WATCH_RUNTIME_FAILED, memory_order_release);
    return error;
}

static int runtime_fail_after_task(int error) {
    cancel_prepared_process();
    return runtime_fail(error);
}

static int session_cancel_after_task(
        struct watch_session *session, int error) {
    lock(&sessions_lock);
    if (session->phase != WATCH_SESSION_FREE) {
        session->phase = ISH_WATCH_SESSION_EXITED;
        session->wait_status = 0;
        session->client_closed = true;
        session_release_if_finished_locked(session);
    }
    unlock(&sessions_lock);
    cancel_prepared_process();
    return error;
}

static void watch_handle_exit(struct task *task, int UNUSED(code)) {
    struct task *leader = task->group->leader;
    if (leader->parent == NULL) {
        atomic_store_explicit(
                &runtime_accepts_sessions, false, memory_order_release);
        atomic_store_explicit(
                &runtime_phase, ISH_WATCH_RUNTIME_STOPPED,
                memory_order_release);
        return;
    }

    lock(&leader->group->lock);
    dword_t wait_status = leader->exit_code;
    if (leader->group->doing_group_exit)
        wait_status = leader->group->group_exit_code;
    struct tty *controlling_tty = leader->group->tty;
    unlock(&leader->group->lock);

    lock(&sessions_lock);
    for (size_t index = 0; index < ISH_WATCH_SESSION_LIMIT; index++) {
        struct watch_session *session = &sessions[index];
        if (session->phase != WATCH_SESSION_FREE &&
                session->leader_group == leader->group) {
            session_mark_exited_locked(
                    session, (int32_t) wait_status);
            break;
        }
    }
    unlock(&sessions_lock);

    // shell 退出即结束整个 PTY 会话，不能留下持有 tty 的后台进程耗尽槽位。
    (void) task_kill_controlling_tty_locked(controlling_tty);
}

static void watch_handle_die(const char *UNUSED(message)) {
    atomic_store_explicit(
            &runtime_accepts_sessions, false, memory_order_release);
    atomic_store_explicit(&runtime_error, _EIO, memory_order_release);
    atomic_store_explicit(
            &runtime_phase, ISH_WATCH_RUNTIME_FAILED, memory_order_release);
}

static bool socket_prefix_fits(const char *socket_prefix) {
    char path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    int length = snprintf(
            path, sizeof(path), "%s%d.%u",
            socket_prefix, INT_MAX, UINT32_MAX);
    return length >= 0 && (size_t) length < sizeof(path);
}

static int exec_shell_command(
        const char *command, size_t command_length) {
    static const char shell_path[] = "/bin/sh";
    static const char shell_option[] = "-c";
    size_t arguments_length = sizeof(shell_path) +
            sizeof(shell_option) + command_length + 1;
    char *arguments = malloc(arguments_length);
    if (arguments == NULL)
        return _ENOMEM;

    char *argument = arguments;
    memcpy(argument, shell_path, sizeof(shell_path));
    argument += sizeof(shell_path);
    memcpy(argument, shell_option, sizeof(shell_option));
    argument += sizeof(shell_option);
    memcpy(argument, command, command_length + 1);

    static const char environment[] = "TERM=xterm-256color\0";
    int error = do_execve(shell_path, 3, arguments, environment);
    free(arguments);
    return error;
}

static int session_runtime_availability(void) {
    int phase = atomic_load_explicit(
            &runtime_phase, memory_order_acquire);
    if (phase == ISH_WATCH_RUNTIME_RUNNING)
        return atomic_load_explicit(
                &runtime_accepts_sessions, memory_order_acquire) ?
                0 : _EAGAIN;
    if (phase == ISH_WATCH_RUNTIME_IDLE ||
            phase == ISH_WATCH_RUNTIME_PREPARING)
        return _EAGAIN;
    return _ESHUTDOWN;
}

int ish_watch_session_create(
        const char *command,
        uint16_t columns,
        uint16_t rows,
        ish_watch_session_id *session_id) {
    if (command == NULL || command[0] == '\0' ||
            columns == 0 || rows == 0 || session_id == NULL)
        return _EINVAL;
    size_t command_length = strlen(command);
    if (command_length >= WATCH_COMMAND_LIMIT)
        return _E2BIG;
    *session_id = 0;

    int error = session_runtime_availability();
    if (error < 0)
        return error;

    lock(&session_create_lock);
    error = session_runtime_availability();
    if (error < 0) {
        unlock(&session_create_lock);
        return error;
    }

    lock(&sessions_lock);
    struct watch_session *session;
    error = session_reserve_locked(
            ISH_WATCH_SESSION_STARTING, &session);
    unlock(&sessions_lock);
    if (error < 0) {
        unlock(&session_create_lock);
        return error;
    }

    error = begin_new_init_child();
    if (error < 0) {
        lock(&sessions_lock);
        session_reset_locked(session);
        unlock(&sessions_lock);
        unlock(&session_create_lock);
        return error;
    }

    struct task *guest_task = current;
    lock(&sessions_lock);
    session->leader_group = guest_task->group;
    unlock(&sessions_lock);

    opening_session = session;
    struct tty *tty = pty_open_fake(&watch_session_pty_driver);
    opening_session = NULL;
    if (IS_ERR(tty)) {
        error = (int) PTR_ERR(tty);
        error = session_cancel_after_task(session, error);
        unlock(&session_create_lock);
        return error;
    }

    lock(&tty->lock);
    tty->winsize = (struct winsize_) {
        .col = columns,
        .row = rows,
    };
    unlock(&tty->lock);

    char stdio_file[32];
    int stdio_length = snprintf(
            stdio_file, sizeof(stdio_file), "/dev/pts/%d", tty->num);
    if (stdio_length < 0 || (size_t) stdio_length >= sizeof(stdio_file))
        error = _EOVERFLOW;
    else
        error = create_stdio(
                stdio_file, TTY_PSEUDO_SLAVE_MAJOR, tty->num);

    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
    if (error < 0) {
        error = session_cancel_after_task(session, error);
        unlock(&session_create_lock);
        return error;
    }

    error = exec_shell_command(command, command_length);
    if (error < 0) {
        error = session_cancel_after_task(session, error);
        unlock(&session_create_lock);
        return error;
    }

    lock(&sessions_lock);
    session->phase = ISH_WATCH_SESSION_RUNNING;
    ish_watch_session_id created_id = session->id;
    unlock(&sessions_lock);

    error = commit_prepared_process();
    if (error < 0) {
        error = session_cancel_after_task(session, error);
        unlock(&session_create_lock);
        return error;
    }
    *session_id = created_id;
    unlock(&session_create_lock);
    return 0;
}

int ish_watch_runtime_start(
        const char *root_data,
        const char *socket_prefix,
        const char *hostname,
        const char *boot_command) {
    if (root_data == NULL || root_data[0] == '\0' ||
            socket_prefix == NULL || socket_prefix[0] == '\0' ||
            hostname == NULL || hostname[0] == '\0' ||
            boot_command == NULL || boot_command[0] == '\0')
        return _EINVAL;
    if (!socket_prefix_fits(socket_prefix))
        return _ENAMETOOLONG;
    size_t command_length = strlen(boot_command);
    if (command_length >= WATCH_COMMAND_LIMIT)
        return _E2BIG;

    int expected = ISH_WATCH_RUNTIME_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime_phase, &expected, ISH_WATCH_RUNTIME_PREPARING,
            memory_order_acq_rel, memory_order_acquire))
        return _EALREADY;
    atomic_store_explicit(
            &runtime_accepts_sessions, false, memory_order_release);

    char *owned_socket_prefix = strdup(socket_prefix);
    if (owned_socket_prefix == NULL)
        return runtime_fail(_ENOMEM);
    char *owned_hostname = strdup(hostname);
    if (owned_hostname == NULL) {
        free(owned_socket_prefix);
        return runtime_fail(_ENOMEM);
    }

    int error = mount_root(&fakefs, root_data);
    if (error < 0) {
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail(error);
    }

    error = begin_first_process();
    if (error < 0) {
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail(error);
    }

    create_some_device_nodes();
    (void) generic_setattrat(
            AT_PWD, "/", (struct attr) {
                .type = attr_mode,
                .mode = 0755,
            }, false);

    error = do_mount(&procfs, "proc", "/proc", "", 0);
    if (error < 0) {
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }
    error = do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    if (error < 0) {
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    exit_hook = watch_handle_exit;
    die_handler = watch_handle_die;
    tty_drivers[TTY_CONSOLE_MAJOR] = &watch_console_driver;
    set_console_device(TTY_CONSOLE_MAJOR, WATCH_CONSOLE_NUMBER);
    error = create_stdio(
            "/dev/console", TTY_CONSOLE_MAJOR, WATCH_CONSOLE_NUMBER);
    if (error < 0) {
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    error = exec_shell_command(boot_command, command_length);
    if (error < 0) {
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    extern const char *uname_hostname_override;
    uname_hostname_override = owned_hostname;
    sock_tmp_prefix = owned_socket_prefix;
    expected = ISH_WATCH_RUNTIME_PREPARING;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime_phase, &expected, ISH_WATCH_RUNTIME_RUNNING,
            memory_order_acq_rel, memory_order_acquire))
        return runtime_fail_after_task(_EIO);

    error = commit_prepared_process();
    if (error < 0)
        return runtime_fail_after_task(error);

    if (atomic_load_explicit(
            &runtime_phase, memory_order_acquire) ==
            ISH_WATCH_RUNTIME_RUNNING)
        atomic_store_explicit(
                &runtime_accepts_sessions, true, memory_order_release);

    // DNS 暂不可用不应阻止离线 shell；PID 1 发布后才能取得其 fs 快照。
    (void) ish_apple_guest_configure_dns_pid(1);
    return 0;
}

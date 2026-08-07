#include "platform/apple-watch-runtime.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "platform/apple-command-session-private.h"
#include "platform/apple-watch-runtime-private.h"

#define WATCH_OUTPUT_INITIAL_CAPACITY (64 * 1024)
#define WATCH_DEFAULT_COLUMNS 40
#define WATCH_DEFAULT_ROWS 18
#define WATCH_NO_TTY (-1)
#define WATCH_SESSION_CHUNK_CAPACITY 8

enum watch_session_internal_phase {
    WATCH_SESSION_FREE = 0,
};

struct watch_output_ring {
    unsigned char *bytes;
    size_t capacity;
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

struct watch_session_chunk {
    struct watch_session sessions[WATCH_SESSION_CHUNK_CAPACITY];
    struct watch_session_chunk *next;
};

struct watch_session_transport {
    ish_watch_session_id id;
    struct watch_session *session;
    int tty_number;
};

static struct watch_output_ring console_output;
static lock_t output_lock = LOCK_INITIALIZER;

static struct watch_session_chunk *session_chunks;
static ish_watch_session_id next_session_id = 1;
static lock_t sessions_lock = LOCK_INITIALIZER;

static _Thread_local struct watch_session *opening_session;

static void output_note_dropped(
        struct watch_output_ring *output, size_t length) {
    uint64_t increment = (uint64_t) length;
    if (UINT64_MAX - output->dropped < increment)
        output->dropped = UINT64_MAX;
    else
        output->dropped += increment;
}

static bool output_reserve_locked(
        struct watch_output_ring *output, size_t additional) {
    if (additional > SIZE_MAX - output->count)
        return false;
    size_t required = output->count + additional;
    if (required <= output->capacity)
        return true;

    size_t capacity = output->capacity == 0 ?
            WATCH_OUTPUT_INITIAL_CAPACITY : output->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    unsigned char *replacement = malloc(capacity);
    if (replacement == NULL)
        return false;
    if (output->count != 0) {
        size_t first = output->capacity - output->head;
        if (first > output->count)
            first = output->count;
        memcpy(replacement, output->bytes + output->head, first);
        memcpy(replacement + first, output->bytes,
                output->count - first);
    }
    free(output->bytes);
    output->bytes = replacement;
    output->capacity = capacity;
    output->head = 0;
    return true;
}

static void output_append_locked(
        struct watch_output_ring *output,
        const void *bytes,
        size_t length) {
    if (length == 0)
        return;
    const unsigned char *source = bytes;

    if (!output_reserve_locked(output, length)) {
        if (output->capacity == 0) {
            output_note_dropped(output, length);
            return;
        }
    } else {
        size_t tail = (output->head + output->count) % output->capacity;
        size_t first = output->capacity - tail;
        if (first > length)
            first = length;
        memcpy(output->bytes + tail, source, first);
        memcpy(output->bytes, source + first, length - first);
        output->count += length;
        return;
    }

    if (length >= output->capacity) {
        output_note_dropped(output, output->count);
        output_note_dropped(output, length - output->capacity);
        source += length - output->capacity;
        length = output->capacity;
        memcpy(output->bytes, source, length);
        output->head = 0;
        output->count = length;
        return;
    }

    size_t available = output->capacity - output->count;
    size_t overflow = length > available ? length - available : 0;
    output_note_dropped(output, overflow);
    output->head = (output->head + overflow) % output->capacity;
    output->count -= overflow;

    size_t tail = (output->head + output->count) % output->capacity;
    size_t first = output->capacity - tail;
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
    if (output->capacity == 0)
        return 0;
    size_t first = output->capacity - output->head;
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
    for (struct watch_session_chunk *chunk = session_chunks;
            chunk != NULL; chunk = chunk->next) {
        for (size_t index = 0;
                index < WATCH_SESSION_CHUNK_CAPACITY; index++) {
            struct watch_session *session = &chunk->sessions[index];
            if (session->phase != WATCH_SESSION_FREE &&
                    session->id == session_id)
                return session;
        }
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
    free(session->output.bytes);
    session->output = (struct watch_output_ring) {};
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

static bool session_mark_group_exited_locked(
        struct tgroup *group, int32_t wait_status) {
    for (struct watch_session_chunk *chunk = session_chunks;
            chunk != NULL; chunk = chunk->next) {
        for (size_t index = 0;
                index < WATCH_SESSION_CHUNK_CAPACITY; index++) {
            struct watch_session *session = &chunk->sessions[index];
            if (session->phase != WATCH_SESSION_FREE &&
                    session->leader_group == group) {
                session_mark_exited_locked(session, wait_status);
                return true;
            }
        }
    }
    return false;
}

static int session_reserve_locked(
        int32_t phase, struct watch_session **reserved_session) {
    if (next_session_id == UINT64_MAX)
        return _EOVERFLOW;

    struct watch_session *session = NULL;
    for (struct watch_session_chunk *chunk = session_chunks;
            chunk != NULL && session == NULL; chunk = chunk->next) {
        for (size_t index = 0;
                index < WATCH_SESSION_CHUNK_CAPACITY; index++) {
            session_release_if_finished_locked(&chunk->sessions[index]);
            if (chunk->sessions[index].phase == WATCH_SESSION_FREE) {
                session = &chunk->sessions[index];
                break;
            }
        }
    }
    if (session == NULL) {
        struct watch_session_chunk *chunk = calloc(1, sizeof(*chunk));
        if (chunk == NULL)
            return _ENOMEM;
        chunk->next = session_chunks;
        session_chunks = chunk;
        session = &chunk->sessions[0];
    }

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

    int phase = ish_watch_runtime_current_phase();
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

int ish_watch_session_cancel(ish_watch_session_id session_id) {
    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session == NULL || session->client_closed) {
        unlock(&sessions_lock);
        return _ESTALE;
    }
    if (session->phase == ISH_WATCH_SESSION_EXITED) {
        unlock(&sessions_lock);
        return 0;
    }
    if (session->phase == ISH_WATCH_SESSION_STARTING) {
        unlock(&sessions_lock);
        return _EAGAIN;
    }
    struct watch_session_transport transport = {
        .id = session->id,
        .session = session,
        .tty_number = session->tty_number,
    };
    unlock(&sessions_lock);

    struct tty *tty = session_tty_acquire(transport, false);
    if (tty == NULL)
        return _ESHUTDOWN;
    // 强引用的 tty 是不可复用身份，不依赖可能发生 ABA 的 PID 或 tgroup 地址。
    (void) task_kill_controlling_tty(tty);
    lock(&tty->lock);
    tty_hangup(tty);
    unlock(&tty->lock);
    session_tty_release(tty);
    return 0;
}

int ish_watch_session_close(ish_watch_session_id session_id) {
    int cancel_error = ish_watch_session_cancel(session_id);
    if (cancel_error < 0 && cancel_error != _ESHUTDOWN &&
            cancel_error != _EAGAIN)
        return cancel_error;

    lock(&sessions_lock);
    struct watch_session *session = session_find_locked(session_id);
    if (session == NULL || session->client_closed) {
        unlock(&sessions_lock);
        return _ESTALE;
    }
    session->client_closed = true;
    session_release_if_finished_locked(session);
    unlock(&sessions_lock);
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

int ish_watch_runtime_test_exit_ownership(void) {
    struct tgroup session_group = {};
    struct tgroup child_group = {};

    lock(&sessions_lock);
    struct watch_session *session;
    int error = session_reserve_locked(
            ISH_WATCH_SESSION_RUNNING, &session);
    if (error < 0) {
        unlock(&sessions_lock);
        return error;
    }
    session->leader_group = &session_group;

    bool child_owns_session = session_mark_group_exited_locked(
            &child_group, 7 << 8);
    bool child_preserved_session =
            session->phase == ISH_WATCH_SESSION_RUNNING &&
            session->leader_group == &session_group;
    bool leader_owns_session = session_mark_group_exited_locked(
            &session_group, 9 << 8);
    bool leader_finished_session =
            session->phase == ISH_WATCH_SESSION_EXITED &&
            session->wait_status == (9 << 8) &&
            session->leader_group == NULL;
    session_reset_locked(session);
    unlock(&sessions_lock);

    return !child_owns_session && child_preserved_session &&
            leader_owns_session && leader_finished_session ?
            0 : _EIO;
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
    if (ish_watch_runtime_current_phase() != ISH_WATCH_RUNTIME_RUNNING)
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
    if (ish_watch_runtime_current_phase() != ISH_WATCH_RUNTIME_RUNNING)
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


void ish_watch_terminal_install_console(void) {
    tty_drivers[TTY_CONSOLE_MAJOR] = &watch_console_driver;
    set_console_device(TTY_CONSOLE_MAJOR, WATCH_CONSOLE_NUMBER);
}

void ish_watch_session_handle_exit(
        struct tgroup *group,
        int32_t wait_status,
        struct tty *controlling_tty) {
    lock(&sessions_lock);
    bool owns_session = session_mark_group_exited_locked(
            group, wait_status);
    unlock(&sessions_lock);

    // shell 退出即结束整个 PTY 会话，不能留下持有 tty 的后台进程耗尽槽位。
    if (owns_session)
        (void) task_kill_controlling_tty_locked(controlling_tty);
}

int ish_watch_session_create_process(
        const struct command_arguments *arguments,
        uint16_t columns,
        uint16_t rows,
        ish_watch_session_id *session_id) {
    if (arguments == NULL || arguments->executable == NULL ||
            arguments->argument_count == 0 ||
            arguments->argument_bytes == NULL ||
            arguments->environment_bytes == NULL ||
            columns == 0 || rows == 0 || session_id == NULL)
        return _EINVAL;
    *session_id = 0;

    int error = ish_watch_runtime_operation_availability();
    if (error < 0)
        return error;

    lock(&ish_watch_prepared_task_lock);
    error = ish_watch_runtime_operation_availability();
    if (error < 0) {
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    lock(&sessions_lock);
    struct watch_session *session;
    error = session_reserve_locked(
            ISH_WATCH_SESSION_STARTING, &session);
    unlock(&sessions_lock);
    if (error < 0) {
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    error = begin_new_init_child();
    if (error < 0) {
        lock(&sessions_lock);
        session_reset_locked(session);
        unlock(&sessions_lock);
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    struct task *guest_task = current;
    lock(&sessions_lock);
    session->leader_group = guest_task->group;
    unlock(&sessions_lock);

    if (arguments->working_directory != NULL) {
        error = file_chdir_task(
                guest_task, arguments->working_directory);
        if (error < 0) {
            error = session_cancel_after_task(session, error);
            unlock(&ish_watch_prepared_task_lock);
            return error;
        }
    }

    opening_session = session;
    struct tty *tty = pty_open_fake(&watch_session_pty_driver);
    opening_session = NULL;
    if (IS_ERR(tty)) {
        error = (int) PTR_ERR(tty);
        error = session_cancel_after_task(session, error);
        unlock(&ish_watch_prepared_task_lock);
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
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    error = do_execve(
            arguments->executable,
            arguments->argument_count,
            arguments->argument_bytes,
            arguments->environment_bytes);
    if (error < 0) {
        error = session_cancel_after_task(session, error);
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }

    lock(&sessions_lock);
    session->phase = ISH_WATCH_SESSION_RUNNING;
    ish_watch_session_id created_id = session->id;
    unlock(&sessions_lock);

    error = commit_prepared_process();
    if (error < 0) {
        error = session_cancel_after_task(session, error);
        unlock(&ish_watch_prepared_task_lock);
        return error;
    }
    *session_id = created_id;
    unlock(&ish_watch_prepared_task_lock);
    return 0;
}

int ish_watch_session_create(
        const char *command,
        uint16_t columns,
        uint16_t rows,
        ish_watch_session_id *session_id) {
    if (command == NULL || command[0] == '\0')
        return _EINVAL;
    size_t command_length = strlen(command);
    if (command_length >= WATCH_COMMAND_LIMIT)
        return _E2BIG;

    const char *arguments[] = {"/bin/sh", "-c", command};
    const char *environment[] = {"TERM=xterm-256color"};
    struct ish_apple_command_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(spec),
        .request_id = 1,
        .executable = "/bin/sh",
        .arguments = arguments,
        .environment = environment,
        .argument_count = 3,
        .environment_count = 1,
    };
    struct command_arguments packed = {};
    int32_t error = command_arguments_create_for_spec(&spec, &packed);
    if (error == 0) {
        error = ish_watch_session_create_process(
                &packed, columns, rows, session_id);
    }
    command_arguments_destroy(&packed);
    return error;
}

#include "sdk/iSHApple/Headers/iSHAppleTerminal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "kernel/errno.h"
#include "platform/apple-command-session-private.h"
#include "platform/apple-watch-runtime.h"
#include "platform/apple-watch-runtime-private.h"

struct ish_apple_terminal_session {
    _Atomic uint32_t references;
    pthread_mutex_t lock;
    ish_watch_session_id watch_session_id;
    uint64_t terminal_id;
    bool cancelled;
    bool result_ready;
    uint64_t output_bytes;
    uint64_t dropped_bytes;
    struct timespec started_at;
    struct ish_apple_terminal_result_v1 result;
};

static bool terminal_reserved_zero(const uint64_t values[2]) {
    return values[0] == 0 && values[1] == 0;
}

static uint64_t terminal_elapsed_milliseconds(
        const struct timespec *start,
        const struct timespec *end) {
    int64_t seconds = end->tv_sec - start->tv_sec;
    int64_t nanoseconds = end->tv_nsec - start->tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += INT64_C(1000000000);
    }
    if (seconds < 0)
        return 0;
    return (uint64_t) seconds * UINT64_C(1000) +
            (uint64_t) nanoseconds / UINT64_C(1000000);
}

static uint64_t terminal_saturating_add(
        uint64_t value, uint64_t increment) {
    return UINT64_MAX - value < increment ?
            UINT64_MAX : value + increment;
}

static void terminal_build_result_locked(
        struct ish_apple_terminal_session *session,
        int32_t reason,
        int32_t wait_status,
        int32_t error) {
    struct timespec finished_at;
    if (clock_gettime(CLOCK_MONOTONIC, &finished_at) != 0)
        __builtin_trap();

    int32_t exit_code = -1;
    int32_t termination_signal = 0;
    if (reason != ISH_APPLE_TERMINAL_COMPLETION_CANCELLED &&
            error == 0) {
        if ((wait_status & 0x7f) == 0) {
            exit_code = (wait_status >> 8) & 0xff;
            reason = ISH_APPLE_TERMINAL_COMPLETION_EXITED;
        } else if ((wait_status & 0x7f) != 0x7f) {
            termination_signal = wait_status & 0x7f;
            reason = ISH_APPLE_TERMINAL_COMPLETION_SIGNALED;
        } else {
            reason = ISH_APPLE_TERMINAL_COMPLETION_RUNTIME_FAILURE;
            error = _EIO;
        }
    }

    session->result = (struct ish_apple_terminal_result_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(session->result),
        .terminal_id = session->terminal_id,
        .reason = reason,
        .exit_code = exit_code,
        .termination_signal = termination_signal,
        .error = error,
        .output_bytes = session->output_bytes,
        .dropped_bytes = session->dropped_bytes,
        .elapsed_milliseconds = terminal_elapsed_milliseconds(
                &session->started_at, &finished_at),
    };
    session->result_ready = true;
}

int32_t ish_apple_terminal_session_start(
        const struct ish_apple_terminal_spec_v1 *spec,
        struct ish_apple_terminal_session **session_out) {
    if (session_out == NULL)
        return _EINVAL;
    *session_out = NULL;
    if (spec == NULL)
        return _EINVAL;
    if (spec->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (spec->structure_size < sizeof(*spec) ||
            spec->columns == 0 || spec->rows == 0 ||
            spec->reserved_0 != 0 || spec->terminal_id == 0 ||
            !terminal_reserved_zero(spec->reserved))
        return _EINVAL;

    struct ish_apple_command_spec_v1 command_spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(command_spec),
        .request_id = spec->terminal_id,
        .executable = spec->executable,
        .arguments = spec->arguments,
        .environment = spec->environment,
        .working_directory = spec->working_directory,
        .argument_count = spec->argument_count,
        .environment_count = spec->environment_count,
    };
    struct command_arguments arguments = {};
    int32_t error = command_arguments_create_for_spec(
            &command_spec, &arguments);
    if (error < 0)
        return error;

    struct ish_apple_terminal_session *session =
            calloc(1, sizeof(*session));
    if (session == NULL) {
        command_arguments_destroy(&arguments);
        return _ENOMEM;
    }
    atomic_init(&session->references, 1);
    if (pthread_mutex_init(&session->lock, NULL) != 0) {
        command_arguments_destroy(&arguments);
        free(session);
        return _ENOMEM;
    }
    session->terminal_id = spec->terminal_id;
    if (clock_gettime(CLOCK_MONOTONIC, &session->started_at) != 0) {
        pthread_mutex_destroy(&session->lock);
        command_arguments_destroy(&arguments);
        free(session);
        return _EIO;
    }
    error = ish_watch_session_create_process(
            &arguments, spec->columns, spec->rows,
            &session->watch_session_id);
    command_arguments_destroy(&arguments);
    if (error < 0) {
        pthread_mutex_destroy(&session->lock);
        free(session);
        return error;
    }
    *session_out = session;
    return 0;
}

struct ish_apple_terminal_session *ish_apple_terminal_session_retain(
        struct ish_apple_terminal_session *session) {
    if (session == NULL)
        return NULL;
    uint32_t references = atomic_load_explicit(
            &session->references, memory_order_acquire);
    while (references != 0 && references != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &session->references, &references, references + 1,
                memory_order_acq_rel, memory_order_acquire))
            return session;
    }
    return NULL;
}

void ish_apple_terminal_session_release(
        struct ish_apple_terminal_session *session) {
    if (session == NULL)
        return;
    uint32_t previous = atomic_fetch_sub_explicit(
            &session->references, 1, memory_order_acq_rel);
    if (previous == 0)
        __builtin_trap();
    if (previous != 1)
        return;
    (void) ish_apple_terminal_session_cancel(session);
    (void) ish_watch_session_close(session->watch_session_id);
    pthread_mutex_destroy(&session->lock);
    free(session);
}

int32_t ish_apple_terminal_session_read_output(
        struct ish_apple_terminal_session *session,
        void *bytes,
        uint32_t capacity,
        uint32_t *count_out,
        uint64_t *dropped_out) {
    if (session == NULL || count_out == NULL || dropped_out == NULL ||
            (bytes == NULL && capacity != 0))
        return _EINVAL;
    *count_out = 0;
    *dropped_out = 0;

    pthread_mutex_lock(&session->lock);
    ish_watch_session_id session_id = session->watch_session_id;
    pthread_mutex_unlock(&session->lock);

    uint64_t dropped = 0;
    ssize_t count = ish_watch_session_read_output(
            session_id, bytes, capacity, &dropped);
    if (count < 0)
        return (int32_t) count;

    pthread_mutex_lock(&session->lock);
    session->output_bytes = terminal_saturating_add(
            session->output_bytes, (uint64_t) count);
    session->dropped_bytes = terminal_saturating_add(
            session->dropped_bytes, dropped);
    if (session->result_ready) {
        session->result.output_bytes = session->output_bytes;
        session->result.dropped_bytes = session->dropped_bytes;
    }
    pthread_mutex_unlock(&session->lock);
    *count_out = (uint32_t) count;
    *dropped_out = dropped;
    return 0;
}

int32_t ish_apple_terminal_session_write_input(
        struct ish_apple_terminal_session *session,
        const void *bytes,
        uint32_t length,
        uint32_t *accepted_out) {
    if (session == NULL || accepted_out == NULL ||
            (bytes == NULL && length != 0))
        return _EINVAL;
    *accepted_out = 0;
    if (length > ISH_APPLE_TERMINAL_IO_BYTES_MAX)
        return _EMSGSIZE;

    pthread_mutex_lock(&session->lock);
    ish_watch_session_id session_id = session->watch_session_id;
    bool cancelled = session->cancelled;
    pthread_mutex_unlock(&session->lock);
    if (cancelled)
        return _ESHUTDOWN;
    ssize_t accepted = ish_watch_session_send_input(
            session_id, bytes, length);
    if (accepted < 0)
        return (int32_t) accepted;
    *accepted_out = (uint32_t) accepted;
    return 0;
}

static int32_t terminal_send_control(
        struct ish_apple_terminal_session *session,
        unsigned char control) {
    uint32_t accepted = 0;
    int32_t error = ish_apple_terminal_session_write_input(
            session, &control, 1, &accepted);
    if (error < 0)
        return error;
    return accepted == 1 ? 0 : _EAGAIN;
}

int32_t ish_apple_terminal_session_finish_input(
        struct ish_apple_terminal_session *session) {
    return terminal_send_control(session, 0x04);
}

int32_t ish_apple_terminal_session_interrupt(
        struct ish_apple_terminal_session *session) {
    return terminal_send_control(session, 0x03);
}

int32_t ish_apple_terminal_session_resize(
        struct ish_apple_terminal_session *session,
        uint16_t columns,
        uint16_t rows) {
    if (session == NULL || columns == 0 || rows == 0)
        return _EINVAL;
    pthread_mutex_lock(&session->lock);
    ish_watch_session_id session_id = session->watch_session_id;
    bool cancelled = session->cancelled;
    pthread_mutex_unlock(&session->lock);
    if (cancelled)
        return _ESHUTDOWN;
    return ish_watch_session_set_window_size(
            session_id, columns, rows);
}

int32_t ish_apple_terminal_session_cancel(
        struct ish_apple_terminal_session *session) {
    if (session == NULL)
        return _EINVAL;
    pthread_mutex_lock(&session->lock);
    bool cancelled = session->cancelled;
    ish_watch_session_id session_id = session->watch_session_id;
    pthread_mutex_unlock(&session->lock);
    if (cancelled)
        return 0;

    int32_t error = ish_watch_session_cancel(session_id);
    if (error < 0 && error != _ESHUTDOWN)
        return error;
    pthread_mutex_lock(&session->lock);
    session->cancelled = true;
    pthread_mutex_unlock(&session->lock);
    return 0;
}

int32_t ish_apple_terminal_session_copy_result(
        struct ish_apple_terminal_session *session,
        struct ish_apple_terminal_result_v1 *result_out) {
    if (session == NULL || result_out == NULL)
        return _EINVAL;
    pthread_mutex_lock(&session->lock);
    if (session->result_ready) {
        *result_out = session->result;
        pthread_mutex_unlock(&session->lock);
        return 0;
    }
    ish_watch_session_id session_id = session->watch_session_id;
    pthread_mutex_unlock(&session->lock);

    struct ish_watch_session_status status;
    int32_t error = ish_watch_session_status(session_id, &status);
    if (error < 0)
        return error;
    if (status.phase != ISH_WATCH_SESSION_EXITED)
        return _EAGAIN;

    pthread_mutex_lock(&session->lock);
    if (!session->result_ready) {
        terminal_build_result_locked(
                session,
                session->cancelled ?
                        ISH_APPLE_TERMINAL_COMPLETION_CANCELLED :
                        ISH_APPLE_TERMINAL_COMPLETION_RUNTIME_FAILURE,
                status.wait_status,
                0);
    }
    *result_out = session->result;
    pthread_mutex_unlock(&session->lock);
    return 0;
}

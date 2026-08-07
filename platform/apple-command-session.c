#include "platform/apple-command-session-private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/errno.h"

#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
#include "kernel/signal.h"
#include "kernel/task.h"
#endif

#define COMMAND_OUTPUT_CHUNK ISH_APPLE_COMMAND_OUTPUT_CHUNK_MAX

static _Atomic uint32_t command_object_count;
static _Atomic uint64_t command_next_job_id = 1;
static pthread_mutex_t command_active_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ish_apple_command_session *command_active_sessions;
static uint32_t command_active_count;

#ifdef ISH_APPLE_COMMAND_SESSION_TESTING
uint32_t ish_apple_command_session_test_live_sessions(void) {
    return atomic_load_explicit(
            &command_object_count, memory_order_acquire);
}
#endif

static _Thread_local struct ish_apple_command_session
        *command_stream_callback_session;
static _Thread_local struct ish_apple_command_session
        *command_exit_callback_session;

static void command_unregister_active(
        struct ish_apple_command_session *session) {
    pthread_mutex_lock(&command_active_lock);
    if (session->active_registered) {
        struct ish_apple_command_session **link =
                &command_active_sessions;
        while (*link != NULL && *link != session)
            link = &(*link)->active_next;
        if (*link != session)
            __builtin_trap();
        *link = session->active_next;
        session->active_next = NULL;
        session->active_registered = false;
        if (command_active_count == 0)
            __builtin_trap();
        command_active_count--;
    }
    pthread_mutex_unlock(&command_active_lock);
}

static int32_t command_register_active(
        struct ish_apple_command_session *session) {
    pthread_mutex_lock(&command_active_lock);
    for (struct ish_apple_command_session *active =
                    command_active_sessions;
            active != NULL; active = active->active_next) {
        if (active->request_id == session->request_id) {
            pthread_mutex_unlock(&command_active_lock);
            return _EEXIST;
        }
    }
    session->active_next = command_active_sessions;
    session->active_registered = true;
    command_active_sessions = session;
    command_active_count++;
    pthread_mutex_unlock(&command_active_lock);
    return 0;
}

int32_t command_map_host_error(int error) {
    switch (error) {
        case 0: return 0;
        case EPERM: return _EPERM;
        case ENOENT: return _ENOENT;
        case ESRCH: return _ESRCH;
        case EINTR: return _EINTR;
        case EIO: return _EIO;
        case E2BIG: return _E2BIG;
        case ENOEXEC: return _ENOEXEC;
        case EBADF: return _EBADF;
        case EAGAIN: return _EAGAIN;
        case ENOMEM: return _ENOMEM;
        case EACCES: return _EACCES;
        case EBUSY: return _EBUSY;
        case EINVAL: return _EINVAL;
        case ENFILE: return _ENFILE;
        case EMFILE: return _EMFILE;
        case EPIPE: return _EPIPE;
        case ENAMETOOLONG: return _ENAMETOOLONG;
        default: return _EIO;
    }
}

static void command_session_destroy(
        struct ish_apple_command_session *session) {
    command_unregister_active(session);
    if (session->stdin_fd >= 0)
        close(session->stdin_fd);
    if (session->exit_event_read_fd >= 0)
        close(session->exit_event_read_fd);
    if (session->exit_event_write_fd >= 0)
        close(session->exit_event_write_fd);
    pthread_cond_destroy(&session->changed);
    pthread_mutex_destroy(&session->callback_lock);
    pthread_mutex_destroy(&session->lock);
    free(session);
    atomic_fetch_sub_explicit(
            &command_object_count, 1, memory_order_release);
}

void command_session_internal_retain(
        struct ish_apple_command_session *session) {
    uint32_t previous = atomic_fetch_add_explicit(
            &session->references, 1, memory_order_relaxed);
    if (previous == UINT32_MAX)
        __builtin_trap();
}

void command_session_internal_release(
        struct ish_apple_command_session *session) {
    uint32_t previous = atomic_fetch_sub_explicit(
            &session->references, 1, memory_order_acq_rel);
    if (previous == 0)
        __builtin_trap();
    if (previous == 1)
        command_session_destroy(session);
}

// 只撤销尚未发布的内部引用；调用方同时持有另一份已知引用。
void command_session_rollback_internal_retain(
        struct ish_apple_command_session *session) {
    uint32_t previous = atomic_fetch_sub_explicit(
            &session->references, 1, memory_order_relaxed);
    if (previous <= 1)
        __builtin_trap();
}

static int32_t command_session_create(
        const struct ish_apple_command_spec_v1 *spec,
        const struct ish_apple_command_callbacks_v1 *callbacks,
        struct ish_apple_command_session **session_out) {
    struct ish_apple_command_session *session =
            calloc(1, sizeof(*session));
    if (session == NULL)
        return _ENOMEM;
    atomic_fetch_add_explicit(
            &command_object_count, 1, memory_order_relaxed);
    atomic_init(&session->references, 1);
    atomic_init(&session->pending_wait_status, 0);
    if (pthread_mutex_init(&session->lock, NULL) != 0) {
        free(session);
        atomic_fetch_sub_explicit(
                &command_object_count, 1, memory_order_release);
        return _ENOMEM;
    }
    if (pthread_mutex_init(&session->callback_lock, NULL) != 0) {
        pthread_mutex_destroy(&session->lock);
        free(session);
        atomic_fetch_sub_explicit(
                &command_object_count, 1, memory_order_release);
        return _ENOMEM;
    }
    if (pthread_cond_init(&session->changed, NULL) != 0) {
        pthread_mutex_destroy(&session->callback_lock);
        pthread_mutex_destroy(&session->lock);
        free(session);
        atomic_fetch_sub_explicit(
                &command_object_count, 1, memory_order_release);
        return _ENOMEM;
    }
    session->public_references = 1;
    session->stdin_fd = -1;
    session->exit_event_read_fd = -1;
    session->exit_event_write_fd = -1;
    uint64_t job_id = atomic_fetch_add_explicit(
            &command_next_job_id, 1, memory_order_relaxed);
    if (job_id == 0)
        __builtin_trap();
    session->host_job_id = job_id;
    session->request_id = spec->request_id;
    session->output_limit_enabled = spec->output_byte_limit !=
            ISH_APPLE_COMMAND_OUTPUT_BYTES_DISABLED;
    session->output_byte_limit = !session->output_limit_enabled ? 0 :
            spec->output_byte_limit == 0 ?
            ISH_APPLE_COMMAND_OUTPUT_BYTES_DEFAULT :
            spec->output_byte_limit;
    session->timeout_enabled = spec->timeout_milliseconds !=
            ISH_APPLE_COMMAND_TIMEOUT_MS_DISABLED;
    session->timeout_milliseconds = !session->timeout_enabled ? 0 :
            spec->timeout_milliseconds == 0 ?
            ISH_APPLE_COMMAND_TIMEOUT_MS_DEFAULT :
            spec->timeout_milliseconds;
    session->callbacks = *callbacks;
#ifdef ISH_APPLE_COMMAND_SESSION_TESTING
    session->host_pid = -1;
    for (size_t index = 0; index < 3; index++)
        session->guest_fds[index] = -1;
#endif
    int32_t error = command_register_active(session);
    if (error < 0) {
        command_session_destroy(session);
        return error;
    }
    *session_out = session;
    return 0;
}

static void command_session_process_exited(
        struct ish_apple_command_session *session,
        int32_t wait_status) {
    int stdin_fd = -1;
    pthread_mutex_lock(&session->lock);
    if (!session->process_exited) {
        session->process_exited = true;
        session->wait_status = wait_status;
        stdin_fd = session->stdin_fd;
        session->stdin_fd = -1;
#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
        session->process_group = NULL;
#else
        session->host_pid = -1;
#endif
        pthread_cond_broadcast(&session->changed);
    }
    pthread_mutex_unlock(&session->lock);
    if (stdin_fd >= 0)
        close(stdin_fd);
}


static void command_worker_finished(
        struct ish_apple_command_session *session) {
    pthread_mutex_lock(&session->lock);
    if (session->worker_count == 0)
        __builtin_trap();
    session->worker_count--;
    pthread_cond_broadcast(&session->changed);
    pthread_mutex_unlock(&session->lock);
    command_session_internal_release(session);
}

static int32_t command_cancel_with_reason(
        struct ish_apple_command_session *session,
        int32_t reason);

static void command_dispatch_stream(
        struct ish_apple_command_session *session,
        uint32_t stream,
        const void *bytes,
        uint32_t length,
        int32_t terminal_error) {
    pthread_mutex_lock(&session->callback_lock);
    command_stream_callback_session = session;
    session->callbacks.stream(
            session->callbacks.context,
            session,
            session->request_id,
            stream,
            bytes,
            length,
            terminal_error);
    command_stream_callback_session = NULL;
    pthread_mutex_unlock(&session->callback_lock);
}

static void *command_read_output(void *opaque) {
    struct command_reader *reader = opaque;
    struct ish_apple_command_session *session = reader->session;

    pthread_mutex_lock(&session->lock);
    while (!session->start_decided)
        pthread_cond_wait(&session->changed, &session->lock);
    bool aborted = session->start_aborted;
    pthread_mutex_unlock(&session->lock);

    int32_t terminal_error = 0;
    if (!aborted) {
        unsigned char buffer[COMMAND_OUTPUT_CHUNK];
        for (;;) {
            ssize_t count = read(reader->fd, buffer, sizeof(buffer));
            if (count > 0) {
                uint32_t delivered = (uint32_t) count;
                bool limit_reached = false;
                pthread_mutex_lock(&session->lock);
                uint64_t delivered_total =
                        session->stdout_bytes +
                        session->stderr_bytes;
                if (session->output_limit_enabled &&
                        session->completion_reason ==
                        ISH_APPLE_COMMAND_COMPLETION_OUTPUT_LIMIT) {
                    delivered = 0;
                    limit_reached = true;
                } else if (session->output_limit_enabled) {
                    uint64_t remaining =
                            session->output_byte_limit -
                            delivered_total;
                    if ((uint64_t) delivered > remaining) {
                        delivered = (uint32_t) remaining;
                        limit_reached = true;
                        if (session->completion_reason == 0) {
                            session->completion_reason =
                                    ISH_APPLE_COMMAND_COMPLETION_OUTPUT_LIMIT;
                        }
                    }
                    uint64_t *stream_bytes =
                            reader->stream ==
                                    ISH_APPLE_COMMAND_STREAM_STDOUT ?
                            &session->stdout_bytes :
                            &session->stderr_bytes;
                    *stream_bytes += delivered;
                } else {
                    uint64_t *stream_bytes =
                            reader->stream ==
                                    ISH_APPLE_COMMAND_STREAM_STDOUT ?
                            &session->stdout_bytes :
                            &session->stderr_bytes;
                    *stream_bytes = UINT64_MAX - *stream_bytes < delivered ?
                            UINT64_MAX : *stream_bytes + delivered;
                }
                pthread_mutex_unlock(&session->lock);
                if (delivered != 0) {
                    command_dispatch_stream(
                            session,
                            reader->stream,
                            buffer,
                            delivered,
                            0);
                }
                if (limit_reached) {
                    terminal_error = _EFBIG;
                    (void) command_cancel_with_reason(
                            session,
                            ISH_APPLE_COMMAND_COMPLETION_OUTPUT_LIMIT);
                    break;
                }
                continue;
            }
            if (count == 0)
                break;
            if (errno == EINTR)
                continue;
            terminal_error = command_map_host_error(errno);
            break;
        }
        command_dispatch_stream(
                session,
                reader->stream,
                NULL,
                0,
                terminal_error);
    }
    close(reader->fd);
    reader->fd = -1;

    pthread_mutex_lock(&session->lock);
    if (reader->stream == ISH_APPLE_COMMAND_STREAM_STDOUT)
        session->stdout_ended = true;
    else
        session->stderr_ended = true;
    pthread_cond_broadcast(&session->changed);
    pthread_mutex_unlock(&session->lock);
    command_worker_finished(session);
    return NULL;
}

static uint64_t command_elapsed_milliseconds(
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

static int32_t command_wait_for_exit_event(
        struct ish_apple_command_session *session) {
    bool timeout_delivered = false;
    for (;;) {
        int timeout = -1;
        if (session->timeout_enabled && !timeout_delivered) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                __builtin_trap();
            uint64_t elapsed = command_elapsed_milliseconds(
                    &session->started_at, &now);
            if (elapsed >= session->timeout_milliseconds) {
                timeout_delivered = command_cancel_with_reason(
                        session,
                        ISH_APPLE_COMMAND_COMPLETION_TIMED_OUT) == 0;
                timeout = timeout_delivered ? -1 : 10;
            } else {
                timeout = (int) (
                        session->timeout_milliseconds - elapsed);
            }
        }

        struct pollfd event = {
            .fd = session->exit_event_read_fd,
            .events = POLLIN,
        };
        int ready;
        do {
            ready = poll(&event, 1, timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0)
            __builtin_trap();
        if (ready == 0) {
            if (!timeout_delivered) {
                timeout_delivered = command_cancel_with_reason(
                        session,
                        ISH_APPLE_COMMAND_COMPLETION_TIMED_OUT) == 0;
            }
            continue;
        }

        unsigned char byte;
        ssize_t count;
        do {
            count = read(
                    session->exit_event_read_fd,
                    &byte,
                    sizeof(byte));
        } while (count < 0 && errno == EINTR);
        if (count != (ssize_t) sizeof(byte))
            __builtin_trap();
        return atomic_load_explicit(
                &session->pending_wait_status,
                memory_order_acquire);
    }
}

static void command_build_result(
        struct ish_apple_command_session *session,
        int32_t wait_status) {
    struct timespec finished_at;
    if (clock_gettime(CLOCK_MONOTONIC, &finished_at) != 0)
        __builtin_trap();

    int32_t reason = session->completion_reason;
    int32_t exit_code = -1;
    int32_t termination_signal = 0;
    if ((wait_status & 0x7f) == 0) {
        exit_code = (wait_status >> 8) & 0xff;
        if (reason == 0)
            reason = ISH_APPLE_COMMAND_COMPLETION_EXITED;
    } else if ((wait_status & 0x7f) != 0x7f) {
        termination_signal = wait_status & 0x7f;
        if (reason == 0)
            reason = ISH_APPLE_COMMAND_COMPLETION_SIGNALED;
    } else if (reason == 0) {
        reason = ISH_APPLE_COMMAND_COMPLETION_RUNTIME_FAILURE;
    }

    session->result = (struct ish_apple_command_result_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(session->result),
        .request_id = session->request_id,
        .reason = reason,
        .exit_code = exit_code,
        .termination_signal = termination_signal,
        .error = reason ==
                ISH_APPLE_COMMAND_COMPLETION_RUNTIME_FAILURE ?
                _EIO : 0,
        .stdout_bytes = session->stdout_bytes,
        .stderr_bytes = session->stderr_bytes,
        .elapsed_milliseconds = command_elapsed_milliseconds(
                &session->started_at, &finished_at),
    };
}

static void *command_complete(void *opaque) {
    struct ish_apple_command_session *session = opaque;
    pthread_mutex_lock(&session->lock);
    while (!session->start_decided)
        pthread_cond_wait(&session->changed, &session->lock);
    bool aborted = session->start_aborted;
    pthread_mutex_unlock(&session->lock);

    if (!aborted) {
        int32_t wait_status =
                command_wait_for_exit_event(session);
#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
        // root leader 退出后，按不可逃逸作业 ID 清理仍持有管道的全部后代。
        (void) task_signal_host_job(session->host_job_id, SIGKILL_);
#endif
        command_session_process_exited(session, wait_status);
        command_session_internal_release(session);

        pthread_mutex_lock(&session->lock);
        while (!(session->stdout_ended && session->stderr_ended))
            pthread_cond_wait(&session->changed, &session->lock);
        command_build_result(session, wait_status);
        session->completion_ready = true;
        pthread_cond_broadcast(&session->changed);
        pthread_mutex_unlock(&session->lock);

        pthread_mutex_lock(&session->callback_lock);
        command_exit_callback_session = session;
        session->callbacks.completed(
                session->callbacks.context,
                session,
                &session->result);
        command_exit_callback_session = NULL;
        pthread_mutex_unlock(&session->callback_lock);
        command_unregister_active(session);
    }

    pthread_mutex_lock(&session->lock);
    if (!aborted) {
        session->exit_callback_finished = true;
        pthread_cond_broadcast(&session->changed);
    }
    pthread_mutex_unlock(&session->lock);
    command_worker_finished(session);
    return NULL;
}

static int32_t command_start_worker(
        struct ish_apple_command_session *session,
        void *(*entry)(void *),
        void *context,
        pthread_attr_t *attributes,
        uint32_t ordinal) {
#ifdef ISH_APPLE_COMMAND_SESSION_TESTING
    if (command_backend_test_should_fail_worker(ordinal))
        return _EAGAIN;
#else
    (void) ordinal;
#endif
    command_session_internal_retain(session);
    pthread_mutex_lock(&session->lock);
    session->worker_count++;
    pthread_mutex_unlock(&session->lock);

    pthread_t thread;
    int error = pthread_create(
            &thread, attributes, entry, context);
    if (error == 0)
        return 0;

    pthread_mutex_lock(&session->lock);
    session->worker_count--;
    pthread_mutex_unlock(&session->lock);
    // start 尚未发布调用方引用，失败时只撤销 worker 预留。
    command_session_rollback_internal_retain(session);
    return command_map_host_error(error);
}

static int32_t command_create_pipes(
        struct ish_apple_command_session *session,
        int guest_fds[3]) {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int error_output[2] = {-1, -1};
    int exit_event[2] = {-1, -1};
    if (pipe(input) < 0 || pipe(output) < 0 ||
            pipe(error_output) < 0 || pipe(exit_event) < 0) {
        int error = errno;
        int *all[] = {input, output, error_output, exit_event};
        for (size_t group = 0; group < 4; group++) {
            for (size_t end = 0; end < 2; end++) {
                if (all[group][end] >= 0)
                    close(all[group][end]);
            }
        }
        return command_map_host_error(error);
    }

    session->stdin_fd = input[1];
    session->readers[0] = (struct command_reader) {
        .session = session,
        .stream = ISH_APPLE_COMMAND_STREAM_STDOUT,
        .fd = output[0],
    };
    session->readers[1] = (struct command_reader) {
        .session = session,
        .stream = ISH_APPLE_COMMAND_STREAM_STDERR,
        .fd = error_output[0],
    };
    session->exit_event_read_fd = exit_event[0];
    session->exit_event_write_fd = exit_event[1];
    guest_fds[0] = input[0];
    guest_fds[1] = output[1];
    guest_fds[2] = error_output[1];

    int flags = fcntl(session->stdin_fd, F_GETFL);
    if (flags < 0 ||
            fcntl(session->stdin_fd, F_SETFL,
                    flags | O_NONBLOCK) < 0)
        goto fail;
    flags = fcntl(session->exit_event_write_fd, F_GETFL);
    if (flags < 0 ||
            fcntl(session->exit_event_write_fd, F_SETFL,
                    flags | O_NONBLOCK) < 0)
        goto fail;
#ifdef F_SETNOSIGPIPE
    if (fcntl(session->stdin_fd, F_SETNOSIGPIPE, 1) < 0)
        goto fail;
    if (fcntl(session->exit_event_write_fd, F_SETNOSIGPIPE, 1) < 0)
        goto fail;
#endif
    int host_fds[5] = {
        session->stdin_fd,
        session->readers[0].fd,
        session->readers[1].fd,
        session->exit_event_read_fd,
        session->exit_event_write_fd,
    };
    for (size_t index = 0; index < 5; index++) {
        if (fcntl(host_fds[index], F_SETFD, FD_CLOEXEC) < 0)
            goto fail;
    }
    return 0;

fail: {
        int error = errno;
        close(session->stdin_fd);
        close(session->readers[0].fd);
        close(session->readers[1].fd);
        close(session->exit_event_read_fd);
        close(session->exit_event_write_fd);
        session->stdin_fd = -1;
        session->readers[0].fd = -1;
        session->readers[1].fd = -1;
        session->exit_event_read_fd = -1;
        session->exit_event_write_fd = -1;
        for (size_t index = 0; index < 3; index++) {
            close(guest_fds[index]);
            guest_fds[index] = -1;
        }
        return command_map_host_error(error);
    }
}

static void command_abort_workers(
        struct ish_apple_command_session *session) {
    pthread_mutex_lock(&session->lock);
    session->start_aborted = true;
    session->start_decided = true;
    pthread_cond_broadcast(&session->changed);
    while (session->worker_count != 0)
        pthread_cond_wait(&session->changed, &session->lock);
    pthread_mutex_unlock(&session->lock);
}

int32_t ish_apple_command_session_start(
        const struct ish_apple_command_spec_v1 *spec,
        const struct ish_apple_command_callbacks_v1 *callbacks,
        struct ish_apple_command_session **session_out) {
    if (session_out == NULL)
        return _EINVAL;
    *session_out = NULL;

    struct command_arguments arguments = {};
    int32_t error = command_arguments_create(
            spec, callbacks, &arguments);
    if (error < 0)
        return error;

    struct ish_apple_command_session *session = NULL;
    error = command_session_create(spec, callbacks, &session);
    if (error < 0) {
        command_arguments_destroy(&arguments);
        return error;
    }

    int guest_fds[3] = {-1, -1, -1};
    error = command_create_pipes(session, guest_fds);
    if (error < 0)
        goto fail_session;

    error = command_backend_prepare(
            session, &arguments, guest_fds);
    if (error < 0)
        goto fail_pipes;

    pthread_attr_t attributes;
    int attribute_error = pthread_attr_init(&attributes);
    bool attributes_initialized = attribute_error == 0;
    if (attributes_initialized)
        attribute_error = pthread_attr_setdetachstate(
                &attributes, PTHREAD_CREATE_DETACHED);
    if (attribute_error != 0) {
        if (attributes_initialized)
            pthread_attr_destroy(&attributes);
        error = command_map_host_error(attribute_error);
        goto fail_prepared;
    }

    error = command_start_worker(
            session, command_read_output,
            &session->readers[0], &attributes, 1);
    if (error == 0)
        error = command_start_worker(
                session, command_read_output,
                &session->readers[1], &attributes, 2);
    if (error == 0)
        error = command_start_worker(
                session, command_complete,
                session, &attributes, 3);
    pthread_attr_destroy(&attributes);
    if (error < 0)
        goto fail_prepared;

    if (clock_gettime(CLOCK_MONOTONIC, &session->started_at) != 0) {
        error = command_map_host_error(errno);
        goto fail_prepared;
    }
    error = command_backend_commit(session, &arguments);
    if (error < 0)
        goto fail_prepared;

    command_arguments_destroy(&arguments);
    // 回调线程放行前先发布句柄；回调可能在 start 返回前开始执行。
    *session_out = session;
    pthread_mutex_lock(&session->lock);
    session->start_decided = true;
    pthread_cond_broadcast(&session->changed);
    pthread_mutex_unlock(&session->lock);
    return 0;

fail_prepared:
    command_backend_cancel_prepared(session);
    command_abort_workers(session);
fail_pipes:
    if (session->stdin_fd >= 0) {
        close(session->stdin_fd);
        session->stdin_fd = -1;
    }
    for (size_t index = 0; index < 2; index++) {
        if (session->readers[index].fd >= 0) {
            close(session->readers[index].fd);
            session->readers[index].fd = -1;
        }
    }
fail_session:
    for (size_t index = 0; index < 3; index++) {
        if (guest_fds[index] >= 0)
            close(guest_fds[index]);
    }
    command_arguments_destroy(&arguments);
    command_session_internal_release(session);
    return error;
}

struct ish_apple_command_session *ish_apple_command_session_retain(
        struct ish_apple_command_session *session) {
    if (session == NULL)
        return NULL;
    pthread_mutex_lock(&session->lock);
    bool callback_borrow =
            command_stream_callback_session == session ||
            command_exit_callback_session == session;
    if ((session->public_references == 0 && !callback_borrow) ||
            session->public_references == UINT32_MAX) {
        pthread_mutex_unlock(&session->lock);
        return NULL;
    }
    session->public_references++;
    command_session_internal_retain(session);
    pthread_mutex_unlock(&session->lock);
    return session;
}

void ish_apple_command_session_release(
        struct ish_apple_command_session *session) {
    if (session == NULL)
        return;
    pthread_mutex_lock(&session->lock);
    if (session->public_references == 0) {
        pthread_mutex_unlock(&session->lock);
        return;
    }
    session->public_references--;
    bool last = session->public_references == 0;
    pthread_mutex_unlock(&session->lock);
    if (last) {
        (void) ish_apple_command_session_cancel(session);
        (void) ish_apple_command_session_close_stdin(session);
    }
    command_session_internal_release(session);
}

int32_t ish_apple_command_session_write_stdin(
        struct ish_apple_command_session *session,
        const void *bytes,
        uint32_t length,
        uint32_t *accepted_out) {
    if (session == NULL || accepted_out == NULL ||
            (bytes == NULL && length != 0))
        return _EINVAL;
    *accepted_out = 0;
    if (length > ISH_APPLE_COMMAND_STDIN_WRITE_BYTES_MAX)
        return _EMSGSIZE;
    if (length == 0)
        return 0;

    pthread_mutex_lock(&session->lock);
    if (session->process_exited || session->cancel_requested) {
        pthread_mutex_unlock(&session->lock);
        return _ESHUTDOWN;
    }
    if (session->stdin_fd < 0) {
        pthread_mutex_unlock(&session->lock);
        return _EPIPE;
    }
    ssize_t written;
    do {
        written = write(session->stdin_fd, bytes, length);
    } while (written < 0 && errno == EINTR);
    int error = errno;
    pthread_mutex_unlock(&session->lock);
    if (written < 0)
        return command_map_host_error(error);
    *accepted_out = (uint32_t) written;
    return 0;
}

int32_t ish_apple_command_session_close_stdin(
        struct ish_apple_command_session *session) {
    if (session == NULL)
        return _EINVAL;
    pthread_mutex_lock(&session->lock);
    int fd = session->stdin_fd;
    session->stdin_fd = -1;
    pthread_mutex_unlock(&session->lock);
    if (fd >= 0)
        close(fd);
    return 0;
}

int32_t ish_apple_command_session_interrupt(
        struct ish_apple_command_session *session) {
    if (session == NULL)
        return _EINVAL;
#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
    return command_backend_signal(session, SIGINT_);
#else
    return command_backend_signal(session, SIGINT);
#endif
}

static int32_t command_cancel_with_reason(
        struct ish_apple_command_session *session,
        int32_t reason) {
    pthread_mutex_lock(&session->lock);
    while (session->cancel_in_flight && !session->process_exited)
        pthread_cond_wait(&session->changed, &session->lock);
    if (session->process_exited || session->cancel_delivered) {
        if (!session->process_exited)
            session->cancel_requested = true;
        pthread_mutex_unlock(&session->lock);
        return 0;
    }
    bool installed_reason = session->completion_reason == 0;
    if (installed_reason)
        session->completion_reason = reason;
    session->cancel_requested = true;
    session->cancel_in_flight = true;
    pthread_mutex_unlock(&session->lock);
    (void) ish_apple_command_session_close_stdin(session);
#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
    int32_t error = command_backend_signal(session, SIGKILL_);
#else
    int32_t error = command_backend_signal(session, SIGKILL);
#endif
    bool delivered = error == 0 ||
            error == _ESHUTDOWN || error == _ESRCH;
    pthread_mutex_lock(&session->lock);
    if (!delivered && installed_reason &&
            session->completion_reason == reason)
        session->completion_reason = 0;
    session->cancel_delivered |= delivered;
    session->cancel_in_flight = false;
    pthread_cond_broadcast(&session->changed);
    pthread_mutex_unlock(&session->lock);
    return delivered ? 0 : error;
}

int32_t ish_apple_command_session_cancel(
        struct ish_apple_command_session *session) {
    if (session == NULL)
        return _EINVAL;
    return command_cancel_with_reason(
            session,
            ISH_APPLE_COMMAND_COMPLETION_CANCELLED);
}

int32_t ish_apple_command_session_wait(
        struct ish_apple_command_session *session,
        struct ish_apple_command_result_v1 *result_out) {
    if (session == NULL || result_out == NULL)
        return _EINVAL;
    if (command_stream_callback_session == session ||
            command_exit_callback_session == session)
        return _EDEADLK;
    pthread_mutex_lock(&session->lock);
    while (!session->exit_callback_finished ||
            session->worker_count != 0)
        pthread_cond_wait(&session->changed, &session->lock);
    *result_out = session->result;
    pthread_mutex_unlock(&session->lock);
    return 0;
}

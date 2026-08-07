#ifndef PLATFORM_APPLE_COMMAND_SESSION_PRIVATE_H
#define PLATFORM_APPLE_COMMAND_SESSION_PRIVATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "platform/apple-command-session.h"

#pragma GCC visibility push(hidden)

struct tgroup;

struct command_arguments {
    uint64_t request_id;
    char *executable;
    char *argument_bytes;
    char **argument_vector;
    uint32_t argument_count;
    char *environment_bytes;
    char **environment_vector;
    uint32_t environment_count;
    char *working_directory;
};

struct command_reader {
    struct ish_apple_command_session *session;
    uint32_t stream;
    int fd;
};

struct ish_apple_command_session {
    _Atomic uint32_t references;
    pthread_mutex_t lock;
    pthread_mutex_t callback_lock;
    pthread_cond_t changed;
    uint32_t public_references;
    uint32_t worker_count;
    int stdin_fd;
    int exit_event_read_fd;
    int exit_event_write_fd;
    bool start_decided;
    bool start_aborted;
    bool process_exited;
    bool stdout_ended;
    bool stderr_ended;
    bool completion_ready;
    bool exit_callback_finished;
    bool cancel_requested;
    bool cancel_delivered;
    bool cancel_in_flight;
    int32_t wait_status;
    _Atomic int32_t pending_wait_status;
    uint64_t request_id;
    uint64_t host_job_id;
    uint64_t output_byte_limit;
    bool output_limit_enabled;
    uint64_t stdout_bytes;
    uint64_t stderr_bytes;
    uint32_t timeout_milliseconds;
    bool timeout_enabled;
    int32_t completion_reason;
    struct timespec started_at;
    struct ish_apple_command_result_v1 result;
    struct ish_apple_command_callbacks_v1 callbacks;
    struct command_reader readers[2];
    struct ish_apple_command_session *active_next;
    bool active_registered;
#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
    struct tgroup *process_group;
    struct ish_apple_command_session *registry_next;
    bool registry_linked;
#else
    pid_t host_pid;
    int guest_fds[3];
#endif
};

int32_t command_arguments_create(
        const struct ish_apple_command_spec_v1 *spec,
        const struct ish_apple_command_callbacks_v1 *callbacks,
        struct command_arguments *arguments);
int32_t command_arguments_create_for_spec(
        const struct ish_apple_command_spec_v1 *spec,
        struct command_arguments *arguments);
void command_arguments_destroy(struct command_arguments *arguments);

int32_t command_map_host_error(int error);
void command_session_internal_retain(
        struct ish_apple_command_session *session);
void command_session_internal_release(
        struct ish_apple_command_session *session);
void command_session_rollback_internal_retain(
        struct ish_apple_command_session *session);

int32_t command_backend_prepare(
        struct ish_apple_command_session *session,
        const struct command_arguments *arguments,
        int guest_fds[3]);
int32_t command_backend_commit(
        struct ish_apple_command_session *session,
        const struct command_arguments *arguments);
void command_backend_cancel_prepared(
        struct ish_apple_command_session *session);
int32_t command_backend_signal(
        struct ish_apple_command_session *session, int signal);

#ifdef ISH_APPLE_COMMAND_SESSION_TESTING
bool command_backend_test_should_fail_worker(uint32_t ordinal);
#endif

#pragma GCC visibility pop

#endif

#include "platform/apple-command-session-private.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "kernel/errno.h"

#ifndef ISH_APPLE_COMMAND_SESSION_TESTING
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "platform/apple-watch-runtime-private.h"
#include "util/list.h"
#else
#include <sys/wait.h>
#endif

/*
 * 退出观察器只记录一个固定大小事件并写入非阻塞管道。registry 持有的内部
 * 引用随事件转交给 completion worker，由后者在 pids_lock 之外释放。
 */
static void command_publish_exit_event(
        struct ish_apple_command_session *session,
        int32_t wait_status) {
    atomic_store_explicit(
            &session->pending_wait_status,
            wait_status,
            memory_order_release);
    unsigned char event = 1;
    ssize_t written;
    do {
        written = write(
                session->exit_event_write_fd,
                &event,
                sizeof(event));
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t) sizeof(event))
        __builtin_trap();
}

#ifndef ISH_APPLE_COMMAND_SESSION_TESTING

static struct ish_apple_command_session *command_registry;
static pthread_mutex_t command_observer_lock = PTHREAD_MUTEX_INITIALIZER;
static bool command_observer_installed;

static void command_exit_observer(
        struct task *task, int code, void *context) {
    (void) code;
    (void) context;
    // task_notify_exit_locked 保证整个回调期间持有 pids_lock。
    struct tgroup *group = task->group;
    struct task *leader = group->leader;
    int32_t wait_status = (int32_t) leader->exit_code;
    if (group->doing_group_exit)
        wait_status = (int32_t) group->group_exit_code;

    struct ish_apple_command_session **link = &command_registry;
    struct ish_apple_command_session *session = NULL;
    while (*link != NULL) {
        if ((*link)->process_group == group) {
            session = *link;
            *link = session->registry_next;
            session->registry_next = NULL;
            session->registry_linked = false;
            break;
        }
        link = &(*link)->registry_next;
    }
    if (session == NULL)
        return;

    command_publish_exit_event(session, wait_status);
}

static int32_t command_install_exit_observer(void) {
    pthread_mutex_lock(&command_observer_lock);
    int32_t error = 0;
    if (!command_observer_installed) {
        error = task_exit_observer_register(
                command_exit_observer, NULL);
        if (error == 0)
            command_observer_installed = true;
    }
    pthread_mutex_unlock(&command_observer_lock);
    return error;
}

static void command_registry_add(
        struct ish_apple_command_session *session) {
    command_session_internal_retain(session);
    lock(&pids_lock);
    session->registry_next = command_registry;
    session->registry_linked = true;
    command_registry = session;
    unlock(&pids_lock);
}

static void command_registry_remove(
        struct ish_apple_command_session *session) {
    bool removed = false;
    lock(&pids_lock);
    struct ish_apple_command_session **link = &command_registry;
    while (*link != NULL) {
        if (*link == session) {
            *link = session->registry_next;
            session->registry_next = NULL;
            session->registry_linked = false;
            removed = true;
            break;
        }
        link = &(*link)->registry_next;
    }
    unlock(&pids_lock);
    if (removed)
        command_session_rollback_internal_retain(session);
}

int32_t command_backend_prepare(
        struct ish_apple_command_session *session,
        const struct command_arguments *arguments,
        int guest_fds[3]) {
    int32_t error = command_install_exit_observer();
    if (error < 0)
        goto fail_fds;

    /*
     * current/prepared_process 是进程级事务状态；与可见终端及托管文件操作
     * 共用此锁，让并发公共 start 排队而不是竞争或返回瞬时 EBUSY。
     */
    lock(&ish_watch_prepared_task_lock);
    error = begin_new_init_child();
    if (error < 0)
        goto fail_locked_fds;
    if (!task_set_host_job_id(current, session->host_job_id)) {
        error = _EINVAL;
        goto fail_prepared;
    }
    if (!task_set_host_diagnostic_context(
                current,
                TASK_HOST_DIAGNOSTIC_COMMAND,
                session->request_id)) {
        error = _EINVAL;
        goto fail_prepared;
    }

    if (arguments->working_directory != NULL) {
        error = file_chdir_task(
                current, arguments->working_directory);
        if (error < 0)
            goto fail_prepared;
    }

    error = create_host_stdio(
            current, guest_fds[0], guest_fds[1], guest_fds[2]);
    guest_fds[0] = guest_fds[1] = guest_fds[2] = -1;
    if (error < 0)
        goto fail_prepared;

    error = do_execve(
            arguments->executable,
            arguments->argument_count,
            arguments->argument_bytes,
            arguments->environment_bytes);
    if (error < 0)
        goto fail_prepared;

    session->process_group = current->group;
    return 0;

fail_prepared:
    cancel_prepared_process();
    unlock(&ish_watch_prepared_task_lock);
    return error;
fail_locked_fds:
    unlock(&ish_watch_prepared_task_lock);
fail_fds:
    for (size_t index = 0; index < 3; index++) {
        if (guest_fds[index] >= 0) {
            close(guest_fds[index]);
            guest_fds[index] = -1;
        }
    }
    return error;
}

int32_t command_backend_commit(
        struct ish_apple_command_session *session,
        const struct command_arguments *arguments) {
    (void) arguments;
    command_registry_add(session);
    int32_t error = commit_prepared_process();
    if (error < 0) {
        command_registry_remove(session);
    } else {
        unlock(&ish_watch_prepared_task_lock);
    }
    return error;
}

void command_backend_cancel_prepared(
        struct ish_apple_command_session *session) {
    command_registry_remove(session);
    session->process_group = NULL;
    cancel_prepared_process();
    unlock(&ish_watch_prepared_task_lock);
}

int32_t command_backend_signal(
        struct ish_apple_command_session *session, int signal) {
    pthread_mutex_lock(&session->lock);
    bool exited = session->process_exited;
    pthread_mutex_unlock(&session->lock);
    if (exited)
        return _ESHUTDOWN;
    return task_signal_host_job(
            session->host_job_id, signal) != 0 ? 0 : _ESRCH;
}

#else

#define ISH_COMMAND_TEST_FAIL_NONE 0
#define ISH_COMMAND_TEST_FAIL_PREPARE 1
#define ISH_COMMAND_TEST_FAIL_WORKER 2
#define ISH_COMMAND_TEST_FAIL_COMMIT 3

static _Atomic int command_test_failure;
static _Atomic uint32_t command_test_worker_failure_ordinal;
static _Atomic int32_t command_test_signal_failure;

void ish_apple_command_session_test_fail_once(int stage) {
    atomic_store_explicit(
            &command_test_failure, stage, memory_order_release);
}

void ish_apple_command_session_test_fail_worker_once(
        uint32_t ordinal) {
    atomic_store_explicit(
            &command_test_worker_failure_ordinal,
            ordinal,
            memory_order_release);
}

void ish_apple_command_session_test_fail_signal_once(
        int32_t linux_error) {
    atomic_store_explicit(
            &command_test_signal_failure,
            linux_error,
            memory_order_release);
}

static bool command_test_take_failure(int stage) {
    int expected = stage;
    return atomic_compare_exchange_strong_explicit(
            &command_test_failure, &expected,
            ISH_COMMAND_TEST_FAIL_NONE,
            memory_order_acq_rel, memory_order_acquire);
}

bool command_backend_test_should_fail_worker(uint32_t ordinal) {
    uint32_t expected_ordinal = ordinal;
    return command_test_take_failure(ISH_COMMAND_TEST_FAIL_WORKER) ||
            atomic_compare_exchange_strong_explicit(
                    &command_test_worker_failure_ordinal,
                    &expected_ordinal, 0,
                    memory_order_acq_rel, memory_order_acquire);
}

int32_t command_backend_prepare(
        struct ish_apple_command_session *session,
        const struct command_arguments *arguments,
        int guest_fds[3]) {
    (void) arguments;
    if (command_test_take_failure(
            ISH_COMMAND_TEST_FAIL_PREPARE)) {
        for (size_t index = 0; index < 3; index++) {
            close(guest_fds[index]);
            guest_fds[index] = -1;
        }
        return _EIO;
    }
    for (size_t index = 0; index < 3; index++) {
        session->guest_fds[index] = guest_fds[index];
        guest_fds[index] = -1;
    }
    return 0;
}

static void command_test_close_guest_fds(
        struct ish_apple_command_session *session) {
    for (size_t index = 0; index < 3; index++) {
        if (session->guest_fds[index] >= 0) {
            close(session->guest_fds[index]);
            session->guest_fds[index] = -1;
        }
    }
}

struct command_test_waiter {
    struct ish_apple_command_session *session;
    pid_t pid;
};

static void *command_test_wait(void *opaque) {
    struct command_test_waiter *waiter = opaque;
    int status = 0;
    while (waitpid(waiter->pid, &status, 0) < 0 && errno == EINTR)
        ;
    (void) kill(-waiter->pid, SIGKILL);
    command_publish_exit_event(waiter->session, (int32_t) status);
    free(waiter);
    return NULL;
}

int32_t command_backend_commit(
        struct ish_apple_command_session *session,
        const struct command_arguments *arguments) {
    if (command_test_take_failure(ISH_COMMAND_TEST_FAIL_COMMIT)) {
        command_test_close_guest_fds(session);
        return _EIO;
    }

    int exec_error[2];
    if (pipe(exec_error) < 0) {
        command_test_close_guest_fds(session);
        return command_map_host_error(errno);
    }
    if (fcntl(exec_error[1], F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno;
        close(exec_error[0]);
        close(exec_error[1]);
        command_test_close_guest_fds(session);
        return command_map_host_error(error);
    }

    pid_t pid = fork();
    if (pid < 0) {
        int error = errno;
        close(exec_error[0]);
        close(exec_error[1]);
        command_test_close_guest_fds(session);
        return command_map_host_error(error);
    }
    if (pid == 0) {
        close(exec_error[0]);
        sigset_t unblocked;
        sigemptyset(&unblocked);
        (void) sigprocmask(SIG_SETMASK, &unblocked, NULL);
        (void) setpgid(0, 0);
        for (int number = 0; number < 3; number++) {
            if (dup2(session->guest_fds[number], number) < 0)
                goto child_fail;
        }
        command_test_close_guest_fds(session);
        if (arguments->working_directory != NULL &&
                chdir(arguments->working_directory) < 0)
            goto child_fail;
        execve(arguments->executable,
                arguments->argument_vector,
                arguments->environment_vector);
child_fail: {
            int error = errno;
            (void) write(exec_error[1], &error, sizeof(error));
            _exit(127);
        }
    }

    close(exec_error[1]);
    command_test_close_guest_fds(session);
    (void) setpgid(pid, pid);
    int child_error = 0;
    ssize_t count;
    do {
        count = read(
                exec_error[0], &child_error, sizeof(child_error));
    } while (count < 0 && errno == EINTR);
    close(exec_error[0]);
    if (count != 0) {
        (void) kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        return count == (ssize_t) sizeof(child_error) ?
                command_map_host_error(child_error) : _EIO;
    }

    struct command_test_waiter *waiter = malloc(sizeof(*waiter));
    if (waiter == NULL) {
        (void) kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        return _ENOMEM;
    }
    *waiter = (struct command_test_waiter) {
        .session = session,
        .pid = pid,
    };
    pthread_mutex_lock(&session->lock);
    session->host_pid = pid;
    pthread_mutex_unlock(&session->lock);
    command_session_internal_retain(session);
    pthread_t thread;
    int thread_error = pthread_create(
            &thread, NULL, command_test_wait, waiter);
    if (thread_error != 0) {
        pthread_mutex_lock(&session->lock);
        session->host_pid = -1;
        pthread_mutex_unlock(&session->lock);
        command_session_rollback_internal_retain(session);
        free(waiter);
        (void) kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
        return command_map_host_error(thread_error);
    }
    pthread_detach(thread);
    return 0;
}

void command_backend_cancel_prepared(
        struct ish_apple_command_session *session) {
    command_test_close_guest_fds(session);
}

int32_t command_backend_signal(
        struct ish_apple_command_session *session, int signal) {
    int32_t injected = atomic_exchange_explicit(
            &command_test_signal_failure, 0, memory_order_acq_rel);
    if (injected != 0)
        return injected;
    pthread_mutex_lock(&session->lock);
    pid_t pid = session->host_pid;
    bool exited = session->process_exited;
    int result = !exited && pid > 0 ? kill(-pid, signal) : -1;
    int error = errno;
    pthread_mutex_unlock(&session->lock);
    if (exited || pid <= 0)
        return _ESHUTDOWN;
    return result == 0 ? 0 : command_map_host_error(error);
}

#endif

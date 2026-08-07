#include "platform/apple-watch-runtime.h"

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "debug.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/proc/ish.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "platform/apple-resolver.h"
#include "platform/apple-runtime-mount.h"
#include "platform/apple-watch-runtime-private.h"


#define WATCH_SHARED_PARENT "/mnt"
#define WATCH_SHARED_MOUNT_POINT "/mnt/shared"

enum watch_directory_event {
    WATCH_DIRECTORY_AFTER_LSTAT = 1,
    WATCH_DIRECTORY_AFTER_VALIDATION = 2,
    WATCH_DIRECTORY_AFTER_RELEASE = 3,
};
#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
_Static_assert(
        (int) WATCH_DIRECTORY_AFTER_LSTAT ==
                ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_LSTAT,
        "测试目录事件值必须保持一致");
_Static_assert(
        (int) WATCH_DIRECTORY_AFTER_VALIDATION ==
                ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_VALIDATION,
        "测试目录事件值必须保持一致");
_Static_assert(
        (int) WATCH_DIRECTORY_AFTER_RELEASE ==
                ISH_WATCH_RUNTIME_TEST_DIRECTORY_AFTER_RELEASE,
        "测试目录事件值必须保持一致");
#endif

static _Atomic int runtime_phase = ISH_WATCH_RUNTIME_IDLE;
static _Atomic int runtime_error;
static _Atomic bool runtime_accepts_sessions;
static const char *runtime_documents_directory;
#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
static ish_watch_runtime_test_directory_hook runtime_test_directory_hook;
#endif

lock_t ish_watch_prepared_task_lock = LOCK_INITIALIZER;

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
void ish_watch_runtime_test_set_directory_hook(
        ish_watch_runtime_test_directory_hook hook) {
    runtime_test_directory_hook = hook;
}
#endif

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

static void watch_test_directory_event(
        int32_t stage, const char *path, int directory_fd) {
#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
    if (runtime_test_directory_hook != NULL)
        runtime_test_directory_hook(stage, path, directory_fd);
#else
    (void) stage;
    (void) path;
    (void) directory_fd;
#endif
}

static void watch_release_host_directory(
        int *directory_fd, const char *path) {
    if (*directory_fd < 0)
        return;
    int released_fd = *directory_fd;
    *directory_fd = -1;
    (void) close(released_fd);
    watch_test_directory_event(
            WATCH_DIRECTORY_AFTER_RELEASE, path, released_fd);
}

static bool watch_same_host_file(
        const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev &&
            left->st_ino == right->st_ino;
}

static int watch_open_host_directory(
        const char *path,
        int *directory_fd,
        char **canonical_source) {
    *directory_fd = -1;
    *canonical_source = NULL;

    struct stat original_status;
    if (lstat(path, &original_status) < 0)
        return errno_map();
    if (S_ISLNK(original_status.st_mode))
        return _ELOOP;
    if (!S_ISDIR(original_status.st_mode))
        return _ENOTDIR;

    watch_test_directory_event(
            WATCH_DIRECTORY_AFTER_LSTAT, path, -1);

    int directory = open(
            path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return errno_map();

    int error;
    struct stat opened_status;
    if (fstat(directory, &opened_status) < 0) {
        error = errno_map();
        goto failed;
    }
    if (!S_ISDIR(opened_status.st_mode)) {
        error = _ENOTDIR;
        goto failed;
    }

    struct stat current_status;
    if (lstat(path, &current_status) < 0) {
        error = errno_map();
        goto failed;
    }
    if (S_ISLNK(current_status.st_mode)) {
        error = _ELOOP;
        goto failed;
    }
    if (!S_ISDIR(current_status.st_mode)) {
        error = _ENOTDIR;
        goto failed;
    }
    if (!watch_same_host_file(&original_status, &opened_status) ||
            !watch_same_host_file(&opened_status, &current_status)) {
        error = _ESTALE;
        goto failed;
    }

    char *canonical = realpath(path, NULL);
    if (canonical == NULL) {
        error = errno_map();
        goto failed;
    }
    struct stat canonical_status;
    if (stat(canonical, &canonical_status) < 0) {
        error = errno_map();
        free(canonical);
        goto failed;
    }
    if (!watch_same_host_file(&opened_status, &canonical_status)) {
        free(canonical);
        error = _ESTALE;
        goto failed;
    }

    *directory_fd = directory;
    *canonical_source = canonical;
    return 0;

failed:
    watch_release_host_directory(&directory, path);
    return error;
}

static char *watch_get_documents_directory(void) {
    return strdup(runtime_documents_directory);
}

static int watch_ensure_guest_directory(const char *path) {
    struct fd *directory = generic_open(
            path, O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_, 0);
    if (!IS_ERR(directory))
        return fd_close(directory);
    if (PTR_ERR(directory) != _ENOENT)
        return (int) PTR_ERR(directory);

    int error = generic_mkdirat(AT_PWD, path, 0755);
    if (error < 0 && error != _EEXIST)
        return error;

    directory = generic_open(
            path, O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_, 0);
    if (IS_ERR(directory))
        return (int) PTR_ERR(directory);
    return fd_close(directory);
}

static int watch_mount_shared_directory(
        int host_directory_fd, const char *host_directory_source) {
    int error = watch_ensure_guest_directory(WATCH_SHARED_PARENT);
    if (error < 0)
        return error;
    error = watch_ensure_guest_directory(WATCH_SHARED_MOUNT_POINT);
    if (error < 0)
        return error;

    char mount_point[] = WATCH_SHARED_MOUNT_POINT;
    struct mount *existing = mount_find(mount_point);
    bool already_mounted =
            strcmp(existing->point, WATCH_SHARED_MOUNT_POINT) == 0;
    mount_release(existing);
    if (already_mounted)
        return _EBUSY;

    lock(&mounts_lock);
    error = realfs_mount_from_fd_locked(
            host_directory_fd,
            host_directory_source,
            WATCH_SHARED_MOUNT_POINT,
            "",
            0);
    unlock(&mounts_lock);
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

    ish_watch_session_handle_exit(
            leader->group, (int32_t) wait_status, controlling_tty);
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

int exec_shell_command(
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

int ish_watch_runtime_operation_availability(void) {
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


int ish_watch_runtime_start(
        const char *root_data,
        const char *documents_directory,
        const char *socket_prefix,
        const char *hostname,
        const char *boot_command) {
    if (root_data == NULL || root_data[0] == '\0' ||
            documents_directory == NULL ||
            documents_directory[0] == '\0' ||
            socket_prefix == NULL || socket_prefix[0] == '\0' ||
            hostname == NULL || hostname[0] == '\0' ||
            boot_command == NULL || boot_command[0] == '\0')
        return _EINVAL;
    if (!socket_prefix_fits(socket_prefix))
        return _ENAMETOOLONG;
    size_t command_length = strlen(boot_command);
    if (command_length >= WATCH_COMMAND_LIMIT)
        return _E2BIG;

    int shared_directory_fd;
    char *canonical_documents_source;
    int error = watch_open_host_directory(
            documents_directory,
            &shared_directory_fd,
            &canonical_documents_source);
    if (error < 0)
        return error;
    watch_test_directory_event(
            WATCH_DIRECTORY_AFTER_VALIDATION,
            documents_directory,
            shared_directory_fd);

    int expected = ISH_WATCH_RUNTIME_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime_phase, &expected, ISH_WATCH_RUNTIME_PREPARING,
            memory_order_acq_rel, memory_order_acquire)) {
        watch_release_host_directory(
                &shared_directory_fd, documents_directory);
        free(canonical_documents_source);
        return _EALREADY;
    }
    atomic_store_explicit(
            &runtime_accepts_sessions, false, memory_order_release);

    char *owned_socket_prefix = strdup(socket_prefix);
    if (owned_socket_prefix == NULL) {
        watch_release_host_directory(
                &shared_directory_fd, documents_directory);
        free(canonical_documents_source);
        return runtime_fail(_ENOMEM);
    }
    char *owned_hostname = strdup(hostname);
    if (owned_hostname == NULL) {
        watch_release_host_directory(
                &shared_directory_fd, documents_directory);
        free(canonical_documents_source);
        free(owned_socket_prefix);
        return runtime_fail(_ENOMEM);
    }
    char *owned_documents_directory = strdup(documents_directory);
    if (owned_documents_directory == NULL) {
        watch_release_host_directory(
                &shared_directory_fd, documents_directory);
        free(canonical_documents_source);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail(_ENOMEM);
    }

    error = mount_root(&fakefs, root_data);
    if (error < 0) {
        watch_release_host_directory(
                &shared_directory_fd, documents_directory);
        free(canonical_documents_source);
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail(error);
    }

    error = begin_first_process();
    if (error < 0) {
        watch_release_host_directory(
                &shared_directory_fd, documents_directory);
        free(canonical_documents_source);
        free(owned_documents_directory);
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

    error = watch_mount_shared_directory(
            shared_directory_fd, canonical_documents_source);
    watch_release_host_directory(
            &shared_directory_fd, documents_directory);
    free(canonical_documents_source);
    if (error < 0) {
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }
    error = ish_apple_mount_activate_startup(current);
    if (error < 0) {
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }
    error = do_mount(&procfs, "proc", "/proc", "", 0);
    if (error < 0) {
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }
    error = do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    if (error < 0) {
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    exit_hook = watch_handle_exit;
    die_handler = watch_handle_die;
    ish_watch_terminal_install_console();
    error = create_stdio(
            "/dev/console", TTY_CONSOLE_MAJOR, WATCH_CONSOLE_NUMBER);
    if (error < 0) {
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    error = exec_shell_command(boot_command, command_length);
    if (error < 0) {
        free(owned_documents_directory);
        free(owned_hostname);
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    extern const char *uname_hostname_override;
    uname_hostname_override = owned_hostname;
    sock_tmp_prefix = owned_socket_prefix;
    runtime_documents_directory = owned_documents_directory;
    get_documents_directory = watch_get_documents_directory;
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

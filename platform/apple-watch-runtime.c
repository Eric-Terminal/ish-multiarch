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
#include "platform/apple-rootfs-seed.h"

#define WATCH_OUTPUT_CAPACITY (64 * 1024)
#define WATCH_CONSOLE_NUMBER 1
#define WATCH_DEFAULT_COLUMNS 40
#define WATCH_DEFAULT_ROWS 18

static _Atomic int runtime_phase = ISH_WATCH_RUNTIME_IDLE;
static _Atomic int runtime_error;

static unsigned char output_buffer[WATCH_OUTPUT_CAPACITY];
static size_t output_head;
static size_t output_count;
static uint64_t output_dropped;
static lock_t output_lock = LOCK_INITIALIZER;

static void output_note_dropped(size_t length) {
    uint64_t increment = (uint64_t) length;
    if (UINT64_MAX - output_dropped < increment)
        output_dropped = UINT64_MAX;
    else
        output_dropped += increment;
}

static void output_append(const void *bytes, size_t length) {
    if (length == 0)
        return;
    const unsigned char *source = bytes;
    lock(&output_lock);

    if (length >= sizeof(output_buffer)) {
        output_note_dropped(output_count);
        output_note_dropped(length - sizeof(output_buffer));
        source += length - sizeof(output_buffer);
        length = sizeof(output_buffer);
        memcpy(output_buffer, source, length);
        output_head = 0;
        output_count = length;
        unlock(&output_lock);
        return;
    }

    size_t overflow = output_count + length > sizeof(output_buffer) ?
            output_count + length - sizeof(output_buffer) : 0;
    output_note_dropped(overflow);
    output_head = (output_head + overflow) % sizeof(output_buffer);
    output_count -= overflow;

    size_t tail = (output_head + output_count) % sizeof(output_buffer);
    size_t first = sizeof(output_buffer) - tail;
    if (first > length)
        first = length;
    memcpy(output_buffer + tail, source, first);
    memcpy(output_buffer, source + first, length - first);
    output_count += length;
    unlock(&output_lock);
}

size_t ish_watch_runtime_read_output(
        void *buffer, size_t capacity, uint64_t *dropped_bytes) {
    lock(&output_lock);
    if (dropped_bytes != NULL) {
        *dropped_bytes = output_dropped;
        output_dropped = 0;
    }
    if (buffer == NULL || capacity == 0) {
        unlock(&output_lock);
        return 0;
    }

    size_t length = output_count < capacity ? output_count : capacity;
    size_t first = sizeof(output_buffer) - output_head;
    if (first > length)
        first = length;
    memcpy(buffer, output_buffer + output_head, first);
    memcpy((unsigned char *) buffer + first, output_buffer, length - first);
    output_head = (output_head + length) % sizeof(output_buffer);
    output_count -= length;
    unlock(&output_lock);
    return length;
}

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
void ish_watch_runtime_test_append_output(const void *bytes, size_t length) {
    output_append(bytes, length);
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
    atomic_store_explicit(&runtime_error, error, memory_order_release);
    atomic_store_explicit(
            &runtime_phase, ISH_WATCH_RUNTIME_FAILED, memory_order_release);
    return error;
}

static int runtime_fail_after_task(int error) {
    current = NULL;
    return runtime_fail(error);
}

static void watch_handle_exit(struct task *task, int UNUSED(code)) {
    if (task->parent == NULL) {
        atomic_store_explicit(
                &runtime_phase, ISH_WATCH_RUNTIME_STOPPED,
                memory_order_release);
    }
}

static void watch_handle_die(const char *UNUSED(message)) {
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

int ish_watch_runtime_start(
        const char *seed_root,
        const char *persistent_parent,
        const char *socket_prefix) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            socket_prefix == NULL || socket_prefix[0] == '\0')
        return _EINVAL;
    if (!socket_prefix_fits(socket_prefix))
        return _ENAMETOOLONG;

    int expected = ISH_WATCH_RUNTIME_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime_phase, &expected, ISH_WATCH_RUNTIME_PREPARING,
            memory_order_acq_rel, memory_order_acquire))
        return _EALREADY;

    enum ish_apple_rootfs_seed_result install_result;
    int error = ish_apple_rootfs_seed_install(
            seed_root, persistent_parent, "aarch64", &install_result);
    if (error != 0)
        return runtime_fail(err_map(error));
    (void) install_result;

    char root_data[PATH_MAX];
    int root_length = snprintf(
            root_data, sizeof(root_data), "%s/aarch64/data",
            persistent_parent);
    if (root_length < 0 || (size_t) root_length >= sizeof(root_data))
        return runtime_fail(_ENAMETOOLONG);

    char *owned_socket_prefix = strdup(socket_prefix);
    if (owned_socket_prefix == NULL)
        return runtime_fail(_ENOMEM);

    error = mount_root(&fakefs, root_data);
    if (error < 0) {
        free(owned_socket_prefix);
        return runtime_fail(error);
    }

    error = become_first_process();
    if (error < 0) {
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
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }
    error = do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    if (error < 0) {
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
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    static const char login_arguments[] =
            "/bin/login\0-f\0root\0";
    static const char environment[] = "TERM=xterm-256color\0";
    error = do_execve(
            "/bin/login", 3, login_arguments, environment);
    if (error < 0) {
        free(owned_socket_prefix);
        return runtime_fail_after_task(error);
    }

    sock_tmp_prefix = owned_socket_prefix;
    expected = ISH_WATCH_RUNTIME_PREPARING;
    if (!atomic_compare_exchange_strong_explicit(
            &runtime_phase, &expected, ISH_WATCH_RUNTIME_RUNNING,
            memory_order_acq_rel, memory_order_acquire))
        return runtime_fail_after_task(_EIO);

    struct task *guest_task = current;
    task_start(guest_task);
    current = NULL;
    return 0;
}

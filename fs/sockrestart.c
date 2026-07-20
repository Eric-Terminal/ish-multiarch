#include <errno.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "fs/sockrestart.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "kernel/task.h"
#include "util/list.h"
extern const struct fd_ops socket_fdops;

static lock_t sockrestart_lock = LOCK_INITIALIZER;
static struct list listen_fds = LIST_INITIALIZER(listen_fds);

void sockrestart_begin_listen(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock);
    list_add(&listen_fds, &sock->sockrestart.listen);
    unlock(&sockrestart_lock);
}

void sockrestart_end_listen(struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock);
    list_remove_safe(&sock->sockrestart.listen);
    unlock(&sockrestart_lock);
}

static struct list listen_tasks = LIST_INITIALIZER(listen_tasks);

void sockrestart_begin_listen_wait(struct task *task, struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock);
    if (task->sockrestart.count == 0)
        list_add(&listen_tasks, &task->sockrestart.listen);
    task->sockrestart.count++;
    unlock(&sockrestart_lock);
}

void sockrestart_end_listen_wait(struct task *task, struct fd *sock) {
    if (sock->ops != &socket_fdops)
        return;
    lock(&sockrestart_lock);
    task->sockrestart.count--;
    if (task->sockrestart.count == 0)
        list_remove(&task->sockrestart.listen);
    unlock(&sockrestart_lock);
}

bool sockrestart_should_restart_listen_wait(struct task *task) {
    lock(&sockrestart_lock);
    bool punt = task->sockrestart.punt;
    task->sockrestart.punt = false;
    unlock(&sockrestart_lock);
    return punt;
}

struct saved_socket {
    struct fd *sock;
    int target_fd;
    int identity_fd;
    int type;
    int proto;
    union {
        char name[sizeof(struct sockaddr_storage)];
        struct sockaddr name_addr;
    };
    socklen_t name_len;
    dword_t backlog;
    int status_flags;
    int reuse_address;
#ifdef SO_REUSEPORT
    int reuse_port;
#endif
#ifdef IPV6_V6ONLY
    int ipv6_only;
    bool has_ipv6_only;
#endif
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int no_sigpipe;
#endif
    bool unix_backing_owned;
    uint64_t unix_backing_device;
    uint64_t unix_backing_inode;
    char unix_backing_path[
            sizeof(((struct fd *) 0)->socket.unix_backing_path)];
    struct list saved;
};

static struct list saved_sockets = LIST_INITIALIZER(saved_sockets);

enum saved_identity_result {
    saved_identity_different,
    saved_identity_same,
    saved_identity_same_restore_failed,
    saved_identity_error,
};

// 这两个入口只由主线程串行调用。挂起与恢复只在全局锁内快照注册表、
// 发布结果；socket 状态在锁外读取，以保持 fd->lock → sockrestart_lock 锁序。
// 集成方还必须让 guest/socket 生命周期静止，并保持 raw fd 只由 socket 层
// 持有；监听代次与身份复核不把本协议扩展成可并发 shutdown/listen 的事务。

static void close_saved_identity(struct saved_socket *saved) {
    if (saved->identity_fd >= 0)
        (void) close(saved->identity_fd);
    saved->identity_fd = -1;
}

static int get_status_flags(int raw_fd) {
    int flags;
    do {
        flags = fcntl(raw_fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    return flags;
}

static bool set_status_flags(int raw_fd, int flags) {
    int result;
    do {
        result = fcntl(raw_fd, F_SETFL, flags);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

static bool capture_saved_identity(
        struct saved_socket *saved, int raw_fd) {
    int identity_fd;
    do {
        identity_fd = fcntl(raw_fd, F_DUPFD_CLOEXEC, 0);
    } while (identity_fd < 0 && errno == EINTR);
    if (identity_fd < 0)
        return false;
    close_saved_identity(saved);
    saved->identity_fd = identity_fd;
    return true;
}

static enum saved_identity_result target_saved_identity(
        const struct saved_socket *saved) {
    if (saved->identity_fd < 0)
        return saved_identity_error;
    int original_flags = get_status_flags(saved->identity_fd);
    if (original_flags < 0)
        return saved_identity_error;

    // Darwin 的 socket fstat 身份并不唯一。内部 duplicate 与目标 fd
    // 共享 open-file-description 的状态标志，因此用两种 O_NONBLOCK
    // 状态做无损证明；整个恢复期由主线程串行，期间没有 guest I/O。
    int baseline_flags = original_flags & ~O_NONBLOCK;
    bool baseline_set =
            set_status_flags(saved->identity_fd, baseline_flags);
    int baseline_target = baseline_set ?
            get_status_flags(saved->target_fd) : -1;
    bool probe_set = set_status_flags(saved->identity_fd,
            baseline_flags | O_NONBLOCK);
    int probe_target = probe_set ?
            get_status_flags(saved->target_fd) : -1;
    bool restored = set_status_flags(
            saved->identity_fd, original_flags);
    int restored_target = restored ?
            get_status_flags(saved->target_fd) : -1;
    bool same = baseline_set && probe_set &&
            baseline_target >= 0 && probe_target >= 0 &&
            (baseline_target & O_NONBLOCK) == 0 &&
            (probe_target & O_NONBLOCK) != 0;
    bool restore_verified = restored && restored_target >= 0 &&
            (restored_target & O_NONBLOCK) ==
                    (original_flags & O_NONBLOCK);
    if (same)
        return restore_verified ? saved_identity_same :
                saved_identity_same_restore_failed;
    if (baseline_set && probe_set && restored &&
            baseline_target >= 0 && probe_target >= 0 &&
            restored_target >= 0)
        return saved_identity_different;
    return saved_identity_error;
}

static bool saved_unix_path_matches_name(
        const struct saved_socket *saved) {
    if (saved->name_addr.sa_family != AF_UNIX ||
            !saved->unix_backing_owned ||
            saved->unix_backing_path[0] == '\0')
        return false;
    size_t path_offset = offsetof(struct sockaddr_un, sun_path);
    if (saved->name_len <= path_offset)
        return false;
    const struct sockaddr_un *name =
            (const struct sockaddr_un *) &saved->name;
    size_t path_length = strlen(saved->unix_backing_path);
    size_t wire_length = saved->name_len - path_offset;
    if (wire_length > sizeof(name->sun_path))
        return false;
    while (wire_length != 0 && name->sun_path[wire_length - 1] == '\0')
        wire_length--;
    return wire_length == path_length &&
            memcmp(name->sun_path,
                    saved->unix_backing_path, path_length) == 0;
}

// 1 表示仍是本 socket 创建的节点，0 表示节点已消失，负值表示冲突或错误。
static int saved_unix_path_state(const struct saved_socket *saved) {
    if (!saved_unix_path_matches_name(saved))
        return -1;
    struct stat status;
    if (lstat(saved->unix_backing_path, &status) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(status.st_mode) ||
            (uint64_t) status.st_dev != saved->unix_backing_device ||
            (uint64_t) status.st_ino != saved->unix_backing_inode)
        return -1;
    return 1;
}

static void clear_unix_backing_ownership(struct fd *sock) {
    sock->socket.unix_backing_owned = false;
    sock->socket.unix_backing_device = 0;
    sock->socket.unix_backing_inode = 0;
}

static bool prepare_unix_rebind(
        struct fd *sock, const struct saved_socket *saved) {
    int path_state = saved_unix_path_state(saved);
    if (path_state < 0) {
        clear_unix_backing_ownership(sock);
        return false;
    }
    if (path_state > 0 && unlink(saved->unix_backing_path) < 0)
        return false;
    // 从这一刻起，旧节点不再由 fd 拥有；成功 bind 后再提交新身份。
    clear_unix_backing_ownership(sock);
    return true;
}

static bool capture_new_unix_path_identity(
        const struct saved_socket *saved,
        uint64_t *device, uint64_t *inode) {
    struct stat status;
    if (lstat(saved->unix_backing_path, &status) < 0 ||
            !S_ISSOCK(status.st_mode))
        return false;
    *device = (uint64_t) status.st_dev;
    *inode = (uint64_t) status.st_ino;
    return true;
}

static void remove_new_unix_path(const struct saved_socket *saved,
        uint64_t device, uint64_t inode) {
    struct stat status;
    if (lstat(saved->unix_backing_path, &status) == 0 &&
            S_ISSOCK(status.st_mode) &&
            (uint64_t) status.st_dev == device &&
            (uint64_t) status.st_ino == inode)
        (void) unlink(saved->unix_backing_path);
}

static void fail_socket_restore(struct fd *sock) {
    sock->real_fd = -1;
    sock->socket.host_listening = false;
    sock->socket.guest_listening = false;
    sock->socket.listen_generation++;
    if (sock->socket.listen_generation == 0)
        sock->socket.listen_generation++;
    sockrestart_end_listen(sock);
}

void sockrestart_on_suspend(void) {
    struct list candidates;
    struct list completed;
    list_init(&candidates);
    list_init(&completed);

    lock(&sockrestart_lock);
    assert(list_empty(&saved_sockets));
    struct fd *sock;
    list_for_each_entry(&listen_fds, sock, sockrestart.listen) {
        struct saved_socket *saved = calloc(1, sizeof(*saved));
        if (saved == NULL)
            continue; // better than a crash
        saved->identity_fd = -1;
        saved->sock = fd_try_retain(sock);
        if (saved->sock == NULL) {
            free(saved);
            continue;
        }
        list_add_tail(&candidates, &saved->saved);
    }
    unlock(&sockrestart_lock);

    struct saved_socket *saved;
    struct saved_socket *tmp;
    list_for_each_entry_safe(&candidates, saved, tmp, saved) {
        list_remove(&saved->saved);
        bool valid = false;
        bool snapshot_failed = false;
        lock(&saved->sock->lock);
        if (saved->sock->socket.host_listening) {
            saved->target_fd = saved->sock->real_fd;
            saved->proto = saved->sock->socket.protocol;
            saved->backlog = saved->sock->socket.listen_backlog;
            valid = saved->target_fd >= 0 &&
                    capture_saved_identity(saved, saved->target_fd);
            socklen_t type_size = sizeof(saved->type);
            saved->name_len = sizeof(saved->name);
            valid = valid &&
                    getsockopt(saved->identity_fd, SOL_SOCKET,
                            SO_TYPE, &saved->type, &type_size) == 0 &&
                    type_size == sizeof(saved->type) &&
                    getsockname(saved->identity_fd,
                            (struct sockaddr *) &saved->name,
                            &saved->name_len) == 0 &&
                    saved->name_len >= offsetof(
                            struct sockaddr, sa_data) &&
                    saved->name_len <= sizeof(saved->name);
            if (valid) {
                saved->status_flags =
                        get_status_flags(saved->identity_fd);
                valid = saved->status_flags >= 0;
            }
            if (valid) {
                socklen_t option_size = sizeof(saved->reuse_address);
                valid = getsockopt(saved->identity_fd,
                                SOL_SOCKET, SO_REUSEADDR,
                                &saved->reuse_address,
                                &option_size) == 0 &&
                        option_size == sizeof(saved->reuse_address);
            }
#ifdef SO_REUSEPORT
            if (valid) {
                socklen_t option_size = sizeof(saved->reuse_port);
                valid = getsockopt(saved->identity_fd,
                                SOL_SOCKET, SO_REUSEPORT,
                                &saved->reuse_port,
                                &option_size) == 0 &&
                        option_size == sizeof(saved->reuse_port);
            }
#endif
#ifdef IPV6_V6ONLY
            saved->has_ipv6_only = valid &&
                    saved->name_addr.sa_family == AF_INET6;
            if (valid && saved->has_ipv6_only) {
                socklen_t option_size = sizeof(saved->ipv6_only);
                valid = getsockopt(saved->identity_fd,
                                IPPROTO_IPV6, IPV6_V6ONLY,
                                &saved->ipv6_only,
                                &option_size) == 0 &&
                        option_size == sizeof(saved->ipv6_only);
            }
#endif
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
            if (valid) {
                socklen_t no_sigpipe_size =
                        sizeof(saved->no_sigpipe);
                valid = getsockopt(saved->identity_fd,
                                SOL_SOCKET, SO_NOSIGPIPE,
                                &saved->no_sigpipe,
                                &no_sigpipe_size) == 0 &&
                        no_sigpipe_size == sizeof(saved->no_sigpipe);
            }
#endif
            if (valid && saved->name_addr.sa_family == AF_UNIX) {
                saved->unix_backing_owned =
                        saved->sock->socket.unix_backing_owned;
                saved->unix_backing_device =
                        saved->sock->socket.unix_backing_device;
                saved->unix_backing_inode =
                        saved->sock->socket.unix_backing_inode;
                memcpy(saved->unix_backing_path,
                        saved->sock->socket.unix_backing_path,
                        sizeof(saved->unix_backing_path));
            }
            if (!valid) {
                if (saved->target_fd >= 0)
                    (void) close(saved->target_fd);
                fail_socket_restore(saved->sock);
                snapshot_failed = true;
            }
        }
        unlock(&saved->sock->lock);

        if (valid) {
            list_add_tail(&completed, &saved->saved);
        } else {
            close_saved_identity(saved);
            if (snapshot_failed)
                poll_wakeup(saved->sock, POLL_ERR | POLL_HUP);
            fd_close(saved->sock);
            free(saved);
        }
    }

    lock(&sockrestart_lock);
    while (!list_empty(&completed)) {
        saved = list_first_entry(
                &completed, struct saved_socket, saved);
        list_remove(&saved->saved);
        list_add_tail(&saved_sockets, &saved->saved);
    }
    unlock(&sockrestart_lock);
}

void sockrestart_on_resume(void) {
    struct list restoring_sockets;
    list_init(&restoring_sockets);

    lock(&sockrestart_lock);
    struct saved_socket *saved, *tmp;
    list_for_each_entry_safe(&saved_sockets, saved, tmp, saved) {
        list_remove(&saved->saved);
        list_add_tail(&restoring_sockets, &saved->saved);
    }
    unlock(&sockrestart_lock);

    list_for_each_entry_safe(&restoring_sockets, saved, tmp, saved) {
        list_remove(&saved->saved);
        bool rearm = false;
        bool wake_failed_socket = false;
        bool unix_socket = saved->name_addr.sa_family == AF_UNIX;
        bool new_unix_path = false;
        uint64_t new_unix_device = 0;
        uint64_t new_unix_inode = 0;
        uint64_t restored_generation = 0;
        bool restored_guest_listening = false;
        lock(&saved->sock->lock);
        if (!saved->sock->socket.host_listening)
            goto release_socket_lock;

        if (saved->sock->real_fd != saved->target_fd) {
            printk("恢复 socket 时 raw fd 元数据已改变，拒绝覆盖\n");
            fail_socket_restore(saved->sock);
            wake_failed_socket = true;
            goto release_socket_lock;
        }

        int target_state;
        do {
            target_state = fcntl(saved->target_fd, F_GETFD);
        } while (target_state < 0 && errno == EINTR);
        if (target_state >= 0) {
            enum saved_identity_result identity =
                    target_saved_identity(saved);
            if (identity != saved_identity_same) {
                if (identity == saved_identity_same_restore_failed) {
                    (void) close(saved->target_fd);
                    close_saved_identity(saved);
                }
                if (identity == saved_identity_different)
                    printk("恢复 socket 时目标 fd 已被占用，拒绝覆盖\n");
                else
                    printk("恢复 socket 时无法验证目标 fd 身份，失败关闭\n");
                fail_socket_restore(saved->sock);
                wake_failed_socket = true;
                goto release_socket_lock;
            }
            if (listen(saved->target_fd, (int) saved->backlog) == 0) {
                if (!unix_socket || saved_unix_path_state(saved) > 0) {
                    restored_generation =
                            saved->sock->socket.listen_generation;
                    restored_guest_listening =
                            saved->sock->socket.guest_listening;
                    rearm = true;
                    goto release_socket_lock;
                }
                // 后备路径消失时可安全关闭原 listener 并原位重建；
                // 身份冲突则保留外来节点并进入失败关闭状态。
                int path_state = saved_unix_path_state(saved);
                (void) close(saved->target_fd);
                saved->sock->real_fd = -1;
                close_saved_identity(saved);
                if (path_state < 0) {
                    clear_unix_backing_ownership(saved->sock);
                    fail_socket_restore(saved->sock);
                    wake_failed_socket = true;
                    goto release_socket_lock;
                }
            } else {
                (void) close(saved->target_fd);
                saved->sock->real_fd = -1;
                close_saved_identity(saved);
            }
        } else if (errno == EBADF) {
            saved->sock->real_fd = -1;
            close_saved_identity(saved);
        } else {
            printk("检查待恢复 socket fd 失败: %s\n", strerror(errno));
            fail_socket_restore(saved->sock);
            wake_failed_socket = true;
            goto release_socket_lock;
        }

        if (unix_socket &&
                !prepare_unix_rebind(saved->sock, saved)) {
            printk("恢复 Unix socket 时后备路径身份不匹配，拒绝覆盖\n");
            fail_socket_restore(saved->sock);
            wake_failed_socket = true;
            goto release_socket_lock;
        }

        int new_sock = socket(saved->name_addr.sa_family,
                saved->type, saved->proto);
        if (new_sock < 0) {
            printk("restarting socket(%d, %d, %d) failed: %s\n",
                    saved->name_addr.sa_family, saved->type,
                    saved->proto, strerror(errno));
            fail_socket_restore(saved->sock);
            wake_failed_socket = true;
            goto release_socket_lock;
        }
        if (!set_status_flags(new_sock, saved->status_flags)) {
            printk("恢复 socket 状态标志失败: %s\n", strerror(errno));
        } else if (setsockopt(new_sock, SOL_SOCKET, SO_REUSEADDR,
                           &saved->reuse_address,
                           sizeof(saved->reuse_address)) < 0) {
            printk("恢复 SO_REUSEADDR 失败: %s\n", strerror(errno));
#ifdef SO_REUSEPORT
        } else if (setsockopt(new_sock, SOL_SOCKET, SO_REUSEPORT,
                           &saved->reuse_port,
                           sizeof(saved->reuse_port)) < 0) {
            printk("恢复 SO_REUSEPORT 失败: %s\n", strerror(errno));
#endif
#ifdef IPV6_V6ONLY
        } else if (saved->has_ipv6_only &&
                setsockopt(new_sock, IPPROTO_IPV6, IPV6_V6ONLY,
                        &saved->ipv6_only,
                        sizeof(saved->ipv6_only)) < 0) {
            printk("恢复 IPV6_V6ONLY 失败: %s\n", strerror(errno));
#endif
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
        } else if (setsockopt(new_sock, SOL_SOCKET, SO_NOSIGPIPE,
                           &saved->no_sigpipe,
                           sizeof(saved->no_sigpipe)) < 0) {
            printk("恢复 SO_NOSIGPIPE 失败: %s\n", strerror(errno));
#endif
        } else if (bind(new_sock, (struct sockaddr *) &saved->name,
                           saved->name_len) < 0) {
            printk("rebinding socket failed: %s\n", strerror(errno));
        } else if (unix_socket &&
                !capture_new_unix_path_identity(saved,
                        &new_unix_device, &new_unix_inode)) {
            printk("记录新 Unix socket 后备路径身份失败\n");
        } else {
            new_unix_path = unix_socket;
            if (listen(new_sock, (int) saved->backlog) < 0) {
                printk("恢复监听状态失败: %s\n", strerror(errno));
            } else {
                int installed = new_sock;
                if (new_sock != saved->target_fd)
                    installed = fcntl(
                            new_sock, F_DUPFD, saved->target_fd);
                if (installed < 0) {
                    printk("安装恢复 socket 失败: %s\n",
                            strerror(errno));
                } else if (installed != saved->target_fd) {
                    printk("恢复 socket 时目标 fd 被并发占用，拒绝覆盖\n");
                    if (installed != new_sock)
                        (void) close(installed);
                } else {
                    if (!capture_saved_identity(saved, installed)) {
                        printk("保留恢复 socket 身份失败: %s\n",
                                strerror(errno));
                        if (installed != new_sock)
                            (void) close(installed);
                    } else {
                        saved->sock->real_fd = saved->target_fd;
                        restored_generation =
                                saved->sock->socket.listen_generation;
                        restored_guest_listening =
                                saved->sock->socket.guest_listening;
                        rearm = true;
                        if (unix_socket) {
                            saved->sock->socket.unix_backing_owned = true;
                            saved->sock->socket.unix_backing_device =
                                    new_unix_device;
                            saved->sock->socket.unix_backing_inode =
                                    new_unix_inode;
                            memcpy(saved->sock->socket.unix_backing_path,
                                    saved->unix_backing_path,
                                    sizeof(saved->unix_backing_path));
                        }
                    }
                }
            }
            if (new_sock == saved->target_fd && rearm)
                new_sock = -1;
        }
        if (new_sock >= 0)
            close(new_sock);
        if (!rearm) {
            if (new_unix_path)
                remove_new_unix_path(saved,
                        new_unix_device, new_unix_inode);
            fail_socket_restore(saved->sock);
            wake_failed_socket = true;
        }

release_socket_lock:
        unlock(&saved->sock->lock);
        if (rearm && poll_rearm_fd(saved->sock) < 0) {
            printk("恢复 socket 轮询登记失败\n");
            lock(&saved->sock->lock);
            bool same_restore = saved->identity_fd >= 0 &&
                    saved->sock->real_fd == saved->target_fd &&
                    saved->sock->socket.host_listening &&
                    saved->sock->socket.guest_listening ==
                            restored_guest_listening &&
                    saved->sock->socket.listen_generation ==
                            restored_generation;
            if (same_restore) {
                enum saved_identity_result identity =
                        target_saved_identity(saved);
                if (identity == saved_identity_same ||
                        identity == saved_identity_same_restore_failed)
                    (void) close(saved->target_fd);
                fail_socket_restore(saved->sock);
                wake_failed_socket = true;
            }
            unlock(&saved->sock->lock);
        }
        if (wake_failed_socket)
            poll_wakeup(saved->sock, POLL_ERR | POLL_HUP);
        close_saved_identity(saved);
        fd_close(saved->sock);
        free(saved);
    }

    lock(&sockrestart_lock);
    struct task *task;
    list_for_each_entry(&listen_tasks, task, sockrestart.listen) {
        task->sockrestart.punt = true;
        pthread_kill(task_thread_load(task), SIGUSR1);
    }
    unlock(&sockrestart_lock);
    socket_scm_collect_checkpoint();
}

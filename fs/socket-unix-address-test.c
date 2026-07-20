#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_PAGE UINT32_C(0x1000)
#define USER_LENGTH (USER_PAGE + UINT32_C(0x080))
#define USER_ADDRESS (USER_PAGE + UINT32_C(0x100))
#define USER_RECV_PAYLOAD (USER_PAGE + UINT32_C(0x200))
#define USER_RECV_IOV (USER_PAGE + UINT32_C(0x240))
#define USER_RECV_NAME (USER_PAGE + UINT32_C(0x280))
#define USER_RECV_CONTROL (USER_PAGE + UINT32_C(0x340))
#define USER_RECV_HEADER (USER_PAGE + UINT32_C(0x400))
#define USER_SEND_PAYLOAD (USER_PAGE + UINT32_C(0x500))
#define USER_SEND_IOV (USER_PAGE + UINT32_C(0x540))
#define USER_SEND_CONTROL (USER_PAGE + UINT32_C(0x580))
#define USER_SEND_HEADER (USER_PAGE + UINT32_C(0x5c0))
#define USER_SOCKETPAIR (USER_PAGE + UINT32_C(0x640))
#define USER_READONLY_HEADER UINT32_C(0x2100)
#define USER_UNMAPPED UINT32_C(0x3000)
#define DISCARD_STRESS_ITERATIONS 64
#define SHUTDOWN_RACE_ITERATIONS 64
#define CONNECT_RACE_ITERATIONS 64
#define BLOCKED_SEND_ROUNDS 8
#define BLOCKED_SEND_RECOVERY_ROUNDS 4
#define MANY_DGRAM_SENDERS 40
#define BLOCKED_SEND_BUFFER_SIZE 4096
#define MSG_CMSG_CLOEXEC_ UINT32_C(0x40000000)

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define BLOCKED_SEND_WAIT_MS 5000
#define BLOCKED_SEND_TIMEOUT_SECONDS 10
#else
#define BLOCKED_SEND_WAIT_MS 1000
#define BLOCKED_SEND_TIMEOUT_SECONDS 3
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Unix socket 地址测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
    char root_directory[PATH_MAX];
    char data_directory[PATH_MAX];
    bool mounted;
    struct {
        unsigned close_calls;
    } scm_probe;
};

struct guest_unix_address {
    struct sockaddr_storage storage;
    size_t length;
};

struct guest_rights {
    dword_t length;
    int_t level;
    int_t type;
    fd_t fd;
};

struct shutdown_send_call {
    struct fixture *fixture;
    struct socket_ref sender;
    struct scm *scm;
    atomic_bool ready;
    atomic_bool start;
    ssize_t result;
};

struct blocking_send_call {
    struct fixture *fixture;
    fd_t sender;
    atomic_bool started;
    atomic_bool finished;
    int_t result;
};

struct blocking_datagram_send_call {
    struct fixture *fixture;
    fd_t sender;
    byte_t payload;
    atomic_bool started;
    atomic_bool finished;
    ssize_t result;
};

struct datagram_connect_call {
    struct fixture *fixture;
    fd_t sender;
    const struct guest_unix_address *destination;
    atomic_bool ready;
    atomic_bool start;
    int result;
};

struct drain_recvmsg_probe {
    unsigned calls;
    bool reset_injected;
};

_Static_assert(sizeof(struct guest_rights) ==
        sizeof(struct cmsghdr_) + sizeof(fd_t),
        "i386 SCM_RIGHTS 测试线格式必须紧凑");

static bool create_fakefs_database(const char *path) {
    static const char schema[] =
            "create table meta (id integer unique default 0, db_inode integer);"
            "insert into meta (db_inode) values (0);"
            "create table stats (inode integer primary key, stat blob);"
            "create table paths (path blob primary key, "
                    "inode integer references stats(inode));"
            "create index inode_to_path on paths (inode, path);"
            "pragma user_version=3;";
    sqlite3 *database = NULL;
    int error = sqlite3_open_v2(path, &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (error == SQLITE_OK)
        error = sqlite3_exec(database, schema, NULL, NULL, NULL);
    if (database != NULL) {
        int close_error = sqlite3_close(database);
        if (error == SQLITE_OK)
            error = close_error;
    }
    return error == SQLITE_OK;
}

static bool map_user_page(struct task *task) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(task->mem, PAGE(USER_PAGE), 2, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error == 0;
}

static bool protect_user_page(
        struct task *task, addr_t address, unsigned flags) {
    write_wrlock(&task->mem->lock);
    int error = pt_set_flags(task->mem, PAGE(address), 1, flags);
    write_wrunlock(&task->mem->lock);
    return error == 0;
}

static int scm_probe_close(struct fd *fd) {
    struct fixture *fixture = fd->data;
    fixture->scm_probe.close_calls++;
    return 0;
}

static ssize_t inject_drain_reset_once(
        int fd, struct msghdr *message, int flags, void *opaque) {
    struct drain_recvmsg_probe *probe = opaque;
    probe->calls++;
    if (!probe->reset_injected) {
        probe->reset_injected = true;
        errno = ECONNRESET;
        return -1;
    }
    return recvmsg(fd, message, flags);
}

static const struct fd_ops scm_probe_ops = {
    .close = scm_probe_close,
};

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    memset(&fixture->task, 0, sizeof(fixture->task));
    memset(&fixture->group, 0, sizeof(fixture->group));
    lock_init(&fixture->group.lock);
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    lock_init(&fixture->task.waiting_cond_lock);
    list_init(&fixture->task.queue);
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {256, 256};
    fixture->task.group = &fixture->group;
    fixture->task.euid = 1000;
    fixture->task.egid = 1000;
    fixture->task.sighand = sighand_new();
    CHECK(fixture->task.sighand != NULL,
            "创建测试信号处理表");
    current = &fixture->task;
    task_thread_store(&fixture->task, pthread_self());

    strcpy(fixture->root_directory, "/tmp/ish-unix-address-XXXXXX");
    CHECK(mkdtemp(fixture->root_directory) != NULL,
            "创建隔离 fakefs 根目录");
    CHECK(snprintf(fixture->data_directory,
                    sizeof(fixture->data_directory), "%s/data",
                    fixture->root_directory) <
                    (int) sizeof(fixture->data_directory) &&
            mkdir(fixture->data_directory, 0700) == 0,
            "创建隔离 fakefs data 目录");

    char database_path[PATH_MAX];
    CHECK(snprintf(database_path, sizeof(database_path), "%s/meta.db",
                    fixture->root_directory) < (int) sizeof(database_path) &&
            create_fakefs_database(database_path),
            "创建最小 fakefs 元数据数据库");

    CHECK(mount_root(&fakefs, fixture->data_directory) == 0,
            "挂载隔离 fakefs");
    fixture->mounted = true;
    fixture->task.fs = fs_info_new();
    CHECK(fixture->task.fs != NULL, "创建测试文件系统上下文");
    fixture->task.files = fdtable_new(8);
    CHECK(!IS_ERR(fixture->task.files), "创建测试 fd 表");
    struct mm *memory = mm_new();
    CHECK(memory != NULL, "创建测试 mm");
    task_set_mm(&fixture->task, memory);
    CHECK(map_user_page(&fixture->task),
            "创建测试用户地址空间");
    return true;
}

static void remove_if_present(const char *path) {
    if (unlink(path) < 0)
        (void) errno;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL && !IS_ERR(fixture->task.files))
        fdtable_release(fixture->task.files);
    if (fixture->task.mm != NULL)
        mm_release(fixture->task.mm);
    if (fixture->task.fs != NULL)
        fs_info_release(fixture->task.fs);
    if (fixture->task.sighand != NULL)
        sighand_release(fixture->task.sighand);
    if (fixture->mounted) {
        lock(&mounts_lock);
        (void) do_umount("");
        unlock(&mounts_lock);
    }

    char path[PATH_MAX];
    static const char *const data_entries[] = {
        "no-terminator", "cut", "path-source",
    };
    for (size_t index = 0;
            index < sizeof(data_entries) / sizeof(data_entries[0]); index++) {
        if (snprintf(path, sizeof(path), "%s/%s", fixture->data_directory,
                    data_entries[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    static const char *const database_suffixes[] = {
        "meta.db", "meta.db-wal", "meta.db-shm",
    };
    for (size_t index = 0;
            index < sizeof(database_suffixes) /
                    sizeof(database_suffixes[0]); index++) {
        if (snprintf(path, sizeof(path), "%s/%s", fixture->root_directory,
                    database_suffixes[index]) < (int) sizeof(path))
            remove_if_present(path);
    }
    (void) rmdir(fixture->data_directory);
    (void) rmdir(fixture->root_directory);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static struct guest_unix_address unix_address(
        const void *name, size_t name_length) {
    struct guest_unix_address address = {0};
    struct sockaddr_ *wire = (struct sockaddr_ *) &address.storage;
    wire->family = AF_LOCAL_;
    assert(name_length <= SOCKADDR_DATA_MAX);
    memcpy((byte_t *) &address.storage +
            offsetof(struct sockaddr_, data), name, name_length);
    address.length = offsetof(struct sockaddr_, data) + name_length;
    return address;
}

static fd_t create_unix_socket(struct fixture *fixture, int type) {
    current = &fixture->task;
    return socket_create_task(&fixture->task, AF_LOCAL_, type, 0);
}

static int bind_unix_socket(struct fixture *fixture, fd_t number,
        const struct guest_unix_address *address) {
    struct socket_ref socket;
    int error = socket_ref_get_task(&fixture->task, number, &socket);
    if (error < 0)
        return error;
    error = socket_bind_ref_task(&fixture->task, &socket,
            &address->storage, address->length);
    socket_ref_release(&socket);
    return error;
}

static int connect_unix_socket(struct fixture *fixture, fd_t number,
        const struct guest_unix_address *address) {
    struct socket_ref socket;
    int error = socket_ref_get_task(&fixture->task, number, &socket);
    if (error < 0)
        return error;
    error = socket_connect_ref_task(&fixture->task, &socket,
            &address->storage, address->length);
    socket_ref_release(&socket);
    return error;
}

static bool read_peer_credentials(const struct socket_ref *socket,
        struct ucred_ *credentials) {
    struct socket_option_result result;
    if (socket_getsockopt_ref(socket,
            SOL_SOCKET_, SO_PEERCRED_, sizeof(*credentials),
            SOCKET_GUEST_I386, &result) < 0 ||
            result.length != sizeof(*credentials))
        return false;
    memcpy(credentials, result.value, sizeof(*credentials));
    return true;
}

static bool read_socket_error(
        const struct socket_ref *socket, sdword_t *error) {
    struct socket_option_result result;
    if (socket_getsockopt_ref(socket,
            SOL_SOCKET_, SO_ERROR_, sizeof(*error),
            SOCKET_GUEST_I386, &result) < 0 ||
            result.length != sizeof(*error))
        return false;
    memcpy(error, result.value, sizeof(*error));
    return true;
}

static ssize_t send_unix_datagram(struct fixture *fixture, fd_t sender,
        const struct guest_unix_address *destination,
        const void *payload, size_t payload_length) {
    struct socket_ref socket;
    int error = socket_ref_get_task(&fixture->task, sender, &socket);
    if (error < 0)
        return error;
    struct socket_address prepared;
    error = socket_address_prepare_task(&fixture->task, &socket,
            &destination->storage, destination->length, &prepared);
    if (error < 0) {
        socket_ref_release(&socket);
        return error;
    }
    ssize_t result = socket_sendto_ref(
            &socket, payload, payload_length, 0, &prepared);
    socket_address_release(&prepared);
    socket_ref_release(&socket);
    return result;
}

static ssize_t send_connected_unix_datagram(
        struct fixture *fixture, fd_t sender,
        const void *payload, size_t payload_length, dword_t flags) {
    struct socket_ref socket;
    int error = socket_ref_get_task(&fixture->task, sender, &socket);
    if (error < 0)
        return error;
    ssize_t result = socket_sendto_ref(
            &socket, payload, payload_length, flags, NULL);
    socket_ref_release(&socket);
    return result;
}

static ssize_t receive_unix_datagram(struct fixture *fixture,
        fd_t receiver, dword_t flags, void *payload, size_t payload_length,
        struct socket_address *source) {
    struct socket_ref socket;
    int error = socket_ref_get_task(&fixture->task, receiver, &socket);
    if (error < 0)
        return error;
    ssize_t result = socket_recvfrom_ref(
            &socket, payload, payload_length, flags, source);
    socket_ref_release(&socket);
    return result;
}

static bool source_equals(const struct socket_address *source,
        const void *name, size_t name_length) {
    if (source->length != offsetof(struct sockaddr_, data) + name_length)
        return false;
    const struct sockaddr_ *wire =
            (const struct sockaddr_ *) &source->storage;
    return wire->family == AF_LOCAL_ &&
            memcmp((const byte_t *) &source->storage +
                    offsetof(struct sockaddr_, data),
                    name, name_length) == 0;
}

static bool guest_getsockname(struct fixture *fixture, fd_t socket,
        struct sockaddr_storage *address, dword_t *length) {
    current = &fixture->task;
    dword_t capacity = sizeof(*address);
    memset(address, 0xa5, sizeof(*address));
    if (user_write(USER_LENGTH, &capacity, sizeof(capacity)) != 0 ||
            user_write(USER_ADDRESS, address, sizeof(*address)) != 0 ||
            sys_getsockname(socket, USER_ADDRESS, USER_LENGTH) != 0 ||
            user_read(USER_LENGTH, length, sizeof(*length)) != 0 ||
            user_read(USER_ADDRESS, address, sizeof(*address)) != 0)
        return false;
    return true;
}

static bool host_backing_path(struct fixture *fixture, fd_t number,
        char path[static sizeof(((struct sockaddr_un *) 0)->sun_path)]) {
    struct fd *socket = f_get_task(&fixture->task, number);
    if (socket == NULL)
        return false;
    struct sockaddr_un address = {0};
    socklen_t length = sizeof(address);
    if (getsockname(socket->real_fd,
            (struct sockaddr *) &address, &length) < 0)
        return false;
    size_t path_size = strnlen(address.sun_path, sizeof(address.sun_path));
    if (path_size == 0 || path_size == sizeof(address.sun_path))
        return false;
    memcpy(path, address.sun_path, path_size + 1);
    return true;
}

static bool host_unix_address(const char *path,
        struct sockaddr_un *address, socklen_t *length) {
    size_t path_length = strlen(path);
    if (path_length >= sizeof(address->sun_path))
        return false;
    *address = (struct sockaddr_un) {0};
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, path_length + 1);
    *length = (socklen_t) (offsetof(struct sockaddr_un, sun_path) +
            path_length + 1);
#ifdef __APPLE__
    address->sun_len = (uint8_t) *length;
#endif
    return true;
}

static bool source_is_unnamed(const struct socket_address *source) {
    return source->length == offsetof(struct sockaddr_, data) &&
            ((const struct sockaddr_ *) &source->storage)->family ==
                    AF_LOCAL_;
}

static bool probe_reverse_name_released(struct fixture *fixture,
        fd_t receiver, const char *receiver_backing,
        const char *former_sender_backing) {
    struct sockaddr_un source_address;
    struct sockaddr_un destination_address;
    socklen_t source_length;
    socklen_t destination_length;
    if (!host_unix_address(former_sender_backing,
                    &source_address, &source_length) ||
            !host_unix_address(receiver_backing,
                    &destination_address, &destination_length))
        return false;

    int probe = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (probe < 0)
        return false;
    bool passed = false;
    remove_if_present(former_sender_backing);
    if (bind(probe, (const struct sockaddr *) &source_address,
                    source_length) < 0)
        goto out;
    const byte_t payload = 0xa7;
    if (sendto(probe, &payload, sizeof(payload), 0,
                    (const struct sockaddr *) &destination_address,
                    destination_length) != sizeof(payload))
        goto out;

    byte_t received = 0;
    struct socket_address source;
    passed = receive_unix_datagram(fixture, receiver, 0,
                    &received, sizeof(received), &source) ==
                    sizeof(received) &&
            received == payload && source_is_unnamed(&source);
out:
    close(probe);
    remove_if_present(former_sender_backing);
    return passed;
}

static bool prepare_recvmsg(addr_t header_address,
        addr_t payload_address, addr_t name_address,
        addr_t control_address, uint_t control_length) {
    const struct iovec_ iov = {
        .base = payload_address,
        .len = 3,
    };
    const struct msghdr_ header = {
        .msg_name = name_address,
        .msg_namelen = sizeof(struct sockaddr_storage),
        .msg_iov = USER_RECV_IOV,
        .msg_iovlen = 1,
        .msg_control = control_address,
        .msg_controllen = control_length,
    };
    return user_write(USER_RECV_IOV, &iov, sizeof(iov)) == 0 &&
            user_write(header_address, &header, sizeof(header)) == 0;
}

static bool guest_name_equals(addr_t address, dword_t length,
        const void *name, size_t name_length) {
    struct sockaddr_storage storage = {0};
    return length == offsetof(struct sockaddr_, data) + name_length &&
            user_read(address, &storage, sizeof(storage)) == 0 &&
            ((const struct sockaddr_ *) &storage)->family == AF_LOCAL_ &&
            memcmp((const byte_t *) &storage +
                    offsetof(struct sockaddr_, data),
                    name, name_length) == 0;
}

static bool prepare_sendmsg_rights(fd_t passed_number) {
    static const byte_t payload[3] = {0x53, 0x43, 0x4d};
    const struct iovec_ iov = {
        .base = USER_SEND_PAYLOAD,
        .len = sizeof(payload),
    };
    const struct guest_rights rights = {
        .length = sizeof(rights),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
        .fd = passed_number,
    };
    const struct msghdr_ header = {
        .msg_iov = USER_SEND_IOV,
        .msg_iovlen = 1,
        .msg_control = USER_SEND_CONTROL,
        .msg_controllen = sizeof(rights),
    };
    return user_write(USER_SEND_PAYLOAD,
                    payload, sizeof(payload)) == 0 &&
            user_write(USER_SEND_IOV, &iov, sizeof(iov)) == 0 &&
            user_write(USER_SEND_CONTROL,
                    &rights, sizeof(rights)) == 0 &&
            user_write(USER_SEND_HEADER,
                    &header, sizeof(header)) == 0;
}

static bool prepare_sendmsg_rights_to(fd_t passed_number,
        const struct guest_unix_address *destination) {
    static const byte_t payload[3] = {0x53, 0x43, 0x4d};
    const struct iovec_ iov = {
        .base = USER_SEND_PAYLOAD,
        .len = sizeof(payload),
    };
    const struct guest_rights rights = {
        .length = sizeof(rights),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
        .fd = passed_number,
    };
    const struct msghdr_ header = {
        .msg_name = USER_ADDRESS,
        .msg_namelen = (dword_t) destination->length,
        .msg_iov = USER_SEND_IOV,
        .msg_iovlen = 1,
        .msg_control = USER_SEND_CONTROL,
        .msg_controllen = sizeof(rights),
    };
    return user_write(USER_ADDRESS, &destination->storage,
                    destination->length) == 0 &&
            user_write(USER_SEND_PAYLOAD,
                    payload, sizeof(payload)) == 0 &&
            user_write(USER_SEND_IOV, &iov, sizeof(iov)) == 0 &&
            user_write(USER_SEND_CONTROL,
                    &rights, sizeof(rights)) == 0 &&
            user_write(USER_SEND_HEADER,
                    &header, sizeof(header)) == 0;
}

static bool close_socket(struct fixture *fixture, fd_t *number) {
    if (*number < 0)
        return true;
    int error = f_close_task(&fixture->task, *number);
    *number = -1;
    return error == 0;
}

static int open_host_fds(void) {
    int count = 0;
    int limit = getdtablesize();
    for (int number = 0; number < limit; number++) {
        errno = 0;
        if (fcntl(number, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
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

static bool poll_registration_matches(struct task *task, bool active) {
    lock(&task->waiting_cond_lock);
    bool matches = active ?
            task->waiting_poll_active &&
                    task->waiting_poll_notify_fd >= 0 :
            !task->waiting_poll_active &&
                    task->waiting_poll_notify_fd == -1;
    unlock(&task->waiting_cond_lock);
    return matches;
}

static bool wait_for_poll_registration(
        struct task *task, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (poll_registration_matches(task, true))
            return true;
        nanosleep(&interval, NULL);
    }
    return false;
}

static bool fill_host_datagram_queue(
        int socket, byte_t filler, unsigned *message_count,
        int *terminal_error) {
    *message_count = 0;
    *terminal_error = 0;
    for (unsigned attempt = 0; attempt < 65536; attempt++) {
        ssize_t sent = send(socket, &filler, sizeof(filler), MSG_DONTWAIT);
        if (sent == (ssize_t) sizeof(filler)) {
            (*message_count)++;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            attempt--;
            continue;
        }
        if (sent < 0)
            *terminal_error = errno;
        return *message_count != 0 && sent < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ENOBUFS);
    }
    return false;
}

static void *send_during_shutdown(void *opaque) {
    struct shutdown_send_call *call = opaque;
    current = &call->fixture->task;
    atomic_store_explicit(&call->ready, true, memory_order_release);
    while (!atomic_load_explicit(&call->start, memory_order_acquire))
        sched_yield();
    const byte_t payload = 0x73;
    call->result = socket_sendmsg_ref(&call->sender,
            &payload, sizeof(payload), MSG_NOSIGNAL_, NULL, &call->scm);
    if (call->scm != NULL) {
        socket_scm_release(call->scm);
        call->scm = NULL;
    }
    current = NULL;
    return NULL;
}

static void *sendmsg_while_blocked(void *opaque) {
    struct blocking_send_call *call = opaque;
    current = &call->fixture->task;
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = sys_sendmsg(
            call->sender, USER_SEND_HEADER, MSG_NOSIGNAL_);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *send_datagram_while_blocked(void *opaque) {
    struct blocking_datagram_send_call *call = opaque;
    current = &call->fixture->task;
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = send_connected_unix_datagram(
            call->fixture, call->sender,
            &call->payload, sizeof(call->payload), MSG_NOSIGNAL_);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *connect_datagram_concurrently(void *opaque) {
    struct datagram_connect_call *call = opaque;
    current = &call->fixture->task;
    atomic_store_explicit(&call->ready, true, memory_order_release);
    while (!atomic_load_explicit(&call->start, memory_order_acquire))
        sched_yield();
    call->result = connect_unix_socket(
            call->fixture, call->sender, call->destination);
    current = NULL;
    return NULL;
}

static bool test_abstract_type_namespaces(struct fixture *fixture) {
    static const byte_t name[] = {
        0, 's', 'a', 'm', 'e', 0, 0xff,
    };
    struct guest_unix_address address = unix_address(name, sizeof(name));
    fd_t stream = create_unix_socket(fixture, SOCK_STREAM_);
    fd_t datagram = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t second_stream = create_unix_socket(fixture, SOCK_STREAM_);
    fd_t second_datagram = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(stream >= 0 && datagram >= 0 &&
            second_stream >= 0 && second_datagram >= 0,
            "创建 abstract 类型隔离 socket");
    CHECK(bind_unix_socket(fixture, stream, &address) == 0 &&
            bind_unix_socket(fixture, datagram, &address) == 0,
            "相同 abstract 字节可同时绑定 STREAM 与 DGRAM");
    CHECK(bind_unix_socket(fixture, second_stream, &address) ==
                    _EADDRINUSE &&
            bind_unix_socket(fixture, second_datagram, &address) ==
                    _EADDRINUSE,
            "相同类型仍共享同一 abstract 名称空间");
    CHECK(close_socket(fixture, &second_datagram) &&
            close_socket(fixture, &second_stream) &&
            close_socket(fixture, &datagram) &&
            close_socket(fixture, &stream),
            "清理 abstract 类型隔离 socket");
    return true;
}

static bool test_pathname_getsockname(struct fixture *fixture) {
    static const char unterminated[] = "/no-terminator";
    static const char terminated[] = "/no-terminator\0";
    struct guest_unix_address address =
            unix_address(unterminated, sizeof(unterminated) - 1);
    fd_t socket = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(socket >= 0 && bind_unix_socket(fixture, socket, &address) == 0,
            "绑定未携带终止 NUL 的 pathname");

    struct sockaddr_storage returned;
    dword_t returned_length = 0;
    CHECK(guest_getsockname(
                    fixture, socket, &returned, &returned_length) &&
            returned_length == offsetof(struct sockaddr_, data) +
                    sizeof(terminated) - 1 &&
            ((struct sockaddr_ *) &returned)->family == AF_LOCAL_ &&
            memcmp((byte_t *) &returned + offsetof(struct sockaddr_, data),
                    terminated, sizeof(terminated) - 1) == 0,
            "getsockname 为 pathname 补回一个终止 NUL");
    CHECK(close_socket(fixture, &socket) &&
            file_unlinkat_task(&fixture->task, AT_FDCWD_,
                    unterminated, false) == 0,
            "清理未终止 pathname socket");

    static const byte_t embedded[] = {
        '/', 'c', 'u', 't', 0, 't', 'a', 'i', 'l',
    };
    static const byte_t expected[] = {'/', 'c', 'u', 't', 0};
    address = unix_address(embedded, sizeof(embedded));
    socket = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(socket >= 0 && bind_unix_socket(fixture, socket, &address) == 0,
            "绑定含 embedded NUL 与尾随字节的 pathname");
    CHECK(guest_getsockname(
                    fixture, socket, &returned, &returned_length) &&
            returned_length == offsetof(struct sockaddr_, data) +
                    sizeof(expected) &&
            memcmp((byte_t *) &returned + offsetof(struct sockaddr_, data),
                    expected, sizeof(expected)) == 0,
            "pathname 在 embedded NUL 截断并忽略尾随字节");
    CHECK(close_socket(fixture, &socket) &&
            file_unlinkat_task(&fixture->task, AT_FDCWD_,
                    "/cut", false) == 0,
            "清理 embedded NUL pathname socket");
    return true;
}

static bool test_datagram_path_source(struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'p', 'a', 't', 'h', '-', 'r', 'x',
    };
    static const char source_path[] = "/path-source";
    static const char expected_source[] = "/path-source\0";
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address source_address =
            unix_address(source_path, sizeof(source_path) - 1);
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &source_address) == 0,
            "绑定 pathname 数据报发送端与 abstract 接收端");

    static const byte_t payload[] = {0x00, 0x41, 0xff};
    CHECK(send_unix_datagram(fixture, sender, &receiver_address,
                    payload, sizeof(payload)) == sizeof(payload),
            "pathname 数据报发送成功");
    byte_t received[sizeof(payload)] = {0};
    struct socket_address source;
    CHECK(receive_unix_datagram(fixture, receiver, 0,
                    received, sizeof(received), &source) ==
                    sizeof(received) &&
            memcmp(received, payload, sizeof(payload)) == 0 &&
            source_equals(&source,
                    expected_source, sizeof(expected_source) - 1),
            "recvfrom 精确回报 pathname 数据报源地址");
    CHECK(close_socket(fixture, &sender) &&
            close_socket(fixture, &receiver) &&
            file_unlinkat_task(&fixture->task, AT_FDCWD_,
                    source_path, false) == 0,
            "清理 pathname 数据报 socket");
    return true;
}

static bool test_closed_binary_source_and_peek(struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'b', 'i', 'n', '-', 'r', 'x',
    };
    static const byte_t sender_name[] = {
        0, 0x41, 0x00, 0xff, 0x42,
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0,
            "绑定二进制 abstract 数据报两端");

    char sender_backing[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    CHECK(host_backing_path(fixture, sender, sender_backing),
            "取得二进制 abstract 发送端 host 后备路径");
    static const byte_t payload[] = {0x5a, 0x00, 0xc3};
    CHECK(send_unix_datagram(fixture, sender, &receiver_address,
                    payload, sizeof(payload)) == sizeof(payload) &&
            close_socket(fixture, &sender),
            "sendto 成功后立即关闭二进制 abstract 源 fd");
    errno = 0;
    CHECK(lstat(sender_backing, &(struct stat) {0}) == -1 &&
            errno == ENOENT,
            "源 fd 关闭后内部 host 后备节点已删除");

    byte_t received[sizeof(payload)] = {0};
    struct socket_address peek_source;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_PEEK_,
                    received, sizeof(received), &peek_source) ==
                    sizeof(received) &&
            memcmp(received, payload, sizeof(payload)) == 0 &&
            source_equals(&peek_source, sender_name, sizeof(sender_name)),
            "MSG_PEEK 在源 fd 关闭后仍精确回报二进制 abstract 名称");

    memset(received, 0, sizeof(received));
    struct socket_address consumed_source;
    CHECK(receive_unix_datagram(fixture, receiver, 0,
                    received, sizeof(received), &consumed_source) ==
                    sizeof(received) &&
            memcmp(received, payload, sizeof(payload)) == 0 &&
            source_equals(&consumed_source,
                    sender_name, sizeof(sender_name)),
            "MSG_PEEK 未提前释放名称，正式消费仍回报原名");

    fd_t rebound = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(rebound >= 0 &&
            bind_unix_socket(fixture, rebound, &sender_address) == 0,
            "已关闭发送端的二进制 abstract 名称可立即重绑");
    CHECK(close_socket(fixture, &rebound) &&
            close_socket(fixture, &receiver),
            "清理二进制 abstract 数据报 socket");
    return true;
}

enum recvmsg_fault_stage {
    RECVMSG_PAYLOAD_FAULT,
    RECVMSG_NAME_FAULT,
    RECVMSG_HEADER_FAULT,
};

static bool run_recvmsg_fault_case(struct fixture *fixture,
        enum recvmsg_fault_stage stage) {
    byte_t receiver_name[] = {
        0, 'm', 's', 'g', '-', 'r', (byte_t) ('0' + stage),
    };
    byte_t sender_name[] = {
        0, 'm', 's', 'g', '-', 's', (byte_t) ('0' + stage),
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0,
            "创建 recvmsg EFAULT 数据报两端");

    char receiver_backing[sizeof(
            ((struct sockaddr_un *) 0)->sun_path)];
    char sender_backing[sizeof(
            ((struct sockaddr_un *) 0)->sun_path)];
    CHECK(host_backing_path(fixture, receiver, receiver_backing) &&
            host_backing_path(fixture, sender, sender_backing),
            "取得 recvmsg EFAULT 两端 host 后备路径");
    static const byte_t payload[3] = {0xe1, 0xfa, 0x17};
    CHECK(send_unix_datagram(fixture, sender, &receiver_address,
                    payload, sizeof(payload)) == sizeof(payload),
            "为 recvmsg EFAULT 排入数据报");

    byte_t cleared_payload[sizeof(payload)] = {0};
    struct sockaddr_storage cleared_name = {0};
    CHECK(user_write(USER_RECV_PAYLOAD,
                    cleared_payload, sizeof(cleared_payload)) == 0 &&
            user_write(USER_RECV_NAME,
                    &cleared_name, sizeof(cleared_name)) == 0,
            "清空 recvmsg EFAULT 输出缓冲区");

    addr_t header_address = stage == RECVMSG_HEADER_FAULT ?
            USER_READONLY_HEADER : USER_RECV_HEADER;
    addr_t payload_address = stage == RECVMSG_PAYLOAD_FAULT ?
            USER_UNMAPPED : USER_RECV_PAYLOAD;
    addr_t name_address = stage == RECVMSG_NAME_FAULT ?
            USER_UNMAPPED : USER_RECV_NAME;
    CHECK(prepare_recvmsg(header_address,
                    payload_address, name_address, 0, 0),
            "写入 recvmsg EFAULT iovec 与 msghdr");

    if (stage == RECVMSG_HEADER_FAULT)
        CHECK(protect_user_page(&fixture->task,
                        USER_READONLY_HEADER, P_READ),
                "把最终 msghdr 所在页降为只读");
    int_t result = sys_recvmsg(receiver, header_address, 0);
    bool restored = true;
    if (stage == RECVMSG_HEADER_FAULT)
        restored = protect_user_page(&fixture->task,
                USER_READONLY_HEADER, P_RWX);
    CHECK(restored && result == _EFAULT,
            "recvmsg 在指定的消费后写回阶段返回 EFAULT");

    if (stage != RECVMSG_PAYLOAD_FAULT) {
        byte_t returned_payload[sizeof(payload)] = {0};
        CHECK(user_read(USER_RECV_PAYLOAD,
                        returned_payload, sizeof(returned_payload)) == 0 &&
                memcmp(returned_payload, payload, sizeof(payload)) == 0,
                "后置 EFAULT 前 payload 已按 Linux 顺序写回");
    }
    if (stage == RECVMSG_HEADER_FAULT)
        CHECK(guest_name_equals(USER_RECV_NAME,
                        offsetof(struct sockaddr_, data) +
                                sizeof(sender_name),
                        sender_name, sizeof(sender_name)),
                "最终 msghdr EFAULT 前源名称已经写回");

    byte_t unexpected = 0;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &unexpected, sizeof(unexpected), NULL) == _EAGAIN,
            "recvmsg 写回 EFAULT 后原数据报已经消费");
    CHECK(close_socket(fixture, &sender),
            "关闭 recvmsg EFAULT 的具名发送端");
    CHECK(probe_reverse_name_released(fixture, receiver,
                    receiver_backing, sender_backing),
            "recvmsg 写回 EFAULT 恰好释放原报文的反向名称计数");
    CHECK(close_socket(fixture, &receiver),
            "清理 recvmsg EFAULT 接收端");
    return true;
}

static bool test_recvmsg_fault_name_lifetime(struct fixture *fixture) {
    CHECK(run_recvmsg_fault_case(fixture, RECVMSG_PAYLOAD_FAULT),
            "payload EFAULT 不遗留反向名称计数");
    CHECK(run_recvmsg_fault_case(fixture, RECVMSG_NAME_FAULT),
            "name EFAULT 不遗留反向名称计数");
    CHECK(run_recvmsg_fault_case(fixture, RECVMSG_HEADER_FAULT),
            "最终 msghdr EFAULT 不遗留反向名称计数");
    return true;
}

static bool test_recvmsg_peek_name_lifetime(struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'm', 's', 'g', '-', 'p', 'r',
    };
    static const byte_t sender_name[] = {
        0, 'm', 's', 'g', '-', 'p', 's', 0, 0xf1,
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0,
            "创建 recvmsg MSG_PEEK 数据报两端");

    char receiver_backing[sizeof(
            ((struct sockaddr_un *) 0)->sun_path)];
    char sender_backing[sizeof(
            ((struct sockaddr_un *) 0)->sun_path)];
    CHECK(host_backing_path(fixture, receiver, receiver_backing) &&
            host_backing_path(fixture, sender, sender_backing),
            "取得 recvmsg MSG_PEEK 两端 host 后备路径");
    static const byte_t payload[3] = {0x70, 0x65, 0x65};
    CHECK(send_unix_datagram(fixture, sender, &receiver_address,
                    payload, sizeof(payload)) == sizeof(payload),
            "为 recvmsg MSG_PEEK 排入数据报");
    CHECK(prepare_recvmsg(USER_RECV_HEADER,
                    USER_RECV_PAYLOAD, USER_RECV_NAME, 0, 0) &&
            sys_recvmsg(receiver, USER_RECV_HEADER, MSG_PEEK_) ==
                    sizeof(payload),
            "recvmsg MSG_PEEK 成功读取但不消费数据报");

    struct msghdr_ returned_header;
    byte_t returned_payload[sizeof(payload)] = {0};
    CHECK(user_read(USER_RECV_HEADER,
                    &returned_header, sizeof(returned_header)) == 0 &&
            user_read(USER_RECV_PAYLOAD,
                    returned_payload, sizeof(returned_payload)) == 0 &&
            memcmp(returned_payload, payload, sizeof(payload)) == 0 &&
            guest_name_equals(USER_RECV_NAME,
                    returned_header.msg_namelen,
                    sender_name, sizeof(sender_name)),
            "MSG_PEEK 首次返回完整 payload 与二进制源名称");
    CHECK(close_socket(fixture, &sender),
            "MSG_PEEK 后关闭具名发送端");

    memset(returned_payload, 0, sizeof(returned_payload));
    CHECK(user_write(USER_RECV_PAYLOAD,
                    returned_payload, sizeof(returned_payload)) == 0 &&
            prepare_recvmsg(USER_RECV_HEADER,
                    USER_RECV_PAYLOAD, USER_RECV_NAME, 0, 0) &&
            sys_recvmsg(receiver, USER_RECV_HEADER, 0) ==
                    sizeof(payload) &&
            user_read(USER_RECV_HEADER,
                    &returned_header, sizeof(returned_header)) == 0 &&
            user_read(USER_RECV_PAYLOAD,
                    returned_payload, sizeof(returned_payload)) == 0 &&
            memcmp(returned_payload, payload, sizeof(payload)) == 0 &&
            guest_name_equals(USER_RECV_NAME,
                    returned_header.msg_namelen,
                    sender_name, sizeof(sender_name)),
            "MSG_PEEK 未释放名称，正式 recv 仍返回同一二进制源名");

    byte_t unexpected = 0;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &unexpected, sizeof(unexpected), NULL) == _EAGAIN,
            "正式 recv 后原数据报仅消费一次");
    CHECK(probe_reverse_name_released(fixture, receiver,
                    receiver_backing, sender_backing),
            "正式 recv 恰好释放 MSG_PEEK 保留的名称计数");
    CHECK(close_socket(fixture, &receiver),
            "清理 recvmsg MSG_PEEK 接收端");
    return true;
}

static bool test_recvmsg_scm_container_lifetime(struct fixture *fixture) {
    fd_t pair[2] = {-1, -1};
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0 &&
            pair[0] >= 0 && pair[1] >= 0 && pair[0] != pair[1],
            "创建 i386 AF_UNIX DGRAM socketpair");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 SCM_RIGHTS 引用探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(
            &fixture->task, passed, 0);
    CHECK(passed_number >= 0,
            "安装 SCM_RIGHTS 引用探针描述符");

    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == 3,
            "AF_UNIX DGRAM sendmsg 成功排入 SCM_RIGHTS 容器");
    struct fd *receiver = f_get_task(&fixture->task, pair[1]);
    CHECK(receiver != NULL, "取得 SCM_RIGHTS 接收 socket");
    lock(&receiver->lock);
    bool queued_once = !list_empty(&receiver->socket.unix_scm) &&
            receiver->socket.unix_scm.next ==
                    receiver->socket.unix_scm.prev;
    unlock(&receiver->lock);
    CHECK(queued_once && atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "发送后仅有一个 SCM 容器和一份传递引用");

    struct guest_rights peek_cleared = {0};
    CHECK(user_write(USER_RECV_CONTROL,
                    &peek_cleared, sizeof(peek_cleared)) == 0 &&
            prepare_recvmsg(USER_RECV_HEADER,
                    USER_RECV_PAYLOAD, 0,
                    USER_RECV_CONTROL, sizeof(peek_cleared)) &&
            sys_recvmsg(pair[1], USER_RECV_HEADER,
                    MSG_PEEK_ | MSG_CMSG_CLOEXEC_) == 3,
            "i386 MSG_PEEK 克隆并交付 SCM_RIGHTS");
    struct guest_rights peek_rights = {0};
    struct msghdr_ peek_header = {0};
    CHECK(user_read(USER_RECV_CONTROL,
                    &peek_rights, sizeof(peek_rights)) == 0 &&
            user_read(USER_RECV_HEADER,
                    &peek_header, sizeof(peek_header)) == 0 &&
            peek_rights.length == sizeof(peek_rights) &&
            peek_rights.level == SOL_SOCKET_ &&
            peek_rights.type == SCM_RIGHTS_ &&
            peek_rights.fd >= 0 &&
            f_get_task(&fixture->task, peek_rights.fd) == passed &&
            f_getfd_task(&fixture->task, peek_rights.fd) == FD_CLOEXEC_ &&
            peek_header.msg_flags == MSG_CMSG_CLOEXEC_ &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 3,
            "MSG_PEEK 返回独立 fd 且不回显输入标志");
    lock(&receiver->lock);
    bool queued_after_peek = !list_empty(&receiver->socket.unix_scm);
    unlock(&receiver->lock);
    CHECK(queued_after_peek &&
            f_close_task(&fixture->task, peek_rights.fd) == 0 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "关闭 PEEK fd 后原 SCM 队列仍供正式接收");

    struct guest_rights cleared_rights = {0};
    CHECK(user_write(USER_RECV_CONTROL,
                    &cleared_rights, sizeof(cleared_rights)) == 0 &&
            prepare_recvmsg(USER_RECV_HEADER,
                    USER_RECV_PAYLOAD, 0,
                    USER_RECV_CONTROL, sizeof(cleared_rights)) &&
            sys_recvmsg(pair[1], USER_RECV_HEADER, 0) == 3,
            "AF_UNIX DGRAM recvmsg 成功接收 SCM_RIGHTS");

    struct guest_rights received_rights = {0};
    struct msghdr_ received_header = {0};
    byte_t received_payload[3] = {0};
    CHECK(user_read(USER_RECV_CONTROL,
                    &received_rights, sizeof(received_rights)) == 0 &&
            user_read(USER_RECV_HEADER,
                    &received_header, sizeof(received_header)) == 0 &&
            user_read(USER_RECV_PAYLOAD,
                    received_payload, sizeof(received_payload)) == 0 &&
            memcmp(received_payload, "SCM", sizeof(received_payload)) == 0 &&
            received_rights.length == sizeof(received_rights) &&
            received_rights.level == SOL_SOCKET_ &&
            received_rights.type == SCM_RIGHTS_ &&
            received_header.msg_controllen == sizeof(received_rights) &&
            received_rights.fd >= 0 &&
            f_get_task(&fixture->task, received_rights.fd) == passed,
            "recvmsg 返回 payload 与同一 SCM_RIGHTS 对象");

    lock(&receiver->lock);
    bool queues_empty = list_empty(&receiver->socket.unix_scm) &&
            list_empty(&receiver->socket.unix_pending_scm);
    unlock(&receiver->lock);
    CHECK(queues_empty && atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "成功接收后 SCM 容器出队且仅保留两个 fd 表引用");
    CHECK(f_close_task(&fixture->task, received_rights.fd) == 0 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "关闭接收描述符只释放传递引用");
    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "关闭原描述符后 SCM 对象恰好析构一次");
    CHECK(close_socket(fixture, &pair[1]) &&
            close_socket(fixture, &pair[0]),
            "清理 SCM_RIGHTS 数据报 socketpair");
    return true;
}

static bool test_sendmsg_input_boundaries(struct fixture *fixture) {
    fd_t pair[2] = {-1, -1};
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 sendmsg 边界测试 socketpair");

    static const byte_t payload[3] = {0x62, 0x6e, 0x64};
    const struct iovec_ ordinary_iov = {
        .base = USER_SEND_PAYLOAD,
        .len = sizeof(payload),
    };
    const struct cmsghdr_ short_control = {
        .len = sizeof(struct cmsghdr_) - 1,
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    struct msghdr_ header = {
        .msg_iov = USER_SEND_IOV,
        .msg_iovlen = 1,
        .msg_control = USER_SEND_CONTROL,
        .msg_controllen = sizeof(short_control),
    };
    CHECK(user_write(USER_SEND_PAYLOAD,
                    payload, sizeof(payload)) == 0 &&
            user_write(USER_SEND_IOV,
                    &ordinary_iov, sizeof(ordinary_iov)) == 0 &&
            user_write(USER_SEND_CONTROL,
                    &short_control, sizeof(short_control)) == 0 &&
            user_write(USER_SEND_HEADER, &header, sizeof(header)) == 0 &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == _EINVAL,
            "sendmsg 拒绝短于 cmsghdr 的 len 而不发生无符号下溢");

    header = (struct msghdr_) {
        .msg_iov = USER_UNMAPPED,
        .msg_iovlen = 1025,
    };
    CHECK(user_write(USER_SEND_HEADER, &header, sizeof(header)) == 0 &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == _EMSGSIZE,
            "sendmsg 在读取 guest iovec 前拒绝超过 Linux IOV_MAX 的数量");

    const struct iovec_ oversized_iov[2] = {
        {.base = USER_UNMAPPED,
                .len = SOCKET_IO_TRANSACTION_LIMIT / 2 + 1},
        {.base = USER_UNMAPPED,
                .len = SOCKET_IO_TRANSACTION_LIMIT / 2 + 1},
    };
    header = (struct msghdr_) {
        .msg_iov = USER_SEND_IOV,
        .msg_iovlen = 2,
    };
    CHECK(user_write(USER_SEND_IOV,
                    oversized_iov, sizeof(oversized_iov)) == 0 &&
            user_write(USER_SEND_HEADER, &header, sizeof(header)) == 0 &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == _EMSGSIZE,
            "数据报 sendmsg 在读取 payload 前拒绝超过事务内存上限的总长度");

    header = (struct msghdr_) {
        .msg_iov = USER_UNMAPPED,
        .msg_iovlen = 1025,
    };
    CHECK(user_write(USER_RECV_HEADER, &header, sizeof(header)) == 0 &&
            sys_recvmsg(pair[1], USER_RECV_HEADER, MSG_DONTWAIT_) ==
                    _EMSGSIZE,
            "recvmsg 在 host I/O 前拒绝超过 Linux IOV_MAX 的数量");
    byte_t unexpected = 0;
    CHECK(receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                    &unexpected, sizeof(unexpected), NULL) == _EAGAIN,
            "无效 sendmsg 边界输入没有向 host 队列写入 payload");
    CHECK(close_socket(fixture, &pair[1]) &&
            close_socket(fixture, &pair[0]),
            "清理 sendmsg 边界测试 socketpair");
    return true;
}

static bool test_recvmsg_scm_truncation_and_rollback(
        struct fixture *fixture) {
    fd_t pair[2] = {-1, -1};
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 SCM 截断与回滚测试 socketpair");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 SCM 截断引用探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    CHECK(passed_number >= 0, "安装 SCM 截断引用探针");

    byte_t control_sentinel[sizeof(struct guest_rights)];
    memset(control_sentinel, 0xa5, sizeof(control_sentinel));
    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == 3 &&
            user_write(USER_RECV_CONTROL,
                    control_sentinel, sizeof(control_sentinel)) == 0 &&
            prepare_recvmsg(USER_RECV_HEADER, USER_RECV_PAYLOAD, 0,
                    USER_RECV_CONTROL, sizeof(struct cmsghdr_)) &&
            sys_recvmsg(pair[1], USER_RECV_HEADER, 0) == 3,
            "控制容量不足时仍消费携带 SCM 的 payload");
    struct msghdr_ returned_header = {0};
    byte_t returned_control[sizeof(control_sentinel)] = {0};
    CHECK(user_read(USER_RECV_HEADER,
                    &returned_header, sizeof(returned_header)) == 0 &&
            user_read(USER_RECV_CONTROL,
                    returned_control, sizeof(returned_control)) == 0 &&
            returned_header.msg_controllen == 0 &&
            (returned_header.msg_flags & MSG_CTRUNC_) != 0 &&
            memcmp(returned_control,
                    control_sentinel, sizeof(control_sentinel)) == 0 &&
            atomic_load_explicit(&passed->refcount,
                    memory_order_relaxed) == 1,
            "SCM 容量不足设置 MSG_CTRUNC、丢弃引用且不越界写控制区");

    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == 3 &&
            prepare_recvmsg(USER_RECV_HEADER, USER_RECV_PAYLOAD, 0,
                    USER_UNMAPPED, sizeof(struct guest_rights)) &&
            sys_recvmsg(pair[1], USER_RECV_HEADER, 0) == _EFAULT,
            "SCM guest 控制写回失败返回 EFAULT");
    struct fd *receiver = f_get_task(&fixture->task, pair[1]);
    CHECK(receiver != NULL, "取得 SCM 回滚接收 socket");
    lock(&receiver->lock);
    bool queue_empty = list_empty(&receiver->socket.unix_scm);
    unlock(&receiver->lock);
    CHECK(queue_empty && atomic_load_explicit(&passed->refcount,
                    memory_order_relaxed) == 1 &&
            f_get_task(&fixture->task, passed_number + 1) == NULL,
            "控制写回 EFAULT 精确回滚不可见 fd 并消费 SCM 容器");
    byte_t unexpected = 0;
    CHECK(receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                    &unexpected, sizeof(unexpected), NULL) == _EAGAIN,
            "截断与写回失败的两条 SCM 报文均只消费一次");

    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[0], USER_SEND_HEADER, 0) == 3 &&
            sys_recvfrom(pair[1], USER_RECV_PAYLOAD, 3, 0, 0, 0) == 3,
            "不请求 ancillary 的 recv 仍消费携带 SCM_RIGHTS 的 payload");
    lock(&receiver->lock);
    queue_empty = list_empty(&receiver->socket.unix_scm);
    unlock(&receiver->lock);
    CHECK(queue_empty && atomic_load_explicit(&passed->refcount,
                    memory_order_relaxed) == 1,
            "recv 丢弃未请求的 fd 并保持下一次 recvmsg 队列对齐");

    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "SCM 截断与回滚后底层对象恰好析构一次");
    CHECK(close_socket(fixture, &pair[1]) &&
            close_socket(fixture, &pair[0]),
            "清理 SCM 截断与回滚测试 socketpair");
    return true;
}

static bool test_datagram_shutdown_preserves_queue(
        struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 's', 'h', 'u', 't', '-', 'r', 'x',
    };
    static const byte_t sender_name[] = {
        0, 's', 'h', 'u', 't', '-', 't', 'x',
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0 &&
            connect_unix_socket(fixture, receiver, &sender_address) == 0 &&
            connect_unix_socket(fixture, sender, &receiver_address) == 0,
            "创建双向连接的 shutdown 数据报两端");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 shutdown SCM 引用探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    CHECK(passed_number >= 0, "安装 shutdown SCM 引用探针");

    const byte_t ordinary[] = {0x44, 0x52};
    CHECK(send_connected_unix_datagram(
                    fixture, sender, NULL, 0, 0) == 0,
            "shutdown 前排入零长度数据报");
    CHECK(send_connected_unix_datagram(fixture, sender,
                    ordinary, sizeof(ordinary), 0) == sizeof(ordinary),
            "shutdown 前排入普通数据报");
    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(sender, USER_SEND_HEADER, 0) == 3,
            "shutdown 前排入 SCM 数据报");
    CHECK(
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "shutdown 前 SCM 容器持有一份传递引用");

    CHECK(sys_shutdown(receiver, SHUT_RD) == 0,
            "连接的数据报接收端成功执行 SHUT_RD");
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "SHUT_RD 保留已排队 SCM 的传递引用");

    const byte_t rejected = 0x65;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &rejected, sizeof(rejected), MSG_NOSIGNAL_) == _EPIPE &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(sender, USER_SEND_HEADER, MSG_NOSIGNAL_) == _EPIPE &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "读 shutdown 后普通与 SCM 发送按 Linux 语义返回 EPIPE 且不再入队");

    byte_t zero_sentinel = 0xa5;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &zero_sentinel, sizeof(zero_sentinel), NULL) == 0 &&
            zero_sentinel == 0xa5,
            "SHUT_RD 后先读取保留的零长度数据报");
    byte_t received_ordinary[sizeof(ordinary)] = {0};
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    received_ordinary, sizeof(received_ordinary), NULL) ==
                    sizeof(received_ordinary) &&
            memcmp(received_ordinary,
                    ordinary, sizeof(received_ordinary)) == 0,
            "SHUT_RD 后按顺序读取保留的普通数据报");

    struct socket_ref receiver_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, receiver, &receiver_ref) == 0,
            "取得已 SHUT_RD 的数据报接收端强引用");
    byte_t scm_payload[3] = {0};
    dword_t message_flags = 0;
    struct scm *received_scm = NULL;
    ssize_t received_length = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_, NULL,
            &message_flags, &received_scm);
    socket_ref_release(&receiver_ref);
    CHECK(received_length == sizeof(scm_payload) &&
            memcmp(scm_payload, "SCM", sizeof(scm_payload)) == 0 &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "SHUT_RD 后完整读取保留的 SCM 数据报");
    socket_scm_release(received_scm);
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "读取保留的 SCM 后仅剩已安装描述符的引用");

    byte_t empty = 0;
    CHECK(receive_unix_datagram(fixture, receiver, 0,
                    &empty, sizeof(empty), NULL) == 0,
            "SHUT_RD 的阻塞数据报接收在空队列返回 EOF");
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &empty, sizeof(empty), NULL) == _EAGAIN,
            "SHUT_RD 的 MSG_DONTWAIT 数据报接收在空队列返回 EAGAIN");

    CHECK(sys_shutdown(receiver, SHUT_RD) == 0 &&
            sys_shutdown(receiver, SHUT_RDWR) == 0 &&
            sys_shutdown(receiver, SHUT_RDWR) == 0,
            "重复 SHUT_RD 与 SHUT_RDWR 保持幂等");

    fd_t nonblocking_pair[2] = {-1, -1};
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_ | SOCK_NONBLOCK_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, nonblocking_pair,
                    sizeof(nonblocking_pair)) == 0 &&
            sys_shutdown(nonblocking_pair[1], SHUT_RD) == 0 &&
            receive_unix_datagram(fixture, nonblocking_pair[1], 0,
                    &empty, sizeof(empty), NULL) == _EAGAIN,
            "SHUT_RD 的非阻塞数据报接收在空队列返回 EAGAIN");
    CHECK(close_socket(fixture, &nonblocking_pair[1]) &&
            close_socket(fixture, &nonblocking_pair[0]),
            "清理非阻塞 shutdown 数据报 socketpair");

    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "shutdown 后读取的 SCM 对象恰好析构一次");
    CHECK(close_socket(fixture, &sender),
            "关闭 shutdown 数据报发送端");
    fd_t rebound = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(rebound >= 0 &&
            bind_unix_socket(fixture, rebound, &sender_address) == 0,
            "读取保留报文后释放每条报文的反向名称计数");
    CHECK(close_socket(fixture, &rebound) &&
            close_socket(fixture, &receiver),
            "清理已读 shutdown 的数据报两端");
    return true;
}

static bool test_datagram_socketpair_close_detaches_peer(
        struct fixture *fixture) {
    int host_fd_baseline = open_host_fds();
    fd_t pair[2] = {-1, -1};
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 peer 关闭回归的 DGRAM socketpair");

    struct socket_ref sender;
    CHECK(socket_ref_get_task(
                    &fixture->task, pair[0], &sender) == 0,
            "取得 DGRAM surviving peer 强引用");
    CHECK(close_socket(fixture, &pair[1]) &&
            sender.fd->socket.unix_dgram_peer == NULL &&
            list_null(&sender.fd->socket.unix_dgram_peer_link),
            "DGRAM peer 最终关闭后 raw 关系已安全拆除");

    struct sockaddr_storage peer_name;
    dword_t peer_name_length = 0;
    struct socket_option_result error_result;
    sdword_t socket_error = -1;
    int events_before_send = sender.fd->ops->poll(sender.fd);
    CHECK(socket_getname_ref(&sender, true,
                    &peer_name, &peer_name_length) == 0 &&
            socket_getsockopt_ref(&sender,
                    SOL_SOCKET_, SO_ERROR_, sizeof(socket_error),
                    SOCKET_GUEST_I386, &error_result) == 0 &&
            error_result.length == sizeof(socket_error),
            "peer close 后 guest 逻辑连接与 SO_ERROR 仍可查询");
    memcpy(&socket_error, error_result.value, sizeof(socket_error));
    CHECK(socket_error == 0 &&
            (events_before_send & POLL_WRITE) != 0 &&
            (events_before_send &
                    (POLL_READ | POLL_ERR | POLL_HUP)) == 0,
            "peer close 不泄漏 Darwin 的 reset、READ 或 HUP");

    const byte_t payload = 0x71;
    const struct iovec_ iov = {
        .base = USER_SEND_PAYLOAD,
        .len = sizeof(payload),
    };
    const struct msghdr_ header = {
        .msg_iov = USER_SEND_IOV,
        .msg_iovlen = 1,
    };
    CHECK(user_write(USER_SEND_PAYLOAD,
                    &payload, sizeof(payload)) == 0 &&
            user_write(USER_SEND_IOV, &iov, sizeof(iov)) == 0 &&
            user_write(USER_SEND_HEADER, &header, sizeof(header)) == 0,
            "准备 peer 关闭后的标量与向量发送");
    int_t scalar_result = sys_sendto(pair[0], USER_SEND_PAYLOAD,
            sizeof(payload), MSG_DONTWAIT_ | MSG_NOSIGNAL_, 0, 0);
    int_t peer_after_first = socket_getname_ref(&sender, true,
            &peer_name, &peer_name_length);
    int_t vector_result = sys_sendmsg(pair[0], USER_SEND_HEADER,
            MSG_DONTWAIT_ | MSG_NOSIGNAL_);
    byte_t empty = 0;
    int_t receive_result = (int_t) socket_recvfrom_ref(
            &sender, &empty, sizeof(empty), MSG_DONTWAIT_, NULL);
    socket_error = -1;
    CHECK(socket_getsockopt_ref(&sender,
                    SOL_SOCKET_, SO_ERROR_, sizeof(socket_error),
                    SOCKET_GUEST_I386, &error_result) == 0,
            "peer close 首次发送后仍可查询 SO_ERROR");
    memcpy(&socket_error, error_result.value, sizeof(socket_error));
    int events_after_send = sender.fd->ops->poll(sender.fd);
    CHECK(scalar_result == _ECONNREFUSED &&
            peer_after_first == _ENOTCONN &&
            vector_result == _ENOTCONN &&
            receive_result == _EAGAIN && socket_error == 0 &&
            (events_after_send & POLL_WRITE) != 0 &&
            (events_after_send & (POLL_ERR | POLL_HUP)) == 0,
            "首个无地址发送返回 ECONNREFUSED，后续稳定为 ENOTCONN");

    socket_ref_release(&sender);
    CHECK(close_socket(fixture, &pair[0]) &&
            open_host_fds() == host_fd_baseline,
            "DGRAM peer 关闭回归未泄漏 guest 或 host fd");
    return true;
}

static bool test_datagram_close_detaches_many_senders(
        struct fixture *fixture) {
    static const byte_t target_name[] = {
        0, 'm', 'a', 'n', 'y', '-', 's', 'e', 'n', 'd', 'e', 'r', 's',
    };
    struct guest_unix_address target_address =
            unix_address(target_name, sizeof(target_name));
    int host_fd_baseline = open_host_fds();
    fd_t target = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t senders[MANY_DGRAM_SENDERS];
    for (size_t index = 0; index < MANY_DGRAM_SENDERS; index++)
        senders[index] = -1;
    CHECK(target >= 0 &&
            bind_unix_socket(fixture,
                    target, &target_address) == 0,
            "创建多 sender DGRAM close 目标");
    for (size_t index = 0; index < MANY_DGRAM_SENDERS; index++) {
        senders[index] = create_unix_socket(fixture, SOCK_DGRAM_);
        CHECK(senders[index] >= 0 &&
                connect_unix_socket(fixture,
                        senders[index], &target_address) == 0,
                "建立超过单批容量的 DGRAM incoming 弱边");
    }

    CHECK(close_socket(fixture, &target),
            "最终关闭拥有多批 incoming sender 的目标");
    const byte_t payload = 0x65;
    for (size_t index = 0; index < MANY_DGRAM_SENDERS; index++) {
        struct socket_ref sender_ref;
        CHECK(socket_ref_get_task(&fixture->task,
                        senders[index], &sender_ref) == 0 &&
                sender_ref.fd->socket.unix_dgram_peer == NULL &&
                list_null(&sender_ref.fd->socket.unix_dgram_peer_link),
                "每一批 sender 都已脱离关闭目标");
        socket_ref_release(&sender_ref);
        CHECK(send_connected_unix_datagram(fixture, senders[index],
                        &payload, sizeof(payload), MSG_NOSIGNAL_) ==
                        _ECONNREFUSED &&
                close_socket(fixture, &senders[index]),
                "多批 sender 各自消费一次 dead-peer 错误并安全关闭");
    }
    CHECK(open_host_fds() == host_fd_baseline,
            "多批 DGRAM detach 未泄漏 guest 或 host fd");
    return true;
}

static bool test_datagram_explicit_send_preserves_dead_peer(
        struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'd', 'e', 'a', 'd', '-', 'p', 'e', 'e', 'r', '-', 's', 'e', 'n', 'd',
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    int host_fd_baseline = open_host_fds();
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t pair[2] = {-1, -1};
    CHECK(receiver >= 0 &&
            bind_unix_socket(fixture,
                    receiver, &receiver_address) == 0 &&
            sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建显式发送不消费 dead-peer 状态的端点");

    struct socket_ref sender;
    CHECK(socket_ref_get_task(
                    &fixture->task, pair[0], &sender) == 0 &&
            close_socket(fixture, &pair[1]),
            "关闭显式发送回归的原连接 peer");
    const byte_t payload = 0x6d;
    CHECK(send_unix_datagram(fixture, pair[0], &receiver_address,
                    &payload, sizeof(payload)) == sizeof(payload),
            "带 msg_name 的 sendto 不消费 dead-peer 状态");
    struct sockaddr_storage peer_name;
    dword_t peer_name_length = 0;
    CHECK(socket_getname_ref(&sender, true,
                    &peer_name, &peer_name_length) == 0,
            "显式 sendto 后原逻辑 peer 仍可查询");
    CHECK(user_write(USER_SEND_PAYLOAD,
                    &payload, sizeof(payload)) == 0 &&
            sys_sendto(pair[0], USER_SEND_PAYLOAD,
                    sizeof(payload), MSG_NOSIGNAL_, 0, 0) ==
                    _ECONNREFUSED,
            "后续首个无地址发送才消费 dead-peer 状态");
    byte_t delivered = 0;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &delivered, sizeof(delivered), NULL) ==
                    (ssize_t) sizeof(delivered) &&
            delivered == payload,
            "显式数据报只送达指定 receiver");

    socket_ref_release(&sender);
    CHECK(close_socket(fixture, &pair[0]) &&
            close_socket(fixture, &receiver) &&
            open_host_fds() == host_fd_baseline,
            "显式发送 dead-peer 回归未泄漏 fd");
    return true;
}

static bool test_datagram_dead_peer_send_purges_queue(
        struct fixture *fixture) {
    int host_fd_baseline = open_host_fds();
    fd_t pair[2] = {-1, -1};
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 dead-peer 队列清理回归的 DGRAM socketpair");
    struct socket_ref survivor;
    CHECK(socket_ref_get_task(
                    &fixture->task, pair[0], &survivor) == 0,
            "取得 dead-peer 队列 survivor 强引用");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 dead-peer SCM 引用探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    const byte_t ordinary = 0x6e;
    CHECK(passed_number >= 0 &&
            user_write(USER_SEND_PAYLOAD,
                    &ordinary, sizeof(ordinary)) == 0 &&
            sys_sendto(pair[1], USER_SEND_PAYLOAD,
                    sizeof(ordinary), MSG_NOSIGNAL_, 0, 0) ==
                    (int_t) sizeof(ordinary) &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[1], USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "dead-peer 前排入普通数据报与 SCM_RIGHTS");
    CHECK(close_socket(fixture, &pair[1]) &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "peer close 本身保留 survivor 的旧入站队列");

    byte_t peeked = 0;
    CHECK(socket_recvfrom_ref(&survivor,
                    &peeked, sizeof(peeked),
                    MSG_PEEK_ | MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(peeked) &&
            peeked == ordinary,
            "首个发送前旧入站数据仍可观察");
    const byte_t outbound = 0x6f;
    CHECK(user_write(USER_SEND_PAYLOAD,
                    &outbound, sizeof(outbound)) == 0 &&
            sys_sendto(pair[0], USER_SEND_PAYLOAD,
                    sizeof(outbound), MSG_NOSIGNAL_, 0, 0) ==
                    _ECONNREFUSED &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "首个无地址发送清理旧 payload、SCM 与引用");
    byte_t discarded = 0;
    CHECK(socket_recvfrom_ref(&survivor,
                    &discarded, sizeof(discarded),
                    MSG_DONTWAIT_, NULL) == _EAGAIN &&
            f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "dead-peer 清理后队列为空且 SCM 探针恰好析构一次");

    socket_ref_release(&survivor);
    CHECK(close_socket(fixture, &pair[0]) &&
            open_host_fds() == host_fd_baseline,
            "dead-peer 队列清理回归未泄漏 fd");
    return true;
}

static bool test_datagram_peer_credentials_stay_snapshot(
        struct fixture *fixture) {
    static const byte_t pair_target_name[] = {
        0, 'p', 'e', 'e', 'r', 'c', 'r', 'e', 'd', '-', 'p', 'a', 'i', 'r',
    };
    static const byte_t ordinary_target_name[] = {
        0, 'p', 'e', 'e', 'r', 'c', 'r', 'e', 'd', '-', 'n', 'a', 'm', 'e', 'd',
    };
    struct guest_unix_address pair_target_address =
            unix_address(pair_target_name, sizeof(pair_target_name));
    struct guest_unix_address ordinary_target_address =
            unix_address(ordinary_target_name, sizeof(ordinary_target_name));
    int host_fd_baseline = open_host_fds();
    fd_t pair_target = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t pair[2] = {-1, -1};
    CHECK(pair_target >= 0 &&
            bind_unix_socket(fixture,
                    pair_target, &pair_target_address) == 0 &&
            sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 DGRAM socketpair peercred 快照回归");
    struct socket_ref pair_sender;
    struct ucred_ expected_pair;
    struct ucred_ observed_pair;
    CHECK(socket_ref_get_task(
                    &fixture->task, pair[0], &pair_sender) == 0 &&
            read_peer_credentials(&pair_sender, &expected_pair) &&
            connect_unix_socket(fixture,
                    pair[0], &pair_target_address) == 0 &&
            read_peer_credentials(&pair_sender, &observed_pair) &&
            memcmp(&expected_pair,
                    &observed_pair, sizeof(expected_pair)) == 0,
            "socketpair 重连具名 peer 后保留创建时凭据");

    uint16_t unspecified_family = 0;
    CHECK(socket_connect_ref_task(&fixture->task, &pair_sender,
                    &unspecified_family,
                    sizeof(unspecified_family)) == 0 &&
            read_peer_credentials(&pair_sender, &observed_pair) &&
            memcmp(&expected_pair,
                    &observed_pair, sizeof(expected_pair)) == 0 &&
            connect_unix_socket(fixture,
                    pair[0], &pair_target_address) == 0,
            "AF_UNSPEC disconnect 不清除 socketpair peercred 快照");
    CHECK(close_socket(fixture, &pair_target),
            "关闭 peercred 回归的具名目标");
    const byte_t payload = 0x70;
    CHECK(user_write(USER_SEND_PAYLOAD,
                    &payload, sizeof(payload)) == 0 &&
            sys_sendto(pair[0], USER_SEND_PAYLOAD,
                    sizeof(payload), MSG_NOSIGNAL_, 0, 0) ==
                    _ECONNREFUSED &&
            read_peer_credentials(&pair_sender, &observed_pair) &&
            memcmp(&expected_pair,
                    &observed_pair, sizeof(expected_pair)) == 0,
            "目标 close 与首个失败发送仍保留 socketpair peercred");
    socket_ref_release(&pair_sender);
    CHECK(close_socket(fixture, &pair[1]) &&
            close_socket(fixture, &pair[0]),
            "清理 socketpair peercred 回归端点");

    fd_t ordinary_target = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t ordinary = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(ordinary_target >= 0 && ordinary >= 0 &&
            bind_unix_socket(fixture,
                    ordinary_target, &ordinary_target_address) == 0,
            "创建普通 DGRAM peercred 回归端点");
    struct socket_ref ordinary_sender;
    struct ucred_ invalid_before;
    struct ucred_ invalid_after;
    CHECK(socket_ref_get_task(
                    &fixture->task, ordinary, &ordinary_sender) == 0 &&
            read_peer_credentials(&ordinary_sender, &invalid_before) &&
            invalid_before.pid == 0 &&
            invalid_before.uid == (uid_t_) -1 &&
            invalid_before.gid == (uid_t_) -1 &&
            connect_unix_socket(fixture,
                    ordinary, &ordinary_target_address) == 0 &&
            read_peer_credentials(&ordinary_sender, &invalid_after) &&
            memcmp(&invalid_before,
                    &invalid_after, sizeof(invalid_before)) == 0,
            "普通具名 DGRAM connect 不凭空获得目标凭据");
    socket_ref_release(&ordinary_sender);
    CHECK(close_socket(fixture, &ordinary) &&
            close_socket(fixture, &ordinary_target) &&
            open_host_fds() == host_fd_baseline,
            "DGRAM peercred 快照回归未泄漏 fd");
    return true;
}

static bool test_datagram_same_name_rebind_changes_identity(
        struct fixture *fixture) {
    static const byte_t sender_name[] = {
        0, 'r', 'e', 'b', 'i', 'n', 'd', '-', 's', 'e', 'n', 'd', 'e', 'r',
    };
    static const byte_t target_name[] = {
        0, 'r', 'e', 'b', 'i', 'n', 'd', '-', 't', 'a', 'r', 'g', 'e', 't',
    };
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    struct guest_unix_address target_address =
            unix_address(target_name, sizeof(target_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t old_target = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && old_target >= 0 &&
            bind_unix_socket(fixture,
                    sender, &sender_address) == 0 &&
            bind_unix_socket(fixture,
                    old_target, &target_address) == 0 &&
            connect_unix_socket(fixture,
                    sender, &target_address) == 0 &&
            connect_unix_socket(fixture,
                    old_target, &sender_address) == 0,
            "创建同名重绑前的双向 DGRAM 路由");
    const byte_t stale = 0x71;
    CHECK(send_connected_unix_datagram(fixture, old_target,
                    &stale, sizeof(stale), MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(stale) &&
            close_socket(fixture, &old_target),
            "旧同名 endpoint 关闭前排入待清理数据报");

    fd_t new_target = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(new_target >= 0 &&
            bind_unix_socket(fixture,
                    new_target, &target_address) == 0,
            "相同 guest 名称已绑定到新的 endpoint 对象");
    const byte_t premature = 0x70;
    CHECK(send_unix_datagram(fixture, new_target, &sender_address,
                    &premature, sizeof(premature)) == _EPERM,
            "dead-peer 墓碑按对象身份拒绝尚未重连的同名 endpoint");
    CHECK(connect_unix_socket(fixture,
                    sender, &target_address) == 0,
            "相同 guest 名称的新 endpoint 以新 transport 身份重连");
    byte_t discarded = 0;
    CHECK(receive_unix_datagram(fixture, sender, MSG_DONTWAIT_,
                    &discarded, sizeof(discarded), NULL) == _EAGAIN,
            "同名重绑仍清除旧 endpoint 的入站队列");
    const byte_t outbound = 0x72;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &outbound, sizeof(outbound), MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(outbound),
            "同名重绑后无地址发送使用新 transport 身份");
    byte_t delivered = 0;
    CHECK(receive_unix_datagram(fixture, new_target, MSG_DONTWAIT_,
                    &delivered, sizeof(delivered), NULL) ==
                    (ssize_t) sizeof(delivered) &&
            delivered == outbound,
            "同名重绑后的 payload 只到达新 endpoint");

    CHECK(close_socket(fixture, &new_target) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "同名重绑身份回归未泄漏 fd");
    return true;
}

static bool test_datagram_concurrent_connect_commits_one_route(
        struct fixture *fixture) {
    static const byte_t first_name[] = {
        0, 'c', 'o', 'n', 'n', 'e', 'c', 't', '-', 'r', 'a', 'c', 'e', '-', 'a',
    };
    static const byte_t second_name[] = {
        0, 'c', 'o', 'n', 'n', 'e', 'c', 't', '-', 'r', 'a', 'c', 'e', '-', 'b',
    };
    struct guest_unix_address first_address =
            unix_address(first_name, sizeof(first_name));
    struct guest_unix_address second_address =
            unix_address(second_name, sizeof(second_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t first = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t second = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && first >= 0 && second >= 0 &&
            bind_unix_socket(fixture, first, &first_address) == 0 &&
            bind_unix_socket(fixture, second, &second_address) == 0,
            "创建并发 DGRAM connect 的两个目标");
    struct socket_ref sender_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, sender, &sender_ref) == 0,
            "取得并发 DGRAM connect sender 强引用");

    for (unsigned iteration = 0;
            iteration < CONNECT_RACE_ITERATIONS; iteration++) {
        struct datagram_connect_call first_call = {
            .fixture = fixture,
            .sender = sender,
            .destination = &first_address,
            .result = _EIO,
        };
        struct datagram_connect_call second_call = {
            .fixture = fixture,
            .sender = sender,
            .destination = &second_address,
            .result = _EIO,
        };
        atomic_init(&first_call.ready, false);
        atomic_init(&first_call.start, false);
        atomic_init(&second_call.ready, false);
        atomic_init(&second_call.start, false);
        pthread_t first_thread;
        pthread_t second_thread;
        CHECK(pthread_create(&first_thread, NULL,
                        connect_datagram_concurrently,
                        &first_call) == 0 &&
                pthread_create(&second_thread, NULL,
                        connect_datagram_concurrently,
                        &second_call) == 0,
                "启动并发 DGRAM connect 线程");
        while (!atomic_load_explicit(
                &first_call.ready, memory_order_acquire) ||
                !atomic_load_explicit(
                &second_call.ready, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(
                &first_call.start, true, memory_order_release);
        atomic_store_explicit(
                &second_call.start, true, memory_order_release);
        CHECK(pthread_join(first_thread, NULL) == 0 &&
                pthread_join(second_thread, NULL) == 0 &&
                first_call.result == 0 && second_call.result == 0,
                "并发 DGRAM connect 都在线性化事务中完成");

        struct socket_address peer = {0};
        dword_t peer_length = 0;
        CHECK(socket_getname_ref(&sender_ref, true,
                        &peer.storage, &peer_length) == 0,
                "并发 connect 后取得最终 guest peer 名称");
        peer.length = (socklen_t) peer_length;
        bool chose_first = source_equals(
                &peer, first_name, sizeof(first_name));
        bool chose_second = source_equals(
                &peer, second_name, sizeof(second_name));
        CHECK(chose_first != chose_second,
                "最终 guest 元数据只提交一个完整 peer 身份");

        byte_t payload = (byte_t) iteration;
        CHECK(send_connected_unix_datagram(fixture, sender,
                        &payload, sizeof(payload), MSG_NOSIGNAL_) ==
                        (ssize_t) sizeof(payload),
                "并发 connect 后无地址发送成功");
        byte_t first_payload = 0;
        byte_t second_payload = 0;
        ssize_t first_result = receive_unix_datagram(
                fixture, first, MSG_DONTWAIT_,
                &first_payload, sizeof(first_payload), NULL);
        ssize_t second_result = receive_unix_datagram(
                fixture, second, MSG_DONTWAIT_,
                &second_payload, sizeof(second_payload), NULL);
        bool first_delivery = chose_first &&
                first_result == (ssize_t) sizeof(first_payload) &&
                first_payload == payload && second_result == _EAGAIN;
        bool second_delivery = chose_second &&
                second_result == (ssize_t) sizeof(second_payload) &&
                second_payload == payload && first_result == _EAGAIN;
        CHECK(first_delivery || second_delivery,
                "host 路由与最终 guest peer 元数据指向同一目标");
    }

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建并发 connect 后的 SCM 探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    CHECK(passed_number >= 0 &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(sender, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3,
            "并发 connect 后 SCM_RIGHTS 使用最终路由");
    struct socket_address final_peer = {0};
    dword_t final_peer_length = 0;
    CHECK(socket_getname_ref(&sender_ref, true,
                    &final_peer.storage, &final_peer_length) == 0,
            "SCM 发送后取得最终 peer 名称");
    final_peer.length = (socklen_t) final_peer_length;
    bool final_first = source_equals(
            &final_peer, first_name, sizeof(first_name));
    fd_t final_receiver = final_first ? first : second;
    fd_t other_receiver = final_first ? second : first;
    struct socket_ref receiver_ref;
    CHECK(socket_ref_get_task(&fixture->task,
                    final_receiver, &receiver_ref) == 0,
            "取得并发 connect 最终 SCM receiver 强引用");
    byte_t scm_payload[3] = {0};
    dword_t message_flags = 0;
    struct scm *received_scm = NULL;
    ssize_t received = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    socket_ref_release(&receiver_ref);
    byte_t unexpected = 0;
    CHECK(received == (ssize_t) sizeof(scm_payload) &&
            memcmp(scm_payload, "SCM", sizeof(scm_payload)) == 0 &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed &&
            receive_unix_datagram(fixture, other_receiver,
                    MSG_DONTWAIT_, &unexpected,
                    sizeof(unexpected), NULL) == _EAGAIN,
            "SCM host dummy、内部 owner 与最终 guest 名称一致");
    socket_scm_release(received_scm);
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "并发 connect 后 SCM 引用精确归还");

    socket_ref_release(&sender_ref);
    CHECK(close_socket(fixture, &second) &&
            close_socket(fixture, &first) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "并发 DGRAM connect 回归未泄漏 fd");
    return true;
}

static bool test_datagram_failed_connect_preserves_old_route(
        struct fixture *fixture) {
    static const byte_t sender_name[] = {
        0, 'f', 'a', 'i', 'l', '-', 'c', 'o', 'n', 'n', '-', 's', 'e', 'n', 'd',
    };
    static const byte_t old_name[] = {
        0, 'f', 'a', 'i', 'l', '-', 'c', 'o', 'n', 'n', '-', 'o', 'l', 'd',
    };
    static const byte_t missing_name[] = {
        0, 'f', 'a', 'i', 'l', '-', 'c', 'o', 'n', 'n', '-', 'm', 'i', 's', 's',
    };
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    struct guest_unix_address old_address =
            unix_address(old_name, sizeof(old_name));
    struct guest_unix_address missing_address =
            unix_address(missing_name, sizeof(missing_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t old_peer = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t failed_peer = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && old_peer >= 0 && failed_peer >= 0 &&
            bind_unix_socket(fixture,
                    sender, &sender_address) == 0 &&
            bind_unix_socket(fixture,
                    old_peer, &old_address) == 0 &&
            bind_unix_socket(fixture,
                    failed_peer, &missing_address) == 0 &&
            connect_unix_socket(fixture,
                    sender, &old_address) == 0 &&
            connect_unix_socket(fixture,
                    old_peer, &sender_address) == 0,
            "创建失败 connect 前的旧双向 DGRAM 路由");
    const byte_t queued = 0x73;
    CHECK(send_connected_unix_datagram(fixture, old_peer,
                    &queued, sizeof(queued), MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(queued),
            "失败 connect 前在 sender 排入旧数据报");
    struct socket_ref failed_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, failed_peer, &failed_ref) == 0,
            "取得 host 失败目标的强引用");
    char failed_backing[sizeof(
            ((struct sockaddr_un *) 0)->sun_path)];
    strcpy(failed_backing,
            failed_ref.fd->socket.unix_backing_path);
    CHECK(unlink(failed_backing) == 0,
            "仅移除失败目标的 host backing，保留 guest 名称与 owner");
    CHECK(connect_unix_socket(fixture,
                    sender, &missing_address) == _ECONNREFUSED,
            "host connect 失败并破坏路由后回滚为 ECONNREFUSED");

    struct socket_ref sender_ref;
    struct socket_address peer = {0};
    dword_t peer_length = 0;
    CHECK(socket_ref_get_task(
                    &fixture->task, sender, &sender_ref) == 0 &&
            socket_getname_ref(&sender_ref, true,
                    &peer.storage, &peer_length) == 0,
            "失败 connect 后取得旧 peer 元数据");
    peer.length = (socklen_t) peer_length;
    struct socket_option_result error_result;
    sdword_t socket_error = -1;
    CHECK(source_equals(&peer, old_name, sizeof(old_name)) &&
            socket_getsockopt_ref(&sender_ref,
                    SOL_SOCKET_, SO_ERROR_, sizeof(socket_error),
                    SOCKET_GUEST_I386, &error_result) == 0,
            "失败 connect 后 guest peer 身份与 SO_ERROR 可查询");
    memcpy(&socket_error, error_result.value, sizeof(socket_error));
    byte_t peeked = 0;
    CHECK(socket_error == 0 &&
            socket_recvfrom_ref(&sender_ref,
                    &peeked, sizeof(peeked),
                    MSG_PEEK_ | MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(peeked) &&
            peeked == queued,
            "失败 connect 保留旧入站 payload 与错误状态");
    const byte_t outbound = 0x74;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &outbound, sizeof(outbound), MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(outbound),
            "失败 connect 后旧无地址发送路由仍可用");
    byte_t delivered = 0;
    CHECK(receive_unix_datagram(fixture, old_peer, MSG_DONTWAIT_,
                    &delivered, sizeof(delivered), NULL) ==
                    (ssize_t) sizeof(delivered) &&
            delivered == outbound,
            "失败 connect 后 payload 仍送达旧 peer");
    socket_ref_release(&sender_ref);
    socket_ref_release(&failed_ref);

    CHECK(close_socket(fixture, &failed_peer) &&
            close_socket(fixture, &old_peer) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "失败 DGRAM connect 回归未泄漏 fd");
    return true;
}

static bool test_datagram_connected_receiver_filters_senders(
        struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'm', 'a', 'y', '-', 'r', 'e', 'c', 'v',
    };
    static const byte_t allowed_name[] = {
        0, 'm', 'a', 'y', '-', 'a', 'l', 'l', 'o', 'w',
    };
    static const byte_t intruder_name[] = {
        0, 'm', 'a', 'y', '-', 'd', 'e', 'n', 'y',
    };
    static const byte_t old_target_name[] = {
        0, 'm', 'a', 'y', '-', 'o', 'l', 'd',
    };
    static const byte_t alternate_name[] = {
        0, 'm', 'a', 'y', '-', 'a', 'l', 't',
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address allowed_address =
            unix_address(allowed_name, sizeof(allowed_name));
    struct guest_unix_address intruder_address =
            unix_address(intruder_name, sizeof(intruder_name));
    struct guest_unix_address old_target_address =
            unix_address(old_target_name, sizeof(old_target_name));
    struct guest_unix_address alternate_address =
            unix_address(alternate_name, sizeof(alternate_name));
    int host_fd_baseline = open_host_fds();
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t allowed = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t intruder = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t old_target = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t alternate = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && allowed >= 0 && intruder >= 0 &&
            old_target >= 0 && alternate >= 0 &&
            bind_unix_socket(fixture,
                    receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture,
                    allowed, &allowed_address) == 0 &&
            bind_unix_socket(fixture,
                    intruder, &intruder_address) == 0 &&
            bind_unix_socket(fixture,
                    old_target, &old_target_address) == 0 &&
            bind_unix_socket(fixture,
                    alternate, &alternate_address) == 0,
            "创建 unix_may_send 具名数据报端点");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 unix_may_send SCM 探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    const byte_t queued_before_connect = 0x41;
    CHECK(passed_number >= 0 &&
            send_unix_datagram(fixture, intruder, &receiver_address,
                    &queued_before_connect,
                    sizeof(queued_before_connect)) ==
                    (ssize_t) sizeof(queued_before_connect) &&
            prepare_sendmsg_rights_to(
                    passed_number, &receiver_address) &&
            sys_sendmsg(intruder, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2 &&
            connect_unix_socket(fixture,
                    receiver, &allowed_address) == 0,
            "首次 connect 保留连接前已排队的第三方 payload 与 SCM");

    byte_t received = 0;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &received, sizeof(received), NULL) ==
                    (ssize_t) sizeof(received) &&
            received == queued_before_connect,
            "首次 connect 后读取旧普通数据报");
    struct socket_ref receiver_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, receiver, &receiver_ref) == 0,
            "取得 unix_may_send 接收端引用");
    byte_t scm_payload[3] = {0};
    dword_t message_flags = 0;
    struct scm *received_scm = NULL;
    ssize_t received_length = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    CHECK(received_length == sizeof(scm_payload) &&
            memcmp(scm_payload, "SCM", sizeof(scm_payload)) == 0 &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "首次 connect 后读取旧 SCM 数据报");
    socket_scm_release(received_scm);
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            connect_unix_socket(fixture,
                    intruder, &old_target_address) == 0,
            "旧 SCM 引用归还并为第三方建立原路由");

    const byte_t rejected = 0x52;
    CHECK(send_unix_datagram(fixture, intruder, &receiver_address,
                    &rejected, sizeof(rejected)) == _EPERM,
            "已连接 receiver 拒绝第三方 payload");
    CHECK(prepare_sendmsg_rights_to(
                    passed_number, &receiver_address) &&
            sys_sendmsg(intruder, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == _EPERM &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "已连接 receiver 拒绝第三方 SCM 且归还临时引用");
    CHECK(connect_unix_socket(fixture,
                    intruder, &receiver_address) == _EPERM,
            "第三方 connect 已连接 receiver 返回 EPERM");
    struct socket_ref intruder_ref;
    struct socket_address intruder_peer = {0};
    dword_t intruder_peer_length = 0;
    CHECK(socket_ref_get_task(
                    &fixture->task, intruder, &intruder_ref) == 0 &&
            socket_getname_ref(&intruder_ref, true,
                    &intruder_peer.storage,
                    &intruder_peer_length) == 0,
            "失败 connect 后取得第三方旧路由");
    intruder_peer.length = (socklen_t) intruder_peer_length;
    const byte_t old_route_payload = 0x53;
    CHECK(source_equals(&intruder_peer,
                    old_target_name, sizeof(old_target_name)) &&
            send_connected_unix_datagram(fixture, intruder,
                    &old_route_payload, sizeof(old_route_payload),
                    MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(old_route_payload) &&
            receive_unix_datagram(fixture, old_target, MSG_DONTWAIT_,
                    &received, sizeof(received), NULL) ==
                    (ssize_t) sizeof(received) &&
            received == old_route_payload,
            "EPERM connect 完整保留第三方旧路由");
    socket_ref_release(&intruder_ref);

    const byte_t allowed_payload = 0x61;
    CHECK(send_unix_datagram(fixture, allowed, &receiver_address,
                    &allowed_payload, sizeof(allowed_payload)) ==
                    (ssize_t) sizeof(allowed_payload) &&
            prepare_sendmsg_rights_to(
                    passed_number, &receiver_address) &&
            sys_sendmsg(allowed, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3,
            "receiver 认可的 sender 可发送 payload 与 SCM");
    CHECK(socket_recvfrom_ref(&receiver_ref,
                    &received, sizeof(received), MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(received) &&
            received == allowed_payload,
            "认可 sender 的普通数据报精确入队");
    received_scm = NULL;
    received_length = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    CHECK(received_length == sizeof(scm_payload) &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "认可 sender 的 SCM 数据报精确入队");
    socket_scm_release(received_scm);
    sdword_t socket_error = -1;
    CHECK(read_socket_error(&receiver_ref, &socket_error) &&
            socket_error == 0 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "EPERM 门禁不污染 SO_ERROR 或 SCM 引用");

    const byte_t preserved_inbound = 0x71;
    const byte_t alternate_outbound = 0x72;
    CHECK(send_unix_datagram(fixture, allowed, &receiver_address,
                    &preserved_inbound, sizeof(preserved_inbound)) ==
                    (ssize_t) sizeof(preserved_inbound) &&
            prepare_sendmsg_rights_to(
                    passed_number, &receiver_address) &&
            sys_sendmsg(allowed, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            send_unix_datagram(fixture, receiver, &alternate_address,
                    &alternate_outbound, sizeof(alternate_outbound)) ==
                    (ssize_t) sizeof(alternate_outbound) &&
            prepare_sendmsg_rights_to(
                    passed_number, &alternate_address) &&
            sys_sendmsg(receiver, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 3,
            "替代目标发送事务保留默认路由入站 payload 与 SCM");
    CHECK(receive_unix_datagram(fixture, alternate, MSG_DONTWAIT_,
                    &received, sizeof(received), NULL) ==
                    (ssize_t) sizeof(received) &&
            received == alternate_outbound,
            "connected DGRAM 显式 payload 送达替代目标");
    struct socket_ref alternate_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, alternate, &alternate_ref) == 0,
            "取得替代目标接收引用");
    received_scm = NULL;
    received_length = socket_recvmsg_ref(&alternate_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    CHECK(received_length == sizeof(scm_payload) &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "connected DGRAM 显式 SCM 送达替代目标");
    socket_scm_release(received_scm);
    socket_ref_release(&alternate_ref);
    CHECK(socket_recvfrom_ref(&receiver_ref,
                    &received, sizeof(received), MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(received) &&
            received == preserved_inbound,
            "替代目标事务未丢弃默认路由入站 payload");
    received_scm = NULL;
    received_length = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    CHECK(received_length == sizeof(scm_payload) &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "替代目标事务未丢弃默认路由入站 SCM");
    socket_scm_release(received_scm);
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "替代目标与默认路由 SCM 引用均完整归还");
    const byte_t default_outbound = 0x73;
    CHECK(send_connected_unix_datagram(fixture, receiver,
                    &default_outbound, sizeof(default_outbound),
                    MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(default_outbound) &&
            receive_unix_datagram(fixture, allowed, MSG_DONTWAIT_,
                    &received, sizeof(received), NULL) ==
                    (ssize_t) sizeof(received) &&
            received == default_outbound,
            "最后一次显式 SCM 事务后默认 host 路由仍已恢复");

    CHECK(sys_shutdown(receiver, SHUT_RD) == 0 &&
            send_unix_datagram(fixture, intruder, &receiver_address,
                    &rejected, sizeof(rejected)) == _EPERM &&
            send_unix_datagram(fixture, allowed, &receiver_address,
                    &rejected, sizeof(rejected)) == _EPIPE,
            "unix_may_send 的 EPERM 优先于 receiver SHUT_RD 的 EPIPE");
    socket_ref_release(&receiver_ref);

    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1 &&
            close_socket(fixture, &alternate) &&
            close_socket(fixture, &old_target) &&
            close_socket(fixture, &intruder) &&
            close_socket(fixture, &allowed) &&
            close_socket(fixture, &receiver) &&
            open_host_fds() == host_fd_baseline,
            "unix_may_send 回归未泄漏 SCM、guest 或 host fd");
    return true;
}

static bool test_datagram_alternate_restore_failure_commits_delivery(
        struct fixture *fixture) {
    static const byte_t sender_name[] = {
        0, 'r', 'e', 's', 't', 'o', 'r', 'e', '-', 's',
    };
    static const byte_t default_name[] = {
        0, 'r', 'e', 's', 't', 'o', 'r', 'e', '-', 'd',
    };
    static const byte_t alternate_name[] = {
        0, 'r', 'e', 's', 't', 'o', 'r', 'e', '-', 'a',
    };
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    struct guest_unix_address default_address =
            unix_address(default_name, sizeof(default_name));
    struct guest_unix_address alternate_address =
            unix_address(alternate_name, sizeof(alternate_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t default_target = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t alternate = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && default_target >= 0 && alternate >= 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0 &&
            bind_unix_socket(fixture,
                    default_target, &default_address) == 0 &&
            bind_unix_socket(fixture,
                    alternate, &alternate_address) == 0 &&
            connect_unix_socket(fixture,
                    sender, &default_address) == 0 &&
            connect_unix_socket(fixture,
                    default_target, &sender_address) == 0,
            "创建默认与替代 DGRAM 路由恢复失败回归端点");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建路由恢复失败 SCM 探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    const byte_t stale_payload = 0x74;
    CHECK(passed_number >= 0 &&
            send_connected_unix_datagram(fixture, default_target,
                    &stale_payload, sizeof(stale_payload),
                    MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(stale_payload) &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(default_target, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "默认路由预置旧 payload 与 SCM 记录");

    struct socket_ref default_ref;
    CHECK(socket_ref_get_task(&fixture->task,
                    default_target, &default_ref) == 0,
            "取得默认目标 host backing 引用");
    char default_backing[sizeof(
            ((struct sockaddr_un *) 0)->sun_path)];
    strcpy(default_backing,
            default_ref.fd->socket.unix_backing_path);
    CHECK(unlink(default_backing) == 0,
            "移除默认目标 backing 以触发发送后的路由恢复失败");
    socket_ref_release(&default_ref);

    CHECK(prepare_sendmsg_rights_to(
                    passed_number, &alternate_address) &&
            sys_sendmsg(sender, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3,
            "已交付的替代 SCM 在默认路由恢复失败后仍返回成功");
    sdword_t default_error = 0;
    byte_t no_repeated_reset = 0;
    CHECK(socket_ref_get_task(&fixture->task,
                    default_target, &default_ref) == 0 &&
            read_socket_error(&default_ref, &default_error) &&
            default_error == -_ECONNRESET &&
            socket_recvfrom_ref(&default_ref,
                    &no_repeated_reset, sizeof(no_repeated_reset),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "双向旧 peer 只观察一次 ECONNRESET，host reset 不会重复");
    socket_ref_release(&default_ref);
    struct socket_ref sender_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, sender, &sender_ref) == 0,
            "取得恢复失败后的 sender 引用");
    struct sockaddr_storage peer_name;
    dword_t peer_name_length = 0;
    byte_t discarded = 0;
    struct sockaddr_storage host_peer;
    socklen_t host_peer_length = sizeof(host_peer);
    int host_peer_result = getpeername(sender_ref.fd->real_fd,
            (struct sockaddr *) &host_peer, &host_peer_length);
    int host_peer_error = host_peer_result < 0 ? errno : 0;
    CHECK(socket_getname_ref(&sender_ref, true,
                    &peer_name, &peer_name_length) == _ENOTCONN &&
            send_connected_unix_datagram(fixture, sender,
                    &discarded, sizeof(discarded),
                    MSG_NOSIGNAL_) == _ENOTCONN &&
            socket_recvfrom_ref(&sender_ref,
                    &discarded, sizeof(discarded),
                    MSG_DONTWAIT_, NULL) == _EAGAIN &&
            !atomic_load_explicit(
                    &sender_ref.fd->socket.unix_transport_failed,
                    memory_order_acquire) &&
            host_peer_result < 0 &&
            (host_peer_error == ENOTCONN || host_peer_error == EINVAL) &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "恢复失败安全断开 host/guest 路由并清除旧入站队列");

    struct socket_ref alternate_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, alternate, &alternate_ref) == 0,
            "取得恢复失败后的替代目标引用");
    byte_t scm_payload[3] = {0};
    dword_t message_flags = 0;
    struct scm *received_scm = NULL;
    ssize_t received = socket_recvmsg_ref(&alternate_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    socket_ref_release(&alternate_ref);
    CHECK(received == (ssize_t) sizeof(scm_payload) &&
            memcmp(scm_payload, "SCM", sizeof(scm_payload)) == 0 &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "恢复失败前已发送的 host dummy 与内部 SCM 保持配对");
    socket_scm_release(received_scm);
    socket_ref_release(&sender_ref);

    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1 &&
            close_socket(fixture, &alternate) &&
            close_socket(fixture, &default_target) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "恢复失败事务未泄漏 SCM、guest 或 host fd");
    return true;
}

static bool test_datagram_socketpair_retarget_preserves_reverse_peer(
        struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'p', 'a', 'i', 'r', '-', 'r', 'e', 't', 'a', 'r', 'g', 'e', 't',
    };
    static const byte_t old_peer_name[] = {
        0, 'p', 'a', 'i', 'r', '-', 'o', 'l', 'd', '-', 'p', 'e', 'e', 'r',
    };
    static const byte_t failed_name[] = {
        0, 'p', 'a', 'i', 'r', '-', 'f', 'a', 'i', 'l', 'e', 'd',
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address old_peer_address =
            unix_address(old_peer_name, sizeof(old_peer_name));
    struct guest_unix_address failed_address =
            unix_address(failed_name, sizeof(failed_name));
    int host_fd_baseline = open_host_fds();
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t failed = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t pair[2] = {-1, -1};
    CHECK(receiver >= 0 && failed >= 0 &&
            bind_unix_socket(fixture,
                    receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture,
                    failed, &failed_address) == 0 &&
            sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
            "创建 socketpair 改换 peer 回归端点");

    struct socket_ref sender_ref;
    struct socket_ref old_peer_ref;
    struct socket_ref receiver_ref;
    struct socket_ref failed_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, pair[0], &sender_ref) == 0 &&
            socket_ref_get_task(
                    &fixture->task, pair[1], &old_peer_ref) == 0 &&
            socket_ref_get_task(
                    &fixture->task, receiver, &receiver_ref) == 0,
            "取得 socketpair 改换前两端强引用");
    CHECK(socket_ref_get_task(
                    &fixture->task, failed, &failed_ref) == 0,
            "取得 socketpair 失败目标强引用");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建改换 peer 的 SCM 引用探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    const byte_t old_payload = 0x70;
    CHECK(passed_number >= 0 &&
            user_write(USER_SEND_PAYLOAD,
                    &old_payload, sizeof(old_payload)) == 0 &&
            sys_sendto(pair[1], USER_SEND_PAYLOAD,
                    sizeof(old_payload), MSG_NOSIGNAL_, 0, 0) ==
                    (int_t) sizeof(old_payload) &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[1], USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "旧 peer 预置普通数据报与 SCM_RIGHTS");

    CHECK(unlink(failed_ref.fd->socket.unix_backing_path) == 0 &&
            connect_unix_socket(fixture,
                    pair[0], &failed_address) == _ECONNREFUSED &&
            sender_ref.fd->socket.unix_dgram_peer == old_peer_ref.fd &&
            old_peer_ref.fd->socket.unix_dgram_peer == sender_ref.fd,
            "失败 retarget 保留双向关系并为旧 peer 建立 hidden transport");
    const byte_t anonymous_before_bind = 0x6f;
    CHECK(send_unix_datagram(fixture, pair[1], &receiver_address,
                    &anonymous_before_bind,
                    sizeof(anonymous_before_bind)) ==
                    (ssize_t) sizeof(anonymous_before_bind),
            "hidden transport 在 guest bind 前排入匿名来源记录");
    CHECK(bind_unix_socket(fixture,
                    pair[1], &old_peer_address) == 0,
            "hidden transport 不阻止后续 guest bind");
    struct socket_address bound_peer_name = {0};
    dword_t bound_peer_length = 0;
    CHECK(socket_getname_ref(&sender_ref, true,
                    &bound_peer_name.storage,
                    &bound_peer_length) == 0,
            "socketpair peer 后 bind 可重新读取对端名称");
    bound_peer_name.length = (socklen_t) bound_peer_length;
    CHECK(source_equals(&bound_peer_name,
                    old_peer_name, sizeof(old_peer_name)),
            "socketpair peer 后 bind 会刷新存活对端的 getpeername");
    struct socket_address observed_source = {0};
    byte_t observed_payload = 0;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &observed_payload, sizeof(observed_payload),
                    &observed_source) ==
                    (ssize_t) sizeof(observed_payload) &&
            observed_payload == anonymous_before_bind &&
            observed_source.length == offsetof(struct sockaddr_, data) &&
            ((struct sockaddr_ *) &observed_source.storage)->family ==
                    PF_LOCAL_,
            "bind 前已排队记录继续报告 unnamed 源地址");
    const byte_t named_after_bind = 0x70;
    CHECK(send_unix_datagram(fixture, pair[1], &receiver_address,
                    &named_after_bind, sizeof(named_after_bind)) ==
                    (ssize_t) sizeof(named_after_bind) &&
            receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &observed_payload, sizeof(observed_payload),
                    &observed_source) ==
                    (ssize_t) sizeof(observed_payload) &&
            observed_payload == named_after_bind &&
            source_equals(&observed_source,
                    old_peer_name, sizeof(old_peer_name)),
            "bind 后新记录报告当前 guest 源名称");

    CHECK(connect_unix_socket(fixture,
                    pair[0], &receiver_address) == 0 &&
            sender_ref.fd->socket.unix_dgram_peer == receiver_ref.fd &&
            old_peer_ref.fd->socket.unix_dgram_peer == sender_ref.fd &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "DGRAM connect 清旧队列并只迁移发起端有向 peer");
    byte_t discarded = 0;
    CHECK(socket_recvfrom_ref(&sender_ref,
                    &discarded, sizeof(discarded),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "改换 peer 后不会读到旧连接排队记录");
    struct socket_option_result error_result;
    sdword_t old_peer_error = 0;
    CHECK(socket_getsockopt_ref(&old_peer_ref,
                    SOL_SOCKET_, SO_ERROR_, sizeof(old_peer_error),
                    SOCKET_GUEST_I386, &error_result) == 0 &&
            error_result.length == sizeof(old_peer_error),
            "读取旧双向 peer 的重连错误");
    memcpy(&old_peer_error,
            error_result.value, sizeof(old_peer_error));
    CHECK(old_peer_error == -_ECONNRESET,
            "丢弃旧入站记录后旧双向 peer 收到一次 ECONNRESET");

    struct sockaddr_storage reverse_peer_name;
    dword_t reverse_peer_length = 0;
    const byte_t reverse_rejected = 0x71;
    CHECK(socket_getname_ref(&old_peer_ref, true,
                    &reverse_peer_name, &reverse_peer_length) == 0 &&
            send_connected_unix_datagram(fixture, pair[1],
                    &reverse_rejected, sizeof(reverse_rejected),
                    MSG_NOSIGNAL_) == _EPERM,
            "旧端保留 B 到 A 路由但因 A 已认可新 peer 返回 EPERM");

    const byte_t explicit_old_peer = 0x72;
    ssize_t explicit_result = send_unix_datagram(fixture, pair[0],
            &old_peer_address, &explicit_old_peer,
            sizeof(explicit_old_peer));
    ssize_t explicit_received = receive_unix_datagram(fixture, pair[1],
            MSG_DONTWAIT_, &discarded, sizeof(discarded), NULL);
    if (explicit_result != (ssize_t) sizeof(explicit_old_peer) ||
            explicit_received != (ssize_t) sizeof(discarded))
        fprintf(stderr, "socketpair 反向显式发送=%zd，接收=%zd\n",
                explicit_result, explicit_received);
    CHECK(explicit_result == (ssize_t) sizeof(explicit_old_peer) &&
            explicit_received == (ssize_t) sizeof(discarded) &&
            discarded == explicit_old_peer,
            "A 显式发 B 仍通过 B 到 A 的反向授权");

    const byte_t payload = 0x73;
    CHECK(user_write(USER_SEND_PAYLOAD,
                    &payload, sizeof(payload)) == 0 &&
            sys_sendto(pair[0], USER_SEND_PAYLOAD,
                    sizeof(payload), MSG_NOSIGNAL_, 0, 0) ==
                    (int_t) sizeof(payload),
            "改换 peer 后无地址发送指向新端");
    byte_t received_payload = 0;
    CHECK(receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &received_payload, sizeof(received_payload), NULL) ==
                    (ssize_t) sizeof(received_payload) &&
            received_payload == payload,
            "普通 payload 只送达新 peer");

    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(pair[0], USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3,
            "改换 peer 后发送 SCM_RIGHTS");

    byte_t scm_payload[3] = {0};
    dword_t message_flags = 0;
    struct scm *received_scm = NULL;
    ssize_t received = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), MSG_DONTWAIT_,
            NULL, &message_flags, &received_scm);
    CHECK(received == 3 &&
            memcmp(scm_payload, "SCM", sizeof(scm_payload)) == 0 &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "SCM 内部队列与 host payload 一同指向新 peer");
    socket_scm_release(received_scm);
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            f_close_task(&fixture->task, passed_number) == 0,
            "改换 peer 的 SCM 引用完整归还");

    socket_ref_release(&failed_ref);
    socket_ref_release(&receiver_ref);
    socket_ref_release(&old_peer_ref);
    socket_ref_release(&sender_ref);
    CHECK(close_socket(fixture, &pair[1]) &&
            close_socket(fixture, &pair[0]) &&
            close_socket(fixture, &failed) &&
            close_socket(fixture, &receiver) &&
            open_host_fds() == host_fd_baseline,
            "socketpair 改换 peer 回归未泄漏 guest 或 host fd");
    return true;
}

static bool test_datagram_disconnect_resets_route_state(
        struct fixture *fixture) {
    static const byte_t sender_name[] = {
        0, 'd', 'i', 's', 'c', '-', 's', 'e', 'n', 'd', 'e', 'r',
    };
    static const byte_t old_name[] = {
        0, 'd', 'i', 's', 'c', '-', 'o', 'l', 'd',
    };
    static const byte_t new_name[] = {
        0, 'd', 'i', 's', 'c', '-', 'n', 'e', 'w',
    };
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    struct guest_unix_address old_address =
            unix_address(old_name, sizeof(old_name));
    struct guest_unix_address new_address =
            unix_address(new_name, sizeof(new_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t old_peer = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t new_peer = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && old_peer >= 0 && new_peer >= 0 &&
            bind_unix_socket(fixture,
                    sender, &sender_address) == 0 &&
            bind_unix_socket(fixture,
                    old_peer, &old_address) == 0 &&
            bind_unix_socket(fixture,
                    new_peer, &new_address) == 0 &&
            connect_unix_socket(fixture,
                    sender, &old_address) == 0 &&
            connect_unix_socket(fixture,
                    old_peer, &sender_address) == 0,
            "创建双向具名 DGRAM 连接");

    struct socket_ref sender_ref;
    struct socket_ref old_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, sender, &sender_ref) == 0 &&
            socket_ref_get_task(
                    &fixture->task, old_peer, &old_ref) == 0,
            "取得 DGRAM disconnect 两端强引用");
    const byte_t stale = 0x73;
    CHECK(user_write(USER_SEND_PAYLOAD, &stale, sizeof(stale)) == 0 &&
            sys_sendto(old_peer, USER_SEND_PAYLOAD,
                    sizeof(stale), MSG_NOSIGNAL_, 0, 0) ==
                    (int_t) sizeof(stale),
            "disconnect 前排入旧双向 peer 数据报");

    uint16_t unspecified_family = 0;
    CHECK(socket_connect_ref_task(&fixture->task, &sender_ref,
                    &unspecified_family,
                    sizeof(unspecified_family)) == 0 &&
            !sender_ref.fd->socket.unix_peer_name_valid,
            "AF_UNSPEC disconnect 清除 guest 路由状态");
    byte_t discarded = 0;
    CHECK(socket_recvfrom_ref(&sender_ref,
                    &discarded, sizeof(discarded),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "disconnect 丢弃旧连接入站队列");
    struct sockaddr_storage peer_name;
    dword_t peer_name_length = 0;
    CHECK(socket_getname_ref(&sender_ref, true,
                    &peer_name, &peer_name_length) == _ENOTCONN,
            "disconnect 后 getpeername 返回 ENOTCONN");

    struct socket_option_result error_result;
    sdword_t old_peer_error = 0;
    CHECK(socket_getsockopt_ref(&old_ref,
                    SOL_SOCKET_, SO_ERROR_, sizeof(old_peer_error),
                    SOCKET_GUEST_I386, &error_result) == 0 &&
            error_result.length == sizeof(old_peer_error),
            "读取具名旧 peer 的 disconnect 错误");
    memcpy(&old_peer_error,
            error_result.value, sizeof(old_peer_error));
    CHECK(old_peer_error == -_ECONNRESET,
            "双向具名旧 peer 收到一次 ECONNRESET");

    const byte_t queued_while_unconnected = 0x74;
    CHECK(send_unix_datagram(fixture, new_peer, &sender_address,
                    &queued_while_unconnected,
                    sizeof(queued_while_unconnected)) ==
                    (ssize_t) sizeof(queued_while_unconnected) &&
            connect_unix_socket(fixture,
                    sender, &new_address) == 0,
            "断开后的首次 connect 建立新路由");
    byte_t preserved = 0;
    CHECK(receive_unix_datagram(fixture, sender, MSG_DONTWAIT_,
                    &preserved, sizeof(preserved), NULL) ==
                    (ssize_t) sizeof(preserved) &&
            preserved == queued_while_unconnected,
            "首次 connect 不误清未连接状态下的入站队列");

    const byte_t outbound = 0x75;
    CHECK(user_write(USER_SEND_PAYLOAD,
                    &outbound, sizeof(outbound)) == 0 &&
            sys_sendto(sender, USER_SEND_PAYLOAD,
                    sizeof(outbound), MSG_NOSIGNAL_, 0, 0) ==
                    (int_t) sizeof(outbound),
            "disconnect 后可重新建立无地址发送路由");
    byte_t delivered = 0;
    CHECK(receive_unix_datagram(fixture, new_peer, MSG_DONTWAIT_,
                    &delivered, sizeof(delivered), NULL) ==
                    (ssize_t) sizeof(delivered) &&
            delivered == outbound,
            "重新连接后的 payload 只送达新 peer");

    socket_ref_release(&old_ref);
    socket_ref_release(&sender_ref);
    CHECK(close_socket(fixture, &new_peer) &&
            close_socket(fixture, &old_peer) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "DGRAM disconnect 回归未泄漏 guest 或 host fd");
    return true;
}

static bool test_datagram_drain_continues_after_reset(
        struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'd', 'r', 'a', 'i', 'n', '-', 'r', 'e', 's', 'e', 't', '-', 'r',
    };
    static const byte_t old_name[] = {
        0, 'd', 'r', 'a', 'i', 'n', '-', 'r', 'e', 's', 'e', 't', '-', 'o',
    };
    static const byte_t new_name[] = {
        0, 'd', 'r', 'a', 'i', 'n', '-', 'r', 'e', 's', 'e', 't', '-', 'n',
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address old_address =
            unix_address(old_name, sizeof(old_name));
    struct guest_unix_address new_address =
            unix_address(new_name, sizeof(new_name));
    int host_fd_baseline = open_host_fds();
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t old_peer = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t new_peer = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && old_peer >= 0 && new_peer >= 0 &&
            bind_unix_socket(fixture,
                    receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture,
                    old_peer, &old_address) == 0 &&
            bind_unix_socket(fixture,
                    new_peer, &new_address) == 0 &&
            connect_unix_socket(fixture,
                    receiver, &old_address) == 0 &&
            connect_unix_socket(fixture,
                    old_peer, &receiver_address) == 0,
            "创建 reset 后继续排空的数据报端点");

    struct socket_ref receiver_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, receiver, &receiver_ref) == 0,
            "取得 reset 排空接收端强引用");
    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 reset 排空 SCM 探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    CHECK(passed_number >= 0 &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(old_peer, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "旧路由预置一条 SCM 数据报");

    struct drain_recvmsg_probe probe = {0};
    socket_set_unix_drain_recvmsg_test_hook(
            inject_drain_reset_once, &probe);
    int reconnect_error = connect_unix_socket(
            fixture, receiver, &new_address);
    socket_set_unix_drain_recvmsg_test_hook(NULL, NULL);
    CHECK(reconnect_error == 0 && probe.reset_injected &&
            probe.calls >= 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "清除 reset 后继续消费旧 host 记录与 SCM");

    byte_t payload = 0;
    CHECK(socket_recvfrom_ref(&receiver_ref,
                    &payload, sizeof(payload),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "reset 不会让旧记录越过改换路由事务");
    const byte_t delivered = 0x5a;
    CHECK(send_unix_datagram(fixture, new_peer, &receiver_address,
                    &delivered, sizeof(delivered)) ==
                    (ssize_t) sizeof(delivered) &&
            socket_recvfrom_ref(&receiver_ref,
                    &payload, sizeof(payload),
                    MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(payload) &&
            payload == delivered,
            "排空 reset 后的新路由仍可收取数据报");

    socket_ref_release(&receiver_ref);
    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1 &&
            close_socket(fixture, &new_peer) &&
            close_socket(fixture, &old_peer) &&
            close_socket(fixture, &receiver) &&
            open_host_fds() == host_fd_baseline,
            "reset 排空回归未泄漏 SCM、guest 或 host fd");
    return true;
}

static bool test_datagram_self_retarget_does_not_reset(
        struct fixture *fixture) {
    static const byte_t self_name[] = {
        0, 's', 'e', 'l', 'f', '-', 'r', 'o', 'u', 't', 'e',
    };
    static const byte_t target_name[] = {
        0, 's', 'e', 'l', 'f', '-', 't', 'a', 'r', 'g', 'e', 't',
    };
    struct guest_unix_address self_address =
            unix_address(self_name, sizeof(self_name));
    struct guest_unix_address target_address =
            unix_address(target_name, sizeof(target_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t target = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && target >= 0 &&
            bind_unix_socket(fixture,
                    sender, &self_address) == 0 &&
            bind_unix_socket(fixture,
                    target, &target_address) == 0 &&
            connect_unix_socket(fixture,
                    sender, &self_address) == 0,
            "创建 self-connected DGRAM 路由");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建 self-retarget SCM 探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    const byte_t ordinary = 0x31;
    CHECK(passed_number >= 0 &&
            send_connected_unix_datagram(fixture, sender,
                    &ordinary, sizeof(ordinary), MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(ordinary) &&
            send_connected_unix_datagram(fixture, sender,
                    NULL, 0, MSG_NOSIGNAL_) == 0 &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(sender, USER_SEND_HEADER,
                    MSG_NOSIGNAL_) == 3 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "self-connected 队列包含普通、零长与 SCM 数据报");

    CHECK(connect_unix_socket(fixture,
                    sender, &target_address) == 0 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "self retarget 清理旧队列与 SCM");
    struct socket_ref sender_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, sender, &sender_ref) == 0,
            "取得 self-retarget sender 引用");
    sdword_t socket_error = -1;
    byte_t discarded = 0;
    CHECK(read_socket_error(&sender_ref, &socket_error) &&
            socket_error == 0 &&
            socket_recvfrom_ref(&sender_ref,
                    &discarded, sizeof(discarded),
                    MSG_DONTWAIT_, NULL) == _EAGAIN,
            "self retarget 不给自身注入 ECONNRESET 且旧队列为空");

    const byte_t delivered = 0x32;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &delivered, sizeof(delivered), MSG_NOSIGNAL_) ==
                    (ssize_t) sizeof(delivered) &&
            receive_unix_datagram(fixture, target, MSG_DONTWAIT_,
                    &discarded, sizeof(discarded), NULL) ==
                    (ssize_t) sizeof(discarded) &&
            discarded == delivered,
            "self retarget 后新无地址路由可正常交付");
    socket_ref_release(&sender_ref);

    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1 &&
            close_socket(fixture, &target) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "self-retarget 回归未泄漏 SCM、guest 或 host fd");
    return true;
}

static bool test_datagram_dead_route_disconnects_host(
        struct fixture *fixture) {
    static const byte_t sender_name[] = {
        0, 'd', 'e', 'a', 'd', '-', 'r', 'o', 'u', 't', 'e', '-', 's',
    };
    static const byte_t peer_name[] = {
        0, 'd', 'e', 'a', 'd', '-', 'r', 'o', 'u', 't', 'e', '-', 'p',
    };
    static const byte_t third_name[] = {
        0, 'd', 'e', 'a', 'd', '-', 'r', 'o', 'u', 't', 'e', '-', 't',
    };
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    struct guest_unix_address peer_address =
            unix_address(peer_name, sizeof(peer_name));
    struct guest_unix_address third_address =
            unix_address(third_name, sizeof(third_name));
    int host_fd_baseline = open_host_fds();
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t peer = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t third = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(sender >= 0 && peer >= 0 && third >= 0 &&
            bind_unix_socket(fixture,
                    sender, &sender_address) == 0 &&
            bind_unix_socket(fixture,
                    peer, &peer_address) == 0 &&
            bind_unix_socket(fixture,
                    third, &third_address) == 0 &&
            connect_unix_socket(fixture,
                    sender, &peer_address) == 0,
            "创建具名 dead-route 数据报端点");
    struct socket_ref sender_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, sender, &sender_ref) == 0,
            "取得 dead-route sender 引用");
    CHECK(close_socket(fixture, &peer),
            "最终关闭具名 DGRAM target");

    const byte_t payload = 0x64;
    CHECK(send_unix_datagram(fixture, third, &sender_address,
                    &payload, sizeof(payload)) == _EPERM,
            "dead-route tombstone 消费前继续拒绝第三方来源");
    int events = sender_ref.fd->ops->poll(sender_ref.fd);
    for (unsigned iteration = 0; iteration < 64; iteration++)
        events |= sender_ref.fd->ops->poll(sender_ref.fd);
    sdword_t socket_error = -1;
    struct sockaddr_storage peer_result;
    dword_t peer_length = 0;
    CHECK((events & (POLL_READ | POLL_ERR | POLL_HUP)) == 0 &&
            read_socket_error(&sender_ref, &socket_error) &&
            socket_error == 0 &&
            socket_getname_ref(&sender_ref, true,
                    &peer_result, &peer_length) == 0,
            "peer close reset 被一次清理且不伪造 poll/SO_ERROR/断连");

    CHECK(send_connected_unix_datagram(fixture, sender,
                    &payload, sizeof(payload), MSG_NOSIGNAL_) ==
                    _ECONNREFUSED &&
            socket_getname_ref(&sender_ref, true,
                    &peer_result, &peer_length) == _ENOTCONN &&
            send_connected_unix_datagram(fixture, sender,
                    &payload, sizeof(payload), MSG_NOSIGNAL_) ==
                    _ENOTCONN,
            "首次 dead-peer send 消费 ECONNREFUSED 并提交 guest disconnect");
    CHECK(send_unix_datagram(fixture, third, &sender_address,
                    &payload, sizeof(payload)) ==
                    (ssize_t) sizeof(payload),
            "guest disconnect 后 host 路由也允许第三方来源");
    byte_t received = 0;
    CHECK(socket_recvfrom_ref(&sender_ref,
                    &received, sizeof(received), MSG_DONTWAIT_, NULL) ==
                    (ssize_t) sizeof(received) &&
            received == payload,
            "AF_UNSPEC host disconnect 后第三方数据报可读取");
    socket_ref_release(&sender_ref);

    CHECK(close_socket(fixture, &third) &&
            close_socket(fixture, &sender) &&
            open_host_fds() == host_fd_baseline,
            "dead-route 回归未泄漏 guest 或 host fd");
    return true;
}

static bool test_unconnected_shutdown_preserves_queue(
        struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 's', 'h', 'u', 't', '-', 'u', 'n', '-', 'r', 'x',
    };
    static const byte_t sender_name[] = {
        0, 's', 'h', 'u', 't', '-', 'u', 'n', '-', 't', 'x',
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0 &&
            connect_unix_socket(fixture, sender, &receiver_address) == 0,
            "创建接收端未连接的 shutdown 数据报队列");

    fixture->scm_probe.close_calls = 0;
    struct fd *passed = fd_create(&scm_probe_ops);
    CHECK(passed != NULL, "创建未连接 shutdown SCM 探针");
    passed->data = fixture;
    fd_t passed_number = f_install_task(&fixture->task, passed, 0);
    CHECK(passed_number >= 0, "安装未连接 shutdown SCM 探针");
    const byte_t ordinary = 0x51;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &ordinary, sizeof(ordinary), 0) == sizeof(ordinary),
            "未连接接收端预先排入普通数据报");
    CHECK(prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(sender, USER_SEND_HEADER, 0) == 3,
            "未连接接收端预先排入 SCM 数据报");

    CHECK(sys_shutdown(receiver, SHUT_RD) == 0 &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "未连接数据报 SHUT_RD 成功且保留队列与 SCM 引用");

    const byte_t rejected = 0x62;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &rejected, sizeof(rejected), MSG_NOSIGNAL_) == _EPIPE,
            "未连接接收端 SHUT_RD 后的新发送返回 EPIPE");

    byte_t received = 0;
    CHECK(receive_unix_datagram(fixture, receiver, 0,
                    &received, sizeof(received), NULL) == sizeof(received) &&
            received == ordinary,
            "未连接 SHUT_RD 后普通数据报仍可读取");
    struct socket_ref receiver_ref;
    CHECK(socket_ref_get_task(
                    &fixture->task, receiver, &receiver_ref) == 0,
            "取得未连接 shutdown 接收端强引用");
    byte_t scm_payload[3] = {0};
    dword_t message_flags = 0;
    struct scm *received_scm = NULL;
    ssize_t received_length = socket_recvmsg_ref(&receiver_ref,
            scm_payload, sizeof(scm_payload), 0, NULL,
            &message_flags, &received_scm);
    socket_ref_release(&receiver_ref);
    CHECK(received_length == sizeof(scm_payload) &&
            memcmp(scm_payload, "SCM", sizeof(scm_payload)) == 0 &&
            received_scm != NULL && received_scm->num_fds == 1 &&
            received_scm->fds[0] == passed,
            "未连接 SHUT_RD 后 SCM 数据报仍保持配对");
    socket_scm_release(received_scm);
    byte_t empty = 0;
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            receive_unix_datagram(fixture, receiver, 0,
                    &empty, sizeof(empty), NULL) == 0 &&
            receive_unix_datagram(fixture, receiver, MSG_DONTWAIT_,
                    &empty, sizeof(empty), NULL) == _EAGAIN &&
            f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "未连接 SHUT_RD 队列耗尽后返回 EOF/EAGAIN 并精确释放 SCM");
    CHECK(close_socket(fixture, &sender) &&
            close_socket(fixture, &receiver),
            "清理未连接 shutdown 数据报两端");
    return true;
}

static bool test_send_shutdown_atomicity(struct fixture *fixture) {
    fixture->scm_probe.close_calls = 0;
    for (unsigned iteration = 0;
            iteration < SHUTDOWN_RACE_ITERATIONS; iteration++) {
        fd_t pair[2] = {-1, -1};
        CHECK(sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                        USER_SOCKETPAIR) == 0 &&
                user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0,
                "并发轮次创建 AF_UNIX DGRAM socketpair");

        struct fd *passed = fd_create(&scm_probe_ops);
        CHECK(passed != NULL, "并发轮次创建 SCM 引用探针");
        passed->data = fixture;
        fd_t passed_number = f_install_task(
                &fixture->task, passed, 0);
        CHECK(passed_number >= 0, "并发轮次安装 SCM 引用探针");

        struct shutdown_send_call call = {
            .fixture = fixture,
        };
        atomic_init(&call.ready, false);
        atomic_init(&call.start, false);
        CHECK(socket_ref_get_task(
                        &fixture->task, pair[0], &call.sender) == 0 &&
                socket_scm_create_task(&fixture->task,
                        &passed_number, 1, &call.scm) == 0,
                "并发轮次准备发送端与 SCM 容器");
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL,
                        send_during_shutdown, &call) == 0,
                "并发轮次启动发送线程");
        while (!atomic_load_explicit(
                &call.ready, memory_order_acquire))
            sched_yield();
        atomic_store_explicit(&call.start, true, memory_order_release);
        if ((iteration & 1) != 0)
            sched_yield();
        dword_t how = (iteration & 2) != 0 ? SHUT_RDWR : SHUT_RD;
        int_t shutdown_result = sys_shutdown(pair[1], how);
        CHECK(pthread_join(thread, NULL) == 0,
                "并发轮次回收发送线程");
        socket_ref_release(&call.sender);

        CHECK(shutdown_result == 0 &&
                (call.result == 1 || call.result == _EPIPE) &&
                call.scm == NULL,
                "并发 send 与 SHUT_RD 只产生完整发送或 EPIPE");

        struct socket_ref receiver_ref;
        CHECK(socket_ref_get_task(
                        &fixture->task, pair[1], &receiver_ref) == 0,
                "并发轮次取得接收端强引用");
        byte_t received_payload = 0;
        dword_t message_flags = 0;
        struct scm *received_scm = NULL;
        ssize_t received_length = socket_recvmsg_ref(&receiver_ref,
                &received_payload, sizeof(received_payload),
                MSG_DONTWAIT_, NULL, &message_flags, &received_scm);
        socket_ref_release(&receiver_ref);
        unsigned references_before_release = atomic_load_explicit(
                &passed->refcount, memory_order_relaxed);
        bool complete_delivery = call.result == 1 &&
                received_length == 1 && received_payload == 0x73 &&
                received_scm != NULL && received_scm->num_fds == 1 &&
                received_scm->fds[0] == passed &&
                references_before_release == 2;
        bool rejected_cleanly = call.result == _EPIPE &&
                received_length == _EAGAIN && received_scm == NULL &&
                references_before_release == 1;
        if (received_scm != NULL)
            socket_scm_release(received_scm);
        CHECK(complete_delivery || rejected_cleanly,
                "并发 send 成功则完整入队 payload/SCM，失败则不入队");

        byte_t unexpected = 0;
        CHECK(atomic_load_explicit(
                        &passed->refcount, memory_order_relaxed) == 1 &&
                receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                        &unexpected, sizeof(unexpected), NULL) == _EAGAIN,
                "并发轮次消费唯一报文后队列为空且 SCM 无泄漏");
        CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
                fixture->scm_probe.close_calls == iteration + 1 &&
                close_socket(fixture, &pair[1]) &&
                close_socket(fixture, &pair[0]),
                "并发轮次关闭已 shutdown socket 与探针");
    }
    return true;
}

static bool test_blocked_datagram_send_round(
        struct fixture *fixture, unsigned round, int host_fd_baseline) {
    fd_t pair[2] = {-1, -1};
    struct socket_ref sender_ref = {0};
    struct socket_ref receiver_ref = {0};
    bool sender_retained = false;
    bool receiver_retained = false;
    int sender_host_fd = -1;
    int receiver_host_fd = -1;
    int original_status = -1;
    bool pair_created = false;
    bool buffer_configured = false;
    bool queue_full = false;
    bool blocking_restored = false;
    unsigned filler_messages = 0;
    int fill_error = 0;
    struct fd *passed = NULL;
    fd_t passed_number = -1;
    bool rights_prepared = false;
    pthread_t thread;
    bool thread_started = false;
    bool thread_joined = false;
    bool started_observed = false;
    bool waiting_observed = false;
    bool stayed_blocked = false;
    unsigned references_during_wait = 0;
    int_t shutdown_result = _EIO;
    bool woke_promptly = false;
    bool registration_cleared = false;
    unsigned references_after_wake = 0;
    bool queue_preserved = false;
    const byte_t filler = 0x6b;
    const int buffer_size = BLOCKED_SEND_BUFFER_SIZE;
    const struct timeval send_timeout = {
        .tv_sec = BLOCKED_SEND_TIMEOUT_SECONDS,
    };
    struct blocking_send_call call = {
        .fixture = fixture,
        .result = _EIO,
    };
    atomic_init(&call.started, false);
    atomic_init(&call.finished, false);

    pair_created = sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0;
    if (!pair_created)
        goto cleanup;
    sender_retained = socket_ref_get_task(
            &fixture->task, pair[0], &sender_ref) == 0;
    receiver_retained = socket_ref_get_task(
            &fixture->task, pair[1], &receiver_ref) == 0;
    if (!sender_retained || !receiver_retained)
        goto cleanup;
    sender_host_fd = sender_ref.fd->real_fd;
    receiver_host_fd = receiver_ref.fd->real_fd;

    original_status = fcntl(sender_host_fd, F_GETFL);
    buffer_configured = original_status >= 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(receiver_host_fd, SOL_SOCKET, SO_RCVBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDTIMEO,
                    &send_timeout, sizeof(send_timeout)) == 0 &&
            fcntl(sender_host_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0;
    queue_full = buffer_configured && fill_host_datagram_queue(
            sender_host_fd, filler, &filler_messages, &fill_error);
    blocking_restored = original_status >= 0 &&
            fcntl(sender_host_fd, F_SETFL, original_status) == 0;
    if (!buffer_configured || !queue_full || !blocking_restored)
        goto cleanup;

    passed = fd_create(&scm_probe_ops);
    if (passed == NULL)
        goto cleanup;
    passed->data = fixture;
    passed_number = f_install_task(&fixture->task, passed, 0);
    if (passed_number < 0)
        goto cleanup;
    call.sender = pair[0];
    rights_prepared = prepare_sendmsg_rights(passed_number);
    if (!rights_prepared)
        goto cleanup;

    thread_started = pthread_create(
            &thread, NULL, sendmsg_while_blocked, &call) == 0;
    if (!thread_started)
        goto cleanup;
    started_observed = wait_for_flag(
            &call.started, BLOCKED_SEND_WAIT_MS);
    waiting_observed = started_observed && wait_for_poll_registration(
            &fixture->task, BLOCKED_SEND_WAIT_MS);
    const struct timespec stable_wait = {.tv_nsec = 10000000};
    if (waiting_observed)
        nanosleep(&stable_wait, NULL);
    stayed_blocked = waiting_observed &&
            poll_registration_matches(&fixture->task, true) &&
            !atomic_load_explicit(&call.finished, memory_order_acquire);
    references_during_wait = atomic_load_explicit(
            &passed->refcount, memory_order_relaxed);

    shutdown_result = sys_shutdown(pair[1], SHUT_RD);
    woke_promptly = wait_for_flag(
            &call.finished, BLOCKED_SEND_WAIT_MS);
    bool completed = woke_promptly || wait_for_flag(&call.finished,
            BLOCKED_SEND_TIMEOUT_SECONDS * 1000 + BLOCKED_SEND_WAIT_MS);
    if (!completed) {
        (void) shutdown(sender_host_fd, SHUT_RDWR);
        (void) shutdown(receiver_host_fd, SHUT_RDWR);
        completed = wait_for_flag(
                &call.finished, BLOCKED_SEND_WAIT_MS);
    }
    if (!completed) {
        fprintf(stderr,
                "Unix socket 地址测试失败：阻塞数据报发送线程救援后仍未退出\n");
        fflush(stderr);
        _Exit(1);
    }
    thread_joined = pthread_join(thread, NULL) == 0;
    registration_cleared = poll_registration_matches(
            &fixture->task, false);
    references_after_wake = atomic_load_explicit(
            &passed->refcount, memory_order_relaxed);

    byte_t first_peek = 0;
    byte_t second_peek = 0;
    queue_preserved = receive_unix_datagram(fixture, pair[1],
                    MSG_PEEK_ | MSG_DONTWAIT_,
                    &first_peek, sizeof(first_peek), NULL) == 1 &&
            receive_unix_datagram(fixture, pair[1],
                    MSG_PEEK_ | MSG_DONTWAIT_,
                    &second_peek, sizeof(second_peek), NULL) == 1 &&
            first_peek == filler && second_peek == filler;

cleanup:
    if (thread_started && !thread_joined) {
        (void) sys_shutdown(pair[1], SHUT_RD);
        bool completed = wait_for_flag(
                &call.finished, BLOCKED_SEND_WAIT_MS);
        if (!completed && sender_host_fd >= 0) {
            (void) shutdown(sender_host_fd, SHUT_RDWR);
            completed = wait_for_flag(
                    &call.finished, BLOCKED_SEND_WAIT_MS);
        }
        if (!completed) {
            fprintf(stderr,
                    "Unix socket 地址测试失败：清理阻塞数据报发送线程超时\n");
            fflush(stderr);
            _Exit(1);
        }
        thread_joined = pthread_join(thread, NULL) == 0;
    }

    bool probe_closed = false;
    if (passed_number >= 0) {
        probe_closed = f_close_task(
                &fixture->task, passed_number) == 0;
    } else if (passed != NULL) {
        probe_closed = fd_close(passed) == 0;
    }
    if (receiver_retained)
        socket_ref_release(&receiver_ref);
    if (sender_retained)
        socket_ref_release(&sender_ref);
    bool receiver_closed = close_socket(fixture, &pair[1]);
    bool sender_closed = close_socket(fixture, &pair[0]);

    errno = 0;
    bool receiver_host_closed = receiver_host_fd < 0 ||
            (fcntl(receiver_host_fd, F_GETFD) < 0 && errno == EBADF);
    errno = 0;
    bool sender_host_closed = sender_host_fd < 0 ||
            (fcntl(sender_host_fd, F_GETFD) < 0 && errno == EBADF);
    int host_fds_after = open_host_fds();
    bool passed_round = pair_created && sender_retained &&
            receiver_retained && buffer_configured && queue_full &&
            filler_messages != 0 && blocking_restored &&
            passed != NULL && rights_prepared && thread_started &&
            started_observed && waiting_observed && stayed_blocked &&
            references_during_wait == 2 && shutdown_result == 0 &&
            woke_promptly && thread_joined && call.result == _EPIPE &&
            registration_cleared && references_after_wake == 1 &&
            queue_preserved && probe_closed && receiver_closed &&
            sender_closed && receiver_host_closed && sender_host_closed &&
            fixture->scm_probe.close_calls == round + 1 &&
            host_fds_after == host_fd_baseline;
    if (!passed_round) {
        fprintf(stderr,
                "阻塞 DGRAM send 回归失败轮次 %u：填充=%d(%u, errno=%d)，"
                "等待=%d，唤醒=%d，结果=%d，引用=%u/%u，host fd=%d/%d\n",
                round, queue_full, filler_messages, fill_error,
                waiting_observed, woke_promptly, call.result,
                references_during_wait, references_after_wake,
                host_fd_baseline, host_fds_after);
    }
    return passed_round;
}

static bool test_blocked_datagram_send_wakes_on_shutdown(
        struct fixture *fixture) {
    int host_fd_baseline = open_host_fds();
    fixture->scm_probe.close_calls = 0;
    for (unsigned round = 0; round < BLOCKED_SEND_ROUNDS; round++) {
        if (!test_blocked_datagram_send_round(
                fixture, round, host_fd_baseline))
            return false;
    }
    CHECK(fixture->scm_probe.close_calls == BLOCKED_SEND_ROUNDS &&
            open_host_fds() == host_fd_baseline,
            "阻塞 DGRAM send 多轮完成后 SCM 与 host fd 回到基线");
    return true;
}

static bool test_blocked_datagram_send_peer_close_round(
        struct fixture *fixture, unsigned round,
        unsigned close_calls_before, int host_fd_baseline) {
    fd_t pair[2] = {-1, -1};
    struct socket_ref sender_ref = {0};
    struct socket_ref receiver_ref = {0};
    bool sender_retained = false;
    bool receiver_retained = false;
    int sender_host_fd = -1;
    int receiver_host_fd = -1;
    int original_status = -1;
    bool pair_created = false;
    bool buffer_configured = false;
    bool queue_full = false;
    bool blocking_restored = false;
    unsigned filler_messages = 0;
    int fill_error = 0;
    struct fd *passed = NULL;
    fd_t passed_number = -1;
    bool rights_prepared = false;
    pthread_t thread;
    bool thread_started = false;
    bool thread_joined = false;
    bool started_observed = false;
    bool waiting_observed = false;
    bool stayed_blocked = false;
    unsigned references_during_wait = 0;
    bool receiver_closed = false;
    bool woke_promptly = false;
    bool registration_cleared = false;
    unsigned references_after_wake = 0;
    bool peer_state_cleared = false;
    const byte_t filler = 0x6b;
    const int buffer_size = BLOCKED_SEND_BUFFER_SIZE;
    const struct timeval send_timeout = {
        .tv_sec = BLOCKED_SEND_TIMEOUT_SECONDS,
    };
    struct blocking_send_call call = {
        .fixture = fixture,
        .result = _EIO,
    };
    atomic_init(&call.started, false);
    atomic_init(&call.finished, false);

    pair_created = sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0;
    if (!pair_created)
        goto cleanup;
    sender_retained = socket_ref_get_task(
            &fixture->task, pair[0], &sender_ref) == 0;
    receiver_retained = socket_ref_get_task(
            &fixture->task, pair[1], &receiver_ref) == 0;
    if (!sender_retained || !receiver_retained)
        goto cleanup;
    sender_host_fd = sender_ref.fd->real_fd;
    receiver_host_fd = receiver_ref.fd->real_fd;

    original_status = fcntl(sender_host_fd, F_GETFL);
    buffer_configured = original_status >= 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(receiver_host_fd, SOL_SOCKET, SO_RCVBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDTIMEO,
                    &send_timeout, sizeof(send_timeout)) == 0 &&
            fcntl(sender_host_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0;
    queue_full = buffer_configured && fill_host_datagram_queue(
            sender_host_fd, filler, &filler_messages, &fill_error);
    blocking_restored = original_status >= 0 &&
            fcntl(sender_host_fd, F_SETFL, original_status) == 0;
    if (!buffer_configured || !queue_full || !blocking_restored)
        goto cleanup;

    passed = fd_create(&scm_probe_ops);
    if (passed == NULL)
        goto cleanup;
    passed->data = fixture;
    passed_number = f_install_task(&fixture->task, passed, 0);
    if (passed_number < 0)
        goto cleanup;
    call.sender = pair[0];
    rights_prepared = prepare_sendmsg_rights(passed_number);
    if (!rights_prepared)
        goto cleanup;

    thread_started = pthread_create(
            &thread, NULL, sendmsg_while_blocked, &call) == 0;
    if (!thread_started)
        goto cleanup;
    started_observed = wait_for_flag(
            &call.started, BLOCKED_SEND_WAIT_MS);
    waiting_observed = started_observed && wait_for_poll_registration(
            &fixture->task, BLOCKED_SEND_WAIT_MS);
    const struct timespec stable_wait = {.tv_nsec = 10000000};
    if (waiting_observed)
        nanosleep(&stable_wait, NULL);
    stayed_blocked = waiting_observed &&
            poll_registration_matches(&fixture->task, true) &&
            !atomic_load_explicit(&call.finished, memory_order_acquire);
    references_during_wait = atomic_load_explicit(
            &passed->refcount, memory_order_relaxed);

    socket_ref_release(&receiver_ref);
    receiver_retained = false;
    receiver_closed = close_socket(fixture, &pair[1]);
    woke_promptly = receiver_closed && wait_for_flag(
            &call.finished, BLOCKED_SEND_WAIT_MS);
    bool completed = woke_promptly || wait_for_flag(&call.finished,
            BLOCKED_SEND_TIMEOUT_SECONDS * 1000 + BLOCKED_SEND_WAIT_MS);
    if (!completed) {
        (void) shutdown(sender_host_fd, SHUT_RDWR);
        completed = wait_for_flag(
                &call.finished, BLOCKED_SEND_WAIT_MS);
    }
    if (!completed) {
        fprintf(stderr,
                "Unix socket 地址测试失败：peer 关闭后阻塞发送线程仍未退出\n");
        fflush(stderr);
        _Exit(1);
    }
    thread_joined = pthread_join(thread, NULL) == 0;
    registration_cleared = poll_registration_matches(
            &fixture->task, false);
    references_after_wake = atomic_load_explicit(
            &passed->refcount, memory_order_relaxed);

    struct sockaddr_storage peer_name;
    dword_t peer_name_length = 0;
    const byte_t retry_payload = 0x72;
    peer_state_cleared = socket_getname_ref(&sender_ref, true,
                    &peer_name, &peer_name_length) == _ENOTCONN &&
            send_connected_unix_datagram(fixture, pair[0],
                    &retry_payload, sizeof(retry_payload),
                    MSG_NOSIGNAL_) == _ENOTCONN;

cleanup:
    if (thread_started && !thread_joined) {
        if (pair[1] >= 0)
            (void) sys_shutdown(pair[1], SHUT_RD);
        bool completed = wait_for_flag(
                &call.finished, BLOCKED_SEND_WAIT_MS);
        if (!completed && sender_host_fd >= 0) {
            (void) shutdown(sender_host_fd, SHUT_RDWR);
            completed = wait_for_flag(
                    &call.finished, BLOCKED_SEND_WAIT_MS);
        }
        if (!completed) {
            fprintf(stderr,
                    "Unix socket 地址测试失败：清理 peer 关闭发送线程超时\n");
            fflush(stderr);
            _Exit(1);
        }
        thread_joined = pthread_join(thread, NULL) == 0;
        registration_cleared = poll_registration_matches(
                &fixture->task, false);
    }

    bool probe_closed = false;
    if (passed_number >= 0) {
        probe_closed = f_close_task(
                &fixture->task, passed_number) == 0;
    } else if (passed != NULL) {
        probe_closed = fd_close(passed) == 0;
    }
    if (receiver_retained)
        socket_ref_release(&receiver_ref);
    if (sender_retained)
        socket_ref_release(&sender_ref);
    if (!receiver_closed)
        receiver_closed = close_socket(fixture, &pair[1]);
    bool sender_closed = close_socket(fixture, &pair[0]);

    errno = 0;
    bool receiver_host_closed = receiver_host_fd < 0 ||
            (fcntl(receiver_host_fd, F_GETFD) < 0 && errno == EBADF);
    errno = 0;
    bool sender_host_closed = sender_host_fd < 0 ||
            (fcntl(sender_host_fd, F_GETFD) < 0 && errno == EBADF);
    int host_fds_after = open_host_fds();
    bool passed_round = pair_created && sender_retained &&
            buffer_configured && queue_full && filler_messages != 0 &&
            blocking_restored && passed != NULL && rights_prepared &&
            thread_started && started_observed && waiting_observed &&
            stayed_blocked && references_during_wait == 2 &&
            receiver_closed && woke_promptly && thread_joined &&
            call.result == _ECONNREFUSED && registration_cleared &&
            references_after_wake == 1 && peer_state_cleared &&
            probe_closed && sender_closed && receiver_host_closed &&
            sender_host_closed &&
            fixture->scm_probe.close_calls ==
                    close_calls_before + round + 1 &&
            host_fds_after == host_fd_baseline;
    if (!passed_round) {
        fprintf(stderr,
                "peer 关闭阻塞发送回归失败轮次 %u：填充=%d(%u, errno=%d)，"
                "等待/关闭/唤醒=%d/%d/%d，结果=%d，引用=%u/%u，"
                "peer=%d，host fd=%d/%d\n",
                round, queue_full, filler_messages, fill_error,
                waiting_observed, receiver_closed, woke_promptly,
                call.result, references_during_wait,
                references_after_wake, peer_state_cleared,
                host_fd_baseline, host_fds_after);
    }
    return passed_round;
}

static bool test_blocked_datagram_send_wakes_on_peer_close(
        struct fixture *fixture) {
    int host_fd_baseline = open_host_fds();
    unsigned close_calls_before = fixture->scm_probe.close_calls;
    for (unsigned round = 0;
            round < BLOCKED_SEND_RECOVERY_ROUNDS; round++) {
        if (!test_blocked_datagram_send_peer_close_round(fixture,
                    round, close_calls_before, host_fd_baseline))
            return false;
    }
    CHECK(fixture->scm_probe.close_calls ==
                    close_calls_before + BLOCKED_SEND_RECOVERY_ROUNDS &&
            open_host_fds() == host_fd_baseline,
            "peer 关闭多轮完成后 SCM 与 host fd 回到基线");
    return true;
}

static bool test_blocked_datagram_send_receive_round(
        struct fixture *fixture, unsigned round, int host_fd_baseline) {
    fd_t pair[2] = {-1, -1};
    struct socket_ref sender_ref = {0};
    struct socket_ref receiver_ref = {0};
    bool sender_retained = false;
    bool receiver_retained = false;
    int sender_host_fd = -1;
    int receiver_host_fd = -1;
    int original_status = -1;
    bool pair_created = false;
    bool buffer_configured = false;
    bool queue_full = false;
    bool blocking_restored = false;
    unsigned filler_messages = 0;
    int fill_error = 0;
    pthread_t thread;
    bool thread_started = false;
    bool thread_joined = false;
    bool started_observed = false;
    bool waiting_observed = false;
    bool stayed_blocked = false;
    bool consumed_one = false;
    bool woke_promptly = false;
    bool registration_cleared = false;
    bool queue_drained_in_order = false;
    const byte_t filler = 0x6b;
    const int buffer_size = BLOCKED_SEND_BUFFER_SIZE;
    const struct timeval send_timeout = {
        .tv_sec = BLOCKED_SEND_TIMEOUT_SECONDS,
    };
    struct blocking_datagram_send_call call = {
        .fixture = fixture,
        .payload = (byte_t) (0xc0 + round),
        .result = _EIO,
    };
    atomic_init(&call.started, false);
    atomic_init(&call.finished, false);

    pair_created = sys_socketpair(AF_LOCAL_, SOCK_DGRAM_, 0,
                    USER_SOCKETPAIR) == 0 &&
            user_read(USER_SOCKETPAIR, pair, sizeof(pair)) == 0;
    if (!pair_created)
        goto cleanup;
    sender_retained = socket_ref_get_task(
            &fixture->task, pair[0], &sender_ref) == 0;
    receiver_retained = socket_ref_get_task(
            &fixture->task, pair[1], &receiver_ref) == 0;
    if (!sender_retained || !receiver_retained)
        goto cleanup;
    sender_host_fd = sender_ref.fd->real_fd;
    receiver_host_fd = receiver_ref.fd->real_fd;

    original_status = fcntl(sender_host_fd, F_GETFL);
    buffer_configured = original_status >= 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(receiver_host_fd, SOL_SOCKET, SO_RCVBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDTIMEO,
                    &send_timeout, sizeof(send_timeout)) == 0 &&
            fcntl(sender_host_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0;
    queue_full = buffer_configured && fill_host_datagram_queue(
            sender_host_fd, filler, &filler_messages, &fill_error);
    blocking_restored = original_status >= 0 &&
            fcntl(sender_host_fd, F_SETFL, original_status) == 0;
    if (!buffer_configured || !queue_full || !blocking_restored)
        goto cleanup;

    call.sender = pair[0];
    thread_started = pthread_create(
            &thread, NULL, send_datagram_while_blocked, &call) == 0;
    if (!thread_started)
        goto cleanup;
    started_observed = wait_for_flag(
            &call.started, BLOCKED_SEND_WAIT_MS);
    waiting_observed = started_observed && wait_for_poll_registration(
            &fixture->task, BLOCKED_SEND_WAIT_MS);
    const struct timespec stable_wait = {.tv_nsec = 10000000};
    if (waiting_observed)
        nanosleep(&stable_wait, NULL);
    stayed_blocked = waiting_observed &&
            poll_registration_matches(&fixture->task, true) &&
            !atomic_load_explicit(&call.finished, memory_order_acquire);

    byte_t consumed = 0;
    consumed_one = stayed_blocked &&
            receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                    &consumed, sizeof(consumed), NULL) ==
                    (ssize_t) sizeof(consumed) &&
            consumed == filler;
    woke_promptly = consumed_one && wait_for_flag(
            &call.finished, BLOCKED_SEND_WAIT_MS);
    if (atomic_load_explicit(&call.finished, memory_order_acquire)) {
        thread_joined = pthread_join(thread, NULL) == 0;
        registration_cleared = poll_registration_matches(
                &fixture->task, false);
    }

    if (thread_joined && call.result == (ssize_t) sizeof(call.payload)) {
        bool ordered = true;
        byte_t received = 0;
        for (unsigned index = 1; index < filler_messages; index++) {
            if (receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                        &received, sizeof(received), NULL) !=
                        (ssize_t) sizeof(received) ||
                    received != filler) {
                ordered = false;
                break;
            }
        }
        byte_t delivered = 0;
        byte_t unexpected = 0;
        queue_drained_in_order = ordered &&
                receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                        &delivered, sizeof(delivered), NULL) ==
                        (ssize_t) sizeof(delivered) &&
                delivered == call.payload &&
                receive_unix_datagram(fixture, pair[1], MSG_DONTWAIT_,
                        &unexpected, sizeof(unexpected), NULL) == _EAGAIN;
    }

cleanup:
    if (thread_started && !thread_joined) {
        if (pair[1] >= 0)
            (void) sys_shutdown(pair[1], SHUT_RD);
        bool completed = wait_for_flag(
                &call.finished, BLOCKED_SEND_WAIT_MS);
        if (!completed && sender_host_fd >= 0) {
            (void) shutdown(sender_host_fd, SHUT_RDWR);
            completed = wait_for_flag(
                    &call.finished, BLOCKED_SEND_WAIT_MS);
        }
        if (!completed) {
            fprintf(stderr,
                    "Unix socket 地址测试失败：容量恢复发送线程救援后仍未退出\n");
            fflush(stderr);
            _Exit(1);
        }
        thread_joined = pthread_join(thread, NULL) == 0;
        registration_cleared = poll_registration_matches(
                &fixture->task, false);
    }
    if (receiver_retained)
        socket_ref_release(&receiver_ref);
    if (sender_retained)
        socket_ref_release(&sender_ref);
    bool receiver_closed = close_socket(fixture, &pair[1]);
    bool sender_closed = close_socket(fixture, &pair[0]);

    errno = 0;
    bool receiver_host_closed = receiver_host_fd < 0 ||
            (fcntl(receiver_host_fd, F_GETFD) < 0 && errno == EBADF);
    errno = 0;
    bool sender_host_closed = sender_host_fd < 0 ||
            (fcntl(sender_host_fd, F_GETFD) < 0 && errno == EBADF);
    int host_fds_after = open_host_fds();
    bool passed_round = pair_created && sender_retained &&
            receiver_retained && buffer_configured && queue_full &&
            filler_messages != 0 && blocking_restored && thread_started &&
            started_observed && waiting_observed && stayed_blocked &&
            consumed_one && woke_promptly && thread_joined &&
            call.result == (ssize_t) sizeof(call.payload) &&
            registration_cleared && queue_drained_in_order &&
            receiver_closed && sender_closed && receiver_host_closed &&
            sender_host_closed && host_fds_after == host_fd_baseline;
    if (!passed_round) {
        fprintf(stderr,
                "容量恢复回归失败轮次 %u：填充=%d(%u, errno=%d)，"
                "等待=%d，消费=%d，唤醒=%d，结果=%zd，队列=%d，host fd=%d/%d\n",
                round, queue_full, filler_messages, fill_error,
                waiting_observed, consumed_one, woke_promptly, call.result,
                queue_drained_in_order, host_fd_baseline, host_fds_after);
    }
    return passed_round;
}

static bool test_blocked_datagram_send_wakes_on_receive(
        struct fixture *fixture) {
    int host_fd_baseline = open_host_fds();
    for (unsigned round = 0;
            round < BLOCKED_SEND_RECOVERY_ROUNDS; round++) {
        if (!test_blocked_datagram_send_receive_round(
                    fixture, round, host_fd_baseline))
            return false;
    }
    CHECK(open_host_fds() == host_fd_baseline,
            "receiver 消费恢复测试后 host fd 回到基线");
    return true;
}

static bool test_blocked_datagram_retarget_round(
        struct fixture *fixture, unsigned round,
        bool reject_new_route, int host_fd_baseline) {
    byte_t sender_name[] = {0, 'r', 't', '-', 's', (byte_t) round};
    byte_t old_name[] = {0, 'r', 't', '-', 'o', (byte_t) round};
    byte_t new_name[] = {0, 'r', 't', '-', 'n', (byte_t) round};
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));
    struct guest_unix_address old_address =
            unix_address(old_name, sizeof(old_name));
    struct guest_unix_address new_address =
            unix_address(new_name, sizeof(new_name));
    fd_t sender = -1;
    fd_t old_receiver = -1;
    fd_t new_receiver = -1;
    struct socket_ref sender_ref = {0};
    struct socket_ref old_ref = {0};
    struct socket_ref new_ref = {0};
    bool sender_retained = false;
    bool old_retained = false;
    bool new_retained = false;
    int sender_host_fd = -1;
    int old_host_fd = -1;
    int new_host_fd = -1;
    int original_status = -1;
    bool sockets_created = false;
    bool sockets_bound = false;
    bool initially_connected = false;
    bool old_inbound_queued = false;
    bool same_route_preserved = false;
    bool buffer_configured = false;
    bool queue_full = false;
    bool blocking_restored = false;
    bool new_route_ready = false;
    unsigned filler_messages = 0;
    int fill_error = 0;
    pthread_t thread;
    bool thread_started = false;
    bool thread_joined = false;
    bool started_observed = false;
    bool waiting_observed = false;
    bool stayed_blocked = false;
    bool retargeted = false;
    bool woke_promptly = false;
    bool registration_cleared = false;
    bool new_delivery_exact = false;
    bool old_queue_unchanged = false;
    bool old_inbound_purged = false;
    const byte_t filler = 0x74;
    const int buffer_size = BLOCKED_SEND_BUFFER_SIZE;
    const struct timeval send_timeout = {
        .tv_sec = BLOCKED_SEND_TIMEOUT_SECONDS,
    };
    struct blocking_datagram_send_call call = {
        .fixture = fixture,
        .payload = (byte_t) (0xd0 + round),
        .result = _EIO,
    };
    atomic_init(&call.started, false);
    atomic_init(&call.finished, false);

    sender = create_unix_socket(fixture, SOCK_DGRAM_);
    old_receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    new_receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    sockets_created = sender >= 0 && old_receiver >= 0 && new_receiver >= 0;
    if (!sockets_created)
        goto cleanup;
    sockets_bound = bind_unix_socket(
                    fixture, sender, &sender_address) == 0 &&
            bind_unix_socket(fixture, old_receiver, &old_address) == 0 &&
            bind_unix_socket(fixture, new_receiver, &new_address) == 0;
    initially_connected = sockets_bound && connect_unix_socket(
            fixture, sender, &old_address) == 0;
    if (!sockets_bound || !initially_connected)
        goto cleanup;

    sender_retained = socket_ref_get_task(
            &fixture->task, sender, &sender_ref) == 0;
    old_retained = socket_ref_get_task(
            &fixture->task, old_receiver, &old_ref) == 0;
    new_retained = socket_ref_get_task(
            &fixture->task, new_receiver, &new_ref) == 0;
    if (!sender_retained || !old_retained || !new_retained)
        goto cleanup;
    sender_host_fd = sender_ref.fd->real_fd;
    old_host_fd = old_ref.fd->real_fd;
    new_host_fd = new_ref.fd->real_fd;

    const byte_t stale_inbound = 0x75;
    byte_t stale_peek = 0;
    old_inbound_queued = send_unix_datagram(fixture,
                    old_receiver, &sender_address,
                    &stale_inbound, sizeof(stale_inbound)) ==
                    (ssize_t) sizeof(stale_inbound);
    same_route_preserved = old_inbound_queued &&
            connect_unix_socket(fixture, sender, &old_address) == 0 &&
            receive_unix_datagram(fixture, sender,
                    MSG_PEEK_ | MSG_DONTWAIT_,
                    &stale_peek, sizeof(stale_peek), NULL) ==
                    (ssize_t) sizeof(stale_peek) &&
            stale_peek == stale_inbound;
    if (!same_route_preserved)
        goto cleanup;

    original_status = fcntl(sender_host_fd, F_GETFL);
    buffer_configured = original_status >= 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(old_host_fd, SOL_SOCKET, SO_RCVBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(sender_host_fd, SOL_SOCKET, SO_SNDTIMEO,
                    &send_timeout, sizeof(send_timeout)) == 0 &&
            fcntl(sender_host_fd, F_SETFL,
                    original_status | O_NONBLOCK) == 0;
    queue_full = buffer_configured && fill_host_datagram_queue(
            sender_host_fd, filler, &filler_messages, &fill_error);
    blocking_restored = original_status >= 0 &&
            fcntl(sender_host_fd, F_SETFL, original_status) == 0;
    if (!buffer_configured || !queue_full || !blocking_restored)
        goto cleanup;
    new_route_ready = !reject_new_route ||
            sys_shutdown(new_receiver, SHUT_RD) == 0;
    if (!new_route_ready)
        goto cleanup;

    call.sender = sender;
    thread_started = pthread_create(
            &thread, NULL, send_datagram_while_blocked, &call) == 0;
    if (!thread_started)
        goto cleanup;
    started_observed = wait_for_flag(
            &call.started, BLOCKED_SEND_WAIT_MS);
    waiting_observed = started_observed && wait_for_poll_registration(
            &fixture->task, BLOCKED_SEND_WAIT_MS);
    const struct timespec stable_wait = {.tv_nsec = 10000000};
    if (waiting_observed)
        nanosleep(&stable_wait, NULL);
    stayed_blocked = waiting_observed &&
            poll_registration_matches(&fixture->task, true) &&
            !atomic_load_explicit(&call.finished, memory_order_acquire);

    retargeted = stayed_blocked && connect_unix_socket(
            fixture, sender, &new_address) == 0;
    woke_promptly = retargeted && wait_for_flag(
            &call.finished, BLOCKED_SEND_WAIT_MS);
    if (atomic_load_explicit(&call.finished, memory_order_acquire)) {
        thread_joined = pthread_join(thread, NULL) == 0;
        registration_cleared = poll_registration_matches(
                &fixture->task, false);
    }

    if (thread_joined) {
        byte_t delivered = 0;
        byte_t unexpected = 0;
        old_inbound_purged = receive_unix_datagram(
                fixture, sender, MSG_DONTWAIT_,
                &unexpected, sizeof(unexpected), NULL) == _EAGAIN;
        if (reject_new_route) {
            new_delivery_exact = call.result == _EPIPE &&
                    receive_unix_datagram(fixture, new_receiver,
                            MSG_DONTWAIT_, &unexpected,
                            sizeof(unexpected), NULL) == _EAGAIN;
        } else {
            new_delivery_exact =
                    call.result == (ssize_t) sizeof(call.payload) &&
                    receive_unix_datagram(fixture, new_receiver,
                            MSG_DONTWAIT_, &delivered,
                            sizeof(delivered), NULL) ==
                            (ssize_t) sizeof(delivered) &&
                    delivered == call.payload &&
                    receive_unix_datagram(fixture, new_receiver,
                            MSG_DONTWAIT_, &unexpected,
                            sizeof(unexpected), NULL) == _EAGAIN;
        }

        bool old_ordered = true;
        byte_t old_payload = 0;
        for (unsigned index = 0; index < filler_messages; index++) {
            if (receive_unix_datagram(fixture, old_receiver,
                        MSG_DONTWAIT_, &old_payload,
                        sizeof(old_payload), NULL) !=
                        (ssize_t) sizeof(old_payload) ||
                    old_payload != filler) {
                old_ordered = false;
                break;
            }
        }
        old_queue_unchanged = old_ordered &&
                receive_unix_datagram(fixture, old_receiver,
                        MSG_DONTWAIT_, &old_payload,
                        sizeof(old_payload), NULL) == _EAGAIN;
    }

cleanup:
    if (thread_started && !thread_joined) {
        if (sender >= 0)
            (void) sys_shutdown(sender, SHUT_WR);
        bool completed = wait_for_flag(
                &call.finished, BLOCKED_SEND_WAIT_MS);
        if (!completed && sender_host_fd >= 0) {
            (void) shutdown(sender_host_fd, SHUT_RDWR);
            completed = wait_for_flag(
                    &call.finished, BLOCKED_SEND_WAIT_MS);
        }
        if (!completed) {
            fprintf(stderr,
                    "Unix socket 地址测试失败：改换 peer 发送线程救援后仍未退出\n");
            fflush(stderr);
            _Exit(1);
        }
        thread_joined = pthread_join(thread, NULL) == 0;
        registration_cleared = poll_registration_matches(
                &fixture->task, false);
    }
    if (new_retained)
        socket_ref_release(&new_ref);
    if (old_retained)
        socket_ref_release(&old_ref);
    if (sender_retained)
        socket_ref_release(&sender_ref);
    bool new_closed = close_socket(fixture, &new_receiver);
    bool old_closed = close_socket(fixture, &old_receiver);
    bool sender_closed = close_socket(fixture, &sender);

    errno = 0;
    bool new_host_closed = new_host_fd < 0 ||
            (fcntl(new_host_fd, F_GETFD) < 0 && errno == EBADF);
    errno = 0;
    bool old_host_closed = old_host_fd < 0 ||
            (fcntl(old_host_fd, F_GETFD) < 0 && errno == EBADF);
    errno = 0;
    bool sender_host_closed = sender_host_fd < 0 ||
            (fcntl(sender_host_fd, F_GETFD) < 0 && errno == EBADF);
    int host_fds_after = open_host_fds();
    bool passed_round = sockets_created && sockets_bound &&
            initially_connected && sender_retained && old_retained &&
            new_retained && old_inbound_queued && same_route_preserved &&
            buffer_configured && queue_full &&
            filler_messages != 0 && blocking_restored && new_route_ready &&
            thread_started &&
            started_observed && waiting_observed && stayed_blocked &&
            retargeted && woke_promptly && thread_joined &&
            call.result == (reject_new_route ?
                    _EPIPE : (ssize_t) sizeof(call.payload)) &&
            registration_cleared && old_inbound_purged && new_delivery_exact &&
            old_queue_unchanged && new_closed && old_closed &&
            sender_closed && new_host_closed && old_host_closed &&
            sender_host_closed && host_fds_after == host_fd_baseline;
    if (!passed_round) {
        fprintf(stderr,
                "改换 peer 回归失败轮次 %u：填充=%d(%u, errno=%d)，"
                "同路由/清旧=%d/%d，等待=%d，重连=%d，唤醒=%d，"
                "结果=%zd，新/旧队列=%d/%d，"
                "host fd=%d/%d\n",
                round, queue_full, filler_messages, fill_error,
                same_route_preserved, old_inbound_purged,
                waiting_observed, retargeted, woke_promptly, call.result,
                new_delivery_exact, old_queue_unchanged,
                host_fd_baseline, host_fds_after);
    }
    return passed_round;
}

static bool test_blocked_datagram_send_wakes_on_retarget(
        struct fixture *fixture) {
    int host_fd_baseline = open_host_fds();
    for (unsigned round = 0;
            round < BLOCKED_SEND_RECOVERY_ROUNDS; round++) {
        if (!test_blocked_datagram_retarget_round(
                    fixture, round, false, host_fd_baseline))
            return false;
    }
    if (!test_blocked_datagram_retarget_round(fixture,
                BLOCKED_SEND_RECOVERY_ROUNDS,
                true, host_fd_baseline))
        return false;
    CHECK(open_host_fds() == host_fd_baseline,
            "改换 peer 恢复测试后 host fd 回到基线");
    return true;
}

static bool test_receiver_discard_stress(struct fixture *fixture) {
    static const byte_t receiver_name[] = {
        0, 'd', 'r', 'a', 'i', 'n', '-', 'r', 'x',
    };
    static const byte_t sender_name[] = {
        0, 'd', 'r', 'a', 'i', 'n', 0, 0xfe,
    };
    struct guest_unix_address receiver_address =
            unix_address(receiver_name, sizeof(receiver_name));
    struct guest_unix_address sender_address =
            unix_address(sender_name, sizeof(sender_name));

    for (unsigned iteration = 0;
            iteration < DISCARD_STRESS_ITERATIONS; iteration++) {
        fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
        fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
        CHECK(receiver >= 0 && sender >= 0 &&
                bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
                bind_unix_socket(fixture, sender, &sender_address) == 0,
                "压力轮次可重复绑定同一数据报名称");

        char receiver_backing[sizeof(
                ((struct sockaddr_un *) 0)->sun_path)];
        char sender_backing[sizeof(
                ((struct sockaddr_un *) 0)->sun_path)];
        CHECK(host_backing_path(fixture, receiver, receiver_backing) &&
                host_backing_path(fixture, sender, sender_backing),
                "压力轮次取得两端 host 后备路径");
        byte_t payload = (byte_t) iteration;
        CHECK(send_unix_datagram(fixture, sender, &receiver_address,
                        &payload, sizeof(payload)) == sizeof(payload),
                "压力轮次向接收队列写入待丢弃数据报");
        CHECK(close_socket(fixture, &sender) &&
                close_socket(fixture, &receiver),
                "接收端关闭时丢弃队列并释放反向名称映射");

        errno = 0;
        bool sender_removed =
                lstat(sender_backing, &(struct stat) {0}) == -1 &&
                errno == ENOENT;
        errno = 0;
        bool receiver_removed =
                lstat(receiver_backing, &(struct stat) {0}) == -1 &&
                errno == ENOENT;
        CHECK(sender_removed && receiver_removed,
                "压力轮次未遗留内部 host 后备节点");
    }

    fd_t receiver = create_unix_socket(fixture, SOCK_DGRAM_);
    fd_t sender = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0 &&
            bind_unix_socket(fixture, receiver, &receiver_address) == 0 &&
            bind_unix_socket(fixture, sender, &sender_address) == 0,
            "压力完成后名称空间与映射仍可重新建立");
    CHECK(close_socket(fixture, &sender) &&
            close_socket(fixture, &receiver),
            "清理压力后的最终探测 socket");
    return true;
}

int main(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture)) {
        fixture_destroy(&fixture);
        return 1;
    }

    bool passed = test_abstract_type_namespaces(&fixture) &&
            test_pathname_getsockname(&fixture) &&
            test_datagram_path_source(&fixture) &&
            test_closed_binary_source_and_peek(&fixture) &&
            test_recvmsg_fault_name_lifetime(&fixture) &&
            test_recvmsg_peek_name_lifetime(&fixture) &&
            test_sendmsg_input_boundaries(&fixture) &&
            test_recvmsg_scm_truncation_and_rollback(&fixture) &&
            test_recvmsg_scm_container_lifetime(&fixture) &&
            test_datagram_shutdown_preserves_queue(&fixture) &&
            test_datagram_socketpair_close_detaches_peer(&fixture) &&
            test_datagram_close_detaches_many_senders(&fixture) &&
            test_datagram_explicit_send_preserves_dead_peer(&fixture) &&
            test_datagram_dead_peer_send_purges_queue(&fixture) &&
            test_datagram_peer_credentials_stay_snapshot(&fixture) &&
            test_datagram_same_name_rebind_changes_identity(&fixture) &&
            test_datagram_concurrent_connect_commits_one_route(&fixture) &&
            test_datagram_failed_connect_preserves_old_route(&fixture) &&
            test_datagram_connected_receiver_filters_senders(&fixture) &&
            test_datagram_alternate_restore_failure_commits_delivery(
                    &fixture) &&
            test_datagram_socketpair_retarget_preserves_reverse_peer(
                    &fixture) &&
            test_datagram_disconnect_resets_route_state(&fixture) &&
            test_datagram_drain_continues_after_reset(&fixture) &&
            test_datagram_self_retarget_does_not_reset(&fixture) &&
            test_datagram_dead_route_disconnects_host(&fixture) &&
            test_unconnected_shutdown_preserves_queue(&fixture) &&
            test_send_shutdown_atomicity(&fixture) &&
            test_blocked_datagram_send_wakes_on_shutdown(&fixture) &&
            test_blocked_datagram_send_wakes_on_peer_close(&fixture) &&
            test_blocked_datagram_send_wakes_on_receive(&fixture) &&
            test_blocked_datagram_send_wakes_on_retarget(&fixture) &&
            test_receiver_discard_stress(&fixture);
    fixture_destroy(&fixture);
    if (!passed)
        return 1;
    puts("Unix socket 地址测试通过");
    return 0;
}

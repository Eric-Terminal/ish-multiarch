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
#include <sys/un.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
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
#ifdef __APPLE__
#define SHUTDOWN_RACE_ITERATIONS 64
#endif
#define MSG_CMSG_CLOEXEC_ UINT32_C(0x40000000)

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

#ifdef __APPLE__
struct shutdown_send_call {
    struct fixture *fixture;
    struct socket_ref sender;
    struct scm *scm;
    atomic_bool ready;
    atomic_bool start;
    ssize_t result;
};
#endif

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

static const struct fd_ops scm_probe_ops = {
    .close = scm_probe_close,
};

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    memset(&fixture->task, 0, sizeof(fixture->task));
    memset(&fixture->group, 0, sizeof(fixture->group));
    lock_init(&fixture->group.lock);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {256, 256};
    fixture->task.group = &fixture->group;
    fixture->task.euid = 1000;
    fixture->task.egid = 1000;
    current = &fixture->task;

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

#ifdef __APPLE__
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
#endif

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

#ifdef __APPLE__
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
#endif

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

static bool close_socket(struct fixture *fixture, fd_t *number) {
    if (*number < 0)
        return true;
    int error = f_close_task(&fixture->task, *number);
    *number = -1;
    return error == 0;
}

#ifdef __APPLE__
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
#endif

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

#ifdef __APPLE__
static bool test_datagram_shutdown_drain(struct fixture *fixture) {
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
    struct fd *receiver_fd = f_get_task(&fixture->task, receiver);
    CHECK(receiver_fd != NULL &&
            receiver_fd->socket.unix_read_shutdown &&
            list_empty(&receiver_fd->socket.unix_scm) &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1,
            "SHUT_RD 排空零长度之后的报文并释放 SCM 引用");

    const byte_t rejected = 0x65;
    CHECK(send_connected_unix_datagram(fixture, sender,
                    &rejected, sizeof(rejected), MSG_NOSIGNAL_) == _EPIPE &&
            prepare_sendmsg_rights(passed_number) &&
            sys_sendmsg(sender, USER_SEND_HEADER, MSG_NOSIGNAL_) == _EPIPE &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            list_empty(&receiver_fd->socket.unix_scm),
            "读 shutdown 后普通与 SCM 发送按 Linux 语义返回 EPIPE 且不再入队");

    CHECK(sys_shutdown(receiver, SHUT_RD) == 0 &&
            sys_shutdown(receiver, SHUT_RDWR) == 0 &&
            sys_shutdown(receiver, SHUT_RDWR) == 0,
            "重复 SHUT_RD 与 SHUT_RDWR 幂等且不再次进入 Darwin EOF 排空循环");
    CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "shutdown 丢弃的 SCM 对象恰好析构一次");
    CHECK(close_socket(fixture, &sender),
            "关闭 shutdown 数据报发送端");
    fd_t rebound = create_unix_socket(fixture, SOCK_DGRAM_);
    CHECK(rebound >= 0 &&
            bind_unix_socket(fixture, rebound, &sender_address) == 0,
            "shutdown 排空了每条报文的反向名称计数");
    CHECK(close_socket(fixture, &rebound) &&
            close_socket(fixture, &receiver),
            "关闭已读 shutdown 的数据报端不会永久循环");
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

    CHECK(sys_shutdown(receiver, SHUT_RD) == _ENOTCONN,
            "未连接数据报 shutdown 保留 Darwin 的 ENOTCONN");
    struct fd *receiver_fd = f_get_task(&fixture->task, receiver);
    CHECK(receiver_fd != NULL &&
            !receiver_fd->socket.unix_read_shutdown &&
            !list_empty(&receiver_fd->socket.unix_scm) &&
            atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 2,
            "失败的 shutdown 未提前消费 host 队列或 SCM 引用");

    byte_t received = 0;
    CHECK(receive_unix_datagram(fixture, receiver, 0,
                    &received, sizeof(received), NULL) == sizeof(received) &&
            received == ordinary,
            "失败的 shutdown 后普通数据报仍可读取");
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
            "失败的 shutdown 后 SCM 数据报仍保持配对");
    socket_scm_release(received_scm);
    CHECK(atomic_load_explicit(
                    &passed->refcount, memory_order_relaxed) == 1 &&
            f_close_task(&fixture->task, passed_number) == 0 &&
            fixture->scm_probe.close_calls == 1,
            "读取保留的 SCM 后引用恰好释放一次");
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

        struct fd *receiver = f_get_task(&fixture->task, pair[1]);
        CHECK(shutdown_result == 0 &&
                (call.result == 1 || call.result == _EPIPE) &&
                receiver != NULL &&
                receiver->socket.unix_read_shutdown &&
                list_empty(&receiver->socket.unix_scm) &&
                atomic_load_explicit(
                        &passed->refcount, memory_order_relaxed) == 1,
                "并发 send 与 shutdown 在线性化点两侧均不遗留 SCM");
        CHECK(f_close_task(&fixture->task, passed_number) == 0 &&
                fixture->scm_probe.close_calls == iteration + 1 &&
                close_socket(fixture, &pair[1]) &&
                close_socket(fixture, &pair[0]),
                "并发轮次关闭已 shutdown socket 与探针");
    }
    return true;
}
#endif

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
#ifdef __APPLE__
            test_datagram_shutdown_drain(&fixture) &&
            test_unconnected_shutdown_preserves_queue(&fixture) &&
            test_send_shutdown_atomicity(&fixture) &&
#endif
            test_receiver_discard_stress(&fixture);
    fixture_destroy(&fixture);
    if (!passed)
        return 1;
    puts("Unix socket 地址测试通过");
    return 0;
}

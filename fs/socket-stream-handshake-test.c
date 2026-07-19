#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define GUEST_PAGE UINT32_C(0x1000)
#define SEND_PAYLOAD (GUEST_PAGE + UINT32_C(0x100))
#define SEND_IOV (GUEST_PAGE + UINT32_C(0x120))
#define SEND_CONTROL (GUEST_PAGE + UINT32_C(0x140))
#define SEND_HEADER (GUEST_PAGE + UINT32_C(0x180))
#define RECEIVE_PAYLOAD (GUEST_PAGE + UINT32_C(0x200))
#define RECEIVE_IOV (GUEST_PAGE + UINT32_C(0x220))
#define RECEIVE_CONTROL (GUEST_PAGE + UINT32_C(0x240))
#define RECEIVE_HEADER (GUEST_PAGE + UINT32_C(0x280))
#define QUERY_LENGTH (GUEST_PAGE + UINT32_C(0x300))
#define QUERY_ADDRESS (GUEST_PAGE + UINT32_C(0x340))

static int failures;
static atomic_uint unique_name;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AF_UNIX stream 握手测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

struct close_probe {
    int calls;
};

struct guest_rights {
    dword_t length;
    int_t level;
    int_t type;
    fd_t fd;
};

struct fixture {
    struct task task;
    struct tgroup group;
    fd_t listener_number;
    fd_t connector_number;
    fd_t passed_number;
    struct fd *connector;
    struct fd *passed;
    struct close_probe passed_close;
};

static int probe_close(struct fd *fd) {
    struct close_probe *probe = fd->data;
    probe->calls++;
    return 0;
}

static const struct fd_ops probe_ops = {
    .close = probe_close,
};

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL)
        fdtable_release(fixture->task.files);
    if (fixture->task.mm != NULL)
        mm_release(fixture->task.mm);
    if (fixture->task.fs != NULL)
        fs_info_release(fixture->task.fs);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static bool fixture_base_init(struct fixture *fixture,
        bool with_passed_fd, bool with_filesystem) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->listener_number = -1;
    fixture->connector_number = -1;
    fixture->passed_number = -1;
    lock_init(&fixture->group.lock);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(4);
    if (IS_ERR(fixture->task.files)) {
        fixture->task.files = NULL;
        return false;
    }
    task_set_mm(&fixture->task, mm_new());
    if (fixture->task.mm == NULL)
        return false;
    write_wrlock(&fixture->task.mem->lock);
    int map_error = pt_map_nothing(
            fixture->task.mem, PAGE(GUEST_PAGE), 1, P_RWX);
    write_wrunlock(&fixture->task.mem->lock);
    if (map_error < 0)
        return false;
    current = &fixture->task;

    if (with_filesystem) {
        // tmpfs 尚未实现 umount；挂载由这个短生命周期测试进程独占。
        lock(&mounts_lock);
        int mount_error = do_mount(&tmpfs, "", "", "", 0);
        unlock(&mounts_lock);
        if (mount_error < 0)
            return false;
        fixture->task.fs = fs_info_new();
        if (fixture->task.fs == NULL)
            return false;
    }

    fixture->listener_number = socket_create_task(
            &fixture->task, AF_LOCAL_, SOCK_STREAM_, 0);
    fixture->connector_number = socket_create_task(
            &fixture->task, AF_LOCAL_, SOCK_STREAM_, 0);
    if (fixture->listener_number != 0 || fixture->connector_number != 1)
        return false;
    fixture->connector = f_get_task(
            &fixture->task, fixture->connector_number);

    if (with_passed_fd) {
        fixture->passed = fd_create(&probe_ops);
        if (fixture->passed == NULL)
            return false;
        fixture->passed->data = &fixture->passed_close;
        fixture->passed_number = f_install_task(
                &fixture->task, fixture->passed, 0);
        if (fixture->passed_number != 2)
            return false;
    }
    return true;
}

static bool fixture_init(struct fixture *fixture, bool with_passed_fd) {
    if (!fixture_base_init(fixture, with_passed_fd, false))
        return false;

    struct sockaddr_max_ address = {.family = AF_LOCAL_};
    unsigned sequence = atomic_fetch_add_explicit(
            &unique_name, 1, memory_order_relaxed);
    snprintf(address.data + 1, sizeof(address.data) - 1,
            "stream-handshake-%ld-%u", (long) getpid(), sequence);
    size_t address_length = offsetof(struct sockaddr_max_, data) + 1 +
            strlen(address.data + 1);
    struct socket_ref listener = {0};
    struct socket_ref connector = {0};
    bool ready = socket_ref_get_task(&fixture->task,
                         fixture->listener_number, &listener) == 0 &&
            socket_bind_ref_task(&fixture->task, &listener,
                    &address, address_length) == 0 &&
            sys_listen(fixture->listener_number, 4) == 0 &&
            socket_ref_get_task(&fixture->task,
                    fixture->connector_number, &connector) == 0 &&
            socket_connect_ref_task(&fixture->task, &connector,
                    &address, address_length) == 0;
    socket_ref_release(&connector);
    socket_ref_release(&listener);
    return ready;
}

struct guest_unix_name {
    struct sockaddr_storage storage;
    size_t length;
};

static struct guest_unix_name guest_unix_name(
        const void *name, size_t name_length) {
    struct guest_unix_name address = {0};
    struct sockaddr_ *wire = (struct sockaddr_ *) &address.storage;
    wire->family = AF_LOCAL_;
    assert(name_length <= SOCKADDR_DATA_MAX);
    if (name_length != 0)
        memcpy((byte_t *) &address.storage +
                offsetof(struct sockaddr_, data), name, name_length);
    address.length = offsetof(struct sockaddr_, data) + name_length;
    return address;
}

static bool bind_unix_stream(struct fixture *fixture, fd_t number,
        const struct guest_unix_name *address) {
    struct socket_ref socket = {0};
    int error = socket_ref_get_task(&fixture->task, number, &socket);
    if (error == 0)
        error = socket_bind_ref_task(&fixture->task, &socket,
                &address->storage, address->length);
    socket_ref_release(&socket);
    return error == 0;
}

static bool connect_unix_stream(struct fixture *fixture, fd_t number,
        const struct guest_unix_name *address) {
    struct socket_ref socket = {0};
    int error = socket_ref_get_task(&fixture->task, number, &socket);
    if (error == 0)
        error = socket_connect_ref_task(&fixture->task, &socket,
                &address->storage, address->length);
    socket_ref_release(&socket);
    return error == 0;
}

static bool guest_name_query_matches(struct fixture *fixture,
        fd_t number, bool peer, const void *name, size_t name_length) {
    struct sockaddr_storage expected = {0};
    ((struct sockaddr_ *) &expected)->family = AF_LOCAL_;
    if (name_length != 0)
        memcpy((byte_t *) &expected + offsetof(struct sockaddr_, data),
                name, name_length);
    dword_t true_length = (dword_t) (
            offsetof(struct sockaddr_, data) + name_length);
    dword_t capacities[2] = {
        sizeof(struct sockaddr_storage),
        true_length == offsetof(struct sockaddr_, data) ? 1 : 3,
    };

    current = &fixture->task;
    for (size_t index = 0; index < 2; index++) {
        struct sockaddr_storage returned;
        memset(&returned, 0xa5, sizeof(returned));
        dword_t capacity = capacities[index];
        if (user_write(QUERY_LENGTH, &capacity, sizeof(capacity)) != 0 ||
                user_write(QUERY_ADDRESS,
                        &returned, sizeof(returned)) != 0)
            return false;
        int_t result = peer ?
                sys_getpeername(number, QUERY_ADDRESS, QUERY_LENGTH) :
                sys_getsockname(number, QUERY_ADDRESS, QUERY_LENGTH);
        dword_t returned_length = 0;
        if (result != 0 ||
                user_read(QUERY_LENGTH, &returned_length,
                        sizeof(returned_length)) != 0 ||
                user_read(QUERY_ADDRESS, &returned, sizeof(returned)) != 0 ||
                returned_length != true_length)
            return false;
        size_t copied = capacity < true_length ? capacity : true_length;
        if (memcmp(&returned, &expected, copied) != 0)
            return false;
        const byte_t *wire = (const byte_t *) &returned;
        for (size_t offset = copied; offset < sizeof(returned); offset++)
            if (wire[offset] != 0xa5)
                return false;
    }
    return true;
}

static void set_effective_credentials(struct fixture *fixture,
        uid_t_ uid, uid_t_ gid) {
    lock(&pids_lock);
    fixture->task.euid = uid;
    fixture->task.egid = gid;
    unlock(&pids_lock);
}

static bool guest_peercred_matches(struct fixture *fixture, fd_t number,
        pid_t_ pid, uid_t_ uid, uid_t_ gid) {
    struct ucred_ returned = {
        .pid = -1,
        .uid = (uid_t_) -1,
        .gid = (uid_t_) -1,
    };
    dword_t length = sizeof(returned);
    current = &fixture->task;
    if (user_write(QUERY_ADDRESS, &returned, sizeof(returned)) != 0 ||
            user_write(QUERY_LENGTH, &length, sizeof(length)) != 0 ||
            sys_getsockopt(number, SOL_SOCKET_, SO_PEERCRED_,
                    QUERY_ADDRESS, QUERY_LENGTH) != 0 ||
            user_read(QUERY_ADDRESS, &returned, sizeof(returned)) != 0 ||
            user_read(QUERY_LENGTH, &length, sizeof(length)) != 0)
        return false;
    return length == sizeof(returned) && returned.pid == pid &&
            returned.uid == uid && returned.gid == gid;
}

static void test_preaccept_peercred_snapshots(void) {
    struct fixture fixture;
    if (!fixture_base_init(&fixture, false, false)) {
        EXPECT(false, "accept 前 SO_PEERCRED 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    static const byte_t listener_name[] = {
        0, 'p', 'e', 'e', 'r', 'c', 'r', 'e', 'd', 0,
        's', 't', 'r', 'e', 'a', 'm',
    };
    struct guest_unix_name listener_address =
            guest_unix_name(listener_name, sizeof(listener_name));
    fixture.task.pid = 4242;
    set_effective_credentials(&fixture, 1101, 1201);
    bool listening = bind_unix_stream(&fixture,
                    fixture.listener_number, &listener_address) &&
            sys_listen(fixture.listener_number, 4) == 0;
    EXPECT(listening, "以第一组凭据启动 Unix stream listener");
    if (!listening) {
        fixture_destroy(&fixture);
        return;
    }

    set_effective_credentials(&fixture, 2101, 2201);
    bool connected = connect_unix_stream(&fixture,
            fixture.connector_number, &listener_address);
    EXPECT(connected, "变更调用者凭据后完成尚未 accept 的连接");
    if (!connected) {
        fixture_destroy(&fixture);
        return;
    }
    EXPECT(guest_peercred_matches(&fixture, fixture.connector_number,
                    4242, 1101, 1201),
            "connector 在 accept 前取得 listen 时的 listener 凭据快照");

    set_effective_credentials(&fixture, 3101, 3201);
    fd_t accepted_number = sys_accept(fixture.listener_number, 0, 0);
    EXPECT(accepted_number == 2, "凭据快照连接 accept 成功");
    if (accepted_number < 0) {
        fixture_destroy(&fixture);
        return;
    }
    EXPECT(guest_peercred_matches(&fixture, fixture.connector_number,
                    4242, 1101, 1201),
            "accept 不会用接收时凭据改写 connector 的 listener 快照");
    EXPECT(guest_peercred_matches(&fixture, accepted_number,
                    4242, 2101, 2201),
            "accepted peer 取得 connect 时的 connector 凭据快照");

    EXPECT(f_close_task(&fixture.task, fixture.listener_number) == 0,
            "关闭凭据快照 listener 成功");
    fixture.listener_number = -1;
    byte_t sent = 0x6b;
    byte_t received = 0;
    EXPECT(user_write(SEND_PAYLOAD, &sent, sizeof(sent)) == 0 &&
                    sys_sendto(fixture.connector_number,
                            SEND_PAYLOAD, sizeof(sent), 0, 0, 0) ==
                            sizeof(sent) &&
                    sys_recvfrom(accepted_number,
                            RECEIVE_PAYLOAD, sizeof(received), 0, 0, 0) ==
                            sizeof(received) &&
                    user_read(RECEIVE_PAYLOAD,
                            &received, sizeof(received)) == 0 &&
                    received == sent,
            "listener 关闭后已建立连接仍可接收数据");
    EXPECT(guest_peercred_matches(&fixture, fixture.connector_number,
                    4242, 1101, 1201) &&
            guest_peercred_matches(&fixture, accepted_number,
                    4242, 2101, 2201),
            "listener 关闭并完成接收后两端凭据快照保持稳定");

    EXPECT(f_close_task(&fixture.task, fixture.connector_number) == 0,
            "关闭凭据快照 connector 成功");
    fixture.connector_number = -1;
    fixture.connector = NULL;
    EXPECT(guest_peercred_matches(&fixture, accepted_number,
                    4242, 2101, 2201),
            "connector 关闭后 accepted peer 仍保留对端凭据快照");
    EXPECT(f_close_task(&fixture.task, accepted_number) == 0,
            "关闭凭据快照 accepted peer 成功");
    fixture_destroy(&fixture);
}

static void test_pathname_name_snapshots(void) {
    struct fixture fixture;
    if (!fixture_base_init(&fixture, false, true)) {
        EXPECT(false, "pathname 名称快照夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    unsigned sequence = atomic_fetch_add_explicit(
            &unique_name, 1, memory_order_relaxed);
    char listener_name[64];
    char connector_name[64];
    int listener_size = snprintf(listener_name, sizeof(listener_name),
            "/stream-listener-%ld-%u", (long) getpid(), sequence);
    int connector_size = snprintf(connector_name, sizeof(connector_name),
            "/stream-connector-%ld-%u", (long) getpid(), sequence);
    bool names_valid = listener_size > 0 && connector_size > 0 &&
            (size_t) listener_size < sizeof(listener_name) &&
            (size_t) connector_size < sizeof(connector_name);
    struct guest_unix_name listener_address = guest_unix_name(
            listener_name, names_valid ? (size_t) listener_size + 1 : 0);
    struct guest_unix_name connector_address = guest_unix_name(
            connector_name, names_valid ? (size_t) connector_size + 1 : 0);
    bool connected = names_valid &&
            bind_unix_stream(&fixture, fixture.listener_number,
                    &listener_address) &&
            bind_unix_stream(&fixture, fixture.connector_number,
                    &connector_address) &&
            sys_listen(fixture.listener_number, 4) == 0 &&
            connect_unix_stream(&fixture, fixture.connector_number,
                    &listener_address);
    EXPECT(connected, "建立双方具名的 pathname stream 连接");
    if (!connected) {
        fixture_destroy(&fixture);
        return;
    }

    EXPECT(guest_name_query_matches(&fixture, fixture.connector_number,
                    true, listener_name, (size_t) listener_size + 1),
            "pathname connector 在 accept 前取得 listener 对端名");
    fd_t accepted_number = sys_accept(fixture.listener_number, 0, 0);
    EXPECT(accepted_number == 2,
            "pathname stream accept 安装成功");
    if (accepted_number < 0) {
        fixture_destroy(&fixture);
        return;
    }
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    false, listener_name, (size_t) listener_size + 1),
            "pathname accepted getsockname 等于 listener 名称");
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    true, connector_name, (size_t) connector_size + 1),
            "pathname accepted getpeername 等于 connector 显式本地名");
    EXPECT(guest_name_query_matches(&fixture, fixture.connector_number,
                    false, connector_name, (size_t) connector_size + 1),
            "pathname connector getsockname 保留显式本地名");

    EXPECT(f_close_task(&fixture.task, fixture.listener_number) == 0,
            "关闭 pathname listener 成功");
    fixture.listener_number = -1;
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    false, listener_name, (size_t) listener_size + 1) &&
            guest_name_query_matches(&fixture, accepted_number,
                    true, connector_name, (size_t) connector_size + 1) &&
            guest_name_query_matches(&fixture, fixture.connector_number,
                    false, connector_name, (size_t) connector_size + 1) &&
            guest_name_query_matches(&fixture, fixture.connector_number,
                    true, listener_name, (size_t) listener_size + 1),
            "listener 关闭后 pathname 已建立两端保留本地名与对端名");

    EXPECT(f_close_task(&fixture.task, fixture.connector_number) == 0,
            "关闭 pathname connector 成功");
    fixture.connector_number = -1;
    fixture.connector = NULL;
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    false, listener_name, (size_t) listener_size + 1) &&
            guest_name_query_matches(&fixture, accepted_number,
                    true, connector_name, (size_t) connector_size + 1),
            "connector 关闭后 accepted 保留 pathname 本地与 peer 快照");
    EXPECT(f_close_task(&fixture.task, accepted_number) == 0,
            "清理 pathname accepted 端");
    fixture_destroy(&fixture);
}

static void test_binary_abstract_anonymous_name_snapshots(void) {
    struct fixture fixture;
    if (!fixture_base_init(&fixture, false, false)) {
        EXPECT(false, "二进制 abstract 名称快照夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    static const byte_t listener_name[] = {
        0, 's', 0, 0xff, 't', 'r', 'e', 'a', 'm',
    };
    struct guest_unix_name listener_address =
            guest_unix_name(listener_name, sizeof(listener_name));
    bool connected = bind_unix_stream(&fixture,
                    fixture.listener_number, &listener_address) &&
            sys_listen(fixture.listener_number, 4) == 0 &&
            connect_unix_stream(&fixture, fixture.connector_number,
                    &listener_address);
    EXPECT(connected, "建立匿名 connector 到二进制 abstract listener 的连接");
    if (!connected) {
        fixture_destroy(&fixture);
        return;
    }

    EXPECT(guest_name_query_matches(&fixture, fixture.connector_number,
                    true, listener_name, sizeof(listener_name)),
            "匿名 connector 在 accept 前取得二进制 abstract 对端名");
    fd_t accepted_number = sys_accept(fixture.listener_number, 0, 0);
    EXPECT(accepted_number == 2,
            "二进制 abstract stream accept 安装成功");
    if (accepted_number < 0) {
        fixture_destroy(&fixture);
        return;
    }
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    false, listener_name, sizeof(listener_name)),
            "二进制 abstract accepted getsockname 等于 listener 名称");
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    true, NULL, 0),
            "accepted getpeername 将匿名 connector 回报为 family-only");
    EXPECT(guest_name_query_matches(&fixture, fixture.connector_number,
                    false, NULL, 0),
            "匿名 connector getsockname 为 family-only");

    EXPECT(f_close_task(&fixture.task, fixture.listener_number) == 0,
            "关闭二进制 abstract listener 成功");
    fixture.listener_number = -1;
    EXPECT(guest_name_query_matches(&fixture, accepted_number,
                    false, listener_name, sizeof(listener_name)) &&
            guest_name_query_matches(&fixture, accepted_number,
                    true, NULL, 0) &&
            guest_name_query_matches(&fixture, fixture.connector_number,
                    false, NULL, 0) &&
            guest_name_query_matches(&fixture, fixture.connector_number,
                    true, listener_name, sizeof(listener_name)),
            "listener 关闭后二进制 abstract 两端保留本地名与对端名");

    EXPECT(f_close_task(&fixture.task, accepted_number) == 0,
            "关闭二进制 abstract accepted 端成功");
    EXPECT(guest_name_query_matches(&fixture, fixture.connector_number,
                    false, NULL, 0) &&
            guest_name_query_matches(&fixture, fixture.connector_number,
                    true, listener_name, sizeof(listener_name)),
            "accepted 关闭后 connector 保留匿名本地名与 abstract peer 快照");
    fixture_destroy(&fixture);
}

static bool prepare_sendmsg(struct fixture *fixture) {
    const byte_t payload = 0x5a;
    const struct iovec_ iov = {
        .base = SEND_PAYLOAD,
        .len = sizeof(payload),
    };
    const struct guest_rights rights = {
        .length = sizeof(rights),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
        .fd = fixture->passed_number,
    };
    const struct msghdr_ header = {
        .msg_iov = SEND_IOV,
        .msg_iovlen = 1,
        .msg_control = SEND_CONTROL,
        .msg_controllen = sizeof(rights),
    };
    return user_write(SEND_PAYLOAD, &payload, sizeof(payload)) == 0 &&
            user_write(SEND_IOV, &iov, sizeof(iov)) == 0 &&
            user_write(SEND_CONTROL, &rights, sizeof(rights)) == 0 &&
            user_write(SEND_HEADER, &header, sizeof(header)) == 0;
}

static fd_t receive_rights(fd_t accepted_number) {
    const struct iovec_ iov = {
        .base = RECEIVE_PAYLOAD,
        .len = 1,
    };
    const struct msghdr_ header = {
        .msg_iov = RECEIVE_IOV,
        .msg_iovlen = 1,
        .msg_control = RECEIVE_CONTROL,
        .msg_controllen = sizeof(struct guest_rights),
    };
    if (user_write(RECEIVE_IOV, &iov, sizeof(iov)) != 0 ||
            user_write(RECEIVE_HEADER, &header, sizeof(header)) != 0 ||
            sys_recvmsg(accepted_number, RECEIVE_HEADER, 0) != 1)
        return -1;
    byte_t payload = 0;
    struct guest_rights rights = {0};
    if (user_read(RECEIVE_PAYLOAD, &payload, sizeof(payload)) != 0 ||
            user_read(RECEIVE_CONTROL, &rights, sizeof(rights)) != 0 ||
            payload != 0x5a || rights.length != sizeof(rights) ||
            rights.level != SOL_SOCKET_ || rights.type != SCM_RIGHTS_)
        return -1;
    return rights.fd;
}

static bool wait_for_completion(atomic_bool *finished, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(finished, memory_order_acquire))
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(finished, memory_order_acquire);
}

static void test_accept_failure_releases_transit_and_rights(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture, true)) {
        EXPECT(false, "accept 失败夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    EXPECT(atomic_load_explicit(&fixture.connector->refcount,
                   memory_order_relaxed) == 2,
            "connect 返回后仅表项与 in_transit 两份 connector 引用");
    EXPECT(prepare_sendmsg(&fixture) &&
                    sys_sendmsg(fixture.connector_number,
                            SEND_HEADER, 0) == 1,
            "accept 前发送 SCM_RIGHTS 成功");
    EXPECT(atomic_load_explicit(&fixture.passed->refcount,
                   memory_order_relaxed) == 2,
            "待交付 SCM_RIGHTS 恰好持有一份传递对象引用");

    lock(&fixture.group.lock);
    fixture.group.limits[RLIMIT_NOFILE_].cur = 3;
    unlock(&fixture.group.lock);
    EXPECT(sys_accept(fixture.listener_number, 0, 0) == _EMFILE,
            "满 fd 表拒绝 accept 安装");
    EXPECT(atomic_load_explicit(&fixture.connector->refcount,
                   memory_order_relaxed) == 1,
            "accept 安装失败消费 in_transit 引用");
    lock(&fixture.connector->lock);
    bool pending_empty = list_empty(
            &fixture.connector->socket.unix_pending_scm);
    unlock(&fixture.connector->lock);
    EXPECT(pending_empty &&
                    atomic_load_explicit(&fixture.passed->refcount,
                            memory_order_relaxed) == 1,
            "accept 安装失败丢弃不可交付 SCM_RIGHTS 及其引用");
    fixture_destroy(&fixture);
    EXPECT(fixture.passed_close.calls == 1,
            "accept 失败夹具最终只关闭传递对象一次");
}

static void test_listener_close_releases_unaccepted_transit(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture, false)) {
        EXPECT(false, "listener 关闭夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    struct fd *connector = fixture.connector;
    int connector_host_fd = connector->real_fd;
    EXPECT(atomic_load_explicit(&connector->refcount,
                   memory_order_relaxed) == 2,
            "未 accept 连接保有一份 in_transit 引用");
    EXPECT(f_close_task(&fixture.task, fixture.listener_number) == 0,
            "关闭含未 accept 连接的 listener 成功");
    fixture.listener_number = -1;
    EXPECT(f_close_task(&fixture.task, fixture.connector_number) == 0,
            "关闭未 accept 的 connector 表项成功");
    fixture.connector_number = -1;
    errno = 0;
    bool connector_closed =
            fcntl(connector_host_fd, F_GETFD) == -1 && errno == EBADF;
    EXPECT(connector_closed,
            "listener 关闭后会撤销未消费 in_transit 并最终关闭 connector");

    // 当前缺口会遗留一份在途引用；显式消费，避免诊断测试自身污染 sanitizer。
    if (!connector_closed)
        fd_close(connector);
    fixture.connector = NULL;
    fixture_destroy(&fixture);
}

static void test_preaccept_rights_reach_accepted_peer(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture, true)) {
        EXPECT(false, "成功握手夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    EXPECT(prepare_sendmsg(&fixture) &&
                    sys_sendmsg(fixture.connector_number,
                            SEND_HEADER, 0) == 1,
            "accept 前 SCM_RIGHTS 与 payload 同步入队");
    fd_t accepted_number = sys_accept(fixture.listener_number, 0, 0);
    EXPECT(accepted_number == 3,
            "accept 成功安装最低空闲描述符");
    EXPECT(atomic_load_explicit(&fixture.connector->refcount,
                   memory_order_relaxed) == 1,
            "成功 accept 消费 in_transit 引用");
    lock(&fixture.connector->lock);
    bool pending_empty = list_empty(
            &fixture.connector->socket.unix_pending_scm);
    unlock(&fixture.connector->lock);
    EXPECT(pending_empty, "成功 accept 原子迁移 accept 前 SCM 队列");

    fd_t received_number = accepted_number >= 0 ?
            receive_rights(accepted_number) : -1;
    EXPECT(received_number == 4 &&
                    f_get_task(&fixture.task, received_number) ==
                            fixture.passed,
            "accepted peer 收到原 payload 与同一传递对象");
    if (received_number >= 0)
        EXPECT(f_close_task(&fixture.task, received_number) == 0,
                "关闭接收的 SCM_RIGHTS 描述符成功");
    if (accepted_number >= 0)
        EXPECT(f_close_task(&fixture.task, accepted_number) == 0,
                "关闭 accepted peer 成功");
    fixture_destroy(&fixture);
    EXPECT(fixture.passed_close.calls == 1,
            "成功传递后底层对象仍只关闭一次");
}

struct close_call {
    struct fixture *fixture;
    fd_t fd;
    atomic_bool started;
    atomic_bool finished;
    int_t result;
};

static void *run_close(void *opaque) {
    struct close_call *call = opaque;
    current = &call->fixture->task;
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = f_close_task(&call->fixture->task, call->fd);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

struct alternate_send_call {
    struct task *task;
    fd_t socket;
    atomic_bool finished;
    int_t result;
};

static void *run_alternate_send(void *opaque) {
    struct alternate_send_call *call = opaque;
    current = call->task;
    call->result = sys_sendmsg(call->socket, SEND_HEADER, 0);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

struct blocking_scm_send_call {
    struct fixture *fixture;
    atomic_bool started;
    atomic_bool finished;
    int_t result;
};

static void *run_blocking_scm_send(void *opaque) {
    struct blocking_scm_send_call *call = opaque;
    current = &call->fixture->task;
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = sys_sendmsg(call->fixture->connector_number,
            SEND_HEADER, MSG_NOSIGNAL_);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

struct concurrent_call {
    struct fixture *fixture;
    atomic_bool *start;
    atomic_bool finished;
    bool send;
    int_t result;
};

struct connect_race_call {
    struct fixture *fixture;
    const struct guest_unix_name *address;
    atomic_uint *ready;
    atomic_bool *start;
    int_t result;
};

static void *run_connect_race(void *opaque) {
    struct connect_race_call *call = opaque;
    current = &call->fixture->task;
    struct socket_ref socket = {0};
    call->result = socket_ref_get_task(&call->fixture->task,
            call->fixture->connector_number, &socket);
    atomic_fetch_add_explicit(call->ready, 1, memory_order_release);
    const struct timespec interval = {.tv_nsec = 1000000};
    while (!atomic_load_explicit(call->start, memory_order_acquire))
        nanosleep(&interval, NULL);
    if (call->result == 0) {
        call->result = socket_connect_ref_task(&call->fixture->task,
                &socket, &call->address->storage,
                call->address->length);
        socket_ref_release(&socket);
    }
    current = NULL;
    return NULL;
}

static void *run_concurrent_call(void *opaque) {
    struct concurrent_call *call = opaque;
    current = &call->fixture->task;
    const struct timespec interval = {.tv_nsec = 1000000};
    while (!atomic_load_explicit(call->start, memory_order_acquire))
        nanosleep(&interval, NULL);
    call->result = call->send ?
            sys_sendmsg(call->fixture->connector_number, SEND_HEADER, 0) :
            sys_accept(call->fixture->listener_number, 0, 0);
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void test_peer_close_does_not_park_rights_as_preaccept(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture, true)) {
        EXPECT(false, "peer 关闭竞态夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    fd_t accepted_number = sys_accept(fixture.listener_number, 0, 0);
    struct fd *accepted = accepted_number >= 0 ?
            f_get_task(&fixture.task, accepted_number) : NULL;
    if (accepted_number != 3 || accepted == NULL ||
            !prepare_sendmsg(&fixture)) {
        EXPECT(false, "peer 关闭竞态建立已 accept 连接");
        fixture_destroy(&fixture);
        return;
    }

    // 使用独立 fd 表模拟 fork 后的共享 socket，使 send 不受 close 的表锁阻塞。
    struct task sender = {0};
    sender.group = &fixture.group;
    sender.files = fdtable_copy(fixture.task.files);
    task_set_mm(&sender, fixture.task.mm);
    if (IS_ERR(sender.files) ||
            f_close_task(&sender, accepted_number) != 0) {
        EXPECT(false, "peer 关闭竞态建立独立发送 fd 表");
        if (!IS_ERR(sender.files))
            fdtable_release(sender.files);
        fixture_destroy(&fixture);
        return;
    }

    // 让最后一个 close 在清除弱 peer 后停在队列清理点。
    lock(&accepted->lock);
    struct close_call close_call = {
        .fixture = &fixture,
        .fd = accepted_number,
        .result = _EIO,
    };
    atomic_init(&close_call.started, false);
    atomic_init(&close_call.finished, false);
    pthread_t close_thread;
    EXPECT(pthread_create(&close_thread, NULL,
                    run_close, &close_call) == 0,
            "peer 关闭线程启动成功");
    EXPECT(wait_for_completion(&close_call.started, 1000),
            "peer 关闭线程进入关闭路径");
    const struct timespec close_interval = {.tv_nsec = 20000000};
    nanosleep(&close_interval, NULL);

    struct alternate_send_call send_call = {
        .task = &sender,
        .socket = fixture.connector_number,
        .result = _EIO,
    };
    atomic_init(&send_call.finished, false);
    pthread_t send_thread;
    bool send_started = pthread_create(&send_thread, NULL,
            run_alternate_send, &send_call) == 0;
    EXPECT(send_started, "peer 关闭窗口的 sendmsg 线程启动成功");
    bool send_finished = send_started &&
            !atomic_load_explicit(&close_call.finished,
                    memory_order_acquire) &&
            wait_for_completion(&send_call.finished, 500);
    if (send_finished) {
        lock(&fixture.connector->lock);
        bool pending_empty = list_empty(
                &fixture.connector->socket.unix_pending_scm);
        unlock(&fixture.connector->lock);
        EXPECT(pending_empty &&
                        atomic_load_explicit(&fixture.passed->refcount,
                                memory_order_relaxed) == 2,
                "已 accept peer 关闭竞态不把 SCM_RIGHTS 误存为待 accept");
    }
    unlock(&accepted->lock);
    if (send_started)
        EXPECT(pthread_join(send_thread, NULL) == 0,
                "peer 关闭窗口 sendmsg 线程回收成功");
    EXPECT(pthread_join(close_thread, NULL) == 0 &&
                    close_call.result == 0,
            "peer 关闭线程完成并回收成功");
    EXPECT(send_finished,
            "peer 清除后且 host fd 关闭前的 sendmsg 窗口可复现");
    fdtable_release(sender.files);
    fixture_destroy(&fixture);
    EXPECT(fixture.passed_close.calls == 1,
            "peer 关闭竞态后传递对象只关闭一次");
}

static void test_blocked_sendmsg_does_not_hold_peer_lock(void) {
    struct fixture fixture;
    if (!fixture_init(&fixture, true)) {
        EXPECT(false, "阻塞 sendmsg 夹具初始化成功");
        fixture_destroy(&fixture);
        return;
    }
    fd_t accepted_number = sys_accept(fixture.listener_number, 0, 0);
    struct fd *receiver = accepted_number >= 0 ?
            f_get_task(&fixture.task, accepted_number) : NULL;
    if (accepted_number != 3 || fixture.connector == NULL ||
            receiver == NULL || !prepare_sendmsg(&fixture)) {
        EXPECT(false, "阻塞 sendmsg 建立已 accept 连接");
        fixture_destroy(&fixture);
        return;
    }
    int receiver_host_fd = receiver->real_fd;
    EXPECT(f_close_task(&fixture.task, fixture.listener_number) == 0,
            "锁序回归关闭已无用的 listener");
    fixture.listener_number = -1;

    // 人为占住接收端锁：新实现会先完成 host I/O，再在发布内部 SCM 时等待。
    lock(&receiver->lock);
    struct blocking_scm_send_call send_call = {
        .fixture = &fixture,
        .result = _EIO,
    };
    atomic_init(&send_call.started, false);
    atomic_init(&send_call.finished, false);
    pthread_t send_thread;
    bool send_started = pthread_create(&send_thread, NULL,
                    run_blocking_scm_send, &send_call) == 0;
    EXPECT(send_started && wait_for_completion(&send_call.started, 1000),
            "持有 peer 锁时启动 SCM sendmsg");
    const struct timespec interval = {.tv_nsec = 20000000};
    nanosleep(&interval, NULL);

    byte_t peek_payload = 0;
    struct iovec peek_iov = {
        .iov_base = &peek_payload,
        .iov_len = sizeof(peek_payload),
    };
    char peek_control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr peek_message = {
        .msg_iov = &peek_iov,
        .msg_iovlen = 1,
        .msg_control = peek_control,
        .msg_controllen = sizeof(peek_control),
    };
    ssize_t peeked = recvmsg(receiver_host_fd, &peek_message,
            MSG_PEEK | MSG_DONTWAIT);
    struct cmsghdr *peek_header = CMSG_FIRSTHDR(&peek_message);
    bool host_visible = peeked == 1 && peek_payload == 0x5a &&
            peek_header != NULL &&
            peek_header->cmsg_level == SOL_SOCKET &&
            peek_header->cmsg_type == SCM_RIGHTS &&
            peek_header->cmsg_len >= CMSG_LEN(sizeof(int));
    if (host_visible) {
        int peek_fd;
        memcpy(&peek_fd, CMSG_DATA(peek_header), sizeof(peek_fd));
        close(peek_fd);
    }
    EXPECT(host_visible && !atomic_load_explicit(&send_call.finished,
                    memory_order_acquire),
            "sendmsg 不持 peer fd 锁完成 host I/O，再等待内部 SCM 发布");

    struct close_call close_call = {
        .fixture = &fixture,
        .fd = accepted_number,
        .result = _EIO,
    };
    atomic_init(&close_call.started, false);
    atomic_init(&close_call.finished, false);
    pthread_t close_thread;
    bool close_started = host_visible &&
            pthread_create(&close_thread, NULL,
                    run_close, &close_call) == 0;
    EXPECT(close_started && wait_for_completion(&close_call.started, 1000),
            "host I/O 后、SCM 发布前启动并发 peer close");
    unlock(&receiver->lock);

    bool completed = send_started && close_started &&
            wait_for_completion(&send_call.finished, 1000) &&
            wait_for_completion(&close_call.finished, 1000);

    if (!completed) {
        // 诊断失败时解除 host 阻塞，确保回归自身不会遗留线程。
        (void) shutdown(fixture.connector->real_fd, SHUT_RDWR);
        (void) shutdown(receiver_host_fd, SHUT_RDWR);
        (void) wait_for_completion(&send_call.finished, 1000);
        (void) wait_for_completion(&close_call.finished, 1000);
    }
    if (send_started)
        EXPECT(pthread_join(send_thread, NULL) == 0,
                "回收阻塞 sendmsg 线程");
    if (close_started)
        EXPECT(pthread_join(close_thread, NULL) == 0,
                "回收阻塞 sendmsg 的 peer close 线程");
    EXPECT(completed && close_call.result == 0 && send_call.result == 1,
            "SCM 发布与 peer close 在释放锁后完成且无锁序死锁");
    EXPECT(atomic_load_explicit(&fixture.passed->refcount,
                    memory_order_relaxed) == 1,
            "阻塞 sendmsg 失败回滚唯一 SCM 传递引用");
    fixture_destroy(&fixture);
    EXPECT(fixture.passed_close.calls == 1,
            "阻塞 sendmsg 竞态后传递对象只析构一次");
}

static void test_accept_sendmsg_lock_order(void) {
    for (unsigned iteration = 0; iteration < 24; iteration++) {
        struct fixture fixture;
        if (!fixture_init(&fixture, true)) {
            EXPECT(false, "并发握手夹具初始化成功");
            fixture_destroy(&fixture);
            return;
        }
        if (!prepare_sendmsg(&fixture)) {
            EXPECT(false, "并发发送消息写入 guest 内存");
            fixture_destroy(&fixture);
            return;
        }
        atomic_bool start;
        atomic_init(&start, false);
        struct concurrent_call calls[2] = {
            {.fixture = &fixture, .start = &start, .send = true,
                    .result = _EIO},
            {.fixture = &fixture, .start = &start, .send = false,
                    .result = _EIO},
        };
        atomic_init(&calls[0].finished, false);
        atomic_init(&calls[1].finished, false);
        pthread_t threads[2];
        bool threads_started =
                pthread_create(&threads[0], NULL,
                        run_concurrent_call, &calls[0]) == 0 &&
                pthread_create(&threads[1], NULL,
                        run_concurrent_call, &calls[1]) == 0;
        EXPECT(threads_started, "并发 sendmsg/accept 线程启动成功");
        if (!threads_started) {
            atomic_store_explicit(&start, true, memory_order_release);
            fixture_destroy(&fixture);
            return;
        }
        atomic_store_explicit(&start, true, memory_order_release);
        EXPECT(pthread_join(threads[0], NULL) == 0 &&
                        pthread_join(threads[1], NULL) == 0,
                "并发 sendmsg/accept 无锁序死锁并及时返回");
        EXPECT(calls[0].result == 1 && calls[1].result == 3,
                "并发 sendmsg/accept 均完成预期操作");
        fd_t received_number = calls[1].result >= 0 ?
                receive_rights(calls[1].result) : -1;
        EXPECT(received_number == 4,
                "并发握手没有丢失 SCM_RIGHTS");
        if (received_number >= 0)
            f_close_task(&fixture.task, received_number);
        if (calls[1].result >= 0)
            f_close_task(&fixture.task, calls[1].result);
        fixture_destroy(&fixture);
        EXPECT(fixture.passed_close.calls == 1,
                "并发握手后传递对象只关闭一次");
    }
}

static void test_concurrent_connect_and_preaccept_close(void) {
    for (unsigned iteration = 0; iteration < 32; iteration++) {
        struct fixture fixture;
        if (!fixture_base_init(&fixture, false, false)) {
            EXPECT(false, "并发 connect 夹具初始化成功");
            fixture_destroy(&fixture);
            return;
        }
        byte_t name[64] = {0};
        unsigned sequence = atomic_fetch_add_explicit(
                &unique_name, 1, memory_order_relaxed);
        int written = snprintf((char *) name + 1, sizeof(name) - 1,
                "connect-race-%ld-%u", (long) getpid(), sequence);
        struct guest_unix_name address = guest_unix_name(
                name, 1 + (size_t) written);
        bool listening = written > 0 &&
                (size_t) written < sizeof(name) - 1 &&
                bind_unix_stream(&fixture,
                        fixture.listener_number, &address) &&
                sys_listen(fixture.listener_number, 4) == 0;
        EXPECT(listening, "并发 connect listener 启动成功");
        if (!listening) {
            fixture_destroy(&fixture);
            return;
        }

        atomic_uint ready;
        atomic_bool start;
        atomic_init(&ready, 0);
        atomic_init(&start, false);
        struct connect_race_call calls[2] = {
            {.fixture = &fixture, .address = &address,
                    .ready = &ready, .start = &start, .result = _EIO},
            {.fixture = &fixture, .address = &address,
                    .ready = &ready, .start = &start, .result = _EIO},
        };
        pthread_t threads[2];
        bool started = pthread_create(&threads[0], NULL,
                               run_connect_race, &calls[0]) == 0 &&
                pthread_create(&threads[1], NULL,
                        run_connect_race, &calls[1]) == 0;
        EXPECT(started, "两个并发 connect 线程启动成功");
        if (!started) {
            atomic_store_explicit(&start, true, memory_order_release);
            fixture_destroy(&fixture);
            return;
        }
        const struct timespec interval = {.tv_nsec = 1000000};
        for (unsigned elapsed = 0; elapsed < 1000 &&
                atomic_load_explicit(&ready,
                        memory_order_acquire) != 2; elapsed++)
            nanosleep(&interval, NULL);
        bool both_ready = atomic_load_explicit(
                &ready, memory_order_acquire) == 2;
        EXPECT(both_ready, "两个并发 connect 均取得 retained socket");
        atomic_store_explicit(&start, true, memory_order_release);
        EXPECT(pthread_join(threads[0], NULL) == 0 &&
                        pthread_join(threads[1], NULL) == 0,
                "两个并发 connect 均及时返回");
        unsigned successes = (calls[0].result == 0) +
                (calls[1].result == 0);
        int_t other = calls[0].result == 0 ?
                calls[1].result : calls[0].result;
        EXPECT(both_ready && successes == 1 &&
                        (other == _EALREADY || other == _EISCONN),
                "同一 socket 只发布一个连接尝试及一个稳定重复错误");
        if (!both_ready || successes != 1 ||
                (other != _EALREADY && other != _EISCONN)) {
            fixture_destroy(&fixture);
            continue;
        }

        EXPECT(f_close_task(&fixture.task,
                        fixture.connector_number) == 0,
                "accept 前关闭 connector 表项成功");
        fixture.connector_number = -1;
        fixture.connector = NULL;
        fd_t accepted = sys_accept(fixture.listener_number, 0, 0);
        EXPECT(accepted >= 0,
                "connector 表项先关闭后仍可消费已排队连接");
        if (accepted >= 0) {
            EXPECT(sys_recvfrom(accepted,
                            RECEIVE_PAYLOAD, 1, 0, 0, 0) == 0,
                    "accept 消费 pending 强引用后 accepted 端立即看到 EOF");
            EXPECT(f_close_task(&fixture.task, accepted) == 0,
                    "并发 connect accepted fd 清理成功");
        }
        fixture_destroy(&fixture);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--successful-paths") == 0) {
        test_preaccept_peercred_snapshots();
        test_preaccept_rights_reach_accepted_peer();
        test_accept_sendmsg_lock_order();
        return failures == 0 ? 0 : 1;
    }
    test_accept_failure_releases_transit_and_rights();
    test_listener_close_releases_unaccepted_transit();
    test_pathname_name_snapshots();
    test_binary_abstract_anonymous_name_snapshots();
    test_preaccept_peercred_snapshots();
    test_preaccept_rights_reach_accepted_peer();
    test_peer_close_does_not_park_rights_as_preaccept();
    test_blocked_sendmsg_does_not_hold_peer_lock();
    test_accept_sendmsg_lock_order();
    test_concurrent_connect_and_preaccept_close();
    if (failures != 0)
        fprintf(stderr, "AF_UNIX stream 握手测试共发现 %d 个失败。\n",
                failures);
    return failures == 0 ? 0 : 1;
}

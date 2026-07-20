#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abd789a0000)
#define USER_MEMORY_SIZE UINT32_C(0x30000)
#define USER_ACCESS_LOG_SIZE 32

#define SYS_SOCKET 198
#define SYS_SOCKETPAIR 199
#define SYS_BIND 200
#define SYS_LISTEN 201
#define SYS_ACCEPT 202
#define SYS_ACCEPT4 242

#define UNKNOWN_SOCKET_FLAG UINT32_C(0x40000000)
#define TEST_TIMEOUT_SECONDS 30

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 socket 监听测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct linux_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t address;
    byte_t zero[8];
};

_Static_assert(sizeof(struct linux_sockaddr_in) == 16,
        "Linux IPv4 sockaddr wire 必须为 16 字节");

struct user_access {
    qword_t address;
    dword_t size;
    dword_t access;
};

enum hook_kind {
    HOOK_NONE,
    HOOK_PAIR_RESERVATION,
    HOOK_PAIR_SECOND_WRITE,
    HOOK_ACCEPT_REUSE,
};

struct user_memory {
    byte_t *bytes;
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned read_calls;
    unsigned write_calls;
    struct user_access accesses[USER_ACCESS_LOG_SIZE];
    unsigned access_count;

    enum hook_kind hook;
    struct task *hook_task;
    qword_t hook_address;
    fd_t hook_fd;
    int hook_raw_fd;
    fd_t nested_fd;
    int replacement_closes;
    bool hook_fired;
    bool reserved_invisible;
    bool hook_succeeded;
    bool raw_open_during_hook;
};

struct fixture {
    struct task task;
    struct tgroup group;
    struct guest_linux_syscall_completion completion;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static int replacement_close(struct fd *fd) {
    int *closes = fd->data;
    (*closes)++;
    return 0;
}

static const struct fd_ops replacement_ops = {
    .close = replacement_close,
};

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] =
            "AArch64 socket 监听测试失败：超过硬超时\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void reset_access(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->fail_write_at = UINT64_MAX;
    memory->read_calls = 0;
    memory->write_calls = 0;
    memory->access_count = 0;
    memory->hook = HOOK_NONE;
    memory->hook_task = NULL;
    memory->hook_address = 0;
    memory->hook_fd = -1;
    memory->hook_raw_fd = -1;
    memory->nested_fd = -1;
    memory->hook_fired = false;
    memory->reserved_invisible = false;
    memory->hook_succeeded = false;
    memory->raw_open_during_hook = false;
}

static bool range_contains(qword_t address, dword_t size, qword_t target) {
    return size != 0 && target >= address && target - address < size;
}

static bool user_range(qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE)
        return false;
    qword_t relative = address - USER_BASE;
    if (relative > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
}

static void record_access(struct user_memory *memory,
        qword_t address, dword_t size, enum guest_memory_access access) {
    if (memory->access_count < USER_ACCESS_LOG_SIZE) {
        memory->accesses[memory->access_count] = (struct user_access) {
            .address = address,
            .size = size,
            .access = (dword_t) access,
        };
    }
    memory->access_count++;
}

static void set_fault(struct guest_linux_user_fault *fault,
        qword_t address, enum guest_memory_access access,
        enum guest_memory_fault_kind kind) {
    *fault = (struct guest_linux_user_fault) {
        .address = address,
        .access = (dword_t) access,
        .kind = (dword_t) kind,
    };
}

static struct fd *new_replacement(struct user_memory *memory) {
    struct fd *replacement = fd_create(&replacement_ops);
    if (replacement != NULL)
        replacement->data = &memory->replacement_closes;
    return replacement;
}

static void run_hook(struct user_memory *memory,
        qword_t address, dword_t size,
        enum guest_memory_access access, const void *source) {
    if (memory->hook_task == NULL || memory->hook_fired ||
            address != memory->hook_address || size != sizeof(dword_t))
        return;

    if ((memory->hook == HOOK_PAIR_RESERVATION ||
            memory->hook == HOOK_PAIR_SECOND_WRITE) &&
            access == GUEST_MEMORY_WRITE) {
        fd_t number;
        memcpy(&number, source, sizeof(number));
        memory->hook_fired = true;
        memory->reserved_invisible =
                f_get_task(memory->hook_task, number) == NULL &&
                f_close_task(memory->hook_task, number) == _EBADF &&
                f_get_task(memory->hook_task, 0) == NULL &&
                f_get_task(memory->hook_task, 1) == NULL;
        if (memory->hook == HOOK_PAIR_RESERVATION) {
            struct fd *replacement = new_replacement(memory);
            if (replacement == NULL)
                return;
            memory->nested_fd = f_install_task(
                    memory->hook_task, replacement, 0);
            memory->hook_succeeded = memory->nested_fd == 2;
        }
        return;
    }

    if (memory->hook == HOOK_ACCEPT_REUSE &&
            access == GUEST_MEMORY_READ) {
        memory->hook_fired = true;
        int close_result = f_close_task(
                memory->hook_task, memory->hook_fd);
        memory->raw_open_during_hook =
                fcntl(memory->hook_raw_fd, F_GETFD) >= 0;
        memory->reserved_invisible =
                f_get_task(memory->hook_task, 1) == NULL &&
                f_close_task(memory->hook_task, 1) == _EBADF;
        struct fd *replacement = new_replacement(memory);
        if (replacement == NULL)
            return;
        memory->nested_fd = f_install_task(
                memory->hook_task, replacement, 0);
        memory->hook_succeeded = close_result == 0 &&
                memory->nested_fd == memory->hook_fd;
    }
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    record_access(memory, address, size, GUEST_MEMORY_READ);
    memory->read_calls++;
    run_hook(memory, address, size, GUEST_MEMORY_READ, NULL);
    size_t offset;
    if (!user_range(address, size, &offset)) {
        set_fault(fault, address, GUEST_MEMORY_READ,
                GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        return false;
    }
    if (memory->fail_read_at != UINT64_MAX &&
            range_contains(address, size, memory->fail_read_at)) {
        set_fault(fault, memory->fail_read_at, GUEST_MEMORY_READ,
                GUEST_MEMORY_FAULT_UNMAPPED);
        return false;
    }
    memcpy(destination, memory->bytes + offset, size);
    set_fault(fault, address, GUEST_MEMORY_READ,
            GUEST_MEMORY_FAULT_NONE);
    return true;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    record_access(memory, address, size, GUEST_MEMORY_WRITE);
    memory->write_calls++;
    run_hook(memory, address, size, GUEST_MEMORY_WRITE, source);
    size_t offset;
    if (!user_range(address, size, &offset)) {
        set_fault(fault, address, GUEST_MEMORY_WRITE,
                GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        return false;
    }
    if (memory->fail_write_at != UINT64_MAX &&
            range_contains(address, size, memory->fail_write_at)) {
        size_t prefix = (size_t) (memory->fail_write_at - address);
        memcpy(memory->bytes + offset, source, prefix);
        set_fault(fault, memory->fail_write_at, GUEST_MEMORY_WRITE,
                GUEST_MEMORY_FAULT_UNMAPPED);
        return false;
    }
    memcpy(memory->bytes + offset, source, size);
    set_fault(fault, address, GUEST_MEMORY_WRITE,
            GUEST_MEMORY_FAULT_NONE);
    return true;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {64, 64};
    fixture->task.group = &fixture->group;
    fixture->task.sighand = sighand_new();
    fixture->task.files = fdtable_new(4);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->task.waiting_poll_notify_fd = -1;
    if (fixture->task.sighand == NULL || IS_ERR(fixture->task.files))
        return false;
    current = &fixture->task;
    lock(&mounts_lock);
    int mount_error = do_mount(&tmpfs, "", "", "", 0);
    unlock(&mounts_lock);
    fixture->task.fs = fs_info_new();
    return mount_error == 0 && fixture->task.fs != NULL;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    fdtable_release(fixture->task.files);
    fs_info_release(fixture->task.fs);
    sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    current = NULL;
}

static qword_t invoke(struct fixture *fixture, struct user_memory *memory,
        struct guest_linux_user_fault *fault, qword_t number,
        qword_t argument0, qword_t argument1, qword_t argument2,
        qword_t argument3, qword_t argument4, qword_t argument5) {
    fixture->completion.disposition = GUEST_LINUX_SYSCALL_RETURN;
    const struct guest_linux_syscall_context context = {
        .runtime_opaque = ish_aarch64_linux_syscall_service.runtime_opaque,
        .task_opaque = &fixture->task,
        .completion = &fixture->completion,
        .user = {
            .opaque = memory,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {
            argument0, argument1, argument2,
            argument3, argument4, argument5,
        },
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static qword_t put_user(struct user_memory *memory,
        size_t offset, const void *value, size_t size) {
    memcpy(memory->bytes + offset, value, size);
    return USER_BASE + offset;
}

static void get_user(const struct user_memory *memory,
        size_t offset, void *value, size_t size) {
    memcpy(value, memory->bytes + offset, size);
}

static bool access_is(const struct user_memory *memory, unsigned index,
        qword_t address, dword_t size, enum guest_memory_access access) {
    return index < memory->access_count &&
            memory->accesses[index].address == address &&
            memory->accesses[index].size == size &&
            memory->accesses[index].access == (dword_t) access;
}

static struct linux_sockaddr_in linux_loopback(uint16_t port) {
    return (struct linux_sockaddr_in) {
        .family = AF_INET_,
        .port = port,
        .address = htonl(INADDR_LOOPBACK),
    };
}

static int create_guest_socket(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t domain, dword_t type, dword_t protocol) {
    return (int) (sqword_t) invoke(fixture, memory, fault, SYS_SOCKET,
            domain, type, protocol, 0, 0, 0);
}

static int install_ordinary(struct fixture *fixture,
        struct user_memory *memory) {
    struct fd *ordinary = new_replacement(memory);
    return ordinary == NULL ? _ENOMEM :
            f_install_task(&fixture->task, ordinary, 0);
}

static bool wait_for_event(int fd, short events) {
    struct pollfd descriptor = {.fd = fd, .events = events};
    int result;
    do {
        result = poll(&descriptor, 1, 2000);
    } while (result < 0 && errno == EINTR);
    return result == 1 &&
            (descriptor.revents & (events | POLLERR | POLLHUP)) != 0;
}

static int connect_host(struct fixture *fixture, fd_t listener,
        const struct sockaddr_in *address) {
    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0)
        return -1;
    int flags = fcntl(client, F_GETFL);
    if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;
    if (connect(client, (const struct sockaddr *) address,
                sizeof(*address)) < 0 && errno != EINPROGRESS)
        goto fail;
    if (!wait_for_event(client, POLLOUT))
        goto fail;
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (getsockopt(client, SOL_SOCKET, SO_ERROR,
                &error, &error_length) < 0 || error != 0)
        goto fail;
    if (fcntl(client, F_SETFL, flags & ~O_NONBLOCK) < 0)
        goto fail;
    struct fd *server = f_get_task(&fixture->task, listener);
    if (server == NULL || !wait_for_event(server->real_fd, POLLIN))
        goto fail;
    return client;

fail:
    close(client);
    return -1;
}

static int make_listener(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        struct sockaddr_in *host_address) {
    int listener = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    if (listener < 0)
        return listener;
    struct linux_sockaddr_in wire = linux_loopback(0);
    qword_t pointer = put_user(memory, 0x1000, &wire, sizeof(wire));
    if (invoke(fixture, memory, fault, SYS_BIND, listener,
                pointer, sizeof(wire), 0, 0, 0) != 0)
        goto fail;
    struct fd *socket = f_get_task(&fixture->task, listener);
    socklen_t length = sizeof(*host_address);
    if (socket == NULL || getsockname(socket->real_fd,
                (struct sockaddr *) host_address, &length) < 0 ||
            host_address->sin_port == 0)
        goto fail;
    if (invoke(fixture, memory, fault, SYS_LISTEN,
                listener, 8, 0, 0, 0, 0) != 0)
        goto fail;
    return listener;

fail:
    f_close_task(&fixture->task, listener);
    return _EIO;
}

static bool pair_communicates(struct fixture *fixture,
        fd_t first, fd_t second) {
    struct fd *left = f_get_task(&fixture->task, first);
    struct fd *right = f_get_task(&fixture->task, second);
    byte_t sent = 0x6d;
    byte_t received = 0;
    return left != NULL && right != NULL &&
            send(left->real_fd, &sent, sizeof(sent), 0) == 1 &&
            wait_for_event(right->real_fd, POLLIN) &&
            recv(right->real_fd, &received, sizeof(received), 0) == 1 &&
            received == sent;
}

static bool accepted_communicates(struct fixture *fixture,
        fd_t accepted, int client) {
    struct fd *socket = f_get_task(&fixture->task, accepted);
    byte_t sent = 0x4e;
    byte_t received = 0;
    return socket != NULL &&
            send(client, &sent, sizeof(sent), 0) == 1 &&
            wait_for_event(socket->real_fd, POLLIN) &&
            recv(socket->real_fd, &received, sizeof(received), 0) == 1 &&
            received == sent;
}

static qword_t accept_call(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t listener, qword_t address, qword_t length, qword_t flags) {
    return invoke(fixture, memory, fault, SYS_ACCEPT4,
            listener, address, length, flags, 0, 0);
}

static bool accept_queue_empty(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        fd_t listener) {
    reset_access(memory);
    return accept_call(fixture, memory, fault,
            listener, 0, UINT64_MAX, 0) == encoded_error(_EAGAIN) &&
            memory->access_count == 0;
}

static int test_socketpair(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t pair_offset = 0x2000;
    const qword_t pair_pointer = USER_BASE + pair_offset;
    dword_t pair[2] = {UINT32_MAX, UINT32_MAX};
    put_user(memory, pair_offset, pair, sizeof(pair));

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    UINT64_C(0xabcd000000007fff),
                    UINT64_C(0x1234000000000000) |
                            SOCK_STREAM_ | UNKNOWN_SOCKET_FLAG,
                    UINT64_C(0x9876000000000000), UINT64_MAX, 0, 0) ==
                    encoded_error(_EINVAL) && memory->access_count == 0,
            "socketpair 先按低 32 位校验 type flags");

    fixture->group.limits[RLIMIT_NOFILE_].cur = 1;
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_LOCAL_, SOCK_STREAM_, 0,
                    pair_pointer, 0, 0) == encoded_error(_EMFILE) &&
            memory->access_count == 0,
            "socketpair 必须先成功预留两个 fd 才写编号");
    fixture->group.limits[RLIMIT_NOFILE_].cur = 64;

    pair[0] = pair[1] = UINT32_MAX;
    put_user(memory, pair_offset, pair, sizeof(pair));
    reset_access(memory);
    memory->fail_write_at = pair_pointer + sizeof(dword_t);
    memory->hook = HOOK_PAIR_SECOND_WRITE;
    memory->hook_task = &fixture->task;
    memory->hook_address = pair_pointer + sizeof(dword_t);
    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_LOCAL_, SOCK_STREAM_, 0,
                    pair_pointer, 0, 0) == encoded_error(_EFAULT) &&
            memory->access_count == 2 &&
            access_is(memory, 0, pair_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 1, pair_pointer + sizeof(dword_t),
                    sizeof(dword_t), GUEST_MEMORY_WRITE) &&
            memory->hook_fired && memory->reserved_invisible,
            "socketpair 第二字 fault 时预留槽保持不可见");
    get_user(memory, pair_offset, pair, sizeof(pair));
    CHECK(pair[0] == 0 && pair[1] == UINT32_MAX &&
            f_get_task(&fixture->task, 0) == NULL,
            "socketpair 第二字 fault 保留第一字并撤销预留");
    int reused = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_DGRAM_, 0);
    CHECK(reused == 0 && f_close_task(&fixture->task, reused) == 0,
            "socketpair 写 fault 后最低槽可立即复用");

    pair[0] = pair[1] = UINT32_MAX;
    put_user(memory, pair_offset, pair, sizeof(pair));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    UINT64_C(0xbeef000000007fff), SOCK_STREAM_, 0,
                    pair_pointer, 0, 0) == encoded_error(_EAFNOSUPPORT) &&
            memory->access_count == 2,
            "socketpair 在 family 失败前已经逐字写回编号");
    get_user(memory, pair_offset, pair, sizeof(pair));
    CHECK(pair[0] == 0 && pair[1] == 1 &&
            f_get_task(&fixture->task, 0) == NULL &&
            f_get_task(&fixture->task, 1) == NULL,
            "socketpair 协议失败只留下编号副作用");

    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_LOCAL_, 0, 0, pair_pointer, 0, 0) ==
                    encoded_error(_ESOCKTNOSUPPORT) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_LOCAL_, 11, 999, pair_pointer, 0, 0) ==
                    encoded_error(_EINVAL) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_INET_, SOCK_STREAM_, 255, pair_pointer, 0, 0) ==
                    encoded_error(_EPROTONOSUPPORT) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_INET_, SOCK_RAW_, 0, pair_pointer, 0, 0) ==
                    encoded_error(_EPROTONOSUPPORT) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_INET_, SOCK_RAW_, 255, pair_pointer, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_INET6_, SOCK_RAW_, 0, pair_pointer, 0, 0) ==
                    encoded_error(_EPROTONOSUPPORT) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR, AF_INET6_,
                    SOCK_RAW_, IPPROTO_ICMPV6, pair_pointer, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    AF_INET6_, SOCK_RAW_, 256, pair_pointer, 0, 0) ==
                    encoded_error(_EINVAL),
            "socketpair 保留 family/type/protocol 的 Linux 错误优先级");
    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR, AF_LOCAL_,
                    SOCK_SEQPACKET_, PF_LOCAL_,
                    pair_pointer, 0, 0) == 0,
            "socketpair 支持本地有序记录对");
    get_user(memory, pair_offset, pair, sizeof(pair));
    struct fd *seqpacket_left =
            f_get_task(&fixture->task, pair[0]);
    CHECK(seqpacket_left != NULL &&
            seqpacket_left->socket.type == SOCK_SEQPACKET_ &&
            pair_communicates(fixture, pair[0], pair[1]) &&
            f_close_task(&fixture->task, pair[0]) == 0 &&
            f_close_task(&fixture->task, pair[1]) == 0,
            "本地有序记录对保留 guest 类型、消息边界与生命周期");

    int close_baseline = memory->replacement_closes;
    reset_access(memory);
    memory->hook = HOOK_PAIR_RESERVATION;
    memory->hook_task = &fixture->task;
    memory->hook_address = pair_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SOCKETPAIR,
                    UINT64_C(0x1111000000000000) | AF_LOCAL_,
                    UINT64_C(0x2222000000000000) | SOCK_STREAM_ |
                            SOCK_NONBLOCK_ | SOCK_CLOEXEC_,
                    UINT64_C(0x3333000000000000),
                    pair_pointer, 0, 0) == 0 &&
            memory->hook_fired && memory->reserved_invisible &&
            memory->hook_succeeded && memory->nested_fd == 2 &&
            memory->access_count == 2,
            "socketpair 预留期间嵌套安装跳过两个槽位");
    get_user(memory, pair_offset, pair, sizeof(pair));
    CHECK(pair[0] == 0 && pair[1] == 1 &&
            f_getfd_task(&fixture->task, pair[0]) == FD_CLOEXEC_ &&
            f_getfd_task(&fixture->task, pair[1]) == FD_CLOEXEC_ &&
            (f_getfl_task(&fixture->task, pair[0]) & O_NONBLOCK_) != 0 &&
            (f_getfl_task(&fixture->task, pair[1]) & O_NONBLOCK_) != 0 &&
            pair_communicates(fixture, pair[0], pair[1]),
            "socketpair 成功双端带 flags 且可以通信");
    CHECK(f_close_task(&fixture->task, pair[0]) == 0 &&
            f_close_task(&fixture->task, pair[1]) == 0 &&
            f_close_task(&fixture->task, memory->nested_fd) == 0 &&
            memory->replacement_closes == close_baseline + 1,
            "socketpair 成功路径精确清理三个 fd");
    return 0;
}

static int test_listen(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int ordinary = install_ordinary(fixture, memory);
    CHECK(ordinary == 0,
            "listen 优先级测试安装普通 fd");
    CHECK(invoke(fixture, memory, fault, SYS_LISTEN,
                    UINT64_C(0xaaaa000000000063),
                    UINT64_C(0xbbbb0000ffffffff), 0, 0, 0, 0) ==
                    encoded_error(_EBADF) &&
            invoke(fixture, memory, fault, SYS_LISTEN,
                    UINT64_C(0xcccc000000000000),
                    UINT64_C(0xdddd0000ffffffff), 0, 0, 0, 0) ==
                    encoded_error(_ENOTSOCK),
            "listen 保持 EBADF 与 ENOTSOCK 优先于 backlog");
    CHECK(f_close_task(&fixture->task, ordinary) == 0,
            "listen 普通 fd 清理成功");

    struct sockaddr_in address = {0};
    int listener = make_listener(fixture, memory, fault, &address);
    CHECK(listener == 0, "listen 回环 listener 创建成功");
    qword_t decorated_fd = UINT64_C(0x1234000000000000) |
            (dword_t) listener;
    CHECK(invoke(fixture, memory, fault, SYS_LISTEN,
                    decorated_fd, UINT64_C(0x56780000ffffffff),
                    0, 0, 0, 0) == 0 &&
            invoke(fixture, memory, fault, SYS_LISTEN,
                    decorated_fd, UINT64_C(0x9abc000000010000),
                    0, 0, 0, 0) == 0 &&
            invoke(fixture, memory, fault, SYS_LISTEN,
                    decorated_fd, 16, 0, 0, 0, 0) == 0,
            "listen 负数与超大 backlog 均钳制成功且可重复调用");
    CHECK(f_close_task(&fixture->task, listener) == 0,
            "重复 listen 后 listener 可正常关闭");
    return 0;
}

static int test_accept_priority(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int ordinary = install_ordinary(fixture, memory);
    CHECK(ordinary == 0, "accept4 优先级安装普通 fd");
    reset_access(memory);
    CHECK(accept_call(fixture, memory, fault, 99,
                    USER_BASE, USER_BASE + 8, UNKNOWN_SOCKET_FLAG) ==
                    encoded_error(_EBADF) && memory->access_count == 0,
            "accept4 的 EBADF 优先于 flags 与 guest 指针");
    fixture->group.limits[RLIMIT_NOFILE_].cur = 1;
    CHECK(accept_call(fixture, memory, fault, ordinary,
                    USER_BASE, USER_BASE + 8, UNKNOWN_SOCKET_FLAG) ==
                    encoded_error(_EINVAL),
            "accept4 在满表和 ENOTSOCK 前校验 flags");
    CHECK(accept_call(fixture, memory, fault, ordinary,
                    USER_BASE, USER_BASE + 8, 0) ==
                    encoded_error(_EMFILE),
            "accept4 在 ENOTSOCK 前预留返回槽位");
    fixture->group.limits[RLIMIT_NOFILE_].cur = 64;
    CHECK(accept_call(fixture, memory, fault, ordinary,
                    USER_BASE, USER_BASE + 8, 0) ==
                    encoded_error(_ENOTSOCK) &&
            memory->access_count == 0,
            "accept4 空表时返回稳定 ENOTSOCK 且不访问 guest");
    CHECK(f_close_task(&fixture->task, ordinary) == 0,
            "accept4 优先级普通 fd 清理成功");
    int datagram = create_guest_socket(
            fixture, memory, fault, AF_INET_, SOCK_DGRAM_, 0);
    CHECK(datagram == 0 && accept_call(fixture, memory, fault,
                    datagram, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            f_close_task(&fixture->task, datagram) == 0,
            "accept4 对不支持 accept 的数据报协议返回 EOPNOTSUPP");
    return 0;
}

static int test_accept_full_table(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    struct sockaddr_in address = {0};
    int listener = make_listener(fixture, memory, fault, &address);
    int client = connect_host(fixture, listener, &address);
    CHECK(listener == 0 && client >= 0,
            "满表测试建立 pending 连接");
    fixture->group.limits[RLIMIT_NOFILE_].cur = 1;
    reset_access(memory);
    CHECK(accept_call(fixture, memory, fault, listener,
                    0, UINT64_MAX, 0) == encoded_error(_EMFILE) &&
            memory->access_count == 0,
            "accept4 满表时不访问地址参数");
    fixture->group.limits[RLIMIT_NOFILE_].cur = 64;
    qword_t accepted_result = invoke(fixture, memory, fault, SYS_ACCEPT,
            UINT64_C(0xfeed000000000000) | (dword_t) listener,
            0, UINT64_MAX, 0, 0, 0);
    fd_t accepted = (fd_t) (sqword_t) accepted_result;
    CHECK(accepted == 1 && memory->access_count == 0 &&
            f_getfd_task(&fixture->task, accepted) == 0 &&
            (f_getfl_task(&fixture->task, accepted) & O_NONBLOCK_) == 0 &&
            accepted_communicates(fixture, accepted, client),
            "满表失败不消费 pending 且 accept 不继承非阻塞");
    CHECK(f_close_task(&fixture->task, accepted) == 0 &&
            f_close_task(&fixture->task, listener) == 0,
            "满表 accept 测试 guest fd 清理成功");
    close(client);
    return 0;
}

static int test_accept_addresses(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t address_offset = 0x3000;
    const size_t length_offset = 0x3100;
    const qword_t address_pointer = USER_BASE + address_offset;
    const qword_t length_pointer = USER_BASE + length_offset;
    struct sockaddr_in listener_address = {0};
    int listener = make_listener(
            fixture, memory, fault, &listener_address);
    CHECK(listener == 0, "accept 地址矩阵 listener 创建成功");

    int client = connect_host(fixture, listener, &listener_address);
    sdword_t capacity = -1;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    memset(memory->bytes + address_offset, 0xa5, 16);
    reset_access(memory);
    CHECK(client >= 0 && accept_call(fixture, memory, fault, listener,
                    address_pointer, length_pointer, 0) ==
                    encoded_error(_EINVAL) &&
            memory->access_count == 1 &&
            access_is(memory, 0, length_pointer, sizeof(capacity),
                    GUEST_MEMORY_READ) &&
            memory->bytes[address_offset] == 0xa5 &&
            accept_queue_empty(fixture, memory, fault, listener),
            "accept4 负容量消费连接但不写任何输出");
    close(client);

    client = connect_host(fixture, listener, &listener_address);
    capacity = 0;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    fd_t accepted = (fd_t) (sqword_t) accept_call(
            fixture, memory, fault, listener,
            address_pointer, length_pointer, 0);
    dword_t returned_length = 0;
    get_user(memory, length_offset, &returned_length,
            sizeof(returned_length));
    CHECK(client >= 0 && accepted >= 0 && returned_length == 16 &&
            memory->access_count == 2 &&
            access_is(memory, 1, length_pointer,
                    sizeof(returned_length), GUEST_MEMORY_WRITE),
            "accept4 零容量只写真实地址长度");
    CHECK(f_close_task(&fixture->task, accepted) == 0,
            "accept4 零容量 accepted fd 清理成功");
    close(client);

    client = connect_host(fixture, listener, &listener_address);
    struct sockaddr_in client_address = {0};
    socklen_t client_length = sizeof(client_address);
    CHECK(client >= 0 && getsockname(client,
                    (struct sockaddr *) &client_address,
                    &client_length) == 0,
            "accept4 短地址取得 host 客户端地址");
    capacity = 3;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    memset(memory->bytes + address_offset, 0xa5, 16);
    reset_access(memory);
    accepted = (fd_t) (sqword_t) accept_call(fixture, memory, fault,
            UINT64_C(0xabcd000000000000) | (dword_t) listener,
            address_pointer, length_pointer, 0);
    struct linux_sockaddr_in expected =
            linux_loopback(client_address.sin_port);
    CHECK(accepted >= 0 && memory->access_count == 3 &&
            memcmp(memory->bytes + address_offset, &expected, 3) == 0,
            "accept4 使用 64 位指针并按容量截断地址");
    CHECK(f_close_task(&fixture->task, accepted) == 0,
            "accept4 短地址 accepted fd 清理成功");
    close(client);

    client = connect_host(fixture, listener, &listener_address);
    capacity = 16;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_read_at = length_pointer;
    CHECK(client >= 0 && accept_call(fixture, memory, fault, listener,
                    address_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT) &&
            accept_queue_empty(fixture, memory, fault, listener),
            "accept4 长度读取 fault 保留连接已消费副作用");
    close(client);

    client = connect_host(fixture, listener, &listener_address);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = length_pointer;
    CHECK(client >= 0 && accept_call(fixture, memory, fault, listener,
                    address_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT) &&
            memory->access_count == 2 &&
            accept_queue_empty(fixture, memory, fault, listener),
            "accept4 长度写回 fault 发生在连接出队后");
    close(client);

    client = connect_host(fixture, listener, &listener_address);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = address_pointer;
    CHECK(client >= 0 && accept_call(fixture, memory, fault, listener,
                    address_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT),
            "accept4 地址 fault 返回 EFAULT");
    get_user(memory, length_offset, &returned_length,
            sizeof(returned_length));
    CHECK(returned_length == 16 &&
            accept_queue_empty(fixture, memory, fault, listener),
            "accept4 地址 fault 前已写真实长度且连接已消费");
    close(client);

    CHECK(f_close_task(&fixture->task, listener) == 0,
            "accept 地址矩阵 listener 清理成功");
    return 0;
}

static int test_accept_flags(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    struct sockaddr_in address = {0};
    int listener = make_listener(fixture, memory, fault, &address);
    CHECK(listener == 0 &&
            (f_getfl_task(&fixture->task, listener) & O_NONBLOCK_) != 0,
            "accept flags listener 为非阻塞");
    static const dword_t flag_cases[] = {
        0,
        SOCK_CLOEXEC_,
        SOCK_NONBLOCK_,
        SOCK_CLOEXEC_ | SOCK_NONBLOCK_,
    };
    for (size_t index = 0; index < array_size(flag_cases); index++) {
        int client = connect_host(fixture, listener, &address);
        reset_access(memory);
        qword_t decorated_flags = UINT64_C(0xcafe000000000000) |
                flag_cases[index];
        fd_t accepted = (fd_t) (sqword_t) accept_call(
                fixture, memory, fault, listener,
                0, UINT64_MAX, decorated_flags);
        bool expected_cloexec =
                (flag_cases[index] & SOCK_CLOEXEC_) != 0;
        bool expected_nonblock =
                (flag_cases[index] & SOCK_NONBLOCK_) != 0;
        struct fd *accepted_fd = f_get_task(&fixture->task, accepted);
        CHECK(client >= 0 && accepted_fd != NULL &&
                (f_getfd_task(&fixture->task, accepted) == FD_CLOEXEC_) ==
                        expected_cloexec &&
                ((f_getfl_task(&fixture->task, accepted) & O_NONBLOCK_) != 0) ==
                        expected_nonblock &&
                ((fcntl(accepted_fd->real_fd, F_GETFL) & O_NONBLOCK) != 0) ==
                        expected_nonblock &&
                memory->access_count == 0 &&
                accepted_communicates(fixture, accepted, client),
                "accept4 四种 flags 精确应用且不继承 listener 状态");
        CHECK(f_close_task(&fixture->task, accepted) == 0,
                "accept4 flags accepted fd 清理成功");
        close(client);
    }
    CHECK(f_close_task(&fixture->task, listener) == 0,
            "accept4 flags listener 清理成功");
    return 0;
}

static int test_accept_close_reuse(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t address_offset = 0x4000;
    const size_t length_offset = 0x4100;
    const qword_t address_pointer = USER_BASE + address_offset;
    const qword_t length_pointer = USER_BASE + length_offset;
    struct sockaddr_in address = {0};
    int listener = make_listener(fixture, memory, fault, &address);
    int client = connect_host(fixture, listener, &address);
    struct fd *listener_fd = f_get_task(&fixture->task, listener);
    CHECK(listener == 0 && client >= 0 && listener_fd != NULL,
            "accept 生命周期 listener 与客户端创建成功");
    int raw_listener = listener_fd->real_fd;
    sdword_t capacity = 16;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    int close_baseline = memory->replacement_closes;
    reset_access(memory);
    memory->hook = HOOK_ACCEPT_REUSE;
    memory->hook_task = &fixture->task;
    memory->hook_address = length_pointer;
    memory->hook_fd = listener;
    memory->hook_raw_fd = raw_listener;
    fd_t accepted = (fd_t) (sqword_t) accept_call(
            fixture, memory, fault, listener,
            address_pointer, length_pointer, 0);
    CHECK(accepted == 1 && memory->hook_fired &&
            memory->hook_succeeded && memory->reserved_invisible &&
            memory->raw_open_during_hook &&
            accepted_communicates(fixture, accepted, client),
            "accept retained 引用跨越 close/reuse 与 guest 回调");
    errno = 0;
    CHECK(fcntl(raw_listener, F_GETFD) == -1 && errno == EBADF &&
            f_close_task(&fixture->task, accepted) == 0 &&
            f_close_task(&fixture->task, listener) == 0 &&
            memory->replacement_closes == close_baseline + 1,
            "accept 返回后原 listener 与同号替换对象各自释放");
    close(client);
    return 0;
}

static unsigned occupied_fds(struct fixture *fixture) {
    unsigned count = 0;
    for (fd_t number = 0; number < 64; number++)
        if (f_get_task(&fixture->task, number) != NULL)
            count++;
    return count;
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

int main(void) {
    struct sigaction action = {
        .sa_handler = timeout_handler,
    };
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    alarm(TEST_TIMEOUT_SECONDS);

    struct fixture fixture;
    struct user_memory memory = {
        .bytes = calloc(1, USER_MEMORY_SIZE),
    };
    struct guest_linux_user_fault fault = {0};
    CHECK(memory.bytes != NULL, "guest 测试内存分配成功");
    if (!fixture_init(&fixture)) {
        fprintf(stderr,
                "AArch64 socket 监听测试失败：测试夹具初始化失败\n");
        free(memory.bytes);
        return 1;
    }
    int host_fd_baseline = open_host_fds();

    int result = test_socketpair(&fixture, &memory, &fault);
    if (result == 0)
        result = test_listen(&fixture, &memory, &fault);
    if (result == 0)
        result = test_accept_priority(&fixture, &memory, &fault);
    if (result == 0)
        result = test_accept_full_table(&fixture, &memory, &fault);
    if (result == 0)
        result = test_accept_addresses(&fixture, &memory, &fault);
    if (result == 0)
        result = test_accept_flags(&fixture, &memory, &fault);
    if (result == 0)
        result = test_accept_close_reuse(&fixture, &memory, &fault);
    if (result == 0 && occupied_fds(&fixture) != 0) {
        fprintf(stderr,
                "AArch64 socket 监听测试失败：仍有 guest fd 未关闭\n");
        result = 1;
    }
    if (result == 0 && open_host_fds() != host_fd_baseline) {
        fprintf(stderr,
                "AArch64 socket 监听测试失败：host fd 未回到基线\n");
        result = 1;
    }

    fixture_destroy(&fixture);
    free(memory.bytes);
    alarm(0);
    return result;
}

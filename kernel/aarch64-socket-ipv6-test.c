#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
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
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc67890000)
#define USER_MEMORY_SIZE UINT32_C(0x30000)
#define USER_ACCESS_LOG_SIZE 16

#define SYS_SOCKET 198
#define SYS_BIND 200
#define SYS_CONNECT 203
#define SYS_SENDTO 206
#define SYS_SETSOCKOPT 208

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 socket IPv6/RAW 测试失败：%s（第 %d 行）\n", \
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

struct linux_sockaddr_in6 {
    uint16_t family;
    uint16_t port;
    uint32_t flowinfo;
    byte_t address[16];
    uint32_t scope_id;
};

_Static_assert(sizeof(struct linux_sockaddr_in) == 16,
        "Linux IPv4 sockaddr wire 大小必须为 16 字节");
_Static_assert(sizeof(struct linux_sockaddr_in6) == 28,
        "Linux IPv6 sockaddr wire 大小必须为 28 字节");
_Static_assert(offsetof(struct linux_sockaddr_in6, scope_id) == 24,
        "Linux IPv6 RFC2133 兼容长度必须为 24 字节");

struct user_access {
    qword_t address;
    dword_t size;
    dword_t access;
};

struct user_memory {
    byte_t *bytes;
    qword_t fail_read_at;
    unsigned read_calls;
    unsigned write_calls;
    struct user_access accesses[USER_ACCESS_LOG_SIZE];
    unsigned access_count;
};

struct fixture {
    struct task task;
    struct tgroup group;
    struct guest_linux_syscall_completion completion;
};

typedef int (*test_function)(struct fixture *, struct user_memory *,
        struct guest_linux_user_fault *);

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static void reset_access(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->read_calls = 0;
    memory->write_calls = 0;
    memory->access_count = 0;
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
        memory->accesses[memory->access_count] =
                (struct user_access) {
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

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    record_access(memory, address, size, GUEST_MEMORY_READ);
    memory->read_calls++;
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
    set_fault(fault, address, GUEST_MEMORY_READ, GUEST_MEMORY_FAULT_NONE);
    return true;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    record_access(memory, address, size, GUEST_MEMORY_WRITE);
    memory->write_calls++;
    size_t offset;
    if (!user_range(address, size, &offset)) {
        set_fault(fault, address, GUEST_MEMORY_WRITE,
                GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        return false;
    }
    memcpy(memory->bytes + offset, source, size);
    set_fault(fault, address, GUEST_MEMORY_WRITE, GUEST_MEMORY_FAULT_NONE);
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
    return true;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    fdtable_release(fixture->task.files);
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
    if (offset > USER_MEMORY_SIZE || size > USER_MEMORY_SIZE - offset)
        abort();
    memcpy(memory->bytes + offset, value, size);
    return USER_BASE + offset;
}

static bool access_is(const struct user_memory *memory, unsigned index,
        qword_t address, dword_t size, enum guest_memory_access access) {
    return index < memory->access_count &&
            memory->accesses[index].address == address &&
            memory->accesses[index].size == size &&
            memory->accesses[index].access == (dword_t) access;
}

static int create_guest_socket(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t family, dword_t type, dword_t protocol) {
    return (int) (sqword_t) invoke(fixture, memory, fault, SYS_SOCKET,
            family, type, protocol, 0, 0, 0);
}

static struct linux_sockaddr_in linux_loopback4(uint16_t port) {
    return (struct linux_sockaddr_in) {
        .family = AF_INET_,
        .port = port,
        .address = htonl(INADDR_LOOPBACK),
    };
}

static struct linux_sockaddr_in6 linux_loopback6(uint16_t port) {
    struct linux_sockaddr_in6 address = {
        .family = AF_INET6_,
        .port = port,
    };
    address.address[15] = 1;
    return address;
}

static int host_udp4_listener(struct sockaddr_in *address) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;
    memset(address, 0, sizeof(*address));
#ifdef __APPLE__
    address->sin_len = sizeof(*address);
#endif
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_fd, (const struct sockaddr *) address,
                sizeof(*address)) < 0) {
        close(socket_fd);
        return -1;
    }
    socklen_t length = sizeof(*address);
    if (getsockname(socket_fd,
                (struct sockaddr *) address, &length) < 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static int host_udp6_listener(struct sockaddr_in6 *address) {
    int socket_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;
    memset(address, 0, sizeof(*address));
#ifdef __APPLE__
    address->sin6_len = sizeof(*address);
#endif
    address->sin6_family = AF_INET6;
    address->sin6_addr = in6addr_loopback;
    if (bind(socket_fd, (const struct sockaddr *) address,
                sizeof(*address)) < 0) {
        close(socket_fd);
        return -1;
    }
    socklen_t length = sizeof(*address);
    if (getsockname(socket_fd,
                (struct sockaddr *) address, &length) < 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static bool receive_equals(int socket_fd,
        const void *expected, size_t expected_size) {
    struct pollfd event = {.fd = socket_fd, .events = POLLIN};
    int ready;
    do {
        ready = poll(&event, 1, 1000);
    } while (ready < 0 && errno == EINTR);
    if (ready != 1 || (event.revents & POLLIN) == 0)
        return false;
    byte_t received[64];
    ssize_t size;
    do {
        size = recv(socket_fd, received, sizeof(received), 0);
    } while (size < 0 && errno == EINTR);
    return size == (ssize_t) expected_size &&
            memcmp(received, expected, expected_size) == 0;
}

static int set_guest_v6only(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int socket_fd, int enabled) {
    int32_t option = enabled;
    qword_t option_pointer = put_user(
            memory, 0x300, &option, sizeof(option));
    return (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_SETSOCKOPT, (dword_t) socket_fd,
            IPPROTO_IPV6, IPV6_V6ONLY_, option_pointer,
            sizeof(option), 0);
}

static int test_ipv4_bind_compat(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int any_socket = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_DGRAM_, 0);
    CHECK(any_socket >= 0, "IPv4 AF_UNSPEC bind socket 创建成功");
    struct linux_sockaddr_in any = {
        .family = 0,
        .address = htonl(INADDR_ANY),
    };
    qword_t any_pointer = put_user(memory, 0x100, &any, sizeof(any));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (dword_t) any_socket, any_pointer,
                    sizeof(any), 0, 0, 0) == 0 &&
            memory->read_calls == 1 &&
            access_is(memory, 0, any_pointer,
                    sizeof(any), GUEST_MEMORY_READ),
            "IPv4 bind 接受 AF_UNSPEC 与 INADDR_ANY");
    struct fd *bound_fd = f_get_task(&fixture->task, any_socket);
    struct sockaddr_in bound = {0};
    socklen_t bound_length = sizeof(bound);
    CHECK(bound_fd != NULL && getsockname(bound_fd->real_fd,
                    (struct sockaddr *) &bound, &bound_length) == 0 &&
            bound.sin_port != 0,
            "AF_UNSPEC bind 实际分配 host IPv4 端口");

    int short_socket = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_DGRAM_, 0);
    CHECK(short_socket >= 0, "IPv4 短地址 bind socket 创建成功");
    uint16_t short_family = AF_LOCAL_;
    qword_t short_pointer = put_user(memory,
            0x200, &short_family, sizeof(short_family));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (dword_t) short_socket, short_pointer,
                    sizeof(short_family), 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            memory->read_calls == 1,
            "IPv4 bind 在 family 前按 16 字节最小长度返回 EINVAL");

    int nonany_socket = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_DGRAM_, 0);
    CHECK(nonany_socket >= 0, "IPv4 非 ANY bind socket 创建成功");
    struct linux_sockaddr_in nonany = linux_loopback4(0);
    nonany.family = 0;
    qword_t nonany_pointer = put_user(
            memory, 0x400, &nonany, sizeof(nonany));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (dword_t) nonany_socket, nonany_pointer,
                    sizeof(nonany), 0, 0, 0) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "IPv4 bind 仅为 AF_UNSPEC 的 INADDR_ANY 开兼容例外");
    return 0;
}

static int test_ipv6_bind_dispatch(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "IPv6 bind socket 创建成功");
    struct linux_sockaddr_in6 address = linux_loopback6(0);
    qword_t address_pointer = put_user(
            memory, 0x500, &address, sizeof(address));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (dword_t) socket_fd, address_pointer,
                    sizeof(address), 0, 0, 0) == 0 &&
            memory->read_calls == 1 &&
            access_is(memory, 0, address_pointer,
                    sizeof(address), GUEST_MEMORY_READ),
            "IPv6 bind 经 AArch64 dispatcher 读取 28 字节 wire");
    struct fd *installed = f_get_task(&fixture->task, socket_fd);
    struct sockaddr_in6 bound = {0};
    socklen_t bound_length = sizeof(bound);
    CHECK(installed != NULL &&
                    getsockname(installed->real_fd,
                            (struct sockaddr *) &bound,
                            &bound_length) == 0 &&
                    bound.sin6_family == AF_INET6 &&
                    bound.sin6_port != 0 &&
                    memcmp(&bound.sin6_addr, &in6addr_loopback,
                            sizeof(bound.sin6_addr)) == 0,
            "IPv6 bind 在 host 回环地址分配端口");
    return 0;
}

static int test_udp_error_precedence(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "UDP 错误优先级 socket 创建成功");
    static const byte_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, payload_pointer,
                    sizeof(payload), 0, UINT64_MAX, 0) ==
                    encoded_error(_EINVAL) &&
            memory->read_calls == 0,
            "非空非法地址指针配合 addrlen=0 不解引用并返回 EINVAL");

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, payload_pointer,
                    sizeof(payload), 0, 0, UINT64_MAX) ==
                    encoded_error(_EDESTADDRREQ) &&
            memory->read_calls == 0,
            "未连接 UDP 在 payload fault 前返回 EDESTADDRREQ");

    struct linux_sockaddr_in port_zero = linux_loopback4(0);
    qword_t port_zero_pointer = put_user(
            memory, 0x200, &port_zero, sizeof(port_zero));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, payload_pointer,
                    sizeof(payload), 0, port_zero_pointer,
                    sizeof(port_zero)) == encoded_error(_EINVAL) &&
            memory->read_calls == 1 &&
            access_is(memory, 0, port_zero_pointer,
                    sizeof(port_zero), GUEST_MEMORY_READ),
            "UDP 目的端口 0 在 payload fault 前返回 EINVAL");
    return 0;
}

static int test_connect_family_errors(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int ipv4 = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_DGRAM_, 0);
    int ipv6 = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(ipv4 >= 0 && ipv6 >= 0,
            "connect family 错误测试 socket 创建成功");

    struct linux_sockaddr_in invalid4 = {
        .family = UINT16_C(0x7fff),
    };
    byte_t invalid6[offsetof(
            struct linux_sockaddr_in6, scope_id)] = {0};
    uint16_t invalid_family = UINT16_C(0x7fff);
    memcpy(invalid6, &invalid_family, sizeof(invalid_family));
    qword_t invalid4_pointer = put_user(
            memory, 0x100, &invalid4, sizeof(invalid4));
    qword_t invalid6_pointer = put_user(
            memory, 0x200, invalid6, sizeof(invalid6));

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) ipv4, invalid4_pointer,
                    sizeof(invalid4), 0, 0, 0) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "IPv4 connect 对足长未知 family 返回 EAFNOSUPPORT");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) ipv6, invalid6_pointer,
                    sizeof(invalid6), 0, 0, 0) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "IPv6 connect 对足长未知 family 返回 EAFNOSUPPORT");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) ipv6, invalid6_pointer,
                    sizeof(struct linux_sockaddr_in), 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            memory->read_calls == 1,
            "IPv6 connect 对短地址先返回 EINVAL");
    return 0;
}

static int test_ipv6_input_length(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t address_length) {
    struct sockaddr_in6 host_address;
    int receiver = host_udp6_listener(&host_address);
    CHECK(receiver >= 0, "IPv6 长度测试 host 接收端创建成功");
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "IPv6 长度测试 guest socket 创建成功");

    byte_t address_bytes[128] = {0};
    struct linux_sockaddr_in6 destination =
            linux_loopback6(host_address.sin6_port);
    memcpy(address_bytes, &destination, sizeof(destination));
    qword_t address_pointer = put_user(
            memory, 0x100, address_bytes, sizeof(address_bytes));
    const byte_t payload[] = {0x60, (byte_t) address_length, 0x6};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));

    reset_access(memory);
    qword_t result = invoke(fixture, memory, fault, SYS_SENDTO,
            (dword_t) socket_fd, payload_pointer, sizeof(payload), 0,
            address_pointer, address_length);
    bool received = result == sizeof(payload) &&
            receive_equals(receiver, payload, sizeof(payload));
    close(receiver);
    CHECK(received && memory->read_calls == 2 &&
            access_is(memory, 0, address_pointer,
                    address_length, GUEST_MEMORY_READ) &&
            access_is(memory, 1, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "IPv6 24/28/128 字节输入均按 Linux wire 规则发送");
    return 0;
}

static int test_ipv6_length_24(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    return test_ipv6_input_length(fixture, memory, fault, 24);
}

static int test_ipv6_length_28(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    return test_ipv6_input_length(fixture, memory, fault, 28);
}

static int test_ipv6_length_128(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    return test_ipv6_input_length(fixture, memory, fault, 128);
}

static int test_ipv6_dualstack_ipv4(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    struct sockaddr_in host_address;
    int receiver = host_udp4_listener(&host_address);
    CHECK(receiver >= 0, "dual-stack host IPv4 接收端创建成功");
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "dual-stack guest IPv6 socket 创建成功");
    CHECK(set_guest_v6only(fixture, memory, fault, socket_fd, 0) == 0,
            "dual-stack socket 关闭 IPV6_V6ONLY");

    struct linux_sockaddr_in destination =
            linux_loopback4(host_address.sin_port);
    qword_t address_pointer = put_user(
            memory, 0x100, &destination, sizeof(destination));
    static const byte_t payload[] = {0x44, 0x53, 0x34};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));
    reset_access(memory);
    qword_t result = invoke(fixture, memory, fault, SYS_SENDTO,
            (dword_t) socket_fd, payload_pointer, sizeof(payload), 0,
            address_pointer, sizeof(destination));
    bool received = result == sizeof(payload) &&
            receive_equals(receiver, payload, sizeof(payload));
    CHECK(received && memory->read_calls == 2,
            "IPv6 dual-stack socket 接受 AF_INET 目的地址");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, address_pointer,
                    sizeof(destination), 0, 0, 0) == 0 &&
            memory->read_calls == 1,
            "IPv6 dual-stack connect 将 AF_INET 转为 v4-mapped peer");
    static const byte_t connected_payload[] = {0x43, 0x34};
    qword_t connected_payload_pointer = put_user(memory, 0x1100,
            connected_payload, sizeof(connected_payload));
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, connected_payload_pointer,
                    sizeof(connected_payload), 0, 0, 0) ==
                    sizeof(connected_payload) &&
            receive_equals(receiver,
                    connected_payload, sizeof(connected_payload)),
            "dual-stack connect 后无目的 sendto 到达 IPv4 peer");
    close(receiver);
    return 0;
}

static int test_ipv6_v6only_ipv4(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "V6ONLY guest IPv6 socket 创建成功");
    CHECK(set_guest_v6only(fixture, memory, fault, socket_fd, 1) == 0,
            "IPv6 socket 启用 IPV6_V6ONLY");
    struct linux_sockaddr_in destination = linux_loopback4(htons(9));
    qword_t address_pointer = put_user(
            memory, 0x100, &destination, sizeof(destination));
    static const byte_t payload[] = {0x6, 0x4};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, payload_pointer,
                    sizeof(payload), 0, address_pointer,
                    sizeof(destination)) == encoded_error(_ENETUNREACH) &&
            memory->read_calls == 1,
            "V6ONLY 对 AF_INET 在 payload fault 前返回 ENETUNREACH");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, address_pointer,
                    sizeof(destination), 0, 0, 0) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "V6ONLY connect 以 EAFNOSUPPORT 拒绝 AF_INET peer");
    return 0;
}

static int test_ipv6_unspec_unconnected(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "AF_UNSPEC 未连接 IPv6 socket 创建成功");
    uint16_t family = 0;
    qword_t address_pointer = put_user(
            memory, 0x100, &family, sizeof(family));
    static const byte_t payload[] = {0x0, 0x6};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, payload_pointer,
                    sizeof(payload), 0, address_pointer,
                    sizeof(family)) == encoded_error(_EDESTADDRREQ) &&
            memory->read_calls == 1,
            "IPv6 UDP 将 AF_UNSPEC 视为清除显式目的地址");
    return 0;
}

static int test_ipv6_unspec_connected(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    struct sockaddr_in6 host_address;
    int receiver = host_udp6_listener(&host_address);
    CHECK(receiver >= 0, "AF_UNSPEC connected host IPv6 接收端创建成功");
    int socket_fd = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "AF_UNSPEC connected guest socket 创建成功");
    struct linux_sockaddr_in6 peer =
            linux_loopback6(host_address.sin6_port);
    qword_t peer_pointer = put_user(
            memory, 0x100, &peer, sizeof(peer));
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, peer_pointer,
                    sizeof(peer), 0, 0, 0) == 0,
            "IPv6 UDP guest socket 连接回环接收端");

    uint16_t family = 0;
    qword_t unspec_pointer = put_user(
            memory, 0x300, &family, sizeof(family));
    static const byte_t payload[] = {0x41, 0x46, 0x30};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));
    reset_access(memory);
    qword_t result = invoke(fixture, memory, fault, SYS_SENDTO,
            (dword_t) socket_fd, payload_pointer, sizeof(payload), 0,
            unspec_pointer, sizeof(family));
    bool received = result == sizeof(payload) &&
            receive_equals(receiver, payload, sizeof(payload));
    close(receiver);
    CHECK(received && memory->read_calls == 2,
            "connected IPv6 UDP 的 AF_UNSPEC 回退到已连接 peer");
    return 0;
}

static int test_udp_disconnect_family(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault, dword_t family) {
    struct sockaddr_in first4;
    struct sockaddr_in second4;
    struct sockaddr_in6 first6;
    struct sockaddr_in6 second6;
    int first_receiver;
    int second_receiver;
    byte_t first_peer[sizeof(struct linux_sockaddr_in6)] = {0};
    byte_t second_peer[sizeof(struct linux_sockaddr_in6)] = {0};
    size_t peer_size;
    if (family == AF_INET_) {
        first_receiver = host_udp4_listener(&first4);
        second_receiver = host_udp4_listener(&second4);
        if (first_receiver < 0 || second_receiver < 0) {
            if (first_receiver >= 0)
                close(first_receiver);
            if (second_receiver >= 0)
                close(second_receiver);
            CHECK(false, "UDP 断连测试 host 接收端创建成功");
        }
        struct linux_sockaddr_in guest_first =
                linux_loopback4(first4.sin_port);
        struct linux_sockaddr_in guest_second =
                linux_loopback4(second4.sin_port);
        memcpy(first_peer, &guest_first, sizeof(guest_first));
        memcpy(second_peer, &guest_second, sizeof(guest_second));
        peer_size = sizeof(guest_first);
    } else {
        first_receiver = host_udp6_listener(&first6);
        second_receiver = host_udp6_listener(&second6);
        if (first_receiver < 0 || second_receiver < 0) {
            if (first_receiver >= 0)
                close(first_receiver);
            if (second_receiver >= 0)
                close(second_receiver);
            CHECK(false, "UDP 断连测试 host 接收端创建成功");
        }
        struct linux_sockaddr_in6 guest_first =
                linux_loopback6(first6.sin6_port);
        struct linux_sockaddr_in6 guest_second =
                linux_loopback6(second6.sin6_port);
        memcpy(first_peer, &guest_first, sizeof(guest_first));
        memcpy(second_peer, &guest_second, sizeof(guest_second));
        peer_size = sizeof(guest_first);
    }
    int socket_fd = create_guest_socket(fixture, memory, fault,
            family, SOCK_DGRAM_, 0);
    CHECK(socket_fd >= 0, "UDP 断连测试 guest socket 创建成功");
    int32_t receive_buffer = 32768;
    qword_t receive_buffer_pointer = put_user(
            memory, 0x500, &receive_buffer, sizeof(receive_buffer));
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    (dword_t) socket_fd, SOL_SOCKET_, SO_RCVBUF_,
                    receive_buffer_pointer,
                    sizeof(receive_buffer), 0) == 0,
            "UDP 断连前设置接收缓冲选项成功");
    qword_t first_pointer = put_user(
            memory, 0x100, first_peer, peer_size);
    qword_t second_pointer = put_user(
            memory, 0x200, second_peer, peer_size);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, first_pointer,
                    peer_size, 0, 0, 0) == 0,
            "UDP socket 首次 connect 成功");
    struct fd *installed = f_get_task(&fixture->task, socket_fd);
    struct sockaddr_storage local_address = {0};
    socklen_t local_length = sizeof(local_address);
    CHECK(installed != NULL && getsockname(installed->real_fd,
                    (struct sockaddr *) &local_address,
                    &local_length) == 0,
            "UDP 首次 connect 后可查询自动本地地址");
    uint16_t automatic_port = family == AF_INET_ ?
            ((struct sockaddr_in *) &local_address)->sin_port :
            ((struct sockaddr_in6 *) &local_address)->sin6_port;
    CHECK(automatic_port != 0,
            "UDP 首次 connect 自动分配非零本地端口");
    int original_receive_buffer = 0;
    socklen_t receive_buffer_length = sizeof(original_receive_buffer);
    CHECK(getsockopt(installed->real_fd, SOL_SOCKET, SO_RCVBUF,
                    &original_receive_buffer,
                    &receive_buffer_length) == 0 &&
            original_receive_buffer > 0,
            "UDP 断连前可查询接收缓冲选项");
    CHECK(f_setfl_task(&fixture->task,
                    socket_fd, O_NONBLOCK_) == 0,
            "UDP 断连前启用非阻塞状态");

    static const byte_t first_payload[] = {0x63, 0x31};
    qword_t first_payload_pointer = put_user(
            memory, 0x1000, first_payload, sizeof(first_payload));
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, first_payload_pointer,
                    sizeof(first_payload), 0, 0, 0) ==
                    sizeof(first_payload) &&
            receive_equals(first_receiver,
                    first_payload, sizeof(first_payload)),
            "UDP 首次 connect 后可按已连接目的发送");

    uint16_t unspec = 0;
    byte_t unspec_address[sizeof(struct linux_sockaddr_in6)] = {0};
    qword_t unspec_pointer = put_user(
            memory, 0x300, unspec_address, peer_size);
    reset_access(memory);
    memory->fail_read_at = unspec_pointer + peer_size - 1;
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, unspec_pointer,
                    peer_size, 0, 0, 0) == encoded_error(_EFAULT) &&
            memory->read_calls == 1 &&
            fault->address == memory->fail_read_at,
            "AF_UNSPEC 仍完整复制 caller 提供的 sockaddr");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, first_payload_pointer,
                    sizeof(first_payload), 0, 0, 0) ==
                    sizeof(first_payload) &&
            receive_equals(first_receiver,
                    first_payload, sizeof(first_payload)),
            "AF_UNSPEC 尾部复制故障不会改变原连接");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, unspec_pointer,
                    peer_size, 0, 0, 0) == 0 &&
            memory->read_calls == 1 &&
            access_is(memory, 0, unspec_pointer,
                    peer_size, GUEST_MEMORY_READ),
            "已连接 UDP 完整复制 AF_UNSPEC 后断连成功");

    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    errno = 0;
    CHECK(installed != NULL &&
            getpeername(installed->real_fd,
                    (struct sockaddr *) &peer, &peer_length) < 0 &&
            err_map(errno) == _ENOTCONN,
            "UDP 断连后 getpeername 返回 ENOTCONN");
    memset(&local_address, 0, sizeof(local_address));
    local_length = sizeof(local_address);
    CHECK(getsockname(installed->real_fd,
                    (struct sockaddr *) &local_address,
                    &local_length) == 0 &&
            (family == AF_INET_ ?
                    ((struct sockaddr_in *) &local_address)->sin_port :
                    ((struct sockaddr_in6 *) &local_address)->sin6_port) == 0,
            "未显式 bind 的 UDP 断连后释放自动本地端口");
    int recreated_receive_buffer = 0;
    receive_buffer_length = sizeof(recreated_receive_buffer);
    CHECK(getsockopt(installed->real_fd, SOL_SOCKET, SO_RCVBUF,
                    &recreated_receive_buffer,
                    &receive_buffer_length) == 0 &&
            recreated_receive_buffer == original_receive_buffer &&
            (fcntl(installed->real_fd, F_GETFL) & O_NONBLOCK) != 0,
            "重建式断连保留 socket option 与非阻塞状态");

    static const byte_t disconnected_payload[] = {0x64, 0x30};
    qword_t disconnected_payload_pointer = put_user(memory, 0x1100,
            disconnected_payload, sizeof(disconnected_payload));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, disconnected_payload_pointer,
                    sizeof(disconnected_payload), 0, 0, 0) ==
                    encoded_error(_EDESTADDRREQ) &&
            memory->read_calls == 0,
            "UDP 断连后无目的 sendto 返回 EDESTADDRREQ");

    byte_t bind_address[sizeof(struct linux_sockaddr_in6)] = {0};
    if (family == AF_INET_) {
        struct linux_sockaddr_in guest_bind =
                linux_loopback4(automatic_port);
        memcpy(bind_address, &guest_bind, sizeof(guest_bind));
    } else {
        struct linux_sockaddr_in6 guest_bind =
                linux_loopback6(automatic_port);
        memcpy(bind_address, &guest_bind, sizeof(guest_bind));
    }
    qword_t bind_pointer = put_user(
            memory, 0x400, bind_address, peer_size);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (dword_t) socket_fd, bind_pointer,
                    peer_size, 0, 0, 0) == 0,
            "UDP 隐式绑定断连后可显式重绑原端口");

    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, second_pointer,
                    peer_size, 0, 0, 0) == 0,
            "UDP 断连后可重新 connect");
    static const byte_t second_payload[] = {0x63, 0x32};
    qword_t second_payload_pointer = put_user(
            memory, 0x1200, second_payload, sizeof(second_payload));
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_fd, second_payload_pointer,
                    sizeof(second_payload), 0, 0, 0) ==
                    sizeof(second_payload) &&
            receive_equals(second_receiver,
                    second_payload, sizeof(second_payload)),
            "UDP reconnect 后使用新目的发送");
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) socket_fd, unspec_pointer,
                    peer_size, 0, 0, 0) == 0,
            "显式 bind 的 UDP 可再次断连");
    memset(&local_address, 0, sizeof(local_address));
    local_length = sizeof(local_address);
    CHECK(getsockname(installed->real_fd,
                    (struct sockaddr *) &local_address,
                    &local_length) == 0 &&
            (family == AF_INET_ ?
                    ((struct sockaddr_in *) &local_address)->sin_port :
                    ((struct sockaddr_in6 *) &local_address)->sin6_port) ==
                    automatic_port,
            "显式 bind 的 UDP 断连后保留本地端口");

    int unconnected_fd = create_guest_socket(fixture, memory, fault,
            family, SOCK_DGRAM_, 0);
    CHECK(unconnected_fd >= 0,
            "未连接 AF_UNSPEC 测试 guest socket 创建成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) unconnected_fd, UINT64_MAX,
                    1, 0, 0, 0) == encoded_error(_EFAULT) &&
            memory->read_calls == 0 && fault->address == UINT64_MAX,
            "connect 的一字节 sockaddr 仍先校验 guest 地址");

    qword_t short_pointer = put_user(memory, 0x380, &unspec, 1);
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) unconnected_fd, short_pointer,
                    1, 0, 0, 0) == encoded_error(_EINVAL) &&
            memory->read_calls == 1 &&
            access_is(memory, 0, short_pointer,
                    1, GUEST_MEMORY_READ),
            "connect 复制一字节 sockaddr 后由协议返回 EINVAL");

    qword_t boundary_pointer = put_user(memory,
            USER_MEMORY_SIZE - sizeof(unspec),
            &unspec, sizeof(unspec));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    (dword_t) unconnected_fd, boundary_pointer,
                    sizeof(unspec), 0, 0, 0) == 0 &&
            memory->read_calls == 1 &&
            access_is(memory, 0, boundary_pointer,
                    sizeof(unspec), GUEST_MEMORY_READ),
            "未连接 UDP 接受完整的两字节 AF_UNSPEC sockaddr");

    close(first_receiver);
    close(second_receiver);
    return 0;
}

static int test_ipv4_udp_disconnect(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    return test_udp_disconnect_family(
            fixture, memory, fault, AF_INET_);
}

static int test_ipv6_udp_disconnect(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    return test_udp_disconnect_family(
            fixture, memory, fault, AF_INET6_);
}

static int test_raw_length_and_oob(struct fixture *fixture,
        struct user_memory *memory,
    struct guest_linux_user_fault *fault) {
    int ipv4 = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_RAW_ | SOCK_CLOEXEC_ | SOCK_NONBLOCK_,
            IPPROTO_ICMP);
    int ipv6 = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_RAW_, IPPROTO_ICMPV6);
    CHECK(ipv4 >= 0 && ipv6 >= 0,
            "IPv4/IPv6 RAW guest socket 创建成功");
    struct fd *flagged_raw = f_get_task(&fixture->task, ipv4);
    CHECK(flagged_raw != NULL &&
                    bit_test(ipv4, fixture->task.files->cloexec) &&
                    (fcntl(flagged_raw->real_fd, F_GETFL) &
                            O_NONBLOCK) != 0,
            "RAW socket 同时保留类型并落实 CLOEXEC/NONBLOCK");
    static const byte_t payload[] = {0x8, 0x0, 0x0, 0x0};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv4, payload_pointer, 65536,
                    MSG_OOB_, 0, 0) == encoded_error(_EMSGSIZE) &&
            memory->read_calls == 0,
            "IPv4 RAW 的 65535 限长先于 MSG_OOB");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv6, payload_pointer, 65536,
                    MSG_OOB_, 0, 0) == encoded_error(_EOPNOTSUPP) &&
            memory->read_calls == 0,
            "IPv6 RAW 不套用 IPv4 限长且先返回 MSG_OOB 错误");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv4, payload_pointer, sizeof(payload),
                    MSG_OOB_, 0, 0) == encoded_error(_EOPNOTSUPP) &&
            memory->read_calls == 0,
            "IPv4 RAW 拒绝 MSG_OOB 且不读取 payload");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv6, payload_pointer, sizeof(payload),
                    MSG_OOB_, 0, 0) == encoded_error(_EOPNOTSUPP) &&
            memory->read_calls == 0,
            "IPv6 RAW 拒绝 MSG_OOB 且不读取 payload");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv6, payload_pointer, 65536,
                    0, 0, 0) == encoded_error(_EDESTADDRREQ) &&
            memory->read_calls == 0,
            "IPv6 RAW 允许 65536 字节并先报告缺少目的地址");
    return 0;
}

static int test_raw_address_differences(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int ipv4 = create_guest_socket(fixture, memory, fault,
            AF_INET_, SOCK_RAW_, IPPROTO_ICMP);
    int ipv6 = create_guest_socket(fixture, memory, fault,
            AF_INET6_, SOCK_RAW_, IPPROTO_ICMPV6);
    CHECK(ipv4 >= 0 && ipv6 >= 0,
            "RAW 地址差异 guest socket 创建成功");
    static const byte_t payload[] = {0x8, 0x0, 0x0, 0x0};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv4, payload_pointer, sizeof(payload),
                    0, UINT64_MAX, 0) == encoded_error(_EDESTADDRREQ) &&
            memory->read_calls == 0,
            "IPv4 RAW 的显式 addrlen=0 等价于没有目的地址");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv6, payload_pointer, sizeof(payload),
                    0, UINT64_MAX, 0) == encoded_error(_EINVAL) &&
            memory->read_calls == 0,
            "IPv6 RAW 的显式 addrlen=0 返回 EINVAL");

    struct linux_sockaddr_in wrong4 = linux_loopback4(0);
    wrong4.family = AF_INET6_;
    qword_t wrong4_pointer = put_user(
            memory, 0x100, &wrong4, sizeof(wrong4));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv4, payload_pointer, sizeof(payload),
                    0, wrong4_pointer, sizeof(wrong4)) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "IPv4 RAW 对非零错误 family 返回 EAFNOSUPPORT");

    struct linux_sockaddr_in6 wrong6 = linux_loopback6(0);
    wrong6.family = AF_INET_;
    qword_t wrong6_pointer = put_user(
            memory, 0x300, &wrong6, sizeof(wrong6));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv6, payload_pointer, sizeof(payload),
                    0, wrong6_pointer, sizeof(wrong6)) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "IPv6 RAW 对 AF_INET 返回 EAFNOSUPPORT");

    struct linux_sockaddr_in unspec4 = linux_loopback4(0);
    unspec4.family = 0;
    qword_t unspec4_pointer = put_user(
            memory, 0x500, &unspec4, sizeof(unspec4));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv4, payload_pointer, sizeof(payload),
                    0, unspec4_pointer, sizeof(unspec4)) ==
                    encoded_error(_EFAULT) &&
            memory->read_calls == 2,
            "IPv4 RAW 容忍 AF_UNSPEC 并继续到 payload 读取");

    struct linux_sockaddr_in6 unspec6 = linux_loopback6(0);
    unspec6.family = 0;
    qword_t unspec6_pointer = put_user(
            memory, 0x700, &unspec6, sizeof(unspec6));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) ipv6, payload_pointer, sizeof(payload),
                    0, unspec6_pointer, sizeof(unspec6)) ==
                    encoded_error(_EFAULT) &&
            memory->read_calls == 2,
            "IPv6 RAW 容忍 AF_UNSPEC 并继续到 payload 读取");
    return 0;
}

static int run_test(const char *name, test_function function) {
    struct fixture fixture;
    struct user_memory memory = {
        .bytes = calloc(1, USER_MEMORY_SIZE),
    };
    struct guest_linux_user_fault fault = {0};
    if (memory.bytes == NULL || !fixture_init(&fixture)) {
        fprintf(stderr, "测试夹具初始化失败：%s\n", name);
        free(memory.bytes);
        return 1;
    }
    reset_access(&memory);
    int result = function(&fixture, &memory, &fault);
    fixture_destroy(&fixture);
    free(memory.bytes);
    if (result == 0)
        printf("通过：%s\n", name);
    return result;
}

int main(void) {
    static const struct {
        const char *name;
        test_function function;
    } tests[] = {
        {"IPv4 bind 兼容边界", test_ipv4_bind_compat},
        {"IPv6 bind dispatcher", test_ipv6_bind_dispatch},
        {"UDP 错误优先级", test_udp_error_precedence},
        {"connect family 错误", test_connect_family_errors},
        {"IPv6 24 字节地址", test_ipv6_length_24},
        {"IPv6 28 字节地址", test_ipv6_length_28},
        {"IPv6 128 字节地址", test_ipv6_length_128},
        {"IPv6 dual-stack AF_INET", test_ipv6_dualstack_ipv4},
        {"IPv6 V6ONLY AF_INET", test_ipv6_v6only_ipv4},
        {"IPv6 未连接 AF_UNSPEC", test_ipv6_unspec_unconnected},
        {"IPv6 已连接 AF_UNSPEC", test_ipv6_unspec_connected},
        {"IPv4 UDP AF_UNSPEC 断连", test_ipv4_udp_disconnect},
        {"IPv6 UDP AF_UNSPEC 断连", test_ipv6_udp_disconnect},
        {"RAW 长度与 MSG_OOB", test_raw_length_and_oob},
        {"RAW 地址与 family 差异", test_raw_address_differences},
    };
    unsigned failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++)
        failures += (unsigned) run_test(
                tests[index].name, tests[index].function);
    if (failures != 0)
        fprintf(stderr, "共有 %u 组 socket IPv6/RAW 测试失败\n", failures);
    return failures == 0 ? 0 : 1;
}

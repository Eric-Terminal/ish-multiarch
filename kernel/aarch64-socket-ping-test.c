#include <netinet/in.h>
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
#include "guest/aarch64/linux-socket-abi.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc789a0000)
#define USER_MEMORY_SIZE UINT32_C(0x20000)
#define USER_ACCESS_LOG_SIZE 16

#define SYS_SOCKET 198
#define SYS_SENDTO 206
#define SYS_SENDMSG 211

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 ping sendto 测试失败：%s（第 %d 行）\n", \
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

struct aarch64_linux_iovec_wire {
    qword_t base;
    qword_t length;
};

_Static_assert(sizeof(struct linux_sockaddr_in) == 16,
        "Linux IPv4 sockaddr wire 大小必须为 16 字节");
_Static_assert(sizeof(struct linux_sockaddr_in6) == 28,
        "Linux IPv6 sockaddr wire 大小必须为 28 字节");

struct user_access {
    qword_t address;
    dword_t size;
    dword_t access;
};

struct user_memory {
    byte_t *bytes;
    qword_t fail_read_at;
    unsigned read_calls;
    struct user_access accesses[USER_ACCESS_LOG_SIZE];
    unsigned access_count;
};

struct fixture {
    struct task task;
    struct tgroup group;
    struct guest_linux_syscall_completion completion;
    int connected_peers[4];
    size_t connected_peer_count;
};

typedef int (*test_function)(struct fixture *, struct user_memory *,
        struct guest_linux_user_fault *);

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static void reset_access(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->read_calls = 0;
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
    if (fixture->task.sighand == NULL || IS_ERR(fixture->task.files)) {
        if (fixture->task.sighand != NULL)
            sighand_release(fixture->task.sighand);
        if (!IS_ERR(fixture->task.files))
            fdtable_release(fixture->task.files);
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    fdtable_release(fixture->task.files);
    for (size_t index = 0; index < fixture->connected_peer_count; index++)
        close(fixture->connected_peers[index]);
    sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
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
        qword_t address, dword_t size) {
    return index < memory->access_count &&
            memory->accesses[index].address == address &&
            memory->accesses[index].size == size &&
            memory->accesses[index].access == GUEST_MEMORY_READ;
}

static unsigned access_count_at(const struct user_memory *memory,
        qword_t address, dword_t size) {
    unsigned count = 0;
    unsigned recorded = memory->access_count < USER_ACCESS_LOG_SIZE ?
            memory->access_count : USER_ACCESS_LOG_SIZE;
    for (unsigned index = 0; index < recorded; index++)
        if (access_is(memory, index, address, size))
            count++;
    return count;
}

static int create_ping_socket(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t domain, dword_t protocol) {
    reset_access(memory);
    qword_t result = invoke(fixture, memory, fault, SYS_SOCKET,
            domain, SOCK_DGRAM_, 0, 0, 0, 0);
    if ((sqword_t) result < 0)
        return (int) (sqword_t) result;
    struct fd *socket = f_get_task(&fixture->task, (fd_t) result);
    if (socket == NULL)
        return _EBADF;
    // 普通 UDP host fd 足以承载纯预检；只改 guest 元数据即可避开 ICMP 权限。
    socket->socket.protocol = (int) protocol;
    return (int) result;
}

static int test_flagged_datagram_socket(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault) {
    reset_access(memory);
#ifdef __APPLE__
    dword_t protocol = IPPROTO_ICMP;
#else
    dword_t protocol = 0;
#endif
    qword_t result = invoke(fixture, memory, fault, SYS_SOCKET,
            AF_INET_, SOCK_DGRAM_ | SOCK_NONBLOCK_ | SOCK_CLOEXEC_,
            protocol, 0, 0, 0);
    CHECK((sqword_t) result >= 0,
            "带 NONBLOCK/CLOEXEC 创建 IPv4 数据报 socket");

    struct fd *socket = f_get_task(&fixture->task, (fd_t) result);
    CHECK(socket != NULL, "取得带标志数据报 socket");

#ifdef __APPLE__
    int strip_header = 0;
    socklen_t option_length = sizeof(strip_header);
    CHECK(getsockopt(socket->real_fd, IPPROTO_IP, IP_STRIPHDR,
                    &strip_header, &option_length) == 0 &&
                    strip_header != 0,
            "带创建标志的 IPv4 数据报 socket 仍启用 IP_STRIPHDR");
#endif
    return 0;
}

static bool attach_connected_host_pair(
        struct fixture *fixture, fd_t socket_number) {
    if (fixture->connected_peer_count >=
            sizeof(fixture->connected_peers) /
                    sizeof(fixture->connected_peers[0]))
        return false;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) < 0)
        return false;
    struct fd *socket = f_get_task(&fixture->task, socket_number);
    if (socket == NULL) {
        close(pair[1]);
        close(pair[0]);
        return false;
    }
    close(socket->real_fd);
    socket->real_fd = pair[0];
    fixture->connected_peers[fixture->connected_peer_count++] = pair[1];
    return true;
}

static int check_common_errors(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t domain, dword_t protocol) {
    int socket_number = create_ping_socket(
            fixture, memory, fault, domain, protocol);
    CHECK(socket_number >= 0, "ping guest socket 创建成功");
    byte_t payload[8] = {0};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer, 65536,
                    0, 0, 0) == encoded_error(_EMSGSIZE) &&
            memory->read_calls == 0,
            "ping 的 65535 长度上限先于 payload 读取");

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer, 7,
                    0, 0, 0) == encoded_error(_EINVAL) &&
            memory->read_calls == 0,
            "ping 的八字节头下限先于 payload 读取");

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer, 8,
                    MSG_OOB_, 0, 0) == encoded_error(_EOPNOTSUPP) &&
            memory->read_calls == 0,
            "ping 的 MSG_OOB 错误先于 payload 读取");
    return 0;
}

static int test_common_errors(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault) {
    CHECK(check_common_errors(fixture, memory, fault,
                    AF_INET_, IPPROTO_ICMP) == 0,
            "IPv4 ping 公共错误顺序正确");
    CHECK(check_common_errors(fixture, memory, fault,
                    AF_INET6_, IPPROTO_ICMPV6) == 0,
            "IPv6 ping 公共错误顺序正确");
    return 0;
}

static int check_address_order(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t domain, dword_t protocol, byte_t valid_type,
        const void *wrong_address, size_t wrong_length,
        const void *unspec_address, size_t unspec_length) {
    int socket_number = create_ping_socket(
            fixture, memory, fault, domain, protocol);
    CHECK(socket_number >= 0, "地址顺序测试 socket 创建成功");
    qword_t wrong_pointer = put_user(
            memory, 0x100, wrong_address, wrong_length);
    qword_t unspec_pointer = put_user(
            memory, 0x300, unspec_address, unspec_length);
    byte_t valid_header[8] = {valid_type, 0};
    byte_t invalid_header[8] = {valid_type, 1};
    qword_t payload_pointer = put_user(
            memory, 0x1000, valid_header, sizeof(valid_header));

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer,
                    sizeof(valid_header), 0,
                    wrong_pointer, (dword_t) wrong_length) ==
                    encoded_error(_EFAULT) &&
            memory->read_calls == 2 &&
            access_is(memory, 0, wrong_pointer, (dword_t) wrong_length) &&
            access_is(memory, 1, payload_pointer, sizeof(valid_header)),
            "ping 先读取八字节头，再校验目的地址协议");

    put_user(memory, 0x1000, invalid_header, sizeof(invalid_header));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer,
                    sizeof(invalid_header), 0,
                    wrong_pointer, (dword_t) wrong_length) ==
                    encoded_error(_EINVAL) &&
            memory->read_calls == 2 &&
            access_is(memory, 1, payload_pointer, sizeof(invalid_header)),
            "ping 的 type/code 校验先于目的地址 family 校验");

    put_user(memory, 0x1000, valid_header, sizeof(valid_header));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer,
                    sizeof(valid_header), 0,
                    wrong_pointer, (dword_t) wrong_length) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 2,
            "ping 拒绝另一地址族的目的地址");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer,
                    sizeof(valid_header), 0,
                    unspec_pointer, (dword_t) unspec_length) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 2,
            "ping 不把 AF_UNSPEC 目的地址当作已连接发送");
    return 0;
}

static int test_address_order(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault) {
    struct linux_sockaddr_in wrong4 = {
        .family = AF_INET6_,
        .port = 9,
        .address = UINT32_C(0x0100007f),
    };
    struct linux_sockaddr_in unspec4 = wrong4;
    unspec4.family = 0;
    CHECK(check_address_order(fixture, memory, fault,
                    AF_INET_, IPPROTO_ICMP, 8,
                    &wrong4, sizeof(wrong4),
                    &unspec4, sizeof(unspec4)) == 0,
            "IPv4 ping 地址与头部顺序正确");

    struct linux_sockaddr_in6 wrong6 = {
        .family = AF_INET_,
        .port = 9,
        .address = {[15] = 1},
    };
    struct linux_sockaddr_in6 unspec6 = wrong6;
    unspec6.family = 0;
    CHECK(check_address_order(fixture, memory, fault,
                    AF_INET6_, IPPROTO_ICMPV6, 128,
                    &wrong6, sizeof(wrong6),
                    &unspec6, sizeof(unspec6)) == 0,
            "IPv6 ping 地址与头部顺序正确");
    return 0;
}

static int check_destination_state(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t domain, dword_t protocol, byte_t valid_type) {
    int socket_number = create_ping_socket(
            fixture, memory, fault, domain, protocol);
    CHECK(socket_number >= 0, "目的状态测试 socket 创建成功");
    byte_t payload[9] = {valid_type, 0};
    qword_t payload_pointer = put_user(
            memory, 0x1000, payload, sizeof(payload));

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer, 8,
                    0, UINT64_MAX, 0) == encoded_error(_EINVAL) &&
            memory->read_calls == 1 &&
            access_is(memory, 0, payload_pointer, 8),
            "ping 的显式 addrlen=0 在读取头部后返回 EINVAL");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer, 8,
                    0, 0, 0) == encoded_error(_EDESTADDRREQ) &&
            memory->read_calls == 1 &&
            access_is(memory, 0, payload_pointer, 8),
            "未连接 ping 在读取有效头部后返回 EDESTADDRREQ");

    CHECK(attach_connected_host_pair(fixture, socket_number),
            "目的状态测试建立无网络 socket peer");
    reset_access(memory);
    memory->fail_read_at = payload_pointer + 8;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    (dword_t) socket_number, payload_pointer,
                    sizeof(payload), 0, 0, 0) == encoded_error(_EFAULT) &&
            memory->read_calls == 2 &&
            access_is(memory, 0, payload_pointer, 8) &&
            access_is(memory, 1, payload_pointer + 8, 1),
            "已连接 ping 越过目的地址检查后才读取剩余 payload");
    return 0;
}

static int test_destination_state(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault) {
    CHECK(check_destination_state(fixture, memory, fault,
                    AF_INET_, IPPROTO_ICMP, 8) == 0,
            "IPv4 ping 目的状态正确");
    CHECK(check_destination_state(fixture, memory, fault,
                    AF_INET6_, IPPROTO_ICMPV6, 128) == 0,
            "IPv6 ping 目的状态正确");
    return 0;
}

static int test_sendmsg_split_prefix(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault) {
    int socket_number = create_ping_socket(
            fixture, memory, fault, AF_INET_, IPPROTO_ICMP);
    CHECK(socket_number >= 0, "sendmsg ping guest socket 创建成功");
    CHECK(attach_connected_host_pair(fixture, socket_number),
            "sendmsg ping 建立无网络 socket peer");

    const byte_t first[3] = {8, 0, 0x11};
    const byte_t second[5] = {0x22, 0x33, 0x44, 0x55, 0x66};
    const byte_t tail[3] = {0x77, 0x88, 0x99};
    qword_t first_pointer = put_user(
            memory, 0x1000, first, sizeof(first));
    qword_t second_pointer = put_user(
            memory, 0x1100, second, sizeof(second));
    qword_t tail_pointer = put_user(
            memory, 0x1200, tail, sizeof(tail));
    struct aarch64_linux_iovec_wire vectors[] = {
        {.base = USER_BASE, .length = 0},
        {.base = first_pointer, .length = sizeof(first)},
        {.base = USER_BASE, .length = 0},
        {.base = second_pointer, .length = sizeof(second)},
        {.base = tail_pointer, .length = sizeof(tail)},
    };
    qword_t vectors_pointer = put_user(
            memory, 0x2000, vectors, sizeof(vectors));
    struct aarch64_linux_user_msghdr message = {
        .vectors = vectors_pointer,
        .vector_count = sizeof(vectors) / sizeof(vectors[0]),
    };
    qword_t message_pointer = put_user(
            memory, 0x3000, &message, sizeof(message));

    reset_access(memory);
    memory->fail_read_at = tail_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    (dword_t) socket_number, message_pointer,
                    0, 0, 0, 0) == encoded_error(_EFAULT) &&
            memory->read_calls == 5 &&
            access_count_at(memory, first_pointer, sizeof(first)) == 1 &&
            access_count_at(memory, second_pointer, sizeof(second)) == 1 &&
            access_count_at(memory, tail_pointer, sizeof(tail)) == 1,
            "sendmsg 跨 iovec 读取八字节头一次，再从精确尾部继续");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    (dword_t) socket_number, message_pointer,
                    0, 0, 0, 0) == sizeof(first) + sizeof(second) +
                            sizeof(tail) &&
            memory->read_calls == 5 &&
            access_count_at(memory, first_pointer, sizeof(first)) == 1 &&
            access_count_at(memory, second_pointer, sizeof(second)) == 1,
            "sendmsg 不重复读取跨 iovec 的 ICMP 前缀");

    byte_t received[sizeof(first) + sizeof(second) + sizeof(tail)] = {0};
    byte_t expected[sizeof(received)];
    memcpy(expected, first, sizeof(first));
    memcpy(expected + sizeof(first), second, sizeof(second));
    memcpy(expected + sizeof(first) + sizeof(second), tail, sizeof(tail));
    CHECK(recv(fixture->connected_peers[0], received,
                    sizeof(received), 0) == sizeof(received) &&
            memcmp(received, expected, sizeof(expected)) == 0,
            "sendmsg 跨 iovec 的头部与尾部保持 guest 字节顺序");
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
        {"带创建标志的数据报宿主选项", test_flagged_datagram_socket},
        {"ICMP/ICMPv6 公共错误顺序", test_common_errors},
        {"ICMP/ICMPv6 头部与地址顺序", test_address_order},
        {"ICMP/ICMPv6 目的连接状态", test_destination_state},
        {"ICMP sendmsg 跨 iovec 前缀", test_sendmsg_split_prefix},
    };
    unsigned failures = 0;
    for (size_t index = 0; index < sizeof(tests) / sizeof(tests[0]); index++)
        failures += (unsigned) run_test(
                tests[index].name, tests[index].function);
    if (failures != 0)
        fprintf(stderr, "共有 %u 组 AArch64 ping sendto 测试失败\n", failures);
    return failures == 0 ? 0 : 1;
}

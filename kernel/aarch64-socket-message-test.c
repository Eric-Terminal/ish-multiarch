#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "guest/linux/syscall-service.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc67890000)
#define USER_MEMORY_SIZE UINT32_C(0x20000)
#define USER_ACCESS_LOG_SIZE 64

#define SYS_SOCKET 198
#define SYS_BIND 200
#define SYS_CONNECT 203
#define SYS_SETSOCKOPT 208
#define SYS_SENDMSG 211
#define SYS_RECVMSG 212

#define MSG_CMSG_CLOEXEC_ UINT32_C(0x40000000)
#define MSG_CMSG_COMPAT_ UINT32_C(0x80000000)

#define LINUX_SOL_IPV6 INT32_C(41)
#define LINUX_SOL_UDP INT32_C(17)
#define LINUX_IPV6_PKTINFO INT32_C(50)
#define LINUX_IPV6_HOPLIMIT INT32_C(52)
#define LINUX_UDP_SEGMENT INT32_C(103)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 socket 消息测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct aarch64_linux_msghdr {
    qword_t name;
    dword_t namelen;
    dword_t padding0;
    qword_t iov;
    qword_t iovlen;
    qword_t control;
    qword_t controllen;
    dword_t flags;
    dword_t padding1;
} __attribute__((packed, aligned(8)));

struct aarch64_linux_iovec_wire {
    qword_t base;
    qword_t length;
} __attribute__((packed, aligned(8)));

struct aarch64_linux_cmsghdr {
    qword_t length;
    sdword_t level;
    sdword_t type;
} __attribute__((packed, aligned(8)));

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

struct linux_timeval64 {
    int64_t seconds;
    int64_t microseconds;
};

_Static_assert(sizeof(struct aarch64_linux_msghdr) == 56,
        "AArch64 Linux msghdr 必须固定为 56 字节");
_Static_assert(offsetof(struct aarch64_linux_msghdr, name) == 0 &&
        offsetof(struct aarch64_linux_msghdr, namelen) == 8 &&
        offsetof(struct aarch64_linux_msghdr, iov) == 16 &&
        offsetof(struct aarch64_linux_msghdr, iovlen) == 24 &&
        offsetof(struct aarch64_linux_msghdr, control) == 32 &&
        offsetof(struct aarch64_linux_msghdr, controllen) == 40 &&
        offsetof(struct aarch64_linux_msghdr, flags) == 48,
        "AArch64 Linux msghdr 字段偏移必须与 LP64 ABI 一致");
_Static_assert(sizeof(struct aarch64_linux_iovec_wire) == 16 &&
        offsetof(struct aarch64_linux_iovec_wire, base) == 0 &&
        offsetof(struct aarch64_linux_iovec_wire, length) == 8,
        "AArch64 Linux iovec 必须由两个 64 位字段组成");
_Static_assert(sizeof(struct aarch64_linux_cmsghdr) == 16 &&
        offsetof(struct aarch64_linux_cmsghdr, length) == 0 &&
        offsetof(struct aarch64_linux_cmsghdr, level) == 8 &&
        offsetof(struct aarch64_linux_cmsghdr, type) == 12,
        "AArch64 Linux cmsghdr 字段偏移必须与 LP64 ABI 一致");
_Static_assert(sizeof(struct linux_sockaddr_in) == 16,
        "Linux IPv4 sockaddr wire 必须固定为 16 字节");
_Static_assert(sizeof(struct linux_sockaddr_in6) == 28,
        "Linux IPv6 sockaddr wire 必须固定为 28 字节");
_Static_assert(sizeof(struct linux_timeval64) == 16,
        "AArch64 Linux timeval wire 必须固定为 16 字节");

struct user_access {
    qword_t address;
    dword_t size;
    dword_t access;
};

struct user_memory {
    byte_t *bytes;
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned read_calls;
    unsigned write_calls;
    struct user_access accesses[USER_ACCESS_LOG_SIZE];
    unsigned access_count;
    qword_t observe_fd_write_at;
    struct task *observe_fd_task;
    bool observed_fd_write;
    bool observed_fd_unpublished;
};

struct fixture {
    struct task task;
    struct tgroup group;
    struct guest_linux_syscall_completion completion;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static void reset_access(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->fail_write_at = UINT64_MAX;
    memory->read_calls = 0;
    memory->write_calls = 0;
    memory->access_count = 0;
    memory->observe_fd_write_at = UINT64_MAX;
    memory->observe_fd_task = NULL;
    memory->observed_fd_write = false;
    memory->observed_fd_unpublished = false;
}

static bool range_contains(qword_t address, dword_t size, qword_t target) {
    return size != 0 && target >= address && target - address < size;
}

static bool user_range(qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE)
        return false;
    qword_t relative = address - USER_BASE;
    if (relative > USER_MEMORY_SIZE || size > USER_MEMORY_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
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
    if (address == memory->observe_fd_write_at &&
            size == sizeof(fd_t) && memory->observe_fd_task != NULL) {
        fd_t number;
        memcpy(&number, source, sizeof(number));
        memory->observed_fd_write = true;
        memory->observed_fd_unpublished =
                fdtable_get(memory->observe_fd_task->files,
                        number) == NULL;
    }
    memcpy(memory->bytes + offset, source, size);
    set_fault(fault, address, GUEST_MEMORY_WRITE,
            GUEST_MEMORY_FAULT_NONE);
    return true;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {64, 64};
    fixture->task.group = &fixture->group;
    fixture->task.sighand = sighand_new();
    fixture->task.files = fdtable_new(4);
    lock_init(&fixture->task.waiting_cond_lock);
    lock_init(&fixture->task.ptrace.lock);
    list_init(&fixture->task.queue);
    fixture->task.waiting_poll_notify_fd = -1;
    if (fixture->task.sighand == NULL || IS_ERR(fixture->task.files))
        return false;
    fixture->task.sighand->action[SIGUSR1_].handler =
            UINT64_C(0x1000);
    current = &fixture->task;
    task_thread_store(&fixture->task, pthread_self());
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
    lock(&fixture->task.sighand->lock);
    signal_flush_pending(&fixture->task);
    signal_flush_group_pending(&fixture->group);
    unlock(&fixture->task.sighand->lock);
    sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.ptrace.lock.m);
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
    memcpy(memory->bytes + offset, value, size);
    return USER_BASE + offset;
}

static void get_user(const struct user_memory *memory,
        size_t offset, void *value, size_t size) {
    memcpy(value, memory->bytes + offset, size);
}

static bool saw_access(const struct user_memory *memory,
        qword_t address, dword_t size, enum guest_memory_access access) {
    unsigned count = memory->access_count < USER_ACCESS_LOG_SIZE ?
            memory->access_count : USER_ACCESS_LOG_SIZE;
    for (unsigned index = 0; index < count; index++) {
        if (memory->accesses[index].address == address &&
                memory->accesses[index].size == size &&
                memory->accesses[index].access == (dword_t) access)
            return true;
    }
    return false;
}

static int create_guest_socket(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t domain, dword_t type) {
    return (int) (sqword_t) invoke(fixture, memory, fault, SYS_SOCKET,
            domain, type, 0, 0, 0, 0);
}

static struct linux_sockaddr_in linux_loopback(uint16_t port) {
    return (struct linux_sockaddr_in) {
        .family = AF_INET_,
        .port = port,
        .address = htonl(INADDR_LOOPBACK),
    };
}

static int host_udp_bound(struct sockaddr_in *address) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;
    *address = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
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

static int host_udp6_bound(struct sockaddr_in6 *address) {
    int socket_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;
    memset(address, 0, sizeof(*address));
    address->sin6_family = AF_INET6;
    address->sin6_addr = in6addr_loopback;
#ifdef __APPLE__
    address->sin6_len = sizeof(*address);
#endif
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

static bool wait_readable(int fd) {
    struct pollfd event = {.fd = fd, .events = POLLIN};
    int result;
    do {
        result = poll(&event, 1, 1000);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (event.revents & POLLIN) != 0;
}

static bool bind_guest_udp(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int guest_socket, struct sockaddr_in *host_address) {
    struct linux_sockaddr_in address = linux_loopback(0);
    qword_t pointer = put_user(memory, 0x200, &address, sizeof(address));
    if (invoke(fixture, memory, fault, SYS_BIND,
            (dword_t) guest_socket, pointer, sizeof(address), 0, 0, 0) != 0)
        return false;
    struct fd *socket_fd = f_get_task(&fixture->task, guest_socket);
    if (socket_fd == NULL)
        return false;
    socklen_t length = sizeof(*host_address);
    return getsockname(socket_fd->real_fd,
            (struct sockaddr *) host_address, &length) == 0;
}

static bool send_host_packet(int sender,
        const struct sockaddr_in *destination,
        const void *payload, size_t length) {
    return sendto(sender, payload, length, 0,
            (const struct sockaddr *) destination,
            sizeof(*destination)) == (ssize_t) length;
}

static int test_udp_sendmsg(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t header_offset = 0x1000;
    const size_t vectors_offset = 0x1100;
    const size_t first_offset = 0x2000;
    const size_t second_offset = 0x2100;
    const size_t address_offset = 0x1200;

    struct sockaddr_in host_address;
    int receiver = host_udp_bound(&host_address);
    CHECK(receiver >= 0, "sendmsg host UDP 接收端启动成功");
    int guest_socket = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_DGRAM_);
    CHECK(guest_socket >= 0, "sendmsg guest UDP socket 创建成功");

    struct linux_sockaddr_in destination =
            linux_loopback(host_address.sin_port);
    qword_t destination_pointer = put_user(memory,
            address_offset, &destination, sizeof(destination));
    static const byte_t first[] = {0x00, 'm', 's'};
    static const byte_t second[] = {'g', 0xff, '!'};
    qword_t first_pointer = put_user(memory,
            first_offset, first, sizeof(first));
    qword_t second_pointer = put_user(memory,
            second_offset, second, sizeof(second));
    struct aarch64_linux_iovec_wire vectors[2] = {
        {.base = first_pointer, .length = sizeof(first)},
        {.base = second_pointer, .length = sizeof(second)},
    };
    qword_t vectors_pointer = put_user(memory,
            vectors_offset, vectors, sizeof(vectors));
    struct aarch64_linux_msghdr message = {
        .name = destination_pointer,
        .namelen = sizeof(destination),
        .padding0 = UINT32_C(0xdeadbeef),
        .iov = vectors_pointer,
        .iovlen = 2,
        .flags = UINT32_C(0xa5a5a5a5),
        .padding1 = UINT32_C(0xcafebabe),
    };
    qword_t message_pointer = put_user(memory,
            header_offset, &message, sizeof(message));

    reset_access(memory);
    memory->fail_read_at = message_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    99, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EBADF) && memory->read_calls == 0,
            "sendmsg 在读取 56 字节头前返回 EBADF");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    (dword_t) guest_socket,
                    UINT64_MAX - 31, 0, 0, 0, 0) ==
                    encoded_error(_EFAULT),
            "sendmsg 拒绝回绕的 64 位头地址");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    UINT64_C(0xabcdef0000000000) |
                            (dword_t) guest_socket,
                    message_pointer, UINT64_C(0x1357246800000000),
                    0, 0, 0) == sizeof(first) + sizeof(second) &&
            saw_access(memory, message_pointer, sizeof(message),
                    GUEST_MEMORY_READ) && memory->write_calls == 0,
            "sendmsg 忽略 fd/flags 高位并读取精确 LP64 消息头");
    byte_t received[16] = {0};
    static const byte_t expected[] = {0x00, 'm', 's', 'g', 0xff, '!'};
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(expected) &&
            memcmp(received, expected, sizeof(expected)) == 0,
            "sendmsg 将多个 64 位 iovec 聚合为单个 UDP datagram");
    struct aarch64_linux_msghdr unchanged;
    get_user(memory, header_offset, &unchanged, sizeof(unchanged));
    CHECK(memcmp(&unchanged, &message, sizeof(message)) == 0,
            "sendmsg 忽略输入 msg_flags 且不改写消息头");

    byte_t inet_control[32] = {0};
    struct aarch64_linux_cmsghdr inet_header = {
        .length = sizeof(inet_header) + sizeof(fd_t),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    fd_t ignored_fd = -1;
    memcpy(inet_control, &inet_header, sizeof(inet_header));
    memcpy(inet_control + sizeof(inet_header),
            &ignored_fd, sizeof(ignored_fd));
    qword_t inet_control_pointer = put_user(memory,
            0x1300, inet_control, 24);
    message.control = inet_control_pointer;
    message.controllen = 24;
    put_user(memory, header_offset, &message, sizeof(message));
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    sizeof(expected) &&
            wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(expected),
            "INET sendmsg 忽略 SOL_SOCKET/SCM_RIGHTS 及其中的无效 fd");

    memset(inet_control, 0, sizeof(inet_control));
    inet_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(inet_header) + 12,
        .level = SOL_SOCKET_,
        .type = SCM_CREDENTIALS_,
    };
    memcpy(inet_control, &inet_header, sizeof(inet_header));
    put_user(memory, 0x1300, inet_control, sizeof(inet_control));
    message.controllen = sizeof(inet_control);
    put_user(memory, header_offset, &message, sizeof(message));
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    sizeof(expected) &&
            wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(expected),
            "INET sendmsg 忽略 SOL_SOCKET/SCM_CREDENTIALS");

    memset(inet_control, 0, sizeof(inet_control));
    inet_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(inet_header),
        .level = INT32_MAX,
        .type = INT32_MAX,
    };
    memcpy(inet_control, &inet_header, sizeof(inet_header));
    put_user(memory, 0x1300, inet_control, sizeof(inet_header));
    message.controllen = sizeof(inet_header);
    put_user(memory, header_offset, &message, sizeof(message));
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    sizeof(expected) &&
            wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(expected),
            "INET sendmsg 忽略与协议无关的未知 ancillary level");

    memset(inet_control, 0, sizeof(inet_control));
    inet_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(inet_header) + sizeof(dword_t),
        .level = IPPROTO_IP,
        .type = IP_TTL_,
    };
    dword_t guest_ttl = 64;
    memcpy(inet_control, &inet_header, sizeof(inet_header));
    memcpy(inet_control + sizeof(inet_header),
            &guest_ttl, sizeof(guest_ttl));
    put_user(memory, 0x1300, inet_control, 24);
    message.controllen = 24;
    put_user(memory, header_offset, &message, sizeof(message));
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            recv(receiver, received, sizeof(received), MSG_DONTWAIT) < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK),
            "未翻译的 IP ancillary 在发送前明确返回 EOPNOTSUPP");

    message.name = 0;
    message.namelen = 0;
    put_user(memory, header_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EDESTADDRREQ),
            "未连接 UDP 在 IP ancillary 语义解析前报告缺少目的地址");
    reset_access(memory);
    memory->fail_read_at = inet_control_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EFAULT),
            "sendmsg 在协议错误排序前仍优先复制原始控制缓冲");

    message.name = destination_pointer;
    message.namelen = sizeof(destination);
    message.control = 0;
    message.controllen = 0;
    put_user(memory, header_offset, &message, sizeof(message));

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer,
                    MSG_CMSG_COMPAT_, 0, 0, 0) ==
                    encoded_error(_EINVAL),
            "sendmsg 显式拒绝 32 位兼容控制消息布局");

    struct aarch64_linux_msghdr invalid = message;
    invalid.iovlen = 1025;
    put_user(memory, header_offset, &invalid, sizeof(invalid));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EMSGSIZE) &&
            saw_access(memory, destination_pointer,
                    sizeof(destination), GUEST_MEMORY_READ) &&
            !saw_access(memory, vectors_pointer,
                    sizeof(vectors), GUEST_MEMORY_READ),
            "sendmsg 复制名称后、导入 iovec 表前拒绝超过 IOV_MAX 的数量");

    invalid = message;
    invalid.iov = UINT64_MAX - 7;
    invalid.iovlen = 1;
    put_user(memory, header_offset, &invalid, sizeof(invalid));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EFAULT),
            "sendmsg 拒绝回绕的 64 位 iovec 表地址");

    invalid = message;
    invalid.name = 0;
    invalid.namelen = UINT32_MAX;
    put_user(memory, header_offset, &invalid, sizeof(invalid));
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    guest_socket, destination_pointer,
                    sizeof(destination), 0, 0, 0) == 0,
            "sendmsg 连接式 UDP socket 准备成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    sizeof(expected),
            "sendmsg 在 name 为 NULL 时忽略输入 namelen");
    memset(received, 0, sizeof(received));
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(expected) &&
            memcmp(received, expected, sizeof(expected)) == 0,
            "连接式 sendmsg 的 NULL name datagram 到达");

    invalid.name = destination_pointer;
    invalid.namelen = 0;
    put_user(memory, header_offset, &invalid, sizeof(invalid));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    guest_socket, message_pointer, 0, 0, 0, 0) ==
                    sizeof(expected) &&
            !saw_access(memory, destination_pointer,
                    sizeof(destination), GUEST_MEMORY_READ),
            "sendmsg 将非 NULL name 与零 namelen 按已连接 socket 处理");
    memset(received, 0, sizeof(received));
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(expected) &&
            memcmp(received, expected, sizeof(expected)) == 0,
            "零 namelen 不会误走显式目的地址路径");

    CHECK(f_close_task(&fixture->task, guest_socket) == 0,
            "sendmsg guest UDP socket 清理成功");
    close(receiver);
    return 0;
}

static int test_udp_ancillary_levels(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t message_offset = 0x1a000;
    const size_t vector_offset = 0x1a100;
    const size_t payload_offset = 0x1a200;
    const size_t control_offset = 0x1a300;
    const size_t address4_offset = 0x1a400;
    const size_t address6_offset = 0x1a500;

    const byte_t payload = 'A';
    qword_t payload_pointer = put_user(
            memory, payload_offset, &payload, sizeof(payload));
    struct aarch64_linux_iovec_wire vector = {
        .base = payload_pointer,
        .length = sizeof(payload),
    };
    qword_t vector_pointer = put_user(
            memory, vector_offset, &vector, sizeof(vector));
    byte_t control[48] = {0};
    struct aarch64_linux_cmsghdr header = {0};
    qword_t control_pointer = put_user(
            memory, control_offset, control, sizeof(control));
    struct aarch64_linux_msghdr message = {
        .iov = vector_pointer,
        .iovlen = 1,
        .control = control_pointer,
    };
    qword_t message_pointer = put_user(
            memory, message_offset, &message, sizeof(message));
    byte_t received = 0;
    dword_t integer = 64;

    struct sockaddr_in host4_address;
    int receiver4 = host_udp_bound(&host4_address);
    int ipv4 = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_DGRAM_);
    CHECK(receiver4 >= 0 && ipv4 >= 0,
            "IPv4 ancillary UDP 两端启动成功");
    struct linux_sockaddr_in destination4 =
            linux_loopback(host4_address.sin_port);
    qword_t destination4_pointer = put_user(memory,
            address4_offset, &destination4, sizeof(destination4));
    message.name = destination4_pointer;
    message.namelen = sizeof(destination4);

    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header) + sizeof(integer),
        .level = LINUX_SOL_IPV6,
        .type = LINUX_IPV6_HOPLIMIT,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    message.controllen = 24;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv4, message_pointer, 0, 0, 0, 0) ==
                    sizeof(payload) &&
            wait_readable(receiver4) &&
            recv(receiver4, &received, sizeof(received), 0) ==
                    sizeof(received) && received == payload,
            "IPv4 路径忽略 foreign IPV6-level ancillary");

    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header),
        .level = IPPROTO_IP,
        .type = INT32_MAX,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    put_user(memory, control_offset, control, sizeof(header));
    message.controllen = sizeof(header);
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv4, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            recv(receiver4, &received, sizeof(received), MSG_DONTWAIT) < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK),
            "IPv4 路径在读取 payload 前按 Linux 拒绝未知 IP type");

    uint16_t segment_size = 1200;
    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header) + sizeof(segment_size),
        .level = LINUX_SOL_UDP,
        .type = LINUX_UDP_SEGMENT,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header),
            &segment_size, sizeof(segment_size));
    put_user(memory, control_offset, control, 24);
    message.controllen = 24;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv4, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            recv(receiver4, &received, sizeof(received), MSG_DONTWAIT) < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK),
            "合法 UDP_SEGMENT wire 在 host 未实现时明确失败且不发送");

    header.length = sizeof(header) + sizeof(dword_t);
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv4, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "UDP_SEGMENT 按 Linux 要求精确的 16 位 payload 长度");

    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header),
        .level = LINUX_SOL_UDP,
        .type = INT32_MAX,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    put_user(memory, control_offset, control, sizeof(header));
    message.controllen = sizeof(header);
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv4, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "UDP socket 按 Linux 拒绝未知 SOL_UDP type");

    struct sockaddr_in6 host6_address;
    int receiver6 = host_udp6_bound(&host6_address);
    int ipv6 = create_guest_socket(fixture,
            memory, fault, AF_INET6_, SOCK_DGRAM_);
    CHECK(receiver6 >= 0 && ipv6 >= 0,
            "IPv6 ancillary UDP 两端启动成功");
    struct linux_sockaddr_in6 destination6 = {
        .family = AF_INET6_,
        .port = host6_address.sin6_port,
    };
    memcpy(destination6.address,
            &in6addr_loopback, sizeof(destination6.address));
    qword_t destination6_pointer = put_user(memory,
            address6_offset, &destination6, sizeof(destination6));
    message.name = destination6_pointer;
    message.namelen = sizeof(destination6);

    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header) + sizeof(integer),
        .level = IPPROTO_IP,
        .type = IP_TTL_,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    message.controllen = 24;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv6, message_pointer, 0, 0, 0, 0) ==
                    sizeof(payload) &&
            wait_readable(receiver6) &&
            recv(receiver6, &received, sizeof(received), 0) ==
                    sizeof(received) && received == payload,
            "纯 IPv6 路径忽略 foreign IP-level ancillary");

    header.level = LINUX_SOL_IPV6;
    header.type = LINUX_IPV6_HOPLIMIT;
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv6, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            recv(receiver6, &received, sizeof(received), MSG_DONTWAIT) < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK),
            "纯 IPv6 路径在 payload 前拒绝尚未翻译的已知 type");

    header.length = sizeof(header);
    header.type = INT32_MAX;
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    put_user(memory, control_offset, control, sizeof(header));
    message.controllen = sizeof(header);
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    ipv6, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "纯 IPv6 路径按 Linux 拒绝未知 IPV6 type");

    int mapped = create_guest_socket(fixture,
            memory, fault, AF_INET6_, SOCK_DGRAM_);
    struct fd *mapped_fd = f_get_task(&fixture->task, mapped);
    int ipv6_only = 0;
    CHECK(mapped >= 0 && mapped_fd != NULL &&
            setsockopt(mapped_fd->real_fd, IPPROTO_IPV6, IPV6_V6ONLY,
                    &ipv6_only, sizeof(ipv6_only)) == 0,
            "v4-mapped ancillary UDP socket 启用双栈");
    message.name = destination4_pointer;
    message.namelen = sizeof(destination4);
    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header) + sizeof(integer),
        .level = IPPROTO_IP,
        .type = IP_TTL_,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    message.controllen = 24;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    mapped, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "显式 v4-mapped IPv6 目的地址按 IPv4 路径解析 IP level");

    header.level = LINUX_SOL_IPV6;
    header.type = LINUX_IPV6_HOPLIMIT;
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    mapped, message_pointer, 0, 0, 0, 0) ==
                    sizeof(payload) &&
            wait_readable(receiver4) &&
            recv(receiver4, &received, sizeof(received), 0) ==
                    sizeof(received) && received == payload,
            "v4-mapped IPv6 路径忽略非 pktinfo 的 IPV6 level");

    byte_t mapped_pktinfo[20] = {0};
    mapped_pktinfo[10] = 0xff;
    mapped_pktinfo[11] = 0xff;
    uint32_t loopback = htonl(INADDR_LOOPBACK);
    memcpy(mapped_pktinfo + 12, &loopback, sizeof(loopback));
    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header) + sizeof(mapped_pktinfo),
        .level = LINUX_SOL_IPV6,
        .type = LINUX_IPV6_PKTINFO,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header),
            mapped_pktinfo, sizeof(mapped_pktinfo));
    put_user(memory, control_offset, control, 40);
    message.controllen = 40;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    mapped, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "v4-mapped IPV6_PKTINFO 不会被误当成 foreign level 静默忽略");

    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    mapped, destination4_pointer, sizeof(destination4),
                    0, 0, 0) == 0,
            "v4-mapped ancillary UDP socket 连接成功");
    message.name = 0;
    message.namelen = 0;
    header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(header) + sizeof(integer),
        .level = IPPROTO_IP,
        .type = IP_TTL_,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &integer, sizeof(integer));
    put_user(memory, control_offset, control, 24);
    message.controllen = 24;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    mapped, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "已连接 v4-mapped IPv6 peer 仍按 IPv4 路径解析 ancillary");

    CHECK(f_close_task(&fixture->task, ipv4) == 0 &&
            f_close_task(&fixture->task, ipv6) == 0 &&
            f_close_task(&fixture->task, mapped) == 0,
            "ancillary UDP guest sockets 清理成功");
    close(receiver4);
    close(receiver6);
    return 0;
}

static int test_udp_recvmsg(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t header_offset = 0x3000;
    const size_t vectors_offset = 0x3100;
    const size_t first_offset = 0x4000;
    const size_t second_offset = 0x4100;
    const size_t name_offset = 0x3200;
    qword_t header_pointer = USER_BASE + header_offset;
    qword_t first_pointer = USER_BASE + first_offset;
    qword_t second_pointer = USER_BASE + second_offset;
    qword_t name_pointer = USER_BASE + name_offset;

    int guest_socket = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_DGRAM_);
    struct sockaddr_in guest_address = {0};
    CHECK(guest_socket >= 0 && bind_guest_udp(fixture,
                    memory, fault, guest_socket, &guest_address),
            "recvmsg guest UDP 接收端绑定成功");
    struct sockaddr_in sender_address;
    int sender = host_udp_bound(&sender_address);
    CHECK(sender >= 0, "recvmsg host UDP 发送端启动成功");

    struct aarch64_linux_iovec_wire vectors[2] = {
        {.base = first_pointer, .length = 2},
        {.base = second_pointer, .length = 4},
    };
    qword_t vectors_pointer = put_user(memory,
            vectors_offset, vectors, sizeof(vectors));
    struct aarch64_linux_msghdr message = {
        .name = name_pointer,
        .namelen = sizeof(struct linux_sockaddr_in),
        .padding0 = UINT32_C(0x11223344),
        .iov = vectors_pointer,
        .iovlen = 2,
        .control = 0,
        .controllen = 0,
        .flags = UINT32_C(0xa5a5a5a5),
        .padding1 = UINT32_C(0x55667788),
    };
    put_user(memory, header_offset, &message, sizeof(message));

    reset_access(memory);
    memory->fail_read_at = header_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    99, header_pointer, MSG_DONTWAIT_, 0, 0, 0) ==
                    encoded_error(_EBADF) && memory->read_calls == 0,
            "recvmsg 在读取 56 字节头前返回 EBADF");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    guest_socket, UINT64_MAX - 31,
                    MSG_DONTWAIT_, 0, 0, 0) == encoded_error(_EFAULT),
            "recvmsg 拒绝回绕的 64 位头地址");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    guest_socket, header_pointer,
                    MSG_CMSG_COMPAT_, 0, 0, 0) == encoded_error(_EINVAL),
            "recvmsg 显式拒绝 32 位兼容控制消息布局");

    static const byte_t packet[] = {'v', 'e', 'c', 0x00, 0xff, '!'};
    CHECK(send_host_packet(sender, &guest_address,
                    packet, sizeof(packet)),
            "recvmsg 多向量 UDP 数据发送成功");
    memset(memory->bytes + first_offset, 0xcc, 8);
    memset(memory->bytes + second_offset, 0xcc, 8);
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    UINT64_C(0xabcdef0000000000) |
                            (dword_t) guest_socket,
                    header_pointer, UINT64_C(0x1234567800000000),
                    0, 0, 0) == sizeof(packet) &&
            saw_access(memory, header_pointer, sizeof(message),
                    GUEST_MEMORY_READ) &&
            saw_access(memory, first_pointer, 2,
                    GUEST_MEMORY_WRITE) &&
            saw_access(memory, second_pointer, 4,
                    GUEST_MEMORY_WRITE),
            "recvmsg 忽略 fd/flags 高位并按 64 位 iovec 分散写回");
    CHECK(memcmp(memory->bytes + first_offset, packet, 2) == 0 &&
            memcmp(memory->bytes + second_offset, packet + 2, 4) == 0 &&
            memory->bytes[first_offset + 2] == 0xcc &&
            memory->bytes[second_offset + 4] == 0xcc,
            "recvmsg 只写实际收到的向量负载");

    struct aarch64_linux_msghdr returned;
    get_user(memory, header_offset, &returned, sizeof(returned));
    struct linux_sockaddr_in source;
    get_user(memory, name_offset, &source, sizeof(source));
    CHECK(returned.namelen == sizeof(source) &&
            returned.controllen == 0 && returned.flags == 0 &&
            source.family == AF_INET_ &&
            source.port == sender_address.sin_port,
            "recvmsg 写回源地址、真实 namelen、零 controllen 与 flags");
    CHECK(returned.name == message.name &&
            returned.iov == message.iov &&
            returned.iovlen == message.iovlen &&
            returned.control == message.control &&
            returned.padding0 == message.padding0 &&
            returned.padding1 == message.padding1,
            "recvmsg 不覆盖三个输出字段之外的消息头字节");
    CHECK(saw_access(memory,
                    header_pointer + offsetof(
                            struct aarch64_linux_msghdr, namelen),
                    sizeof(returned.namelen), GUEST_MEMORY_WRITE) &&
            saw_access(memory,
                    header_pointer + offsetof(
                            struct aarch64_linux_msghdr, controllen),
                    sizeof(returned.controllen), GUEST_MEMORY_WRITE) &&
            saw_access(memory,
                    header_pointer + offsetof(
                            struct aarch64_linux_msghdr, flags),
                    sizeof(returned.flags), GUEST_MEMORY_WRITE) &&
            !saw_access(memory, header_pointer, sizeof(returned),
                    GUEST_MEMORY_WRITE),
            "recvmsg 仅以 LP64 精确偏移写回三个元数据字段");

    static const byte_t long_packet[] = "truncate";
    CHECK(send_host_packet(sender, &guest_address,
                    long_packet, sizeof(long_packet) - 1),
            "recvmsg MSG_TRUNC 数据发送成功");
    vectors[0].length = 3;
    put_user(memory, vectors_offset, vectors, sizeof(vectors[0]));
    message.name = 0;
    message.namelen = UINT32_MAX;
    message.iovlen = 1;
    message.flags = UINT32_C(0xffffffff);
    put_user(memory, header_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    guest_socket, header_pointer, MSG_TRUNC_, 0, 0, 0) ==
                    sizeof(long_packet) - 1,
            "recvmsg MSG_TRUNC 返回完整 UDP datagram 长度");
    get_user(memory, header_offset, &returned, sizeof(returned));
    CHECK(returned.namelen == UINT32_MAX && returned.controllen == 0 &&
            (returned.flags & MSG_TRUNC_) != 0 &&
            memcmp(memory->bytes + first_offset, long_packet, 3) == 0,
            "recvmsg 的 NULL name 保留输入 namelen 并报告截断标志");

    message = (struct aarch64_linux_msghdr) {
        .iov = vectors_pointer,
        .iovlen = 1025,
    };
    put_user(memory, header_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    guest_socket, header_pointer,
                    MSG_DONTWAIT_, 0, 0, 0) == encoded_error(_EMSGSIZE) &&
            memory->read_calls == 1,
            "recvmsg 在导入 iovec 表前拒绝超过 IOV_MAX 的数量");

    static const byte_t fault_packet[] = "fault";
    CHECK(send_host_packet(sender, &guest_address,
                    fault_packet, sizeof(fault_packet) - 1),
            "recvmsg payload fault 数据发送成功");
    vectors[0] = (struct aarch64_linux_iovec_wire) {
        .base = first_pointer,
        .length = sizeof(fault_packet) - 1,
    };
    put_user(memory, vectors_offset, vectors, sizeof(vectors[0]));
    message = (struct aarch64_linux_msghdr) {
        .iov = vectors_pointer,
        .iovlen = 1,
    };
    put_user(memory, header_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_write_at = first_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    guest_socket, header_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EFAULT),
            "recvmsg 将已消费 datagram 的 payload 写回 fault 报告为 EFAULT");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    guest_socket, header_pointer,
                    MSG_DONTWAIT_, 0, 0, 0) == encoded_error(_EAGAIN),
            "recvmsg payload 写回失败后不会重复交付 datagram");

    CHECK(f_close_task(&fixture->task, guest_socket) == 0,
            "recvmsg guest UDP socket 清理成功");
    close(sender);
    return 0;
}

static int marker_close(struct fd *fd) {
    int *closes = fd->data;
    (*closes)++;
    return 0;
}

static const struct fd_ops marker_ops = {
    .close = marker_close,
};

static unsigned occupied_fds(struct task *task) {
    unsigned occupied = 0;
    for (fd_t number = 0;
            number < (fd_t) task->group->limits[RLIMIT_NOFILE_].cur;
            number++) {
        if (f_get_task(task, number) != NULL)
            occupied++;
    }
    return occupied;
}

static qword_t put_abstract_address(struct user_memory *memory,
        size_t offset, const char *suffix, dword_t *length) {
    byte_t wire[2 + 32] = {0};
    uint16_t family = AF_LOCAL_;
    size_t suffix_length = strlen(suffix);
    memcpy(wire, &family, sizeof(family));
    memcpy(wire + sizeof(family) + 1, suffix, suffix_length);
    *length = (dword_t) (sizeof(family) + 1 + suffix_length);
    return put_user(memory, offset, wire, *length);
}

static int send_control_packet(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int sender, const void *control, size_t control_length,
        qword_t destination, sdword_t destination_length,
        dword_t flags, size_t base_offset) {
    static const byte_t payload = 'R';
    qword_t payload_pointer = put_user(memory,
            base_offset, &payload, sizeof(payload));
    struct aarch64_linux_iovec_wire vector = {
        .base = payload_pointer,
        .length = sizeof(payload),
    };
    qword_t vector_pointer = put_user(memory,
            base_offset + 0x40, &vector, sizeof(vector));
    qword_t control_pointer = control_length == 0 ? 0 :
            put_user(memory, base_offset + 0x80,
                    control, control_length);
    struct aarch64_linux_msghdr message = {
        .name = destination,
        .namelen = (dword_t) destination_length,
        .iov = vector_pointer,
        .iovlen = 1,
        .control = control_pointer,
        .controllen = control_length,
        .flags = UINT32_C(0xffffffff),
    };
    qword_t message_pointer = put_user(memory,
            base_offset + 0xc0, &message, sizeof(message));
    return (int) (sqword_t) invoke(fixture, memory, fault, SYS_SENDMSG,
            (dword_t) sender, message_pointer, flags, 0, 0, 0);
}

static int send_one_right_to(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int sender, fd_t transferred, qword_t destination,
        sdword_t destination_length, size_t base_offset) {
    byte_t control[24] = {0};
    struct aarch64_linux_cmsghdr header = {
        .length = sizeof(header) + sizeof(transferred),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &transferred, sizeof(transferred));
    return send_control_packet(fixture, memory, fault,
            sender, control, sizeof(control), destination,
            destination_length, 0, base_offset);
}

static int send_one_right(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int sender, fd_t transferred, size_t base_offset) {
    return send_one_right_to(fixture, memory, fault,
            sender, transferred, 0, 0, base_offset);
}

static int send_two_rights(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int sender, fd_t first, fd_t second, size_t base_offset) {
    byte_t control[24] = {0};
    struct aarch64_linux_cmsghdr header = {
        .length = sizeof(header) + 2 * sizeof(fd_t),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    memcpy(control, &header, sizeof(header));
    memcpy(control + sizeof(header), &first, sizeof(first));
    memcpy(control + sizeof(header) + sizeof(first),
            &second, sizeof(second));
    return send_control_packet(fixture, memory, fault,
            sender, control, sizeof(control), 0, 0, 0, base_offset);
}

static int receive_rights_message(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        int receiver, qword_t control_capacity, dword_t flags,
        size_t base_offset, struct aarch64_linux_msghdr *returned,
        struct aarch64_linux_cmsghdr *returned_control,
        fd_t *received_fd) {
    qword_t payload_pointer = USER_BASE + base_offset;
    struct aarch64_linux_iovec_wire vector = {
        .base = payload_pointer,
        .length = 1,
    };
    qword_t vector_pointer = put_user(memory,
            base_offset + 0x40, &vector, sizeof(vector));
    byte_t control[32];
    memset(control, 0xcc, sizeof(control));
    qword_t control_pointer = put_user(memory,
            base_offset + 0x80, control, sizeof(control));
    struct aarch64_linux_msghdr message = {
        .iov = vector_pointer,
        .iovlen = 1,
        .control = control_pointer,
        .controllen = control_capacity,
    };
    qword_t message_pointer = put_user(memory,
            base_offset + 0xc0, &message, sizeof(message));
    memory->bytes[base_offset] = 0;
    reset_access(memory);
    int result = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_RECVMSG, (dword_t) receiver,
            message_pointer, flags, 0, 0, 0);
    if (result < 0)
        return result;
    get_user(memory, base_offset + 0xc0,
            returned, sizeof(*returned));
    get_user(memory, base_offset + 0x80,
            returned_control, sizeof(*returned_control));
    if (control_capacity >= sizeof(*returned_control) +
            sizeof(*received_fd)) {
        get_user(memory,
                base_offset + 0x80 + sizeof(*returned_control),
                received_fd, sizeof(*received_fd));
    }
    return result;
}

static int test_scm_rights(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int receiver = create_guest_socket(fixture,
            memory, fault, AF_LOCAL_, SOCK_DGRAM_);
    int sender = create_guest_socket(fixture,
            memory, fault, AF_LOCAL_, SOCK_DGRAM_);
    CHECK(receiver >= 0 && sender >= 0,
            "SCM_RIGHTS Unix datagram socket 创建成功");
    reset_access(memory);
    memory->fail_read_at = USER_BASE + 0x5000;
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, NULL, 0, 0, 0, 0, 0x5000) == _EFAULT,
            "未连接 AF_UNIX datagram 在 ENOTCONN 前复制 payload 并报告 EFAULT");
    reset_access(memory);
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, NULL, 0, 0, 0, 0, 0x5000) == _ENOTCONN,
            "未连接 AF_UNIX datagram 在 payload 可读时返回 ENOTCONN");
    dword_t address_length;
    qword_t address_pointer = put_abstract_address(memory,
            0x6000, "aarch64-msg-rights", &address_length);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    receiver, address_pointer, address_length,
                    0, 0, 0) == 0 &&
            invoke(fixture, memory, fault, SYS_CONNECT,
                    sender, address_pointer, address_length,
                    0, 0, 0) == 0,
            "SCM_RIGHTS Unix datagram 通道连接成功");

    int marker_closes = 0;
    struct fd *marker = fd_create(&marker_ops);
    CHECK(marker != NULL, "SCM_RIGHTS 标记 fd 创建成功");
    marker->data = &marker_closes;
    fd_t transferred = f_install_task(&fixture->task, marker, 0);
    CHECK(transferred >= 0, "SCM_RIGHTS 标记 fd 安装成功");
    unsigned baseline = occupied_fds(&fixture->task);

    byte_t validation_control[24] = {0};
    struct aarch64_linux_cmsghdr validation_header = {
        .length = sizeof(validation_header) - 1,
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    memcpy(validation_control, &validation_header,
            sizeof(validation_header));
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control,
                    sizeof(validation_control), 0, 0, 0, 0xb000) ==
                    _EINVAL,
            "sendmsg 拒绝短于 cmsghdr 的控制消息长度");
    validation_header.length = sizeof(validation_control) + 1;
    memcpy(validation_control, &validation_header,
            sizeof(validation_header));
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control,
                    sizeof(validation_control), 0, 0, 0, 0xb200) ==
                    _EINVAL,
            "sendmsg 拒绝超出控制缓冲区的 cmsg_len");
    validation_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(validation_header) + sizeof(fd_t),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    fd_t invalid_fd = -1;
    memset(validation_control, 0, sizeof(validation_control));
    memcpy(validation_control, &validation_header,
            sizeof(validation_header));
    memcpy(validation_control + sizeof(validation_header),
            &invalid_fd, sizeof(invalid_fd));
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control,
                    sizeof(validation_control), 0, 0, 0, 0xb400) ==
                    _EBADF,
            "sendmsg 在发送 datagram 前拒绝无效 SCM_RIGHTS fd");
    validation_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(validation_header),
        .level = SOL_SOCKET_,
        .type = INT32_MAX,
    };
    memcpy(validation_control, &validation_header,
            sizeof(validation_header));
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control,
                    sizeof(validation_header), 0, 0, 0, 0xb600) ==
                    _EINVAL,
            "sendmsg 拒绝未支持的 SOL_SOCKET 控制消息类型");

    struct aarch64_linux_msghdr validation_message;
    struct aarch64_linux_cmsghdr validation_result;
    fd_t validation_fd = -1;
    CHECK(receive_rights_message(fixture, memory, fault,
                    receiver, 24, MSG_DONTWAIT_, 0xb800,
                    &validation_message, &validation_result,
                    &validation_fd) == _EAGAIN &&
            occupied_fds(&fixture->task) == baseline,
            "控制消息校验失败不会发送 datagram 或泄漏 fd 引用");

    validation_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(validation_header) + sizeof(transferred),
        .level = SOL_SOCKET_,
        .type = SCM_RIGHTS_,
    };
    memset(validation_control, 0, sizeof(validation_control));
    memcpy(validation_control, &validation_header,
            sizeof(validation_header));
    memcpy(validation_control + sizeof(validation_header),
            &transferred, sizeof(transferred));
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control,
                    sizeof(validation_control), 0, 0,
                    MSG_CMSG_CLOEXEC_, 0xba00) == 1 &&
            receive_rights_message(fixture, memory, fault,
                    receiver, 24, 0, 0xbc00,
                    &validation_message, &validation_result,
                    &validation_fd) == 1 &&
            validation_message.flags == 0 &&
            f_get_task(&fixture->task, validation_fd) ==
                    f_get_task(&fixture->task, transferred) &&
            f_getfd_task(&fixture->task, validation_fd) == 0 &&
            f_close_task(&fixture->task, validation_fd) == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "sendmsg 忽略仅供 recvmsg 使用的 MSG_CMSG_CLOEXEC");

    validation_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(validation_header),
        .level = 0,
        .type = INT32_MAX,
    };
    memcpy(validation_control, &validation_header,
            sizeof(validation_header));
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control,
                    sizeof(validation_header), 0, 0, 0, 0xbe00) == 1 &&
            receive_rights_message(fixture, memory, fault,
                    receiver, 24, 0, 0xc000,
                    &validation_message, &validation_result,
                    &validation_fd) == 1 &&
            memory->bytes[0xc000] == 'R' &&
            validation_message.controllen == 0 &&
            validation_message.flags == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "AF_UNIX 忽略结构合法的非 SOL_SOCKET 控制消息");

    memset(validation_control, 0x5a, 15);
    CHECK(send_control_packet(fixture, memory, fault,
                    sender, validation_control, 15,
                    0, 0, 0, 0xc200) == 1 &&
            receive_rights_message(fixture, memory, fault,
                    receiver, 24, 0, 0xc400,
                    &validation_message, &validation_result,
                    &validation_fd) == 1 &&
            validation_message.controllen == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "sendmsg 忽略不足一个 cmsghdr 的尾部控制字节");

    byte_t large_control[2049] = {0};
    validation_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(large_control),
        .level = 0,
        .type = INT32_MAX,
    };
    memcpy(large_control, &validation_header,
            sizeof(validation_header));
    static const byte_t large_payload = 'L';
    qword_t large_payload_pointer = put_user(memory,
            0xe000, &large_payload, sizeof(large_payload));
    struct aarch64_linux_iovec_wire large_vector = {
        .base = large_payload_pointer,
        .length = sizeof(large_payload),
    };
    qword_t large_vector_pointer = put_user(memory,
            0xe040, &large_vector, sizeof(large_vector));
    qword_t large_control_pointer = put_user(memory,
            0x10000, large_control, sizeof(large_control));
    struct aarch64_linux_msghdr large_message = {
        .iov = large_vector_pointer,
        .iovlen = 1,
        .control = large_control_pointer,
        .controllen = sizeof(large_control),
    };
    qword_t large_message_pointer = put_user(memory,
            0xe0c0, &large_message, sizeof(large_message));
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    sender, large_message_pointer, 0, 0, 0, 0) == 1 &&
            receive_rights_message(fixture, memory, fault,
                    receiver, 24, 0, 0x11000,
                    &validation_message, &validation_result,
                    &validation_fd) == 1 &&
            memory->bytes[0x11000] == large_payload &&
            validation_message.controllen == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "sendmsg 动态接收超过 2048 字节的合法控制缓冲");
    large_message.control = USER_BASE;
    large_message.controllen = USER_MEMORY_SIZE;
    put_user(memory, 0xe0c0, &large_message, sizeof(large_message));
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    sender, large_message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_ENOBUFS) &&
            receive_rights_message(fixture, memory, fault,
                    receiver, 24, MSG_DONTWAIT_, 0x11800,
                    &validation_message, &validation_result,
                    &validation_fd) == _EAGAIN &&
            occupied_fds(&fixture->task) == baseline,
            "sendmsg 按 Linux 默认 optmem 上限拒绝 128 KiB 控制缓冲");

    CHECK(send_one_right(fixture, memory, fault,
                    sender, transferred, 0x7000) == 1,
            "SCM_RIGHTS 回滚样本发送成功");
    const size_t receive_base = 0x8000;
    const qword_t payload_pointer = USER_BASE + receive_base;
    struct aarch64_linux_iovec_wire vector = {
        .base = payload_pointer,
        .length = 1,
    };
    qword_t vector_pointer = put_user(memory,
            receive_base + 0x40, &vector, sizeof(vector));
    byte_t empty_control[24] = {0};
    qword_t control_pointer = put_user(memory,
            receive_base + 0x80, empty_control, sizeof(empty_control));
    struct aarch64_linux_msghdr message = {
        .iov = vector_pointer,
        .iovlen = 1,
        .control = control_pointer,
        .controllen = sizeof(empty_control),
    };
    qword_t message_pointer = put_user(memory,
            receive_base + 0xc0, &message, sizeof(message));
    reset_access(memory);
    memory->fail_write_at = message_pointer +
            offsetof(struct aarch64_linux_msghdr, flags);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    receiver, message_pointer,
                    MSG_CMSG_CLOEXEC_, 0, 0, 0) ==
                    encoded_error(_EFAULT) &&
            occupied_fds(&fixture->task) == baseline + 1,
            "recvmsg 最终元数据 fault 保留此前已成功交付的 SCM_RIGHTS");
    struct aarch64_linux_cmsghdr fault_control;
    get_user(memory, receive_base + 0x80,
            &fault_control, sizeof(fault_control));
    fd_t delivered_before_fault;
    get_user(memory,
            receive_base + 0x80 + sizeof(fault_control),
            &delivered_before_fault, sizeof(delivered_before_fault));
    CHECK(fault_control.length ==
                    sizeof(fault_control) + sizeof(fd_t) &&
            f_get_task(&fixture->task, delivered_before_fault) ==
                    f_get_task(&fixture->task, transferred) &&
            f_getfd_task(&fixture->task, delivered_before_fault) ==
                    FD_CLOEXEC_ &&
            f_close_task(&fixture->task, delivered_before_fault) == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "元数据 fault 前交付的 fd 保持可见且可由 guest 正常关闭");

    reset_access(memory);
    CHECK(send_one_right(fixture, memory, fault,
                    sender, transferred, 0x9000) == 1,
            "SCM_RIGHTS CLOEXEC 样本发送成功");
    memset(memory->bytes + receive_base, 0, 1);
    memset(memory->bytes + receive_base + 0x80,
            0, sizeof(empty_control));
    put_user(memory, receive_base + 0xc0, &message, sizeof(message));
    reset_access(memory);
    memory->observe_fd_write_at =
            control_pointer + sizeof(struct aarch64_linux_cmsghdr);
    memory->observe_fd_task = &fixture->task;
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    receiver, message_pointer,
                    MSG_CMSG_CLOEXEC_, 0, 0, 0) == 1 &&
            memory->observed_fd_write &&
            memory->observed_fd_unpublished,
            "recvmsg 先写出 fd 编号，再向并发 guest 线程发布描述符");
    struct aarch64_linux_msghdr returned;
    get_user(memory, receive_base + 0xc0,
            &returned, sizeof(returned));
    struct aarch64_linux_cmsghdr returned_control;
    get_user(memory, receive_base + 0x80,
            &returned_control, sizeof(returned_control));
    fd_t received_fd;
    get_user(memory,
            receive_base + 0x80 + sizeof(returned_control),
            &received_fd, sizeof(received_fd));
    CHECK(memory->bytes[receive_base] == 'R' &&
            returned.controllen == sizeof(empty_control) &&
            returned.flags == MSG_CMSG_CLOEXEC_ &&
            returned_control.length ==
                    sizeof(returned_control) + sizeof(received_fd) &&
            returned_control.level == SOL_SOCKET_ &&
            returned_control.type == SCM_RIGHTS_,
            "recvmsg 写回 LP64 cmsghdr、controllen 与消息负载");
    CHECK(received_fd >= 0 && received_fd != transferred &&
            f_get_task(&fixture->task, received_fd) ==
                    f_get_task(&fixture->task, transferred) &&
            f_getfd_task(&fixture->task, received_fd) == FD_CLOEXEC_,
            "MSG_CMSG_CLOEXEC 以关闭执行标记安装同一文件对象");
    CHECK(f_close_task(&fixture->task, received_fd) == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "接收的 SCM_RIGHTS fd 可独立关闭且不泄漏槽位");

    CHECK(send_one_right(fixture, memory, fault,
                    sender, transferred, 0x9800) == 1,
            "SCM_RIGHTS fd 编号写回 fault 样本发送成功");
    message.control = control_pointer;
    message.controllen = sizeof(empty_control);
    put_user(memory, receive_base + 0xc0, &message, sizeof(message));
    reset_access(memory);
    memory->fail_write_at = control_pointer +
            sizeof(struct aarch64_linux_cmsghdr);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    receiver, message_pointer, 0, 0, 0, 0) == 1,
            "recvmsg 吞掉 ancillary 写号 fault 并保留 payload 成功结果");
    get_user(memory, receive_base + 0xc0,
            &returned, sizeof(returned));
    CHECK(returned.controllen == 0 &&
            (returned.flags & MSG_CTRUNC_) != 0 &&
            occupied_fds(&fixture->task) == baseline,
            "fd 编号写回失败不发布不可见槽位并报告 MSG_CTRUNC");

    CHECK(send_two_rights(fixture, memory, fault,
                    sender, transferred, transferred, 0x9c00) == 1,
            "SCM_RIGHTS fdtable 部分交付样本发送成功");
    memset(memory->bytes + receive_base + 0x80,
            0, sizeof(empty_control));
    put_user(memory, receive_base + 0xc0, &message, sizeof(message));
    qword_t original_limit =
            fixture->group.limits[RLIMIT_NOFILE_].cur;
    fixture->group.limits[RLIMIT_NOFILE_].cur = baseline + 1;
    reset_access(memory);
    int partial_result = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_RECVMSG, receiver, message_pointer, 0, 0, 0, 0);
    fixture->group.limits[RLIMIT_NOFILE_].cur = original_limit;
    get_user(memory, receive_base + 0xc0,
            &returned, sizeof(returned));
    get_user(memory, receive_base + 0x80,
            &returned_control, sizeof(returned_control));
    fd_t partial_fd;
    get_user(memory,
            receive_base + 0x80 + sizeof(returned_control),
            &partial_fd, sizeof(partial_fd));
    CHECK(partial_result == 1 && returned.controllen == 24 &&
            (returned.flags & MSG_CTRUNC_) != 0 &&
            returned_control.length == 20 &&
            f_get_task(&fixture->task, partial_fd) ==
                    f_get_task(&fixture->task, transferred) &&
            occupied_fds(&fixture->task) == baseline + 1,
            "fdtable 满载时保留先前交付 fd、丢弃余项并报告截断");
    CHECK(f_close_task(&fixture->task, partial_fd) == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "部分交付后的可见 fd 可独立关闭且无隐藏槽位");

    CHECK(send_one_right(fixture, memory, fault,
                    sender, transferred, 0xa000) == 1,
            "SCM_RIGHTS 截断样本发送成功");
    message.control = 0;
    message.controllen = 0;
    put_user(memory, receive_base + 0xc0, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVMSG,
                    receiver, message_pointer, 0, 0, 0, 0) == 1,
            "recvmsg 在无控制缓冲时仍消费 SCM_RIGHTS datagram");
    get_user(memory, receive_base + 0xc0,
            &returned, sizeof(returned));
    CHECK(returned.controllen == 0 &&
            (returned.flags & MSG_CTRUNC_) != 0 &&
            occupied_fds(&fixture->task) == baseline,
            "recvmsg 丢弃无处交付的 fd 并报告 MSG_CTRUNC");

    CHECK(send_one_right(fixture, memory, fault,
                    sender, transferred, 0xd000) == 1,
            "SCM_RIGHTS MSG_PEEK 样本发送成功");
    fd_t peek_fds[3] = {-1, -1, -1};
    struct aarch64_linux_msghdr peek_messages[3];
    struct aarch64_linux_cmsghdr peek_controls[3];
    for (size_t index = 0; index < 3; index++) {
        dword_t flags = index < 2 ? MSG_PEEK_ : 0;
        int peek_result = receive_rights_message(fixture, memory, fault,
                        receiver, 24, flags, 0xd200,
                        &peek_messages[index], &peek_controls[index],
                        &peek_fds[index]);
        CHECK(peek_result == 1 && memory->bytes[0xd200] == 'R',
                "MSG_PEEK 与正式接收均返回原始 datagram 负载");
        CHECK(peek_messages[index].controllen == 24,
                "MSG_PEEK 与正式接收均写回 24 字节控制长度");
        CHECK(peek_messages[index].flags == 0,
                "MSG_PEEK 输入标志不会出现在返回 msg_flags");
        CHECK(peek_controls[index].length == 20 &&
                peek_controls[index].level == SOL_SOCKET_ &&
                peek_controls[index].type == SCM_RIGHTS_,
                "MSG_PEEK 与正式接收均写回完整 SCM_RIGHTS 头");
        CHECK(peek_fds[index] >= 0 &&
                f_get_task(&fixture->task, peek_fds[index]) ==
                        f_get_task(&fixture->task, transferred),
                "MSG_PEEK 与正式接收均安装同一文件对象的新 fd");
    }
    CHECK(peek_fds[0] != peek_fds[1] &&
            peek_fds[0] != peek_fds[2] &&
            peek_fds[1] != peek_fds[2] &&
            occupied_fds(&fixture->task) == baseline + 3,
            "连续两次 MSG_PEEK 不消费队列且分别安装互异 fd");
    fd_t empty_fd = -1;
    CHECK(receive_rights_message(fixture, memory, fault,
                    receiver, 24, MSG_DONTWAIT_, 0xd400,
                    &validation_message, &validation_result,
                    &empty_fd) == _EAGAIN,
            "正式 recvmsg 后 SCM_RIGHTS datagram 已从队列消费");
    for (size_t index = 0; index < 3; index++)
        CHECK(f_close_task(&fixture->task, peek_fds[index]) == 0,
                "MSG_PEEK 交付的 fd 可独立关闭");
    CHECK(occupied_fds(&fixture->task) == baseline,
            "MSG_PEEK 与正式接收未泄漏 fd 槽位");

    static const qword_t control_capacities[] = {20, 23, 24};
    for (size_t index = 0;
            index < sizeof(control_capacities) /
                    sizeof(control_capacities[0]); index++) {
        CHECK(send_one_right(fixture, memory, fault,
                        sender, transferred, 0xd600) == 1,
                "SCM_RIGHTS 控制缓冲边界样本发送成功");
        struct aarch64_linux_msghdr capacity_message;
        struct aarch64_linux_cmsghdr capacity_control;
        fd_t capacity_fd = -1;
        CHECK(receive_rights_message(fixture, memory, fault,
                        receiver, control_capacities[index], 0,
                        0xd800, &capacity_message,
                        &capacity_control, &capacity_fd) == 1 &&
                capacity_message.controllen ==
                        control_capacities[index] &&
                capacity_message.flags == 0 &&
                capacity_control.length == 20 &&
                capacity_control.level == SOL_SOCKET_ &&
                capacity_control.type == SCM_RIGHTS_ &&
                f_get_task(&fixture->task, capacity_fd) ==
                        f_get_task(&fixture->task, transferred),
                "20、23、24 字节控制缓冲均返回 cmsg_len 20 与精确 controllen");
        CHECK(f_close_task(&fixture->task, capacity_fd) == 0 &&
                occupied_fds(&fixture->task) == baseline,
                "控制缓冲边界接收不会泄漏 fd 槽位");
    }

    int explicit_sender = create_guest_socket(fixture,
            memory, fault, AF_LOCAL_, SOCK_DGRAM_);
    CHECK(explicit_sender >= 0,
            "显式目的地址 Unix datagram 发送端创建成功");
    unsigned explicit_baseline = occupied_fds(&fixture->task);
    CHECK(send_one_right_to(fixture, memory, fault,
                    explicit_sender, transferred,
                    address_pointer, address_length, 0xda00) == 1,
            "sendmsg 通过显式 AF_UNIX 目的地址发送 SCM_RIGHTS");
    struct aarch64_linux_msghdr explicit_message;
    struct aarch64_linux_cmsghdr explicit_control;
    fd_t explicit_fd = -1;
    CHECK(receive_rights_message(fixture, memory, fault,
                    receiver, 24, 0, 0xdc00,
                    &explicit_message, &explicit_control,
                    &explicit_fd) == 1 &&
            explicit_message.controllen == 24 &&
            explicit_control.length == 20 &&
            f_get_task(&fixture->task, explicit_fd) ==
                    f_get_task(&fixture->task, transferred) &&
            occupied_fds(&fixture->task) == explicit_baseline + 1,
            "显式目的地址路由将 SCM_RIGHTS 交付给对应接收 socket");
    CHECK(f_close_task(&fixture->task, explicit_fd) == 0 &&
            f_close_task(&fixture->task, explicit_sender) == 0 &&
            occupied_fds(&fixture->task) == baseline,
            "显式目的地址 SCM_RIGHTS 临时 fd 全部清理成功");

    CHECK(f_close_task(&fixture->task, transferred) == 0 &&
            marker_closes == 1,
            "SCM_RIGHTS 所有临时引用均已释放");
    CHECK(f_close_task(&fixture->task, sender) == 0 &&
            f_close_task(&fixture->task, receiver) == 0,
            "SCM_RIGHTS Unix datagram socket 清理成功");
    return 0;
}

struct interrupted_socket_call {
    struct fixture *fixture;
    struct user_memory *memory;
    qword_t number;
    qword_t arguments[6];
    atomic_bool started;
    atomic_bool finished;
    qword_t result;
    dword_t restart;
};

static void host_wakeup_handler(int signal_number) {
    (void) signal_number;
}

static bool wait_for_atomic_flag(
        atomic_bool *flag, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(flag, memory_order_acquire);
}

static void clear_guest_signals(struct fixture *fixture) {
    lock(&fixture->task.sighand->lock);
    signal_flush_pending(&fixture->task);
    signal_flush_group_pending(&fixture->group);
    unlock(&fixture->task.sighand->lock);
}

static bool guest_signal_pending(
        struct fixture *fixture, int signal_number) {
    lock(&fixture->task.sighand->lock);
    bool pending = sigset_has(
            signal_pending_mask_locked(&fixture->task), signal_number);
    unlock(&fixture->task.sighand->lock);
    return pending;
}

static int test_tcp_connect_error_state(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int reserved_port = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in host_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    CHECK(reserved_port >= 0 && bind(reserved_port,
                    (const struct sockaddr *) &host_address,
                    sizeof(host_address)) == 0,
            "TCP 异步错误测试预留回环端口");
    socklen_t host_length = sizeof(host_address);
    CHECK(getsockname(reserved_port,
                    (struct sockaddr *) &host_address,
                    &host_length) == 0,
            "TCP 异步错误测试取得回环端口");
    close(reserved_port);

    int tcp = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_STREAM_);
    struct fd *tcp_socket = f_get_task(&fixture->task, tcp);
    CHECK(tcp >= 0 && tcp_socket != NULL &&
            fd_setflags(tcp_socket, O_NONBLOCK_) == 0,
            "TCP 异步错误测试创建非阻塞 guest socket");
    struct linux_sockaddr_in destination =
            linux_loopback(host_address.sin_port);
    qword_t destination_pointer = put_user(
            memory, 0x18000, &destination, sizeof(destination));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    tcp, destination_pointer, sizeof(destination),
                    0, 0, 0) == encoded_error(_EINPROGRESS),
            "非阻塞 TCP connect 进入 EINPROGRESS");
    lock(&tcp_socket->lock);
    bool pending = tcp_socket->socket.inet_connect_pending;
    unlock(&tcp_socket->lock);
    CHECK(pending, "EINPROGRESS 必须记录连接中状态");

    struct pollfd wait = {
        .fd = tcp_socket->real_fd,
        .events = POLLOUT,
    };
    CHECK(poll(&wait, 1, 2000) == 1 &&
            (wait.revents & (POLLOUT | POLLERR | POLLHUP)) != 0,
            "回环拒绝连接在两秒内产生发送状态");

    byte_t payload = 0x5a;
    qword_t payload_pointer = put_user(
            memory, 0x18100, &payload, sizeof(payload));
    struct aarch64_linux_iovec_wire vector = {
        .base = payload_pointer,
        .length = sizeof(payload),
    };
    qword_t vector_pointer = put_user(
            memory, 0x18200, &vector, sizeof(vector));
    struct aarch64_linux_msghdr message = {
        .iov = vector_pointer,
        .iovlen = 1,
    };
    qword_t message_pointer = put_user(
            memory, 0x18300, &message, sizeof(message));

    clear_guest_signals(fixture);
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    encoded_error(_ECONNREFUSED) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            !guest_signal_pending(fixture, SIGPIPE_),
            "sendmsg 在 payload fault 前消费异步 ECONNREFUSED 且不发送 SIGPIPE");
    lock(&tcp_socket->lock);
    pending = tcp_socket->socket.inet_connect_pending;
    unlock(&tcp_socket->lock);
    CHECK(!pending, "消费异步错误后清除连接中状态");

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    encoded_error(_EPIPE) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "异步错误只消费一次，后续未连接发送返回 EPIPE");
    CHECK(f_close_task(&fixture->task, tcp) == 0,
            "TCP 异步错误 guest socket 清理成功");
    return 0;
}

static void queue_guest_signal(
        struct fixture *fixture, int signal_number) {
    struct task *saved = current;
    current = NULL;
    deliver_signal(&fixture->task, signal_number, (struct siginfo_) {
        .code = SI_USER_,
        .payload_kind = SIGNAL_INFO_PAYLOAD_KILL,
    });
    current = saved;
}

static int test_stream_sendmsg_error_order(
        struct fixture *fixture, struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t message_offset = 0x16000;
    const size_t vector_offset = 0x16100;
    const size_t control_offset = 0x16200;
    const size_t payload_offset = 0x16300;
    const size_t name_offset = 0x16400;

    const byte_t payload = 'T';
    qword_t payload_pointer = put_user(
            memory, payload_offset, &payload, sizeof(payload));
    struct aarch64_linux_iovec_wire vector = {
        .base = payload_pointer,
        .length = sizeof(payload),
    };
    qword_t vector_pointer = put_user(
            memory, vector_offset, &vector, sizeof(vector));
    byte_t control[24] = {0};
    struct aarch64_linux_cmsghdr control_header = {
        .length = sizeof(control_header),
        .level = SOL_SOCKET_,
        .type = INT32_MAX,
    };
    memcpy(control, &control_header, sizeof(control_header));
    qword_t control_pointer = put_user(
            memory, control_offset, control, sizeof(control));
    struct aarch64_linux_msghdr message = {
        .iov = vector_pointer,
        .iovlen = 1,
        .control = control_pointer,
        .controllen = sizeof(control_header),
    };
    qword_t message_pointer = put_user(
            memory, message_offset, &message, sizeof(message));

    int tcp = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_STREAM_);
    CHECK(tcp >= 0, "未连接 TCP sendmsg socket 创建成功");
    struct fd *tcp_socket = f_get_task(&fixture->task, tcp);
    CHECK(tcp_socket != NULL &&
            fd_setflags(tcp_socket, O_NONBLOCK_) == 0,
            "TCP 连接中状态测试启用非阻塞模式");
    lock(&tcp_socket->lock);
    tcp_socket->socket.inet_connect_pending = true;
    unlock(&tcp_socket->lock);
    clear_guest_signals(fixture);
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EAGAIN) &&
            saw_access(memory, control_pointer,
                    sizeof(control_header), GUEST_MEMORY_READ) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            !guest_signal_pending(fixture, SIGPIPE_),
            "非阻塞 TCP 连接中在 payload fault 前返回 EAGAIN 且不发送 SIGPIPE");
    lock(&tcp_socket->lock);
    tcp_socket->socket.inet_connect_pending = false;
    unlock(&tcp_socket->lock);
    CHECK(fd_setflags(tcp_socket, 0) == 0,
            "TCP 连接中状态测试恢复阻塞模式");
    clear_guest_signals(fixture);
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    encoded_error(_EPIPE) &&
            saw_access(memory, control_pointer,
                    sizeof(control_header), GUEST_MEMORY_READ) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            !guest_signal_pending(fixture, SIGPIPE_),
            "未连接 TCP 在 cmsg 语义和 payload fault 前返回 EPIPE，"
            "MSG_NOSIGNAL 抑制 SIGPIPE");

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EPIPE) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ) &&
            guest_signal_pending(fixture, SIGPIPE_),
            "未连接 TCP 的发送前 EPIPE 排队 guest SIGPIPE");
    clear_guest_signals(fixture);

    uint16_t ignored_family = UINT16_MAX;
    qword_t name_pointer = put_user(
            memory, name_offset, &ignored_family, sizeof(ignored_family));
    message.name = name_pointer;
    message.namelen = sizeof(ignored_family);
    message.control = 0;
    message.controllen = 0;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    encoded_error(_EPIPE) &&
            saw_access(memory, name_pointer,
                    sizeof(ignored_family), GUEST_MEMORY_READ) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "TCP 只复制而不解释 msg_name，连接错误仍先于 payload fault");

    message.namelen = sizeof(struct sockaddr_storage) + 1;
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    encoded_error(_EPIPE) &&
            saw_access(memory, name_pointer,
                    sizeof(struct sockaddr_storage), GUEST_MEMORY_READ) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "sendmsg 截断过长名称后仍先按 TCP 连接状态返回 EPIPE");
    CHECK(f_close_task(&fixture->task, tcp) == 0,
            "未连接 TCP sendmsg socket 清理成功");

    int local = create_guest_socket(fixture,
            memory, fault, AF_LOCAL_, SOCK_STREAM_);
    CHECK(local >= 0, "未连接 Unix stream sendmsg socket 创建成功");
    message = (struct aarch64_linux_msghdr) {
        .iov = vector_pointer,
        .iovlen = 1,
    };
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    local, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_ENOTCONN) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "未连接 Unix stream 在 payload fault 前返回 ENOTCONN");

    uint16_t local_family = AF_LOCAL_;
    name_pointer = put_user(
            memory, name_offset, &local_family, sizeof(local_family));
    message.name = name_pointer;
    message.namelen = sizeof(local_family);
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    local, message_pointer, 0, 0, 0, 0) ==
                    encoded_error(_EOPNOTSUPP) &&
            !saw_access(memory, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "未连接 Unix stream 的显式名称在 payload fault 前返回 EOPNOTSUPP");
    CHECK(f_close_task(&fixture->task, local) == 0,
            "未连接 Unix stream sendmsg socket 清理成功");

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in listener_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    CHECK(listener >= 0 && bind(listener,
                    (const struct sockaddr *) &listener_address,
                    sizeof(listener_address)) == 0 &&
            listen(listener, 1) == 0,
            "TCP ancillary host listener 启动成功");
    socklen_t listener_length = sizeof(listener_address);
    CHECK(getsockname(listener,
                    (struct sockaddr *) &listener_address,
                    &listener_length) == 0,
            "TCP ancillary host listener 地址可查询");
    tcp = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_STREAM_);
    struct linux_sockaddr_in destination =
            linux_loopback(listener_address.sin_port);
    qword_t destination_pointer = put_user(
            memory, name_offset, &destination, sizeof(destination));
    CHECK(tcp >= 0 && invoke(fixture, memory, fault, SYS_CONNECT,
                    tcp, destination_pointer, sizeof(destination),
                    0, 0, 0) == 0,
            "TCP ancillary guest socket 连接成功");
    int accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0, "TCP ancillary host 连接接收成功");

    control_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(control_header) + sizeof(dword_t),
        .level = IPPROTO_IP,
        .type = IP_TTL_,
    };
    dword_t ttl = 64;
    memset(control, 0, sizeof(control));
    memcpy(control, &control_header, sizeof(control_header));
    memcpy(control + sizeof(control_header), &ttl, sizeof(ttl));
    put_user(memory, control_offset, control, sizeof(control));
    message = (struct aarch64_linux_msghdr) {
        .iov = vector_pointer,
        .iovlen = 1,
        .control = control_pointer,
        .controllen = sizeof(control),
    };
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    sizeof(payload),
            "TCP 忽略 IP-level ancillary 并发送 payload");
    byte_t received = 0;
    CHECK(wait_readable(accepted) &&
            recvfrom(accepted, &received, sizeof(received), MSG_DONTWAIT,
                    NULL, NULL) ==
                    sizeof(received) && received == payload,
            "TCP ancillary 回归的 payload 到达 host peer");

    control_header = (struct aarch64_linux_cmsghdr) {
        .length = sizeof(control_header),
        .level = LINUX_SOL_UDP,
        .type = INT32_MAX,
    };
    memset(control, 0, sizeof(control));
    memcpy(control, &control_header, sizeof(control_header));
    put_user(memory, control_offset, control, sizeof(control_header));
    message.controllen = sizeof(control_header);
    put_user(memory, message_offset, &message, sizeof(message));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDMSG,
                    tcp, message_pointer, MSG_NOSIGNAL_, 0, 0, 0) ==
                    sizeof(payload) && wait_readable(accepted) &&
            recvfrom(accepted, &received, sizeof(received), MSG_DONTWAIT,
                    NULL, NULL) ==
                    sizeof(received) && received == payload,
            "非 UDP socket 忽略 foreign SOL_UDP ancillary");

    CHECK(f_close_task(&fixture->task, tcp) == 0,
            "TCP ancillary guest socket 清理成功");
    close(accepted);
    close(listener);
    return 0;
}

static void *invoke_blocking_socket_call(void *opaque) {
    struct interrupted_socket_call *call = opaque;
    struct guest_linux_user_fault fault = {0};
    current = &call->fixture->task;
    task_thread_store(&call->fixture->task, pthread_self());
    atomic_store_explicit(&call->started, true, memory_order_release);
    call->result = invoke(call->fixture, call->memory, &fault,
            call->number,
            call->arguments[0], call->arguments[1],
            call->arguments[2], call->arguments[3],
            call->arguments[4], call->arguments[5]);
    call->restart = call->fixture->completion.restart;
    atomic_store_explicit(&call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool run_interrupted_socket_call(
        struct interrupted_socket_call *call, int host_socket) {
    atomic_init(&call->started, false);
    atomic_init(&call->finished, false);
    call->result = encoded_error(_EIO);
    call->restart = GUEST_LINUX_SYSCALL_RESTART_DEFAULT;

    pthread_t thread;
    if (pthread_create(&thread, NULL,
                invoke_blocking_socket_call, call) != 0)
        return false;
    bool started = wait_for_atomic_flag(&call->started, 1000);
    const struct timespec blocking_sample = {.tv_nsec = 50000000};
    if (started)
        nanosleep(&blocking_sample, NULL);
    bool blocked = started && !atomic_load_explicit(
            &call->finished, memory_order_acquire);
    if (blocked)
        queue_guest_signal(call->fixture, SIGUSR1_);

    bool finished = wait_for_atomic_flag(&call->finished, 4000);
    if (!finished) {
        (void) shutdown(host_socket, SHUT_RDWR);
        (void) pthread_kill(thread, SIGUSR1);
        finished = wait_for_atomic_flag(&call->finished, 4000);
    }
    if (!finished) {
        fprintf(stderr,
                "AArch64 socket 消息测试失败："
                "阻塞 I/O 线程救援后仍未退出\n");
        fflush(stderr);
        _Exit(1);
    }
    pthread_join(thread, NULL);
    task_thread_store(&call->fixture->task, pthread_self());

    bool pending = guest_signal_pending(call->fixture, SIGUSR1_);
    clear_guest_signals(call->fixture);
    bool valid = blocked && pending &&
            call->result == encoded_error(_EINTR) &&
            call->restart == GUEST_LINUX_SYSCALL_RESTART_NEVER;
    if (!valid) {
        fprintf(stderr,
                "阻塞 I/O 中断详情：blocked=%d pending=%d "
                "result=%lld restart=%u\n",
                blocked, pending, (long long) (sqword_t) call->result,
                call->restart);
    }
    return valid;
}

static bool set_guest_socket_timeout(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault,
        fd_t socket_number, sdword_t option, size_t offset) {
    const struct linux_timeval64 timeout = {
        .seconds = 2,
    };
    qword_t timeout_pointer = put_user(
            memory, offset, &timeout, sizeof(timeout));
    fixture->completion.restart = GUEST_LINUX_SYSCALL_RESTART_NEVER;
    reset_access(memory);
    if (invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                socket_number, SOL_SOCKET_, option,
                timeout_pointer, sizeof(timeout), 0) != 0 ||
            fixture->completion.restart !=
                    GUEST_LINUX_SYSCALL_RESTART_DEFAULT)
        return false;

    struct fd *socket_fd = f_get_task(&fixture->task, socket_number);
    struct timeval host_timeout = {0};
    socklen_t length = sizeof(host_timeout);
    return socket_fd != NULL && getsockopt(socket_fd->real_fd,
            SOL_SOCKET,
            option == SO_RCVTIMEO_NEW_ ? SO_RCVTIMEO : SO_SNDTIMEO,
            &host_timeout, &length) == 0 &&
            (host_timeout.tv_sec != 0 || host_timeout.tv_usec != 0);
}

static int install_host_stream_pair(
        struct fixture *fixture, int *peer) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
        return _EIO;
    struct fd *sender = fd_create(&socket_fdops);
    if (sender == NULL) {
        close(sockets[0]);
        close(sockets[1]);
        return _ENOMEM;
    }
    sender->real_fd = sockets[0];
    // 这里只固定服务所需的 stream 语义；
    // host socketpair 提供确定的背压。
    sender->socket.domain = AF_INET_;
    sender->socket.type = SOCK_STREAM_;
    int number = f_install_task(&fixture->task, sender, 0);
    if (number < 0) {
        close(sockets[1]);
        return number;
    }
    *peer = sockets[1];
    return number;
}

static bool fill_host_send_buffer(int socket_fd) {
    int original_flags = fcntl(socket_fd, F_GETFL);
    if (original_flags < 0 || fcntl(socket_fd, F_SETFL,
                original_flags | O_NONBLOCK) < 0)
        return false;

    byte_t payload[4096];
    memset(payload, 0x6d, sizeof(payload));
    bool full = false;
    size_t total = 0;
    while (total < 64 * 1024 * 1024) {
        ssize_t sent = send(socket_fd, payload, sizeof(payload), 0);
        if (sent > 0) {
            total += (size_t) sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        full = sent < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    bool restored = fcntl(
            socket_fd, F_SETFL, original_flags) == 0;
    return full && restored;
}

static int test_timeout_interrupt_restart_hint(
        struct fixture *fixture, struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t recv_message_offset = 0x12000;
    const size_t recv_vector_offset = 0x12100;
    const size_t recv_payload_offset = 0x12200;
    const size_t recv_timeout_offset = 0x12300;

    int receiver = create_guest_socket(fixture,
            memory, fault, AF_INET_, SOCK_DGRAM_);
    struct sockaddr_in receiver_address = {0};
    CHECK(receiver >= 0 && bind_guest_udp(fixture, memory, fault,
                    receiver, &receiver_address),
            "超时 recvmsg guest UDP socket 绑定成功");
    CHECK(set_guest_socket_timeout(fixture, memory, fault,
                    receiver, SO_RCVTIMEO_NEW_, recv_timeout_offset),
            "guest SO_RCVTIMEO 已实际设置到 host socket");

    struct aarch64_linux_iovec_wire recv_vector = {
        .base = USER_BASE + recv_payload_offset,
        .length = 1,
    };
    qword_t recv_vector_pointer = put_user(memory,
            recv_vector_offset, &recv_vector, sizeof(recv_vector));
    struct aarch64_linux_msghdr recv_message = {
        .iov = recv_vector_pointer,
        .iovlen = 1,
    };
    qword_t recv_message_pointer = put_user(memory,
            recv_message_offset, &recv_message, sizeof(recv_message));
    reset_access(memory);
    struct interrupted_socket_call recv_call = {
        .fixture = fixture,
        .memory = memory,
        .number = SYS_RECVMSG,
        .arguments = {receiver, recv_message_pointer, 0, 0, 0, 0},
    };
    struct fd *receiver_fd = f_get_task(&fixture->task, receiver);
    CHECK(receiver_fd != NULL && run_interrupted_socket_call(
                    &recv_call, receiver_fd->real_fd),
            "SO_RCVTIMEO recvmsg 被 guest 信号中断并禁止 SA_RESTART 重启");
    CHECK(f_close_task(&fixture->task, receiver) == 0,
            "超时 recvmsg guest UDP socket 清理成功");

    const size_t send_message_offset = 0x13000;
    const size_t send_vector_offset = 0x13100;
    const size_t send_payload_offset = 0x13200;
    const size_t send_timeout_offset = 0x14400;
    int peer = -1;
    int sender = install_host_stream_pair(fixture, &peer);
    CHECK(sender >= 0 && peer >= 0,
            "超时 sendmsg stream socketpair 安装成功");

    struct fd *sender_fd = f_get_task(&fixture->task, sender);
    int buffer_size = 8192;
    CHECK(sender_fd != NULL &&
            setsockopt(sender_fd->real_fd, SOL_SOCKET, SO_SNDBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            setsockopt(peer, SOL_SOCKET, SO_RCVBUF,
                    &buffer_size, sizeof(buffer_size)) == 0 &&
            fill_host_send_buffer(sender_fd->real_fd),
            "超时 sendmsg host stream 发送缓冲已填满");
    CHECK(set_guest_socket_timeout(fixture, memory, fault,
                    sender, SO_SNDTIMEO_NEW_, send_timeout_offset),
            "guest SO_SNDTIMEO 已实际设置到 host socket");

    byte_t send_payload[4096];
    memset(send_payload, 0x96, sizeof(send_payload));
    qword_t send_payload_pointer = put_user(memory,
            send_payload_offset, send_payload, sizeof(send_payload));
    struct aarch64_linux_iovec_wire send_vector = {
        .base = send_payload_pointer,
        .length = sizeof(send_payload),
    };
    qword_t send_vector_pointer = put_user(memory,
            send_vector_offset, &send_vector, sizeof(send_vector));
    struct aarch64_linux_msghdr send_message = {
        .iov = send_vector_pointer,
        .iovlen = 1,
    };
    qword_t send_message_pointer = put_user(memory,
            send_message_offset, &send_message, sizeof(send_message));
    reset_access(memory);
    struct interrupted_socket_call send_call = {
        .fixture = fixture,
        .memory = memory,
        .number = SYS_SENDMSG,
        .arguments = {
            sender, send_message_pointer, MSG_NOSIGNAL_, 0, 0, 0,
        },
    };
    CHECK(run_interrupted_socket_call(
                    &send_call, sender_fd->real_fd),
            "SO_SNDTIMEO sendmsg 被 guest 信号中断并禁止 SA_RESTART 重启");

    CHECK(f_close_task(&fixture->task, sender) == 0,
            "超时 sendmsg guest stream socket 清理成功");
    close(peer);
    return 0;
}

int main(void) {
    struct sigaction host_action = {0};
    struct sigaction old_host_action;
    host_action.sa_handler = host_wakeup_handler;
    sigemptyset(&host_action.sa_mask);
    CHECK(sigaction(SIGUSR1, &host_action, &old_host_action) == 0,
            "安装 host SIGUSR1 唤醒处理器");

    struct fixture fixture;
    struct user_memory memory = {
        .bytes = calloc(1, USER_MEMORY_SIZE),
    };
    struct guest_linux_user_fault fault = {0};
    CHECK(memory.bytes != NULL, "用户内存分配成功");
    reset_access(&memory);
    if (!fixture_init(&fixture)) {
        fprintf(stderr,
                "AArch64 socket 消息测试失败：socket 消息测试夹具初始化成功（第 %d 行）\n",
                __LINE__);
        free(memory.bytes);
        sigaction(SIGUSR1, &old_host_action, NULL);
        return 1;
    }

    CHECK(test_udp_sendmsg(&fixture, &memory, &fault) == 0,
            "UDP sendmsg dispatcher 语义正确");
    CHECK(test_udp_ancillary_levels(&fixture, &memory, &fault) == 0,
            "UDP ancillary 的协议路径与错误语义正确");
    CHECK(test_udp_recvmsg(&fixture, &memory, &fault) == 0,
            "UDP recvmsg dispatcher 语义正确");
    CHECK(test_tcp_connect_error_state(
                    &fixture, &memory, &fault) == 0,
            "TCP connect 与 sendmsg 异步错误状态正确");
    CHECK(test_stream_sendmsg_error_order(
                    &fixture, &memory, &fault) == 0,
            "stream sendmsg 的连接、cmsg 与 payload 错误顺序正确");
    CHECK(test_timeout_interrupt_restart_hint(
                    &fixture, &memory, &fault) == 0,
            "socket 超时 I/O 的 restart hint 端到端语义正确");
    CHECK(test_scm_rights(&fixture, &memory, &fault) == 0,
            "Unix SCM_RIGHTS dispatcher 语义正确");

    fixture_destroy(&fixture);
    free(memory.bytes);
    CHECK(sigaction(SIGUSR1, &old_host_action, NULL) == 0,
            "恢复 host SIGUSR1 处理器");
    puts("AArch64 socket 消息测试通过");
    return 0;
}

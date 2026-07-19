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
#include <sys/time.h>
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

#define USER_BASE UINT64_C(0x00007abc56780000)
#define USER_MEMORY_SIZE UINT32_C(0x110000)
#define USER_ACCESS_LOG_SIZE 32

#define SYS_SOCKET 198
#define SYS_BIND 200
#define SYS_CONNECT 203
#define SYS_SENDTO 206
#define SYS_RECVFROM 207
#define SYS_SETSOCKOPT 208

#define UNKNOWN_MESSAGE_FLAG UINT32_C(0x01000000)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 socket 系统调用测试失败：%s（第 %d 行）\n", \
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

struct linux_timeval64 {
    int64_t seconds;
    int64_t microseconds;
};

_Static_assert(sizeof(struct linux_sockaddr_in) == 16,
        "Linux IPv4 sockaddr wire 大小必须固定为 16 字节");
_Static_assert(sizeof(struct linux_timeval64) == 16,
        "AArch64 Linux timeval wire 大小必须固定为 16 字节");
_Static_assert(sizeof(socklen_t) == 4,
        "Apple 与本机测试 host 的 socklen_t 必须为 32 位");
_Static_assert(sizeof(struct sockaddr_storage) == 128,
        "sockaddr_storage 必须覆盖 Linux 最大地址");

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

    struct task *hook_task;
    qword_t hook_address;
    dword_t hook_access;
    fd_t hook_fd;
    int hook_raw_fd;
    int replacement_closes;
    bool hook_fired;
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

static void reset_access(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->fail_write_at = UINT64_MAX;
    memory->read_calls = 0;
    memory->write_calls = 0;
    memory->access_count = 0;
    memory->hook_task = NULL;
    memory->hook_address = 0;
    memory->hook_access = 0;
    memory->hook_fd = -1;
    memory->hook_raw_fd = -1;
    memory->hook_fired = false;
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

static void run_lifecycle_hook(struct user_memory *memory,
        qword_t address, dword_t size, enum guest_memory_access access) {
    if (memory->hook_task == NULL || memory->hook_fired ||
            memory->hook_access != (dword_t) access ||
            !range_contains(address, size, memory->hook_address))
        return;
    memory->hook_fired = true;
    int close_result = f_close_task(memory->hook_task, memory->hook_fd);
    memory->raw_open_during_hook =
            fcntl(memory->hook_raw_fd, F_GETFD) >= 0;
    struct fd *replacement = fd_create(&replacement_ops);
    if (replacement == NULL)
        return;
    replacement->data = &memory->replacement_closes;
    fd_t installed = f_install_task(memory->hook_task, replacement, 0);
    memory->hook_succeeded = close_result == 0 &&
            installed == memory->hook_fd;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    record_access(memory, address, size, GUEST_MEMORY_READ);
    memory->read_calls++;
    run_lifecycle_hook(memory, address, size, GUEST_MEMORY_READ);
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
    run_lifecycle_hook(memory, address, size, GUEST_MEMORY_WRITE);
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

static int create_guest_udp(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault) {
    qword_t result = invoke(fixture, memory, fault, SYS_SOCKET,
            AF_INET_, SOCK_DGRAM_, 0, 0, 0, 0);
    return (int) (sqword_t) result;
}

static struct linux_sockaddr_in linux_loopback(uint16_t port) {
    return (struct linux_sockaddr_in) {
        .family = AF_INET_,
        .port = port,
        .address = htonl(INADDR_LOOPBACK),
    };
}

static int host_udp_listener(struct sockaddr_in *address) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;
    *address = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = 0,
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

static bool wait_readable(int fd) {
    struct pollfd event = {.fd = fd, .events = POLLIN};
    int result;
    do {
        result = poll(&event, 1, 1000);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (event.revents & POLLIN) != 0;
}

static bool access_is(const struct user_memory *memory, unsigned index,
        qword_t address, dword_t size, enum guest_memory_access access) {
    return index < memory->access_count &&
            memory->accesses[index].address == address &&
            memory->accesses[index].size == size &&
            memory->accesses[index].access == (dword_t) access;
}

static int test_bind(struct fixture *fixture, struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t address_offset = 0x100;
    struct linux_sockaddr_in address = linux_loopback(0);
    qword_t address_pointer = put_user(memory,
            address_offset, &address, sizeof(address));

    struct fd *ordinary = fd_create(&replacement_ops);
    CHECK(ordinary != NULL, "bind 顺序测试普通 fd 创建成功");
    ordinary->data = &memory->replacement_closes;
    CHECK(f_install_task(&fixture->task, ordinary, 0) == 0,
            "bind 顺序测试普通 fd 安装成功");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    99, address_pointer, sizeof(address), 0, 0, 0) ==
                    encoded_error(_EBADF) && memory->read_calls == 0,
            "bind 在地址读取前返回 EBADF");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    0, address_pointer, sizeof(address), 0, 0, 0) ==
                    encoded_error(_ENOTSOCK) && memory->read_calls == 0,
            "bind 在地址读取前返回 ENOTSOCK");

    int guest_socket = create_guest_udp(fixture, memory, fault);
    CHECK(guest_socket == 1, "bind 测试 UDP socket 创建成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (qword_t) (dword_t) guest_socket,
                    address_pointer, UINT64_C(0xffffffff), 0, 0, 0) ==
                    encoded_error(_EINVAL) && memory->read_calls == 0,
            "bind 将 addrlen 按低 32 位有符号数解释");

    reset_access(memory);
    qword_t decorated_fd = UINT64_C(0xabcdef0000000000) |
            (dword_t) guest_socket;
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    decorated_fd, address_pointer,
                    sizeof(address), 0, 0, 0) == 0 &&
            access_is(memory, 0, address_pointer,
                    sizeof(address), GUEST_MEMORY_READ),
            "bind 忽略 fd 高 32 位并读取 64 位 guest 地址");
    struct fd *bound = f_get_task(&fixture->task, guest_socket);
    struct sockaddr_in bound_address = {0};
    socklen_t bound_length = sizeof(bound_address);
    CHECK(bound != NULL && getsockname(bound->real_fd,
                    (struct sockaddr *) &bound_address,
                    &bound_length) == 0 && bound_address.sin_port != 0,
            "bind 实际绑定 host 回环地址");

    int lifecycle_socket = create_guest_udp(fixture, memory, fault);
    CHECK(lifecycle_socket == 2, "bind 生命周期 socket 创建成功");
    struct fd *lifecycle_fd = f_get_task(
            &fixture->task, lifecycle_socket);
    CHECK(lifecycle_fd != NULL, "bind 生命周期取得原 socket");
    int original_raw_fd = lifecycle_fd->real_fd;
    int observer_fd = dup(original_raw_fd);
    CHECK(observer_fd >= 0, "bind 生命周期复制 host 观察 fd");

    reset_access(memory);
    memory->hook_task = &fixture->task;
    memory->hook_address = address_pointer;
    memory->hook_access = GUEST_MEMORY_READ;
    memory->hook_fd = lifecycle_socket;
    memory->hook_raw_fd = original_raw_fd;
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    lifecycle_socket, address_pointer,
                    sizeof(struct sockaddr_storage), 0, 0, 0) == 0 &&
            memory->hook_fired && memory->hook_succeeded &&
            memory->raw_open_during_hook,
            "bind 归一 overlong IPv4 地址且 retained 引用跨越回调");
    bound_address = (struct sockaddr_in) {0};
    bound_length = sizeof(bound_address);
    CHECK(getsockname(observer_fd,
                    (struct sockaddr *) &bound_address,
                    &bound_length) == 0 && bound_address.sin_port != 0,
            "bind 在回调复用后仍操作原 socket");
    errno = 0;
    CHECK(fcntl(original_raw_fd, F_GETFD) == -1 && errno == EBADF,
            "bind 返回后原 socket 最后一个内部引用已释放");
    CHECK(f_close_task(&fixture->task, lifecycle_socket) == 0 &&
            memory->replacement_closes == 1,
            "bind 生命周期替换 fd 独立清理");
    close(observer_fd);
    return 0;
}

static int test_unix_bind(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    static const char pathname[] = "/aarch64-bind-dispatch";
    byte_t pathname_wire[sizeof(uint16_t) + sizeof(pathname)] = {0};
    uint16_t unix_family = AF_LOCAL_;
    memcpy(pathname_wire, &unix_family, sizeof(unix_family));
    memcpy(pathname_wire + sizeof(unix_family),
            pathname, sizeof(pathname));
    qword_t pathname_pointer = put_user(memory, 0x6000,
            pathname_wire, sizeof(pathname_wire));

    int pathname_socket = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_SOCKET, AF_LOCAL_, SOCK_STREAM_, 0, 0, 0, 0);
    CHECK(pathname_socket >= 0,
            "Unix pathname bind socket 创建成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_BIND,
                    (dword_t) pathname_socket, pathname_pointer,
                    sizeof(pathname_wire), 0, 0, 0) == 0 &&
            access_is(memory, 0, pathname_pointer,
                    sizeof(pathname_wire), GUEST_MEMORY_READ),
            "AArch64 bind 经 dispatcher 创建 Unix pathname 节点");
    struct fd *pathname_fd = f_get_task(
            &fixture->task, pathname_socket);
    CHECK(pathname_fd != NULL &&
                    pathname_fd->socket.unix_name_len ==
                            sizeof(pathname) &&
                    memcmp(pathname_fd->socket.unix_name,
                            pathname, sizeof(pathname)) == 0,
            "Unix pathname bind 保存 Linux 返回名称");
    CHECK(f_close_task(&fixture->task, pathname_socket) == 0,
            "Unix pathname bind socket 关闭成功");

    int rebound_socket = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_SOCKET, AF_LOCAL_, SOCK_STREAM_, 0, 0, 0, 0);
    CHECK(rebound_socket >= 0 &&
                    invoke(fixture, memory, fault, SYS_BIND,
                            (dword_t) rebound_socket, pathname_pointer,
                            sizeof(pathname_wire), 0, 0, 0) ==
                            encoded_error(_EADDRINUSE),
            "pathname socket 关闭后 Linux 名称节点继续占用地址");
    CHECK(file_unlinkat_task(&fixture->task, AT_FDCWD_,
                    pathname, false) == 0 &&
                    invoke(fixture, memory, fault, SYS_BIND,
                            (dword_t) rebound_socket, pathname_pointer,
                            sizeof(pathname_wire), 0, 0, 0) == 0,
            "显式 unlink pathname 节点后同一 socket 可重绑");
    CHECK(f_close_task(&fixture->task, rebound_socket) == 0 &&
                    file_unlinkat_task(&fixture->task, AT_FDCWD_,
                            pathname, false) == 0,
            "Unix pathname 重绑 socket 与名称节点清理成功");

    static const byte_t abstract_name[] = {
        0, 'a', 'a', 'r', 'c', 'h', '6', '4', '-', 'b', 'i', 'n', 'd',
    };
    byte_t abstract_wire[sizeof(uint16_t) +
            sizeof(abstract_name)] = {0};
    memcpy(abstract_wire, &unix_family, sizeof(unix_family));
    memcpy(abstract_wire + sizeof(unix_family),
            abstract_name, sizeof(abstract_name));
    qword_t abstract_pointer = put_user(memory, 0x6200,
            abstract_wire, sizeof(abstract_wire));
    int abstract_socket = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_SOCKET, AF_LOCAL_, SOCK_DGRAM_, 0, 0, 0, 0);
    CHECK(abstract_socket >= 0 &&
                    invoke(fixture, memory, fault, SYS_BIND,
                            (dword_t) abstract_socket, abstract_pointer,
                            sizeof(abstract_wire), 0, 0, 0) == 0,
            "AArch64 bind 经 dispatcher 创建二进制抽象名称");
    struct fd *abstract_fd = f_get_task(
            &fixture->task, abstract_socket);
    CHECK(abstract_fd != NULL &&
                    abstract_fd->socket.unix_name_len ==
                            sizeof(abstract_name) &&
                    memcmp(abstract_fd->socket.unix_name,
                            abstract_name, sizeof(abstract_name)) == 0,
            "Unix abstract bind 保留前导 NUL 与精确长度");
    CHECK(f_close_task(&fixture->task, abstract_socket) == 0,
            "Unix abstract bind socket 清理成功");

    qword_t autobind_pointer = put_user(memory, 0x6400,
            &unix_family, sizeof(unix_family));
    int autobind_socket = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_SOCKET, AF_LOCAL_, SOCK_DGRAM_, 0, 0, 0, 0);
    CHECK(autobind_socket >= 0 &&
                    invoke(fixture, memory, fault, SYS_BIND,
                            (dword_t) autobind_socket, autobind_pointer,
                            sizeof(unix_family), 0, 0, 0) == 0,
            "AArch64 bind 以两字节 sockaddr 触发 Unix autobind");
    struct fd *autobind_fd = f_get_task(
            &fixture->task, autobind_socket);
    CHECK(autobind_fd != NULL &&
                    autobind_fd->socket.unix_name_len == 6 &&
                    autobind_fd->socket.unix_name[0] == '\0',
            "Unix autobind 生成前导 NUL 与五位名称");
    CHECK(f_close_task(&fixture->task, autobind_socket) == 0,
            "Unix autobind socket 清理成功");
    return 0;
}

static int test_sendto(struct fixture *fixture, struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t address_offset = 0x200;
    const size_t payload_offset = 0x2000;
    struct sockaddr_in host_address;
    int receiver = host_udp_listener(&host_address);
    CHECK(receiver >= 0, "sendto host UDP 接收端启动成功");
    struct linux_sockaddr_in destination =
            linux_loopback(host_address.sin_port);
    qword_t destination_pointer = put_user(memory,
            address_offset, &destination, sizeof(destination));
    static const byte_t payload[] = {0x00, 0x41, 0xff, 0x42};
    qword_t payload_pointer = put_user(memory,
            payload_offset, payload, sizeof(payload));

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    99, payload_pointer, sizeof(payload), 0, 0, 0) ==
                    encoded_error(_EBADF) && memory->read_calls == 0,
            "sendto 对范围合法的未映射 payload 先返回 EBADF");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    99, UINT64_MAX - 3, 8, 0, 0, 0) ==
                    encoded_error(_EFAULT) && memory->read_calls == 0,
            "sendto 对越界 span 先于 fd lookup 返回 EFAULT");

    int guest_socket = create_guest_udp(fixture, memory, fault);
    CHECK(guest_socket >= 0, "sendto guest UDP socket 创建成功");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    guest_socket, payload_pointer, sizeof(payload),
                    0, destination_pointer, 129) ==
                    encoded_error(_EINVAL) && memory->read_calls == 0,
            "sendto 在 payload 前拒绝过长 sockaddr");

    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    guest_socket, payload_pointer, 65536,
                    0, destination_pointer, sizeof(destination)) ==
                    encoded_error(_EMSGSIZE) &&
            memory->read_calls == 1 &&
            access_is(memory, 0, destination_pointer,
                    sizeof(destination), GUEST_MEMORY_READ),
            "sendto 在地址校验后、payload 读取前返回 UDP EMSGSIZE");

    struct linux_sockaddr_in wrong_family = destination;
    wrong_family.family = AF_LOCAL_;
    qword_t wrong_family_pointer = put_user(memory,
            0x400, &wrong_family, sizeof(wrong_family));
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    guest_socket, payload_pointer, 65536, 0,
                    wrong_family_pointer, sizeof(wrong_family)) ==
                    encoded_error(_EMSGSIZE) && memory->read_calls == 1,
            "sendto 在 family 解析前优先返回 UDP EMSGSIZE");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    guest_socket, payload_pointer, sizeof(payload), 0,
                    wrong_family_pointer, sizeof(wrong_family)) ==
                    encoded_error(_EAFNOSUPPORT) &&
            memory->read_calls == 1,
            "sendto 按 socket domain 拒绝异族地址且不读取 payload");
    reset_access(memory);
    memory->fail_read_at = payload_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    guest_socket, payload_pointer, sizeof(payload),
                    MSG_OOB_, destination_pointer, sizeof(destination)) ==
                    encoded_error(_EOPNOTSUPP) && memory->read_calls == 1,
            "sendto 在 payload 读取前拒绝 UDP MSG_OOB");

    struct fd *socket_fd = f_get_task(&fixture->task, guest_socket);
    CHECK(socket_fd != NULL, "sendto 生命周期取得原 socket");
    int original_raw_fd = socket_fd->real_fd;
    reset_access(memory);
    memory->hook_task = &fixture->task;
    memory->hook_address = payload_pointer;
    memory->hook_access = GUEST_MEMORY_READ;
    memory->hook_fd = guest_socket;
    memory->hook_raw_fd = original_raw_fd;
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    UINT64_C(0x1234000000000000) | (dword_t) guest_socket,
                    payload_pointer, sizeof(payload), 0,
                    destination_pointer,
                    sizeof(struct sockaddr_storage)) ==
                    sizeof(payload) && memory->hook_fired &&
            memory->hook_succeeded && memory->raw_open_during_hook,
            "sendto 归一 overlong IPv4 地址且生命周期引用跨越 payload 回调");
    byte_t received[16] = {0};
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(payload) &&
            memcmp(received, payload, sizeof(payload)) == 0,
            "sendto 二进制 datagram 由原 socket 完整发送");
    errno = 0;
    CHECK(fcntl(original_raw_fd, F_GETFD) == -1 && errno == EBADF &&
            f_close_task(&fixture->task, guest_socket) == 0,
            "sendto 返回后原 socket 与替换 fd 各自释放");

    int connected_socket = create_guest_udp(fixture, memory, fault);
    CHECK(connected_socket >= 0, "sendto 连接式 UDP socket 创建成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_CONNECT,
                    connected_socket, destination_pointer,
                    sizeof(destination), 0, 0, 0) == 0,
            "sendto 连接式 UDP 准备成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    connected_socket, payload_pointer, sizeof(payload),
                    0, 0, UINT64_MAX) == sizeof(payload) &&
            memory->read_calls == 1 &&
            access_is(memory, 0, payload_pointer,
                    sizeof(payload), GUEST_MEMORY_READ),
            "sendto 在目标为 NULL 时完全忽略 addrlen");
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(payload),
            "连接式 sendto 的 NULL 目标 datagram 到达");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SENDTO,
                    connected_socket, payload_pointer, sizeof(payload),
                    UNKNOWN_MESSAGE_FLAG, 0, 0) == sizeof(payload),
            "sendto 保持 UDP 忽略未知消息 flag 的 Linux 语义");
    memset(received, 0, sizeof(received));
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) ==
                    (ssize_t) sizeof(payload) &&
            memcmp(received, payload, sizeof(payload)) == 0,
            "带未知消息 flag 的 UDP datagram 完整到达");
    CHECK(f_close_task(&fixture->task, connected_socket) == 0,
            "连接式 sendto socket 清理成功");

    int zero_socket = create_guest_udp(fixture, memory, fault);
    CHECK(zero_socket >= 0, "零长度 datagram socket 创建成功");
    reset_access(memory);
    qword_t zero_result = invoke(fixture, memory, fault, SYS_SENDTO,
            zero_socket, payload_pointer, 0, 0,
            destination_pointer, sizeof(destination));
    CHECK(zero_result == 0 &&
            memory->read_calls == 1,
            "sendto 零长度 datagram 不读取 payload");
    CHECK(wait_readable(receiver) &&
            recv(receiver, received, sizeof(received), 0) == 0,
            "sendto 确实发送零长度 datagram");
    CHECK(f_close_task(&fixture->task, zero_socket) == 0,
            "零长度 datagram socket 清理成功");
    close(receiver);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in listener_address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t listener_length = sizeof(listener_address);
    CHECK(listener >= 0 && bind(listener,
                    (const struct sockaddr *) &listener_address,
                    sizeof(listener_address)) == 0 &&
            listen(listener, 1) == 0 &&
            getsockname(listener, (struct sockaddr *) &listener_address,
                    &listener_length) == 0,
            "sendto 大 stream host 监听端启动成功");
    destination = linux_loopback(listener_address.sin_port);
    put_user(memory, address_offset, &destination, sizeof(destination));
    int stream_socket = (int) (sqword_t) invoke(fixture, memory, fault,
            SYS_SOCKET, AF_INET_, SOCK_STREAM_, 0, 0, 0, 0);
    CHECK(stream_socket >= 0 && invoke(fixture, memory, fault, SYS_CONNECT,
                    stream_socket, destination_pointer,
                    sizeof(destination), 0, 0, 0) == 0,
            "sendto 大 stream guest 连接成功");
    int stream_peer = accept(listener, NULL, NULL);
    CHECK(stream_peer >= 0, "sendto 大 stream host 连接接收成功");
    reset_access(memory);
    sqword_t stream_result = (sqword_t) invoke(fixture, memory, fault,
            SYS_SENDTO, stream_socket, payload_pointer,
            SOCKET_IO_TRANSACTION_LIMIT + 1, MSG_DONTWAIT_, 0, 0);
    CHECK(stream_result > 0 &&
            stream_result <= SOCKET_STREAM_NONBLOCK_TRANSACTION_LIMIT &&
            memory->read_calls == 1 &&
            memory->accesses[0].size ==
                    SOCKET_STREAM_NONBLOCK_TRANSACTION_LIMIT,
            "sendto 大 stream 以有界非阻塞短写替代等待");
    CHECK(f_close_task(&fixture->task, stream_socket) == 0,
            "sendto 大 stream guest socket 清理成功");
    close(stream_peer);
    close(listener);
    return 0;
}

static bool bind_guest_udp(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        fd_t guest_socket, struct sockaddr_in *host_address) {
    struct linux_sockaddr_in wire = linux_loopback(0);
    qword_t wire_pointer = put_user(memory, 0x300, &wire, sizeof(wire));
    reset_access(memory);
    if (invoke(fixture, memory, fault, SYS_BIND,
            guest_socket, wire_pointer, sizeof(wire), 0, 0, 0) != 0)
        return false;
    struct fd *socket_fd = f_get_task(&fixture->task, guest_socket);
    if (socket_fd == NULL)
        return false;
    socklen_t length = sizeof(*host_address);
    return getsockname(socket_fd->real_fd,
            (struct sockaddr *) host_address, &length) == 0 &&
            host_address->sin_port != 0;
}

static bool send_host_packet(int sender,
        const struct sockaddr_in *destination,
        const void *payload, size_t size) {
    return sendto(sender, payload, size, 0,
            (const struct sockaddr *) destination,
            sizeof(*destination)) == (ssize_t) size;
}

static int test_recvfrom(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t buffer_offset = 0x5000;
    const size_t source_offset = 0x6000;
    const size_t length_offset = 0x6100;
    const qword_t buffer_pointer = USER_BASE + buffer_offset;
    const qword_t source_pointer = USER_BASE + source_offset;
    const qword_t length_pointer = USER_BASE + length_offset;

    int guest_socket = create_guest_udp(fixture, memory, fault);
    struct sockaddr_in guest_address = {0};
    CHECK(guest_socket >= 0 && bind_guest_udp(fixture,
                    memory, fault, guest_socket, &guest_address),
            "recvfrom guest UDP 接收端绑定成功");
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(sender >= 0, "recvfrom host UDP 发送端创建成功");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 16, MSG_DONTWAIT_,
                    source_pointer, UINT64_MAX) ==
                    encoded_error(_EAGAIN) && memory->access_count == 0,
            "recvfrom 空队列先返回 EAGAIN 且不读取 addrlen");

    static const byte_t short_packet[] = {0x10, 0x00, 0xff};
    memset(memory->bytes + buffer_offset, 0xcc, 16);
    CHECK(send_host_packet(sender, &guest_address,
                    short_packet, sizeof(short_packet)),
            "recvfrom 短包发送成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 8, 0,
                    0, UINT64_MAX) == sizeof(short_packet) &&
            memory->access_count == 1 &&
            access_is(memory, 0, buffer_pointer,
                    sizeof(short_packet), GUEST_MEMORY_WRITE) &&
            memcmp(memory->bytes + buffer_offset,
                    short_packet, sizeof(short_packet)) == 0 &&
            memory->bytes[buffer_offset + sizeof(short_packet)] == 0xcc,
            "recvfrom 只写实际 payload 且 NULL 地址忽略 addrlen");

    static const byte_t unknown_flag_packet[] = "flag";
    CHECK(send_host_packet(sender, &guest_address,
                    unknown_flag_packet, sizeof(unknown_flag_packet) - 1),
            "recvfrom 未知消息 flag 数据发送成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer,
                    sizeof(unknown_flag_packet) - 1,
                    UNKNOWN_MESSAGE_FLAG, 0, 0) ==
                    sizeof(unknown_flag_packet) - 1 &&
            memcmp(memory->bytes + buffer_offset,
                    unknown_flag_packet,
                    sizeof(unknown_flag_packet) - 1) == 0,
            "recvfrom 保持 UDP 忽略未知消息 flag 的 Linux 语义");

    static const byte_t long_packet[] = "abcdefgh";
    CHECK(send_host_packet(sender, &guest_address,
                    long_packet, sizeof(long_packet) - 1),
            "recvfrom PEEK/TRUNC 数据发送成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 3, MSG_PEEK_,
                    0, 0) == 3 &&
            memcmp(memory->bytes + buffer_offset, long_packet, 3) == 0,
            "recvfrom MSG_PEEK 返回截断 payload 且不消费包");
    reset_access(memory);
    qword_t trunc_result = invoke(fixture, memory, fault, SYS_RECVFROM,
            guest_socket, buffer_pointer, 3, MSG_TRUNC_, 0, 0);
    CHECK(trunc_result == sizeof(long_packet) - 1 &&
            memory->access_count == 1 &&
            access_is(memory, 0, buffer_pointer, 3,
                    GUEST_MEMORY_WRITE),
            "recvfrom MSG_TRUNC 返回完整 datagram 长度但只写容量");

    static const byte_t ordered_packet[] = "neg";
    sdword_t capacity = -1;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    CHECK(send_host_packet(sender, &guest_address,
                    ordered_packet, sizeof(ordered_packet) - 1),
            "recvfrom 负 addrlen 数据发送成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer,
                    sizeof(ordered_packet) - 1, 0,
                    source_pointer, length_pointer) ==
                    encoded_error(_EINVAL) &&
            access_is(memory, 0, buffer_pointer,
                    sizeof(ordered_packet) - 1, GUEST_MEMORY_WRITE) &&
            access_is(memory, 1, length_pointer,
                    sizeof(capacity), GUEST_MEMORY_READ) &&
            memory->access_count == 2,
            "recvfrom 消费并写 payload 后才拒绝负 addrlen");

    capacity = 0;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    CHECK(send_host_packet(sender, &guest_address, "zero", 4),
            "recvfrom 零地址容量数据发送成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 4, 0,
                    source_pointer, length_pointer) == 4 &&
            memory->access_count == 3 &&
            access_is(memory, 2, length_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            *(dword_t *) (memory->bytes + length_offset) == 16,
            "recvfrom 零容量只写回真实地址长度");

    capacity = 16;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    CHECK(send_host_packet(sender, &guest_address, "addr", 4),
            "recvfrom 地址 fault 数据发送成功");
    reset_access(memory);
    memory->fail_write_at = source_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 4, 0,
                    source_pointer, length_pointer) ==
                    encoded_error(_EFAULT) &&
            *(dword_t *) (memory->bytes + length_offset) == 16 &&
            memory->access_count == 4 &&
            access_is(memory, 2, length_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 3, source_pointer, 16,
                    GUEST_MEMORY_WRITE),
            "recvfrom 先写真实长度再报告地址写回 fault");

    capacity = 16;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    CHECK(send_host_packet(sender, &guest_address, "data", 4),
            "recvfrom payload fault 数据发送成功");
    reset_access(memory);
    memory->fail_write_at = buffer_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 4, 0,
                    source_pointer, length_pointer) ==
                    encoded_error(_EFAULT) && memory->access_count == 1,
            "recvfrom payload fault 后不再访问地址参数");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    guest_socket, buffer_pointer, 4, MSG_DONTWAIT_,
                    0, 0) == encoded_error(_EAGAIN),
            "recvfrom payload fault 后 datagram 已消费");

    int lifecycle_socket = create_guest_udp(fixture, memory, fault);
    struct sockaddr_in lifecycle_address = {0};
    CHECK(lifecycle_socket >= 0 && bind_guest_udp(fixture, memory, fault,
                    lifecycle_socket, &lifecycle_address) &&
            send_host_packet(sender, &lifecycle_address, "life", 4),
            "recvfrom 生命周期 datagram 准备成功");
    struct fd *lifecycle_fd = f_get_task(
            &fixture->task, lifecycle_socket);
    CHECK(lifecycle_fd != NULL, "recvfrom 生命周期取得原 socket");
    int original_raw_fd = lifecycle_fd->real_fd;
    reset_access(memory);
    memory->hook_task = &fixture->task;
    memory->hook_address = buffer_pointer;
    memory->hook_access = GUEST_MEMORY_WRITE;
    memory->hook_fd = lifecycle_socket;
    memory->hook_raw_fd = original_raw_fd;
    CHECK(invoke(fixture, memory, fault, SYS_RECVFROM,
                    lifecycle_socket, buffer_pointer, 4, 0, 0, 0) == 4 &&
            memory->hook_fired && memory->hook_succeeded &&
            memory->raw_open_during_hook &&
            memcmp(memory->bytes + buffer_offset, "life", 4) == 0,
            "recvfrom retained 引用跨越 payload 写回与同号复用");
    errno = 0;
    CHECK(fcntl(original_raw_fd, F_GETFD) == -1 && errno == EBADF &&
            f_close_task(&fixture->task, lifecycle_socket) == 0,
            "recvfrom 返回后原 socket 与替换 fd 各自释放");

    CHECK(f_close_task(&fixture->task, guest_socket) == 0,
            "recvfrom guest socket 清理成功");
    close(sender);
    return 0;
}

static int test_setsockopt(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t value_offset = 0x7000;
    const qword_t value_pointer = USER_BASE + value_offset;
    sdword_t enabled = 1;
    put_user(memory, value_offset, &enabled, sizeof(enabled));

    reset_access(memory);
    memory->fail_read_at = value_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    99, SOL_SOCKET_, SO_REUSEADDR_,
                    value_pointer, sizeof(enabled), 0) ==
                    encoded_error(_EBADF) && memory->read_calls == 0,
            "setsockopt 在 optval 与 optlen 前返回 EBADF");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    0, SOL_SOCKET_, SO_REUSEADDR_,
                    value_pointer, sizeof(enabled), 0) ==
                    encoded_error(_ENOTSOCK) && memory->read_calls == 0,
            "setsockopt 在 optval 前返回 ENOTSOCK");

    int guest_socket = create_guest_udp(fixture, memory, fault);
    CHECK(guest_socket >= 0, "setsockopt guest UDP socket 创建成功");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, SOL_SOCKET_, SO_REUSEADDR_,
                    value_pointer, UINT64_C(0xffffffff), 0) ==
                    encoded_error(_EINVAL) && memory->read_calls == 0,
            "setsockopt 负 optlen 不读取 optval");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, 0x7fff, 1,
                    UINT64_MAX, INT32_MAX, 0) ==
                    encoded_error(_ENOPROTOOPT) && memory->read_calls == 0,
            "setsockopt 未知 level 不读取 optval");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    UINT64_C(0xface000000000000) | (dword_t) guest_socket,
                    UINT64_C(0xbeef000000000000) | SOL_SOCKET_,
                    UINT64_C(0xcafe000000000000) | SO_REUSEADDR_,
                    value_pointer, INT32_MAX, 0) == 0 &&
            memory->access_count == 1 &&
            access_is(memory, 0, value_pointer, sizeof(enabled),
                    GUEST_MEMORY_READ),
            "setsockopt 忽略标量高位且巨大 optlen 只读取四字节");
    struct fd *socket_fd = f_get_task(&fixture->task, guest_socket);
    int host_enabled = 0;
    socklen_t host_enabled_length = sizeof(host_enabled);
    CHECK(socket_fd != NULL && getsockopt(socket_fd->real_fd,
                    SOL_SOCKET, SO_REUSEADDR,
                    &host_enabled, &host_enabled_length) == 0 &&
            host_enabled != 0,
            "setsockopt 标量选项实际作用于 host socket");

    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, SOL_SOCKET_, SO_LINGER_,
                    value_pointer, sizeof(enabled), 0) ==
                    encoded_error(_EINVAL) && memory->read_calls == 1 &&
            memory->accesses[0].size == sizeof(enabled),
            "setsockopt SO_LINGER 短值只执行首阶段四字节读取");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, SOL_SOCKET_, SO_TYPE_,
                    value_pointer, sizeof(enabled), 0) ==
                    encoded_error(_ENOPROTOOPT) && memory->read_calls == 1,
            "setsockopt 只读 SOL_SOCKET 选项读取标量后返回 ENOPROTOOPT");

    uint8_t ttl = 42;
    put_user(memory, value_offset, &ttl, sizeof(ttl));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, IPPROTO_IP, IP_TTL_,
                    value_pointer, sizeof(ttl), 0) == 0 &&
            memory->read_calls == 1 && memory->accesses[0].size == 1,
            "setsockopt IP 标量接受 Linux 单字节形式");
    host_enabled = 0;
    host_enabled_length = sizeof(host_enabled);
    CHECK(getsockopt(socket_fd->real_fd, IPPROTO_IP, IP_TTL,
                    &host_enabled, &host_enabled_length) == 0 &&
            host_enabled == ttl,
            "setsockopt 单字节 IP_TTL 实际作用于 host socket");
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, IPPROTO_IP, IP_TOS_,
                    UINT64_MAX, 0, 0) == 0 && memory->read_calls == 0,
            "setsockopt IP_TOS 允许零长度清零且不访问 optval");

    struct linux_timeval64 timeout = {
        .seconds = 1,
        .microseconds = 250000,
    };
    put_user(memory, value_offset, &timeout, sizeof(timeout));
    static const sdword_t timeout_options[] = {
        SO_RCVTIMEO_OLD_, SO_SNDTIMEO_OLD_,
        SO_RCVTIMEO_NEW_, SO_SNDTIMEO_NEW_,
    };
    for (size_t index = 0; index < array_size(timeout_options); index++) {
        reset_access(memory);
        CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                        guest_socket, SOL_SOCKET_, timeout_options[index],
                        value_pointer, sizeof(timeout), 0) == 0 &&
                memory->read_calls == 1 &&
                memory->accesses[0].size == sizeof(timeout),
                "setsockopt 四组 AArch64 timeval64 编号均按 16 字节解码");
    }
    timeout.microseconds = 1000000;
    put_user(memory, value_offset, &timeout, sizeof(timeout));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, SOL_SOCKET_, SO_RCVTIMEO_OLD_,
                    value_pointer, sizeof(timeout), 0) ==
                    encoded_error(_EDOM) && memory->read_calls == 1,
            "setsockopt 拒绝越界 timeval64 微秒字段");

    enabled = 1;
    put_user(memory, value_offset, &enabled, sizeof(enabled));
    socket_fd = f_get_task(&fixture->task, guest_socket);
    CHECK(socket_fd != NULL, "setsockopt 生命周期取得原 socket");
    int original_raw_fd = socket_fd->real_fd;
    int observer_fd = dup(original_raw_fd);
    CHECK(observer_fd >= 0, "setsockopt 生命周期复制观察 fd");
    reset_access(memory);
    memory->hook_task = &fixture->task;
    memory->hook_address = value_pointer;
    memory->hook_access = GUEST_MEMORY_READ;
    memory->hook_fd = guest_socket;
    memory->hook_raw_fd = original_raw_fd;
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    guest_socket, SOL_SOCKET_, SO_BROADCAST_,
                    value_pointer, sizeof(enabled), 0) == 0 &&
            memory->hook_fired && memory->hook_succeeded &&
            memory->raw_open_during_hook,
            "setsockopt retained 引用跨越 optval 回调与同号复用");
    host_enabled = 0;
    host_enabled_length = sizeof(host_enabled);
    CHECK(getsockopt(observer_fd, SOL_SOCKET, SO_BROADCAST,
                    &host_enabled, &host_enabled_length) == 0 &&
            host_enabled != 0,
            "setsockopt 在 fd 复用后仍修改原 socket");
    errno = 0;
    CHECK(fcntl(original_raw_fd, F_GETFD) == -1 && errno == EBADF &&
            f_close_task(&fixture->task, guest_socket) == 0,
            "setsockopt 返回后原 socket 与替换 fd 各自释放");
    close(observer_fd);
    return 0;
}

int main(void) {
    struct fixture fixture;
    struct user_memory memory = {
        .bytes = calloc(1, USER_MEMORY_SIZE),
    };
    struct guest_linux_user_fault fault = {0};
    CHECK(memory.bytes != NULL, "guest 测试内存分配成功");
    CHECK(fixture_init(&fixture), "socket syscall 测试夹具初始化成功");

    int result = test_bind(&fixture, &memory, &fault);
    if (result == 0)
        result = test_unix_bind(&fixture, &memory, &fault);
    if (result == 0)
        result = test_sendto(&fixture, &memory, &fault);
    if (result == 0)
        result = test_recvfrom(&fixture, &memory, &fault);
    if (result == 0)
        result = test_setsockopt(&fixture, &memory, &fault);

    fixture_destroy(&fixture);
    free(memory.bytes);
    return result;
}

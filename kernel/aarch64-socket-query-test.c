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
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc67890000)
#define USER_MEMORY_SIZE UINT32_C(0x20000)
#define USER_ACCESS_LOG_SIZE 16

#define SYS_SOCKET 198
#define SYS_BIND 200
#define SYS_CONNECT 203
#define SYS_GETSOCKNAME 204
#define SYS_GETPEERNAME 205
#define SYS_SETSOCKOPT 208
#define SYS_GETSOCKOPT 209
#define SYS_SHUTDOWN 210

#define LINUX_SO_ACCEPTCONN 30

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 socket 查询测试失败：%s（第 %d 行）\n", \
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
        "Linux IPv4 sockaddr wire 必须为 16 字节");
_Static_assert(sizeof(struct linux_timeval64) == 16,
        "AArch64 Linux timeval wire 必须为 16 字节");

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

static int create_guest_socket(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        dword_t type) {
    return (int) (sqword_t) invoke(fixture, memory, fault, SYS_SOCKET,
            AF_INET_, type, 0, 0, 0, 0);
}

static struct linux_sockaddr_in linux_loopback(uint16_t port) {
    return (struct linux_sockaddr_in) {
        .family = AF_INET_,
        .port = port,
        .address = htonl(INADDR_LOOPBACK),
    };
}

static bool bind_guest_loopback(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        fd_t socket_number, size_t offset, struct sockaddr_in *host) {
    struct linux_sockaddr_in wire = linux_loopback(0);
    qword_t pointer = put_user(memory, offset, &wire, sizeof(wire));
    if (invoke(fixture, memory, fault, SYS_BIND, socket_number,
                pointer, sizeof(wire), 0, 0, 0) != 0)
        return false;
    struct fd *socket = f_get_task(&fixture->task, socket_number);
    socklen_t length = sizeof(*host);
    return socket != NULL && getsockname(socket->real_fd,
            (struct sockaddr *) host, &length) == 0 &&
            length == sizeof(*host) && host->sin_port != 0;
}

static int host_udp_listener(struct sockaddr_in *address) {
    int listener = socket(AF_INET, SOCK_DGRAM, 0);
    if (listener < 0)
        return -1;
    *address = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(listener, (const struct sockaddr *) address,
                sizeof(*address)) < 0) {
        close(listener);
        return -1;
    }
    socklen_t length = sizeof(*address);
    if (getsockname(listener, (struct sockaddr *) address, &length) < 0) {
        close(listener);
        return -1;
    }
    return listener;
}

static int test_error_priority(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const qword_t address_pointer = USER_BASE + 0x100;
    const qword_t length_pointer = USER_BASE + 0x200;
    struct fd *ordinary = fd_create(&replacement_ops);
    CHECK(ordinary != NULL, "普通 fd 创建成功");
    ordinary->data = &memory->replacement_closes;
    CHECK(f_install_task(&fixture->task, ordinary, 0) == 0,
            "普通 fd 安装到首个槽位");

    static const qword_t name_calls[] = {
        SYS_GETSOCKNAME, SYS_GETPEERNAME,
    };
    for (size_t index = 0; index < array_size(name_calls); index++) {
        reset_access(memory);
        memory->fail_read_at = length_pointer;
        CHECK(invoke(fixture, memory, fault, name_calls[index],
                        99, address_pointer, length_pointer, 0, 0, 0) ==
                        encoded_error(_EBADF) &&
                memory->access_count == 0,
                "getname 在 guest 长度访问前返回 EBADF");
        reset_access(memory);
        memory->fail_read_at = length_pointer;
        CHECK(invoke(fixture, memory, fault, name_calls[index],
                        0, address_pointer, length_pointer, 0, 0, 0) ==
                        encoded_error(_ENOTSOCK) &&
                memory->access_count == 0,
                "getname 在 guest 长度访问前返回 ENOTSOCK");
    }

    reset_access(memory);
    memory->fail_read_at = length_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    99, SOL_SOCKET_, SO_DOMAIN_,
                    address_pointer, length_pointer, 0) ==
                    encoded_error(_EBADF) && memory->access_count == 0,
            "getsockopt 在 optlen 访问前返回 EBADF");
    reset_access(memory);
    memory->fail_read_at = length_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    0, SOL_SOCKET_, SO_DOMAIN_,
                    address_pointer, length_pointer, 0) ==
                    encoded_error(_ENOTSOCK) && memory->access_count == 0,
            "getsockopt 在 optlen 访问前返回 ENOTSOCK");
    CHECK(invoke(fixture, memory, fault, SYS_SHUTDOWN,
                    99, SHUT_RDWR, 0, 0, 0, 0) ==
                    encoded_error(_EBADF) &&
            invoke(fixture, memory, fault, SYS_SHUTDOWN,
                    0, SHUT_RDWR, 0, 0, 0, 0) ==
                    encoded_error(_ENOTSOCK),
            "shutdown 保持 EBADF 与 ENOTSOCK 优先级");

    int udp = create_guest_socket(fixture, memory, fault, SOCK_DGRAM_);
    CHECK(udp >= 0, "未连接 UDP socket 创建成功");
    reset_access(memory);
    memory->fail_read_at = length_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETPEERNAME,
                    udp, address_pointer, length_pointer, 0, 0, 0) ==
                    encoded_error(_ENOTCONN) &&
            memory->access_count == 0,
            "getpeername 在协议状态判定后才读取长度");
    CHECK(f_close_task(&fixture->task, udp) == 0 &&
            f_close_task(&fixture->task, 0) == 0,
            "优先级测试 fd 精确清理成功");
    return 0;
}

static int test_socket_names(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t address_offset = 0x1000;
    const size_t length_offset = 0x1100;
    const qword_t address_pointer = USER_BASE + address_offset;
    const qword_t length_pointer = USER_BASE + length_offset;
    int udp = create_guest_socket(fixture, memory, fault, SOCK_DGRAM_);
    struct sockaddr_in host_address = {0};
    CHECK(udp >= 0 && bind_guest_loopback(fixture, memory, fault,
                    udp, 0x1200, &host_address),
            "getsockname UDP 回环绑定成功");
    const struct linux_sockaddr_in expected =
            linux_loopback(host_address.sin_port);

    sdword_t capacity = -1;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    memset(memory->bytes + address_offset, 0xa5, sizeof(expected));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKNAME,
                    udp, address_pointer, length_pointer, 0, 0, 0) ==
                    encoded_error(_EINVAL) &&
            memory->access_count == 1 &&
            access_is(memory, 0, length_pointer, sizeof(capacity),
                    GUEST_MEMORY_READ) &&
            memory->bytes[address_offset] == 0xa5,
            "getsockname 负容量只读取长度并保持地址不变");

    capacity = 0;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKNAME,
                    udp, address_pointer, length_pointer, 0, 0, 0) == 0 &&
            memory->access_count == 2 &&
            access_is(memory, 0, length_pointer, sizeof(capacity),
                    GUEST_MEMORY_READ) &&
            access_is(memory, 1, length_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            *(dword_t *) (memory->bytes + length_offset) ==
                    sizeof(expected) &&
            memory->bytes[address_offset] == 0xa5,
            "getsockname 零容量只写回真实长度");

    capacity = 3;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    qword_t decorated_fd = UINT64_C(0xfeed000000000000) |
            (dword_t) udp;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKNAME,
                    decorated_fd, address_pointer, length_pointer,
                    0, 0, 0) == 0 && memory->access_count == 3 &&
            access_is(memory, 1, length_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 2, address_pointer, 3,
                    GUEST_MEMORY_WRITE) &&
            memcmp(memory->bytes + address_offset, &expected, 3) == 0,
            "getsockname 忽略 fd 高位并截断写入 64 位地址");

    capacity = sizeof(expected);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    memset(memory->bytes + address_offset, 0xa5, sizeof(expected));
    reset_access(memory);
    memory->fail_write_at = length_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKNAME,
                    udp, address_pointer, length_pointer, 0, 0, 0) ==
                    encoded_error(_EFAULT) &&
            memory->access_count == 2 &&
            memory->bytes[address_offset] == 0xa5,
            "getsockname 长度写回失败后不写地址");
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = address_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKNAME,
                    udp, address_pointer, length_pointer, 0, 0, 0) ==
                    encoded_error(_EFAULT) &&
            memory->access_count == 3 &&
            *(dword_t *) (memory->bytes + length_offset) ==
                    sizeof(expected),
            "getsockname 在地址 fault 前已写回真实长度");

    struct fd *socket = f_get_task(&fixture->task, udp);
    CHECK(socket != NULL, "getsockname 生命周期取得原 socket");
    int original_raw_fd = socket->real_fd;
    int observer = dup(original_raw_fd);
    int replacement_baseline = memory->replacement_closes;
    CHECK(observer >= 0, "getsockname 生命周期复制观察 fd");
    capacity = sizeof(expected);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->hook_task = &fixture->task;
    memory->hook_address = length_pointer;
    memory->hook_access = GUEST_MEMORY_READ;
    memory->hook_fd = udp;
    memory->hook_raw_fd = original_raw_fd;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKNAME,
                    udp, address_pointer, length_pointer, 0, 0, 0) == 0 &&
            memory->hook_fired && memory->hook_succeeded &&
            memory->raw_open_during_hook &&
            memcmp(memory->bytes + address_offset,
                    &expected, sizeof(expected)) == 0,
            "getsockname retained 引用跨越长度回调与同号复用");
    errno = 0;
    CHECK(fcntl(original_raw_fd, F_GETFD) == -1 && errno == EBADF &&
            f_close_task(&fixture->task, udp) == 0 &&
            memory->replacement_closes == replacement_baseline + 1,
            "getsockname 返回后原 socket 与替换 fd 各自释放");
    close(observer);

    struct sockaddr_in peer_address = {0};
    int peer = host_udp_listener(&peer_address);
    int connected = create_guest_socket(
            fixture, memory, fault, SOCK_DGRAM_);
    struct linux_sockaddr_in destination =
            linux_loopback(peer_address.sin_port);
    qword_t destination_pointer = put_user(
            memory, 0x1300, &destination, sizeof(destination));
    CHECK(peer >= 0 && connected >= 0 &&
            invoke(fixture, memory, fault, SYS_CONNECT,
                    connected, destination_pointer, sizeof(destination),
                    0, 0, 0) == 0,
            "getpeername UDP 回环连接成功");
    capacity = sizeof(destination);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_GETPEERNAME,
                    connected, address_pointer, length_pointer,
                    0, 0, 0) == 0 && memory->access_count == 3 &&
            memcmp(memory->bytes + address_offset,
                    &destination, sizeof(destination)) == 0 &&
            *(dword_t *) (memory->bytes + length_offset) ==
                    sizeof(destination),
            "getpeername 返回完整 Linux IPv4 wire");
    CHECK(f_close_task(&fixture->task, connected) == 0,
            "getpeername guest socket 清理成功");
    close(peer);
    return 0;
}

static int query_int(struct fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        fd_t socket_number, sdword_t option,
        size_t value_offset, size_t length_offset, sdword_t *value) {
    sdword_t capacity = sizeof(*value);
    memset(memory->bytes + value_offset, 0xa5, sizeof(*value));
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    qword_t result = invoke(fixture, memory, fault, SYS_GETSOCKOPT,
            socket_number, SOL_SOCKET_, (dword_t) option,
            USER_BASE + value_offset, USER_BASE + length_offset, 0);
    get_user(memory, value_offset, value, sizeof(*value));
    return (int) (sqword_t) result;
}

static int test_socket_options(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t value_offset = 0x3000;
    const size_t length_offset = 0x3100;
    const qword_t value_pointer = USER_BASE + value_offset;
    const qword_t length_pointer = USER_BASE + length_offset;
    int udp = create_guest_socket(fixture, memory, fault, SOCK_DGRAM_);
    CHECK(udp >= 0, "getsockopt UDP socket 创建成功");

    static const sdword_t capacities[] = {0, 1, 3, 4, 8};
    static const struct {
        sdword_t option;
        sdword_t expected;
    } integer_cases[] = {
        {SO_DOMAIN_, AF_INET_}, {SO_TYPE_, SOCK_DGRAM_},
        {SO_PROTOCOL_, IPPROTO_UDP}, {LINUX_SO_ACCEPTCONN, 0},
    };
    for (size_t option_index = 0;
            option_index < array_size(integer_cases); option_index++) {
        for (size_t index = 0; index < array_size(capacities); index++) {
        sdword_t capacity = capacities[index];
        dword_t copied = capacity < (sdword_t) sizeof(sdword_t) ?
                (dword_t) capacity : (dword_t) sizeof(sdword_t);
        memset(memory->bytes + value_offset, 0xa5, 8);
        put_user(memory, length_offset, &capacity, sizeof(capacity));
        reset_access(memory);
        qword_t decorated_fd = UINT64_C(0xbeef000000000000) |
                (dword_t) udp;
        qword_t decorated_level = UINT64_C(0xface000000000000) |
                SOL_SOCKET_;
        qword_t decorated_option = UINT64_C(0xcafe000000000000) |
                (dword_t) integer_cases[option_index].option;
        CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                        decorated_fd, decorated_level, decorated_option,
                        value_pointer, length_pointer, 0) == 0 &&
                memory->access_count == (copied == 0 ? 2 : 3) &&
                access_is(memory, 0, length_pointer, sizeof(capacity),
                        GUEST_MEMORY_READ) &&
                (copied == 0 || access_is(memory, 1,
                        value_pointer, copied, GUEST_MEMORY_WRITE)) &&
                access_is(memory, copied == 0 ? 1 : 2,
                        length_pointer, sizeof(dword_t),
                        GUEST_MEMORY_WRITE) &&
                *(dword_t *) (memory->bytes + length_offset) == copied &&
                (copied == 0 || memcmp(memory->bytes + value_offset,
                        &integer_cases[option_index].expected,
                        copied) == 0),
                "SOL_SOCKET 整数选项支持零、短、完整与超长容量");
      }
    }

    sdword_t integer;
    sdword_t capacity = -1;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    udp, SOL_SOCKET_, SO_DOMAIN_,
                    value_pointer, length_pointer, 0) ==
                    encoded_error(_EINVAL) && memory->access_count == 1,
            "getsockopt 将 optlen 按低 32 位有符号数解释");

    capacity = sizeof(sdword_t);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = value_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    udp, SOL_SOCKET_, SO_DOMAIN_,
                    value_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT) &&
            memory->access_count == 2 &&
            *(sdword_t *) (memory->bytes + length_offset) == capacity,
            "SOL_SOCKET 值写回失败后不写 optlen");
    memset(memory->bytes + value_offset, 0xa5, sizeof(sdword_t));
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = length_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    udp, SOL_SOCKET_, SO_DOMAIN_,
                    value_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT) &&
            memory->access_count == 3 &&
            *(sdword_t *) (memory->bytes + value_offset) == AF_INET_,
            "SOL_SOCKET 在 optlen fault 前已经写回值");

    struct linux_timeval64 timeout = {.seconds = 2};
    qword_t timeout_pointer = put_user(
            memory, 0x3200, &timeout, sizeof(timeout));
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    udp, SOL_SOCKET_, SO_RCVTIMEO_NEW_,
                    timeout_pointer, sizeof(timeout), 0) == 0,
            "设置 AArch64 接收超时成功");
    timeout.seconds = 3;
    put_user(memory, 0x3200, &timeout, sizeof(timeout));
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    udp, SOL_SOCKET_, SO_SNDTIMEO_OLD_,
                    timeout_pointer, sizeof(timeout), 0) == 0,
            "设置 AArch64 发送超时成功");
    static const struct {
        sdword_t option;
        int64_t seconds;
    } timeout_cases[] = {
        {SO_RCVTIMEO_OLD_, 2}, {SO_RCVTIMEO_NEW_, 2},
        {SO_SNDTIMEO_OLD_, 3}, {SO_SNDTIMEO_NEW_, 3},
    };
    for (size_t index = 0; index < array_size(timeout_cases); index++) {
        capacity = sizeof(struct linux_timeval64);
        memset(memory->bytes + value_offset, 0xa5, sizeof(timeout));
        put_user(memory, length_offset, &capacity, sizeof(capacity));
        reset_access(memory);
        CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                        udp, SOL_SOCKET_, timeout_cases[index].option,
                        value_pointer, length_pointer, 0) == 0 &&
                memory->access_count == 3 &&
                access_is(memory, 1, value_pointer, sizeof(timeout),
                        GUEST_MEMORY_WRITE) &&
                access_is(memory, 2, length_pointer, sizeof(dword_t),
                        GUEST_MEMORY_WRITE),
                "AArch64 OLD/NEW timeout 均写回 16 字节值后长度");
        get_user(memory, value_offset, &timeout, sizeof(timeout));
        CHECK(timeout.seconds == timeout_cases[index].seconds &&
                timeout.microseconds == 0 &&
                *(dword_t *) (memory->bytes + length_offset) ==
                        sizeof(timeout),
                "AArch64 timeout 字段转换保持 timeval64 wire");
    }
    sdword_t ttl = 42;
    qword_t ttl_pointer = put_user(memory, 0x3280, &ttl, sizeof(ttl));
    CHECK(invoke(fixture, memory, fault, SYS_SETSOCKOPT,
                    udp, IPPROTO_IP, IP_TTL_, ttl_pointer,
                    sizeof(ttl), 0) == 0,
            "IP_TTL 短缓冲回归设置固定值成功");
    capacity = 3;
    memset(memory->bytes + value_offset, 0xa5, 4);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    udp, IPPROTO_IP, IP_TTL_,
                    value_pointer, length_pointer, 0) == 0 &&
            memory->access_count == 3 &&
            access_is(memory, 1, length_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 2, value_pointer, 1,
                    GUEST_MEMORY_WRITE) &&
            *(dword_t *) (memory->bytes + length_offset) == 1 &&
            memory->bytes[value_offset] == ttl &&
            memory->bytes[value_offset + 1] == 0xa5,
            "IP_TTL 三字节容量按 Linux 规则只返回一字节");
    capacity = 1;
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = value_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    udp, IPPROTO_IP, IP_TTL_,
                    value_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT) &&
            *(dword_t *) (memory->bytes + length_offset) == 1,
            "IP_TTL 值 fault 前已经按协议层顺序写回长度");
    CHECK(f_close_task(&fixture->task, udp) == 0,
            "getsockopt UDP socket 清理成功");

    int tcp = create_guest_socket(fixture, memory, fault, SOCK_STREAM_);
    CHECK(tcp >= 0, "TCP 查询 socket 创建成功");
    CHECK(query_int(fixture, memory, fault, tcp, SO_TYPE_,
                    value_offset, length_offset, &integer) == 0 &&
            integer == SOCK_STREAM_ &&
            query_int(fixture, memory, fault, tcp, SO_PROTOCOL_,
                    value_offset, length_offset, &integer) == 0 &&
            integer == IPPROTO_TCP,
            "TCP socket 返回 guest stream 类型与默认协议号 6");
    static const byte_t congestion[16] = {'c', 'u', 'b', 'i', 'c'};
    capacity = sizeof(congestion);
    memset(memory->bytes + value_offset, 0xa5, 16);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    tcp, IPPROTO_TCP, TCP_CONGESTION_,
                    value_pointer, length_pointer, 0) == 0 &&
            memory->access_count == 3 &&
            access_is(memory, 1, length_pointer, sizeof(dword_t),
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 2, value_pointer, sizeof(congestion),
                    GUEST_MEMORY_WRITE) &&
            *(dword_t *) (memory->bytes + length_offset) ==
                    sizeof(congestion) &&
            memcmp(memory->bytes + value_offset,
                    congestion, sizeof(congestion)) == 0,
            "TCP_CONGESTION 先写长度并返回 16 字节零填充名称");
    capacity = 32;
    memset(memory->bytes + value_offset, 0xa5, 16);
    put_user(memory, length_offset, &capacity, sizeof(capacity));
    reset_access(memory);
    memory->fail_write_at = value_pointer;
    CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                    tcp, IPPROTO_TCP, TCP_CONGESTION_,
                    value_pointer, length_pointer, 0) ==
                    encoded_error(_EFAULT) &&
            memory->access_count == 3 &&
            *(dword_t *) (memory->bytes + length_offset) == 16,
            "TCP_CONGESTION 值 fault 前已将长度截为 16");

    sdword_t accepting;
    CHECK(query_int(fixture, memory, fault, tcp,
                    LINUX_SO_ACCEPTCONN,
                    value_offset, length_offset, &accepting) == 0 &&
            accepting == 0,
            "SO_ACCEPTCONN 在 listen 前返回零");
    struct sockaddr_in bound = {0};
    CHECK(bind_guest_loopback(fixture, memory, fault,
                    tcp, 0x3300, &bound) &&
            sys_listen(tcp, 1) == 0,
            "SO_ACCEPTCONN 测试 listener 建立成功");
    CHECK(query_int(fixture, memory, fault, tcp,
                    LINUX_SO_ACCEPTCONN,
                    value_offset, length_offset, &accepting) == 0 &&
            accepting == 1,
            "SO_ACCEPTCONN 使用显式监听状态返回一");
    CHECK(sys_listen(tcp, 4) == 0 &&
            query_int(fixture, memory, fault, tcp,
                    LINUX_SO_ACCEPTCONN,
                    value_offset, length_offset, &accepting) == 0 &&
            accepting == 1,
            "重复 listen 成功后 SO_ACCEPTCONN 继续返回一");
    CHECK(f_close_task(&fixture->task, tcp) == 0,
            "TCP 查询 socket 清理成功");
    return 0;
}

static int test_so_error_consumption(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    const size_t value_offset = 0x4000;
    const size_t length_offset = 0x4100;
    const qword_t value_pointer = USER_BASE + value_offset;
    const qword_t length_pointer = USER_BASE + length_offset;
    for (unsigned trial = 0; trial < 3; trial++) {
        int reservation = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in host_address = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        CHECK(reservation >= 0 && bind(reservation,
                        (const struct sockaddr *) &host_address,
                        sizeof(host_address)) == 0,
                "SO_ERROR 预留回环 TCP 端口成功");
        socklen_t host_length = sizeof(host_address);
        CHECK(getsockname(reservation,
                        (struct sockaddr *) &host_address,
                        &host_length) == 0,
                "SO_ERROR 取得预留 TCP 端口成功");
        close(reservation);

        int tcp = create_guest_socket(fixture, memory, fault,
                SOCK_STREAM_ | SOCK_NONBLOCK_);
        struct linux_sockaddr_in destination =
                linux_loopback(host_address.sin_port);
        qword_t destination_pointer = put_user(
                memory, 0x4200, &destination, sizeof(destination));
        CHECK(tcp >= 0 && invoke(fixture, memory, fault, SYS_CONNECT,
                        tcp, destination_pointer, sizeof(destination),
                        0, 0, 0) == encoded_error(_EINPROGRESS),
                "非阻塞 TCP connect 进入 EINPROGRESS");
        struct fd *socket = f_get_task(&fixture->task, tcp);
        struct pollfd event = {
            .fd = socket == NULL ? -1 : socket->real_fd,
            .events = POLLOUT,
        };
        CHECK(socket != NULL && poll(&event, 1, 2000) == 1 &&
                (event.revents & (POLLOUT | POLLERR | POLLHUP)) != 0,
                "回环拒绝连接在两秒内产生异步状态");

        sdword_t capacity = trial == 2 ? 0 : sizeof(sdword_t);
        sdword_t sentinel = INT32_C(0x5a5a5a5a);
        put_user(memory, value_offset, &sentinel, sizeof(sentinel));
        put_user(memory, length_offset, &capacity, sizeof(capacity));
        reset_access(memory);
        if (trial < 2)
            memory->fail_write_at = trial == 0 ?
                    value_pointer : length_pointer;
        qword_t option_value_pointer = trial == 2 ?
                UINT64_MAX : value_pointer;
        CHECK(invoke(fixture, memory, fault, SYS_GETSOCKOPT,
                        tcp, SOL_SOCKET_, SO_ERROR_,
                        option_value_pointer, length_pointer, 0) ==
                        (trial == 2 ? 0 : encoded_error(_EFAULT)) &&
                memory->access_count == (trial == 1 ? 3 : 2),
                "SO_ERROR 在 copyout fault 或零容量下只消费一次");
        lock(&socket->lock);
        bool pending = socket->socket.inet_connect_pending;
        unlock(&socket->lock);
        CHECK(!pending,
                "SO_ERROR 在任何实际 copyout 前已消费异步错误");

        sdword_t error_value = -1;
        CHECK(query_int(fixture, memory, fault, tcp, SO_ERROR_,
                        value_offset, length_offset, &error_value) == 0 &&
                error_value == 0,
                "SO_ERROR 消费后的第二次查询只返回零");
        CHECK(f_close_task(&fixture->task, tcp) == 0,
                "SO_ERROR guest socket 清理成功");
    }
    return 0;
}

static int install_connected_stream(struct fixture *fixture, int *peer) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
        return _EIO;
    struct fd *socket = fd_create(&socket_fdops);
    if (socket == NULL) {
        close(sockets[0]);
        close(sockets[1]);
        return _ENOMEM;
    }
    socket->real_fd = sockets[0];
    // 使用确定的 host pair，只向核心声明本测试需要的 INET stream 语义。
    socket->socket.domain = AF_INET_;
    socket->socket.type = SOCK_STREAM_;
    int number = f_install_task(&fixture->task, socket, 0);
    if (number < 0) {
        close(sockets[1]);
        return number;
    }
    *peer = sockets[1];
    return number;
}

static int test_shutdown(struct fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault) {
    int peer = -1;
    int socket_number = install_connected_stream(fixture, &peer);
    CHECK(socket_number >= 0 && peer >= 0,
            "shutdown 确定性 stream pair 创建成功");
    CHECK(invoke(fixture, memory, fault, SYS_SHUTDOWN,
                    socket_number, UINT64_C(0xffffffff),
                    0, 0, 0, 0) == encoded_error(_EINVAL),
            "shutdown 将 how 按低 32 位有符号数解释");

    struct fd *socket = f_get_task(&fixture->task, socket_number);
    byte_t payload = 0x6b;
    CHECK(socket != NULL && send(peer, &payload, sizeof(payload), 0) == 1 &&
            recv(socket->real_fd, &payload, sizeof(payload), 0) == 1,
            "无效 shutdown 后连接仍可双向使用");
    qword_t decorated_fd = UINT64_C(0xabcd000000000000) |
            (dword_t) socket_number;
    qword_t decorated_how = UINT64_C(0x1234000000000000) | SHUT_WR;
    CHECK(invoke(fixture, memory, fault, SYS_SHUTDOWN,
                    decorated_fd, decorated_how, 0, 0, 0, 0) == 0,
            "shutdown 忽略 fd 与 how 的高 32 位");

    CHECK(recv(peer, &payload, sizeof(payload), 0) == 0,
            "SHUT_WR 向对端交付有序 EOF");
    payload = 0x7c;
    CHECK(send(peer, &payload, sizeof(payload), 0) == 1 &&
            recv(socket->real_fd, &payload, sizeof(payload), 0) == 1 &&
            payload == 0x7c,
            "SHUT_WR 后本端读取方向仍然可用");
    CHECK(f_close_task(&fixture->task, socket_number) == 0,
            "shutdown guest socket 清理成功");
    close(peer);
    return 0;
}

static unsigned occupied_fds(struct fixture *fixture) {
    unsigned count = 0;
    for (fd_t number = 0; number < 64; number++)
        if (f_get_task(&fixture->task, number) != NULL)
            count++;
    return count;
}

int main(void) {
    struct fixture fixture;
    struct user_memory memory = {
        .bytes = calloc(1, USER_MEMORY_SIZE),
    };
    struct guest_linux_user_fault fault = {0};
    CHECK(memory.bytes != NULL, "guest 测试内存分配成功");
    if (!fixture_init(&fixture)) {
        fprintf(stderr,
                "AArch64 socket 查询测试失败：测试夹具初始化失败\n");
        free(memory.bytes);
        return 1;
    }

    int result = test_error_priority(&fixture, &memory, &fault);
    if (result == 0)
        result = test_socket_names(&fixture, &memory, &fault);
    if (result == 0)
        result = test_socket_options(&fixture, &memory, &fault);
    if (result == 0)
        result = test_so_error_consumption(&fixture, &memory, &fault);
    if (result == 0)
        result = test_shutdown(&fixture, &memory, &fault);
    if (result == 0 && occupied_fds(&fixture) != 0) {
        fprintf(stderr,
                "AArch64 socket 查询测试失败：成功路径仍有 guest fd 未关闭\n");
        result = 1;
    }

    fixture_destroy(&fixture);
    free(memory.bytes);
    return result;
}

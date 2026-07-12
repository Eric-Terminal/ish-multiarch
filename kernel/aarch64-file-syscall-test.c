#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE UINT32_C(0x8000)
#define IO_DATA_SIZE 8192

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 文件系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct user_memory {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned read_calls;
    unsigned write_calls;
    dword_t max_read_size;
    dword_t max_write_size;
    struct task *replacement_task;
    struct fd *replacement_fd;
    fd_t replacement_number;
    qword_t replace_on_read;
    qword_t replace_on_write;
    bool replacement_installed;
};

struct io_probe {
    byte_t written_data[IO_DATA_SIZE];
    size_t written_size;
    size_t max_write;
    unsigned write_calls;
    size_t write_requests[4];
    unsigned write_error_call;
    int write_error;
};

#define DIRECTORY_PROBE_CAPACITY 4

struct directory_probe {
    const char *names[DIRECTORY_PROBE_CAPACITY];
    qword_t inodes[DIRECTORY_PROBE_CAPACITY];
    size_t count;
    size_t position;
    size_t error_position;
    int error;
    unsigned read_calls;
    unsigned seek_calls;
    unsigned close_calls;
};

struct syscall_fixture {
    struct task task;
    struct tgroup group;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static bool contains_address(qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
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

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    if (memory->replacement_fd != NULL &&
            address == memory->replace_on_read) {
        assert(f_close_task(memory->replacement_task,
                memory->replacement_number) == 0);
        assert(f_install_task(memory->replacement_task,
                memory->replacement_fd, 0) ==
                memory->replacement_number);
        memory->replacement_fd = NULL;
        memory->replacement_installed = true;
    }
    memory->read_calls++;
    if (size > memory->max_read_size)
        memory->max_read_size = size;
    size_t offset;
    if (!user_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return false;
    }
    if (memory->fail_read_at != UINT64_MAX &&
            contains_address(address, size, memory->fail_read_at)) {
        dword_t prefix = (dword_t) (memory->fail_read_at - address);
        memcpy(destination, memory->bytes + offset, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = memory->fail_read_at,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    memcpy(destination, memory->bytes + offset, size);
    return true;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    if (memory->replacement_fd != NULL &&
            address == memory->replace_on_write) {
        assert(f_close_task(memory->replacement_task,
                memory->replacement_number) == 0);
        assert(f_install_task(memory->replacement_task,
                memory->replacement_fd, 0) ==
                memory->replacement_number);
        memory->replacement_fd = NULL;
        memory->replacement_installed = true;
    }
    memory->write_calls++;
    if (size > memory->max_write_size)
        memory->max_write_size = size;
    size_t offset;
    if (!user_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return false;
    }
    if (memory->fail_write_at != UINT64_MAX &&
            contains_address(address, size, memory->fail_write_at)) {
        dword_t prefix = (dword_t) (memory->fail_write_at - address);
        memcpy(memory->bytes + offset, source, prefix);
        *fault = (struct guest_linux_user_fault) {
            .address = memory->fail_write_at,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_UNMAPPED,
        };
        return false;
    }
    memcpy(memory->bytes + offset, source, size);
    return true;
}

static void reset_user(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->fail_write_at = UINT64_MAX;
    memory->read_calls = 0;
    memory->write_calls = 0;
    memory->max_read_size = 0;
    memory->max_write_size = 0;
    memory->replacement_number = 0;
    memory->replace_on_read = UINT64_MAX;
    memory->replace_on_write = UINT64_MAX;
    memory->replacement_installed = false;
}

static ssize_t probe_write(struct fd *fd, const void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    probe->write_calls++;
    if (probe->write_calls <= array_size(probe->write_requests))
        probe->write_requests[probe->write_calls - 1] = size;
    if (probe->write_error_call == probe->write_calls)
        return probe->write_error;
    size_t copied = size;
    if (probe->max_write != 0 && copied > probe->max_write)
        copied = probe->max_write;
    if (copied > sizeof(probe->written_data) - probe->written_size)
        copied = sizeof(probe->written_data) - probe->written_size;
    memcpy(probe->written_data + probe->written_size, buffer, copied);
    probe->written_size += copied;
    return (ssize_t) copied;
}

static const struct fd_ops probe_fd_ops = {
    .write = probe_write,
};

static int directory_readdir(struct fd *fd, struct dir_entry *entry) {
    struct directory_probe *probe = fd->data;
    probe->read_calls++;
    if (probe->error != 0 &&
            probe->position == probe->error_position)
        return probe->error;
    if (probe->position == probe->count)
        return 0;
    assert(probe->position < probe->count);
    entry->inode = probe->inodes[probe->position];
    strcpy(entry->name, probe->names[probe->position]);
    probe->position++;
    return 1;
}

static unsigned long directory_telldir(struct fd *fd) {
    struct directory_probe *probe = fd->data;
    return (unsigned long) probe->position;
}

static void directory_seekdir(struct fd *fd, unsigned long position) {
    struct directory_probe *probe = fd->data;
    assert(position <= probe->count);
    probe->seek_calls++;
    probe->position = (size_t) position;
}

static int directory_close(struct fd *fd) {
    struct directory_probe *probe = fd->data;
    probe->close_calls++;
    return 0;
}

static const struct fd_ops directory_fd_ops = {
    .readdir = directory_readdir,
    .telldir = directory_telldir,
    .seekdir = directory_seekdir,
    .close = directory_close,
};

static struct fd *create_directory_fd(struct directory_probe *probe) {
    struct fd *fd = fd_create(&directory_fd_ops);
    if (fd == NULL)
        return NULL;
    fd->data = probe;
    fd->type = S_IFDIR;
    fd->flags = O_RDONLY_;
    return fd;
}

static void reset_io(struct io_probe *probe) {
    probe->written_size = 0;
    probe->max_write = 0;
    probe->write_calls = 0;
    memset(probe->write_requests, 0, sizeof(probe->write_requests));
    probe->write_error_call = 0;
    probe->write_error = 0;
}

static bool init_fixture(struct syscall_fixture *fixture,
        struct io_probe *probe) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {4, 4};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;
    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return false;
    fd->data = probe;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    if (f_install_task(&fixture->task, fd, 0) != 0)
        return false;
    current = &fixture->task;
    return true;
}

static void destroy_fixture(struct syscall_fixture *fixture) {
    fdtable_release(fixture->task.files);
    current = NULL;
}

static qword_t invoke(struct syscall_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, qword_t fd, qword_t vectors, qword_t count) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = memory,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {fd, vectors, count},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static void store_vectors(struct user_memory *memory, size_t offset,
        const struct aarch64_linux_iovec *vectors, size_t count) {
    memcpy(memory->bytes + offset, vectors, count * sizeof(*vectors));
}

int main(void) {
    struct io_probe io = {0};
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture, &io), "任务与 fd 夹具初始化成功");
    struct user_memory memory = {0};
    reset_user(&memory);
    struct guest_linux_user_fault fault;

    const size_t groups_offset = 0x703;
    fixture.task.ngroups = 3;
    fixture.task.groups[0] = UINT32_C(7);
    fixture.task.groups[1] = UINT32_MAX;
    fixture.task.groups[2] = UINT32_C(0x12345678);
    reset_user(&memory);
    qword_t result = invoke(&fixture, &memory, &fault, 158,
            UINT64_C(0xabcdef01ffffffff), UINT64_MAX, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.write_calls == 0,
            "getgroups 按低 32 位有符号解释负容量");
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 158,
            UINT64_C(0x1234567800000000), UINT64_MAX, 0);
    CHECK(result == 3 && memory.write_calls == 0,
            "getgroups 的零容量查询只返回附加组数量");
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 158,
            UINT64_C(0xfeedface00000002), UINT64_MAX, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.write_calls == 0,
            "getgroups 容量不足时不访问 guest 列表");

    memset(memory.bytes + groups_offset - 1, 0xa5,
            fixture.task.ngroups * sizeof(uid_t_) + 2);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 158,
            UINT64_C(0xfeedface00000004), USER_BASE + groups_offset, 0);
    CHECK(result == 3 && memory.write_calls == 1 &&
            memory.max_write_size == 3 * sizeof(uid_t_) &&
            memcmp(memory.bytes + groups_offset, fixture.task.groups,
                    3 * sizeof(uid_t_)) == 0,
            "getgroups 忽略容量高位并按原顺序写出三个 32 位 gid");
    CHECK(memory.bytes[groups_offset - 1] == 0xa5 &&
            memory.bytes[groups_offset + 3 * sizeof(uid_t_)] == 0xa5,
            "getgroups 不修改输出范围外的哨兵");

    memset(memory.bytes + groups_offset, 0xa5,
            fixture.task.ngroups * sizeof(uid_t_));
    reset_user(&memory);
    memory.fail_write_at = USER_BASE + groups_offset + sizeof(uid_t_);
    result = invoke(&fixture, &memory, &fault, 158, 3,
            USER_BASE + groups_offset, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 1 &&
            memcmp(memory.bytes + groups_offset,
                    fixture.task.groups, sizeof(uid_t_)) == 0 &&
            memory.bytes[groups_offset + sizeof(uid_t_)] == 0xa5,
            "getgroups 保持部分 guest 写回与 EFAULT");
    CHECK(fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "getgroups 传播精确写回故障");

    reset_user(&memory);
    qword_t wrapping_groups = UINT64_MAX - sizeof(uid_t_) + 2;
    result = invoke(&fixture, &memory, &fault, 158, 3,
            wrapping_groups, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            fault.address == wrapping_groups &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "getgroups 在回绕地址触发回调前返回 EFAULT");

    fixture.task.ngroups = 0;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 158, 1, UINT64_MAX, 0);
    CHECK(result == 0 && memory.write_calls == 0,
            "零附加组不访问无效列表指针");

    const size_t vector_offset = 0x100;
    const size_t first_offset = 0x1000;
    const size_t second_offset = 0x3000;
    memcpy(memory.bytes + first_offset, "hello", 5);
    memcpy(memory.bytes + second_offset, " vector", 7);
    struct aarch64_linux_iovec vectors[2] = {
        {USER_BASE + first_offset, 5},
        {USER_BASE + second_offset, 7},
    };
    store_vectors(&memory, vector_offset, vectors, 2);

    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == 12 && io.write_calls == 1 &&
            io.write_requests[0] == 12 && io.written_size == 12,
            "writev 把同一块中的多个向量合并为一次后端写入");
    CHECK(memcmp(io.written_data, "hello vector", 12) == 0 &&
            memory.read_calls == 3 && memory.max_read_size == 32,
            "writev 从 64 位 guest 地址读取精确线格式与负载");

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 4097,
    };
    for (size_t index = 0; index < 4097; index++)
        memory.bytes[first_offset + index] = (byte_t) (index * 13 + 7);
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == 4097 && io.write_calls == 1 &&
            io.write_requests[0] == 4097,
            "writev 把超过 PIPE_BUF 的向量保留为一次后端写入");
    CHECK(memcmp(io.written_data, memory.bytes + first_offset, 4097) == 0 &&
            memory.max_read_size == 4097,
            "writev 聚合负载时不丢失或重排字节");

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, UINT64_C(0x100001),
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_ENOMEM) && memory.read_calls == 1 &&
            io.write_calls == 0,
            "超过一兆字节的向量事务在复制负载前可靠返回 ENOMEM");

    memcpy(memory.bytes + first_offset, "one", 3);
    memcpy(memory.bytes + second_offset, "fault", 5);
    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 3,
    };
    vectors[1] = (struct aarch64_linux_iovec) {
        USER_BASE + second_offset, 5,
    };
    store_vectors(&memory, vector_offset, vectors, 2);
    reset_io(&io);
    reset_user(&memory);
    memory.fail_read_at = USER_BASE + second_offset + 2;
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == encoded_error(_EFAULT) && io.write_calls == 0 &&
            io.written_size == 0,
            "writev 的部分 guest 复制失败不会发送残缺消息");
    CHECK(fault.address == USER_BASE + second_offset + 2 &&
            fault.access == GUEST_MEMORY_READ,
            "writev 保持后续负载读取故障信息");

    reset_io(&io);
    reset_user(&memory);
    io.max_write = 4;
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == 4 && io.write_calls == 1 && io.written_size == 4,
            "writev 在后端短写后返回可见前缀并停止");

    reset_io(&io);
    reset_user(&memory);
    io.write_error_call = 1;
    io.write_error = _ENOSPC;
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == encoded_error(_ENOSPC) && io.write_calls == 1 &&
            io.written_size == 0,
            "writev 保持单次后端写入错误");

    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1025);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            io.write_calls == 0,
            "向量数量超过 Linux UIO_MAXIOV 时不访问 guest");

    memset(memory.bytes + vector_offset, 0,
            1024 * sizeof(struct aarch64_linux_iovec));
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1024);
    CHECK(result == 0 && memory.read_calls == 1 &&
            memory.max_read_size == 1024 * sizeof(struct aarch64_linux_iovec) &&
            io.write_calls == 0,
            "恰好 1024 个零长度向量被解析后不调用后端");

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, UINT64_C(1) << 63,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 1 &&
            io.write_calls == 0,
            "超出 ssize_t 的 iov_len 在负载访问前返回 EINVAL");

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 1,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    memory.fail_read_at = USER_BASE + vector_offset + 8;
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            io.write_calls == 0 &&
            fault.address == USER_BASE + vector_offset + 8,
            "iovec 表部分复制失败不会访问负载或后端");

    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            UINT64_MAX - 7, 1);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 0 &&
            io.write_calls == 0 && fault.access == GUEST_MEMORY_READ,
            "iovec 表自身回绕在后端读取前拒绝");

    vectors[0] = (struct aarch64_linux_iovec) {UINT64_MAX, 2};
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            io.write_calls == 0 && fault.address == UINT64_MAX &&
            fault.access == GUEST_MEMORY_READ,
            "writev 在负载复制前拒绝回绕的来源向量");

    vectors[0] = (struct aarch64_linux_iovec) {
        AARCH64_LINUX_USER_ADDRESS_MAX + UINT64_C(1), 0,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == 0 && memory.read_calls == 1 && io.write_calls == 0,
            "零长度 iovec 接受 AArch64 用户空间的 one-past 地址");
    vectors[0].base++;
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            io.write_calls == 0 && fault.address == vectors[0].base,
            "零长度 iovec 拒绝超过 one-past 的 base");

    reset_io(&io);
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 66, 0, UINT64_MAX, 0) == 0 &&
            io.write_calls == 0 && memory.read_calls == 0,
            "零向量 writev 不访问向量也不调用后端");
    CHECK(invoke(&fixture, &memory, &fault, 66, 99, UINT64_MAX, 0) ==
            encoded_error(_EBADF) && memory.read_calls == 0,
            "零向量 writev 仍先检查 fd");

    reset_io(&io);
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 66, 99,
            UINT64_MAX, 1025) == encoded_error(_EBADF) &&
            memory.read_calls == 0 && io.write_calls == 0,
            "无效 fd 优先于 iovec 数量与地址错误");

    struct fd *fixture_fd = f_get_task(&fixture.task, 0);
    CHECK(fixture_fd != NULL, "测试 fd 可用于访问模式探针");
    fixture_fd->flags = O_RDONLY_;
    reset_io(&io);
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1) == encoded_error(_EBADF) &&
            memory.read_calls == 0 && io.write_calls == 0,
            "只读 fd 在导入 iovec 前返回 EBADF");
    fixture_fd->flags = O_RDWR_;

    reset_io(&io);
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 65, 0,
            UINT64_MAX, UINT64_MAX) == encoded_error(_ENOSYS) &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "尚未提供精确部分写回语义前 readv 保持 ENOSYS");

    fd_t event_fd = sys_eventfd2(0, O_NONBLOCK_);
    CHECK(event_fd == 1 && file_write_check_task(&fixture.task, event_fd) == 0,
            "eventfd 以读写模式通过向量写入预检");
    qword_t increment = 7;
    memcpy(memory.bytes + first_offset, &increment, sizeof(increment));
    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, sizeof(increment),
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 66, event_fd,
            USER_BASE + vector_offset, 1);
    struct fd *event = f_get_task(&fixture.task, event_fd);
    CHECK(result == sizeof(increment) && event != NULL &&
            event->eventfd.val == increment,
            "writev 对 eventfd 保持单次八字节写入语义");
    CHECK(f_close_task(&fixture.task, event_fd) == 0,
            "eventfd 测试描述符清理成功");

    const size_t directory_offset = 0x5000;
    char maximum_name[NAME_MAX + 1];
    memset(maximum_name, 'x', NAME_MAX);
    maximum_name[NAME_MAX] = '\0';
    struct directory_probe directory = {
        .names = {"a", "second", maximum_name},
        .inodes = {UINT64_C(0x1020304050607080), 22, 33},
        .count = 3,
    };
    struct fd *directory_object = create_directory_fd(&directory);
    CHECK(directory_object != NULL, "目录探针描述符创建成功");
    fd_t directory_fd = f_install_task(
            &fixture.task, directory_object, 0);
    CHECK(directory_fd == 1, "目录探针安装到空闲描述符");

    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, 99,
            UINT64_MAX, UINT64_MAX);
    CHECK(result == encoded_error(_EBADF) && memory.write_calls == 0,
            "getdents64 的无效 fd 优先于 guest 地址错误");
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, 0,
            UINT64_MAX, UINT64_MAX);
    CHECK(result == encoded_error(_ENOTDIR) && memory.write_calls == 0,
            "getdents64 的非目录错误不访问 guest 缓冲区");

    memset(memory.bytes + directory_offset, 0xa5, 32);
    directory.position = 0;
    directory.seek_calls = 0;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 23);
    CHECK(result == encoded_error(_EINVAL) &&
            directory.position == 0 && directory.seek_calls == 1 &&
            memory.write_calls == 0,
            "首条目录记录差一字节时返回 EINVAL 并回滚游标");
    for (size_t index = 0; index < 32; index++)
        CHECK(memory.bytes[directory_offset + index] == 0xa5,
                "容量不足不修改 guest 哨兵");

    memset(memory.bytes + directory_offset, 0xa5, 40);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 24);
    struct aarch64_linux_dirent64 *first =
            (void *) (memory.bytes + directory_offset);
    CHECK(result == 24 && directory.position == 1 &&
            memory.write_calls == 1 && first->inode == directory.inodes[0] &&
            first->next_offset == 1 && first->length == 24 &&
            first->type == 0 && strcmp(first->name, "a") == 0,
            "短名称目录记录使用 24 字节线格式和下一位置 cookie");
    for (size_t index = 21; index < 24; index++)
        CHECK(memory.bytes[directory_offset + index] == 0,
                "短记录尾部 padding 已清零");
    CHECK(memory.bytes[directory_offset + 24] == 0xa5,
            "精确容量不越过 guest 输出边界");

    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 32);
    struct aarch64_linux_dirent64 *second =
            (void *) (memory.bytes + directory_offset);
    CHECK(result == 32 && directory.position == 2 &&
            second->inode == directory.inodes[1] &&
            second->next_offset == 2 && second->length == 32 &&
            strcmp(second->name, "second") == 0,
            "容量不足后重试从未消费的第二条记录继续");

    memset(memory.bytes + directory_offset, 0xa5, 336);
    directory.position = 0;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset,
            UINT64_C(0x12345678ffffffff));
    first = (void *) (memory.bytes + directory_offset);
    second = (void *) (memory.bytes + directory_offset + 24);
    struct aarch64_linux_dirent64 *third =
            (void *) (memory.bytes + directory_offset + 56);
    CHECK(result == 336 && directory.position == 3 &&
            memory.write_calls == 3 && first->length == 24 &&
            second->length == 32 && third->length == 280 &&
            third->next_offset == 3 && strcmp(third->name, maximum_name) == 0,
            "低 32 位最大容量不分配巨型缓冲并输出三条记录");
    for (size_t index = 50; index < 56; index++)
        CHECK(memory.bytes[directory_offset + index] == 0,
                "第二条记录的对齐 padding 已清零");
    for (size_t index = 331; index < 336; index++)
        CHECK(memory.bytes[directory_offset + index] == 0,
                "最大名称记录的对齐 padding 已清零");

    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            UINT64_MAX, UINT64_MAX);
    CHECK(result == 0 && directory.position == 3 &&
            memory.write_calls == 0,
            "目录 EOF 不访问无效 guest 指针");
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 61, directory_fd,
            UINT64_MAX, UINT64_MAX) == 0 && memory.write_calls == 0,
            "重复读取目录 EOF 仍稳定返回零");

    directory.position = 0;
    reset_user(&memory);
    memory.fail_write_at = USER_BASE + directory_offset + 10;
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 336);
    CHECK(result == encoded_error(_EFAULT) && directory.position == 0 &&
            memory.write_calls == 1 && fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "首条记录部分写 fault 返回 EFAULT 并回滚当前游标");

    directory.position = 0;
    reset_user(&memory);
    memory.fail_write_at = USER_BASE + directory_offset + 24 + 5;
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 336);
    CHECK(result == 24 && directory.position == 1 &&
            memory.write_calls == 2 && fault.address == memory.fail_write_at,
            "第二条记录写 fault 优先返回完整前缀并回滚失败条目");
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 32);
    second = (void *) (memory.bytes + directory_offset);
    CHECK(result == 32 && directory.position == 2 &&
            strcmp(second->name, "second") == 0,
            "写 fault 后的下一次调用重新输出失败条目");

    directory.position = 0;
    directory.error_position = 0;
    directory.error = _EIO;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 336);
    CHECK(result == encoded_error(_EIO) && directory.position == 0 &&
            memory.write_calls == 0,
            "首条记录前的后端错误原样传播并保持游标");
    directory.error_position = 1;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 336);
    CHECK(result == 24 && directory.position == 1 &&
            memory.write_calls == 1,
            "已有记录后的后端错误返回完整前缀");
    directory.error = 0;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 32);
    second = (void *) (memory.bytes + directory_offset);
    CHECK(result == 32 && directory.position == 2 &&
            strcmp(second->name, "second") == 0,
            "后端错误解除后从失败位置继续读取");

    directory.position = 0;
    reset_user(&memory);
    qword_t crossing_address =
            AARCH64_LINUX_USER_ADDRESS_MAX - UINT64_C(10);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            crossing_address, 24);
    CHECK(result == encoded_error(_EFAULT) && directory.position == 0 &&
            memory.write_calls == 0 && fault.address == crossing_address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "目录记录跨越 AArch64 用户地址上限时在回调前回滚");

    struct directory_probe replacement_directory = {
        .names = {"new"},
        .inodes = {44},
        .count = 1,
    };
    struct fd *directory_replacement =
            create_directory_fd(&replacement_directory);
    CHECK(directory_replacement != NULL,
            "目录 fd 复用测试替代对象创建成功");
    directory.position = 0;
    reset_user(&memory);
    memory.replacement_task = &fixture.task;
    memory.replacement_fd = directory_replacement;
    memory.replacement_number = directory_fd;
    memory.replace_on_write = USER_BASE + directory_offset;
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 24);
    CHECK(result == 24 && memory.replacement_installed &&
            directory.close_calls == 1 && directory.position == 1,
            "guest 写回期间 close 与复用不会替换本次 retained 目录对象");
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 61, directory_fd,
            USER_BASE + directory_offset, 24);
    struct aarch64_linux_dirent64 *replacement_entry =
            (void *) (memory.bytes + directory_offset);
    CHECK(result == 24 && replacement_directory.position == 1 &&
            strcmp(replacement_entry->name, "new") == 0,
            "下一次 getdents64 才观察到复用后的新目录对象");

    struct io_probe replacement_io = {0};
    struct fd *replacement = fd_create(&probe_fd_ops);
    CHECK(replacement != NULL, "fd 复用测试替代描述符创建成功");
    replacement->data = &replacement_io;
    replacement->type = S_IFREG;
    replacement->flags = O_RDWR_;
    memcpy(memory.bytes + first_offset, "retained", 8);
    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 8,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    memory.replacement_task = &fixture.task;
    memory.replacement_fd = replacement;
    memory.replace_on_read = USER_BASE + first_offset;
    result = invoke(&fixture, &memory, &fault, 66, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == 8 && memory.replacement_installed &&
            io.write_calls == 1 && replacement_io.write_calls == 0 &&
            memcmp(io.written_data, "retained", 8) == 0,
            "writev 在 close 与 fd 号复用期间仍写入预检时保留的对象");

    destroy_fixture(&fixture);
    return 0;
}

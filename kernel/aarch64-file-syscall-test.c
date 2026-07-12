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
    qword_t replace_on_read;
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
        assert(f_close_task(memory->replacement_task, 0) == 0);
        assert(f_install_task(memory->replacement_task,
                memory->replacement_fd, 0) == 0);
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
    qword_t result = invoke(&fixture, &memory, &fault, 66, 0,
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

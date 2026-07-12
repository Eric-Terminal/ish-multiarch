#include <limits.h>
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
    byte_t readable_data[IO_DATA_SIZE];
    size_t readable_size;
    size_t read_position;
    size_t max_read;
    unsigned read_calls;
    size_t read_requests[4];
    unsigned read_error_call;
    int read_error;
    byte_t written_data[IO_DATA_SIZE];
    size_t written_size;
    size_t max_write;
    unsigned write_calls;
    size_t write_requests[4];
    unsigned write_error_call;
    int write_error;
    unsigned pread_calls;
    size_t pread_requests[4];
    off_t pread_offsets[4];
    unsigned pwrite_calls;
    size_t pwrite_requests[4];
    off_t pwrite_offsets[4];
    int poll_events;
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
    struct sighand sighand;
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

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    probe->read_calls++;
    if (probe->read_calls <= array_size(probe->read_requests))
        probe->read_requests[probe->read_calls - 1] = size;
    if (probe->read_error_call == probe->read_calls)
        return probe->read_error;
    size_t available = probe->readable_size - probe->read_position;
    size_t copied = size < available ? size : available;
    if (probe->max_read != 0 && copied > probe->max_read)
        copied = probe->max_read;
    memcpy(buffer, probe->readable_data + probe->read_position, copied);
    probe->read_position += copied;
    return (ssize_t) copied;
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

static ssize_t probe_pread(
        struct fd *fd, void *buffer, size_t size, off_t offset) {
    struct io_probe *probe = fd->data;
    probe->pread_calls++;
    if (probe->pread_calls <= array_size(probe->pread_requests)) {
        probe->pread_requests[probe->pread_calls - 1] = size;
        probe->pread_offsets[probe->pread_calls - 1] = offset;
    }
    size_t position = (size_t) offset;
    size_t available = position < probe->readable_size ?
            probe->readable_size - position : 0;
    size_t copied = size < available ? size : available;
    if (probe->max_read != 0 && copied > probe->max_read)
        copied = probe->max_read;
    if (copied != 0)
        memcpy(buffer, probe->readable_data + position, copied);
    return (ssize_t) copied;
}

static ssize_t probe_pwrite(
        struct fd *fd, const void *buffer, size_t size, off_t offset) {
    struct io_probe *probe = fd->data;
    probe->pwrite_calls++;
    if (probe->pwrite_calls <= array_size(probe->pwrite_requests)) {
        probe->pwrite_requests[probe->pwrite_calls - 1] = size;
        probe->pwrite_offsets[probe->pwrite_calls - 1] = offset;
    }
    size_t copied = size;
    if (probe->max_write != 0 && copied > probe->max_write)
        copied = probe->max_write;
    if (copied > sizeof(probe->written_data) - probe->written_size)
        copied = sizeof(probe->written_data) - probe->written_size;
    memcpy(probe->written_data + probe->written_size, buffer, copied);
    probe->written_size += copied;
    return (ssize_t) copied;
}

static int probe_poll(struct fd *fd) {
    struct io_probe *probe = fd->data;
    return probe->poll_events;
}

static const struct fd_ops probe_fd_ops = {
    .read = probe_read,
    .write = probe_write,
    .pread = probe_pread,
    .pwrite = probe_pwrite,
    .poll = probe_poll,
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
    probe->readable_size = 0;
    probe->read_position = 0;
    probe->max_read = 0;
    probe->read_calls = 0;
    memset(probe->read_requests, 0, sizeof(probe->read_requests));
    probe->read_error_call = 0;
    probe->read_error = 0;
    probe->written_size = 0;
    probe->max_write = 0;
    probe->write_calls = 0;
    memset(probe->write_requests, 0, sizeof(probe->write_requests));
    probe->write_error_call = 0;
    probe->write_error = 0;
    probe->pread_calls = 0;
    memset(probe->pread_requests, 0, sizeof(probe->pread_requests));
    memset(probe->pread_offsets, 0, sizeof(probe->pread_offsets));
    probe->pwrite_calls = 0;
    memset(probe->pwrite_requests, 0, sizeof(probe->pwrite_requests));
    memset(probe->pwrite_offsets, 0, sizeof(probe->pwrite_offsets));
    probe->poll_events = 0;
}

static bool init_fixture(struct syscall_fixture *fixture,
        struct io_probe *probe) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    lock_init(&fixture->sighand.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {4, 4};
    fixture->task.group = &fixture->group;
    fixture->task.sighand = &fixture->sighand;
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

static qword_t invoke_positioned(struct syscall_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, qword_t fd, qword_t buffer, qword_t count,
        qword_t offset, qword_t high, qword_t flags) {
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
        .arguments = {fd, buffer, count, offset, high, flags},
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static qword_t invoke_epoll(struct syscall_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, qword_t epoll_fd, qword_t argument1,
        qword_t argument2, qword_t argument3,
        qword_t argument4, qword_t argument5) {
    return invoke_positioned(fixture, memory, fault, number,
            epoll_fd, argument1, argument2, argument3,
            argument4, argument5);
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

    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 20,
            UINT32_C(0x40000000), 0, 0);
    CHECK(result == encoded_error(_EINVAL),
            "epoll_create1 拒绝 CLOEXEC 之外的标志");

    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 20, O_CLOEXEC_, 0, 0);
    CHECK((sqword_t) result == 1 &&
            f_getfd_task(&fixture.task, (fd_t) result) == FD_CLOEXEC_,
            "epoll_create1 在任务 fd 表安装带 CLOEXEC 的实例");
    fd_t epoll_fd = (fd_t) result;

    const size_t epoll_event_offset = 0x6000;
    const size_t epoll_output_offset = 0x6100;
    const size_t epoll_mask_offset = 0x6200;
    const qword_t epoll_data = UINT64_C(0xfedcba9876543210);
    struct aarch64_linux_epoll_event epoll_event = {
        .events = 1,
        .padding = UINT32_C(0xa5a5a5a5),
        .data = epoll_data,
    };
    memcpy(memory.bytes + epoll_event_offset,
            &epoll_event, sizeof(epoll_event));

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 21,
            epoll_fd, 4, 0, UINT64_MAX, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0,
            "epoll_ctl 对无效操作码返回 EINVAL 且不读取 event");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 21,
            epoll_fd, 1, epoll_fd,
            USER_BASE + epoll_event_offset, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 1 &&
            memory.max_read_size == sizeof(epoll_event),
            "epoll_ctl 拒绝通过重复 fd 监听自身实例");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 21,
            epoll_fd, 1, 0,
            USER_BASE + epoll_event_offset, 0, 0);
    CHECK(result == 0 && memory.read_calls == 1 &&
            memory.max_read_size == sizeof(epoll_event),
            "epoll_ctl 从高位 AArch64 地址读取 16 字节 ADD 事件");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 22,
            epoll_fd, USER_BASE + epoll_output_offset,
            (qword_t) (INT_MAX / (int) sizeof(epoll_event) + 1),
            0, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.write_calls == 0,
            "epoll_pwait 在分配前拒绝超过 ABI 上限的 maxevents");

    reset_user(&memory);
    qword_t crossing_epoll_output =
            AARCH64_LINUX_USER_ADDRESS_MAX - UINT64_C(7);
    result = invoke_epoll(&fixture, &memory, &fault, 22,
            epoll_fd, crossing_epoll_output, 1, 0, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            fault.address == crossing_epoll_output &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "epoll_pwait 在等待前拒绝跨越用户地址上限的输出范围");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 22,
            epoll_fd, USER_BASE + epoll_output_offset, 1, 0,
            USER_BASE + epoll_mask_offset, sizeof(sigset_t_) / 2);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0,
            "epoll_pwait 严格校验非空信号掩码的长度");

    sigset_t_ epoll_mask = sig_mask(SIGUSR1_);
    memcpy(memory.bytes + epoll_mask_offset,
            &epoll_mask, sizeof(epoll_mask));
    fixture.task.blocked = sig_mask(SIGUSR2_);
    io.poll_events = 1;
    memset(memory.bytes + epoll_output_offset, 0xa5,
            sizeof(struct aarch64_linux_epoll_event));
    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 22,
            epoll_fd, USER_BASE + epoll_output_offset, 1, 0,
            USER_BASE + epoll_mask_offset, sizeof(sigset_t_));
    struct aarch64_linux_epoll_event *returned_epoll_event =
            (void *) (memory.bytes + epoll_output_offset);
    CHECK(result == 1 && memory.read_calls == 1 &&
            memory.write_calls == 1 &&
            memory.max_write_size == sizeof(*returned_epoll_event),
            "epoll_pwait 从 64 位地址读取掩码并写回一个 AArch64 事件");
    CHECK(returned_epoll_event->events == 1 &&
            returned_epoll_event->padding == 0 &&
            returned_epoll_event->data == epoll_data,
            "epoll_pwait 保留 64 位用户数据并清零 ABI 填充字段");
    CHECK(!fixture.task.has_saved_mask &&
            fixture.task.blocked == sig_mask(SIGUSR2_),
            "epoll_pwait 无信号返回后恢复原信号掩码");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 22,
            epoll_fd, USER_BASE + epoll_output_offset,
            INT_MAX / (int) sizeof(epoll_event), 0, 0, 0);
    CHECK(result == 1 && memory.write_calls == 1 &&
            memory.max_write_size == sizeof(epoll_event),
            "epoll_pwait 对巨大合法 maxevents 使用有界内部批次");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 21,
            epoll_fd, 2, 0, UINT64_MAX, 0, 0);
    CHECK(result == 0 && memory.read_calls == 0,
            "epoll_ctl DEL 忽略 event 指针并移除登记");

    reset_user(&memory);
    result = invoke_epoll(&fixture, &memory, &fault, 22,
            epoll_fd, USER_BASE + epoll_output_offset, 1, 0,
            0, UINT64_MAX);
    CHECK(result == 0 && memory.write_calls == 0,
            "epoll_pwait 在无就绪登记时立即返回且忽略空掩码长度");
    CHECK(f_close_task(&fixture.task, epoll_fd) == 0,
            "AArch64 epoll 测试实例关闭成功");

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

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 4,
    };
    vectors[1] = (struct aarch64_linux_iovec) {
        USER_BASE + second_offset, 8,
    };
    store_vectors(&memory, vector_offset, vectors, 2);
    memset(memory.bytes + first_offset, 0xa5, 4);
    memset(memory.bytes + second_offset, 0xa5, 8);
    reset_io(&io);
    memcpy(io.readable_data, "read vector", 11);
    io.readable_size = 11;
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == 11 && io.read_calls == 1 &&
            io.read_requests[0] == 12 && memory.read_calls == 1 &&
            memory.write_calls == 2,
            "readv 以一次后端读取把短结果分散到多个向量");
    CHECK(memcmp(memory.bytes + first_offset, "read", 4) == 0 &&
            memcmp(memory.bytes + second_offset, " vector", 7) == 0 &&
            memory.bytes[second_offset + 7] == 0xa5,
            "readv 只写回后端实际返回的字节并保留尾部哨兵");

    reset_io(&io);
    memcpy(io.readable_data, "failure", 7);
    io.readable_size = 7;
    reset_user(&memory);
    memory.fail_write_at = USER_BASE + first_offset + 2;
    result = invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == encoded_error(_EFAULT) && io.read_calls == 1 &&
            memory.write_calls == 1 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "readv 首个向量写回故障传播 EFAULT 与精确地址");

    vectors[0].length = 3;
    vectors[1].length = 5;
    store_vectors(&memory, vector_offset, vectors, 2);
    reset_io(&io);
    memcpy(io.readable_data, "onefault", 8);
    io.readable_size = 8;
    reset_user(&memory);
    memory.fail_write_at = USER_BASE + second_offset + 2;
    result = invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 2);
    CHECK(result == 3 && io.read_calls == 1 &&
            memory.write_calls == 2 &&
            memcmp(memory.bytes + first_offset, "one", 3) == 0,
            "readv 后续向量故障返回此前完整写回的前缀长度");

    vectors[0].length = UINT64_C(0x100001);
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_ENOMEM) && io.read_calls == 0 &&
            memory.read_calls == 1 && memory.write_calls == 0,
            "readv 超过事务上限时在后端读取前返回 ENOMEM");

    vectors[0] = (struct aarch64_linux_iovec) {
        AARCH64_LINUX_USER_ADDRESS_MAX, 2,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    result = invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_EFAULT) && io.read_calls == 0 &&
            memory.read_calls == 1 && memory.write_calls == 0 &&
            fault.address == AARCH64_LINUX_USER_ADDRESS_MAX &&
            fault.access == GUEST_MEMORY_WRITE,
            "readv 在后端读取前拒绝跨越用户上限的目标向量");

    reset_io(&io);
    reset_user(&memory);
    io.read_error_call = 1;
    io.read_error = _EIO;
    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 4,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    result = invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 1);
    CHECK(result == encoded_error(_EIO) && io.read_calls == 1 &&
            memory.write_calls == 0,
            "readv 保持单次后端读取错误且不写 guest");

    reset_io(&io);
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 65, 0,
            UINT64_MAX, 0) == 0 && io.read_calls == 0 &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "零向量 readv 检查有效 fd 后不访问 guest 或后端");
    CHECK(invoke(&fixture, &memory, &fault, 65, 99,
            UINT64_MAX, 0) == encoded_error(_EBADF) &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "零向量 readv 仍优先拒绝无效 fd");
    CHECK(invoke(&fixture, &memory, &fault, 65, 99,
            UINT64_MAX, 1025) == encoded_error(_EBADF) &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "readv 的无效 fd 优先于向量数量和地址错误");

    struct fd *readv_fd = f_get_task(&fixture.task, 0);
    CHECK(readv_fd != NULL, "readv 测试 fd 可用于访问模式探针");
    readv_fd->flags = O_WRONLY_;
    reset_io(&io);
    reset_user(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 65, 0,
            USER_BASE + vector_offset, 1) == encoded_error(_EBADF) &&
            io.read_calls == 0 && memory.read_calls == 0,
            "只写 fd 在导入 readv 向量前返回 EBADF");
    readv_fd->flags = O_RDWR_;

    struct fd *positioned_fd = f_get_task(&fixture.task, 0);
    CHECK(positioned_fd != NULL, "定位读写测试 fd 可用");
    positioned_fd->offset = 1234;

    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            67, 99, UINT64_MAX, 1, UINT64_MAX, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.write_calls == 0 &&
            io.pread_calls == 0,
            "pread64 的负 offset 优先于无效 fd 与 guest 地址");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            68, 99, UINT64_MAX, 1, UINT64_MAX, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            io.pwrite_calls == 0,
            "pwrite64 的负 offset 优先于无效 fd 与 guest 地址");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            69, 99, UINT64_MAX, 1, UINT64_MAX - 1, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            io.pread_calls == 0,
            "preadv 的普通负 offset 优先于无效 fd 与 iovec 地址");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            70, 99, UINT64_MAX, 1, UINT64_MAX - 1, 0, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            io.pwrite_calls == 0,
            "pwritev 的普通负 offset 优先于无效 fd 与 iovec 地址");

    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            67, 99, UINT64_MAX, 4, 0, 0, 0);
    CHECK(result == encoded_error(_EBADF) && memory.write_calls == 0 &&
            io.pread_calls == 0,
            "pread64 的无效 fd 不访问 guest 目标");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            68, 99, UINT64_MAX, 4, 0, 0, 0);
    CHECK(result == encoded_error(_EBADF) && memory.read_calls == 0 &&
            io.pwrite_calls == 0,
            "pwrite64 的无效 fd 不访问 guest 来源");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            69, 99, UINT64_MAX, 1, 0, 0, 0);
    CHECK(result == encoded_error(_EBADF) && memory.read_calls == 0 &&
            io.pread_calls == 0,
            "preadv 的无效 fd 不导入 iovec");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            70, 99, UINT64_MAX, 1, 0, 0, 0);
    CHECK(result == encoded_error(_EBADF) && memory.read_calls == 0 &&
            io.pwrite_calls == 0,
            "pwritev 的无效 fd 不导入 iovec");

    const off_t scalar_read_offset = 37;
    reset_io(&io);
    memcpy(io.readable_data + scalar_read_offset, "pread", 5);
    io.readable_size = scalar_read_offset + 5;
    memset(memory.bytes + first_offset, 0xa5, 5);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            67, 0, USER_BASE + first_offset, 5,
            scalar_read_offset, UINT64_MAX, 0);
    CHECK(result == 5 && io.pread_calls == 1 &&
            io.pread_requests[0] == 5 &&
            io.pread_offsets[0] == scalar_read_offset &&
            memcmp(memory.bytes + first_offset, "pread", 5) == 0,
            "pread64 按显式 offset 读取指定内容");
    CHECK(positioned_fd->offset == 1234,
            "pread64 不修改描述符顺序 offset");

    const off_t scalar_write_offset = (off_t) UINT64_C(0x20000005b);
    memcpy(memory.bytes + first_offset, "pwrite", 6);
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            68, 0, USER_BASE + first_offset, 6,
            scalar_write_offset, UINT64_C(0xfeedface), 0);
    CHECK(result == 6 && io.pwrite_calls == 1 &&
            io.pwrite_requests[0] == 6 &&
            io.pwrite_offsets[0] == scalar_write_offset &&
            io.written_size == 6 &&
            memcmp(io.written_data, "pwrite", 6) == 0,
            "pwrite64 保留完整 64 位 offset 并写出精确负载");
    CHECK(positioned_fd->offset == 1234,
            "pwrite64 不修改描述符顺序 offset");

    const size_t chunked_size = 4097;
    const off_t chunked_read_offset = 17;
    reset_io(&io);
    for (size_t index = 0; index < chunked_size; index++)
        io.readable_data[chunked_read_offset + index] =
                (byte_t) (index * 29 + 3);
    io.readable_size = chunked_read_offset + chunked_size;
    memset(memory.bytes + first_offset, 0, chunked_size);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            67, 0, USER_BASE + first_offset, chunked_size,
            chunked_read_offset, 0, 0);
    CHECK(result == chunked_size && io.pread_calls == 2 &&
            io.pread_requests[0] == 4096 && io.pread_requests[1] == 1 &&
            io.pread_offsets[0] == chunked_read_offset &&
            io.pread_offsets[1] == chunked_read_offset + 4096,
            "pread64 的 4097 字节分块逐次推进后端 offset");
    CHECK(memcmp(memory.bytes + first_offset,
                    io.readable_data + chunked_read_offset,
                    chunked_size) == 0 && positioned_fd->offset == 1234,
            "pread64 分块保持字节顺序且不改变顺序位置");

    const off_t chunked_write_offset = 23;
    for (size_t index = 0; index < chunked_size; index++)
        memory.bytes[first_offset + index] = (byte_t) (index * 31 + 11);
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            68, 0, USER_BASE + first_offset, chunked_size,
            chunked_write_offset, 0, 0);
    CHECK(result == chunked_size && io.pwrite_calls == 2 &&
            io.pwrite_requests[0] == 4096 && io.pwrite_requests[1] == 1 &&
            io.pwrite_offsets[0] == chunked_write_offset &&
            io.pwrite_offsets[1] == chunked_write_offset + 4096,
            "pwrite64 的 4097 字节分块逐次推进后端 offset");
    CHECK(io.written_size == chunked_size &&
            memcmp(io.written_data, memory.bytes + first_offset,
                    chunked_size) == 0 && positioned_fd->offset == 1234,
            "pwrite64 分块保持负载顺序且不改变顺序位置");

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 3,
    };
    vectors[1] = (struct aarch64_linux_iovec) {
        USER_BASE + second_offset, 5,
    };
    store_vectors(&memory, vector_offset, vectors, 2);
    const off_t vector_read_offset = 31;
    reset_io(&io);
    memcpy(io.readable_data + vector_read_offset, "position", 8);
    io.readable_size = vector_read_offset + 8;
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            69, 0, USER_BASE + vector_offset, 2, vector_read_offset,
            UINT64_C(0x8000000000000000), 0);
    CHECK(result == 8 && io.pread_calls == 1 &&
            io.pread_requests[0] == 8 &&
            io.pread_offsets[0] == vector_read_offset &&
            io.read_calls == 0 && positioned_fd->offset == 1234,
            "preadv 聚合为一次定位读取并忽略 x4 高半辅助值");
    CHECK(memcmp(memory.bytes + first_offset, "pos", 3) == 0 &&
            memcmp(memory.bytes + second_offset, "ition", 5) == 0,
            "preadv 把一次定位读取结果按向量顺序分散");

    memcpy(memory.bytes + first_offset, "vec", 3);
    memcpy(memory.bytes + second_offset, "write", 5);
    const off_t vector_write_offset = (off_t) UINT64_C(0x10000002f);
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            70, 0, USER_BASE + vector_offset, 2, vector_write_offset,
            UINT64_C(0xffffffffffffffff), 0);
    CHECK(result == 8 && io.pwrite_calls == 1 &&
            io.pwrite_requests[0] == 8 &&
            io.pwrite_offsets[0] == vector_write_offset &&
            io.write_calls == 0 && positioned_fd->offset == 1234,
            "pwritev 聚合为一次定位写入并忽略 x4 高半辅助值");
    CHECK(io.written_size == 8 &&
            memcmp(io.written_data, "vecwrite", 8) == 0,
            "pwritev 保持多个向量的聚合负载顺序");

    reset_io(&io);
    memcpy(io.readable_data, "__stream!", 9);
    io.readable_size = 9;
    io.read_position = 2;
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            286, 0, USER_BASE + vector_offset, 2, UINT64_MAX,
            UINT64_C(0x123456789abcdef0), 0);
    CHECK(result == 7 && io.read_calls == 1 && io.pread_calls == 0 &&
            io.read_position == 9 &&
            memcmp(memory.bytes + first_offset, "str", 3) == 0 &&
            memcmp(memory.bytes + second_offset, "eam!", 4) == 0,
            "preadv2 的 offset=-1 使用并推进顺序读取位置");

    memcpy(memory.bytes + first_offset, "seq", 3);
    memcpy(memory.bytes + second_offset, "write", 5);
    reset_io(&io);
    memcpy(io.written_data, "__", 2);
    io.written_size = 2;
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            287, 0, USER_BASE + vector_offset, 2, UINT64_MAX,
            UINT64_MAX, 0);
    CHECK(result == 8 && io.write_calls == 1 && io.pwrite_calls == 0 &&
            io.written_size == 10 &&
            memcmp(io.written_data, "__seqwrite", 10) == 0,
            "pwritev2 的 offset=-1 使用并推进顺序写入位置");

    vectors[0] = (struct aarch64_linux_iovec) {
        USER_BASE + first_offset, 4,
    };
    store_vectors(&memory, vector_offset, vectors, 1);
    reset_io(&io);
    reset_user(&memory);
    memory.fail_write_at = USER_BASE + first_offset;
    result = invoke_positioned(&fixture, &memory, &fault,
            286, 0, USER_BASE + vector_offset, 1, 5, 0, 1);
    CHECK(result == encoded_error(_EOPNOTSUPP) &&
            memory.read_calls == 1 && memory.write_calls == 0 &&
            io.read_calls == 0 && io.pread_calls == 0,
            "preadv2 的未知 flags 不读取后端也不写 guest 负载");

    reset_io(&io);
    reset_user(&memory);
    memory.fail_read_at = USER_BASE + first_offset;
    result = invoke_positioned(&fixture, &memory, &fault,
            287, 0, USER_BASE + vector_offset, 1, 5, 0, 2);
    CHECK(result == encoded_error(_EOPNOTSUPP) &&
            memory.read_calls == 1 && io.write_calls == 0 &&
            io.pwrite_calls == 0,
            "pwritev2 的未知 flags 不读取 guest 负载也不写后端");

    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            286, 0, UINT64_MAX, 0, 5, UINT64_MAX, UINT64_MAX);
    CHECK(result == 0 && memory.read_calls == 0 &&
            memory.write_calls == 0 && io.read_calls == 0 &&
            io.pread_calls == 0,
            "零向量 preadv2 即使 flags 未知也返回零且不访问 guest");
    reset_io(&io);
    reset_user(&memory);
    result = invoke_positioned(&fixture, &memory, &fault,
            287, 0, UINT64_MAX, 0, 5, UINT64_MAX, UINT64_MAX);
    CHECK(result == 0 && memory.read_calls == 0 &&
            io.write_calls == 0 && io.pwrite_calls == 0,
            "零向量 pwritev2 即使 flags 未知也返回零且不访问负载");

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

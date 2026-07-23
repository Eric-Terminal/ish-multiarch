#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE UINT32_C(0x1000)
#define HIGH_ARGUMENT UINT64_C(0x5a5a5a5a00000000)
#define AARCH64_TEST_O_DIRECTORY UINT32_C(0x004000)
#define AARCH64_TEST_O_NOFOLLOW UINT32_C(0x008000)
#define AARCH64_TEST_O_LARGEFILE UINT32_C(0x020000)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 fd 系统调用测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
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
    qword_t replace_on_write;
    fd_t replacement_number;
    fd_t other_pipe_number;
    bool replacement_installed;
    bool reinstall_same_endpoint;
    struct fd *reinstalled_endpoint;
};

struct fd_probe {
    unsigned closes;
};

struct syscall_fixture {
    struct task task;
    struct tgroup group;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
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

static bool range_contains(qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
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
            range_contains(address, size, memory->fail_read_at)) {
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
    if ((memory->replacement_fd != NULL ||
            memory->reinstall_same_endpoint) &&
            address == memory->replace_on_write &&
            size == 2 * sizeof(sdword_t)) {
        sdword_t pipe_fds[2];
        memcpy(pipe_fds, source, sizeof(pipe_fds));
        memory->replacement_number = pipe_fds[0];
        memory->other_pipe_number = pipe_fds[1];
        struct fd *replacement = memory->replacement_fd;
        if (memory->reinstall_same_endpoint)
            replacement = f_get_task_retain(
                    memory->replacement_task, pipe_fds[0]);
        memory->reinstalled_endpoint = replacement;
        int close_error = f_close_task(
                memory->replacement_task, pipe_fds[0]);
        fd_t installed = f_install_task(memory->replacement_task,
                replacement, 0);
        memory->replacement_fd = NULL;
        memory->replacement_installed = close_error == 0 &&
                installed == pipe_fds[0];
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
            range_contains(address, size, memory->fail_write_at)) {
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
    memory->replacement_task = NULL;
    memory->replacement_fd = NULL;
    memory->replace_on_write = UINT64_MAX;
    memory->replacement_number = -1;
    memory->other_pipe_number = -1;
    memory->replacement_installed = false;
    memory->reinstall_same_endpoint = false;
    memory->reinstalled_endpoint = NULL;
}

static int probe_close(struct fd *fd) {
    struct fd_probe *probe = fd->data;
    probe->closes++;
    return 0;
}

static const struct fd_ops probe_fd_ops = {
    .close = probe_close,
};

// 测试 inode 由栈持有，最后关闭时阻止通用 fd 路径替它释放所有权。
static int lock_fd_close(struct fd *fd) {
    fd->inode = NULL;
    return 0;
}

static const struct fd_ops lock_fd_ops = {
    .close = lock_fd_close,
};

static struct fd *make_probe_fd(struct fd_probe *probe) {
    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return NULL;
    fd->data = probe;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    return fd;
}

static void init_lock_inode(struct inode_data *inode) {
    memset(inode, 0, sizeof(*inode));
    list_init(&inode->posix_locks);
    cond_init(&inode->posix_unlock);
    lock_init(&inode->lock);
}

static struct fd *make_lock_fd(struct inode_data *inode) {
    struct fd *fd = fd_create(&lock_fd_ops);
    if (fd == NULL)
        return NULL;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    fd->inode = inode;
    return fd;
}

static bool init_fixture(struct syscall_fixture *fixture, rlim_t_ nofile) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {nofile, nofile};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;
    current = &fixture->task;
    return true;
}

static void destroy_fixture(struct syscall_fixture *fixture) {
    fdtable_release(fixture->task.files);
    current = NULL;
}

static qword_t invoke(struct task *task, struct user_memory *memory,
        struct guest_linux_user_fault *fault, qword_t number,
        qword_t argument0, qword_t argument1, qword_t argument2) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = task,
        .user = {
            .opaque = memory,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = number,
        .arguments = {argument0, argument1, argument2},
    };
    current = task;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static bool test_dup_and_fcntl(void) {
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture, 12), "初始化 dup/fcntl 夹具");
    struct user_memory memory = {0};
    reset_user(&memory);
    struct guest_linux_user_fault fault;
    struct fd_probe source_probe = {0};
    struct fd *source = make_probe_fd(&source_probe);
    CHECK(source != NULL && f_install_task(&fixture.task, source, 0) == 0,
            "安装源描述符");

    qword_t result = invoke(&fixture.task, &memory, &fault, 23,
            HIGH_ARGUMENT, UINT64_MAX, UINT64_MAX);
    CHECK(result == 1 && f_get_task(&fixture.task, 1) == source &&
            f_getfd_task(&fixture.task, 1) == 0,
            "dup 忽略高位并创建未标记副本");

    result = invoke(&fixture.task, &memory, &fault, 24,
            HIGH_ARGUMENT, HIGH_ARGUMENT | 5,
            HIGH_ARGUMENT | O_CLOEXEC_);
    CHECK(result == 5 && f_get_task(&fixture.task, 5) == source &&
            f_getfd_task(&fixture.task, 5) == FD_CLOEXEC_,
            "dup3 解码低位并设置 CLOEXEC");

    result = invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT | 5, HIGH_ARGUMENT | F_GETFD_, UINT64_MAX);
    CHECK(result == FD_CLOEXEC_, "fcntl F_GETFD 忽略参数高位");
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT | 5, HIGH_ARGUMENT | F_SETFD_, HIGH_ARGUMENT) == 0 &&
            f_getfd_task(&fixture.task, 5) == 0,
            "fcntl F_SETFD 清除 descriptor flag");

    dword_t status_flags = O_APPEND_ | O_NONBLOCK_;
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT | 5, HIGH_ARGUMENT | F_SETFL_,
            HIGH_ARGUMENT | status_flags) == 0,
            "fcntl F_SETFL 接受低位状态 flags");
    result = invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETFL_, UINT64_MAX);
    CHECK(result == (O_RDWR_ | status_flags),
            "重复描述符共享文件状态 flags");

    source->flags |= O_LARGEFILE_;
    result = invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETFL_, UINT64_MAX);
    CHECK(result == (O_RDWR_ | status_flags | AARCH64_TEST_O_LARGEFILE),
            "F_GETFL 独立转换 AArch64 LARGEFILE 位");
    source->flags &= ~O_LARGEFILE_;
    source->flags |= O_DIRECTORY_;
    result = invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETFL_, UINT64_MAX);
    CHECK(result == (O_RDWR_ | status_flags | AARCH64_TEST_O_DIRECTORY),
            "F_GETFL 独立转换 AArch64 DIRECTORY 位");
    source->flags &= ~O_DIRECTORY_;
    source->flags |= O_NOFOLLOW_;
    result = invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETFL_, UINT64_MAX);
    CHECK(result == (O_RDWR_ | status_flags | AARCH64_TEST_O_NOFOLLOW),
            "F_GETFL 独立转换 AArch64 NOFOLLOW 位");
    source->flags &= ~(O_LARGEFILE_ | O_DIRECTORY_ | O_NOFOLLOW_);

    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_DUPFD_,
            HIGH_ARGUMENT | 3) == 3 &&
            f_get_task(&fixture.task, 3) == source &&
            f_getfd_task(&fixture.task, 3) == 0,
            "F_DUPFD 从指定下界创建未标记副本");
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_DUPFD_CLOEXEC_,
            HIGH_ARGUMENT | 7) == 7 &&
            f_get_task(&fixture.task, 7) == source &&
            f_getfd_task(&fixture.task, 7) == FD_CLOEXEC_,
            "F_DUPFD_CLOEXEC 创建带标记副本");

    CHECK(invoke(&fixture.task, &memory, &fault, 24,
            HIGH_ARGUMENT, HIGH_ARGUMENT, 0) == encoded_error(_EINVAL),
            "dup3 拒绝相同描述符");
    CHECK(invoke(&fixture.task, &memory, &fault, 24,
            HIGH_ARGUMENT, HIGH_ARGUMENT | 4,
            O_NONBLOCK_) == encoded_error(_EINVAL),
            "dup3 拒绝非 CLOEXEC flags");
    CHECK(invoke(&fixture.task, &memory, &fault, 24,
            HIGH_ARGUMENT | 99, HIGH_ARGUMENT | 4, 0) ==
            encoded_error(_EBADF), "dup3 拒绝无效源描述符");
    CHECK(invoke(&fixture.task, &memory, &fault, 24,
            HIGH_ARGUMENT, HIGH_ARGUMENT | UINT32_MAX, 0) ==
            encoded_error(_EBADF), "dup3 拒绝负目标描述符");
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_DUPFD_,
            HIGH_ARGUMENT | UINT32_MAX) == encoded_error(_EINVAL),
            "F_DUPFD 拒绝负下界");
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | UINT32_C(0x7fffffff), 0) ==
            encoded_error(_EINVAL), "fcntl 拒绝未知命令");
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT | 99, HIGH_ARGUMENT | UINT32_C(0x7fffffff), 0) ==
            encoded_error(_EBADF),
            "fcntl 对未知命令仍优先拒绝无效描述符");

    struct task child = {
        .group = &fixture.group,
        .files = fdtable_copy(fixture.task.files),
    };
    CHECK(!IS_ERR(child.files), "复制独立子进程 fdtable");
    CHECK(invoke(&child, &memory, &fault, 25, HIGH_ARGUMENT,
            HIGH_ARGUMENT | F_SETFD_, FD_CLOEXEC_) == 0 &&
            f_getfd_task(&child, 0) == FD_CLOEXEC_ &&
            f_getfd_task(&fixture.task, 0) == 0,
            "descriptor flag 隔离在子进程表内");
    CHECK(invoke(&child, &memory, &fault, 24, HIGH_ARGUMENT,
            HIGH_ARGUMENT | 10, HIGH_ARGUMENT | O_CLOEXEC_) == 10 &&
            f_get_task(&child, 10) == source &&
            f_get_task(&fixture.task, 10) == NULL,
            "子进程 dup3 不修改父进程表");
    CHECK(invoke(&child, &memory, &fault, 25, HIGH_ARGUMENT,
            HIGH_ARGUMENT | F_SETFL_, O_APPEND_) == 0 &&
            f_getfl_task(&fixture.task, 0) == (O_RDWR_ | O_APPEND_),
            "父子共享 open-file description 状态");
    fdtable_do_cloexec(child.files);
    CHECK(f_get_task(&child, 0) == NULL &&
            f_get_task(&child, 1) == source &&
            f_get_task(&child, 10) == NULL &&
            f_get_task(&fixture.task, 0) == source,
            "close-on-exec 只清理子进程带标记条目");
    fdtable_release(child.files);
    current = &fixture.task;

    destroy_fixture(&fixture);
    return true;
}

static bool test_fcntl_record_locks(void) {
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 fcntl 记录锁夹具");
    fixture.task.pid = 42;
    fixture.task.tgid = 42;
    struct user_memory memory = {0};
    reset_user(&memory);
    struct guest_linux_user_fault fault;
    struct inode_data inode;
    init_lock_inode(&inode);
    struct fd *file = make_lock_fd(&inode);
    CHECK(file != NULL && f_install_task(&fixture.task, file, 0) == 0,
            "安装带 inode 的记录锁文件");

    const size_t flock_offset = 3;
    const qword_t flock_address = USER_BASE + flock_offset;
    const sqword_t lock_start = INT64_C(0x100000005);
    struct aarch64_linux_flock wire = {
        .type = F_WRLCK_,
        .whence = LSEEK_SET,
        .padding = UINT32_C(0xa1b2c3d4),
        .start = lock_start,
        .len = 7,
        .pid = -1,
        .tail_padding = UINT32_C(0x55667788),
    };
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    qword_t result = invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_SETLK_, flock_address);
    CHECK(result == 0 && memory.read_calls == 1 &&
            memory.max_read_size == sizeof(wire) &&
            memory.write_calls == 0 && list_size(&inode.posix_locks) == 1,
            "F_SETLK 读取 32 字节 LP64 wire 并建立记录锁");
    struct file_lock *stored = list_first_entry(
            &inode.posix_locks, struct file_lock, locks);
    CHECK(stored->type == F_WRLCK_ && stored->start == lock_start &&
            stored->end == lock_start + 6 && stored->pid == 42 &&
            stored->owner == fixture.task.files,
            "F_SETLK 保留 64 位区间并以调用进程为 owner");

    struct task child = {
        .pid = 84,
        .tgid = 84,
        .group = &fixture.group,
        .files = fdtable_copy(fixture.task.files),
    };
    CHECK(!IS_ERR(child.files), "复制独立记录锁 owner");

    wire = (struct aarch64_linux_flock) {
        .type = F_RDLCK_,
        .whence = LSEEK_SET,
        .padding = UINT32_C(0xa1b2c3d4),
        .start = lock_start,
        .len = 7,
        .pid = -1,
        .tail_padding = UINT32_C(0x55667788),
    };
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    result = invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETLK_, flock_address);
    memcpy(&wire, memory.bytes + flock_offset, sizeof(wire));
    CHECK(result == 0 && memory.read_calls == 1 &&
            memory.write_calls == 1 &&
            memory.max_read_size == sizeof(wire) &&
            memory.max_write_size == sizeof(wire) &&
            wire.type == F_WRLCK_ && wire.whence == LSEEK_SET &&
            wire.start == lock_start && wire.len == 7 && wire.pid == 42,
            "F_GETLK 向另一 owner 写回冲突锁的完整 64 位字段");
    CHECK(wire.padding == UINT32_C(0xa1b2c3d4) &&
            wire.tail_padding == UINT32_C(0x55667788),
            "F_GETLK 保留 Linux copy_from_user 带入的填充字节");

    wire.type = F_WRLCK_;
    wire.pid = -1;
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    CHECK(invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_SETLK_, flock_address) ==
            encoded_error(_EAGAIN),
            "F_SETLK 把不同 owner 的冲突返回 EAGAIN");

    wire.start = lock_start + 0x100;
    wire.len = 0;
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    CHECK(invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_SETLKW_, flock_address) == 0 &&
            list_size(&inode.posix_locks) == 2,
            "F_SETLKW 非冲突路径复用阻塞锁引擎");

    wire.start = lock_start;
    wire.len = 7;
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    memory.fail_read_at = flock_address + 8;
    result = invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_SETLK_, flock_address);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            memory.write_calls == 0 && fault.address == flock_address + 8 &&
            fault.access == GUEST_MEMORY_READ,
            "F_SETLK 传递 guest copyin 页故障");

    reset_user(&memory);
    memory.fail_write_at = flock_address + 24;
    result = invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETLK_, flock_address);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            memory.write_calls == 1 && fault.address == flock_address + 24 &&
            fault.access == GUEST_MEMORY_WRITE,
            "F_GETLK 传递 guest copyout 页故障");

    reset_user(&memory);
    qword_t crossing_address =
            AARCH64_LINUX_USER_ADDRESS_MAX - UINT64_C(15);
    result = invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_SETLK_, crossing_address);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 0 &&
            memory.write_calls == 0 && fault.address == crossing_address &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "F_SETLK 在 copyin 前拒绝跨越 AArch64 用户上限的 wire");

    reset_user(&memory);
    result = invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT | 99, HIGH_ARGUMENT | F_SETLK_,
            crossing_address);
    CHECK(result == encoded_error(_EBADF) && memory.read_calls == 0 &&
            memory.write_calls == 0,
            "记录锁命令依 Linux fcntl 顺序优先返回 EBADF");

    const dword_t unsupported_commands[] = {
        F_GETLK64_, F_SETLK64_, F_SETLKW64_,
    };
    for (size_t index = 0; index < array_size(unsupported_commands); index++) {
        reset_user(&memory);
        CHECK(invoke(&child, &memory, &fault, 25,
                HIGH_ARGUMENT, HIGH_ARGUMENT | unsupported_commands[index],
                flock_address) == encoded_error(_EINVAL) &&
                memory.read_calls == 0 && memory.write_calls == 0,
                "AArch64 64 位 ABI 拒绝仅供兼容进程使用的 flock64 命令");
    }

    struct fd_probe no_inode_probe = {0};
    struct fd *no_inode = make_probe_fd(&no_inode_probe);
    CHECK(no_inode != NULL &&
            f_install_task(&child, no_inode, 0) == 1,
            "安装没有 inode 的有效描述符");
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    CHECK(invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT | 1, HIGH_ARGUMENT | F_SETLK_,
            flock_address) == encoded_error(_EBADF) &&
            memory.read_calls == 1 && memory.write_calls == 0,
            "记录锁拒绝缺少 inode 的 fd 而不解引用空指针");

    wire.type = F_UNLCK_;
    wire.start = lock_start;
    wire.len = 7;
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    CHECK(invoke(&fixture.task, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_SETLK_, flock_address) == 0 &&
            list_size(&inode.posix_locks) == 1,
            "F_UNLCK 通过相同 LP64 wire 解除父 owner 的区间");

    wire = (struct aarch64_linux_flock) {
        .type = F_WRLCK_,
        .whence = LSEEK_CUR,
        .padding = UINT32_C(0xa1b2c3d4),
        .start = lock_start,
        .len = 7,
        .pid = -123,
        .tail_padding = UINT32_C(0x55667788),
    };
    struct aarch64_linux_flock expected_unlocked = wire;
    memcpy(memory.bytes + flock_offset, &wire, sizeof(wire));
    reset_user(&memory);
    CHECK(invoke(&child, &memory, &fault, 25,
            HIGH_ARGUMENT, HIGH_ARGUMENT | F_GETLK_, flock_address) == 0,
            "F_GETLK 无冲突查询成功");
    memcpy(&wire, memory.bytes + flock_offset, sizeof(wire));
    expected_unlocked.type = F_UNLCK_;
    CHECK(memcmp(&wire, &expected_unlocked, sizeof(wire)) == 0,
            "F_GETLK 无冲突时只把 type 改为 UNLCK");

    fdtable_release(child.files);
    current = &fixture.task;
    CHECK(list_empty(&inode.posix_locks),
            "关闭子 fdtable 清理剩余记录锁");
    destroy_fixture(&fixture);
    return true;
}

static bool test_pipe_success(void) {
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 pipe2 成功夹具");
    struct user_memory memory = {0};
    reset_user(&memory);
    struct guest_linux_user_fault fault;
    const size_t pipe_offset = 3;
    qword_t result = invoke(&fixture.task, &memory, &fault, 59,
            USER_BASE + pipe_offset,
            HIGH_ARGUMENT | O_CLOEXEC_ | O_NONBLOCK_, UINT64_MAX);
    sdword_t pipe_fds[2];
    memcpy(pipe_fds, memory.bytes + pipe_offset, sizeof(pipe_fds));
    CHECK(result == 0 && memory.write_calls == 1 &&
            memory.max_write_size == sizeof(pipe_fds) &&
            pipe_fds[0] == 0 && pipe_fds[1] == 1,
            "pipe2 向未对齐高位地址写回两个 32 位 fd");
    CHECK(f_getfd_task(&fixture.task, pipe_fds[0]) == FD_CLOEXEC_ &&
            f_getfd_task(&fixture.task, pipe_fds[1]) == FD_CLOEXEC_ &&
            (f_getfl_task(&fixture.task, pipe_fds[0]) & O_NONBLOCK_) != 0 &&
            (f_getfl_task(&fixture.task, pipe_fds[1]) & O_NONBLOCK_) != 0,
            "pipe2 同时应用 CLOEXEC 与 NONBLOCK");
    CHECK(f_get_task(&fixture.task, pipe_fds[0])->refcount == 1 &&
            f_get_task(&fixture.task, pipe_fds[1])->refcount == 1,
            "guest 写回成功后只保留 fdtable 对两端的引用");

    const size_t input_offset = 0x100;
    const size_t output_offset = 0x200;
    memcpy(memory.bytes + input_offset, "pipe", 4);
    reset_user(&memory);
    CHECK(invoke(&fixture.task, &memory, &fault, 64,
            HIGH_ARGUMENT | (dword_t) pipe_fds[1],
            USER_BASE + input_offset, 4) == 4,
            "pipe2 写端接收真实文件写入");
    reset_user(&memory);
    CHECK(invoke(&fixture.task, &memory, &fault, 63,
            HIGH_ARGUMENT | (dword_t) pipe_fds[0],
            USER_BASE + output_offset, 4) == 4 &&
            memcmp(memory.bytes + output_offset, "pipe", 4) == 0,
            "pipe2 读端返回完整数据");

    reset_user(&memory);
    CHECK(invoke(&fixture.task, &memory, &fault, 59,
            USER_BASE + pipe_offset, HIGH_ARGUMENT | O_APPEND_, 0) ==
            encoded_error(_EINVAL) && memory.write_calls == 0 &&
            f_get_task(&fixture.task, 2) == NULL,
            "pipe2 拒绝无效 flags 且无副作用");

    destroy_fixture(&fixture);
    return true;
}

static bool test_pipe_rollbacks(void) {
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture, 8), "初始化 pipe2 EFAULT 夹具");
    struct user_memory memory = {0};
    reset_user(&memory);
    struct guest_linux_user_fault fault;
    const size_t pipe_offset = 5;
    struct fd_probe sentinel_probe = {0};
    struct fd *sentinel = make_probe_fd(&sentinel_probe);
    CHECK(sentinel != NULL, "创建 EFAULT fd 复用 sentinel");
    memory.replacement_task = &fixture.task;
    memory.replacement_fd = sentinel;
    memory.replace_on_write = USER_BASE + pipe_offset;
    memory.fail_write_at = USER_BASE + pipe_offset + sizeof(sdword_t);
    qword_t result = invoke(&fixture.task, &memory, &fault, 59,
            USER_BASE + pipe_offset, O_CLOEXEC_, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 1 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE &&
            memory.replacement_installed &&
            memory.replacement_number == 0 &&
            memory.other_pipe_number == 1 &&
            f_get_task(&fixture.task, 0) == sentinel &&
            f_get_task(&fixture.task, 1) == NULL &&
            sentinel_probe.closes == 0,
            "pipe2 EFAULT 只回滚原 endpoint 并保留同号 sentinel");

    reset_user(&memory);
    qword_t crossing_address =
            AARCH64_LINUX_USER_ADDRESS_MAX - UINT64_C(3);
    result = invoke(&fixture.task, &memory, &fault, 59,
            crossing_address, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            fault.address == crossing_address &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE &&
            f_get_task(&fixture.task, 1) == NULL &&
            f_get_task(&fixture.task, 2) == NULL,
            "pipe2 跨越 AArch64 用户地址上限时不写 guest 并回滚两端");
    struct fd_probe reuse_probe = {0};
    struct fd *reuse = make_probe_fd(&reuse_probe);
    CHECK(reuse != NULL && f_install_task(&fixture.task, reuse, 0) == 1,
            "EFAULT 后另一原 endpoint 槽可立即复用");
    destroy_fixture(&fixture);

    CHECK(init_fixture(&fixture, 8), "初始化 pipe2 同对象 ABA 夹具");
    memset(&memory, 0, sizeof(memory));
    reset_user(&memory);
    memory.replacement_task = &fixture.task;
    memory.replace_on_write = USER_BASE + pipe_offset;
    memory.reinstall_same_endpoint = true;
    memory.fail_write_at = USER_BASE + pipe_offset + sizeof(sdword_t);
    result = invoke(&fixture.task, &memory, &fault, 59,
            USER_BASE + pipe_offset, 0, 0);
    CHECK(result == encoded_error(_EFAULT) &&
            memory.replacement_installed &&
            f_get_task(&fixture.task, 0) ==
                    memory.reinstalled_endpoint &&
            f_get_task(&fixture.task, 1) == NULL,
            "安装代数阻止 EFAULT 回滚误关同对象 ABA 新表项");
    destroy_fixture(&fixture);

    CHECK(init_fixture(&fixture, 1), "初始化 pipe2 RLIMIT 夹具");
    memset(&memory, 0, sizeof(memory));
    reset_user(&memory);
    result = invoke(&fixture.task, &memory, &fault, 59,
            USER_BASE + pipe_offset, O_CLOEXEC_, 0);
    CHECK(result == encoded_error(_EMFILE) && memory.write_calls == 0 &&
            f_get_task(&fixture.task, 0) == NULL,
            "pipe2 第二端安装失败回滚第一端");
    struct fd_probe limit_probe = {0};
    struct fd *limit_reuse = make_probe_fd(&limit_probe);
    CHECK(limit_reuse != NULL &&
            f_install_task(&fixture.task, limit_reuse, 0) == 0,
            "RLIMIT 回滚后唯一 fd 槽可复用");
    destroy_fixture(&fixture);
    return true;
}

int main(void) {
    if (!test_dup_and_fcntl() ||
            !test_fcntl_record_locks() ||
            !test_pipe_success() ||
            !test_pipe_rollbacks())
        return 1;
    return 0;
}

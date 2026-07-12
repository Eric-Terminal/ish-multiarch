#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define USER_MEMORY_SIZE UINT32_C(0x10000)
#define IO_DATA_SIZE 8192
#define USER_ACCESS_LOG_SIZE 16

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 系统调用服务测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

_Static_assert(sizeof(guest_addr_t) == 4,
        "生产系统调用服务测试必须位于 i386 内核类型域");

struct user_access {
    qword_t address;
    dword_t size;
    dword_t access;
};

struct user_memory {
    byte_t bytes[USER_MEMORY_SIZE];
    qword_t fail_read_at;
    qword_t fail_write_at;
    unsigned read_calls;
    unsigned write_calls;
    qword_t read_bytes;
    qword_t write_bytes;
    dword_t max_read_size;
    dword_t max_write_size;
    struct user_access accesses[USER_ACCESS_LOG_SIZE];
    unsigned access_count;
    struct task *observe_task;
    qword_t observe_read_at;
    qword_t observe_write_at;
    sigset_t_ observed_blocked;
    sigset_t_ observed_write_blocked;
    bool observed_has_saved_mask;
    bool observed_write_has_saved_mask;
    bool observed_read;
    bool observed_write;
};

struct io_probe {
    byte_t read_data[IO_DATA_SIZE];
    size_t read_size;
    size_t read_offset;
    byte_t written_data[IO_DATA_SIZE];
    size_t written_size;
    size_t max_read;
    size_t max_write;
    unsigned read_calls;
    unsigned write_calls;
    size_t read_requests[4];
    size_t write_requests[4];
    unsigned read_error_call;
    unsigned write_error_call;
    int read_error;
    int write_error;
    int poll_events;
};

struct kernel_probe {
    char last_path[MAX_PATH];
    char last_stat_path[MAX_PATH];
    char last_readlink_path[MAX_PATH];
    char last_unlink_path[MAX_PATH];
    char last_rmdir_path[MAX_PATH];
    char last_mkdir_path[MAX_PATH];
    char last_rename_source[MAX_PATH];
    char last_rename_destination[MAX_PATH];
    int last_flags;
    int last_mode;
    int last_mkdir_mode;
    unsigned opens;
    unsigned opened_closes;
    unsigned stat_calls;
    unsigned fstat_calls;
    unsigned readlink_calls;
    unsigned unlink_calls;
    unsigned rmdir_calls;
    unsigned mkdir_calls;
    unsigned rename_calls;
    unsigned ioctl_calls;
    dword_t last_ioctl_command;
    dword_t ioctl_input;
    dword_t ioctl_output;
    dword_t ioctl_scalar;
    int ioctl_result;
    struct statbuf stat;
};

struct fd_state {
    struct kernel_probe *kernel;
    struct io_probe *io;
    const char *path;
    bool allocated;
};

struct task_fixture {
    struct task task;
    struct tgroup group;
    struct fs_info fs;
    struct guest_linux_syscall_completion completion;
    struct mount *mount;
    struct fd *pwd;
    struct fd *root;
    struct fd_state pwd_state;
    struct fd_state root_state;
    struct fd_state io_state;
};

static qword_t encoded_error(int error) {
    return (qword_t) (sqword_t) error;
}

static void reset_user_access(struct user_memory *memory) {
    memory->fail_read_at = UINT64_MAX;
    memory->fail_write_at = UINT64_MAX;
    memory->read_calls = 0;
    memory->write_calls = 0;
    memory->read_bytes = 0;
    memory->write_bytes = 0;
    memory->max_read_size = 0;
    memory->max_write_size = 0;
    memory->access_count = 0;
    memory->observe_task = NULL;
    memory->observed_read = false;
    memory->observed_write = false;
}

static void record_user_access(struct user_memory *memory,
        qword_t address, dword_t size,
        enum guest_memory_access access) {
    if (memory->access_count < array_size(memory->accesses)) {
        memory->accesses[memory->access_count] =
                (struct user_access) {
                    .address = address,
                    .size = size,
                    .access = (dword_t) access,
                };
    }
    memory->access_count++;
}

static bool user_range(const struct user_memory *memory,
        qword_t address, dword_t size, size_t *offset) {
    (void) memory;
    if (address < USER_BASE)
        return false;
    qword_t relative = address - USER_BASE;
    if (relative > USER_MEMORY_SIZE ||
            size > USER_MEMORY_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
}

static bool range_contains(qword_t address, dword_t size, qword_t target) {
    return target >= address && target - address < size;
}

static void set_user_fault(struct guest_linux_user_fault *fault,
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
    record_user_access(memory, address, size, GUEST_MEMORY_READ);
    memory->read_calls++;
    if (memory->observe_task != NULL &&
            range_contains(address, size, memory->observe_read_at)) {
        lock(&memory->observe_task->sighand->lock);
        memory->observed_blocked = memory->observe_task->blocked;
        memory->observed_has_saved_mask =
                memory->observe_task->has_saved_mask;
        unlock(&memory->observe_task->sighand->lock);
        memory->observed_read = true;
    }
    if (size > memory->max_read_size)
        memory->max_read_size = size;
    size_t offset;
    if (!user_range(memory, address, size, &offset)) {
        set_user_fault(fault, address, GUEST_MEMORY_READ,
                GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        return false;
    }
    if (memory->fail_read_at != UINT64_MAX &&
            range_contains(address, size, memory->fail_read_at)) {
        set_user_fault(fault, memory->fail_read_at, GUEST_MEMORY_READ,
                GUEST_MEMORY_FAULT_UNMAPPED);
        return false;
    }
    memcpy(destination, memory->bytes + offset, size);
    memory->read_bytes += size;
    set_user_fault(fault, address, GUEST_MEMORY_READ,
            GUEST_MEMORY_FAULT_NONE);
    return true;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    record_user_access(memory, address, size, GUEST_MEMORY_WRITE);
    memory->write_calls++;
    if (memory->observe_task != NULL &&
            range_contains(address, size, memory->observe_write_at)) {
        lock(&memory->observe_task->sighand->lock);
        memory->observed_write_blocked = memory->observe_task->blocked;
        memory->observed_write_has_saved_mask =
                memory->observe_task->has_saved_mask;
        unlock(&memory->observe_task->sighand->lock);
        memory->observed_write = true;
    }
    if (size > memory->max_write_size)
        memory->max_write_size = size;
    size_t offset;
    if (!user_range(memory, address, size, &offset)) {
        set_user_fault(fault, address, GUEST_MEMORY_WRITE,
                GUEST_MEMORY_FAULT_ADDRESS_SIZE);
        return false;
    }
    if (memory->fail_write_at != UINT64_MAX &&
            range_contains(address, size, memory->fail_write_at)) {
        size_t prefix = (size_t) (memory->fail_write_at - address);
        memcpy(memory->bytes + offset, source, prefix);
        memory->write_bytes += prefix;
        set_user_fault(fault, memory->fail_write_at, GUEST_MEMORY_WRITE,
                GUEST_MEMORY_FAULT_UNMAPPED);
        return false;
    }
    memcpy(memory->bytes + offset, source, size);
    memory->write_bytes += size;
    set_user_fault(fault, address, GUEST_MEMORY_WRITE,
            GUEST_MEMORY_FAULT_NONE);
    return true;
}

static void reset_io(struct io_probe *io) {
    io->read_offset = 0;
    io->written_size = 0;
    io->max_read = 0;
    io->max_write = 0;
    io->read_calls = 0;
    io->write_calls = 0;
    memset(io->read_requests, 0, sizeof(io->read_requests));
    memset(io->write_requests, 0, sizeof(io->write_requests));
    io->read_error_call = 0;
    io->write_error_call = 0;
    io->read_error = 0;
    io->write_error = 0;
}

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    struct fd_state *state = fd->data;
    struct io_probe *io = state->io;
    io->read_calls++;
    if (io->read_calls <= array_size(io->read_requests))
        io->read_requests[io->read_calls - 1] = size;
    if (io->read_error_call == io->read_calls)
        return io->read_error;
    size_t available = io->read_size - io->read_offset;
    size_t copied = size < available ? size : available;
    if (io->max_read != 0 && copied > io->max_read)
        copied = io->max_read;
    memcpy(buffer, io->read_data + io->read_offset, copied);
    io->read_offset += copied;
    return (ssize_t) copied;
}

static ssize_t probe_write(struct fd *fd, const void *buffer, size_t size) {
    struct fd_state *state = fd->data;
    struct io_probe *io = state->io;
    io->write_calls++;
    if (io->write_calls <= array_size(io->write_requests))
        io->write_requests[io->write_calls - 1] = size;
    if (io->write_error_call == io->write_calls)
        return io->write_error;
    size_t copied = size;
    if (io->max_write != 0 && copied > io->max_write)
        copied = io->max_write;
    if (copied > sizeof(io->written_data) - io->written_size)
        copied = sizeof(io->written_data) - io->written_size;
    memcpy(io->written_data + io->written_size, buffer, copied);
    io->written_size += copied;
    return (ssize_t) copied;
}

static int probe_poll(struct fd *fd) {
    struct fd_state *state = fd->data;
    return state->io->poll_events;
}

static int probe_close(struct fd *fd) {
    struct fd_state *state = fd->data;
    if (state->allocated) {
        state->kernel->opened_closes++;
        free((void *) state->path);
        free(state);
    }
    return 0;
}

static ssize_t probe_ioctl_size(int command) {
    switch ((dword_t) command) {
        case FIONREAD_:
        case TIOCSPTLCK_:
        case TIOCGPTN_:
            return sizeof(dword_t);
        case TCFLSH_:
            return 0;
    }
    return _ENOTTY;
}

static int probe_ioctl(struct fd *fd, int command, void *argument) {
    struct fd_state *state = fd->data;
    struct kernel_probe *kernel = state->kernel;
    kernel->ioctl_calls++;
    kernel->last_ioctl_command = (dword_t) command;
    if (kernel->ioctl_result < 0)
        return kernel->ioctl_result;
    switch ((dword_t) command) {
        case TIOCSPTLCK_:
            kernel->ioctl_input = *(const dword_t *) argument;
            return 0;
        case FIONREAD_:
        case TIOCGPTN_:
            *(dword_t *) argument = kernel->ioctl_output;
            return 0;
        case TCFLSH_:
            kernel->ioctl_scalar = (dword_t) (uintptr_t) argument;
            return 0;
    }
    return _ENOTTY;
}

static const struct fd_ops probe_fd_ops = {
    .read = probe_read,
    .write = probe_write,
    .poll = probe_poll,
    .ioctl_size = probe_ioctl_size,
    .ioctl = probe_ioctl,
    .close = probe_close,
};

static struct fd *probe_open(struct mount *mount,
        const char *path, int flags, int mode) {
    struct kernel_probe *probe = mount->data;
    probe->opens++;
    strcpy(probe->last_path, path);
    probe->last_flags = flags;
    probe->last_mode = mode;
    if (strcmp(path, "/work/missing") == 0)
        return ERR_PTR(_ENOENT);

    struct fd_state *state = malloc(sizeof(*state));
    if (state == NULL)
        return ERR_PTR(_ENOMEM);
    char *saved_path = strdup(path);
    if (saved_path == NULL) {
        free(state);
        return ERR_PTR(_ENOMEM);
    }
    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL) {
        free(saved_path);
        free(state);
        return ERR_PTR(_ENOMEM);
    }
    *state = (struct fd_state) {
        .kernel = probe,
        .path = saved_path,
        .allocated = true,
    };
    fd->data = state;
    return fd;
}

static int probe_stat(struct mount *mount,
        const char *path, struct statbuf *stat) {
    struct kernel_probe *probe = mount->data;
    probe->stat_calls++;
    strcpy(probe->last_stat_path, path);
    if (strcmp(path, "/work/metadata") == 0 ||
            strcmp(path, "/work/link") == 0) {
        *stat = probe->stat;
    } else {
        *stat = (struct statbuf) {
            .inode = 1,
            .mode = S_IFDIR | 0777,
            .uid = probe->stat.uid,
            .gid = probe->stat.gid,
        };
    }
    return 0;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct fd_state *state = fd->data;
    state->kernel->fstat_calls++;
    *stat = state->kernel->stat;
    return 0;
}

static ssize_t probe_readlink(struct mount *mount,
        const char *path, char *buffer, size_t size) {
    struct kernel_probe *probe = mount->data;
    probe->readlink_calls++;
    strcpy(probe->last_readlink_path, path);
    if (strcmp(path, "/work/link") != 0)
        return _EINVAL;
    static const char target[] = "metadata";
    size_t length = sizeof(target) - 1;
    if (length > size)
        length = size;
    memcpy(buffer, target, length);
    return (ssize_t) length;
}

static int probe_unlink(struct mount *mount, const char *path) {
    struct kernel_probe *probe = mount->data;
    probe->unlink_calls++;
    strcpy(probe->last_unlink_path, path);
    return 0;
}

static int probe_rmdir(struct mount *mount, const char *path) {
    struct kernel_probe *probe = mount->data;
    probe->rmdir_calls++;
    strcpy(probe->last_rmdir_path, path);
    return 0;
}

static int probe_mkdir(
        struct mount *mount, const char *path, mode_t_ mode) {
    struct kernel_probe *probe = mount->data;
    probe->mkdir_calls++;
    strcpy(probe->last_mkdir_path, path);
    probe->last_mkdir_mode = mode;
    return 0;
}

static int probe_rename(struct mount *mount,
        const char *source, const char *destination) {
    struct kernel_probe *probe = mount->data;
    probe->rename_calls++;
    strcpy(probe->last_rename_source, source);
    strcpy(probe->last_rename_destination, destination);
    return 0;
}

static int probe_getpath(struct fd *fd, char *buffer) {
    struct fd_state *state = fd->data;
    strcpy(buffer, state->path);
    return 0;
}

static const struct fs_ops probe_fs = {
    .open = probe_open,
    .readlink = probe_readlink,
    .unlink = probe_unlink,
    .rmdir = probe_rmdir,
    .mkdir = probe_mkdir,
    .rename = probe_rename,
    .stat = probe_stat,
    .fstat = probe_fstat,
    .getpath = probe_getpath,
};

static struct fd *make_fixture_fd(struct task_fixture *fixture,
        struct fd_state *state, struct kernel_probe *kernel,
        struct io_probe *io, const char *path, mode_t_ type) {
    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return NULL;
    *state = (struct fd_state) {
        .kernel = kernel,
        .io = io,
        .path = path,
    };
    fd->data = state;
    fd->type = type;
    fd->mount = fixture->mount;
    mount_retain(fixture->mount);
    return fd;
}

static bool init_fixture(struct task_fixture *fixture,
        struct kernel_probe *kernel, struct io_probe *io) {
    memset(fixture, 0, sizeof(*fixture));
    lock(&mounts_lock);
    int error = do_mount(&probe_fs, "", "", "", 0);
    unlock(&mounts_lock);
    if (error < 0)
        return false;
    char root_path[] = "/";
    fixture->mount = mount_find(root_path);
    fixture->mount->data = kernel;

    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {8, 8};
    fixture->task.group = &fixture->group;
    fixture->task.sighand = sighand_new();
    if (fixture->task.sighand == NULL)
        return false;
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->task.waiting_poll_notify_fd = -1;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;
    lock_init(&fixture->fs.lock);
    fixture->fs.umask = 0027;
    fixture->task.fs = &fixture->fs;
    fixture->task.euid = kernel->stat.uid;
    fixture->task.egid = kernel->stat.gid;

    fixture->pwd = make_fixture_fd(fixture, &fixture->pwd_state,
            kernel, NULL, "/work", S_IFDIR);
    fixture->root = make_fixture_fd(fixture, &fixture->root_state,
            kernel, NULL, "/", S_IFDIR);
    struct fd *io_fd = make_fixture_fd(fixture, &fixture->io_state,
            kernel, io, NULL, S_IFREG);
    if (fixture->pwd == NULL || fixture->root == NULL || io_fd == NULL)
        return false;
    fixture->fs.pwd = fixture->pwd;
    fixture->fs.root = fixture->root;
    if (f_install_task(&fixture->task, io_fd, 0) != 0)
        return false;
    current = &fixture->task;
    return true;
}

static int destroy_fixture(struct task_fixture *fixture) {
    current = &fixture->task;
    fdtable_release(fixture->task.files);
    fd_close(fixture->fs.pwd);
    fd_close(fixture->root);
    sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    current = NULL;
    mount_release(fixture->mount);
    lock(&mounts_lock);
    int error = mount_remove(fixture->mount);
    unlock(&mounts_lock);
    return error;
}

static qword_t invoke_arguments(struct task_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, const qword_t arguments[6]) {
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
    struct guest_linux_syscall syscall = {
        .number = number,
    };
    memcpy(syscall.arguments, arguments, sizeof(syscall.arguments));
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static qword_t invoke(struct task_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, qword_t argument0, qword_t argument1,
        qword_t argument2, qword_t argument3) {
    const qword_t arguments[6] = {
        argument0, argument1, argument2, argument3, 0, 0,
    };
    return invoke_arguments(fixture, memory, fault, number, arguments);
}

static qword_t invoke_five(struct task_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, qword_t argument0, qword_t argument1,
        qword_t argument2, qword_t argument3, qword_t argument4) {
    const qword_t arguments[6] = {
        argument0, argument1, argument2, argument3, argument4, 0,
    };
    return invoke_arguments(fixture, memory, fault, number, arguments);
}

static qword_t invoke_six(struct task_fixture *fixture,
        struct user_memory *memory, struct guest_linux_user_fault *fault,
        qword_t number, qword_t argument0, qword_t argument1,
        qword_t argument2, qword_t argument3, qword_t argument4,
        qword_t argument5) {
    const qword_t arguments[6] = {
        argument0, argument1, argument2,
        argument3, argument4, argument5,
    };
    return invoke_arguments(fixture, memory, fault, number, arguments);
}

static bool access_is(const struct user_memory *memory, unsigned index,
        qword_t address, dword_t size,
        enum guest_memory_access access) {
    return index < memory->access_count &&
            memory->accesses[index].address == address &&
            memory->accesses[index].size == size &&
            memory->accesses[index].access == (dword_t) access;
}

static int test_pselect6(struct task_fixture *fixture,
        struct user_memory *memory,
        struct guest_linux_user_fault *fault,
        struct io_probe *io) {
    const size_t read_offset = 0x8000;
    const size_t write_offset = 0x8020;
    const size_t except_offset = 0x8040;
    const size_t timeout_offset = 0x8060;
    const size_t mask_argument_offset = 0x8080;
    const size_t mask_offset = 0x80a0;
    const qword_t read_address = USER_BASE + read_offset;
    const qword_t write_address = USER_BASE + write_offset;
    const qword_t except_address = USER_BASE + except_offset;
    const qword_t timeout_address = USER_BASE + timeout_offset;
    const qword_t mask_argument_address =
            USER_BASE + mask_argument_offset;
    const qword_t mask_address = USER_BASE + mask_offset;

    struct aarch64_linux_pselect_sigmask mask_argument = {
        .address = mask_address,
        .size = sizeof(sigset_t_),
    };
    struct aarch64_linux_timespec timeout = {0};
    sigset_t_ mask = sig_mask(SIGUSR2_) |
            sig_mask(SIGKILL_) | sig_mask(SIGSTOP_);
    memcpy(memory->bytes + mask_argument_offset,
            &mask_argument, sizeof(mask_argument));
    memcpy(memory->bytes + timeout_offset, &timeout, sizeof(timeout));
    memcpy(memory->bytes + mask_offset, &mask, sizeof(mask));
    memset(memory->bytes + read_offset, 0xff, sizeof(qword_t));
    memset(memory->bytes + write_offset, 0xff, sizeof(qword_t));
    memset(memory->bytes + except_offset, 0xff, sizeof(qword_t));
    fixture->task.blocked = sig_mask(SIGUSR1_);
    fixture->task.has_saved_mask = false;
    io->poll_events = 0;
    reset_user_access(memory);
    qword_t result = invoke_six(fixture, memory, fault, 72,
            1, read_address, write_address, except_address,
            timeout_address, mask_argument_address);
    qword_t read_set;
    qword_t write_set;
    qword_t except_set;
    memcpy(&read_set, memory->bytes + read_offset, sizeof(read_set));
    memcpy(&write_set, memory->bytes + write_offset, sizeof(write_set));
    memcpy(&except_set, memory->bytes + except_offset, sizeof(except_set));
    CHECK(result == 0 && read_set == 0 && write_set == 0 &&
            except_set == 0 && memory->access_count == 9 &&
            access_is(memory, 0, mask_argument_address, 16,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 1, timeout_address, 16,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 2, mask_address, 8,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 3, read_address, 8,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 4, write_address, 8,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 5, except_address, 8,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 6, read_address, 8,
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 7, write_address, 8,
                    GUEST_MEMORY_WRITE) &&
            access_is(memory, 8, except_address, 8,
                    GUEST_MEMORY_WRITE),
            "pselect6 按 x5、timeout、mask、三组 fdset 的 ABI 顺序访问用户内存");
    CHECK(fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "pselect6 正常超时后恢复原信号掩码");

    memset(memory->bytes + read_offset, 0, sizeof(qword_t));
    memset(memory->bytes + write_offset, 0, sizeof(qword_t));
    memset(memory->bytes + except_offset, 0, sizeof(qword_t));
    memory->bytes[read_offset] = 1;
    io->poll_events = POLL_ERR;
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, write_address, except_address,
            timeout_address, 0);
    memcpy(&read_set, memory->bytes + read_offset, sizeof(read_set));
    memcpy(&write_set, memory->bytes + write_offset, sizeof(write_set));
    CHECK(result == 1 && read_set == 1 && write_set == 0,
            "pselect6 的 POLLERR 不把未请求写位误报为就绪");

    memset(memory->bytes + read_offset, 0, sizeof(qword_t));
    memset(memory->bytes + write_offset, 0, sizeof(qword_t));
    memory->bytes[write_offset] = 1;
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, write_address, except_address,
            timeout_address, 0);
    memcpy(&read_set, memory->bytes + read_offset, sizeof(read_set));
    memcpy(&write_set, memory->bytes + write_offset, sizeof(write_set));
    CHECK(result == 1 && read_set == 0 && write_set == 1,
            "pselect6 的 POLLERR 不把未请求读位误报为就绪");

    memset(memory->bytes + read_offset, 0, sizeof(qword_t));
    memset(memory->bytes + write_offset, 0, sizeof(qword_t));
    memory->bytes[read_offset] = 1;
    memory->bytes[write_offset] = 1;
    io->poll_events = POLL_READ | POLL_WRITE;
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, write_address, except_address,
            timeout_address, 0);
    memcpy(&read_set, memory->bytes + read_offset, sizeof(read_set));
    memcpy(&write_set, memory->bytes + write_offset, sizeof(write_set));
    memcpy(&except_set, memory->bytes + except_offset, sizeof(except_set));
    CHECK(result == 2 && read_set == 1 && write_set == 1 &&
            except_set == 0 && memory->write_calls == 3,
            "pselect6 分别计数同一 fd 的读写命中并清零 64 位 fdset 填充位");

    struct rlimit_ saved_limit =
            fixture->group.limits[RLIMIT_NOFILE_];
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {128, 128};
    memset(memory->bytes + read_offset, 0, 16);
    io->poll_events = 0;
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            65, read_address, 0, 0, timeout_address, 0);
    CHECK(result == 0 && memory->access_count == 3 &&
            access_is(memory, 1, read_address, 16,
                    GUEST_MEMORY_READ) &&
            access_is(memory, 2, read_address, 16,
                    GUEST_MEMORY_WRITE),
            "pselect6 按 AArch64 64 位 unsigned long 把 65 个 fd 舍入为 16 字节");
    fixture->group.limits[RLIMIT_NOFILE_] = saved_limit;

    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            0, UINT64_MAX, UINT64_MAX, UINT64_MAX,
            timeout_address, 0);
    CHECK(result == 0 && memory->access_count == 1 &&
            access_is(memory, 0, timeout_address, 16,
                    GUEST_MEMORY_READ),
            "pselect6 在 nfds 为零时忽略所有 fdset 指针");

    reset_user_access(memory);
    memory->fail_read_at = mask_argument_address + 8;
    result = invoke_six(fixture, memory, fault, 72,
            UINT64_MAX, read_address, write_address, except_address,
            timeout_address, mask_argument_address);
    CHECK(result == encoded_error(_EFAULT) &&
            memory->access_count == 1 &&
            fault->address == mask_argument_address + 8,
            "pselect6 优先传播 16 字节 x5 参数读取故障");

    mask_argument.size = UINT64_C(0x1234567800000008);
    memcpy(memory->bytes + mask_argument_offset,
            &mask_argument, sizeof(mask_argument));
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, write_address, except_address,
            timeout_address, mask_argument_address);
    CHECK(result == encoded_error(_EINVAL) &&
            memory->access_count == 2 && memory->write_calls == 0,
            "pselect6 对非空掩码按完整 64 位检查 sigset size");

    mask_argument.size = sizeof(sigset_t_);
    memcpy(memory->bytes + mask_argument_offset,
            &mask_argument, sizeof(mask_argument));
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            UINT64_C(0x12345678ffffffff),
            read_address, write_address, except_address,
            timeout_address, mask_argument_address);
    CHECK(result == encoded_error(_EINVAL) &&
            memory->access_count == 3 && memory->write_calls == 0 &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "pselect6 读取辅助参数后按低 32 位拒绝负 nfds 并恢复掩码");

    mask = 0;
    memcpy(memory->bytes + mask_offset, &mask, sizeof(mask));
    fixture->task.blocked = sig_mask(SIGUSR1_);
    fixture->task.has_saved_mask = false;
    reset_user_access(memory);
    memory->observe_task = &fixture->task;
    memory->observe_read_at = read_address + 4;
    memory->fail_read_at = read_address + 4;
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, 0, 0,
            timeout_address, mask_argument_address);
    CHECK(result == encoded_error(_EFAULT) &&
            memory->observed_read && memory->observed_blocked == 0 &&
            memory->observed_has_saved_mask &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "pselect6 在读取 fdset 前原子安装临时掩码并在 EFAULT 后恢复");

    timeout = (struct aarch64_linux_timespec) {
        .sec = (sqword_t) INT32_MAX + 123,
        .nsec = 500000000,
    };
    memcpy(memory->bytes + timeout_offset, &timeout, sizeof(timeout));
    memset(memory->bytes + read_offset, 0, sizeof(qword_t));
    memory->bytes[read_offset] = 1;
    io->poll_events = POLL_READ;
    reset_user_access(memory);
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, 0, 0, timeout_address, 0);
    memcpy(&timeout, memory->bytes + timeout_offset, sizeof(timeout));
    CHECK(result == 1 && timeout.sec > INT32_MAX &&
            timeout.nsec >= 0 && timeout.nsec < INT64_C(1000000000) &&
            memory->write_calls == 2,
            "pselect6 的超长等待与剩余时间保持 64 位且不经过 host time_t");

    timeout = (struct aarch64_linux_timespec) {
        .sec = (sqword_t) INT32_MAX + 123,
        .nsec = 500000000,
    };
    memcpy(memory->bytes + timeout_offset, &timeout, sizeof(timeout));
    memory->bytes[read_offset] = 1;
    fixture->task.blocked = sig_mask(SIGUSR1_);
    fixture->task.has_saved_mask = false;
    reset_user_access(memory);
    memory->observe_task = &fixture->task;
    memory->observe_write_at = timeout_address + 8;
    memory->fail_write_at = timeout_address + 8;
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, 0, 0,
            timeout_address, mask_argument_address);
    CHECK(result == 1 && memory->write_calls == 2 &&
            fault->address == read_address &&
            fault->access == GUEST_MEMORY_WRITE &&
            fault->kind == GUEST_MEMORY_FAULT_NONE &&
            memory->observed_write &&
            memory->observed_write_blocked == sig_mask(SIGUSR1_) &&
            !memory->observed_write_has_saved_mask &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "pselect6 正常路径先恢复掩码且 timeout 故障不覆盖结果或 fault");

    timeout = (struct aarch64_linux_timespec) {.sec = 5};
    mask = 0;
    memcpy(memory->bytes + timeout_offset, &timeout, sizeof(timeout));
    memcpy(memory->bytes + mask_offset, &mask, sizeof(mask));
    memory->bytes[read_offset] = 1;
    fixture->task.blocked = sig_mask(SIGUSR1_);
    fixture->task.has_saved_mask = false;
    reset_user_access(memory);
    memory->observe_task = &fixture->task;
    memory->observe_write_at = timeout_address;
    memory->fail_write_at = read_address + 4;
    result = invoke_six(fixture, memory, fault, 72,
            1, read_address, 0, 0,
            timeout_address, mask_argument_address);
    CHECK(result == encoded_error(_EFAULT) &&
            memory->write_calls == 2 &&
            fault->address == read_address + 4 &&
            fault->access == GUEST_MEMORY_WRITE &&
            memory->observed_write &&
            memory->observed_write_blocked == sig_mask(SIGUSR1_) &&
            !memory->observed_write_has_saved_mask &&
            fixture->task.blocked == sig_mask(SIGUSR1_) &&
            !fixture->task.has_saved_mask,
            "pselect6 的 fdset 故障优先并在 timeout 写回前恢复掩码");

    memcpy(memory->bytes + timeout_offset, &timeout, sizeof(timeout));
    fixture->task.blocked = sig_mask(SIGUSR1_);
    fixture->task.has_saved_mask = false;
    lock(&fixture->task.sighand->lock);
    fixture->task.pending |= sig_mask(SIGUSR1_);
    unlock(&fixture->task.sighand->lock);
    reset_user_access(memory);
    memory->observe_task = &fixture->task;
    memory->observe_write_at = timeout_address;
    result = invoke_six(fixture, memory, fault, 72,
            0, UINT64_MAX, UINT64_MAX, UINT64_MAX,
            timeout_address, mask_argument_address);
    memcpy(&timeout, memory->bytes + timeout_offset, sizeof(timeout));
    CHECK(result == encoded_error(_EINTR) &&
            fixture->task.blocked == 0 &&
            fixture->task.has_saved_mask &&
            fixture->task.saved_mask == sig_mask(SIGUSR1_) &&
            memory->observed_write &&
            memory->observed_write_blocked == 0 &&
            memory->observed_write_has_saved_mask &&
            timeout.sec >= 0 && timeout.sec <= 5,
            "pselect6 被信号中断时保留临时掩码并写回剩余超时");
    sigmask_restore_temp_task(&fixture->task);
    lock(&fixture->task.sighand->lock);
    fixture->task.pending &= ~sig_mask(SIGUSR1_);
    unlock(&fixture->task.sighand->lock);
    io->poll_events = 0;
    return 0;
}

static const byte_t expected_stat[128] = {
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    0xa4, 0x81, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0xd4, 0xc3, 0xb2, 0xa1, 0x40, 0x30, 0x20, 0x10,
    0x2c, 0x2b, 0x2a, 0x29, 0x28, 0x27, 0x26, 0x25,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xc9, 0x9a, 0x3b, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0xff,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int main(void) {
    struct kernel_probe kernel = {
        .ioctl_output = UINT32_C(0x78563412),
        .stat = {
            .dev = UINT64_C(0x0102030405060708),
            .inode = UINT64_C(0x1112131415161718),
            .mode = S_IFREG | 0644,
            .nlink = 2,
            .uid = UINT32_C(0xa1b2c3d4),
            .gid = UINT32_C(0x10203040),
            .rdev = UINT64_C(0x25262728292a2b2c),
            .size = UINT64_MAX - 1,
            .blksize = 4096,
            .blocks = UINT64_MAX - 2,
            .atime = UINT32_MAX,
            .atime_nsec = UINT32_C(999999999),
            .mtime = UINT32_C(0x7fffffff),
            .mtime_nsec = 1,
            .ctime = UINT32_C(0x80000000),
            .ctime_nsec = 2,
        },
    };
    struct io_probe io = {.read_size = IO_DATA_SIZE};
    for (size_t i = 0; i < sizeof(io.read_data); i++)
        io.read_data[i] = (byte_t) (i * 37 + 11);
    struct task_fixture fixture;
    CHECK(init_fixture(&fixture, &kernel, &io), "任务与文件系统夹具初始化成功");
    struct user_memory memory;
    memset(&memory, 0xa5, sizeof(memory));
    reset_user_access(&memory);
    struct guest_linux_user_fault fault;

    if (test_pselect6(&fixture, &memory, &fault, &io) != 0) {
        (void) destroy_fixture(&fixture);
        return 1;
    }
    reset_user_access(&memory);

    CHECK(invoke(&fixture, &memory, &fault, 129,
            UINT64_C(0x123456787fffffff), 65, 0, 0) ==
                    encoded_error(_EINVAL) &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "kill 按低 32 位校验信号且不访问 guest 内存");
    CHECK(invoke(&fixture, &memory, &fault, 129,
            UINT64_C(0x123456787fffffff), 0, 0, 0) ==
                    encoded_error(_ESRCH),
            "kill 按低 32 位解析不存在的 PID");
    CHECK(invoke(&fixture, &memory, &fault, 130,
            UINT64_C(0xabcdef0100000000), SIGTERM_, 0, 0) ==
                    encoded_error(_EINVAL),
            "tkill 拒绝低 32 位为零的 TID");
    CHECK(invoke(&fixture, &memory, &fault, 131,
            UINT64_C(0xabcdef0100000000), 1, SIGTERM_, 0) ==
                    encoded_error(_EINVAL),
            "tgkill 拒绝低 32 位为零的 TGID");

    qword_t result = invoke(&fixture, &memory, &fault, 198,
            UINT64_C(0xabcdef0100000002),
            UINT64_C(0x1234567800080001), 0, 0);
    struct fd *network_socket = f_get_task(&fixture.task, 1);
    CHECK(result == 1 && network_socket != NULL &&
            network_socket->real_fd >= 0 &&
            bit_test(1, fixture.task.files->cloexec),
            "socket 按低 32 位创建 IPv4 流并设置 CLOEXEC");

    const size_t socket_address_offset = 0x80;
    struct sockaddr_ invalid_socket_address = {
        .family = UINT16_C(0x7fff),
    };
    memcpy(memory.bytes + socket_address_offset,
            &invalid_socket_address, sizeof(invalid_socket_address));
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + socket_address_offset;
    result = invoke(&fixture, &memory, &fault, 203, 99,
            USER_BASE + socket_address_offset,
            sizeof(invalid_socket_address), 0);
    CHECK(result == encoded_error(_EBADF) && memory.read_calls == 0,
            "connect 在访问地址前拒绝非套接字 fd");
    result = invoke(&fixture, &memory, &fault, 203, 1,
            USER_BASE + socket_address_offset, 1, 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0,
            "connect 在访问地址前拒绝过短 sockaddr");

    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 203, 1,
            USER_BASE + socket_address_offset,
            sizeof(invalid_socket_address), 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 1 &&
            memory.read_bytes == sizeof(invalid_socket_address),
            "connect 复制固定宽度 sockaddr 后拒绝未知地址族");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + socket_address_offset + 3;
    result = invoke(&fixture, &memory, &fault, 203, 1,
            USER_BASE + socket_address_offset,
            sizeof(invalid_socket_address), 0);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "connect 保持 sockaddr 的精确 guest fault");

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0, "host 回环监听套接字创建成功");
    struct sockaddr_in listener_address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    CHECK(bind(listener, (const struct sockaddr *) &listener_address,
                    sizeof(listener_address)) == 0 &&
            listen(listener, 1) == 0,
            "host 回环监听套接字启动成功");
    socklen_t listener_length = sizeof(listener_address);
    CHECK(getsockname(listener,
            (struct sockaddr *) &listener_address,
            &listener_length) == 0,
            "host 回环监听地址查询成功");

    byte_t guest_socket_address[16] = {0};
    word_t guest_family = AF_INET_;
    memcpy(guest_socket_address, &guest_family, sizeof(guest_family));
    memcpy(guest_socket_address + 2,
            &listener_address.sin_port,
            sizeof(listener_address.sin_port));
    memcpy(guest_socket_address + 4,
            &listener_address.sin_addr,
            sizeof(listener_address.sin_addr));
    memcpy(memory.bytes + socket_address_offset,
            guest_socket_address, sizeof(guest_socket_address));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 203, 1,
            USER_BASE + socket_address_offset,
            sizeof(guest_socket_address), 0);
    int accepted = accept(listener, NULL, NULL);
    CHECK(result == 0 && accepted >= 0 && memory.read_calls == 1 &&
            memory.read_bytes == sizeof(guest_socket_address),
            "connect 通过 Linux sockaddr 建立 host 回环连接");
    close(accepted);
    close(listener);
    CHECK(invoke(&fixture, &memory, &fault, 57, 1, 0, 0, 0) == 0,
            "socket 探针 fd 清理成功");

    // openat 只读取 guest ABI 的低位参数，且不会越过路径 NUL。
    const size_t path_offset = 0x100;
    static const char created_path[] = "created";
    memcpy(memory.bytes + path_offset, created_path, sizeof(created_path));
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + sizeof(created_path);
    result = invoke(&fixture, &memory, &fault, 56,
            UINT64_C(0x12345678ffffff9c), USER_BASE + path_offset,
            UINT64_C(0xdeadbeef000a0840),
            UINT64_C(0xfeedfaceabcd01ff));
    CHECK(result == 1, "openat 把 AT_FDCWD 与参数低位正确解码");
    CHECK(strcmp(kernel.last_path, "/work/created") == 0 &&
            kernel.last_flags == (O_CREAT_ | O_CLOEXEC_ | O_NONBLOCK_) &&
            kernel.last_mode == 0750, "openat 应用 cwd、umask 并剥离 LARGEFILE");
    CHECK(memory.read_calls == sizeof(created_path) &&
            memory.max_read_size == 1, "路径复制在 NUL 处停止且逐字节访问");
    struct fd *opened = f_get_task(&fixture.task, 1);
    CHECK(opened != NULL && bit_test(1, fixture.task.files->cloexec) &&
            (fd_getflags(opened) & O_NONBLOCK_),
            "openat 把描述符标志安装到目标任务");

    unsigned closes_before = kernel.opened_closes;
    result = invoke(&fixture, &memory, &fault, 57,
            UINT64_C(0xabcdef0100000001), 0, 0, 0);
    CHECK(result == 0 && f_get_task(&fixture.task, 1) == NULL &&
            kernel.opened_closes == closes_before + 1,
            "close 使用低 32 位 fd 并释放目标表项");
    CHECK(invoke(&fixture, &memory, &fault, 57, 1, 0, 0, 0) ==
            encoded_error(_EBADF), "重复 close 符号扩展 EBADF");

    // ioctl 依据命令方向在宿主缓冲中执行，不把 64 位 guest 地址当作宿主指针。
    const size_t ioctl_offset = 0x40;
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 29, 99,
            FIONREAD_, UINT64_MAX, 0);
    CHECK(result == encoded_error(_EBADF) && memory.read_calls == 0 &&
            memory.write_calls == 0 && kernel.ioctl_calls == 0,
            "ioctl 的无效 fd 优先于命令和 guest 指针");
    result = invoke(&fixture, &memory, &fault, 29, 0,
            UINT64_C(0xabcdef0100001234), UINT64_MAX, 0);
    CHECK(result == encoded_error(_ENOTTY) && memory.read_calls == 0 &&
            memory.write_calls == 0 && kernel.ioctl_calls == 0,
            "ioctl 按低 32 位拒绝未知命令且不访问 guest 指针");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + ioctl_offset;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            UINT64_C(0xabcdef010000541b), USER_BASE + ioctl_offset, 0);
    dword_t ioctl_value;
    memcpy(&ioctl_value, memory.bytes + ioctl_offset, sizeof(ioctl_value));
    CHECK(result == 0 && memory.read_calls == 0 &&
            memory.write_calls == 1 && memory.write_bytes == 4 &&
            ioctl_value == kernel.ioctl_output && kernel.ioctl_calls == 1 &&
            kernel.last_ioctl_command == FIONREAD_,
            "输出型 ioctl 不预读 guest 并按低 32 位解码命令");

    reset_user_access(&memory);
    unsigned ioctl_calls_before = kernel.ioctl_calls;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            TIOCGPTN_, AARCH64_LINUX_USER_ADDRESS_MAX - 1, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            kernel.ioctl_calls == ioctl_calls_before + 1 &&
            fault.address == AARCH64_LINUX_USER_ADDRESS_MAX - 1 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "输出型 ioctl 在后端完成后拒绝跨越用户上限的写回");

    memset(memory.bytes + ioctl_offset, 0xa5, sizeof(dword_t));
    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + ioctl_offset + 2;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            TIOCGPTN_, USER_BASE + ioctl_offset, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 1 &&
            memory.bytes[ioctl_offset] == 0x12 &&
            memory.bytes[ioctl_offset + 1] == 0x34 &&
            memory.bytes[ioctl_offset + 2] == 0xa5 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "输出型 ioctl 保持部分写回并传播精确 guest 故障");

    ioctl_value = UINT32_C(0x01020304);
    memcpy(memory.bytes + ioctl_offset, &ioctl_value, sizeof(ioctl_value));
    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + ioctl_offset;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            UINT64_C(0x1234567840045431), USER_BASE + ioctl_offset, 0);
    CHECK(result == 0 && memory.read_calls == 1 && memory.read_bytes == 4 &&
            memory.write_calls == 0 &&
            kernel.ioctl_input == ioctl_value &&
            kernel.last_ioctl_command == TIOCSPTLCK_,
            "输入型编码 ioctl 只读取 guest 并保持完整 64 位地址");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + ioctl_offset + 1;
    ioctl_calls_before = kernel.ioctl_calls;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            TIOCSPTLCK_, USER_BASE + ioctl_offset, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            kernel.ioctl_calls == ioctl_calls_before &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "输入型 ioctl 在后端执行前传播 guest 读取故障");

    ioctl_value = 1;
    memcpy(memory.bytes + ioctl_offset, &ioctl_value, sizeof(ioctl_value));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 29, 0,
            FIONBIO_, USER_BASE + ioctl_offset, 0);
    CHECK(result == 0 && (fd_getflags(f_get_task(&fixture.task, 0)) &
            O_NONBLOCK_) && kernel.ioctl_calls == ioctl_calls_before,
            "FIONBIO 在目标 fd 上启用非阻塞且不进入后端 ioctl");
    ioctl_value = 0;
    memcpy(memory.bytes + ioctl_offset, &ioctl_value, sizeof(ioctl_value));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 29, 0,
            FIONBIO_, USER_BASE + ioctl_offset, 0);
    CHECK(result == 0 && !(fd_getflags(f_get_task(&fixture.task, 0)) &
            O_NONBLOCK_), "FIONBIO 可在目标 fd 上清除非阻塞");

    struct task decoy_task = {.files = fdtable_new(1)};
    CHECK(!IS_ERR(decoy_task.files), "ioctl 诱饵任务 fd 表初始化成功");
    struct fd *ioctl_fd = f_get_task_retain(&fixture.task, 0);
    CHECK(ioctl_fd != NULL, "ioctl 目标 fd 保持引用成功");
    current = &decoy_task;
    CHECK(file_ioctl_fd_task(&fixture.task, 0, ioctl_fd,
            FIOCLEX_, NULL, 0) == 0 &&
            bit_test(0, fixture.task.files->cloexec) &&
            !bit_test(0, decoy_task.files->cloexec),
            "ioctl helper 修改目标任务而非 current 的 fd 标志");
    fd_close(ioctl_fd);
    current = &fixture.task;
    fdtable_release(decoy_task.files);

    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 29, 0,
            FIONCLEX_, UINT64_MAX, 0);
    CHECK(result == 0 && !bit_test(0, fixture.task.files->cloexec) &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "FIONCLEX 不访问参数并清除目标任务 close-on-exec");
    result = invoke(&fixture, &memory, &fault, 29, 0,
            FIOCLEX_, UINT64_MAX, 0);
    CHECK(result == 0 && bit_test(0, fixture.task.files->cloexec) &&
            memory.read_calls == 0 && memory.write_calls == 0,
            "FIOCLEX 不访问参数并设置目标任务 close-on-exec");
    CHECK(invoke(&fixture, &memory, &fault, 29, 0,
            FIONCLEX_, UINT64_MAX, 0) == 0,
            "FIONCLEX 清理测试设置的 close-on-exec 标志");

    ioctl_calls_before = kernel.ioctl_calls;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            UINT64_C(0xabcdef010000540b),
            UINT64_C(0x1234567800000002), 0);
    CHECK(result == 0 && kernel.ioctl_calls == ioctl_calls_before + 1 &&
            kernel.last_ioctl_command == TCFLSH_ &&
            kernel.ioctl_scalar == 2 && memory.read_calls == 0 &&
            memory.write_calls == 0,
            "无缓冲 ioctl 按低 32 位传递标量且不触碰 guest 内存");

    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + ioctl_offset;
    kernel.ioctl_result = _EIO;
    ioctl_calls_before = kernel.ioctl_calls;
    result = invoke(&fixture, &memory, &fault, 29, 0,
            FIONREAD_, USER_BASE + ioctl_offset, 0);
    CHECK(result == encoded_error(_EIO) &&
            kernel.ioctl_calls == ioctl_calls_before + 1 &&
            memory.write_calls == 0,
            "输出型 ioctl 的后端错误优先于 guest 写回故障");
    kernel.ioctl_result = 0;

    // ppoll 使用 8 字节 pollfd、16 字节 timespec 与 64 位临时信号掩码。
    const size_t poll_offset = 0x300;
    const size_t poll_timeout_offset = 0x380;
    const size_t poll_mask_offset = 0x3a0;
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 9, UINT64_MAX, UINT64_MAX, UINT64_MAX);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            memory.write_calls == 0,
            "ppoll 在访问任何 guest 参数前执行 RLIMIT_NOFILE 检查");

    struct aarch64_linux_timespec poll_timeout = {
        .sec = 0,
        .nsec = INT64_C(1000000000),
    };
    memcpy(memory.bytes + poll_timeout_offset,
            &poll_timeout, sizeof(poll_timeout));
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            0, UINT64_MAX);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 1 &&
            memory.read_bytes == sizeof(poll_timeout) &&
            memory.write_calls == 0,
            "ppoll 拒绝十亿纳秒且空掩码忽略 sigsetsize");
    poll_timeout.sec = -1;
    poll_timeout.nsec = 0;
    memcpy(memory.bytes + poll_timeout_offset,
            &poll_timeout, sizeof(poll_timeout));
    reset_user_access(&memory);
    CHECK(invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            0, 0) == encoded_error(_EINVAL),
            "ppoll 拒绝负的 64 位秒数");
    poll_timeout.sec = 0;
    poll_timeout.nsec = -1;
    memcpy(memory.bytes + poll_timeout_offset,
            &poll_timeout, sizeof(poll_timeout));
    reset_user_access(&memory);
    CHECK(invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            0, 0) == encoded_error(_EINVAL),
            "ppoll 拒绝负的 64 位纳秒数");

    poll_timeout = (struct aarch64_linux_timespec) {
        .sec = (sqword_t) INT32_MAX + 123,
        .nsec = 456789012,
    };
    struct pollfd_ wide_timeout_pollfd = {
        .fd = 0,
        .events = POLL_READ,
        .revents = UINT16_MAX,
    };
    memcpy(memory.bytes + poll_timeout_offset,
            &poll_timeout, sizeof(poll_timeout));
    memcpy(memory.bytes + poll_offset,
            &wide_timeout_pollfd, sizeof(wide_timeout_pollfd));
    io.poll_events = POLL_READ;
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            USER_BASE + poll_offset, 1,
            USER_BASE + poll_timeout_offset, 0, 0);
    memcpy(&wide_timeout_pollfd, memory.bytes + poll_offset,
            sizeof(wide_timeout_pollfd));
    CHECK(result == 1 && wide_timeout_pollfd.revents == POLL_READ,
            "ppoll 接受超过 32 位 time_t 上限的 64 位超时且不阻塞就绪 fd");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + poll_timeout_offset + 8;
    result = invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            0, 0);
    CHECK(result == encoded_error(_EFAULT) &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "ppoll 保持 16 字节 timespec 的精确读取故障");

    poll_timeout = (struct aarch64_linux_timespec) {0};
    memcpy(memory.bytes + poll_timeout_offset,
            &poll_timeout, sizeof(poll_timeout));
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            USER_BASE + poll_mask_offset,
            UINT64_C(0x1234567800000008));
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 1 &&
            memory.read_bytes == sizeof(poll_timeout),
            "ppoll 对非空掩码按完整 64 位检查 sigsetsize");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + poll_mask_offset + 4;
    result = invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            USER_BASE + poll_mask_offset, sizeof(sigset_t_));
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 2 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "ppoll 在读取 pollfd 前传播信号掩码故障");

    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            UINT64_MAX, 0, USER_BASE + poll_timeout_offset,
            0, UINT64_MAX);
    CHECK(result == 0 && memory.read_calls == 1 &&
            memory.write_calls == 0,
            "零 nfds 的 ppoll 忽略 pollfd 地址并执行零超时");

    struct pollfd_ pollfds[4] = {
        {.fd = 0, .events = POLL_READ, .revents = UINT16_MAX},
        {.fd = 0, .events = POLL_READ, .revents = UINT16_MAX},
        {.fd = 0, .events = POLL_WRITE, .revents = UINT16_MAX},
        {.fd = -1, .events = POLL_READ, .revents = UINT16_MAX},
    };
    memcpy(memory.bytes + poll_offset, pollfds, sizeof(pollfds));
    io.poll_events = POLL_READ;
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            USER_BASE + poll_offset, array_size(pollfds),
            USER_BASE + poll_timeout_offset, 0, UINT64_MAX);
    memcpy(pollfds, memory.bytes + poll_offset, sizeof(pollfds));
    CHECK(result == 2 && memory.read_calls == 2 &&
            memory.read_bytes == sizeof(poll_timeout) + sizeof(pollfds) &&
            memory.write_calls == 1 &&
            pollfds[0].revents == POLL_READ &&
            pollfds[1].revents == POLL_READ &&
            pollfds[2].revents == 0 && pollfds[3].revents == 0,
            "ppoll 合并重复 fd 监听并按每个就绪 pollfd 计数");

    struct pollfd_ invalid_pollfd = {
        .fd = 99,
        .events = POLL_READ,
        .revents = UINT16_MAX,
    };
    memcpy(memory.bytes + poll_offset,
            &invalid_pollfd, sizeof(invalid_pollfd));
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            USER_BASE + poll_offset, 1, 0, 0, 0);
    memcpy(&invalid_pollfd, memory.bytes + poll_offset,
            sizeof(invalid_pollfd));
    CHECK(result == 1 && invalid_pollfd.revents == POLL_NVAL &&
            memory.read_calls == 1 && memory.write_calls == 1,
            "ppoll 的无效 fd 在无限超时下立即返回 POLLNVAL");

    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            AARCH64_LINUX_USER_ADDRESS_MAX - 3, 1, 0, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 0 &&
            memory.write_calls == 0 &&
            fault.address == AARCH64_LINUX_USER_ADDRESS_MAX - 3 &&
            fault.access == GUEST_MEMORY_READ,
            "ppoll 拒绝跨越用户上限的 pollfd 数组");

    pollfds[0] = (struct pollfd_) {
        .fd = 0,
        .events = POLL_READ,
        .revents = UINT16_MAX,
    };
    memcpy(memory.bytes + poll_offset, pollfds, sizeof(pollfds[0]));
    sigset_t_ write_fault_mask = 0;
    memcpy(memory.bytes + poll_mask_offset,
            &write_fault_mask, sizeof(write_fault_mask));
    fixture.task.blocked = sig_mask(SIGUSR1_);
    fixture.task.has_saved_mask = false;
    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + poll_offset + 6;
    result = invoke_five(&fixture, &memory, &fault, 73,
            USER_BASE + poll_offset, 1,
            USER_BASE + poll_timeout_offset,
            USER_BASE + poll_mask_offset, sizeof(sigset_t_));
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 1 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE &&
            fixture.task.blocked == sig_mask(SIGUSR1_) &&
            !fixture.task.has_saved_mask,
            "ppoll 在写回故障后传播错误并恢复原掩码");

    const sigset_t_ high_signal_bit = sig_mask(NUM_SIGS);
    sigset_t_ poll_mask = high_signal_bit | sig_mask(SIGUSR2_) |
            sig_mask(SIGKILL_) | sig_mask(SIGSTOP_);
    memcpy(memory.bytes + poll_mask_offset, &poll_mask, sizeof(poll_mask));
    memcpy(memory.bytes + poll_offset, pollfds, sizeof(pollfds[0]));
    fixture.task.blocked = sig_mask(SIGUSR1_);
    fixture.task.has_saved_mask = false;
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 73,
            USER_BASE + poll_offset, 1,
            USER_BASE + poll_timeout_offset,
            USER_BASE + poll_mask_offset, sizeof(sigset_t_));
    CHECK(result == 1 && !fixture.task.has_saved_mask &&
            fixture.task.blocked == sig_mask(SIGUSR1_),
            "ppoll 无信号返回后恢复目标任务的原掩码");

    static const char plain_path[] = "plain";
    memcpy(memory.bytes + path_offset, plain_path, sizeof(plain_path));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 56, AT_FDCWD_,
            USER_BASE + path_offset, UINT64_C(0xdeadbeef000a0000),
            UINT64_C(0x12345678abcd1234));
    CHECK(result == 1 && kernel.last_mode == UINT16_C(0x1234) &&
            kernel.last_flags == O_CLOEXEC_,
            "BusyBox 常用 flags 映射 CLOEXEC、剥离 LARGEFILE 且 mode 只取低位");
    CHECK(invoke(&fixture, &memory, &fault, 57, 1, 0, 0, 0) == 0,
            "mode 解码探针 fd 清理成功");

    unsigned opens_before = kernel.opens;
    static const dword_t rejected_open_flags[] = {
        UINT32_C(0x8000), UINT32_C(0x10000), UINT32_C(0x1000),
        UINT32_C(0x3), UINT32_C(0x4040),
    };
    for (size_t index = 0; index < array_size(rejected_open_flags); index++) {
        reset_user_access(&memory);
        memory.fail_read_at = USER_BASE + path_offset;
        result = invoke(&fixture, &memory, &fault, 56, AT_FDCWD_,
                USER_BASE + path_offset, rejected_open_flags[index], 0);
        CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
                kernel.opens == opens_before,
                "未实现或冲突的 AArch64 open flag 在访问路径前返回 EINVAL");
    }

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + sizeof(plain_path);
    result = invoke(&fixture, &memory, &fault, 56, AT_FDCWD_,
            USER_BASE + path_offset, UINT64_C(0xa4000), 0);
    CHECK(result == encoded_error(_ENOTDIR) &&
            memory.read_calls == sizeof(plain_path) &&
            kernel.opens == opens_before + 1 &&
            kernel.last_flags == (O_DIRECTORY_ | O_CLOEXEC_),
            "BusyBox 目录 flags 显式映射并剥离 AArch64 LARGEFILE");
    opens_before = kernel.opens;

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + 3;
    result = invoke(&fixture, &memory, &fault, 56, AT_FDCWD_,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && kernel.opens == opens_before &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "路径 NUL 前故障保持 guest fault 信息");

    reset_user_access(&memory);
    memset(memory.bytes + path_offset, 'x', MAX_PATH);
    result = invoke(&fixture, &memory, &fault, 56, AT_FDCWD_,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == encoded_error(_ENAMETOOLONG) &&
            memory.read_calls == MAX_PATH && memory.read_bytes == MAX_PATH &&
            kernel.opens == opens_before,
            "前 4096 字节无 NUL 时返回 ENAMETOOLONG");

    static const char mkdir_path[] = "new-directory";
    memcpy(memory.bytes + path_offset, mkdir_path, sizeof(mkdir_path));
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + sizeof(mkdir_path);
    result = invoke(&fixture, &memory, &fault, 34,
            UINT64_C(0x12345678ffffff9c), USER_BASE + path_offset,
            UINT64_C(0xabcdef00000001ff), 0);
    CHECK(result == 0 && kernel.mkdir_calls == 1 &&
            strcmp(kernel.last_mkdir_path, "/work/new-directory") == 0 &&
            kernel.last_mkdir_mode == 0750 &&
            memory.read_calls == sizeof(mkdir_path) &&
            memory.read_bytes == sizeof(mkdir_path) &&
            memory.max_read_size == 1,
            "mkdirat 解码低位参数并应用目标任务 cwd 与 umask");

    memory.bytes[path_offset] = '\0';
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 34, 99,
            USER_BASE + path_offset, 0777, 0);
    CHECK(result == encoded_error(_ENOENT) && memory.read_calls == 1 &&
            kernel.mkdir_calls == 1,
            "mkdirat 空路径在检查 dirfd 前返回 ENOENT");

    static const char mkdir_relative[] = "relative-directory";
    memcpy(memory.bytes + path_offset,
            mkdir_relative, sizeof(mkdir_relative));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 34, 99,
            USER_BASE + path_offset, 0777, 0);
    CHECK(result == encoded_error(_EBADF) &&
            memory.read_calls == sizeof(mkdir_relative) &&
            kernel.mkdir_calls == 1,
            "mkdirat 复制相对路径后拒绝无效 dirfd");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + 3;
    result = invoke(&fixture, &memory, &fault, 34, AT_FDCWD_,
            USER_BASE + path_offset, 0777, 0);
    CHECK(result == encoded_error(_EFAULT) && kernel.mkdir_calls == 1 &&
            memory.read_calls == 4 && memory.read_bytes == 3 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "mkdirat 路径故障保持精确 guest fault 且不创建目录");

    static const char mkdir_absolute[] = "/absolute-directory";
    memcpy(memory.bytes + path_offset,
            mkdir_absolute, sizeof(mkdir_absolute));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 34, 99,
            USER_BASE + path_offset, 0705, 0);
    CHECK(result == 0 && kernel.mkdir_calls == 2 &&
            strcmp(kernel.last_mkdir_path, "/absolute-directory") == 0 &&
            kernel.last_mkdir_mode == 0700,
            "mkdirat 绝对路径从目标 root 解析并忽略无效 dirfd");

    const size_t rename_destination_offset = path_offset + 0x100;
    static const char rename_source[] = "rename-source";
    static const char rename_destination[] = "rename-destination";
    memcpy(memory.bytes + path_offset,
            rename_source, sizeof(rename_source));
    memcpy(memory.bytes + rename_destination_offset,
            rename_destination, sizeof(rename_destination));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 38,
            UINT64_C(0x12345678ffffff9c), USER_BASE + path_offset,
            UINT64_C(0xabcdef01ffffff9c),
            USER_BASE + rename_destination_offset);
    CHECK(result == 0 && kernel.rename_calls == 1 &&
            strcmp(kernel.last_rename_source,
                    "/work/rename-source") == 0 &&
            strcmp(kernel.last_rename_destination,
                    "/work/rename-destination") == 0 &&
            memory.read_calls ==
                    sizeof(rename_source) + sizeof(rename_destination) &&
            memory.max_read_size == 1,
            "renameat 解码两端 64 位参数并使用目标 cwd");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset;
    result = invoke_five(&fixture, &memory, &fault, 276,
            AT_FDCWD_, USER_BASE + path_offset,
            AT_FDCWD_, USER_BASE + rename_destination_offset,
            UINT64_C(0xabcdef0100000001));
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            kernel.rename_calls == 1,
            "renameat2 按 flags 低 32 位在访问路径前拒绝未知位");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + rename_destination_offset + 4;
    result = invoke_five(&fixture, &memory, &fault, 276,
            AT_FDCWD_, USER_BASE + path_offset,
            AT_FDCWD_, USER_BASE + rename_destination_offset, 0);
    CHECK(result == encoded_error(_EFAULT) &&
            memory.read_calls == sizeof(rename_source) + 5 &&
            memory.read_bytes == sizeof(rename_source) + 4 &&
            kernel.rename_calls == 1 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "renameat2 保持目标路径的精确 guest fault 且不重命名");

    static const char rename_absolute_source[] = "/absolute-source";
    static const char rename_absolute_destination[] =
            "/absolute-destination";
    memcpy(memory.bytes + path_offset,
            rename_absolute_source, sizeof(rename_absolute_source));
    memcpy(memory.bytes + rename_destination_offset,
            rename_absolute_destination,
            sizeof(rename_absolute_destination));
    reset_user_access(&memory);
    result = invoke_five(&fixture, &memory, &fault, 276,
            99, USER_BASE + path_offset,
            98, USER_BASE + rename_destination_offset, 0);
    CHECK(result == 0 && kernel.rename_calls == 2 &&
            strcmp(kernel.last_rename_source, "/absolute-source") == 0 &&
            strcmp(kernel.last_rename_destination,
                    "/absolute-destination") == 0,
            "renameat2 的绝对路径从目标 root 解析并忽略两端 dirfd");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset;
    result = invoke(&fixture, &memory, &fault, 35, AT_FDCWD_,
            USER_BASE + path_offset, UINT64_C(0xabcdef0100000001), 0);
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            kernel.unlink_calls == 0 && kernel.rmdir_calls == 0,
            "unlinkat 按 flags 低 32 位在访问路径前拒绝未知位");

    static const char removed_path[] = "removed";
    memcpy(memory.bytes + path_offset, removed_path, sizeof(removed_path));
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + sizeof(removed_path);
    result = invoke(&fixture, &memory, &fault, 35,
            UINT64_C(0x12345678ffffff9c), USER_BASE + path_offset,
            UINT64_C(0xfeedface00000000), 0);
    CHECK(result == 0 && kernel.unlink_calls == 1 &&
            strcmp(kernel.last_unlink_path, "/work/removed") == 0 &&
            memory.read_calls == sizeof(removed_path) &&
            memory.read_bytes == sizeof(removed_path) &&
            memory.max_read_size == 1,
            "unlinkat 忽略参数高位并从目标 cwd 删除普通路径");

    static const char directory_path[] = "directory";
    memcpy(memory.bytes + path_offset,
            directory_path, sizeof(directory_path));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 35, AT_FDCWD_,
            USER_BASE + path_offset,
            UINT64_C(0xabcdef0100000200), 0);
    CHECK(result == 0 && kernel.rmdir_calls == 1 &&
            kernel.unlink_calls == 1 &&
            strcmp(kernel.last_rmdir_path, "/work/directory") == 0,
            "unlinkat 的 REMOVEDIR 只调用目录删除操作");

    memory.bytes[path_offset] = '\0';
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 35, 99,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == encoded_error(_ENOENT) && memory.read_calls == 1 &&
            kernel.unlink_calls == 1,
            "unlinkat 空路径在检查 dirfd 前返回 ENOENT");

    static const char relative_path[] = "relative";
    memcpy(memory.bytes + path_offset,
            relative_path, sizeof(relative_path));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 35, 99,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == encoded_error(_EBADF) &&
            memory.read_calls == sizeof(relative_path) &&
            kernel.unlink_calls == 1,
            "unlinkat 复制相对路径后拒绝无效 dirfd");
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 35, 0,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == encoded_error(_ENOTDIR) && kernel.unlink_calls == 1,
            "unlinkat 拒绝指向普通文件的 dirfd");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + 2;
    result = invoke(&fixture, &memory, &fault, 35, 99,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && kernel.unlink_calls == 1 &&
            memory.read_calls == 3 && memory.read_bytes == 2 &&
            memory.max_read_size == 1 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ &&
            fault.kind == GUEST_MEMORY_FAULT_UNMAPPED,
            "unlinkat 路径故障保持精确 guest fault 且不删除文件");

    static const char absolute_path[] = "/absolute";
    memcpy(memory.bytes + path_offset,
            absolute_path, sizeof(absolute_path));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 35, 99,
            USER_BASE + path_offset, 0, 0);
    CHECK(result == 0 && kernel.unlink_calls == 2 &&
            strcmp(kernel.last_unlink_path, "/absolute") == 0,
            "unlinkat 绝对路径从目标 root 解析并忽略无效 dirfd");

    // write 固定按 4096 字节分块，并保留 Linux 的部分完成结果。
    const size_t io_offset = 0x2000;
    for (size_t i = 0; i < IO_DATA_SIZE; i++)
        memory.bytes[io_offset + i] = (byte_t) (i * 13 + 7);
    reset_io(&io);
    reset_user_access(&memory);
    io.max_write = 3;
    result = invoke(&fixture, &memory, &fault, 64, 0,
            USER_BASE + io_offset, UINT64_MAX, 0);
    CHECK(result == 3 && io.write_calls == 1 && io.write_requests[0] == 4096 &&
            io.written_size == 3 && memory.read_calls == 1 &&
            memory.max_read_size == 4096,
            "超大 write count 不被宿主截断并在短写后停止");
    CHECK(memcmp(io.written_data, memory.bytes + io_offset, 3) == 0,
            "短写内容来自高位 guest 地址");

    reset_io(&io);
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + io_offset + 4096;
    result = invoke(&fixture, &memory, &fault, 64, 0,
            USER_BASE + io_offset, 4097, 0);
    CHECK(result == 4096 && io.write_calls == 1 && memory.read_calls == 2 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "write 第二块用户故障返回首块完成量");

    reset_io(&io);
    reset_user_access(&memory);
    io.write_error_call = 2;
    io.write_error = _ENOSPC;
    result = invoke(&fixture, &memory, &fault, 64, 0,
            USER_BASE + io_offset, 8192, 0);
    CHECK(result == 4096 && io.write_calls == 2 && memory.read_calls == 2,
            "write 后续后端错误不得覆盖已完成字节数");

    reset_io(&io);
    reset_user_access(&memory);
    io.write_error_call = 1;
    io.write_error = _ENOSPC;
    result = invoke(&fixture, &memory, &fault, 64, 0,
            USER_BASE + io_offset, 1, 0);
    CHECK(result == encoded_error(_ENOSPC) && io.write_calls == 1 &&
            memory.read_calls == 1,
            "write 首次后端错误按 64 位负值返回");

    reset_io(&io);
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + io_offset;
    result = invoke(&fixture, &memory, &fault, 64, 0,
            USER_BASE + io_offset, 1, 0);
    CHECK(result == encoded_error(_EFAULT) && io.write_calls == 0,
            "write 首块用户故障返回 EFAULT 且不调用后端");

    reset_io(&io);
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 64, 0, UINT64_MAX, 2, 0);
    CHECK(result == encoded_error(_EFAULT) && io.write_calls == 0 &&
            memory.read_calls == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "write 在回绕前拒绝越过 qword 末端的用户范围");

    reset_io(&io);
    reset_user_access(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 64, 99, UINT64_MAX, 0, 0) ==
            encoded_error(_EBADF) && memory.read_calls == 0,
            "零长度 write 仍检查 fd 且不访问 guest 指针");
    CHECK(invoke(&fixture, &memory, &fault, 64, 0, UINT64_MAX, 0, 0) == 0 &&
            io.write_calls == 1 && io.write_requests[0] == 0 &&
            memory.read_calls == 0,
            "有效 fd 的零长度 write 调用一次文件语义");

    // read 先消费文件数据再写回 guest，后续故障只返回此前完成量。
    reset_io(&io);
    reset_user_access(&memory);
    io.max_read = 3;
    result = invoke(&fixture, &memory, &fault, 63, 0,
            USER_BASE + io_offset, 4096, 0);
    CHECK(result == 3 && io.read_calls == 1 && memory.write_calls == 1 &&
            memory.max_write_size == 3 &&
            memcmp(memory.bytes + io_offset, io.read_data, 3) == 0,
            "短 read 只写回实际读取长度");

    reset_io(&io);
    reset_user_access(&memory);
    memset(memory.bytes + io_offset, 0xa5, 4097);
    memory.fail_write_at = USER_BASE + io_offset + 4096;
    result = invoke(&fixture, &memory, &fault, 63, 0,
            USER_BASE + io_offset, 4097, 0);
    CHECK(result == 4096 && io.read_calls == 2 && io.read_offset == 4097 &&
            memory.write_calls == 2 &&
            memory.bytes[io_offset + 4096] == 0xa5,
            "read 第二块写回故障保留首块结果及既有文件偏移语义");

    reset_io(&io);
    reset_user_access(&memory);
    io.read_error_call = 2;
    io.read_error = _EIO;
    result = invoke(&fixture, &memory, &fault, 63, 0,
            USER_BASE + io_offset, 8192, 0);
    CHECK(result == 4096 && io.read_calls == 2 && memory.write_calls == 1,
            "read 后续后端错误不得覆盖已完成字节数");

    reset_io(&io);
    reset_user_access(&memory);
    io.read_error_call = 1;
    io.read_error = _EIO;
    result = invoke(&fixture, &memory, &fault, 63, 0,
            USER_BASE + io_offset, 1, 0);
    CHECK(result == encoded_error(_EIO) && io.read_calls == 1 &&
            memory.write_calls == 0,
            "read 首次后端错误按 64 位负值返回");

    reset_io(&io);
    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + io_offset;
    result = invoke(&fixture, &memory, &fault, 63, 0,
            USER_BASE + io_offset, 1, 0);
    CHECK(result == encoded_error(_EFAULT) && io.read_calls == 1 &&
            memory.write_calls == 1,
            "read 首次写回故障发生在文件读取之后");

    reset_io(&io);
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 63, 0, UINT64_MAX, 2, 0);
    CHECK(result == encoded_error(_EFAULT) && io.read_calls == 1 &&
            io.read_offset == 2 && memory.write_calls == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "read 在回绕前拒绝越过 qword 末端的用户范围");

    reset_io(&io);
    reset_user_access(&memory);
    CHECK(invoke(&fixture, &memory, &fault, 63, 99, UINT64_MAX, 0, 0) ==
            encoded_error(_EBADF) && memory.write_calls == 0,
            "零长度 read 仍检查 fd 且不访问 guest 指针");
    CHECK(invoke(&fixture, &memory, &fault, 63, 0, UINT64_MAX, 0, 0) == 0 &&
            io.read_calls == 1 && io.read_requests[0] == 0 &&
            memory.write_calls == 0,
            "有效 fd 的零长度 read 调用一次文件语义");

    // getcwd 先完成路径与容量检查，成功后才触碰 guest 内存。
    const size_t cwd_offset = 0x4800;
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 17,
            USER_BASE + cwd_offset, 6, 0, 0);
    CHECK(result == 6 && memory.write_calls == 1 &&
            memcmp(memory.bytes + cwd_offset, "/work", 6) == 0,
            "getcwd 返回并复制包含 NUL 的精确长度");

    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + cwd_offset;
    result = invoke(&fixture, &memory, &fault, 17,
            USER_BASE + cwd_offset, 5, 0, 0);
    CHECK(result == encoded_error(_ERANGE) && memory.write_calls == 0,
            "getcwd 的 ERANGE 优先于 guest EFAULT");

    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 17,
            USER_BASE + cwd_offset, UINT64_MAX, 0, 0);
    CHECK(result == 6 && memory.write_bytes == 6 &&
            memcmp(memory.bytes + cwd_offset, "/work", 6) == 0,
            "getcwd 的 qword 容量不会被宿主 size_t 截断");

    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + cwd_offset;
    result = invoke(&fixture, &memory, &fault, 17,
            USER_BASE + cwd_offset, 6, 0, 0);
    CHECK(result == encoded_error(_EFAULT) &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "getcwd 成功取路径后的写回故障返回 EFAULT");

    // fstat 以未对齐 guest 地址写出精确的 128 字节 AArch64 布局。
    const size_t stat_offset = 0x5003;
    memset(memory.bytes + stat_offset - 1, 0xa5, sizeof(expected_stat) + 2);
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 80,
            UINT64_C(0x1234567800000000), USER_BASE + stat_offset, 0, 0);
    CHECK(result == 0 && memory.write_calls == 1 &&
            memory.write_bytes == sizeof(expected_stat) &&
            memory.max_write_size == sizeof(expected_stat),
            "fstat 一次写回固定 128 字节且接受未对齐地址");
    CHECK(memory.bytes[stat_offset - 1] == 0xa5 &&
            memory.bytes[stat_offset + sizeof(expected_stat)] == 0xa5 &&
            memcmp(memory.bytes + stat_offset,
                    expected_stat, sizeof(expected_stat)) == 0,
            "fstat 字段、符号扩展、填充与边界字节完全匹配 ABI");

    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + stat_offset;
    result = invoke(&fixture, &memory, &fault, 80, 0,
            USER_BASE + stat_offset, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 1 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "有效 fstat 的 guest 写回故障返回 EFAULT");

    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + stat_offset;
    result = invoke(&fixture, &memory, &fault, 80, 99,
            USER_BASE + stat_offset, 0, 0);
    CHECK(result == encoded_error(_EBADF) && memory.write_calls == 0,
            "fstat 的 fd 错误优先于 guest 写回故障");

    const size_t readlink_path_offset = 0x5800;
    const size_t readlink_output_offset = 0x5903;
    static const char readlink_path[] = "link";
    memcpy(memory.bytes + readlink_path_offset,
            readlink_path, sizeof(readlink_path));
    memset(memory.bytes + readlink_output_offset - 1, 0xa5, 18);
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + readlink_path_offset;
    unsigned readlink_calls_before = kernel.readlink_calls;
    result = invoke(&fixture, &memory, &fault, 78, AT_FDCWD_,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset,
            UINT64_C(0xabcdef0100000000));
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            memory.write_calls == 0 &&
            kernel.readlink_calls == readlink_calls_before,
            "readlinkat 的零长度在访问路径前返回 EINVAL");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + readlink_path_offset;
    result = invoke(&fixture, &memory, &fault, 78, AT_FDCWD_,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset,
            UINT64_C(0xabcdef01ffffffff));
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 0 &&
            memory.write_calls == 0 &&
            kernel.readlink_calls == readlink_calls_before,
            "readlinkat 按低 32 位拒绝负长度且不访问路径");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + readlink_path_offset + 2;
    result = invoke(&fixture, &memory, &fault, 78, AT_FDCWD_,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset, 16);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            kernel.readlink_calls == readlink_calls_before &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "readlinkat 保持路径读取故障且不查询后端");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + readlink_path_offset +
            sizeof(readlink_path);
    result = invoke(&fixture, &memory, &fault, 78,
            UINT64_C(0x12345678ffffff9c),
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset,
            UINT64_C(0xabcdef0100000004));
    CHECK(result == 4 && memory.read_calls == sizeof(readlink_path) &&
            memory.write_calls == 1 && memory.write_bytes == 4 &&
            strcmp(kernel.last_readlink_path, "/work/link") == 0 &&
            memcmp(memory.bytes + readlink_output_offset, "meta", 4) == 0,
            "readlinkat 按低位解码参数并截断符号链接内容");
    CHECK(memory.bytes[readlink_output_offset - 1] == 0xa5 &&
            memory.bytes[readlink_output_offset + 4] == 0xa5,
            "readlinkat 不写 NUL 且保持输出范围外哨兵");

    static const char absolute_link_path[] = "/work/link";
    memcpy(memory.bytes + readlink_path_offset,
            absolute_link_path, sizeof(absolute_link_path));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 78, 99,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset, 16);
    CHECK(result == 8 && memory.write_calls == 1 &&
            memcmp(memory.bytes + readlink_output_offset,
                    "metadata", 8) == 0,
            "readlinkat 的绝对路径使用目标 root 并忽略无效 dirfd");

    memcpy(memory.bytes + readlink_path_offset,
            readlink_path, sizeof(readlink_path));
    reset_user_access(&memory);
    readlink_calls_before = kernel.readlink_calls;
    result = invoke(&fixture, &memory, &fault, 78, 99,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset, 16);
    CHECK(result == encoded_error(_EBADF) &&
            memory.read_calls == sizeof(readlink_path) &&
            memory.write_calls == 0 &&
            kernel.readlink_calls == readlink_calls_before,
            "readlinkat 的相对路径在复制后拒绝无效 dirfd");
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 78, 0,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset, 16);
    CHECK(result == encoded_error(_ENOTDIR) && memory.write_calls == 0,
            "readlinkat 的相对路径拒绝普通文件 dirfd");

    reset_user_access(&memory);
    readlink_calls_before = kernel.readlink_calls;
    result = invoke(&fixture, &memory, &fault, 78, AT_FDCWD_,
            USER_BASE + readlink_path_offset,
            AARCH64_LINUX_USER_ADDRESS_MAX - 3, 16);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 0 &&
            kernel.readlink_calls == readlink_calls_before + 1 &&
            fault.address == AARCH64_LINUX_USER_ADDRESS_MAX - 3 &&
            fault.access == GUEST_MEMORY_WRITE &&
            fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE,
            "readlinkat 只按实际结果长度拒绝跨越用户上限的输出");

    memset(memory.bytes + readlink_output_offset, 0xa5, 8);
    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + readlink_output_offset + 3;
    result = invoke(&fixture, &memory, &fault, 78, AT_FDCWD_,
            USER_BASE + readlink_path_offset,
            USER_BASE + readlink_output_offset, 16);
    CHECK(result == encoded_error(_EFAULT) && memory.write_calls == 1 &&
            memcmp(memory.bytes + readlink_output_offset, "met", 3) == 0 &&
            memory.bytes[readlink_output_offset + 3] == 0xa5 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "readlinkat 保持部分 guest 写回并传播精确故障");

    // newfstatat 复用 task 路径语义，并输出同一份 AArch64 stat wire ABI。
    const size_t metadata_offset = 0x6000;
    const size_t metadata_stat_offset = 0x7003;
    static const char metadata_path[] = "metadata";
    memcpy(memory.bytes + metadata_offset,
            metadata_path, sizeof(metadata_path));
    memset(memory.bytes + metadata_stat_offset - 1,
            0xa5, sizeof(expected_stat) + 2);
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + metadata_offset + sizeof(metadata_path);
    unsigned stat_calls_before = kernel.stat_calls;
    unsigned fstat_calls_before = kernel.fstat_calls;
    result = invoke(&fixture, &memory, &fault, 79,
            UINT64_C(0x12345678ffffff9c), USER_BASE + metadata_offset,
            USER_BASE + metadata_stat_offset,
            UINT64_C(0xfacefeed00004800));
    CHECK(result == 0 &&
            strcmp(kernel.last_stat_path, "/work/metadata") == 0 &&
            kernel.stat_calls == stat_calls_before + 1 &&
            kernel.fstat_calls == fstat_calls_before,
            "newfstatat 解码 dirfd、NO_AUTOMOUNT 与同步提示并使用目标 cwd");
    CHECK(memory.read_calls == sizeof(metadata_path) &&
            memory.write_calls == 1 &&
            memory.write_bytes == sizeof(expected_stat),
            "newfstatat 在路径 NUL 处停止并一次写回 stat");
    CHECK(memory.bytes[metadata_stat_offset - 1] == 0xa5 &&
            memory.bytes[metadata_stat_offset + sizeof(expected_stat)] == 0xa5 &&
            memcmp(memory.bytes + metadata_stat_offset,
                    expected_stat, sizeof(expected_stat)) == 0,
            "newfstatat 的未对齐 128 字节结果匹配独立 ABI 向量");

    static const char link_path[] = "link";
    memcpy(memory.bytes + metadata_offset, link_path, sizeof(link_path));
    reset_user_access(&memory);
    stat_calls_before = kernel.stat_calls;
    result = invoke(&fixture, &memory, &fault, 79, AT_FDCWD_,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset,
            0);
    CHECK(result == 0 &&
            strcmp(kernel.last_stat_path, "/work/metadata") == 0,
            "newfstatat 默认跟随末尾符号链接");
    CHECK(kernel.stat_calls > stat_calls_before,
            "newfstatat 跟随链接时必须执行底层 stat");
    reset_user_access(&memory);
    stat_calls_before = kernel.stat_calls;
    result = invoke(&fixture, &memory, &fault, 79, AT_FDCWD_,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset,
            UINT64_C(0xdeadbeef00000100));
    CHECK(result == 0 && strcmp(kernel.last_stat_path, "/work/link") == 0,
            "newfstatat 的 wire NOFOLLOW 只作用于末尾链接");
    CHECK(kernel.stat_calls > stat_calls_before,
            "newfstatat 不跟随链接时必须执行底层 stat");

    memory.bytes[metadata_offset] = '\0';
    memset(memory.bytes + metadata_stat_offset,
            0xa5, sizeof(expected_stat));
    reset_user_access(&memory);
    stat_calls_before = kernel.stat_calls;
    fstat_calls_before = kernel.fstat_calls;
    result = invoke(&fixture, &memory, &fault, 79,
            UINT64_C(0x8765432100000000), USER_BASE + metadata_offset,
            USER_BASE + metadata_stat_offset, UINT64_C(0x1000));
    CHECK(result == 0 && memory.read_calls == 1 &&
            memory.write_calls == 1 &&
            memory.write_bytes == sizeof(expected_stat) &&
            kernel.stat_calls == stat_calls_before &&
            kernel.fstat_calls == fstat_calls_before + 1 &&
            memcmp(memory.bytes + metadata_stat_offset,
                    expected_stat, sizeof(expected_stat)) == 0,
            "newfstatat 的 EMPTY_PATH 可查询普通文件 fd");

    memset(memory.bytes + metadata_stat_offset,
            0xa5, sizeof(expected_stat));
    reset_user_access(&memory);
    fstat_calls_before = kernel.fstat_calls;
    result = invoke(&fixture, &memory, &fault, 79, 0, 0,
            USER_BASE + metadata_stat_offset, UINT64_C(0x1000));
    CHECK(result == 0 && memory.read_calls == 0 &&
            memory.write_calls == 1 &&
            memory.write_bytes == sizeof(expected_stat) &&
            kernel.fstat_calls == fstat_calls_before + 1 &&
            memcmp(memory.bytes + metadata_stat_offset,
                    expected_stat, sizeof(expected_stat)) == 0,
            "newfstatat 接受 NULL 与 EMPTY_PATH 的现代 Linux 组合");

    memset(memory.bytes + metadata_stat_offset,
            0xa5, sizeof(expected_stat));
    reset_user_access(&memory);
    fstat_calls_before = kernel.fstat_calls;
    result = invoke(&fixture, &memory, &fault, 79,
            UINT64_C(0x12345678ffffff9c), 0,
            USER_BASE + metadata_stat_offset, UINT64_C(0x1000));
    CHECK(result == 0 && memory.read_calls == 0 &&
            memory.write_calls == 1 &&
            kernel.fstat_calls == fstat_calls_before + 1 &&
            memcmp(memory.bytes + metadata_stat_offset,
                    expected_stat, sizeof(expected_stat)) == 0,
            "newfstatat 的 NULL 与 EMPTY_PATH 可查询目标 cwd");

    memset(memory.bytes + metadata_stat_offset,
            0xa5, sizeof(expected_stat));
    reset_user_access(&memory);
    fstat_calls_before = kernel.fstat_calls;
    result = invoke(&fixture, &memory, &fault, 79, 0, 0,
            USER_BASE + metadata_stat_offset, UINT64_C(0x80001000));
    CHECK(result == 0 && memory.read_calls == 0 &&
            memory.write_calls == 1 &&
            memory.write_bytes == sizeof(expected_stat) &&
            kernel.fstat_calls == fstat_calls_before + 1 &&
            memcmp(memory.bytes + metadata_stat_offset,
                    expected_stat, sizeof(expected_stat)) == 0,
            "newfstatat 的非负 fd 空名称快路忽略其余 flags");

    reset_user_access(&memory);
    stat_calls_before = kernel.stat_calls;
    fstat_calls_before = kernel.fstat_calls;
    result = invoke(&fixture, &memory, &fault, 79, 99,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset, 0);
    CHECK(result == encoded_error(_ENOENT) && memory.read_calls == 1 &&
            memory.write_calls == 0 && kernel.stat_calls == stat_calls_before &&
            kernel.fstat_calls == fstat_calls_before,
            "newfstatat 空路径无 EMPTY 时优先返回 ENOENT");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + metadata_offset;
    result = invoke(&fixture, &memory, &fault, 79, 99,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset,
            UINT64_C(0x80000000));
    CHECK(result == encoded_error(_EINVAL) && memory.read_calls == 1 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "newfstatat 尝试复制路径后由未知 flags 决定最终错误");

    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + metadata_offset;
    result = invoke(&fixture, &memory, &fault, 79, 99,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "newfstatat 的合法 flags 保持 pathname 错误优先于 dirfd");

    memcpy(memory.bytes + metadata_offset,
            metadata_path, sizeof(metadata_path));
    reset_user_access(&memory);
    stat_calls_before = kernel.stat_calls;
    result = invoke(&fixture, &memory, &fault, 79, AT_FDCWD_,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset,
            UINT64_C(0x80000000));
    CHECK(result == encoded_error(_EINVAL) &&
            memory.read_calls == sizeof(metadata_path) &&
            memory.write_calls == 0 && kernel.stat_calls == stat_calls_before,
            "newfstatat 对合法路径拒绝未知 wire flags");

    reset_user_access(&memory);
    memory.fail_write_at = USER_BASE + metadata_stat_offset;
    stat_calls_before = kernel.stat_calls;
    result = invoke(&fixture, &memory, &fault, 79, AT_FDCWD_,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset, 0);
    CHECK(result == encoded_error(_EFAULT) &&
            kernel.stat_calls == stat_calls_before + 1 &&
            memory.write_calls == 1 &&
            fault.address == memory.fail_write_at &&
            fault.access == GUEST_MEMORY_WRITE,
            "newfstatat 在路径查询成功后保持输出故障");

    reset_user_access(&memory);
    stat_calls_before = kernel.stat_calls;
    result = invoke(&fixture, &memory, &fault, 79, 99,
            USER_BASE + metadata_offset, USER_BASE + metadata_stat_offset, 0);
    CHECK(result == encoded_error(_EBADF) &&
            memory.read_calls == sizeof(metadata_path) &&
            memory.write_calls == 0 && kernel.stat_calls == stat_calls_before,
            "newfstatat 相对路径在复制后拒绝无效 dirfd");

    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 79, 0, 0,
            USER_BASE + metadata_stat_offset, 0);
    CHECK(result == encoded_error(_EFAULT) && memory.read_calls == 1,
            "newfstatat 的 NULL 路径需要 EMPTY_PATH");

    const size_t exec_path_offset = 0x100;
    const size_t empty_string_offset = 0x180;
    const size_t pointer_table_offset = 0x800;
    const size_t pointer_count = 7300;
    strcpy((char *) memory.bytes + exec_path_offset, "/bin/null-argv");
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 221,
            USER_BASE + exec_path_offset, 0, 0, 0);
    CHECK(result == encoded_error(_EACCES) &&
            fixture.completion.disposition == GUEST_LINUX_SYSCALL_RETURN,
            "execve 接受 NULL argv/envp 并在失败时保持普通返回");
    strcpy((char *) memory.bytes + exec_path_offset, "/bin/too-many");
    memory.bytes[empty_string_offset] = '\0';
    qword_t empty_string_address = USER_BASE + empty_string_offset;
    for (size_t index = 0; index < pointer_count; index++)
        memcpy(memory.bytes + pointer_table_offset + index * sizeof(qword_t),
                &empty_string_address, sizeof(empty_string_address));
    memset(memory.bytes + pointer_table_offset +
            pointer_count * sizeof(qword_t), 0, sizeof(qword_t));
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 221,
            USER_BASE + exec_path_offset, USER_BASE + pointer_table_offset,
            USER_BASE + pointer_table_offset, 0);
    CHECK(result == encoded_error(_E2BIG) && memory.write_calls == 0 &&
            fixture.completion.disposition == GUEST_LINUX_SYSCALL_RETURN,
            "execve 对 argv、envp、字符串和指针表应用同一 ARG_MAX 预算");

    static const char chdir_path[] = "next";
    char cwd[MAX_PATH];
    memcpy(memory.bytes + path_offset, chdir_path, sizeof(chdir_path));
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + 2;
    opens_before = kernel.opens;
    result = invoke(&fixture, &memory, &fault, 49,
            USER_BASE + path_offset, 0, 0, 0);
    CHECK(result == encoded_error(_EFAULT) && kernel.opens == opens_before &&
            memory.read_calls == 3 && memory.read_bytes == 2 &&
            memory.max_read_size == 1 &&
            fault.address == memory.fail_read_at &&
            fault.access == GUEST_MEMORY_READ,
            "chdir 路径故障保持精确 guest fault 且不打开目录");

    memset(memory.bytes + path_offset, 'x', MAX_PATH);
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 49,
            USER_BASE + path_offset, 0, 0, 0);
    CHECK(result == encoded_error(_ENAMETOOLONG) &&
            memory.read_calls == MAX_PATH && memory.read_bytes == MAX_PATH &&
            memory.max_read_size == 1 && kernel.opens == opens_before &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work") == 0,
            "chdir 对 4096 字节无 NUL 路径返回 ENAMETOOLONG");

    memory.bytes[path_offset] = '\0';
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 49,
            USER_BASE + path_offset, 0, 0, 0);
    CHECK(result == encoded_error(_ENOENT) && memory.read_calls == 1 &&
            kernel.opens == opens_before,
            "chdir 空路径返回 ENOENT 且不打开目录");

    static const char missing_path[] = "missing";
    memcpy(memory.bytes + path_offset, missing_path, sizeof(missing_path));
    reset_user_access(&memory);
    closes_before = kernel.opened_closes;
    result = invoke(&fixture, &memory, &fault, 49,
            USER_BASE + path_offset, 0, 0, 0);
    CHECK(result == encoded_error(_ENOENT) &&
            kernel.opens == opens_before + 1 &&
            kernel.opened_closes == closes_before &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work") == 0,
            "chdir 对缺失目录返回 ENOENT 且保持 cwd");

    memcpy(memory.bytes + path_offset, chdir_path, sizeof(chdir_path));
    kernel.stat.mode = S_IFREG;
    reset_user_access(&memory);
    closes_before = kernel.opened_closes;
    result = invoke(&fixture, &memory, &fault, 49,
            USER_BASE + path_offset, 0, 0, 0);
    CHECK(result == encoded_error(_ENOTDIR) &&
            kernel.opens == opens_before + 2 &&
            kernel.opened_closes == closes_before + 1 &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work") == 0,
            "chdir 在权限检查前拒绝普通文件、释放 fd 并保持 cwd");

    kernel.stat.mode = S_IFDIR | 0100;
    reset_user_access(&memory);
    memory.fail_read_at = USER_BASE + path_offset + sizeof(chdir_path);
    opens_before = kernel.opens;
    closes_before = kernel.opened_closes;
    result = invoke(&fixture, &memory, &fault, 49,
            USER_BASE + path_offset, UINT64_C(0x1111222233334444),
            UINT64_C(0x5555666677778888), UINT64_MAX);
    CHECK(result == 0 && kernel.opens == opens_before + 1 &&
            kernel.opened_closes == closes_before &&
            strcmp(kernel.last_path, "/work/next") == 0 &&
            kernel.last_flags == O_DIRECTORY_ &&
            memory.read_calls == sizeof(chdir_path) &&
            memory.read_bytes == sizeof(chdir_path) &&
            memory.max_read_size == 1 &&
            fixture.fs.pwd != fixture.pwd &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work/next") == 0,
            "chdir 更新目标 cwd 并接管新目录 fd 的唯一引用");

    kernel.stat.mode = S_IFDIR;
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 50,
            UINT64_C(0xa5a5a5a500000000), UINT64_MAX, UINT64_MAX,
            UINT64_MAX);
    CHECK(result == encoded_error(_ENOTDIR) &&
            memory.read_calls == 0 && memory.write_calls == 0 &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work/next") == 0,
            "fchdir 按低 32 位解码 fd 并拒绝普通文件");
    CHECK(invoke(&fixture, &memory, &fault, 50, 99, 0, 0, 0) ==
            encoded_error(_EBADF),
            "fchdir 对无效描述符返回 EBADF");

    kernel.stat.mode = S_IFDIR | 0500;
    fd_t held_directory = file_openat_task(
            &fixture.task, AT_FDCWD_, "held", O_DIRECTORY_, 0);
    CHECK(held_directory == 1 &&
            strcmp(kernel.last_path, "/work/next/held") == 0,
            "安装用于 fchdir 的可搜索目录描述符");
    kernel.stat.mode = S_IFDIR;
    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 50,
            UINT64_C(0x1122334400000001), UINT64_MAX, UINT64_MAX,
            UINT64_MAX);
    CHECK(result == encoded_error(_EACCES) &&
            memory.read_calls == 0 && memory.write_calls == 0 &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work/next") == 0,
            "fchdir 重新检查当前凭据的目录搜索权限并保持 cwd");

    kernel.stat.mode = S_IFDIR | 0100;
    result = invoke(&fixture, &memory, &fault, 50,
            UINT64_C(0x5566778800000001), UINT64_MAX, UINT64_MAX,
            UINT64_MAX);
    CHECK(result == 0 && memory.read_calls == 0 &&
            memory.write_calls == 0 &&
            fs_getcwd_task(&fixture.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/work/next/held") == 0,
            "fchdir 保留目录引用并只更新目标任务 cwd");

    reset_user_access(&memory);
    result = invoke(&fixture, &memory, &fault, 999, 0, 0, 0, 0);
    CHECK(result == encoded_error(_ENOSYS) && memory.read_calls == 0 &&
            memory.write_calls == 0,
            "未知系统调用符号扩展 ENOSYS 且不访问 guest 内存");

    CHECK(destroy_fixture(&fixture) == 0 && list_empty(&mounts),
            "清理任务、描述符、inode 与测试挂载");
    return 0;
}

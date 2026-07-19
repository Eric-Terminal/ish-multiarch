#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define USER_BASE UINT64_C(0x00007abc12340000)
#define IO_CHUNK_SIZE UINT32_C(4096)
#define IO_DATA_SIZE (2 * IO_CHUNK_SIZE)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "AArch64 标量 I/O 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return false; \
    } \
} while (0)

struct io_probe {
    byte_t readable_data[IO_DATA_SIZE];
    size_t read_position;
    byte_t written_data[IO_DATA_SIZE];
    size_t written_size;
    unsigned read_calls;
    unsigned write_calls;
    unsigned close_calls;
};

struct user_memory {
    byte_t bytes[IO_DATA_SIZE];
    struct task *task;
    struct fd *replacement;
    struct fd *expected_replacement;
    fd_t fd_number;
    bool replace_during_read;
    bool replace_during_write;
    bool replacement_installed;
    unsigned read_calls;
    unsigned write_calls;
    dword_t first_read_size;
    dword_t first_write_size;
};

struct syscall_fixture {
    struct task task;
    struct tgroup group;
    struct io_probe original;
    struct io_probe replacement;
};

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    probe->read_calls++;
    size_t remaining = sizeof(probe->readable_data) - probe->read_position;
    size_t copied = size < remaining ? size : remaining;
    memcpy(buffer, probe->readable_data + probe->read_position, copied);
    probe->read_position += copied;
    return (ssize_t) copied;
}

static ssize_t probe_write(
        struct fd *fd, const void *buffer, size_t size) {
    struct io_probe *probe = fd->data;
    probe->write_calls++;
    size_t remaining = sizeof(probe->written_data) - probe->written_size;
    size_t copied = size < remaining ? size : remaining;
    memcpy(probe->written_data + probe->written_size, buffer, copied);
    probe->written_size += copied;
    return (ssize_t) copied;
}

static int probe_close(struct fd *fd) {
    struct io_probe *probe = fd->data;
    probe->close_calls++;
    return 0;
}

static const struct fd_ops probe_fd_ops = {
    .read = probe_read,
    .write = probe_write,
    .close = probe_close,
};

static struct fd *make_probe_fd(struct io_probe *probe) {
    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return NULL;
    fd->data = probe;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    return fd;
}

static bool user_range(qword_t address, dword_t size, size_t *offset) {
    if (address < USER_BASE)
        return false;
    qword_t relative = address - USER_BASE;
    if (relative > IO_DATA_SIZE || size > IO_DATA_SIZE - relative)
        return false;
    *offset = (size_t) relative;
    return true;
}

static void replace_fd(struct user_memory *memory) {
    struct fd *replacement = memory->replacement;
    memory->replacement = NULL;
    int close_result = f_close_task(memory->task, memory->fd_number);
    fd_t installed = f_install_task(memory->task, replacement, 0);
    memory->replacement_installed =
            close_result == 0 && installed == memory->fd_number;
}

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    struct user_memory *memory = opaque;
    if (memory->read_calls == 0) {
        memory->first_read_size = size;
        if (memory->replace_during_read)
            replace_fd(memory);
    }
    memory->read_calls++;

    size_t offset;
    if (!user_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
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
    if (memory->write_calls == 0) {
        memory->first_write_size = size;
        if (memory->replace_during_write)
            replace_fd(memory);
    }
    memory->write_calls++;

    size_t offset;
    if (!user_range(address, size, &offset)) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_WRITE,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
        return false;
    }
    memcpy(memory->bytes + offset, source, size);
    return true;
}

static bool init_fixture(struct syscall_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {2, 2};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;

    struct fd *original = make_probe_fd(&fixture->original);
    if (original == NULL ||
            f_install_task(&fixture->task, original, 0) != 0) {
        fdtable_release(fixture->task.files);
        return false;
    }
    current = &fixture->task;
    return true;
}

static void destroy_fixture(struct syscall_fixture *fixture) {
    fdtable_release(fixture->task.files);
    current = NULL;
}

static qword_t invoke(struct syscall_fixture *fixture,
        struct user_memory *memory, qword_t number,
        struct guest_linux_user_fault *fault) {
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
        .arguments = {0, USER_BASE, IO_DATA_SIZE},
    };
    current = &fixture->task;
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

static bool replacement_owns_slot(const struct user_memory *memory) {
    struct fd *active = f_get_task_retain(memory->task, memory->fd_number);
    bool matches = active == memory->expected_replacement;
    if (active != NULL)
        fd_close(active);
    return matches;
}

static bool test_read_retains_original_fd(void) {
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture), "初始化 read 生命周期夹具");
    for (size_t index = 0; index < IO_DATA_SIZE; index++)
        fixture.original.readable_data[index] =
                (byte_t) ((index * 37 + 11) & 0xff);

    struct fd *replacement = make_probe_fd(&fixture.replacement);
    CHECK(replacement != NULL, "创建 read 替换对象");
    struct user_memory memory = {
        .task = &fixture.task,
        .replacement = replacement,
        .expected_replacement = replacement,
        .fd_number = 0,
        .replace_during_write = true,
    };
    struct guest_linux_user_fault fault = {0};
    qword_t result = invoke(&fixture, &memory, 63, &fault);

    CHECK(result == IO_DATA_SIZE && memory.write_calls == 2 &&
            memory.first_write_size == IO_CHUNK_SIZE,
            "read 以两个 4096 字节 guest 写回完成");
    CHECK(memory.replacement_installed && replacement_owns_slot(&memory),
            "read 首次 guest 写回期间完成同号 fd 复用");
    CHECK(fixture.original.read_calls == 2 &&
            fixture.replacement.read_calls == 0,
            "read 的两个分块都读取系统调用开始时的对象");
    CHECK(memcmp(memory.bytes, fixture.original.readable_data,
                    IO_DATA_SIZE) == 0,
            "read 写回完整保留对象内容");
    CHECK(fixture.original.close_calls == 1 &&
            fixture.replacement.close_calls == 0,
            "read 返回时仅释放已从 fd 表移除的原对象");

    destroy_fixture(&fixture);
    CHECK(fixture.replacement.close_calls == 1,
            "read 夹具销毁时释放 fd 表中的替换对象");
    return true;
}

static bool test_write_retains_original_fd(void) {
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture), "初始化 write 生命周期夹具");
    struct fd *replacement = make_probe_fd(&fixture.replacement);
    CHECK(replacement != NULL, "创建 write 替换对象");
    struct user_memory memory = {
        .task = &fixture.task,
        .replacement = replacement,
        .expected_replacement = replacement,
        .fd_number = 0,
        .replace_during_read = true,
    };
    for (size_t index = 0; index < IO_DATA_SIZE; index++)
        memory.bytes[index] = (byte_t) ((index * 19 + 7) & 0xff);

    struct guest_linux_user_fault fault = {0};
    qword_t result = invoke(&fixture, &memory, 64, &fault);

    CHECK(result == IO_DATA_SIZE && memory.read_calls == 2 &&
            memory.first_read_size == IO_CHUNK_SIZE,
            "write 以两个 4096 字节 guest 读取完成");
    CHECK(memory.replacement_installed && replacement_owns_slot(&memory),
            "write 首次 guest 读取期间完成同号 fd 复用");
    CHECK(fixture.original.write_calls == 2 &&
            fixture.replacement.write_calls == 0,
            "write 的两个分块都写入系统调用开始时的对象");
    CHECK(fixture.original.written_size == IO_DATA_SIZE &&
            memcmp(fixture.original.written_data, memory.bytes,
                    IO_DATA_SIZE) == 0,
            "write 将完整 guest 内容写入保留对象");
    CHECK(fixture.original.close_calls == 1 &&
            fixture.replacement.close_calls == 0,
            "write 返回时仅释放已从 fd 表移除的原对象");

    destroy_fixture(&fixture);
    CHECK(fixture.replacement.close_calls == 1,
            "write 夹具销毁时释放 fd 表中的替换对象");
    return true;
}

int main(void) {
    if (!test_read_retains_original_fd())
        return 1;
    if (!test_write_retains_original_fd())
        return 1;
    return 0;
}

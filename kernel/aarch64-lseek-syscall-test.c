#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/aarch64-syscall-service.h"
#include "kernel/errno.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 lseek 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct seek_probe {
    unsigned calls;
    sqword_t offset;
    int whence;
    sqword_t result;
};

struct user_probe {
    unsigned reads;
    unsigned writes;
};

struct syscall_fixture {
    struct task task;
    struct tgroup group;
};

static off_t_ probe_lseek(struct fd *fd, off_t_ offset, int whence) {
    struct seek_probe *probe = fd->data;
    probe->calls++;
    probe->offset = offset;
    probe->whence = whence;
    return probe->result;
}

static const struct fd_ops seek_ops = {
    .lseek = probe_lseek,
};

static const struct fd_ops stream_ops = {};

static bool read_user(void *opaque, qword_t address,
        void *destination, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) destination;
    (void) size;
    (void) fault;
    struct user_probe *probe = opaque;
    probe->reads++;
    return false;
}

static bool write_user(void *opaque, qword_t address,
        const void *source, dword_t size,
        struct guest_linux_user_fault *fault) {
    (void) address;
    (void) source;
    (void) size;
    (void) fault;
    struct user_probe *probe = opaque;
    probe->writes++;
    return false;
}

static bool init_fixture(
        struct syscall_fixture *fixture, struct seek_probe *probe) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {4, 4};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;

    struct fd *seekable = fd_create(&seek_ops);
    if (seekable == NULL)
        return false;
    seekable->data = probe;
    seekable->type = S_IFREG;
    seekable->flags = O_RDWR_;
    if (f_install_task(&fixture->task, seekable, 0) != 0)
        return false;

    struct fd *stream = fd_create(&stream_ops);
    if (stream == NULL)
        return false;
    stream->type = S_IFIFO;
    stream->flags = O_RDWR_;
    if (f_install_task(&fixture->task, stream, 0) != 1)
        return false;
    current = &fixture->task;
    return true;
}

static qword_t invoke(struct syscall_fixture *fixture,
        struct user_probe *user, struct guest_linux_user_fault *fault,
        qword_t fd, qword_t offset, qword_t whence) {
    const struct guest_linux_syscall_context context = {
        .task_opaque = &fixture->task,
        .user = {
            .opaque = user,
            .read = read_user,
            .write = write_user,
        },
    };
    const struct guest_linux_syscall syscall = {
        .number = 62,
        .arguments = {
            fd,
            offset,
            whence,
            UINT64_C(0x1122334455667788),
        },
    };
    return ish_aarch64_linux_syscall_service.dispatch(
            &context, &syscall, fault);
}

int main(void) {
    struct seek_probe seek = {0};
    struct syscall_fixture fixture;
    CHECK(init_fixture(&fixture, &seek), "初始化可定位与流式 fd");
    struct user_probe user = {0};
    struct guest_linux_user_fault fault;

    seek.result = INT64_C(0x123456789abcdef);
    qword_t result = invoke(&fixture, &user, &fault,
            UINT64_C(0xa5a5a5a500000000),
            UINT64_C(0xfedcba9876543210),
            UINT64_C(0xa5a5a5a500000002));
    CHECK(result == (qword_t) seek.result && seek.calls == 1 &&
            seek.offset == (sqword_t) UINT64_C(0xfedcba9876543210) &&
            seek.whence == LSEEK_END,
            "lseek 保留 64 位有符号偏移并按低 32 位解码 fd 与 whence");
    CHECK(user.reads == 0 && user.writes == 0 &&
            fault.kind == GUEST_MEMORY_FAULT_NONE,
            "lseek 不访问用户内存与未使用参数");

    seek.result = 17;
    result = invoke(&fixture, &user, &fault, 0,
            (qword_t) INT64_C(-7), LSEEK_CUR);
    CHECK(result == 17 && seek.calls == 2 && seek.offset == -7 &&
            seek.whence == LSEEK_CUR,
            "lseek 向后端传递负偏移");

    seek.result = _EINVAL;
    result = invoke(&fixture, &user, &fault, 0, 5, UINT32_MAX);
    CHECK(result == (qword_t) (sqword_t) _EINVAL && seek.calls == 3 &&
            seek.whence == -1,
            "lseek 保持后端错误与 whence 的有符号低位");

    result = invoke(&fixture, &user, &fault, 1, 0, LSEEK_SET);
    CHECK(result == (qword_t) (sqword_t) _ESPIPE && seek.calls == 3,
            "不可定位描述符返回 ESPIPE");
    result = invoke(&fixture, &user, &fault, 99, 0, LSEEK_SET);
    CHECK(result == (qword_t) (sqword_t) _EBADF && seek.calls == 3,
            "无效描述符返回 EBADF");

    fdtable_release(fixture.task.files);
    current = NULL;
    return 0;
}

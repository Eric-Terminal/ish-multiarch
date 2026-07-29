#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/pipe.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/resource.h"
#include "kernel/task.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s（第 %d 行）\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
};

static bool fixture_init(struct fixture *fixture, rlim_t_ nofile) {
    *fixture = (struct fixture) {};
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {
        .cur = nofile,
        .max = nofile,
    };
    fixture->task.group = &fixture->group;
    fixture->task.uid = 1000;
    fixture->task.gid = 1000;
    fixture->task.files = fdtable_new(3);
    return !IS_ERR(fixture->task.files);
}

static void fixture_destroy(struct fixture *fixture) {
    if (fixture->task.files != NULL &&
            !IS_ERR(fixture->task.files)) {
        fdtable_release(fixture->task.files);
        fixture->task.files = NULL;
    }
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 &&
            fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool read_exact(int fd, const void *expected, size_t length) {
    unsigned char actual[64];
    if (length > sizeof(actual))
        return false;
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(fd, actual + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += (size_t) count;
    }
    return memcmp(actual, expected, length) == 0;
}

static bool read_would_block(int fd) {
    unsigned char byte;
    errno = 0;
    return read(fd, &byte, sizeof(byte)) < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK);
}

static bool host_fd_is_closed(int fd) {
    errno = 0;
    return fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

static void test_three_distinct_streams(void) {
    struct fixture fixture;
    CHECK(fixture_init(&fixture, 3), "建立三路标准流测试 task");
    if (IS_ERR(fixture.task.files))
        return;

    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int error_output[2] = {-1, -1};
    CHECK(pipe(input) == 0 && pipe(output) == 0 &&
            pipe(error_output) == 0, "建立三组宿主管道");
    if (input[0] < 0 || output[0] < 0 || error_output[0] < 0) {
        fixture_destroy(&fixture);
        return;
    }

    int install_error = create_host_stdio(
            &fixture.task, input[0], output[1], error_output[1]);
    input[0] = -1;
    output[1] = -1;
    error_output[1] = -1;
    CHECK(install_error == 0, "事务安装 guest 0/1/2");
    CHECK(set_nonblocking(output[0]) &&
            set_nonblocking(error_output[0]),
            "将宿主输出读端设为非阻塞");

    static const char input_bytes[] = "stdin";
    char guest_input[sizeof(input_bytes) - 1];
    CHECK(write(input[1], input_bytes, sizeof(input_bytes) - 1) ==
            (ssize_t) (sizeof(input_bytes) - 1),
            "宿主写入 stdin");
    CHECK(file_read_task(
                &fixture.task, 0, guest_input, sizeof(guest_input)) ==
                    (ssize_t) sizeof(guest_input) &&
            memcmp(guest_input, input_bytes, sizeof(guest_input)) == 0,
            "guest fd 0 只读取 stdin");

    static const char stdout_bytes[] = "stdout";
    CHECK(file_write_task(
                &fixture.task, 1,
                stdout_bytes, sizeof(stdout_bytes) - 1) ==
                    (ssize_t) (sizeof(stdout_bytes) - 1),
            "guest 写入 stdout");
    CHECK(read_exact(
                output[0], stdout_bytes, sizeof(stdout_bytes) - 1),
            "宿主 stdout 收到对应字节");
    CHECK(read_would_block(error_output[0]),
            "stdout 不得串入 stderr");

    static const char stderr_bytes[] = "stderr";
    CHECK(file_write_task(
                &fixture.task, 2,
                stderr_bytes, sizeof(stderr_bytes) - 1) ==
                    (ssize_t) (sizeof(stderr_bytes) - 1),
            "guest 写入 stderr");
    CHECK(read_exact(
                error_output[0], stderr_bytes,
                sizeof(stderr_bytes) - 1),
            "宿主 stderr 收到对应字节");
    CHECK(read_would_block(output[0]),
            "stderr 不得串入 stdout");

    CHECK(file_write_task(&fixture.task, 0, "x", 1) == _EBADF &&
            file_read_task(&fixture.task, 1, guest_input, 1) == _EBADF &&
            file_read_task(&fixture.task, 2, guest_input, 1) == _EBADF,
            "三路标准流保留各自访问方向");

    close(input[1]);
    input[1] = -1;
    CHECK(file_read_task(
                &fixture.task, 0, guest_input, sizeof(guest_input)) == 0,
            "宿主关闭 stdin 后 guest 读取 EOF");

    fixture_destroy(&fixture);
    CHECK(read(output[0], guest_input, 1) == 0 &&
            read(error_output[0], guest_input, 1) == 0,
            "fdtable 销毁后两路宿主输出读端收到 EOF");

    close(output[0]);
    close(error_output[0]);
}

static struct fd *occupy_guest_fd_two(
        struct fixture *fixture, int peer[2]) {
    if (pipe(peer) < 0)
        return ERR_PTR(_EIO);
    struct fd *occupied = file_pipe_wrap_host_fd(
            &fixture->task, peer[0]);
    peer[0] = -1;
    if (IS_ERR(occupied))
        return occupied;

    fd_retain(occupied);
    fd_retain(occupied);
    if (f_install_task(&fixture->task, occupied, 0) != 0 ||
            f_install_task(&fixture->task, occupied, 0) != 1 ||
            f_install_task(&fixture->task, occupied, 0) != 2) {
        return ERR_PTR(_EIO);
    }
    f_close_task(&fixture->task, 0);
    f_close_task(&fixture->task, 1);
    return occupied;
}

static void test_partial_failure_rolls_back(void) {
    struct fixture fixture;
    CHECK(fixture_init(&fixture, 3), "建立失败回滚测试 task");
    if (IS_ERR(fixture.task.files))
        return;

    int occupied_peer[2] = {-1, -1};
    struct fd *occupied = occupy_guest_fd_two(
            &fixture, occupied_peer);
    CHECK(!IS_ERR(occupied), "预占 guest fd 2");
    if (IS_ERR(occupied)) {
        fixture_destroy(&fixture);
        return;
    }

    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int error_output[2] = {-1, -1};
    CHECK(pipe(input) == 0 && pipe(output) == 0 &&
            pipe(error_output) == 0, "建立失败回滚管道");
    if (input[0] < 0 || output[0] < 0 || error_output[0] < 0) {
        fixture_destroy(&fixture);
        return;
    }

    int transferred[3] = {
        input[0],
        output[1],
        error_output[1],
    };
    int install_error = create_host_stdio(
            &fixture.task,
            transferred[0], transferred[1], transferred[2]);
    input[0] = -1;
    output[1] = -1;
    error_output[1] = -1;

    CHECK(install_error == _EMFILE,
            "第三路没有槽位时返回精确 Linux EMFILE");
    CHECK(f_get_task(&fixture.task, 0) == NULL &&
            f_get_task(&fixture.task, 1) == NULL &&
            f_get_task(&fixture.task, 2) == occupied,
            "失败只撤销本次安装并保留既有 fd 2");
    CHECK(host_fd_is_closed(transferred[0]) &&
            host_fd_is_closed(transferred[1]) &&
            host_fd_is_closed(transferred[2]),
            "失败关闭三路已转移宿主 fd");
    CHECK(read(output[0], transferred, 1) == 0 &&
            read(error_output[0], transferred, 1) == 0,
            "失败回滚关闭两路 guest 写端");

    close(input[1]);
    close(output[0]);
    close(error_output[0]);
    close(occupied_peer[1]);
    fixture_destroy(&fixture);
}

static void test_piped_stdio_preserves_host_globals(void) {
    struct fixture fixture;
    CHECK(fixture_init(&fixture, 3), "建立全局标准流所有权测试 task");
    if (IS_ERR(fixture.task.files))
        return;

    int original_flags[3];
    for (int number = 0; number < 3; number++) {
        original_flags[number] = fcntl(number, F_GETFD);
        CHECK(original_flags[number] >= 0,
                "测试进程必须提供宿主标准流");
        if (original_flags[number] < 0) {
            fixture_destroy(&fixture);
            return;
        }
    }

    struct task *previous = current;
    current = &fixture.task;
    int install_error = create_piped_stdio();
    current = previous;
    CHECK(install_error == 0, "从宿主标准流副本建立 guest 0/1/2");
    CHECK(f_get_task(&fixture.task, 0) != NULL &&
            f_get_task(&fixture.task, 1) != NULL &&
            f_get_task(&fixture.task, 2) != NULL,
            "guest 标准流已经安装");

    fixture_destroy(&fixture);
    CHECK(fcntl(STDIN_FILENO, F_GETFD) == original_flags[0] &&
            fcntl(STDOUT_FILENO, F_GETFD) == original_flags[1] &&
            fcntl(STDERR_FILENO, F_GETFD) == original_flags[2],
            "fdtable 销毁不得关闭或改写 App 全局标准流");
}

int main(void) {
    test_three_distinct_streams();
    test_partial_failure_rolls_back();
    test_piped_stdio_preserves_host_globals();

    if (failures == 0) {
        puts("宿主标准流事务回归通过");
        return 0;
    }
    fprintf(stderr, "宿主标准流事务回归失败：%d 项\n", failures);
    return 1;
}

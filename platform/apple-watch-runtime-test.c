#include "platform/apple-watch-runtime.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"

#define TEST_OUTPUT_CAPACITY (64 * 1024)

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s\n", message); \
        failures++; \
    } \
} while (0)

static void check_idle(const char *message) {
    CHECK(ish_watch_runtime_current_phase() == ISH_WATCH_RUNTIME_IDLE,
            message);
    CHECK(ish_watch_runtime_last_error() == 0,
            "参数校验失败不应记录 runtime 错误");
}

static void test_output_overflow(void) {
    const size_t extra = 7;
    const size_t source_length = TEST_OUTPUT_CAPACITY + extra;
    unsigned char *source = malloc(source_length);
    unsigned char *output = malloc(TEST_OUTPUT_CAPACITY);
    CHECK(source != NULL && output != NULL,
            "应能分配终端环形缓冲测试数据");
    if (source == NULL || output == NULL) {
        free(source);
        free(output);
        return;
    }

    for (size_t i = 0; i < source_length; i++)
        source[i] = (unsigned char) (i & 0xff);
    ish_watch_runtime_test_append_output(source, source_length);

    uint64_t dropped = 0;
    size_t length = ish_watch_runtime_read_output(
            output, TEST_OUTPUT_CAPACITY, &dropped);
    CHECK(length == TEST_OUTPUT_CAPACITY,
            "终端环形缓冲应保留最新的 64 KiB 输出");
    CHECK(dropped == extra,
            "终端环形缓冲应报告精确的丢弃字节数");
    CHECK(memcmp(output, source + extra, TEST_OUTPUT_CAPACITY) == 0,
            "终端环形缓冲溢出后应保持最新字节顺序");

    ish_watch_runtime_test_append_output(
            source, TEST_OUTPUT_CAPACITY - 4);
    ish_watch_runtime_test_append_output(
            source + TEST_OUTPUT_CAPACITY - 4, 8);
    dropped = 0;
    length = ish_watch_runtime_read_output(
            output, TEST_OUTPUT_CAPACITY, &dropped);
    CHECK(length == TEST_OUTPUT_CAPACITY,
            "多次小块写入溢出后应保留完整容量");
    CHECK(dropped == 4,
            "多次小块写入应报告被覆盖的四个字节");
    CHECK(memcmp(output, source + 4, TEST_OUTPUT_CAPACITY) == 0,
            "跨环形尾部的读取应保持字节顺序");

    dropped = UINT64_MAX;
    CHECK(ish_watch_runtime_read_output(
            output, TEST_OUTPUT_CAPACITY, &dropped) == 0,
            "读取后终端环形缓冲应为空");
    CHECK(dropped == 0,
            "读取后应清零终端丢弃计数");
    free(source);
    free(output);
}

int main(void) {
    unsigned char output;
    uint64_t dropped = UINT64_MAX;
    CHECK(ish_watch_runtime_current_phase() == ISH_WATCH_RUNTIME_IDLE,
            "新进程中的 Watch runtime 应为空闲态");
    CHECK(ish_watch_runtime_last_error() == 0,
            "空闲态不应携带旧错误");
    CHECK(ish_watch_runtime_read_output(
            &output, sizeof(output), &dropped) == 0,
            "启动前没有终端输出");
    CHECK(dropped == 0,
            "启动前没有被丢弃的终端输出");
    CHECK(ish_watch_runtime_send_input("x", 1) == _EAGAIN,
            "启动前拒绝终端输入");
    CHECK(ish_watch_runtime_send_input(NULL, 1) == _EINVAL,
            "拒绝无字节地址的非空输入");
    CHECK(ish_watch_runtime_send_input(NULL, 0) == 0,
            "允许无字节地址的空输入");
    CHECK(ish_watch_runtime_send_input(
            "x", (size_t) SSIZE_MAX + 1) == _EMSGSIZE,
            "拒绝无法由 ssize_t 表示的输入长度");
    CHECK(ish_watch_runtime_set_window_size(40, 18) == _EAGAIN,
            "启动前拒绝窗口尺寸更新");
    CHECK(ish_watch_runtime_set_window_size(0, 18) == _EINVAL,
            "拒绝零列窗口尺寸");
    CHECK(ish_watch_runtime_set_window_size(40, 0) == _EINVAL,
            "拒绝零行窗口尺寸");

    CHECK(ish_watch_runtime_start(
            NULL, "/tmp", "/tmp/ishsock") == _EINVAL,
            "拒绝缺失 seed 路径");
    check_idle("缺失 seed 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "", "/tmp", "/tmp/ishsock") == _EINVAL,
            "拒绝空 seed 路径");
    check_idle("空 seed 路径不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", NULL, "/tmp/ishsock") == _EINVAL,
            "拒绝缺失持久化目录");
    check_idle("缺失持久化目录不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "", "/tmp/ishsock") == _EINVAL,
            "拒绝空持久化目录");
    check_idle("空持久化目录不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", NULL) == _EINVAL,
            "拒绝缺失 socket 前缀");
    check_idle("缺失 socket 前缀不消耗一次性启动机会");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "") == _EINVAL,
            "拒绝空 socket 前缀");
    check_idle("空 socket 前缀不消耗一次性启动机会");

    char long_socket_prefix[256];
    memset(long_socket_prefix, 's', sizeof(long_socket_prefix) - 1);
    long_socket_prefix[sizeof(long_socket_prefix) - 1] = '\0';
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", long_socket_prefix) == _ENAMETOOLONG,
            "拒绝无法放入 sockaddr_un 的 socket 前缀");
    check_idle("过长 socket 前缀不消耗一次性启动机会");

    test_output_overflow();

    char long_parent[PATH_MAX + 1];
    memset(long_parent, 'p', sizeof(long_parent) - 1);
    long_parent[sizeof(long_parent) - 1] = '\0';
    int start_error = ish_watch_runtime_start(
            "/dev/null", long_parent, "/tmp/ishsock");
    CHECK(start_error == _ENAMETOOLONG,
            "rootfs 宿主错误应映射为负 Linux errno");
    CHECK(ish_watch_runtime_current_phase() == ISH_WATCH_RUNTIME_FAILED,
            "rootfs 安装失败后 runtime 应进入失败态");
    CHECK(ish_watch_runtime_last_error() == start_error,
            "失败态应保留公共 API 返回的同一错误");
    CHECK(ish_watch_runtime_start(
            "/tmp", "/tmp", "/tmp/ishsock") == _EALREADY,
            "失败后拒绝第二次启动全局 guest");

    if (failures == 0)
        puts("Watch runtime 公共边界回归通过");
    return failures == 0 ? 0 : 1;
}

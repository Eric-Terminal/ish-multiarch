#include "sdk/iSHApple/Headers/iSHApple.h"

#include <stdint.h>
#include <stdio.h>

#include "kernel/errno.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s（第 %d 行）\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

int main(void) {
    const char *arguments[] = {"/bin/sh", "-l"};
    const char *environment[] = {
        "TERM=xterm-256color",
        "PATH=/usr/bin:/bin",
    };
    struct ish_apple_terminal_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(spec),
        .columns = 80,
        .rows = 24,
        .terminal_id = 42,
        .executable = "/bin/sh",
        .arguments = arguments,
        .environment = environment,
        .working_directory = "/",
        .argument_count = 2,
        .environment_count = 2,
    };
    ish_apple_terminal_session *session = (void *) 1;

    CHECK(ish_apple_terminal_session_start(
            &spec, NULL) == _EINVAL,
            "拒绝空 session_out");

    uint32_t version = spec.version;
    spec.version++;
    CHECK(ish_apple_terminal_session_start(
            &spec, &session) == _ENOTSUP && session == NULL,
            "拒绝未知终端配置版本且不发布句柄");
    spec.version = version;

    spec.reserved[1] = 1;
    session = (void *) 1;
    CHECK(ish_apple_terminal_session_start(
            &spec, &session) == _EINVAL && session == NULL,
            "拒绝非零保留字段");
    spec.reserved[1] = 0;

    spec.columns = 0;
    session = (void *) 1;
    CHECK(ish_apple_terminal_session_start(
            &spec, &session) == _EINVAL && session == NULL,
            "拒绝零列 PTY");
    spec.columns = 80;

    session = (void *) 1;
    CHECK(ish_apple_terminal_session_start(
            &spec, &session) == _EAGAIN && session == NULL,
            "runtime 启动前返回真实可用性错误且不泄漏句柄");

    return failures == 0 ? 0 : 1;
}

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sdk/iSHApple/Headers/iSHApple.h"

// 与客户端相同的 runtime/command API，供真实 Alpine 网络和诊断验收使用。
static void stream_output(void *context, ish_apple_command_session *session,
        uint64_t request_id, uint32_t stream, const void *bytes,
        uint32_t length, int32_t error) {
    (void) context;
    (void) session;
    (void) request_id;
    if (length != 0)
        fwrite(bytes, 1, length,
                stream == ISH_APPLE_COMMAND_STREAM_STDOUT ? stdout : stderr);
    if (error != 0)
        fprintf(stderr, "命令流错误：%d\n", error);
}

static void completed(void *context, ish_apple_command_session *session,
        const struct ish_apple_command_result_v1 *result) {
    (void) context;
    (void) session;
    (void) result;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "用法：%s <隔离 fakefs 根目录> <shell 脚本>\n", argv[0]);
        return 2;
    }
    char root_data[PATH_MAX];
    if (snprintf(root_data, sizeof(root_data), "%s/data", argv[1]) >=
            (int) sizeof(root_data))
        return 2;
    char shared[] = "/tmp/ish-command-network.XXXXXX";
    if (mkdtemp(shared) == NULL)
        return 2;
    char socket_prefix[PATH_MAX];
    snprintf(socket_prefix, sizeof(socket_prefix), "%s/sock", shared);
    const struct ish_apple_runtime_spec_v1 runtime = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(runtime),
        .root_data = root_data,
        .shared_directory = shared,
        .socket_prefix = socket_prefix,
        .hostname = "network-test",
        .boot_command = "exec /bin/sleep 3600",
    };
    int status = ish_apple_runtime_start(&runtime);
    if (status < 0) {
        fprintf(stderr, "启动失败：%d\n", status);
        rmdir(shared);
        return 2;
    }
    const char *arguments[] = {"/bin/sh", "-c", argv[2]};
    const char *environment[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/root"};
    const struct ish_apple_command_spec_v1 command = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(command),
        .request_id = 1,
        .timeout_milliseconds = 30000,
        .executable = arguments[0], .arguments = arguments, .argument_count = 3,
        .environment = environment, .environment_count = 2,
    };
    const struct ish_apple_command_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(callbacks),
        .stream = stream_output, .completed = completed,
    };
    ish_apple_command_session *session = NULL;
    status = ish_apple_command_session_start(&command, &callbacks, &session);
    struct ish_apple_command_result_v1 result = {0};
    if (status == 0) {
        ish_apple_command_session_close_stdin(session);
        status = ish_apple_command_session_wait(session, &result);
        ish_apple_command_session_release(session);
    }
    uint32_t count = 0;
    struct ish_apple_diagnostic_event_v1 events[64];
    int diagnostic_status = ish_apple_diagnostics_drain(
            ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND, 1, events, 64, &count);
    for (uint32_t i = 0; i < count; i++)
        fprintf(stderr, "诊断：process=%s syscall=%s(%" PRIu64 ") errno=%d\n",
                events[i].process_name, events[i].syscall_name,
                events[i].syscall_number, events[i].linux_error);
    fprintf(stderr, "命令结果：status=%d reason=%d exit=%d elapsed=%" PRIu64
            "ms diagnostics=%u\n", status, result.reason, result.exit_code,
            result.elapsed_milliseconds, count);
    int stop_status = ish_apple_runtime_stop();
    rmdir(shared);
    if (status != 0 || diagnostic_status != 0 || stop_status != 0 ||
            result.reason != ISH_APPLE_COMMAND_COMPLETION_EXITED)
        return 2;
    // 成功退出也必须检查兼容性事件：libc 可能忽略不支持调用的返回值。
    return result.exit_code == 0 && count == 0 ? 0 : 1;
}

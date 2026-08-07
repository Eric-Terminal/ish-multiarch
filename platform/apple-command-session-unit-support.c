#include "platform/apple-command-session-unit-support.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "platform/apple-command-session.h"

extern void ish_apple_command_session_test_fail_worker_once(
        uint32_t ordinal);
extern void ish_apple_command_session_test_fail_signal_once(
        int32_t linux_error);
extern uint32_t ish_apple_command_session_test_live_sessions(void);

static int failures;
static const char *test_executable;
static uint64_t next_request_id = UINT64_C(0xf000000000000000);

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s（第 %d 行）\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

struct callback_context {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t stdout_bytes;
    uint64_t stderr_bytes;
    int32_t stdout_error;
    int32_t stderr_error;
    uint32_t completion_count;
    struct ish_apple_command_result_v1 result;
};

static struct timespec deadline_after_seconds(int seconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

static void context_init(struct callback_context *context) {
    *context = (struct callback_context) {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
}

static void context_destroy(struct callback_context *context) {
    pthread_cond_destroy(&context->changed);
    pthread_mutex_destroy(&context->lock);
}

static void stream_callback(
        void *opaque,
        struct ish_apple_command_session *session,
        uint64_t request_id,
        uint32_t stream,
        const void *bytes,
        uint32_t length,
        int32_t terminal_error) {
    (void) session;
    (void) request_id;
    (void) bytes;
    struct callback_context *context = opaque;
    pthread_mutex_lock(&context->lock);
    if (stream == ISH_APPLE_COMMAND_STREAM_STDOUT) {
        context->stdout_bytes += length;
        if (length == 0)
            context->stdout_error = terminal_error;
    } else {
        context->stderr_bytes += length;
        if (length == 0)
            context->stderr_error = terminal_error;
    }
    pthread_cond_broadcast(&context->changed);
    pthread_mutex_unlock(&context->lock);
}

static void completion_callback(
        void *opaque,
        struct ish_apple_command_session *session,
        const struct ish_apple_command_result_v1 *result) {
    (void) session;
    struct callback_context *context = opaque;
    pthread_mutex_lock(&context->lock);
    context->completion_count++;
    context->result = *result;
    pthread_cond_broadcast(&context->changed);
    pthread_mutex_unlock(&context->lock);
}

static bool context_wait(
        struct callback_context *context,
        bool wait_for_output) {
    struct timespec deadline = deadline_after_seconds(10);
    pthread_mutex_lock(&context->lock);
    while ((wait_for_output ?
            context->stdout_bytes == 0 :
            context->completion_count == 0)) {
        if (pthread_cond_timedwait(
                &context->changed, &context->lock,
                &deadline) == ETIMEDOUT)
            break;
    }
    bool ready = wait_for_output ?
            context->stdout_bytes != 0 :
            context->completion_count != 0;
    pthread_mutex_unlock(&context->lock);
    return ready;
}

static int32_t start_child(
        const char *mode,
        const char *value,
        uint64_t request_id,
        uint32_t timeout_milliseconds,
        uint64_t output_byte_limit,
        const struct ish_apple_command_callbacks_v1 *callbacks,
        struct ish_apple_command_session **session_out) {
    const char *arguments[] = {
        test_executable,
        mode,
        value == NULL ? "" : value,
    };
    struct ish_apple_command_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(spec),
        .timeout_milliseconds = timeout_milliseconds,
        .request_id = request_id,
        .output_byte_limit = output_byte_limit,
        .executable = test_executable,
        .arguments = arguments,
        .argument_count = 3,
    };
    return ish_apple_command_session_start(
            &spec, callbacks, session_out);
}

static int32_t start_basic(
        const char *mode,
        const char *value,
        uint64_t request_id,
        uint32_t timeout_milliseconds,
        uint64_t output_byte_limit,
        struct callback_context *context,
        struct ish_apple_command_session **session_out) {
    struct ish_apple_command_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(callbacks),
        .context = context,
        .stream = stream_callback,
        .completed = completion_callback,
    };
    return start_child(
            mode, value, request_id,
            timeout_milliseconds, output_byte_limit,
            &callbacks, session_out);
}

static bool wait_for_no_live_sessions(void) {
    for (size_t attempt = 0; attempt < 10000; attempt++) {
        if (ish_apple_command_session_test_live_sessions() == 0)
            return true;
        usleep(1000);
    }
    return false;
}

struct contract_context {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct ish_apple_command_session **published_out;
    uint64_t request_id;
    bool stream_entered;
    bool allow_stream_return;
    bool stream_checks_passed;
    bool completion_checks_passed;
    bool callback_retain_passed;
    uint32_t completion_count;
};

static void contract_stream_callback(
        void *opaque,
        struct ish_apple_command_session *session,
        uint64_t request_id,
        uint32_t stream,
        const void *bytes,
        uint32_t length,
        int32_t terminal_error) {
    (void) stream;
    (void) bytes;
    (void) terminal_error;
    struct contract_context *context = opaque;
    struct ish_apple_command_result_v1 result = {};
    bool passed = *context->published_out == session &&
            request_id == context->request_id &&
            ish_apple_command_session_wait(session, &result) ==
                    _EDEADLK;
    pthread_mutex_lock(&context->lock);
    context->stream_checks_passed &= passed;
    if (length != 0 && !context->stream_entered) {
        context->stream_entered = true;
        pthread_cond_broadcast(&context->changed);
        while (!context->allow_stream_return)
            pthread_cond_wait(&context->changed, &context->lock);
        struct ish_apple_command_session *retained =
                ish_apple_command_session_retain(session);
        context->callback_retain_passed = retained == session;
        if (retained != NULL)
            ish_apple_command_session_release(retained);
    }
    pthread_mutex_unlock(&context->lock);
}

static void contract_completion_callback(
        void *opaque,
        struct ish_apple_command_session *session,
        const struct ish_apple_command_result_v1 *result) {
    struct contract_context *context = opaque;
    struct ish_apple_command_result_v1 waited = {};
    bool passed = *context->published_out == session &&
            result->request_id == context->request_id &&
            ish_apple_command_session_wait(session, &waited) ==
                    _EDEADLK;
    pthread_mutex_lock(&context->lock);
    context->completion_checks_passed &= passed;
    context->completion_count++;
    pthread_cond_broadcast(&context->changed);
    pthread_mutex_unlock(&context->lock);
}

static void test_callback_contract(void) {
    struct contract_context context = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
        .request_id = next_request_id++,
        .stream_checks_passed = true,
        .completion_checks_passed = true,
    };
    struct ish_apple_command_session *session = NULL;
    context.published_out = &session;
    struct ish_apple_command_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(callbacks),
        .context = &context,
        .stream = contract_stream_callback,
        .completed = contract_completion_callback,
    };
    CHECK(start_child(
            "--child-pause", NULL, context.request_id,
            0, 0, &callbacks, &session) == 0,
            "启动回调发布与借用生命周期回归");
    if (session != NULL) {
        struct timespec deadline = deadline_after_seconds(10);
        pthread_mutex_lock(&context.lock);
        while (!context.stream_entered) {
            if (pthread_cond_timedwait(
                    &context.changed, &context.lock,
                    &deadline) == ETIMEDOUT)
                break;
        }
        bool entered = context.stream_entered;
        pthread_mutex_unlock(&context.lock);
        CHECK(entered, "输出回调在期限内进入");
        ish_apple_command_session_release(session);
        pthread_mutex_lock(&context.lock);
        context.allow_stream_return = true;
        pthread_cond_broadcast(&context.changed);
        deadline = deadline_after_seconds(10);
        while (context.completion_count == 0) {
            if (pthread_cond_timedwait(
                    &context.changed, &context.lock,
                    &deadline) == ETIMEDOUT)
                break;
        }
        CHECK(context.stream_checks_passed,
                "流回调前已发布 session_out 且 wait 返回 _EDEADLK");
        CHECK(context.completion_checks_passed &&
                context.completion_count == 1,
                "完成回调前已发布 session_out 且 wait 返回 _EDEADLK");
        CHECK(context.callback_retain_passed,
                "公开引用归零后已进入的回调仍可 retain/release");
        pthread_mutex_unlock(&context.lock);
    }
    CHECK(wait_for_no_live_sessions(),
            "回调借用生命周期回归没有残留 session");
    pthread_cond_destroy(&context.changed);
    pthread_mutex_destroy(&context.lock);
}

static void finish_session(
        struct callback_context *context,
        struct ish_apple_command_session *session) {
    if (session == NULL)
        return;
    (void) ish_apple_command_session_cancel(session);
    CHECK(context_wait(context, false),
            "清理活跃命令时在期限内收到完成回调");
    struct ish_apple_command_result_v1 result = {};
    CHECK(ish_apple_command_session_wait(session, &result) == 0,
            "清理活跃命令时等待完成回调退出");
    ish_apple_command_session_release(session);
}

static void test_active_identity_and_dynamic_registry(void) {
    enum { active_count = 12 };
    struct callback_context contexts[active_count + 2];
    struct ish_apple_command_session *sessions[active_count] = {};
    for (size_t index = 0; index < active_count + 2; index++)
        context_init(&contexts[index]);

    uint64_t first_id = next_request_id;
    for (size_t index = 0; index < active_count; index++) {
        uint64_t request_id = next_request_id++;
        CHECK(start_basic(
                "--child-pause", NULL, request_id,
                0, 0, &contexts[index], &sessions[index]) == 0,
                "动态注册表允许大量并发命令启动");
        if (index == 0) {
            struct ish_apple_command_session *duplicate = (void *) 1;
            CHECK(start_basic(
                    "--child-pause", NULL, request_id,
                    0, 0, &contexts[active_count], &duplicate) ==
                            _EEXIST &&
                    duplicate == NULL,
                    "重复活跃 request_id 返回 _EEXIST");
        }
    }
    struct ish_apple_command_session *additional = NULL;
    CHECK(start_basic(
            "--child-pause", NULL, next_request_id++,
            0, 0, &contexts[active_count + 1], &additional) == 0 &&
            additional != NULL,
            "动态注册表不以固定会话配额拒绝额外命令");

    for (size_t index = 0; index < active_count; index++)
        finish_session(&contexts[index], sessions[index]);
    finish_session(&contexts[active_count + 1], additional);
    CHECK(wait_for_no_live_sessions(),
            "活跃命令完成后清空动态注册表和 request_id");

    struct ish_apple_command_session *reused = NULL;
    CHECK(start_basic(
            "--child-token", "request-id-reused", first_id,
            0, 0, &contexts[active_count], &reused) == 0,
            "完成后可复用 request_id");
    if (reused != NULL) {
        CHECK(context_wait(&contexts[active_count], false),
                "复用 request_id 的命令正常完成");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(reused, &result) == 0,
                "复用 request_id 的完成回调已经退出");
        ish_apple_command_session_release(reused);
    }
    for (size_t index = 0; index < active_count + 2; index++)
        context_destroy(&contexts[index]);
}

static void test_resource_limits(void) {
    struct callback_context output;
    context_init(&output);
    struct ish_apple_command_session *session = NULL;
    const uint64_t limit = UINT64_C(12345);
    CHECK(start_basic(
            "--child-large", NULL, next_request_id++,
            0, limit, &output, &session) == 0,
            "启动小输出上限命令");
    if (session != NULL) {
        CHECK(context_wait(&output, false),
                "输出达到上限后在期限内完成");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0,
                "输出上限完成回调已经退出");
        CHECK(output.result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_OUTPUT_LIMIT &&
                output.stdout_bytes + output.stderr_bytes == limit &&
                output.result.stdout_bytes == output.stdout_bytes &&
                output.result.stderr_bytes == output.stderr_bytes &&
                (output.stdout_error == _EFBIG ||
                        output.stderr_error == _EFBIG),
                "输出精确截断且 OUTPUT_LIMIT 结果与统计吻合");
        ish_apple_command_session_release(session);
    }
    context_destroy(&output);

    struct callback_context timeout;
    context_init(&timeout);
    session = NULL;
    CHECK(start_basic(
            "--child-pause", NULL, next_request_id++,
            20, 0, &timeout, &session) == 0,
            "启动短超时命令");
    if (session != NULL) {
        CHECK(context_wait(&timeout, false) &&
                timeout.result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_TIMED_OUT &&
                timeout.result.termination_signal == SIGKILL,
                "很短 timeout 产生 TIMED_OUT 结构化结果");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0,
                "timeout 完成回调已经退出");
        ish_apple_command_session_release(session);
    }
    context_destroy(&timeout);

    struct callback_context unlimited;
    context_init(&unlimited);
    session = NULL;
    CHECK(start_basic(
            "--child-pause", NULL, next_request_id++,
            ISH_APPLE_COMMAND_TIMEOUT_MS_DISABLED,
            ISH_APPLE_COMMAND_OUTPUT_BYTES_DISABLED,
            &unlimited, &session) == 0,
            "允许显式关闭命令超时和输出终止阈值");
    if (session != NULL) {
        CHECK(context_wait(&unlimited, true),
                "无限时长命令在取消前保持运行并产生输出");
        CHECK(ish_apple_command_session_cancel(session) == 0 &&
                context_wait(&unlimited, false) &&
                unlimited.result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_CANCELLED,
                "无限时长命令仍可由调用方明确取消");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0,
                "无限时长命令取消后的完成回调已经退出");
        ish_apple_command_session_release(session);
    }
    context_destroy(&unlimited);
}

static int count_open_fds(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
}

static void test_worker_rollback_and_cancel_retry(void) {
    int baseline = count_open_fds();
    for (uint32_t ordinal = 1; ordinal <= 3; ordinal++) {
        struct callback_context context;
        context_init(&context);
        ish_apple_command_session_test_fail_worker_once(ordinal);
        struct ish_apple_command_session *session = (void *) 1;
        CHECK(start_basic(
                "--child-token", "worker-failure",
                next_request_id++, 0, 0,
                &context, &session) == _EAGAIN &&
                session == NULL &&
                context.completion_count == 0 &&
                count_open_fds() == baseline &&
                ish_apple_command_session_test_live_sessions() == 0,
                "第 1/2/3 个 worker 故障均完整回滚");
        context_destroy(&context);
    }

    struct callback_context retry;
    context_init(&retry);
    struct ish_apple_command_session *session = NULL;
    CHECK(start_basic(
            "--child-pause", NULL, next_request_id++,
            0, 0, &retry, &session) == 0,
            "启动 cancel 瞬时故障重试命令");
    if (session != NULL) {
        CHECK(context_wait(&retry, true),
                "cancel 重试前命令已经就绪");
        ish_apple_command_session_test_fail_signal_once(_EIO);
        CHECK(ish_apple_command_session_cancel(session) == _EIO &&
                ish_apple_command_session_cancel(session) == 0,
                "backend 瞬时失败后第二次 cancel 可重试");
        CHECK(context_wait(&retry, false) &&
                retry.result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_CANCELLED,
                "重试成功后交付 CANCELLED 结果");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0,
                "cancel 重试完成回调已经退出");
        ish_apple_command_session_release(session);
    }
    context_destroy(&retry);
}

int ish_apple_command_session_run_unit_contract_tests(
        const char *executable) {
    test_executable = executable;
    failures = 0;
    test_callback_contract();
    test_active_identity_and_dynamic_registry();
    test_resource_limits();
    test_worker_rollback_and_cancel_retry();
    CHECK(wait_for_no_live_sessions(),
            "扩展命令会话回归结束后没有残留 session");
    return failures;
}

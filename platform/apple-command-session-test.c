#include "platform/apple-command-session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "platform/apple-command-session-unit-support.h"

#define TEST_FAIL_PREPARE 1
#define TEST_FAIL_WORKER 2
#define TEST_FAIL_COMMIT 3

extern void ish_apple_command_session_test_fail_once(int stage);
extern uint32_t ish_apple_command_session_test_live_sessions(void);

static int failures;
static char test_executable[PATH_MAX];
static _Atomic uint64_t next_request_id = 1;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s（第 %d 行）\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

struct byte_buffer {
    unsigned char *bytes;
    size_t length;
    size_t capacity;
};

struct callback_context {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct byte_buffer output;
    struct byte_buffer error_output;
    bool stdout_ended;
    bool stderr_ended;
    int32_t stdout_error;
    int32_t stderr_error;
    uint32_t event_order;
    uint32_t stdout_end_order;
    uint32_t stderr_end_order;
    uint32_t exit_order;
    uint32_t exit_count;
    struct ish_apple_command_result_v1 result;
};

static bool buffer_append(
        struct byte_buffer *buffer,
        const void *bytes,
        size_t length) {
    if (length > SIZE_MAX - buffer->length)
        return false;
    size_t required = buffer->length + length;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        unsigned char *replacement =
                realloc(buffer->bytes, capacity);
        if (replacement == NULL)
            return false;
        buffer->bytes = replacement;
        buffer->capacity = capacity;
    }
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length = required;
    return true;
}

static void context_init(struct callback_context *context) {
    *context = (struct callback_context) {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
}

static void context_destroy(struct callback_context *context) {
    free(context->error_output.bytes);
    free(context->output.bytes);
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
    struct callback_context *context = opaque;
    pthread_mutex_lock(&context->lock);
    struct byte_buffer *buffer =
            stream == ISH_APPLE_COMMAND_STREAM_STDOUT ?
            &context->output : &context->error_output;
    if (length != 0 && !buffer_append(buffer, bytes, length))
        failures++;
    if (request_id == 0)
        failures++;
    if (length == 0) {
        context->event_order++;
        if (stream == ISH_APPLE_COMMAND_STREAM_STDOUT) {
            context->stdout_ended = true;
            context->stdout_error = terminal_error;
            context->stdout_end_order = context->event_order;
        } else {
            context->stderr_ended = true;
            context->stderr_error = terminal_error;
            context->stderr_end_order = context->event_order;
        }
    }
    pthread_cond_broadcast(&context->changed);
    pthread_mutex_unlock(&context->lock);
}

static void exit_callback(
        void *opaque,
        struct ish_apple_command_session *session,
        const struct ish_apple_command_result_v1 *result) {
    (void) session;
    struct callback_context *context = opaque;
    pthread_mutex_lock(&context->lock);
    context->event_order++;
    context->exit_order = context->event_order;
    context->exit_count++;
    context->result = *result;
    pthread_cond_broadcast(&context->changed);
    pthread_mutex_unlock(&context->lock);
}

static struct timespec deadline_after_seconds(int seconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

static bool buffer_contains(
        const struct byte_buffer *buffer, const char *needle) {
    size_t needle_length = strlen(needle);
    if (needle_length == 0)
        return true;
    if (needle_length > buffer->length)
        return false;
    for (size_t index = 0;
            index + needle_length <= buffer->length; index++) {
        if (memcmp(buffer->bytes + index,
                    needle, needle_length) == 0)
            return true;
    }
    return false;
}

static bool context_wait_for_output(
        struct callback_context *context, const char *needle) {
    struct timespec deadline = deadline_after_seconds(10);
    pthread_mutex_lock(&context->lock);
    while (!buffer_contains(&context->output, needle) &&
            !context->stdout_ended) {
        if (pthread_cond_timedwait(
                &context->changed, &context->lock,
                &deadline) == ETIMEDOUT)
            break;
    }
    bool found = buffer_contains(&context->output, needle);
    pthread_mutex_unlock(&context->lock);
    return found;
}

static bool context_wait_for_exit(struct callback_context *context) {
    struct timespec deadline = deadline_after_seconds(10);
    pthread_mutex_lock(&context->lock);
    while (context->exit_count == 0) {
        if (pthread_cond_timedwait(
                &context->changed, &context->lock,
                &deadline) == ETIMEDOUT)
            break;
    }
    bool exited = context->exit_count != 0;
    pthread_mutex_unlock(&context->lock);
    return exited;
}

static int32_t start_child(
        const char *mode,
        const char *value,
        const char *working_directory,
        const char *const *environment,
        uint32_t environment_count,
        struct callback_context *context,
        struct ish_apple_command_session **session) {
    const char *arguments[] = {
        test_executable,
        mode,
        value == NULL ? "" : value,
    };
    struct ish_apple_command_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(spec),
        .request_id = atomic_fetch_add_explicit(
                &next_request_id, 1, memory_order_relaxed),
        .executable = test_executable,
        .arguments = arguments,
        .argument_count = 3,
        .working_directory = working_directory,
        .environment = environment,
        .environment_count = environment_count,
    };
    struct ish_apple_command_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(callbacks),
        .context = context,
        .stream = stream_callback,
        .completed = exit_callback,
    };
    return ish_apple_command_session_start(
            &spec, &callbacks, session);
}

static bool write_all_fd(int fd, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    while (length != 0) {
        ssize_t count = write(fd, cursor, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        cursor += count;
        length -= (size_t) count;
    }
    return true;
}

static int child_roundtrip(void) {
    unsigned char input[4096];
    size_t length = 0;
    for (;;) {
        ssize_t count = read(
                STDIN_FILENO, input + length,
                sizeof(input) - length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        length += (size_t) count;
        if (length == sizeof(input))
            break;
    }

    char directory[PATH_MAX];
    const char *environment = getenv("BRIDGE_VALUE");
    if (getcwd(directory, sizeof(directory)) == NULL ||
            environment == NULL)
        return 90;
    dprintf(STDOUT_FILENO, "cwd=%s\nenv=%s\nstdin=", directory, environment);
    if (!write_all_fd(STDOUT_FILENO, input, length) ||
            !write_all_fd(STDOUT_FILENO, "\n", 1))
        return 91;
    dprintf(STDERR_FILENO, "stderr=%zu\n", length);
    return 37;
}

static int child_large_output(void) {
    unsigned char output[4096];
    unsigned char error_output[4096];
    memset(output, 'O', sizeof(output));
    memset(error_output, 'E', sizeof(error_output));
    for (size_t index = 0; index < 128; index++) {
        if (!write_all_fd(STDOUT_FILENO, output, sizeof(output)) ||
                !write_all_fd(
                        STDERR_FILENO, error_output,
                        sizeof(error_output)))
            return 92;
    }
    return 0;
}

static volatile sig_atomic_t interrupted;

static void note_interrupt(int signal_number) {
    (void) signal_number;
    interrupted = 1;
}

static int child_interrupt(void) {
    struct sigaction action = {
        .sa_handler = note_interrupt,
    };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0)
        return 93;
    if (!write_all_fd(STDOUT_FILENO, "ready\n", 6))
        return 94;
    while (!interrupted)
        pause();
    return 44;
}

static int child_pause(void) {
    if (!write_all_fd(STDOUT_FILENO, "ready\n", 6))
        return 95;
    for (;;)
        pause();
}

static int child_token(const char *token) {
    write_all_fd(STDOUT_FILENO, token, strlen(token));
    write_all_fd(STDERR_FILENO, token, strlen(token));
    return 0;
}

static int run_child(int argc, char **argv) {
    if (argc < 2)
        return -1;
    if (strcmp(argv[1], "--child-roundtrip") == 0)
        return child_roundtrip();
    if (strcmp(argv[1], "--child-large") == 0)
        return child_large_output();
    if (strcmp(argv[1], "--child-signal") == 0) {
        raise(SIGTERM);
        return 96;
    }
    if (strcmp(argv[1], "--child-interrupt") == 0)
        return child_interrupt();
    if (strcmp(argv[1], "--child-pause") == 0)
        return child_pause();
    if (strcmp(argv[1], "--child-token") == 0)
        return child_token(argc >= 3 ? argv[2] : "");
    return -1;
}

static void check_completion_order(
        const struct callback_context *context,
        const char *message) {
    CHECK(context->exit_count == 1 &&
            context->stdout_ended &&
            context->stderr_ended &&
            context->stdout_error == 0 &&
            context->stderr_error == 0 &&
            context->exit_order > context->stdout_end_order &&
            context->exit_order > context->stderr_end_order,
            message);
}

static void test_roundtrip_cwd_environment(void) {
    char directory[] = "/tmp/ish-command-cwd-XXXXXX";
    CHECK(mkdtemp(directory) != NULL, "建立 cwd 测试目录");
    const char *environment[] = {"BRIDGE_VALUE=独立环境"};
    struct callback_context context;
    context_init(&context);
    struct ish_apple_command_session *session = NULL;
    CHECK(start_child(
            "--child-roundtrip", NULL, directory,
            environment, 1, &context, &session) == 0,
            "以结构化 argv、cwd 与 env 启动命令");
    if (session != NULL) {
        uint32_t rejected = UINT32_MAX;
        CHECK(ish_apple_command_session_write_stdin(
                session,
                (const void *) (uintptr_t) 1,
                UINT32_MAX,
                &rejected) == ISH_APPLE_LINUX_EMSGSIZE &&
                        rejected == 0,
                "stdin 在读取缓冲区前拒绝超过 ssize_t 的单次写入");
        static const char input[] = "stdin-roundtrip";
        uint32_t accepted = 0;
        CHECK(ish_apple_command_session_write_stdin(
                session, input, sizeof(input) - 1, &accepted) == 0 &&
                        accepted == sizeof(input) - 1,
                "stdin 独立管道完整写入");
        CHECK(ish_apple_command_session_close_stdin(session) == 0 &&
                ish_apple_command_session_close_stdin(session) == 0,
                "stdin EOF 操作可重复");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0 &&
                result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_EXITED &&
                result.exit_code == 37 &&
                result.termination_signal == 0,
                "wait 结构化返回 exit 37");
        struct ish_apple_command_result_v1 second_result = {};
        CHECK(ish_apple_command_session_wait(
                session, &second_result) == 0 &&
                memcmp(&second_result, &result, sizeof(result)) == 0,
                "wait 可重复读取同一结果");
        CHECK(buffer_contains(&context.output, directory) &&
                buffer_contains(&context.output, "env=独立环境") &&
                buffer_contains(
                        &context.output, "stdin=stdin-roundtrip") &&
                buffer_contains(&context.error_output, "stderr=15"),
                "cwd、env、stdin、stdout 与 stderr 彼此正确");
        check_completion_order(
                &context, "exit 只在两路 EOF 后恰好交付一次");
        ish_apple_command_session_release(session);
    }
    context_destroy(&context);
    rmdir(directory);
}

static void test_large_output_has_no_drop(void) {
    struct callback_context context;
    context_init(&context);
    struct ish_apple_command_session *session = NULL;
    CHECK(start_child(
            "--child-large", NULL, NULL, NULL, 0,
            &context, &session) == 0,
            "启动双路大输出命令");
    if (session != NULL) {
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0 &&
                result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_EXITED &&
                result.exit_code == 0,
                "大输出命令正常退出");
        CHECK(context.output.length == 128 * 4096 &&
                context.error_output.length == 128 * 4096,
                "stdout 与 stderr 均无静默丢弃");
        bool patterns = context.output.length == 128 * 4096 &&
                context.error_output.length == context.output.length;
        for (size_t index = 0;
                patterns && index < context.output.length; index++) {
            if (context.output.bytes[index] != 'O' ||
                    context.error_output.bytes[index] != 'E') {
                patterns = false;
                break;
            }
        }
        CHECK(patterns, "双路大输出保持内容与顺序");
        check_completion_order(
                &context, "大输出结束事件先于唯一退出事件");
        ish_apple_command_session_release(session);
    }
    context_destroy(&context);
}

static void test_exit_by_signal(void) {
    struct callback_context context;
    context_init(&context);
    struct ish_apple_command_session *session = NULL;
    CHECK(start_child(
            "--child-signal", NULL, NULL, NULL, 0,
            &context, &session) == 0,
            "启动主动信号退出命令");
    if (session != NULL) {
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(session, &result) == 0 &&
                result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_SIGNALED &&
                result.termination_signal == SIGTERM,
                "wait 结构化返回 SIGTERM");
        check_completion_order(
                &context, "信号退出仍等待双路 EOF");
        ish_apple_command_session_release(session);
    }
    context_destroy(&context);
}

static void test_interrupt_and_cancel(void) {
    struct callback_context interrupt_context;
    context_init(&interrupt_context);
    struct ish_apple_command_session *interrupted_session = NULL;
    CHECK(start_child(
            "--child-interrupt", NULL, NULL, NULL, 0,
            &interrupt_context, &interrupted_session) == 0,
            "启动可中断命令");
    if (interrupted_session != NULL) {
        CHECK(context_wait_for_output(
                &interrupt_context, "ready\n"),
                "中断前等待 guest handler 就绪");
        int32_t interrupt_error =
                ish_apple_command_session_interrupt(
                        interrupted_session);
        CHECK(interrupt_error == 0,
                "向独立命令作业发送 SIGINT");
        if (interrupt_error < 0)
            (void) ish_apple_command_session_cancel(
                    interrupted_session);
        bool interrupt_exited =
                context_wait_for_exit(&interrupt_context);
        if (!interrupt_exited)
            (void) ish_apple_command_session_cancel(
                    interrupted_session);
        CHECK(interrupt_exited,
                "SIGINT 后命令应在期限内完成");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(
                interrupted_session, &result) == 0 &&
                result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_EXITED &&
                result.exit_code == 44,
                "SIGINT 由命令处理并返回 exit 44");
        ish_apple_command_session_release(interrupted_session);
    }
    context_destroy(&interrupt_context);

    struct callback_context cancel_context;
    context_init(&cancel_context);
    struct ish_apple_command_session *canceled_session = NULL;
    CHECK(start_child(
            "--child-pause", NULL, NULL, NULL, 0,
            &cancel_context, &canceled_session) == 0,
            "启动可取消命令");
    if (canceled_session != NULL) {
        CHECK(context_wait_for_output(
                &cancel_context, "ready\n"),
                "取消前等待命令就绪");
        CHECK(ish_apple_command_session_cancel(
                canceled_session) == 0 &&
                ish_apple_command_session_cancel(
                        canceled_session) == 0,
                "cancel 可重复且不重复建立退场任务");
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(
                canceled_session, &result) == 0 &&
                result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_CANCELLED &&
                result.termination_signal == SIGKILL,
                "cancel 以结构化取消原因和 SIGKILL 结束作业");
        ish_apple_command_session_release(canceled_session);
    }
    context_destroy(&cancel_context);
}

static void test_concurrent_isolation(void) {
    enum { count = 4 };
    struct callback_context contexts[count];
    struct ish_apple_command_session *sessions[count] = {};
    char tokens[count][24];
    for (size_t index = 0; index < count; index++) {
        context_init(&contexts[index]);
        snprintf(tokens[index], sizeof(tokens[index]),
                "session-%zu-only", index);
        CHECK(start_child(
                "--child-token", tokens[index],
                NULL, NULL, 0,
                &contexts[index], &sessions[index]) == 0,
                "并发启动独立命令");
    }
    for (size_t index = 0; index < count; index++) {
        if (sessions[index] == NULL)
            continue;
        struct ish_apple_command_result_v1 result = {};
        CHECK(ish_apple_command_session_wait(
                sessions[index], &result) == 0 &&
                result.reason ==
                        ISH_APPLE_COMMAND_COMPLETION_EXITED &&
                result.exit_code == 0,
                "并发命令独立完成");
        CHECK(contexts[index].output.length ==
                        strlen(tokens[index]) &&
                contexts[index].error_output.length ==
                        strlen(tokens[index]) &&
                memcmp(contexts[index].output.bytes,
                        tokens[index],
                        strlen(tokens[index])) == 0 &&
                memcmp(contexts[index].error_output.bytes,
                        tokens[index],
                        strlen(tokens[index])) == 0,
                "并发 stdout/stderr 不串流");
        ish_apple_command_session_release(sessions[index]);
    }
    for (size_t index = 0; index < count; index++)
        context_destroy(&contexts[index]);
}

static int count_open_fds(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
}

static void test_error_rollback(void) {
    struct callback_context context;
    context_init(&context);
    int baseline = count_open_fds();

    const char *arguments[] = {"/definitely/missing"};
    struct ish_apple_command_spec_v1 missing = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(missing),
        .request_id = atomic_fetch_add_explicit(
                &next_request_id, 1, memory_order_relaxed),
        .executable = arguments[0],
        .arguments = arguments,
        .argument_count = 1,
    };
    struct ish_apple_command_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(callbacks),
        .context = &context,
        .stream = stream_callback,
        .completed = exit_callback,
    };
    struct ish_apple_command_session *session = (void *) 1;
    CHECK(ish_apple_command_session_start(
            &missing, &callbacks, &session) == _ENOENT &&
            session == NULL,
            "exec 同步失败不发布 session");
    CHECK(count_open_fds() == baseline &&
            context.exit_count == 0,
            "exec 失败完整回滚 fd、线程与回调");

    int stages[] = {
        TEST_FAIL_PREPARE,
        TEST_FAIL_WORKER,
        TEST_FAIL_COMMIT,
    };
    for (size_t index = 0;
            index < sizeof(stages) / sizeof(stages[0]); index++) {
        ish_apple_command_session_test_fail_once(stages[index]);
        session = (void *) 1;
        CHECK(start_child(
                "--child-token", "failure", NULL, NULL, 0,
                &context, &session) < 0 && session == NULL,
                "准备、worker 与提交故障均不发布句柄");
        CHECK(count_open_fds() == baseline &&
                ish_apple_command_session_test_live_sessions() == 0,
                "每个启动阶段故障均无 session 或 fd 泄漏");
    }

    struct ish_apple_command_spec_v1 invalid = missing;
    invalid.version++;
    session = (void *) 1;
    CHECK(ish_apple_command_session_start(
            &invalid, &callbacks, &session) == _ENOTSUP &&
            session == NULL,
            "拒绝未知 ABI 版本且不消耗资源");
    context_destroy(&context);
}

struct release_gate {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    size_t ready;
    bool go;
};

struct release_request {
    struct release_gate *gate;
    struct ish_apple_command_session *session;
};

static void *release_thread(void *opaque) {
    struct release_request *request = opaque;
    pthread_mutex_lock(&request->gate->lock);
    request->gate->ready++;
    pthread_cond_broadcast(&request->gate->changed);
    while (!request->gate->go)
        pthread_cond_wait(
                &request->gate->changed,
                &request->gate->lock);
    pthread_mutex_unlock(&request->gate->lock);
    ish_apple_command_session_release(request->session);
    return NULL;
}

static bool wait_for_no_live_sessions(void) {
    for (size_t attempt = 0; attempt < 10000; attempt++) {
        if (ish_apple_command_session_test_live_sessions() == 0)
            return true;
        usleep(1000);
    }
    return false;
}

static void test_release_race_cancels_orphan(void) {
    struct callback_context context;
    context_init(&context);
    struct ish_apple_command_session *session = NULL;
    CHECK(start_child(
            "--child-pause", NULL, NULL, NULL, 0,
            &context, &session) == 0,
            "启动 release 竞态命令");
    if (session == NULL) {
        context_destroy(&context);
        return;
    }
    CHECK(context_wait_for_output(&context, "ready\n"),
            "release 竞态前等待命令就绪");

    enum { retain_count = 8 };
    struct release_gate gate = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    struct release_request requests[retain_count];
    pthread_t threads[retain_count];
    for (size_t index = 0; index < retain_count; index++) {
        requests[index] = (struct release_request) {
            .gate = &gate,
            .session = ish_apple_command_session_retain(session),
        };
        CHECK(requests[index].session == session,
                "并发 release 前取得独立引用");
        CHECK(pthread_create(
                &threads[index], NULL,
                release_thread, &requests[index]) == 0,
                "启动并发 release 线程");
    }

    pthread_mutex_lock(&gate.lock);
    while (gate.ready != retain_count)
        pthread_cond_wait(&gate.changed, &gate.lock);
    gate.go = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);
    ish_apple_command_session_release(session);

    for (size_t index = 0; index < retain_count; index++)
        pthread_join(threads[index], NULL);
    CHECK(context_wait_for_exit(&context),
            "最后一个公开引用释放后自动完成退场");
    CHECK(context.exit_count == 1 &&
            context.result.reason ==
                    ISH_APPLE_COMMAND_COMPLETION_CANCELLED &&
            context.result.termination_signal == SIGKILL,
            "release 竞态只产生一次结构化取消结果");
    CHECK(wait_for_no_live_sessions(),
            "release 竞态结束后没有 session、worker 或子进程孤儿");

    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.lock);
    context_destroy(&context);
}

int main(int argc, char **argv) {
    int child_result = run_child(argc, argv);
    if (child_result >= 0)
        return child_result;

    signal(SIGPIPE, SIG_IGN);
    CHECK(realpath(argv[0], test_executable) != NULL,
            "取得集成测试自身的绝对路径");
    test_roundtrip_cwd_environment();
    test_large_output_has_no_drop();
    test_exit_by_signal();
    test_interrupt_and_cancel();
    test_concurrent_isolation();
    test_error_rollback();
    test_release_race_cancels_orphan();
    failures += ish_apple_command_session_run_unit_contract_tests(
            test_executable);
    CHECK(wait_for_no_live_sessions(),
            "全部测试结束后没有残留 session");

    if (failures == 0) {
        puts("Apple 结构化命令会话集成回归通过");
        return 0;
    }
    fprintf(stderr, "Apple 结构化命令会话集成回归失败：%d 项\n",
            failures);
    return 1;
}

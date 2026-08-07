#include "sdk/iSHApple/Headers/iSHAppleDiagnostics.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "guest/aarch64/backend.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "platform/apple-diagnostics-private.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Apple 结构化诊断测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct record_thread {
    uint64_t first_request_id;
    uint32_t count;
};

static void *record_filesystem_events(void *opaque) {
    struct record_thread *thread = opaque;
    for (uint32_t index = 0; index < thread->count; index++) {
        ish_apple_diagnostics_record_filesystem(
                ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
                thread->first_request_id + index,
                ISH_APPLE_DIAGNOSTIC_FILESYSTEM_UNSUPPORTED,
                _ENOSYS);
    }
    return NULL;
}

static int test_event_fields_and_filtering(void) {
    const uint64_t command_id = UINT64_C(41);
    const uint64_t guest_file_id = UINT64_C(42);
    uint32_t cleared;
    CHECK(ish_apple_diagnostics_clear(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND,
                    command_id, &cleared) == 0,
            "清理命令测试上下文");
    CHECK(ish_apple_diagnostics_clear(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
                    guest_file_id, &cleared) == 0,
            "清理文件测试上下文");

    struct tgroup group = {
        .host_diagnostic_scope =
                ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND,
        .host_diagnostic_request_id = command_id,
    };
    struct task task = {.group = &group};
    ish_apple_diagnostics_record_undefined_instruction(
            &task,
            UINT64_C(0x400100),
            UINT32_C(0xffffffff),
            SIGILL_,
            AARCH64_BACKEND_C);
    ish_apple_diagnostics_record_filesystem(
            ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
            guest_file_id,
            ISH_APPLE_DIAGNOSTIC_FILESYSTEM_UNSUPPORTED,
            _ENOSYS);
    ish_apple_diagnostics_record_unsupported_syscall(
            &task,
            UINT64_C(0x400200),
            UINT64_C(437),
            _ENOSYS,
            AARCH64_BACKEND_THREADED);

    uint32_t available = 0;
    CHECK(ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND,
                    command_id, NULL, 0, &available) == 0 &&
            available == 2,
            "查询命令上下文而不消费其他 scope");

    struct ish_apple_diagnostic_event_v1 events[2];
    uint32_t count = 0;
    CHECK(ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND,
                    command_id, events, 1, &count) == 0 &&
            count == 1 &&
            events[0].version == ISH_APPLE_ABI_VERSION &&
            events[0].structure_size == sizeof(events[0]) &&
            events[0].category ==
                    ISH_APPLE_DIAGNOSTIC_CATEGORY_INSTRUCTION &&
            events[0].kind ==
                    ISH_APPLE_DIAGNOSTIC_INSTRUCTION_UNDEFINED &&
            events[0].scope ==
                    ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND &&
            events[0].request_id == command_id &&
            events[0].guest_pc == UINT64_C(0x400100) &&
            events[0].opcode == UINT32_C(0xffffffff) &&
            events[0].signal == SIGILL_ &&
            events[0].backend == ISH_APPLE_DIAGNOSTIC_BACKEND_C &&
            events[0].build_identity[0] != '\0',
            "未定义指令事件保留完整诊断字段");

    uint64_t first_sequence = events[0].sequence;
    CHECK(ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND,
                    command_id, events, 2, &count) == 0 &&
            count == 1 &&
            events[0].sequence > first_sequence &&
            events[0].category ==
                    ISH_APPLE_DIAGNOSTIC_CATEGORY_SYSCALL &&
            events[0].kind ==
                    ISH_APPLE_DIAGNOSTIC_SYSCALL_UNSUPPORTED &&
            events[0].guest_pc == UINT64_C(0x400200) &&
            events[0].syscall_number == UINT64_C(437) &&
            strcmp(events[0].syscall_name, "openat2") == 0 &&
            events[0].linux_error == _ENOSYS &&
            events[0].backend ==
                    ISH_APPLE_DIAGNOSTIC_BACKEND_THREADED,
            "未实现 syscall 事件包含编号、名称、errno 与后端");

    CHECK(ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
                    guest_file_id, events, 2, &count) == 0 &&
            count == 1 &&
            events[0].category ==
                    ISH_APPLE_DIAGNOSTIC_CATEGORY_FILESYSTEM &&
            events[0].request_id == guest_file_id,
            "文件系统事件与命令上下文严格隔离");
    return 0;
}

static int test_dynamic_concurrent_registry(void) {
    enum { THREAD_COUNT = 4, EVENTS_PER_THREAD = 17 };
    pthread_t threads[THREAD_COUNT];
    struct record_thread records[THREAD_COUNT];
    for (uint32_t index = 0; index < THREAD_COUNT; index++) {
        records[index] = (struct record_thread) {
            .first_request_id =
                    UINT64_C(1000) + index * EVENTS_PER_THREAD,
            .count = EVENTS_PER_THREAD,
        };
        CHECK(pthread_create(
                        &threads[index], NULL,
                        record_filesystem_events,
                        &records[index]) == 0,
                "并发建立诊断生产线程");
    }
    for (uint32_t index = 0; index < THREAD_COUNT; index++)
        CHECK(pthread_join(threads[index], NULL) == 0,
                "等待诊断生产线程");

    struct ish_apple_diagnostic_event_v1 event;
    for (uint32_t thread = 0; thread < THREAD_COUNT; thread++) {
        for (uint32_t index = 0; index < EVENTS_PER_THREAD; index++) {
            uint32_t count = 0;
            uint64_t request_id =
                    records[thread].first_request_id + index;
            CHECK(ish_apple_diagnostics_drain(
                            ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
                            request_id, &event, 1, &count) == 0 &&
                    count == 1 && event.request_id == request_id,
                    "动态队列在并发写入后按 request ID 精确消费");
        }
    }
    return 0;
}

static int test_validation_and_runtime(void) {
    uint32_t count = 0;
    struct ish_apple_diagnostic_event_v1 event;
    CHECK(ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND,
                    0, &event, 1, &count) == _EINVAL &&
            ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME,
                    1, &event, 1, &count) == _EINVAL &&
            ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME,
                    0, NULL, 1, &count) == _EINVAL,
            "拒绝无效 scope、request ID 与缓冲区组合");

    ish_apple_diagnostics_record_runtime(
            ISH_APPLE_DIAGNOSTIC_RUNTIME_BRIDGE_FAILED,
            _EIO);
    CHECK(ish_apple_diagnostics_drain(
                    ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME,
                    0, &event, 1, &count) == 0 &&
            count == 1 &&
            event.category ==
                    ISH_APPLE_DIAGNOSTIC_CATEGORY_RUNTIME &&
            event.request_id == 0 &&
            event.linux_error == _EIO,
            "runtime 事件使用显式全局 scope");
    return 0;
}

int main(void) {
    if (test_event_fields_and_filtering() != 0)
        return 1;
    if (test_dynamic_concurrent_registry() != 0)
        return 1;
    if (test_validation_and_runtime() != 0)
        return 1;
    puts("Apple 结构化兼容性诊断回归通过");
    return 0;
}

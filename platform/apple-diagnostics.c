#include "sdk/iSHApple/Headers/iSHAppleDiagnostics.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apple-build-identity.h"
#include "guest/aarch64/backend.h"
#include "guest/aarch64/linux-syscall-name.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "platform/apple-diagnostics-private.h"

struct diagnostic_node {
    struct ish_apple_diagnostic_event_v1 event;
    struct diagnostic_node *next;
};

static pthread_mutex_t diagnostics_lock = PTHREAD_MUTEX_INITIALIZER;
static struct diagnostic_node *diagnostics_head;
static struct diagnostic_node *diagnostics_tail;
static uint64_t diagnostics_next_sequence = 1;

_Static_assert(
        TASK_HOST_DIAGNOSTIC_COMMAND ==
                ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND &&
        TASK_HOST_DIAGNOSTIC_TERMINAL ==
                ISH_APPLE_DIAGNOSTIC_SCOPE_TERMINAL &&
        TASK_HOST_DIAGNOSTIC_GUEST_FILE ==
                ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE,
        "内核诊断 scope 必须与公共 ABI 保持一致");
_Static_assert(sizeof(ISH_APPLE_BUILD_IDENTITY) <=
                ISH_APPLE_DIAGNOSTIC_BUILD_IDENTITY_BYTES_MAX,
        "构建身份必须完整放入公共诊断事件");

static bool diagnostic_scope_valid(
        uint32_t scope, uint64_t request_id) {
    switch (scope) {
        case ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME:
            return request_id == 0;
        case ISH_APPLE_DIAGNOSTIC_SCOPE_COMMAND:
        case ISH_APPLE_DIAGNOSTIC_SCOPE_TERMINAL:
        case ISH_APPLE_DIAGNOSTIC_SCOPE_GUEST_FILE:
            return request_id != 0;
        default:
            return false;
    }
}

static uint32_t diagnostic_backend(enum aarch64_backend backend) {
    switch (backend) {
        case AARCH64_BACKEND_C:
            return ISH_APPLE_DIAGNOSTIC_BACKEND_C;
        case AARCH64_BACKEND_THREADED:
            return ISH_APPLE_DIAGNOSTIC_BACKEND_THREADED;
    }
    return ISH_APPLE_DIAGNOSTIC_BACKEND_UNKNOWN;
}

static void diagnostic_copy_text(
        char *destination, size_t capacity, const char *source) {
    if (capacity == 0)
        return;
    if (source == NULL)
        source = "";
    size_t length = strnlen(source, capacity - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void diagnostic_set_syscall_name(
        struct ish_apple_diagnostic_event_v1 *event) {
    const char *name = aarch64_linux_syscall_name(
            event->syscall_number);
    if (name != NULL) {
        diagnostic_copy_text(
                event->syscall_name,
                sizeof(event->syscall_name), name);
        return;
    }
    (void) snprintf(
            event->syscall_name,
            sizeof(event->syscall_name),
            "syscall_%" PRIu64,
            event->syscall_number);
}

static void diagnostic_enqueue(
        struct ish_apple_diagnostic_event_v1 event) {
    if (!diagnostic_scope_valid(event.scope, event.request_id))
        return;
    struct diagnostic_node *node = malloc(sizeof(*node));
    if (node == NULL)
        return;

    event.version = ISH_APPLE_ABI_VERSION;
    event.structure_size = sizeof(event);
    event.architecture =
            ISH_APPLE_DIAGNOSTIC_ARCHITECTURE_AARCH64;
    diagnostic_copy_text(
            event.build_identity,
            sizeof(event.build_identity),
            ISH_APPLE_BUILD_IDENTITY);
    if (event.category == ISH_APPLE_DIAGNOSTIC_CATEGORY_SYSCALL)
        diagnostic_set_syscall_name(&event);

    pthread_mutex_lock(&diagnostics_lock);
    if (diagnostics_next_sequence == 0)
        __builtin_trap();
    event.sequence = diagnostics_next_sequence++;
    node->event = event;
    node->next = NULL;
    if (diagnostics_tail == NULL)
        diagnostics_head = node;
    else
        diagnostics_tail->next = node;
    diagnostics_tail = node;
    pthread_mutex_unlock(&diagnostics_lock);
}

static bool diagnostic_matches(
        const struct diagnostic_node *node,
        uint32_t scope,
        uint64_t request_id) {
    return node->event.scope == scope &&
            node->event.request_id == request_id;
}

static void diagnostic_restore_tail(void) {
    if (diagnostics_head == NULL) {
        diagnostics_tail = NULL;
        return;
    }
    if (diagnostics_tail != NULL)
        return;
    diagnostics_tail = diagnostics_head;
    while (diagnostics_tail->next != NULL)
        diagnostics_tail = diagnostics_tail->next;
}

int32_t ish_apple_diagnostics_drain(
        uint32_t scope,
        uint64_t request_id,
        struct ish_apple_diagnostic_event_v1 *events,
        uint32_t capacity,
        uint32_t *count_out) {
    if (count_out == NULL ||
            (events == NULL && capacity != 0) ||
            !diagnostic_scope_valid(scope, request_id))
        return _EINVAL;
    *count_out = 0;

    pthread_mutex_lock(&diagnostics_lock);
    if (events == NULL) {
        uint32_t count = 0;
        for (struct diagnostic_node *node = diagnostics_head;
                node != NULL; node = node->next) {
            if (diagnostic_matches(node, scope, request_id) &&
                    count != UINT32_MAX)
                count++;
        }
        *count_out = count;
        pthread_mutex_unlock(&diagnostics_lock);
        return 0;
    }

    struct diagnostic_node **link = &diagnostics_head;
    while (*link != NULL && *count_out < capacity) {
        struct diagnostic_node *node = *link;
        if (!diagnostic_matches(node, scope, request_id)) {
            link = &node->next;
            continue;
        }
        events[*count_out] = node->event;
        (*count_out)++;
        *link = node->next;
        if (diagnostics_tail == node)
            diagnostics_tail = NULL;
        free(node);
    }
    diagnostic_restore_tail();
    pthread_mutex_unlock(&diagnostics_lock);
    return 0;
}

int32_t ish_apple_diagnostics_clear(
        uint32_t scope,
        uint64_t request_id,
        uint32_t *cleared_out) {
    if (cleared_out == NULL ||
            !diagnostic_scope_valid(scope, request_id))
        return _EINVAL;
    *cleared_out = 0;

    pthread_mutex_lock(&diagnostics_lock);
    struct diagnostic_node **link = &diagnostics_head;
    while (*link != NULL) {
        struct diagnostic_node *node = *link;
        if (!diagnostic_matches(node, scope, request_id)) {
            link = &node->next;
            continue;
        }
        *link = node->next;
        if (diagnostics_tail == node)
            diagnostics_tail = NULL;
        free(node);
        if (*cleared_out != UINT32_MAX)
            (*cleared_out)++;
    }
    diagnostic_restore_tail();
    pthread_mutex_unlock(&diagnostics_lock);
    return 0;
}

static bool diagnostic_task_context(
        const struct task *task,
        uint32_t *scope,
        uint64_t *request_id) {
    if (task == NULL || task->group == NULL)
        return false;
    *scope = task->group->host_diagnostic_scope;
    *request_id = task->group->host_diagnostic_request_id;
    return diagnostic_scope_valid(*scope, *request_id);
}

static void diagnostic_set_task_identity(
        struct ish_apple_diagnostic_event_v1 *event,
        struct task *task) {
    event->guest_process_id = (uint32_t) task->pid;
    event->guest_thread_group_id = (uint32_t) task->tgid;
    lock(&task->general_lock);
    diagnostic_copy_text(
            event->process_name,
            sizeof(event->process_name),
            task->comm);
    unlock(&task->general_lock);
}

void ish_apple_diagnostics_record_undefined_instruction(
        struct task *task,
        uint64_t guest_pc,
        uint32_t opcode,
        int32_t signal,
        enum aarch64_backend backend) {
    uint32_t scope;
    uint64_t request_id;
    if (!diagnostic_task_context(task, &scope, &request_id))
        return;
    struct ish_apple_diagnostic_event_v1 event = {
        .category = ISH_APPLE_DIAGNOSTIC_CATEGORY_INSTRUCTION,
        .kind = ISH_APPLE_DIAGNOSTIC_INSTRUCTION_UNDEFINED,
        .scope = scope,
        .backend = diagnostic_backend(backend),
        .signal = signal,
        .opcode = opcode,
        .request_id = request_id,
        .guest_pc = guest_pc,
    };
    diagnostic_set_task_identity(&event, task);
    diagnostic_enqueue(event);
}

void ish_apple_diagnostics_record_unsupported_syscall(
        struct task *task,
        uint64_t guest_pc,
        uint64_t syscall_number,
        int32_t linux_error,
        enum aarch64_backend backend) {
    uint32_t scope;
    uint64_t request_id;
    if (!diagnostic_task_context(task, &scope, &request_id))
        return;
    struct ish_apple_diagnostic_event_v1 event = {
        .category = ISH_APPLE_DIAGNOSTIC_CATEGORY_SYSCALL,
        .kind = ISH_APPLE_DIAGNOSTIC_SYSCALL_UNSUPPORTED,
        .scope = scope,
        .backend = diagnostic_backend(backend),
        .linux_error = linux_error,
        .request_id = request_id,
        .guest_pc = guest_pc,
        .syscall_number = syscall_number,
    };
    diagnostic_set_task_identity(&event, task);
    diagnostic_enqueue(event);
}

void ish_apple_diagnostics_record_filesystem(
        uint32_t scope,
        uint64_t request_id,
        uint32_t kind,
        int32_t linux_error) {
    diagnostic_enqueue((struct ish_apple_diagnostic_event_v1) {
        .category = ISH_APPLE_DIAGNOSTIC_CATEGORY_FILESYSTEM,
        .kind = kind,
        .scope = scope,
        .backend = diagnostic_backend(aarch64_backend_default()),
        .linux_error = linux_error,
        .request_id = request_id,
    });
}

void ish_apple_diagnostics_record_runtime(
        uint32_t kind,
        int32_t linux_error) {
    diagnostic_enqueue((struct ish_apple_diagnostic_event_v1) {
        .category = ISH_APPLE_DIAGNOSTIC_CATEGORY_RUNTIME,
        .kind = kind,
        .scope = ISH_APPLE_DIAGNOSTIC_SCOPE_RUNTIME,
        .backend = diagnostic_backend(aarch64_backend_default()),
        .linux_error = linux_error,
    });
}

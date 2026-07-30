#include <pthread.h>
#include <stdio.h>

#include "kernel/errno.h"
#include "kernel/task.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s（第 %d 行）\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

struct order_context {
    int values[16];
    size_t count;
};

struct marker_context {
    struct order_context *order;
    int marker;
};

static struct order_context *legacy_order;

static void legacy_hook(struct task *task, int code) {
    (void) task;
    (void) code;
    legacy_order->values[legacy_order->count++] = 1;
}

static void ordered_observer(
        struct task *task, int code, void *opaque) {
    (void) task;
    (void) code;
    struct marker_context *context = opaque;
    context->order->values[context->order->count++] =
            context->marker;
}

static void notify_exit(struct task *task, int code) {
    lock(&pids_lock);
    task_notify_exit_locked(task, code);
    unlock(&pids_lock);
}

static void test_order_and_registration(void) {
    struct task task = {};
    struct order_context order = {};
    struct marker_context first = {
        .order = &order,
        .marker = 2,
    };
    struct marker_context second = {
        .order = &order,
        .marker = 3,
    };
    legacy_order = &order;
    exit_hook = legacy_hook;

    CHECK(task_exit_observer_register(
            ordered_observer, &first) == 0,
            "注册第一个退出观察者");
    CHECK(task_exit_observer_register(
            ordered_observer, &second) == 0,
            "注册第二个退出观察者");
    CHECK(task_exit_observer_register(
            ordered_observer, &first) == _EEXIST,
            "拒绝重复注册同一观察者与上下文");

    notify_exit(&task, 41);
    CHECK(order.count == 3 &&
            order.values[0] == 1 &&
            order.values[1] == 2 &&
            order.values[2] == 3,
            "严格保留旧 hook 先于新增观察者的调用顺序");

    CHECK(task_exit_observer_unregister(
            ordered_observer, &first) == 0,
            "注销第一个观察者");
    notify_exit(&task, 42);
    CHECK(order.count == 5 &&
            order.values[3] == 1 &&
            order.values[4] == 3,
            "注销只移除精确观察者且不改变旧 hook");
    CHECK(task_exit_observer_unregister(
            ordered_observer, &first) == _ENOENT,
            "重复注销返回精确 ENOENT");
    CHECK(task_exit_observer_unregister(NULL, NULL) == _EINVAL &&
            task_exit_observer_register(NULL, NULL) == _EINVAL,
            "拒绝空观察者");

    CHECK(task_exit_observer_unregister(
            ordered_observer, &second) == 0,
            "清理第二个观察者");
    exit_hook = NULL;
    legacy_order = NULL;
}

static void test_capacity(void) {
    struct order_context order = {};
    struct marker_context contexts[9];
    for (size_t index = 0; index < 9; index++) {
        contexts[index] = (struct marker_context) {
            .order = &order,
            .marker = (int) index,
        };
    }
    for (size_t index = 0; index < 8; index++) {
        CHECK(task_exit_observer_register(
                ordered_observer, &contexts[index]) == 0,
                "八个观察者槽位均可注册");
    }
    CHECK(task_exit_observer_register(
            ordered_observer, &contexts[8]) == _ENOSPC,
            "第九个观察者返回精确 ENOSPC");
    for (size_t index = 0; index < 8; index++) {
        CHECK(task_exit_observer_unregister(
                ordered_observer, &contexts[index]) == 0,
                "容量测试后释放观察者槽位");
    }
}

struct reentrant_context {
    int register_result;
    int unregister_result;
};

static struct reentrant_context *legacy_reentrant_context;

static void reentrant_legacy_hook(struct task *task, int code) {
    (void) task;
    (void) code;
    legacy_reentrant_context->register_result =
            task_exit_observer_register(
                    ordered_observer,
                    legacy_reentrant_context);
    legacy_reentrant_context->unregister_result =
            task_exit_observer_unregister(
                    ordered_observer,
                    legacy_reentrant_context);
}

static void reentrant_observer(
        struct task *task, int code, void *opaque) {
    (void) task;
    (void) code;
    struct reentrant_context *context = opaque;
    context->register_result = task_exit_observer_register(
            ordered_observer, context);
    context->unregister_result = task_exit_observer_unregister(
            reentrant_observer, context);
}

static void test_reentrant_registration_is_rejected(void) {
    struct task task = {};
    struct reentrant_context context = {};
    CHECK(task_exit_observer_register(
            reentrant_observer, &context) == 0,
            "注册重入边界观察者");
    notify_exit(&task, 43);
    CHECK(context.register_result == _EDEADLK &&
            context.unregister_result == _EDEADLK,
            "观察者内部注册与注销返回 EDEADLK 而非自锁");
    CHECK(task_exit_observer_unregister(
            reentrant_observer, &context) == 0,
            "通知返回后可正常注销重入边界观察者");

    context = (struct reentrant_context) {};
    legacy_reentrant_context = &context;
    exit_hook = reentrant_legacy_hook;
    notify_exit(&task, 44);
    CHECK(context.register_result == _EDEADLK &&
            context.unregister_result == _EDEADLK,
            "旧 exit hook 内注册与注销同样拒绝自锁");
    exit_hook = NULL;
    legacy_reentrant_context = NULL;
}

struct blocking_context {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool entered;
    bool may_return;
    size_t calls;
};

static void blocking_observer(
        struct task *task, int code, void *opaque) {
    (void) task;
    (void) code;
    struct blocking_context *context = opaque;
    pthread_mutex_lock(&context->lock);
    context->entered = true;
    context->calls++;
    pthread_cond_broadcast(&context->changed);
    while (!context->may_return)
        pthread_cond_wait(&context->changed, &context->lock);
    pthread_mutex_unlock(&context->lock);
}

struct notify_request {
    struct task *task;
};

static void *notify_thread(void *opaque) {
    struct notify_request *request = opaque;
    notify_exit(request->task, 77);
    return NULL;
}

struct unregister_request {
    struct blocking_context *context;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool attempted;
    bool finished;
    int result;
};

static void *unregister_thread(void *opaque) {
    struct unregister_request *request = opaque;
    pthread_mutex_lock(&request->lock);
    request->attempted = true;
    pthread_cond_broadcast(&request->changed);
    pthread_mutex_unlock(&request->lock);

    int result = task_exit_observer_unregister(
            blocking_observer, request->context);

    pthread_mutex_lock(&request->lock);
    request->result = result;
    request->finished = true;
    pthread_cond_broadcast(&request->changed);
    pthread_mutex_unlock(&request->lock);
    return NULL;
}

static void test_unregister_waits_for_notification(void) {
    struct task task = {};
    struct blocking_context context = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    CHECK(task_exit_observer_register(
            blocking_observer, &context) == 0,
            "注册并发注销观察者");

    struct notify_request notify = {.task = &task};
    pthread_t notifying;
    CHECK(pthread_create(
            &notifying, NULL, notify_thread, &notify) == 0,
            "启动退出通知线程");

    pthread_mutex_lock(&context.lock);
    while (!context.entered)
        pthread_cond_wait(&context.changed, &context.lock);
    pthread_mutex_unlock(&context.lock);

    struct unregister_request unregister = {
        .context = &context,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    pthread_t unregistering;
    CHECK(pthread_create(
            &unregistering, NULL,
            unregister_thread, &unregister) == 0,
            "启动观察者注销线程");

    pthread_mutex_lock(&unregister.lock);
    while (!unregister.attempted)
        pthread_cond_wait(&unregister.changed, &unregister.lock);
    CHECK(!unregister.finished,
            "通知尚未返回时注销必须等待 pids_lock");
    pthread_mutex_unlock(&unregister.lock);

    pthread_mutex_lock(&context.lock);
    context.may_return = true;
    pthread_cond_broadcast(&context.changed);
    pthread_mutex_unlock(&context.lock);

    pthread_join(notifying, NULL);
    pthread_join(unregistering, NULL);
    CHECK(unregister.result == 0 && unregister.finished,
            "通知返回后注销成功");

    notify_exit(&task, 78);
    CHECK(context.calls == 1,
            "注销返回后不会再执行旧观察者");
    pthread_cond_destroy(&unregister.changed);
    pthread_mutex_destroy(&unregister.lock);
    pthread_cond_destroy(&context.changed);
    pthread_mutex_destroy(&context.lock);
}

int main(void) {
    test_order_and_registration();
    test_capacity();
    test_reentrant_registration_is_rejected();
    test_unregister_waits_for_notification();

    if (failures == 0) {
        puts("退出观察者串行契约回归通过");
        return 0;
    }
    fprintf(stderr, "退出观察者串行契约回归失败：%d 项\n", failures);
    return 1;
}

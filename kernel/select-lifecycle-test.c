#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"
#include "kernel/time.h"

#define USER_PAGE UINT32_C(0x00100000)
#define READFDS_ADDRESS (USER_PAGE + 16)
#define TIMEOUT_ADDRESS (USER_PAGE + 32)
#define UNMAPPED_ADDRESS UINT32_C(0x00200000)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "select 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct select_fixture {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
};

struct readiness_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool entered;
    bool released;
};

struct fd_probe {
    struct readiness_gate *gate;
    atomic_uint close_calls;
};

struct select_call {
    struct task *task;
    sdword_t result;
};

static int map_user_page(struct task *task) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(
            task->mem, PAGE(USER_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error;
}

static bool fixture_init(struct select_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {4, 4};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files)) {
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }

    atomic_init(&fixture->sighand.refcount, 1);
    lock_init(&fixture->sighand.lock);
    fixture->task.sighand = &fixture->sighand;
    lock_init(&fixture->task.waiting_cond_lock);
    struct mm *mm = mm_new();
    if (mm == NULL) {
        fdtable_release(fixture->task.files);
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->sighand.lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }
    task_set_mm(&fixture->task, mm);
    if (map_user_page(&fixture->task) != 0) {
        fdtable_release(fixture->task.files);
        mm_release(fixture->task.mm);
        pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
        pthread_mutex_destroy(&fixture->sighand.lock.m);
        pthread_mutex_destroy(&fixture->group.lock.m);
        return false;
    }
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct select_fixture *fixture) {
    current = NULL;
    fdtable_release(fixture->task.files);
    mm_release(fixture->task.mm);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->sighand.lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
}

static void readiness_gate_init(struct readiness_gate *gate) {
    memset(gate, 0, sizeof(*gate));
    pthread_mutex_init(&gate->lock, NULL);
    pthread_cond_init(&gate->cond, NULL);
}

static void readiness_gate_release(struct readiness_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->released = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static void readiness_gate_destroy(struct readiness_gate *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

static bool readiness_gate_wait_until_entered(
        struct readiness_gate *gate) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;

    pthread_mutex_lock(&gate->lock);
    int error = 0;
    while (!gate->entered && error == 0)
        error = pthread_cond_timedwait(
                &gate->cond, &gate->lock, &deadline);
    bool entered = gate->entered;
    pthread_mutex_unlock(&gate->lock);
    return entered;
}

static int probe_poll(struct fd *fd) {
    struct fd_probe *probe = fd->data;
    if (probe->gate == NULL)
        return 0;

    pthread_mutex_lock(&probe->gate->lock);
    probe->gate->entered = true;
    pthread_cond_broadcast(&probe->gate->cond);
    while (!probe->gate->released)
        pthread_cond_wait(&probe->gate->cond, &probe->gate->lock);
    pthread_mutex_unlock(&probe->gate->lock);
    return POLL_READ;
}

static int probe_close(struct fd *fd) {
    struct fd_probe *probe = fd->data;
    atomic_fetch_add_explicit(
            &probe->close_calls, 1, memory_order_relaxed);
    return 0;
}

static const struct fd_ops probe_ops = {
    .poll = probe_poll,
    .close = probe_close,
};

static struct fd *make_fd(struct fd_probe *probe) {
    struct fd *fd = fd_create(&probe_ops);
    if (fd != NULL)
        fd->data = probe;
    return fd;
}

static void *run_select(void *opaque) {
    struct select_call *call = opaque;
    current = call->task;
    call->result = (sdword_t) sys_select(
            1, READFDS_ADDRESS, 0, 0, TIMEOUT_ADDRESS);
    current = NULL;
    return NULL;
}

static int test_argument_bounds(void) {
    struct timeval_ immediate = {0};
    CHECK(user_put(TIMEOUT_ADDRESS, immediate) == 0,
            "写入立即超时参数");
    CHECK((sdword_t) sys_select(-1, 0, 0, 0, 0) == _EINVAL,
            "拒绝负数 nfds");
    CHECK((sdword_t) sys_select(5, 0, 0, 0, 0) == _EINVAL,
            "拒绝超过 NOFILE 限制的 nfds");
    CHECK((sdword_t) sys_select(0, UNMAPPED_ADDRESS,
            UNMAPPED_ADDRESS, UNMAPPED_ADDRESS,
            TIMEOUT_ADDRESS) == 0,
            "nfds 为零时不访问 fd_set 指针");
    return 0;
}

static int test_error_cleanup_balance(struct select_fixture *fixture) {
    struct fd_probe probe = {0};
    atomic_init(&probe.close_calls, 0);
    struct fd *fd = make_fd(&probe);
    CHECK(fd != NULL && f_install_task(&fixture->task, fd, 0) == 0,
            "安装错误清理测试文件对象");

    byte_t selected = 3;
    CHECK(user_put(READFDS_ADDRESS, selected) == 0,
            "选择有效与无效描述符");
    CHECK((sdword_t) sys_select(
            2, READFDS_ADDRESS, 0, 0, TIMEOUT_ADDRESS) == _EBADF,
            "构建中遇到无效描述符返回 EBADF");
    CHECK(atomic_load_explicit(
            &fd->refcount, memory_order_relaxed) == 1,
            "错误退出释放此前取得的独立引用");
    CHECK(f_close_task(&fixture->task, 0) == 0 &&
            atomic_load_explicit(&probe.close_calls,
                    memory_order_relaxed) == 1,
            "错误退出未泄漏文件对象");
    return 0;
}

static int test_close_and_reuse(struct select_fixture *fixture) {
    struct readiness_gate gate;
    readiness_gate_init(&gate);
    struct fd_probe original_probe = {.gate = &gate};
    struct fd_probe replacement_probe = {0};
    atomic_init(&original_probe.close_calls, 0);
    atomic_init(&replacement_probe.close_calls, 0);
    struct fd *original = make_fd(&original_probe);
    struct fd *replacement = make_fd(&replacement_probe);
    CHECK(original != NULL && replacement != NULL &&
            f_install_task(&fixture->task, original, 0) == 0,
            "安装 close/reuse 竞态测试文件对象");

    byte_t selected = 1;
    struct timeval_ immediate = {0};
    CHECK(user_put(READFDS_ADDRESS, selected) == 0 &&
            user_put(TIMEOUT_ADDRESS, immediate) == 0,
            "准备 close/reuse select 参数");
    struct select_call call = {.task = &fixture->task};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_select, &call) == 0,
            "启动 select 等待线程");

    bool entered = readiness_gate_wait_until_entered(&gate);
    bool retained = entered && atomic_load_explicit(
            &original->refcount, memory_order_acquire) == 2;
    int close_result = _EBADF;
    fd_t replacement_number = _EBADF;
    unsigned retained_after_close = 0;
    if (retained) {
        close_result = f_close_task(&fixture->task, 0);
        retained_after_close = atomic_load_explicit(
                &original->refcount, memory_order_acquire);
        replacement_number = f_install_task(
                &fixture->task, replacement, 0);
        replacement = NULL;
    }
    readiness_gate_release(&gate);
    int join_result = pthread_join(thread, NULL);

    byte_t result_set = 0;
    int read_result = user_get(READFDS_ADDRESS, result_set);
    int replacement_close_result = _EBADF;
    if (!retained) {
        f_close_task(&fixture->task, 0);
        fd_close(replacement);
    } else if (replacement_number >= 0) {
        replacement_close_result = f_close_task(
                &fixture->task, replacement_number);
    }
    unsigned original_close_calls = atomic_load_explicit(
            &original_probe.close_calls, memory_order_relaxed);
    unsigned replacement_close_calls = atomic_load_explicit(
            &replacement_probe.close_calls, memory_order_relaxed);
    readiness_gate_destroy(&gate);

    CHECK(entered && retained,
            "select 在就绪回调期间持有独立 fd 引用");
    CHECK(close_result == 0 && retained_after_close == 1 &&
            replacement_number == 0,
            "并发 close 后原对象存活且槽位可安全复用");
    CHECK(join_result == 0 && call.result == 1 &&
            read_result == 0 && bit_test(0, &result_set),
            "close/reuse 期间 select 仍按原对象完成投递");
    CHECK(original_close_calls == 1,
            "select 返回后恰好释放一次原文件对象");
    CHECK(replacement_close_result == 0 && replacement_close_calls == 1,
            "测试结束时释放复用槽位中的替换对象");
    return 0;
}

int main(void) {
    struct select_fixture fixture;
    CHECK(fixture_init(&fixture), "初始化 select 生命周期夹具");

    int result = test_argument_bounds();
    if (result == 0)
        result = test_error_cleanup_balance(&fixture);
    if (result == 0)
        result = test_close_and_reuse(&fixture);

    fixture_destroy(&fixture);
    return result;
}

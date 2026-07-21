#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fs/tty.h"
#include "kernel/task.h"

static atomic_uint failures = ATOMIC_VAR_INIT(0);
static atomic_uint cleanup_calls = ATOMIC_VAR_INIT(0);

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "TTY 回显生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed); \
    } \
} while (0)

struct echo_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool entered;
    bool allow_return;
    bool held_tty_lock;
    bool block_first;
    unsigned calls;
    char output[64];
    size_t output_size;
};

static struct echo_gate *active_gate;

static int gated_echo_write(struct tty *tty, const void *buffer,
        size_t size, bool blocking) {
    (void) buffer;
    (void) blocking;
    struct echo_gate *gate = active_gate;
    pthread_mutex_lock(&gate->mutex);
    size_t room = sizeof(gate->output) - gate->output_size;
    size_t copied = size < room ? size : room;
    memcpy(gate->output + gate->output_size, buffer, copied);
    gate->output_size += copied;
    gate->calls++;
    bool block = gate->block_first && gate->calls == 1;
    gate->held_tty_lock = lock_owned_by_current(&tty->lock);
    gate->entered = true;
    pthread_cond_broadcast(&gate->changed);
    while (block && !gate->allow_return)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
    return (int) size;
}

static void tracked_cleanup(struct tty *tty) {
    EXPECT(!lock_owned_by_current(&tty->lock),
            "cleanup 不持有 tty 对象锁");
    EXPECT(lock_owned_by_current(&ttys_lock),
            "cleanup 保持全局发布锁契约");
    atomic_fetch_add_explicit(&cleanup_calls, 1, memory_order_relaxed);
}

static const struct tty_driver_ops echo_ops = {
    .write = gated_echo_write,
    .cleanup = tracked_cleanup,
};

struct input_worker {
    struct task task;
    struct tty *tty;
    const char *input;
    size_t size;
    atomic_bool *attempted;
    ssize_t result;
};

static void *input_worker_main(void *opaque) {
    struct input_worker *worker = opaque;
    current = &worker->task;
    if (worker->attempted != NULL) {
        atomic_store_explicit(
                worker->attempted, true, memory_order_release);
    }
    worker->result = tty_input(
            worker->tty, worker->input, worker->size, false);
    current = NULL;
    return NULL;
}

static void test_echo_write_releases_tty_lock(void) {
    atomic_store_explicit(&cleanup_calls, 0, memory_order_relaxed);
    struct echo_gate gate = {.block_first = true};
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化回显闸门 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化回显闸门 condition");
    active_gate = &gate;

    struct tty_driver driver = {.ops = &echo_ops};
    struct task owner = {
        .euid = 8501,
        .egid = 8502,
    };
    current = &owner;
    struct tty *tty = pty_open_fake(&driver);
    EXPECT(tty != NULL && !IS_ERR(tty),
            "创建受控回显 tty");
    if (tty == NULL || IS_ERR(tty))
        goto destroy_gate;
    int tty_num = tty->num;

    struct input_worker worker = {
        .tty = tty,
        .input = "a",
        .size = 1,
        .result = -1,
    };
    pthread_t input_thread;
    int create_error = pthread_create(
            &input_thread, NULL, input_worker_main, &worker);
    EXPECT(create_error == 0, "建立受控 tty_input 线程");
    if (create_error != 0)
        goto release_tty;

    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    bool held_tty_lock = gate.held_tty_lock;
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(!held_tty_lock,
            "可能阻塞的 driver 回显写发生在 tty 锁外");

    int input_lock_result = trylock(&tty->input_lock);
    EXPECT(input_lock_result != 0,
            "宿主回显期间仍持有独立输入事务锁");
    if (input_lock_result == 0)
        unlock(&tty->input_lock);

    int lock_result = trylock(&tty->lock);
    EXPECT(lock_result == 0,
            "回显 driver 阻塞时 hangup 仍可取得 tty 锁");
    if (lock_result == 0) {
        tty_hangup(tty);
        unlock(&tty->lock);
    }

    pthread_mutex_lock(&gate.mutex);
    gate.allow_return = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(pthread_join(input_thread, NULL) == 0,
            "解除回显阻塞后输入线程有界退出");
    EXPECT(worker.result == 1,
            "hangup 前已接受的回显字符保持成功计数");

release_tty:
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
    EXPECT(atomic_load_explicit(&cleanup_calls,
            memory_order_relaxed) == 1,
            "回显测试对象最终只 cleanup 一次");
    EXPECT(driver.ttys[tty_num] == NULL &&
            !driver.reserved[tty_num],
            "回显测试最终清理发布槽与 reservation");

destroy_gate:
    current = NULL;
    active_gate = NULL;
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

static void test_concurrent_input_preserves_echo_order(void) {
    atomic_store_explicit(&cleanup_calls, 0, memory_order_relaxed);
    struct echo_gate gate = {.block_first = true};
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化并发顺序回显闸门 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化并发顺序回显闸门 condition");
    active_gate = &gate;

    struct tty_driver driver = {.ops = &echo_ops};
    struct task owner = {
        .euid = 8511,
        .egid = 8512,
    };
    current = &owner;
    struct tty *tty = pty_open_fake(&driver);
    EXPECT(tty != NULL && !IS_ERR(tty),
            "创建并发顺序回显 tty");
    if (tty == NULL || IS_ERR(tty))
        goto destroy_gate;
    int tty_num = tty->num;

    struct input_worker first = {
        .tty = tty,
        .input = "a",
        .size = 1,
        .result = -1,
    };
    pthread_t first_thread;
    int first_error = pthread_create(
            &first_thread, NULL, input_worker_main, &first);
    EXPECT(first_error == 0, "建立首个并发输入线程");
    if (first_error != 0)
        goto release_tty;

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    atomic_bool second_attempted;
    atomic_init(&second_attempted, false);
    struct input_worker second = {
        .tty = tty,
        .input = "b",
        .size = 1,
        .attempted = &second_attempted,
        .result = -1,
    };
    pthread_t second_thread;
    int second_error = pthread_create(
            &second_thread, NULL, input_worker_main, &second);
    EXPECT(second_error == 0, "建立第二个并发输入线程");
    if (second_error == 0) {
        while (!atomic_load_explicit(
                &second_attempted, memory_order_acquire))
            sched_yield();

        int input_lock_result = trylock(&tty->input_lock);
        EXPECT(input_lock_result != 0,
                "首个回显阻塞时第二个输入不能插入行规事务");
        if (input_lock_result == 0)
            unlock(&tty->input_lock);

        lock(&tty->lock);
        EXPECT(tty->bufsize == 1 && tty->buf[0] == 'a',
                "第二输入尝试期间缓冲仍只含首个已提交字符");
        unlock(&tty->lock);
    }

    pthread_mutex_lock(&gate.mutex);
    gate.allow_return = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(pthread_join(first_thread, NULL) == 0,
            "首个并发输入线程有界退出");
    if (second_error == 0) {
        EXPECT(pthread_join(second_thread, NULL) == 0,
                "第二个并发输入线程有界退出");
    }
    alarm(0);

    EXPECT(first.result == 1 &&
            (second_error != 0 || second.result == 1),
            "两个并发输入均报告精确接受长度");
    if (second_error == 0) {
        pthread_mutex_lock(&gate.mutex);
        EXPECT(gate.calls == 2 && gate.output_size == 2 &&
                memcmp(gate.output, "ab", 2) == 0,
                "回显顺序与缓冲提交顺序一致");
        pthread_mutex_unlock(&gate.mutex);
        lock(&tty->lock);
        EXPECT(tty->bufsize == 2 &&
                memcmp(tty->buf, "ab", 2) == 0,
                "并发普通字符按调用线性化顺序进入行规缓冲");
        unlock(&tty->lock);
    }

release_tty:
    alarm(0);
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
    EXPECT(atomic_load_explicit(&cleanup_calls,
            memory_order_relaxed) == 1,
            "并发顺序回显对象最终只 cleanup 一次");
    EXPECT(driver.ttys[tty_num] == NULL &&
            !driver.reserved[tty_num],
            "并发顺序回显测试最终清理槽位");

destroy_gate:
    current = NULL;
    active_gate = NULL;
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

static void test_kill_does_not_erase_concurrent_input(void) {
    atomic_store_explicit(&cleanup_calls, 0, memory_order_relaxed);
    struct echo_gate gate = {.block_first = true};
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化 kill 并发回显闸门 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化 kill 并发回显闸门 condition");
    active_gate = &gate;

    struct tty_driver driver = {.ops = &echo_ops};
    struct task owner = {
        .euid = 8521,
        .egid = 8522,
    };
    current = &owner;
    struct tty *tty = pty_open_fake(&driver);
    EXPECT(tty != NULL && !IS_ERR(tty),
            "创建 kill 并发回显 tty");
    if (tty == NULL || IS_ERR(tty))
        goto destroy_gate;
    int tty_num = tty->num;

    lock(&tty->lock);
    tty->termios.lflags &= ~ECHO_;
    char kill_char = tty->termios.cc[VKILL_];
    unlock(&tty->lock);
    EXPECT(tty_input(tty, "ab", 2, false) == 2,
            "无回显预置待删除的未结束行");
    lock(&tty->lock);
    tty->termios.lflags |= ECHO_ | ECHOK_;
    unlock(&tty->lock);

    struct input_worker killer = {
        .tty = tty,
        .input = &kill_char,
        .size = 1,
        .result = -1,
    };
    pthread_t killer_thread;
    int killer_error = pthread_create(
            &killer_thread, NULL, input_worker_main, &killer);
    EXPECT(killer_error == 0, "建立 kill 输入线程");
    if (killer_error != 0)
        goto release_tty;

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    atomic_bool concurrent_attempted;
    atomic_init(&concurrent_attempted, false);
    struct input_worker concurrent = {
        .tty = tty,
        .input = "c",
        .size = 1,
        .attempted = &concurrent_attempted,
        .result = -1,
    };
    pthread_t concurrent_thread;
    int concurrent_error = pthread_create(
            &concurrent_thread, NULL, input_worker_main, &concurrent);
    EXPECT(concurrent_error == 0, "建立与 kill 交错的输入线程");
    if (concurrent_error == 0) {
        while (!atomic_load_explicit(
                &concurrent_attempted, memory_order_acquire))
            sched_yield();

        int input_lock_result = trylock(&tty->input_lock);
        EXPECT(input_lock_result != 0,
                "kill 回显窗口仍独占完整输入事务");
        if (input_lock_result == 0)
            unlock(&tty->input_lock);
        lock(&tty->lock);
        EXPECT(tty->bufsize == 1 && tty->buf[0] == 'a',
                "kill 首次擦除后并发字符尚未插入缓冲");
        unlock(&tty->lock);
    }

    pthread_mutex_lock(&gate.mutex);
    gate.allow_return = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(pthread_join(killer_thread, NULL) == 0,
            "kill 输入线程有界退出");
    if (concurrent_error == 0) {
        EXPECT(pthread_join(concurrent_thread, NULL) == 0,
                "kill 后的并发输入线程有界退出");
    }
    alarm(0);

    EXPECT(killer.result == 1 &&
            (concurrent_error != 0 || concurrent.result == 1),
            "kill 与后续字符均报告精确接受长度");
    if (concurrent_error == 0) {
        static const char expected_echo[] = "\b \b\b \bc";
        pthread_mutex_lock(&gate.mutex);
        EXPECT(gate.output_size == sizeof(expected_echo) - 1 &&
                memcmp(gate.output, expected_echo,
                sizeof(expected_echo) - 1) == 0,
                "kill 的两次擦除回显完整先于后续字符");
        pthread_mutex_unlock(&gate.mutex);
        lock(&tty->lock);
        EXPECT(tty->bufsize == 1 && tty->buf[0] == 'c',
                "kill 只删除旧行且保留随后并发输入的字符");
        unlock(&tty->lock);
    }

release_tty:
    alarm(0);
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
    EXPECT(atomic_load_explicit(&cleanup_calls,
            memory_order_relaxed) == 1,
            "kill 并发回显对象最终只 cleanup 一次");
    EXPECT(driver.ttys[tty_num] == NULL &&
            !driver.reserved[tty_num],
            "kill 并发回显测试最终清理槽位");

destroy_gate:
    current = NULL;
    active_gate = NULL;
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

int main(void) {
    test_echo_write_releases_tty_lock();
    test_concurrent_input_preserves_echo_order();
    test_kill_does_not_erase_concurrent_input();

    unsigned count = atomic_load_explicit(
            &failures, memory_order_relaxed);
    if (count != 0) {
        fprintf(stderr, "TTY 回显生命周期测试共发现 %u 项失败\n", count);
        return 1;
    }
    puts("TTY 回显生命周期测试通过");
    return 0;
}

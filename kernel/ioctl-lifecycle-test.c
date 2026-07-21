#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define TEST_IOCTL_COMMAND UINT32_C(0x545e)

struct ioctl_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool entered;
    bool active;
    bool allow_return;
    unsigned close_calls;
    unsigned close_while_active;
    unsigned successor_close_calls;
};

struct ioctl_call {
    struct task *task;
    dword_t result;
};

struct close_counter {
    unsigned calls;
};

static struct ioctl_gate gate = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

static ssize_t probe_ioctl_size(int command) {
    return command == TEST_IOCTL_COMMAND ? 0 : -1;
}

static int probe_ioctl(
        struct fd *UNUSED(fd), int command, void *UNUSED(argument)) {
    if (command != TEST_IOCTL_COMMAND)
        return _ENOTTY;

    pthread_mutex_lock(&gate.mutex);
    gate.active = true;
    gate.entered = true;
    pthread_cond_broadcast(&gate.changed);
    while (!gate.allow_return)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    gate.active = false;
    pthread_mutex_unlock(&gate.mutex);
    return 37;
}

static int probe_close(struct fd *UNUSED(fd)) {
    pthread_mutex_lock(&gate.mutex);
    gate.close_calls++;
    if (gate.active)
        gate.close_while_active++;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    return 0;
}

static int successor_close(struct fd *UNUSED(fd)) {
    pthread_mutex_lock(&gate.mutex);
    gate.successor_close_calls++;
    pthread_mutex_unlock(&gate.mutex);
    return 0;
}

static int counter_close(struct fd *fd) {
    struct close_counter *counter = fd->data;
    counter->calls++;
    return 0;
}

static const struct fd_ops probe_ops = {
    .ioctl_size = probe_ioctl_size,
    .ioctl = probe_ioctl,
    .close = probe_close,
};

static const struct fd_ops successor_ops = {
    .close = successor_close,
};

static const struct fd_ops counter_ops = {
    .close = counter_close,
};

static void *ioctl_main(void *opaque) {
    struct ioctl_call *call = opaque;
    current = call->task;
    call->result = sys_ioctl(0, TEST_IOCTL_COMMAND, 0);
    current = NULL;
    return NULL;
}

static bool run_ioctl_close_lifecycle(void) {
    struct tgroup group = {0};
    lock_init(&group.lock);
    group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {1, 1};
    struct task task = {
        .group = &group,
        .files = fdtable_new(1),
    };
    if (IS_ERR(task.files)) {
        fputs("ioctl 生命周期测试失败：无法创建 fd 表\n", stderr);
        lock_destroy(&group.lock);
        return false;
    }

    struct fd *fd = fd_create(&probe_ops);
    if (fd == NULL) {
        fputs("ioctl 生命周期测试失败：无法创建测试 fd\n", stderr);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }
    if (f_install_task(&task, fd, 0) != 0) {
        fputs("ioctl 生命周期测试失败：无法安装测试 fd\n", stderr);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }

    struct fd *successor = fd_create(&successor_ops);
    if (successor == NULL) {
        fputs("ioctl 生命周期测试失败：无法创建复用 fd\n", stderr);
        f_close_task(&task, 0);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }

    struct ioctl_call call = {
        .task = &task,
        .result = (dword_t) _EIO,
    };
    pthread_t thread;
    if (pthread_create(&thread, NULL, ioctl_main, &call) != 0) {
        fputs("ioctl 生命周期测试失败：无法创建 ioctl 线程\n", stderr);
        fd_close(successor);
        f_close_task(&task, 0);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }

    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    current = &task;
    int close_result = (int) sys_close(0);
    current = NULL;
    fd_t reused_number = f_install_task(&task, successor, 0);

    pthread_mutex_lock(&gate.mutex);
    bool deferred = gate.close_calls == 0 &&
            gate.close_while_active == 0 &&
            gate.successor_close_calls == 0;
    gate.allow_return = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);

    int join_result = pthread_join(thread, NULL);

    pthread_mutex_lock(&gate.mutex);
    bool finalized = gate.close_calls == 1 &&
            gate.close_while_active == 0 &&
            gate.successor_close_calls == 0;
    pthread_mutex_unlock(&gate.mutex);

    fdtable_release(task.files);
    pthread_mutex_lock(&gate.mutex);
    bool successor_finalized = gate.successor_close_calls == 1;
    pthread_mutex_unlock(&gate.mutex);
    lock_destroy(&group.lock);
    bool passed = close_result == 0 && join_result == 0 &&
            reused_number == 0 && call.result == 37 && deferred &&
            finalized && successor_finalized;
    if (!passed) {
        fputs("ioctl 生命周期测试失败：并发 close 必须等在途 ioctl 释放强引用\n",
                stderr);
    }
    return passed;
}

static bool run_cloexec_slot_lifecycle(void) {
    struct tgroup group = {0};
    lock_init(&group.lock);
    group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {1, 1};
    struct task task = {
        .group = &group,
        .files = fdtable_new(1),
    };
    if (IS_ERR(task.files)) {
        fputs("ioctl 生命周期测试失败：无法创建 cloexec fd 表\n", stderr);
        lock_destroy(&group.lock);
        return false;
    }

    struct close_counter original_close = {0};
    struct fd *original = fd_create(&counter_ops);
    if (original == NULL) {
        fputs("ioctl 生命周期测试失败：无法创建 cloexec 原 fd\n", stderr);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }
    original->data = &original_close;
    if (f_install_task(&task, original, 0) != 0) {
        fputs("ioctl 生命周期测试失败：无法安装 cloexec 原 fd\n", stderr);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }

    struct fd *retained = f_get_task_retain(&task, 0);
    current = &task;
    dword_t set_result = sys_ioctl(0, FIOCLEX_, 0);
    bool set = bit_test(0, task.files->cloexec);
    dword_t clear_result = sys_ioctl(0, FIONCLEX_, 0);
    bool cleared = !bit_test(0, task.files->cloexec);
    current = NULL;

    int close_result = f_close_task(&task, 0);
    struct close_counter successor_close = {0};
    struct fd *successor = fd_create(&counter_ops);
    if (successor == NULL) {
        fputs("ioctl 生命周期测试失败：无法创建 cloexec 复用 fd\n", stderr);
        fd_close(retained);
        fdtable_release(task.files);
        lock_destroy(&group.lock);
        return false;
    }
    successor->data = &successor_close;
    fd_t reused_number = f_install_task(&task, successor, O_CLOEXEC_);
    bool reused_set = bit_test(0, task.files->cloexec);
    int reused_clear_result = file_ioctl_fd_task(
            &task, 0, retained, FIONCLEX_, NULL, 0);
    bool reused_cleared = !bit_test(0, task.files->cloexec);
    int reused_set_result = file_ioctl_fd_task(
            &task, 0, retained, FIOCLEX_, NULL, 0);
    bool reused_reset = bit_test(0, task.files->cloexec);

    bool retained_ok = retained != NULL;
    fd_close(retained);
    bool original_finalized = original_close.calls == 1;
    fdtable_release(task.files);
    bool successor_finalized = successor_close.calls == 1;
    lock_destroy(&group.lock);

    bool passed = retained_ok && set_result == 0 && set &&
            clear_result == 0 && cleared && close_result == 0 &&
            reused_number == 0 && reused_set && reused_clear_result == 0 &&
            reused_cleared && reused_set_result == 0 && reused_reset &&
            original_finalized && successor_finalized;
    if (!passed) {
        fputs("ioctl 生命周期测试失败：cloexec 必须遵循 Linux 的 fd 槽位语义\n",
                stderr);
    }
    return passed;
}

int main(void) {
    alarm(10);
    bool passed = run_ioctl_close_lifecycle() &&
            run_cloexec_slot_lifecycle();
    alarm(0);
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
    if (!passed)
        return 1;
    puts("ioctl 生命周期测试通过");
    return 0;
}

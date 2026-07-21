#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/errno.h"
#include "kernel/task.h"

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

static atomic_uint failures = ATOMIC_VAR_INIT(0);
static atomic_uint last_close_calls = ATOMIC_VAR_INIT(0);
static atomic_uint nonlast_close_calls = ATOMIC_VAR_INIT(0);
static atomic_uint cleanup_calls = ATOMIC_VAR_INIT(0);

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "TTY 打开生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed); \
    } \
} while (0)

struct open_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool block_next;
    bool entered;
    bool allow_finish;
    bool worker_finished;
    int result;
};

static struct open_gate *active_gate;

static int gated_open(struct tty *tty) {
    (void) tty;
    struct open_gate *gate = active_gate;
    pthread_mutex_lock(&gate->mutex);
    if (gate->block_next) {
        gate->block_next = false;
        gate->entered = true;
        pthread_cond_broadcast(&gate->changed);
        while (!gate->allow_finish)
            pthread_cond_wait(&gate->changed, &gate->mutex);
    }
    int result = gate->result;
    pthread_mutex_unlock(&gate->mutex);
    return result;
}

static int tracked_close(struct tty *tty) {
    lock(&tty->lock);
    if (tty->open_count == 0) {
        tty->hung_up = true;
        atomic_fetch_add_explicit(
                &last_close_calls, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(
                &nonlast_close_calls, 1, memory_order_relaxed);
    }
    unlock(&tty->lock);
    return 0;
}

static void tracked_cleanup(struct tty *tty) {
    EXPECT(!lock_owned_by_current(&tty->lock),
            "cleanup 不持有对象锁");
    EXPECT(lock_owned_by_current(&ttys_lock),
            "cleanup 保持全局发布锁契约");
    atomic_fetch_add_explicit(&cleanup_calls, 1, memory_order_relaxed);
}

static const struct tty_driver_ops gated_ops = {
    .open = gated_open,
    .close = tracked_close,
    .cleanup = tracked_cleanup,
};

struct open_worker {
    struct task task;
    struct fd *fd;
    int major;
    int minor;
    struct open_gate *completion_gate;
    int result;
};

static void *open_worker_main(void *opaque) {
    struct open_worker *worker = opaque;
    current = &worker->task;
    worker->result = dev_open(worker->major, worker->minor,
            DEV_CHAR, worker->fd);
    if (worker->completion_gate != NULL) {
        pthread_mutex_lock(&worker->completion_gate->mutex);
        worker->completion_gate->worker_finished = true;
        pthread_cond_broadcast(&worker->completion_gate->changed);
        pthread_mutex_unlock(&worker->completion_gate->mutex);
    }
    current = NULL;
    return NULL;
}

static void test_pending_open_keeps_last_close_live(void) {
    struct open_gate gate = {};
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化打开闸门 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化打开闸门 condition");
    active_gate = &gate;

    struct tty *slots[1] = {};
    bool reserved[1] = {};
    struct tty_driver driver = {
        .ops = &gated_ops,
        .major = TTY_CONSOLE_MAJOR,
        .ttys = slots,
        .reserved = reserved,
        .limit = 1,
    };
    struct tty_driver *saved_driver = tty_drivers[TTY_CONSOLE_MAJOR];
    tty_drivers[TTY_CONSOLE_MAJOR] = &driver;

    struct task owner = {};
    current = &owner;
    struct fd *first = fd_create(NULL);
    EXPECT(first != NULL, "创建首个 tty fd");
    if (first == NULL)
        goto restore_driver;
    first->flags = O_RDWR_ | O_NOCTTY_;
    EXPECT(dev_open(TTY_CONSOLE_MAJOR, 0, DEV_CHAR, first) == 0,
            "完成首个 tty 打开");
    if (first->tty == NULL) {
        fd_close(first);
        goto restore_driver;
    }
    struct tty *tty = first->tty;

    struct open_worker worker = {
        .fd = fd_create(NULL),
        .major = TTY_CONSOLE_MAJOR,
        .result = -1,
    };
    EXPECT(worker.fd != NULL, "创建并发打开 fd");
    if (worker.fd == NULL) {
        fd_close(first);
        goto restore_driver;
    }
    worker.fd->flags = O_RDWR_ | O_NOCTTY_;

    pthread_mutex_lock(&gate.mutex);
    gate.block_next = true;
    pthread_mutex_unlock(&gate.mutex);

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    pthread_t opener;
    int create_error = pthread_create(
            &opener, NULL, open_worker_main, &worker);
    EXPECT(create_error == 0, "建立受控并发打开线程");
    if (create_error != 0) {
        fd_close(worker.fd);
        fd_close(first);
        alarm(0);
        goto restore_driver;
    }

    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex);

    lock(&tty->lock);
    EXPECT(tty->refcount == 2 && tty->open_count == 2,
            "driver open 阻塞时已经原子计入在途文件");
    unlock(&tty->lock);

    EXPECT(fd_close(first) == 0,
            "关闭旧 fd 不会越过在途 open 触发最终关闭");
    first = NULL;
    lock(&tty->lock);
    EXPECT(tty->open_count == 1 && !tty->hung_up,
            "在途 open 保持对象可用且不产生伪 hangup");
    unlock(&tty->lock);
    EXPECT(atomic_load_explicit(&nonlast_close_calls,
            memory_order_relaxed) == 1 &&
            atomic_load_explicit(&last_close_calls,
            memory_order_relaxed) == 0,
            "旧 fd 关闭被识别为非最终文件关闭");

    pthread_mutex_lock(&gate.mutex);
    gate.allow_finish = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(pthread_join(opener, NULL) == 0,
            "在途 open 有界完成");
    alarm(0);
    EXPECT(worker.result == 0 && worker.fd->tty == tty,
            "并发打开发布同一存活 tty");

    EXPECT(fd_close(worker.fd) == 0,
            "最终 fd 关闭完成对象析构");
    worker.fd = NULL;
    EXPECT(atomic_load_explicit(&last_close_calls,
            memory_order_relaxed) == 1,
            "只在真正最后一个文件关闭时执行 hangup");
    EXPECT(atomic_load_explicit(&cleanup_calls,
            memory_order_relaxed) == 1,
            "最终引用触发且只触发一次 cleanup");
    EXPECT(slots[0] == NULL && !reserved[0],
            "并发打开测试最终清除发布槽和 reservation");

restore_driver:
    tty_drivers[TTY_CONSOLE_MAJOR] = saved_driver;
    current = NULL;
    active_gate = NULL;
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

static void test_ptmx_master_never_becomes_controlling(void) {
    struct tgroup group = {
        .sid = 8301,
        .pgid = 8301,
    };
    lock_init(&group.lock);
    struct task owner = {
        .pid = 8301,
        .euid = 8301,
        .egid = 8302,
        .group = &group,
    };
    group.leader = &owner;
    current = &owner;

    struct fd *master_fd = fd_create(NULL);
    EXPECT(master_fd != NULL, "创建 PTMX master fd");
    if (master_fd == NULL)
        goto out;
    // 故意不带 O_NOCTTY，验证 PTMX master 的设备语义本身禁止自动控制终端。
    master_fd->flags = O_RDWR_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "session leader 打开 PTMX master");
    if (err < 0) {
        fd_close(master_fd);
        goto out;
    }

    int pty_num = master_fd->tty->num;
    lock(&group.lock);
    struct tty *unexpected_controlling = group.tty;
    if (unexpected_controlling != NULL)
        group.tty = NULL;
    unlock(&group.lock);
    EXPECT(unexpected_controlling == NULL,
            "PTMX master 不会自动成为 controlling tty");

    EXPECT(fd_close(master_fd) == 0,
            "关闭唯一 master fd 立即回收 PTY 对");
    master_fd = NULL;
    if (unexpected_controlling != NULL) {
        // 失败态也要解除旧实现误加的内部引用，避免污染后续测试。
        lock(&ttys_lock);
        tty_release(unexpected_controlling);
        unlock(&ttys_lock);
    }

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == NULL &&
            pty_slave.ttys[pty_num] == NULL &&
            !pty_master.reserved[pty_num] &&
            !pty_slave.reserved[pty_num],
            "无 controlling 引用时 master 关闭完整清理主从槽位");
    unlock(&ttys_lock);

out:
    current = NULL;
    lock_destroy(&group.lock);
}

static void test_explicit_master_controlling_close(void) {
    struct tgroup group = {
        .sid = 8401,
        .pgid = 8401,
    };
    lock_init(&group.lock);
    struct task leader = {.pid = 8401};
    struct task owner = {
        .pid = 8402,
        .euid = 8401,
        .egid = 8402,
        .group = &group,
    };
    group.leader = &leader;
    current = &owner;

    struct fd *master_fd = fd_create(NULL);
    EXPECT(master_fd != NULL, "为显式 controlling 测试创建 master fd");
    if (master_fd == NULL)
        goto out;
    master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "创建显式 controlling 测试 PTY 对");
    if (err < 0) {
        fd_close(master_fd);
        goto out;
    }

    struct tty *master = master_fd->tty;
    int pty_num = master->num;
    dword_t unlocked = 0;
    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSPTLCK_, &unlocked) == 0,
            "解锁显式 controlling 测试的 slave");
    struct fd *slave_fd = fd_create(NULL);
    EXPECT(slave_fd != NULL, "创建显式 controlling 测试的 slave fd");
    if (slave_fd == NULL) {
        fd_close(master_fd);
        goto out;
    }
    slave_fd->flags = O_RDWR_ | O_NOCTTY_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
            DEV_CHAR, slave_fd);
    EXPECT(err == 0, "打开显式 controlling 测试的 slave");
    if (err < 0) {
        fd_close(slave_fd);
        fd_close(master_fd);
        goto out;
    }
    struct tty *slave = slave_fd->tty;

    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSCTTY_, NULL) == 0,
            "session leader 进程的非 leader 线程可显式设置 controlling tty");
    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSCTTY_, NULL) == 0,
            "同一非 leader 线程重复 TIOCSCTTY 保持幂等成功");
    lock(&group.lock);
    EXPECT(group.tty == slave,
            "经 master 发起的 TIOCSCTTY 为 group 保留 slave 引用");
    unlock(&group.lock);

    lock(&slave->lock);
    slave->fg_group = 0;
    unlock(&slave->lock);
    dword_t expected_fg_group = group.pgid;
    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSPGRP_, &expected_fg_group) == 0,
            "master TIOCSPGRP 在真实 slave 上设置前台进程组");
    dword_t observed_fg_group = 0;
    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCGPGRP_, &observed_fg_group) == 0 &&
            observed_fg_group == expected_fg_group,
            "master TIOCGPGRP 从同一 slave 读取前台进程组");

    struct fd *live_dev_tty = fd_create(NULL);
    EXPECT(live_dev_tty != NULL, "为存活控制终端创建 /dev/tty fd");
    if (live_dev_tty != NULL) {
        live_dev_tty->flags = O_RDWR_ | O_NOCTTY_;
        EXPECT(dev_open(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR,
                DEV_CHAR, live_dev_tty) == 0 &&
                live_dev_tty->tty == slave,
                "经 master 设置后 /dev/tty 打开真实 slave");
        fd_close(live_dev_tty);
    }

    EXPECT(fd_close(master_fd) == 0,
            "关闭唯一 master 文件会执行永久 hangup");
    master_fd = NULL;
    lock(&slave->lock);
    bool slave_hung_up = slave->hung_up;
    unlock(&slave->lock);
    lock(&ttys_lock);
    bool master_released = pty_master.ttys[pty_num] == NULL;
    unlock(&ttys_lock);
    EXPECT(master_released && slave_hung_up,
            "slave controlling 引用不阻止 master 最后文件关闭和回收");

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    char byte;
    EXPECT(slave_fd->ops->read(slave_fd, &byte, 1) == 0,
            "显式 controlling master 关闭后 slave 读到 EOF");
    alarm(0);

    struct fd *reopened_slave = fd_create(NULL);
    EXPECT(reopened_slave != NULL, "为 master 关闭后的 slave 重开检查创建 fd");
    if (reopened_slave != NULL) {
        reopened_slave->flags = O_RDWR_ | O_NOCTTY_;
        EXPECT(dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
                DEV_CHAR, reopened_slave) == _EIO,
                "没有打开 master 文件时不能伪重开 slave");
        fd_close(reopened_slave);
    }

    struct fd *dev_tty_fd = fd_create(NULL);
    EXPECT(dev_tty_fd != NULL, "为 /dev/tty 重开检查创建 fd");
    if (dev_tty_fd != NULL) {
        dev_tty_fd->flags = O_RDWR_ | O_NOCTTY_;
        EXPECT(dev_open(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR,
                DEV_CHAR, dev_tty_fd) == _EIO,
                "master 关闭后已挂断 slave 经 /dev/tty 重开返回 EIO");
        fd_close(dev_tty_fd);
    }

    EXPECT(fd_close(slave_fd) == 0,
            "关闭显式 controlling 测试的 slave fd");
    slave_fd = NULL;
    lock(&group.lock);
    EXPECT(group.tty == slave,
            "文件关闭后仅保留 group 的 slave controlling 引用");
    group.tty = NULL;
    unlock(&group.lock);
    lock(&slave->lock);
    slave->session = 0;
    unlock(&slave->lock);
    lock(&ttys_lock);
    tty_release(slave);
    unlock(&ttys_lock);

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == NULL &&
            pty_slave.ttys[pty_num] == NULL &&
            !pty_master.reserved[pty_num] &&
            !pty_slave.reserved[pty_num],
            "释放 controlling 引用后完整清理显式 master PTY 对");
    unlock(&ttys_lock);

out:
    current = NULL;
    lock_destroy(&group.lock);
}

static void test_failed_master_open_never_becomes_pending(void) {
    struct open_gate gate = {
        .block_next = true,
        .result = _EIO,
    };
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化失败 master open 闸门 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化失败 master open 闸门 condition");

    struct tgroup group = {
        .sid = 8451,
        .pgid = 8451,
    };
    lock_init(&group.lock);
    struct task owner = {
        .pid = 8451,
        .euid = 8451,
        .egid = 8452,
        .group = &group,
    };
    group.leader = &owner;
    current = &owner;

    struct fd *master_fd = fd_create(NULL);
    struct fd *probe_slave = NULL;
    struct tty *master = NULL;
    int pty_num = -1;
    bool ops_swapped = false;
    bool worker_started = false;
    pthread_t opener = {};
    struct open_worker worker = {};
    const struct tty_driver_ops *saved_ops = pty_master.ops;
    struct tty_driver_ops gated_master_ops = *saved_ops;
    gated_master_ops.open = gated_open;

    EXPECT(master_fd != NULL, "创建 pending master 竞态的 PTMX fd");
    if (master_fd == NULL)
        goto cleanup;
    master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "创建 pending master 竞态的 PTY 对");
    if (err < 0)
        goto cleanup;
    master = master_fd->tty;
    pty_num = master->num;

    dword_t unlocked = 0;
    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSPTLCK_, &unlocked) == 0,
            "解锁 pending master 竞态的 slave");
    pty_master.ops = &gated_master_ops;
    ops_swapped = true;
    active_gate = &gate;

    worker.task.group = &group;
    worker.fd = fd_create(NULL);
    worker.major = TTY_PSEUDO_MASTER_MAJOR;
    worker.minor = pty_num;
    worker.completion_gate = &gate;
    worker.result = 1;
    EXPECT(worker.fd != NULL, "创建失败 /dev/tty open 的 fd");
    if (worker.fd == NULL)
        goto cleanup;
    worker.fd->flags = O_RDWR_ | O_NOCTTY_;

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    int create_error = pthread_create(
            &opener, NULL, open_worker_main, &worker);
    EXPECT(create_error == 0, "建立失败 master open 线程");
    if (create_error != 0)
        goto cleanup;
    worker_started = true;

    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered && !gate.worker_finished)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    bool entered_driver_open = gate.entered;
    bool rejected_early = gate.worker_finished && !gate.entered;
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(rejected_early,
            "非 PTMX master 在计入 pending open 前直接返回 EIO");

    EXPECT(fd_close(master_fd) == 0,
            "关闭最后成功 master fd 不受失败 open 干扰");
    master_fd = NULL;

    probe_slave = fd_create(NULL);
    EXPECT(probe_slave != NULL,
            "创建最后 master fd 关闭后的 slave 探针");
    if (probe_slave != NULL) {
        probe_slave->flags = O_RDWR_ | O_NOCTTY_;
        err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
                DEV_CHAR, probe_slave);
        EXPECT(err == _EIO,
                "失败 master open 窗口不能让 slave 伪成功重开");
    }

    pthread_mutex_lock(&gate.mutex);
    gate.allow_finish = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(pthread_join(opener, NULL) == 0,
            "失败 master open 线程有界退出");
    worker_started = false;
    alarm(0);
    EXPECT(worker.result == _EIO && !entered_driver_open,
            "直接打开 PTY master 时从未调用必失败 driver open");

cleanup:
    if (worker_started) {
        pthread_mutex_lock(&gate.mutex);
        gate.allow_finish = true;
        pthread_cond_broadcast(&gate.changed);
        pthread_mutex_unlock(&gate.mutex);
        pthread_join(opener, NULL);
    }
    alarm(0);
    if (ops_swapped)
        pty_master.ops = saved_ops;
    active_gate = NULL;
    if (worker.fd != NULL)
        fd_close(worker.fd);
    if (probe_slave != NULL)
        fd_close(probe_slave);
    if (master_fd != NULL)
        fd_close(master_fd);

    lock(&group.lock);
    struct tty *controlling = group.tty;
    group.tty = NULL;
    unlock(&group.lock);
    if (controlling != NULL) {
        lock(&controlling->lock);
        controlling->session = 0;
        unlock(&controlling->lock);
        lock(&ttys_lock);
        tty_release(controlling);
        unlock(&ttys_lock);
    }

    if (pty_num >= 0) {
        lock(&ttys_lock);
        EXPECT(pty_master.ttys[pty_num] == NULL &&
                pty_slave.ttys[pty_num] == NULL &&
                !pty_master.reserved[pty_num] &&
                !pty_slave.reserved[pty_num],
                "pending master 竞态测试最终完整清理 PTY 对");
        unlock(&ttys_lock);
    }
    current = NULL;
    lock_destroy(&group.lock);
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

static void test_non_session_leader_rejected(void) {
    struct tgroup group = {
        .sid = 8600,
        .pgid = 8601,
    };
    lock_init(&group.lock);
    struct task leader = {.pid = 8601};
    struct task caller = {
        .pid = 8601,
        .euid = 8601,
        .egid = 8602,
        .group = &group,
    };
    group.leader = &leader;
    current = &caller;

    struct fd *master_fd = fd_create(NULL);
    EXPECT(master_fd != NULL, "为非 session leader 测试创建 master fd");
    if (master_fd == NULL)
        goto out;
    master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "为非 session leader 测试打开 PTMX");
    if (err < 0) {
        fd_close(master_fd);
        goto out;
    }
    int pty_num = master_fd->tty->num;

    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSCTTY_, NULL) == _EPERM,
            "非 session leader 进程显式 TIOCSCTTY 返回 EPERM");
    lock(&group.lock);
    EXPECT(group.tty == NULL,
            "拒绝 TIOCSCTTY 不写入 group controlling tty");
    unlock(&group.lock);
    EXPECT(fd_close(master_fd) == 0,
            "关闭被拒绝场景的 master fd");

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == NULL &&
            pty_slave.ttys[pty_num] == NULL,
            "被拒绝场景最终回收 PTY 对");
    unlock(&ttys_lock);

out:
    current = NULL;
    lock_destroy(&group.lock);
}

static void test_session_leader_thread_auto_controls_slave(void) {
    struct tgroup group = {
        .sid = 8701,
        .pgid = 8701,
    };
    lock_init(&group.lock);
    struct task leader = {.pid = 8701};
    struct task opener = {
        .pid = 8702,
        .euid = 8701,
        .egid = 8702,
        .group = &group,
    };
    group.leader = &leader;
    current = &opener;

    struct fd *master_fd = fd_create(NULL);
    EXPECT(master_fd != NULL, "为非 leader 线程自动控制测试创建 master");
    if (master_fd == NULL)
        goto out;
    master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "创建非 leader 线程自动控制测试 PTY");
    if (err < 0) {
        fd_close(master_fd);
        goto out;
    }
    int pty_num = master_fd->tty->num;
    dword_t unlocked = 0;
    EXPECT(master_fd->ops->ioctl(master_fd,
            TIOCSPTLCK_, &unlocked) == 0,
            "解锁自动 controlling 测试的 slave");

    struct fd *slave_fd = fd_create(NULL);
    EXPECT(slave_fd != NULL, "为自动 controlling 测试创建 slave fd");
    if (slave_fd == NULL) {
        fd_close(master_fd);
        goto out;
    }
    // 故意不带 O_NOCTTY：调用线程不是 thread-group leader，但进程是 session leader。
    slave_fd->flags = O_RDWR_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
            DEV_CHAR, slave_fd);
    EXPECT(err == 0, "session leader 进程的非 leader 线程打开 slave");
    if (err < 0) {
        fd_close(slave_fd);
        fd_close(master_fd);
        goto out;
    }
    struct tty *slave = slave_fd->tty;
    lock(&group.lock);
    struct tty *controlling = group.tty;
    group.tty = NULL;
    unlock(&group.lock);
    EXPECT(controlling == slave,
            "非 leader 线程仍为 session-leader 进程自动取得 slave controlling tty");
    if (controlling != NULL) {
        lock(&controlling->lock);
        controlling->session = 0;
        unlock(&controlling->lock);
        lock(&ttys_lock);
        tty_release(controlling);
        unlock(&ttys_lock);
    }

    EXPECT(fd_close(slave_fd) == 0 &&
            fd_close(master_fd) == 0,
            "关闭自动 controlling 测试的主从 fd");
    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == NULL &&
            pty_slave.ttys[pty_num] == NULL,
            "自动 controlling 测试最终回收 PTY 对");
    unlock(&ttys_lock);

out:
    current = NULL;
    lock_destroy(&group.lock);
}

int main(void) {
    test_pending_open_keeps_last_close_live();
    test_ptmx_master_never_becomes_controlling();
    test_explicit_master_controlling_close();
    test_failed_master_open_never_becomes_pending();
    test_non_session_leader_rejected();
    test_session_leader_thread_auto_controls_slave();

    unsigned count = atomic_load_explicit(
            &failures, memory_order_relaxed);
    if (count != 0) {
        fprintf(stderr, "TTY 打开生命周期测试共发现 %u 项失败\n", count);
        return 1;
    }
    puts("TTY 打开生命周期测试通过");
    return 0;
}

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "debug.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

struct handoff_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct tty *master;
    struct tty *slave;
    pthread_t reader;
    unsigned master_lock_calls;
    bool reader_ready;
    bool start_reader;
    bool handoff_entered;
    bool allow_handoff;
    bool held_slave;
};

static struct handoff_gate *active_handoff_gate;
static void pty_handoff_lock(lock_t *lock);
void pty_handoff_tty_set_winsize(
        struct tty *tty, struct winsize_ winsize);

// 测试副本只替换内部加锁入口并重命名公开符号；真实 PTY 的 open/close
// 仍由库实现执行，生产代码不需要暴露竞态 hook。
#undef lock
#define lock(lock_ptr) pty_handoff_lock((lock_ptr))
#define tty_drivers pty_handoff_tty_drivers
#define ttys_lock pty_handoff_ttys_lock
#define tty_alloc pty_handoff_tty_alloc
#define tty_destroy_unpublished pty_handoff_tty_destroy_unpublished
#define tty_get pty_handoff_tty_get
#define tty_release pty_handoff_tty_release
#define tty_open pty_handoff_tty_open
#define tty_input pty_handoff_tty_input
#define tty_set_winsize pty_handoff_tty_set_winsize
#define tty_hangup pty_handoff_tty_hangup
#define tty_notify_peer_closed pty_handoff_tty_notify_peer_closed
#define console_major pty_handoff_console_major
#define console_minor pty_handoff_console_minor
#define tty_dev pty_handoff_tty_dev
#undef DEFAULT_CHANNEL
#include "fs/tty.c"
#undef tty_dev
#undef console_minor
#undef console_major
#undef tty_notify_peer_closed
#undef tty_hangup
#undef tty_set_winsize
#undef tty_input
#undef tty_open
#undef tty_release
#undef tty_get
#undef tty_destroy_unpublished
#undef tty_alloc
#undef ttys_lock
#undef tty_drivers
#undef lock
#define lock(lock_ptr) __lock((lock_ptr), __FILE__, __LINE__)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "PTY 锁交接测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static bool lock_owned_by_thread(lock_t *lock, pthread_t thread) {
    pthread_t owner = atomic_load_explicit(
            &lock->owner, memory_order_acquire);
    pthread_t empty = zero_init(pthread_t);
    return memcmp(&owner, &empty, sizeof(owner)) != 0 &&
            pthread_equal(owner, thread);
}

static void pty_handoff_lock(lock_t *lock) {
    struct handoff_gate *gate = active_handoff_gate;
    if (gate != NULL && lock == &gate->master->lock &&
            pthread_equal(pthread_self(), gate->reader)) {
        pthread_mutex_lock(&gate->mutex);
        gate->master_lock_calls++;
        if (gate->master_lock_calls == 2) {
            gate->held_slave =
                    lock_owned_by_current(&gate->slave->lock);
            gate->handoff_entered = true;
            pthread_cond_broadcast(&gate->changed);
            while (!gate->allow_handoff)
                pthread_cond_wait(&gate->changed, &gate->mutex);
        }
        pthread_mutex_unlock(&gate->mutex);
    }
    __lock(lock, __FILE__, __LINE__);
}

struct pty_pair {
    struct fd *master_fd;
    struct fd *slave_fd;
    struct tty *master;
    struct tty *slave;
    int num;
};

static bool open_pair(struct pty_pair *pair) {
    *pair = (struct pty_pair) {0};
    pair->master_fd = fd_create(NULL);
    if (pair->master_fd == NULL)
        return false;
    pair->master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int error = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, pair->master_fd);
    if (error < 0)
        return false;

    pair->master = pair->master_fd->tty;
    pair->num = pair->master->num;
    dword_t unlocked = 0;
    if (pair->master_fd->ops->ioctl(
            pair->master_fd, TIOCSPTLCK_, &unlocked) < 0)
        return false;

    pair->slave_fd = fd_create(NULL);
    if (pair->slave_fd == NULL)
        return false;
    pair->slave_fd->flags = O_RDWR_ | O_NOCTTY_;
    error = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pair->num,
            DEV_CHAR, pair->slave_fd);
    if (error < 0)
        return false;
    pair->slave = pair->slave_fd->tty;
    return true;
}

struct read_call {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
    struct handoff_gate *gate;
    struct fd *fd;
    ssize_t result;
};

static void read_call_init(
        struct read_call *call, struct handoff_gate *gate,
        struct fd *fd) {
    *call = (struct read_call) {
        .gate = gate,
        .fd = fd,
        .result = -1,
    };
    call->group.pgid = 7601;
    call->task.group = &call->group;
    call->task.sighand = &call->sighand;
    lock_init(&call->group.lock);
    lock_init(&call->sighand.lock);
    lock_init(&call->task.waiting_cond_lock);
}

static void *run_master_read(void *opaque) {
    struct read_call *call = opaque;
    current = &call->task;
    pthread_mutex_lock(&call->gate->mutex);
    call->gate->reader = pthread_self();
    call->gate->reader_ready = true;
    pthread_cond_broadcast(&call->gate->changed);
    while (!call->gate->start_reader)
        pthread_cond_wait(
                &call->gate->changed, &call->gate->mutex);
    pthread_mutex_unlock(&call->gate->mutex);

    char byte;
    call->result = pty_handoff_tty_dev.fd.read(
            call->fd, &byte, 1);
    current = NULL;
    return NULL;
}

struct peer_operation {
    struct fd *fd;
    int pty_num;
    bool reopen;
    int result;
};

static void *run_peer_operation(void *opaque) {
    struct peer_operation *operation = opaque;
    if (operation->reopen) {
        operation->fd->flags = O_RDWR_ | O_NOCTTY_;
        operation->result = dev_open(
                TTY_PSEUDO_SLAVE_MAJOR, operation->pty_num,
                DEV_CHAR, operation->fd);
    } else {
        operation->result = fd_close(operation->fd);
    }
    return NULL;
}

static int begin_handoff(
        struct handoff_gate *gate, struct read_call *read,
        pthread_t *reader) {
    CHECK(pthread_mutex_init(&gate->mutex, NULL) == 0 &&
            pthread_cond_init(&gate->changed, NULL) == 0,
            "初始化 master/slave 交接闸门");
    active_handoff_gate = gate;
    CHECK(pthread_create(reader, NULL, run_master_read, read) == 0,
            "建立 master read 线程");

    pthread_mutex_lock(&gate->mutex);
    while (!gate->reader_ready)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    gate->start_reader = true;
    pthread_cond_broadcast(&gate->changed);
    while (!gate->handoff_entered)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    bool held_slave = gate->held_slave;
    pthread_mutex_unlock(&gate->mutex);
    CHECK(held_slave,
            "slave 状态快照后仍持有 slave 锁再申请 master");
    return 0;
}

static void allow_handoff(struct handoff_gate *gate) {
    pthread_mutex_lock(&gate->mutex);
    gate->allow_handoff = true;
    pthread_cond_broadcast(&gate->changed);
    pthread_mutex_unlock(&gate->mutex);
}

static int test_close_notification_handoff(void) {
    struct task owner = {
        .uid = 7501,
        .gid = 7502,
        .euid = 7501,
        .egid = 7502,
    };
    current = &owner;
    struct pty_pair pair;
    CHECK(open_pair(&pair), "为 close 交接场景创建真实 PTY 对");
    current = NULL;

    struct handoff_gate gate = {
        .master = pair.master,
        .slave = pair.slave,
    };
    struct read_call read;
    read_call_init(&read, &gate, pair.master_fd);
    pthread_t reader;
    CHECK(begin_handoff(&gate, &read, &reader) == 0,
            "把 master read 停在 close 交接窗口");

    lock(&pair.master->lock);
    struct peer_operation close = {
        .fd = pair.slave_fd,
        .pty_num = pair.num,
        .result = -1,
    };
    pthread_t closer;
    CHECK(pthread_create(&closer, NULL,
            run_peer_operation, &close) == 0,
            "建立最后 slave close 线程");
    while (!lock_owned_by_thread(&ttys_lock, closer))
        sched_yield();
    CHECK(lock_owned_by_thread(&pair.slave->lock, reader),
            "close 在 open_count 变更前被 slave 锁挡住");

    allow_handoff(&gate);
    unlock(&pair.master->lock);
    CHECK(pthread_join(closer, NULL) == 0 && close.result == 0,
            "最后 slave close 有界完成");
    pair.slave_fd = NULL;
    CHECK(pthread_join(reader, NULL) == 0,
            "close 通知唤醒 master read");
    CHECK(read.result == _EIO,
            "master read 在 close 通知后返回 EIO");
    active_handoff_gate = NULL;

    current = &owner;
    CHECK(fd_close(pair.master_fd) == 0,
            "清理 close 交接场景的 master");
    current = NULL;
    return 0;
}

static int test_reopen_snapshot_handoff(void) {
    struct tgroup owner_group = {.pgid = 7701};
    lock_init(&owner_group.lock);
    struct task owner = {
        .uid = 7701,
        .gid = 7702,
        .euid = 7701,
        .egid = 7702,
        .group = &owner_group,
    };
    current = &owner;
    struct pty_pair pair;
    CHECK(open_pair(&pair), "为 reopen 交接场景创建真实 PTY 对");
    CHECK(fd_close(pair.slave_fd) == 0,
            "先关闭 reopen 场景的最后 slave fd");
    pair.slave_fd = NULL;
    current = NULL;

    struct handoff_gate gate = {
        .master = pair.master,
        .slave = pair.slave,
    };
    struct read_call read;
    read_call_init(&read, &gate, pair.master_fd);
    pthread_t reader;
    CHECK(begin_handoff(&gate, &read, &reader) == 0,
            "把 master read 停在 reopen 交接窗口");

    lock(&pair.master->lock);
    struct fd *reopened_fd = fd_create(NULL);
    CHECK(reopened_fd != NULL, "创建并发 reopen fd");
    struct peer_operation reopen = {
        .fd = reopened_fd,
        .pty_num = pair.num,
        .reopen = true,
        .result = -1,
    };
    pthread_t reopener;
    CHECK(pthread_create(&reopener, NULL,
            run_peer_operation, &reopen) == 0,
            "建立 slave reopen 线程");
    while (!lock_owned_by_thread(&ttys_lock, reopener))
        sched_yield();
    CHECK(lock_owned_by_thread(&pair.slave->lock, reader),
            "reopen 在 open_count 变更前被 slave 锁挡住");

    allow_handoff(&gate);
    unlock(&pair.master->lock);
    CHECK(pthread_join(reader, NULL) == 0 && read.result == _EIO,
            "master read 在线性化的 closed 快照上返回 EIO");
    CHECK(pthread_join(reopener, NULL) == 0 && reopen.result == 0,
            "slave reopen 在旧 read 完成后有界成功");
    active_handoff_gate = NULL;

    current = &owner;
    char observed = 0;
    CHECK(reopened_fd->ops->write(reopened_fd, "r", 1) == 1 &&
            pair.master_fd->ops->read(
                    pair.master_fd, &observed, 1) == 1 &&
            observed == 'r',
            "reopen 完成后的新 I/O 不继承陈旧 EIO");
    CHECK(fd_close(reopened_fd) == 0 &&
            fd_close(pair.master_fd) == 0,
            "清理 reopen 交接场景的 PTY 对");
    current = NULL;
    return 0;
}

static bool run_isolated(
        int (*scenario)(void), const char *name) {
    pid_t host_child = fork();
    if (host_child < 0) {
        fprintf(stderr, "PTY 锁交接测试无法建立 %s 子进程：%s\n",
                name, strerror(errno));
        return false;
    }
    if (host_child == 0) {
        signal(SIGUSR1, SIG_IGN);
        alarm(10);
        _exit(scenario());
    }

    int status;
    pid_t waited;
    do {
        waited = waitpid(host_child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == host_child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0)
        return true;
    if (waited == host_child && WIFSIGNALED(status)) {
        fprintf(stderr, "PTY 锁交接测试的 %s 场景被 host 信号 %d 终止\n",
                name, WTERMSIG(status));
    } else {
        fprintf(stderr, "PTY 锁交接测试的 %s 场景返回状态 %d\n",
                name, waited == host_child && WIFEXITED(status) ?
                WEXITSTATUS(status) : -1);
    }
    return false;
}

int main(void) {
    if (!run_isolated(test_close_notification_handoff, "close") ||
            !run_isolated(test_reopen_snapshot_handoff, "reopen"))
        return 1;
    puts("PTY 锁交接测试通过");
    return 0;
}

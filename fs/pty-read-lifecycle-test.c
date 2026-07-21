#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fs/dev.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "fs/devices.h"
#include "kernel/errno.h"
#include "kernel/task.h"

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

static atomic_uint failures = ATOMIC_VAR_INIT(0);

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "PTY 阻塞读测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed); \
    } \
} while (0)

struct pty_pair {
    struct fd *master_fd;
    struct fd *slave_fd;
    struct tty *master;
    struct tty *slave;
    int num;
};

static bool open_pair(struct pty_pair *pair) {
    pair->master_fd = fd_create(NULL);
    if (pair->master_fd == NULL)
        return false;
    pair->master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, pair->master_fd);
    if (err < 0)
        goto fail_master;

    pair->master = pair->master_fd->tty;
    pair->num = pair->master->num;
    dword_t unlocked = 0;
    if (pair->master_fd->ops->ioctl(
            pair->master_fd, TIOCSPTLCK_, &unlocked) < 0)
        goto fail_master;

    pair->slave_fd = fd_create(NULL);
    if (pair->slave_fd == NULL)
        goto fail_master;
    pair->slave_fd->flags = O_RDWR_ | O_NOCTTY_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pair->num,
            DEV_CHAR, pair->slave_fd);
    if (err < 0)
        goto fail_slave;
    pair->slave = pair->slave_fd->tty;
    return true;

fail_slave:
    fd_close(pair->slave_fd);
    pair->slave_fd = NULL;
fail_master:
    fd_close(pair->master_fd);
    pair->master_fd = NULL;
    return false;
}

static void close_pair(struct pty_pair *pair) {
    if (pair->slave_fd != NULL) {
        fd_close(pair->slave_fd);
        pair->slave_fd = NULL;
    }
    if (pair->master_fd != NULL) {
        fd_close(pair->master_fd);
        pair->master_fd = NULL;
    }
}

struct blocked_read {
    struct task task;
    struct tgroup group;
    struct sighand sighand;
    struct fd *fd;
    ssize_t result;
};

static void blocked_read_init(struct blocked_read *read, struct fd *fd) {
    *read = (struct blocked_read) {.fd = fd, .result = -1};
    read->group.pgid = 7101;
    read->task.group = &read->group;
    read->task.sighand = &read->sighand;
    lock_init(&read->group.lock);
    lock_init(&read->sighand.lock);
    lock_init(&read->task.waiting_cond_lock);
}

static void blocked_read_destroy(struct blocked_read *read) {
    lock_destroy(&read->task.waiting_cond_lock);
    lock_destroy(&read->sighand.lock);
    lock_destroy(&read->group.lock);
}

static void *blocked_read_main(void *opaque) {
    struct blocked_read *read = opaque;
    current = &read->task;
    char byte;
    read->result = read->fd->ops->read(read->fd, &byte, 1);
    current = NULL;
    return NULL;
}

static void test_close_wakes_blocked_read(bool close_master) {
    struct task owner = {
        .uid = 7001,
        .gid = 7002,
        .euid = 7001,
        .egid = 7002,
    };
    current = &owner;

    struct pty_pair pair = {};
    EXPECT(open_pair(&pair), "创建真实 PTY 主从端");
    if (pair.master_fd == NULL || pair.slave_fd == NULL)
        goto out_current;

    struct tty *target = close_master ? pair.slave : pair.master;
    struct fd *target_fd = close_master ? pair.slave_fd : pair.master_fd;
    struct blocked_read read;
    blocked_read_init(&read, target_fd);

    pthread_t reader;
    int create_error = pthread_create(
            &reader, NULL, blocked_read_main, &read);
    EXPECT(create_error == 0, "建立阻塞读线程");
    if (create_error != 0) {
        blocked_read_destroy(&read);
        close_pair(&pair);
        goto out_current;
    }

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    bool waiting = false;
    while (!waiting) {
        lock(&read.task.waiting_cond_lock);
        waiting = read.task.waiting_cond == &target->produced &&
                read.task.waiting_lock == &target->lock;
        unlock(&read.task.waiting_cond_lock);
        if (!waiting)
            sched_yield();
    }
    EXPECT(waiting, "read 精确阻塞在目标端 produced 条件上");

    if (close_master) {
        EXPECT(fd_close(pair.master_fd) == 0,
                "关闭 master 唤醒 slave 阻塞读");
        pair.master_fd = NULL;
    } else {
        EXPECT(fd_close(pair.slave_fd) == 0,
                "关闭最后一个 slave fd 唤醒 master 阻塞读");
        pair.slave_fd = NULL;
    }

    EXPECT(pthread_join(reader, NULL) == 0,
            "对端关闭后阻塞读线程有界退出");
    alarm(0);
    ssize_t expected = close_master ? 0 : _EIO;
    EXPECT(read.result == expected,
            close_master ?
            "master 关闭后 slave 阻塞读返回 EOF" :
            "最后一个 slave 关闭后 master 阻塞读返回 EIO");
    blocked_read_destroy(&read);
    close_pair(&pair);

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pair.num] == NULL &&
            pty_slave.ttys[pair.num] == NULL &&
            !pty_master.reserved[pair.num] &&
            !pty_slave.reserved[pair.num],
            "阻塞读测试结束后完整清理主从槽位");
    unlock(&ttys_lock);

out_current:
    current = NULL;
}

static bool set_slave_raw(struct fd *slave_fd) {
    struct termios_ raw;
    if (slave_fd->ops->ioctl(slave_fd, TCGETS_, &raw) < 0)
        return false;
    raw.iflags = 0;
    raw.oflags = 0;
    raw.lflags = 0;
    raw.cc[VMIN_] = 1;
    raw.cc[VTIME_] = 0;
    return slave_fd->ops->ioctl(slave_fd, TCSETS_, &raw) == 0;
}

static void test_buffered_close_semantics(bool close_master) {
    struct tgroup group = {.pgid = close_master ? 7301 : 7302};
    lock_init(&group.lock);
    struct task owner = {
        .uid = 7301,
        .gid = 7302,
        .euid = 7301,
        .egid = 7302,
        .group = &group,
    };
    current = &owner;

    struct pty_pair pair = {};
    EXPECT(open_pair(&pair), "为缓冲关闭语义创建真实 PTY 对");
    if (pair.master_fd == NULL || pair.slave_fd == NULL)
        goto out;
    EXPECT(set_slave_raw(pair.slave_fd),
            "把缓冲关闭测试的 slave 切到逐字节模式");

    const char *payload = close_master ? "abc" : "xyz";
    struct fd *writer = close_master ? pair.master_fd : pair.slave_fd;
    struct fd *reader = close_master ? pair.slave_fd : pair.master_fd;
    EXPECT(writer->ops->write(writer, payload, 3) == 3,
            "在关闭对端前写入三个待观察字节");

    if (close_master) {
        EXPECT(fd_close(pair.master_fd) == 0,
                "缓冲数据存在时关闭 master");
        pair.master_fd = NULL;
    } else {
        EXPECT(fd_close(pair.slave_fd) == 0,
                "缓冲数据存在时关闭最后一个 slave");
        pair.slave_fd = NULL;
    }

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    char observed[4] = {};
    ssize_t first = reader->ops->read(reader, observed, 3);
    if (close_master) {
        EXPECT(first == 0,
                "master hangup 丢弃未读 slave 输入并立即返回 EOF");
    } else {
        EXPECT(first == 3 && memcmp(observed, "xyz", 3) == 0,
                "slave half-close 后 master 先排空既有输入");
    }
    ssize_t second = reader->ops->read(reader, observed, 1);
    EXPECT(second == (close_master ? 0 : _EIO),
            close_master ?
            "slave 排空终态保持 EOF" :
            "master 排空终态随后返回 EIO");
    alarm(0);

    close_pair(&pair);
    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pair.num] == NULL &&
            pty_slave.ttys[pair.num] == NULL &&
            !pty_master.reserved[pair.num] &&
            !pty_slave.reserved[pair.num],
            "缓冲关闭测试结束后完整清理主从槽位");
    unlock(&ttys_lock);

out:
    current = NULL;
    lock_destroy(&group.lock);
}

int main(void) {
    test_close_wakes_blocked_read(true);
    test_close_wakes_blocked_read(false);
    test_buffered_close_semantics(true);
    test_buffered_close_semantics(false);

    unsigned count = atomic_load_explicit(
            &failures, memory_order_relaxed);
    if (count != 0) {
        fprintf(stderr, "PTY 阻塞读测试共发现 %u 项失败\n", count);
        return 1;
    }
    puts("PTY 阻塞读测试通过");
    return 0;
}

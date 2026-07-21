#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/dev.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "fs/devices.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define TEST_MAX_PTYS (1 << 12)
#define CONCURRENT_OPENERS 32

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

static atomic_uint failures = ATOMIC_VAR_INIT(0);
static atomic_uint init_calls = ATOMIC_VAR_INIT(0);
static atomic_uint cleanup_calls = ATOMIC_VAR_INIT(0);
static atomic_uint cleanup_with_tty_lock = ATOMIC_VAR_INIT(0);
static atomic_uint cleanup_without_ttys_lock = ATOMIC_VAR_INIT(0);
static atomic_uint cleanup_while_published = ATOMIC_VAR_INIT(0);
static atomic_uint failed_init_calls = ATOMIC_VAR_INIT(0);
static atomic_uint failed_cleanup_calls = ATOMIC_VAR_INIT(0);

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "PTY 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed); \
    } \
} while (0)

static void reset_driver_observations(void) {
    atomic_store_explicit(&init_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&cleanup_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&cleanup_with_tty_lock, 0,
            memory_order_relaxed);
    atomic_store_explicit(&cleanup_without_ttys_lock, 0,
            memory_order_relaxed);
    atomic_store_explicit(&cleanup_while_published, 0,
            memory_order_relaxed);
}

static int tracked_init(struct tty *tty) {
    (void) tty;
    atomic_fetch_add_explicit(&init_calls, 1, memory_order_relaxed);
    return 0;
}

static void tracked_cleanup(struct tty *tty) {
    atomic_fetch_add_explicit(&cleanup_calls, 1, memory_order_relaxed);
    if (lock_owned_by_current(&tty->lock))
        atomic_fetch_add_explicit(&cleanup_with_tty_lock, 1,
                memory_order_relaxed);
    if (!lock_owned_by_current(&ttys_lock)) {
        atomic_fetch_add_explicit(&cleanup_without_ttys_lock, 1,
                memory_order_relaxed);
    } else if (tty->driver->ttys[tty->num] != NULL) {
        atomic_fetch_add_explicit(&cleanup_while_published, 1,
                memory_order_relaxed);
    }
}

static const struct tty_driver_ops tracked_ops = {
    .init = tracked_init,
    .cleanup = tracked_cleanup,
};

static int failed_init(struct tty *tty) {
    (void) tty;
    atomic_fetch_add_explicit(&failed_init_calls, 1,
            memory_order_relaxed);
    return _EIO;
}

static void failed_cleanup(struct tty *tty) {
    (void) tty;
    atomic_fetch_add_explicit(&failed_cleanup_calls, 1,
            memory_order_relaxed);
}

static const struct tty_driver_ops failed_ops = {
    .init = failed_init,
    .cleanup = failed_cleanup,
};

static void release_tty(struct tty **tty) {
    if (tty == NULL || *tty == NULL || IS_ERR(*tty))
        return;
    lock(&ttys_lock);
    tty_release(*tty);
    unlock(&ttys_lock);
    *tty = NULL;
}

static void expect_cleanups(unsigned expected, const char *context) {
    char message[160];
    snprintf(message, sizeof(message), "%s 的 cleanup 次数正确", context);
    EXPECT(atomic_load_explicit(&cleanup_calls, memory_order_relaxed) ==
            expected, message);
    snprintf(message, sizeof(message), "%s 的 cleanup 不持有 tty 锁",
            context);
    EXPECT(atomic_load_explicit(&cleanup_with_tty_lock,
            memory_order_relaxed) == 0, message);
    snprintf(message, sizeof(message), "%s 的 cleanup 仍持有全局表锁",
            context);
    EXPECT(atomic_load_explicit(&cleanup_without_ttys_lock,
            memory_order_relaxed) == 0, message);
    snprintf(message, sizeof(message), "%s 在 cleanup 前已撤销发布",
            context);
    EXPECT(atomic_load_explicit(&cleanup_while_published,
            memory_order_relaxed) == 0, message);
}

static struct task make_credential_task(uid_t_ uid, uid_t_ gid) {
    return (struct task) {
        .uid = uid,
        .gid = gid,
        .euid = uid,
        .egid = gid,
    };
}

static void test_failed_init_rollback(void) {
    atomic_store_explicit(&failed_init_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&failed_cleanup_calls, 0, memory_order_relaxed);
    reset_driver_observations();

    struct task task = make_credential_task(1101, 1201);
    current = &task;
    struct tty_driver failing_driver = {.ops = &failed_ops};
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        struct tty *tty = pty_open_fake(&failing_driver);
        EXPECT(IS_ERR(tty) && PTR_ERR(tty) == _EIO,
                "driver init 失败返回原始错误");

        lock(&ttys_lock);
        EXPECT(failing_driver.ttys != NULL &&
                failing_driver.ttys[0] == NULL,
                "driver init 失败不发布槽位");
        EXPECT(!failing_driver.reserved[0],
                "driver init 失败撤销 bitmap 预留");
        unlock(&ttys_lock);
    }
    EXPECT(atomic_load_explicit(&failed_init_calls,
            memory_order_relaxed) == 16,
            "每次失败都重新执行 driver init");
    EXPECT(atomic_load_explicit(&failed_cleanup_calls,
            memory_order_relaxed) == 0,
            "未发布候选不调用成功态 cleanup");

    struct tty_driver succeeding_driver = {.ops = &tracked_ops};
    struct tty *tty = pty_open_fake(&succeeding_driver);
    EXPECT(!IS_ERR(tty) && tty->num == 0,
            "失败回滚后首个成功分配复用 0 号槽");
    EXPECT(succeeding_driver.ttys == failing_driver.ttys,
            "不同 fake driver 绑定同一公开从端表");
    release_tty(&tty);
    expect_cleanups(1, "失败回滚后的成功对象");
    current = NULL;
}

struct publication_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool entered;
    bool allow_publish;
    bool worker_done;
    bool reservation_seen;
    bool slot_hidden;
    int pty_num;
};

static struct publication_gate *active_publication_gate;

static int gated_init(struct tty *tty) {
    atomic_fetch_add_explicit(&init_calls, 1, memory_order_relaxed);
    struct publication_gate *gate = active_publication_gate;

    pthread_mutex_lock(&gate->mutex);
    gate->pty_num = tty->num;
    gate->reservation_seen = tty->driver->reserved[tty->num];
    gate->slot_hidden = tty->driver->ttys[tty->num] == NULL;

    // 模拟 iOS driver 初始化期间暂时交出全局表锁。
    unlock(&ttys_lock);
    gate->entered = true;
    pthread_cond_broadcast(&gate->changed);
    while (!gate->allow_publish)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
    lock(&ttys_lock);
    return 0;
}

static const struct tty_driver_ops gated_ops = {
    .init = gated_init,
    .cleanup = tracked_cleanup,
};

struct publication_worker {
    struct publication_gate *gate;
    struct tty_driver *driver;
    struct task task;
    struct tty *tty;
};

static void *publication_worker_main(void *opaque) {
    struct publication_worker *worker = opaque;
    current = &worker->task;
    worker->tty = pty_open_fake(worker->driver);
    current = NULL;

    pthread_mutex_lock(&worker->gate->mutex);
    worker->gate->worker_done = true;
    pthread_cond_broadcast(&worker->gate->changed);
    pthread_mutex_unlock(&worker->gate->mutex);
    return NULL;
}

static void test_reserve_then_publish(void) {
    reset_driver_observations();
    struct publication_gate gate = {
        .pty_num = -1,
    };
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化发布闸门 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化发布闸门 condition");

    struct tty_driver driver = {.ops = &gated_ops};
    struct publication_worker worker = {
        .gate = &gate,
        .driver = &driver,
        .task = make_credential_task(2101, 2201),
    };
    active_publication_gate = &gate;

    pthread_t thread;
    int create_error = pthread_create(
            &thread, NULL, publication_worker_main, &worker);
    EXPECT(create_error == 0, "建立受控 PTY 初始化线程");
    if (create_error != 0)
        goto destroy_gate;

    pthread_mutex_lock(&gate.mutex);
    while (!gate.entered && !gate.worker_done)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    bool entered = gate.entered;
    int pty_num = gate.pty_num;
    bool reservation_seen = gate.reservation_seen;
    bool slot_hidden = gate.slot_hidden;
    pthread_mutex_unlock(&gate.mutex);

    EXPECT(entered, "driver init 进入受控未发布窗口");
    if (entered) {
        EXPECT(pty_num == 0, "受控初始化预留最低槽位");
        EXPECT(reservation_seen, "driver init 期间 bitmap 保持预留");
        EXPECT(slot_hidden, "driver init 期间公开表保持 NULL");

        lock(&ttys_lock);
        EXPECT(driver.reserved[pty_num],
                "并发观察者可确认槽位仍处于预留态");
        EXPECT(driver.ttys[pty_num] == NULL,
                "并发观察者看不到未完成 tty");
        unlock(&ttys_lock);

        struct statbuf stat = {};
        EXPECT(devptsfs.stat(NULL, "/0", &stat) == _ENOENT,
                "devpts stat 不暴露预留槽位");
        struct fd *hidden = devptsfs.open(NULL, "/0", 0, 0);
        EXPECT(IS_ERR(hidden) && PTR_ERR(hidden) == _ENOENT,
                "devpts open 不暴露预留槽位");

        struct fd *directory = devptsfs.open(NULL, "", 0, 0);
        EXPECT(directory != NULL && !IS_ERR(directory),
                "打开 devpts 根目录");
        if (directory != NULL && !IS_ERR(directory)) {
            struct dir_entry entry = {};
            EXPECT(directory->ops->readdir(directory, &entry) == 0,
                    "devpts readdir 跳过预留槽位");
            fd_close(directory);
        }
    }

    pthread_mutex_lock(&gate.mutex);
    gate.allow_publish = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);
    EXPECT(pthread_join(thread, NULL) == 0,
            "等待受控 PTY 初始化线程完成");

    EXPECT(worker.tty != NULL && !IS_ERR(worker.tty),
            "受控初始化最终成功发布 tty");
    if (worker.tty != NULL && !IS_ERR(worker.tty)) {
        struct statbuf stat = {};
        EXPECT(devptsfs.stat(NULL, "/0", &stat) == 0,
                "发布后 devpts 一次性看到 tty");
        EXPECT((stat.mode & S_IFMT) == S_IFCHR &&
                (stat.mode & 0777) == 0620,
                "发布后的 devpts 类型与权限完整");
        EXPECT(stat.uid == 2101 && stat.gid == 2201,
                "发布后的 devpts 凭据完整");
        EXPECT(dev_major(stat.rdev) == TTY_PSEUDO_SLAVE_MAJOR &&
                dev_minor(stat.rdev) == 0,
                "发布后的 devpts 设备号完整");
        lock(&ttys_lock);
        EXPECT(!driver.reserved[0],
                "成功发布同时清除 bitmap 预留");
        EXPECT(driver.ttys[0] == worker.tty,
                "成功发布只留下最终 tty 指针");
        unlock(&ttys_lock);
    }
    release_tty(&worker.tty);
    expect_cleanups(1, "一次发布对象");

destroy_gate:
    active_publication_gate = NULL;
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

static void expect_stat_error(const char *path, const char *message) {
    struct statbuf stat = {};
    EXPECT(devptsfs.stat(NULL, path, &stat) == _ENOENT, message);
}

static void test_devpts_bounded_paths(void) {
    reset_driver_observations();
    struct task task = make_credential_task(3101, 3201);
    current = &task;
    struct tty_driver driver = {.ops = &tracked_ops};
    struct tty *tty = pty_open_fake(&driver);
    EXPECT(tty != NULL && !IS_ERR(tty) && tty->num == 0,
            "为 devpts 路径测试建立 0 号 tty");
    if (tty == NULL || IS_ERR(tty)) {
        current = NULL;
        return;
    }

    struct statbuf stat = {};
    EXPECT(devptsfs.stat(NULL, "", &stat) == 0 &&
            (stat.mode & S_IFMT) == S_IFDIR,
            "devpts 空路径表示根目录");
    EXPECT(devptsfs.stat(NULL, "/0", &stat) == 0,
            "devpts 接受有效 0 号路径");
    EXPECT(devptsfs.stat(NULL, "/0000", &stat) == 0,
            "devpts 有界解析接受前导零");
    EXPECT(stat.uid == 3101 && stat.gid == 3201,
            "devpts stat 返回创建线程凭据");

    expect_stat_error("/1", "devpts 拒绝未发布的合法槽号");
    expect_stat_error("/4095", "devpts 安全查询最高合法槽号");
    expect_stat_error("/4096", "devpts 拒绝首个越界槽号");
    expect_stat_error("/9999999999999999999999999999999999999999",
            "devpts 拒绝超长十进制且不溢出");
    expect_stat_error("/-1", "devpts 拒绝负号");
    expect_stat_error("/+0", "devpts 拒绝正号");
    expect_stat_error("/0/1", "devpts 拒绝多个路径分量");
    expect_stat_error("/", "devpts 拒绝空数字分量");
    expect_stat_error("0", "devpts 拒绝缺少前导斜杠");
    const char non_ascii_path[] = {'/', (char) 0xff, '\0'};
    expect_stat_error(non_ascii_path,
            "devpts 把高位字节作为普通非法字符处理");

    struct fd *invalid = devptsfs.open(NULL, "/4096", 0, 0);
    EXPECT(IS_ERR(invalid) && PTR_ERR(invalid) == _ENOENT,
            "devpts open 对越界槽返回 ENOENT");

    struct fd *opened = devptsfs.open(NULL, "/0", 0, 0);
    EXPECT(opened != NULL && !IS_ERR(opened),
            "devpts open 接受已发布槽位");
    if (opened != NULL && !IS_ERR(opened)) {
        memset(&stat, 0, sizeof(stat));
        EXPECT(devptsfs.fstat(opened, &stat) == 0 &&
                stat.uid == 3101 && stat.gid == 3201,
                "devpts fstat 查询已发布槽位");
        EXPECT(devptsfs.fsetattr(opened, make_attr(mode, 0600)) == 0,
                "devpts fsetattr 传播成功结果");
        EXPECT(devptsfs.fstat(opened, &stat) == 0 &&
                (stat.mode & 0777) == 0600,
                "devpts fsetattr 修改可被随后查询观察");
        EXPECT(devptsfs.fsetattr(opened, make_attr(size, 1)) == _EINVAL,
                "devpts fsetattr 传播非法 size 错误");

        release_tty(&tty);
        EXPECT(devptsfs.fstat(opened, &stat) == _ENOENT,
                "槽位释放后旧 devpts fd 安全返回 ENOENT");
        fd_close(opened);
    } else {
        release_tty(&tty);
    }
    expect_stat_error("/0", "最终释放后 devpts 路径消失");

    struct fd *root = devptsfs.open(NULL, "", 0, 0);
    EXPECT(root != NULL && !IS_ERR(root), "重新打开 devpts 根目录");
    if (root != NULL && !IS_ERR(root)) {
        EXPECT(devptsfs.fsetattr(root, make_attr(mode, 0700)) == _EROFS,
                "devpts 根目录 fsetattr 传播只读错误");
        fd_close(root);
    }
    EXPECT(devptsfs.setattr(NULL, "", make_attr(mode, 0700)) == _EROFS,
            "devpts 根目录 setattr 传播只读错误");
    expect_cleanups(1, "devpts 路径对象");
    current = NULL;
}

struct start_gate {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned ready;
    bool go;
};

struct concurrent_worker {
    struct start_gate *gate;
    struct tty_driver *driver;
    struct task task;
    struct tty *tty;
    bool started;
};

static void *concurrent_worker_main(void *opaque) {
    struct concurrent_worker *worker = opaque;
    pthread_mutex_lock(&worker->gate->mutex);
    worker->gate->ready++;
    pthread_cond_broadcast(&worker->gate->changed);
    while (!worker->gate->go)
        pthread_cond_wait(&worker->gate->changed, &worker->gate->mutex);
    pthread_mutex_unlock(&worker->gate->mutex);

    current = &worker->task;
    worker->tty = pty_open_fake(worker->driver);
    current = NULL;
    return NULL;
}

static void test_concurrent_driver_binding(void) {
    reset_driver_observations();
    struct tty_driver driver = {.ops = &tracked_ops};
    struct start_gate gate = {};
    EXPECT(pthread_mutex_init(&gate.mutex, NULL) == 0,
            "初始化并发启动 mutex");
    EXPECT(pthread_cond_init(&gate.changed, NULL) == 0,
            "初始化并发启动 condition");

    struct concurrent_worker workers[CONCURRENT_OPENERS] = {};
    pthread_t threads[CONCURRENT_OPENERS];
    unsigned created = 0;
    for (unsigned index = 0; index < CONCURRENT_OPENERS; index++) {
        workers[index] = (struct concurrent_worker) {
            .gate = &gate,
            .driver = &driver,
            .task = make_credential_task(4000 + index, 5000 + index),
        };
        int error = pthread_create(&threads[index], NULL,
                concurrent_worker_main, &workers[index]);
        EXPECT(error == 0, "建立并发 fake PTY 打开线程");
        if (error == 0) {
            workers[index].started = true;
            created++;
        }
    }

    pthread_mutex_lock(&gate.mutex);
    while (gate.ready < created)
        pthread_cond_wait(&gate.changed, &gate.mutex);
    gate.go = true;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.mutex);

    for (unsigned index = 0; index < CONCURRENT_OPENERS; index++) {
        if (workers[index].started)
            EXPECT(pthread_join(threads[index], NULL) == 0,
                    "等待并发 fake PTY 打开线程");
    }

    bool seen[TEST_MAX_PTYS] = {};
    unsigned successes = 0;
    lock(&ttys_lock);
    for (unsigned index = 0; index < CONCURRENT_OPENERS; index++) {
        if (!workers[index].started)
            continue;
        struct tty *tty = workers[index].tty;
        EXPECT(tty != NULL && !IS_ERR(tty),
                "并发 fake PTY 打开成功");
        if (tty == NULL || IS_ERR(tty))
            continue;
        successes++;
        EXPECT(tty->num >= 0 && tty->num < TEST_MAX_PTYS,
                "并发分配槽号保持在表内");
        if (tty->num >= 0 && tty->num < TEST_MAX_PTYS) {
            EXPECT(!seen[tty->num], "并发分配不会重复发布同一槽位");
            seen[tty->num] = true;
            EXPECT(driver.ttys[tty->num] == tty,
                    "并发分配的公开表指向最终对象");
            EXPECT(!driver.reserved[tty->num],
                    "并发发布后对应 bitmap 已清除");
        }
        lock(&tty->lock);
        EXPECT(tty->pty.uid == 4000 + index &&
                tty->pty.gid == 5000 + index,
                "每线程 TLS current 决定各自 PTY 凭据");
        unlock(&tty->lock);
    }
    EXPECT(driver.ttys != NULL && driver.limit == TEST_MAX_PTYS &&
            driver.major == TTY_PSEUDO_SLAVE_MAJOR,
            "并发首次绑定得到一份稳定 driver 配置");
    for (unsigned index = 0; index < CONCURRENT_OPENERS; index++) {
        struct tty *tty = workers[index].tty;
        if (tty != NULL && !IS_ERR(tty)) {
            tty_release(tty);
            workers[index].tty = NULL;
        }
    }
    unlock(&ttys_lock);

    EXPECT(successes == created, "所有已启动线程均成功持有唯一 PTY");
    EXPECT(atomic_load_explicit(&init_calls, memory_order_relaxed) ==
            successes, "并发打开每个新槽只初始化一次");
    expect_cleanups(successes, "并发 driver 绑定对象");
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.mutex);
}

struct blocked_pty_write {
    struct task task;
    struct sighand sighand;
    struct fd *fd;
    int result;
};

static void *blocked_pty_write_main(void *opaque) {
    struct blocked_pty_write *write = opaque;
    current = &write->task;
    write->result = write->fd->ops->write(write->fd, "x", 1);
    current = NULL;
    return NULL;
}

static void test_ptmx_pair_close_wakes_blocked_write(void) {
    struct task task = make_credential_task(5101, 5201);
    current = &task;

    struct tty *bypass = tty_get(
            &pty_master, TTY_PSEUDO_MASTER_MAJOR, 0);
    EXPECT(IS_ERR(bypass) && PTR_ERR(bypass) == _ENXIO,
            "空 master 槽只能由 ptmx 事务创建");

    struct fd *master_fd = fd_create(NULL);
    EXPECT(master_fd != NULL, "为 ptmx 创建 fd");
    if (master_fd == NULL)
        goto out_current;
    master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "通过真实 ptmx 路径创建主从端");
    if (err < 0) {
        fd_close(master_fd);
        goto out_current;
    }

    struct tty *master = master_fd->tty;
    int pty_num = master->num;
    dword_t unlocked = 0;
    EXPECT(master_fd->ops->ioctl(
            master_fd, TIOCSPTLCK_, &unlocked) == 0,
            "通过 master ioctl 解锁从端");

    struct fd *slave_fd = fd_create(NULL);
    EXPECT(slave_fd != NULL, "为真实 PTY 从端创建 fd");
    if (slave_fd == NULL) {
        fd_close(master_fd);
        goto out_current;
    }
    slave_fd->flags = O_RDWR_ | O_NOCTTY_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
            DEV_CHAR, slave_fd);
    EXPECT(err == 0, "通过字符设备路径打开真实 PTY 从端");
    if (err < 0) {
        fd_close(slave_fd);
        fd_close(master_fd);
        goto out_current;
    }
    struct tty *slave = slave_fd->tty;

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == master &&
            pty_slave.ttys[pty_num] == slave,
            "ptmx 在同一槽原子发布完整主从端");
    EXPECT(!pty_master.reserved[pty_num] &&
            !pty_slave.reserved[pty_num],
            "ptmx 发布后不遗留 reservation");
    unlock(&ttys_lock);

    char full_buffer[sizeof(master->buf)];
    memset(full_buffer, 'a', sizeof(full_buffer));
    int written = (int) slave_fd->ops->write(
            slave_fd, full_buffer, sizeof(full_buffer));
    EXPECT(written == (int) sizeof(full_buffer),
            "从端可填满 master 输入缓冲区");
    if (written != (int) sizeof(full_buffer)) {
        fd_close(slave_fd);
        fd_close(master_fd);
        goto out_current;
    }

    struct blocked_pty_write blocked = {
        .fd = slave_fd,
        .result = 1,
    };
    blocked.task.sighand = &blocked.sighand;
    lock_init(&blocked.sighand.lock);
    lock_init(&blocked.task.waiting_cond_lock);

    pthread_t writer;
    int writer_error = pthread_create(
            &writer, NULL, blocked_pty_write_main, &blocked);
    EXPECT(writer_error == 0, "建立阻塞从端写线程");
    if (writer_error != 0) {
        lock_destroy(&blocked.task.waiting_cond_lock);
        lock_destroy(&blocked.sighand.lock);
        fd_close(slave_fd);
        fd_close(master_fd);
        goto out_current;
    }

    // 若等待登记或关闭路径发生回归，定向测试必须有界失败而不是拖住整个门禁。
    signal(SIGALRM, SIG_DFL);
    alarm(5);
    bool waiting = false;
    while (!waiting) {
        lock(&blocked.task.waiting_cond_lock);
        waiting = blocked.task.waiting_cond == &master->consumed &&
                blocked.task.waiting_lock == &master->lock;
        unlock(&blocked.task.waiting_cond_lock);
        if (!waiting)
            sched_yield();
    }
    EXPECT(waiting, "从端写精确阻塞在 master consumed 条件上");

    int poll_events = master_fd->ops->poll(master_fd);
    EXPECT((poll_events & POLL_READ) != 0,
            "从端阻塞写不占用 slave 锁，master poll 仍可报告可读");
    lock(&ttys_lock);
    lock(&master->lock);
    EXPECT(master->refcount == 2,
            "阻塞从端写用临时引用固定 master 生命周期");
    unlock(&master->lock);
    unlock(&ttys_lock);

    EXPECT(fd_close(master_fd) == 0,
            "master 最终关闭唤醒阻塞写且完成 cleanup");

    EXPECT(pthread_join(writer, NULL) == 0,
            "master 关闭后阻塞写线程有界退出");
    alarm(0);
    EXPECT(blocked.result == _EIO,
            "已挂断 master 使阻塞从端写返回 EIO");
    lock_destroy(&blocked.task.waiting_cond_lock);
    lock_destroy(&blocked.sighand.lock);

    char devpts_path[32];
    snprintf(devpts_path, sizeof(devpts_path), "/%d", pty_num);
    struct statbuf hidden_stat = {};
    EXPECT(devptsfs.stat(NULL, devpts_path, &hidden_stat) == _ENOENT,
            "master 最后文件关闭后立即隐藏 devpts 节点");

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == NULL &&
            pty_slave.ttys[pty_num] == slave,
            "master 关闭后仅保留从端 fd 引用");
    EXPECT(slave->pty.other == NULL,
            "master cleanup 在释放后断开从端反向指针");
    unlock(&ttys_lock);
    EXPECT(fd_close(slave_fd) == 0, "关闭真实 PTY 从端 fd");

    lock(&ttys_lock);
    EXPECT(pty_master.ttys[pty_num] == NULL &&
            pty_slave.ttys[pty_num] == NULL &&
            !pty_master.reserved[pty_num] &&
            !pty_slave.reserved[pty_num],
            "真实主从端最终释放后槽位完整清空");
    unlock(&ttys_lock);

out_current:
    current = NULL;
}

static void test_ptmx_slave_close_preserves_master_write(void) {
    struct task task = make_credential_task(5301, 5401);
    struct tgroup group = {.pgid = 5301};
    lock_init(&group.lock);
    task.group = &group;
    current = &task;

    struct fd *master_fd = NULL;
    struct fd *slave_fd = NULL;
    struct fd *reopened_fd = NULL;
    struct fd *queued_fd = NULL;
    struct tty *master = NULL;
    struct tty *slave = NULL;
    int pty_num = -1;
    unsigned extra_slave_refs = 0;
    bool writer_started = false;
    bool writer_joined = false;
    bool writer_sync_initialized = false;
    pthread_t writer = {};
    struct blocked_pty_write blocked = {};

    master_fd = fd_create(NULL);
    EXPECT(master_fd != NULL, "为反向关闭测试创建 ptmx fd");
    if (master_fd == NULL)
        goto cleanup;
    master_fd->flags = O_RDWR_ | O_NOCTTY_;
    int err = dev_open(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR,
            DEV_CHAR, master_fd);
    EXPECT(err == 0, "为反向关闭测试创建真实主从端");
    if (err < 0)
        goto cleanup;

    master = master_fd->tty;
    pty_num = master->num;
    dword_t unlocked = 0;
    EXPECT(master_fd->ops->ioctl(
            master_fd, TIOCSPTLCK_, &unlocked) == 0,
            "为反向关闭测试解锁从端");

    slave_fd = fd_create(NULL);
    EXPECT(slave_fd != NULL, "为反向关闭测试创建从端 fd");
    if (slave_fd == NULL)
        goto cleanup;
    slave_fd->flags = O_RDWR_ | O_NOCTTY_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
            DEV_CHAR, slave_fd);
    EXPECT(err == 0, "为反向关闭测试打开从端");
    if (err < 0)
        goto cleanup;
    slave = slave_fd->tty;

    struct termios_ raw;
    EXPECT(slave_fd->ops->ioctl(slave_fd, TCGETS_, &raw) == 0,
            "读取反向关闭测试的 slave termios");
    raw.iflags = 0;
    raw.oflags = 0;
    raw.lflags = 0;
    raw.cc[VMIN_] = 1;
    raw.cc[VTIME_] = 0;
    EXPECT(slave_fd->ops->ioctl(slave_fd, TCSETS_, &raw) == 0,
            "把反向关闭测试的 slave 切到逐字节模式");

    char full_buffer[sizeof(slave->buf)];
    memset(full_buffer, 'b', sizeof(full_buffer));
    int written = (int) master_fd->ops->write(
            master_fd, full_buffer, sizeof(full_buffer));
    EXPECT(written == (int) sizeof(full_buffer),
            "主端可填满 slave 输入缓冲区");
    if (written != (int) sizeof(full_buffer))
        goto cleanup;

    blocked.fd = master_fd;
    blocked.result = _EINTR;
    blocked.task.sighand = &blocked.sighand;
    lock_init(&blocked.sighand.lock);
    lock_init(&blocked.task.waiting_cond_lock);
    writer_sync_initialized = true;

    int writer_error = pthread_create(
            &writer, NULL, blocked_pty_write_main, &blocked);
    EXPECT(writer_error == 0, "建立阻塞主端写线程");
    if (writer_error != 0)
        goto cleanup;
    writer_started = true;

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    bool waiting = false;
    while (!waiting) {
        lock(&blocked.task.waiting_cond_lock);
        waiting = blocked.task.waiting_cond == &slave->consumed &&
                blocked.task.waiting_lock == &slave->lock;
        unlock(&blocked.task.waiting_cond_lock);
        if (!waiting)
            sched_yield();
    }
    EXPECT(waiting, "主端写精确阻塞在 slave consumed 条件上");

    // 模拟多个 tgroup 继承 controlling tty；half-close 只能依赖打开文件数。
    lock(&slave->lock);
    slave->refcount += 3;
    unlock(&slave->lock);
    extra_slave_refs = 3;
    EXPECT(fd_close(slave_fd) == 0,
            "slave 最终关闭但不终止阻塞主端写");
    slave_fd = NULL;

    lock(&ttys_lock);
    lock(&slave->lock);
    lock(&master->lock);
    EXPECT(pty_master.ttys[pty_num] == master &&
            pty_slave.ttys[pty_num] == slave,
            "slave 最终关闭后由 master 保留完整主从对象");
    EXPECT(slave->open_count == 0 &&
            !slave->hung_up && !master->hung_up,
            "slave half-close 只由打开文件数表示且保持 master 可写");
    unlock(&master->lock);
    unlock(&slave->lock);
    unlock(&ttys_lock);

    lock(&blocked.task.waiting_cond_lock);
    waiting = blocked.task.waiting_cond == &slave->consumed &&
            blocked.task.waiting_lock == &slave->lock;
    unlock(&blocked.task.waiting_cond_lock);
    EXPECT(waiting,
            "slave 关闭后 master writer 仍等待可重开的输入队列");

    char devpts_path[32];
    snprintf(devpts_path, sizeof(devpts_path), "/%d", pty_num);
    struct statbuf visible_stat = {};
    EXPECT(devptsfs.stat(NULL, devpts_path, &visible_stat) == 0,
            "最后 slave 关闭但 master 存活时保留可重开 devpts 节点");

    reopened_fd = fd_create(NULL);
    EXPECT(reopened_fd != NULL, "为 half-close 重开创建从端 fd");
    if (reopened_fd == NULL)
        goto cleanup;
    reopened_fd->flags = O_RDWR_ | O_NOCTTY_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
            DEV_CHAR, reopened_fd);
    EXPECT(err == 0, "master 存活时可重开同一从端");
    if (err < 0)
        goto cleanup;

    char drained[sizeof(full_buffer)];
    ssize_t drained_size = reopened_fd->ops->read(
            reopened_fd, drained, sizeof(drained));
    EXPECT(drained_size == (ssize_t) sizeof(drained),
            "重开 slave 后读取旧队列并为阻塞 writer 腾出空间");
    if (drained_size != (ssize_t) sizeof(drained))
        goto cleanup;
    EXPECT(pthread_join(writer, NULL) == 0,
            "重开并读取后阻塞 master writer 有界完成");
    writer_joined = true;
    alarm(0);
    EXPECT(blocked.result == 1,
            "slave reopen 与 drain 使旧 master write 成功而非 EIO");

    char observed = 0;
    EXPECT(reopened_fd->ops->read(reopened_fd, &observed, 1) == 1 &&
            observed == 'x', "重开 slave 可读到阻塞 writer 排队的字节");

    EXPECT(fd_close(reopened_fd) == 0,
            "再次关闭 slave 以验证 half-close 后的新写入");
    reopened_fd = NULL;
    EXPECT(master_fd->ops->write(master_fd, "abc", 3) == 3,
            "最后 slave 关闭后 master 仍可立即排队写入");

    queued_fd = fd_create(NULL);
    EXPECT(queued_fd != NULL, "为读取 half-close 排队数据创建从端 fd");
    if (queued_fd == NULL)
        goto cleanup;
    queued_fd->flags = O_RDWR_ | O_NOCTTY_;
    err = dev_open(TTY_PSEUDO_SLAVE_MAJOR, pty_num,
            DEV_CHAR, queued_fd);
    EXPECT(err == 0, "再次重开同一从端读取 half-close 排队数据");
    if (err < 0)
        goto cleanup;

    char queued[3] = {};
    EXPECT(queued_fd->ops->read(queued_fd, queued, sizeof(queued)) == 3 &&
            memcmp(queued, "abc", 3) == 0,
            "重开 slave 后按序读到 master 在 half-close 期间写入的数据");
    observed = 0;
    EXPECT(queued_fd->ops->write(queued_fd, "s", 1) == 1 &&
            master_fd->ops->read(master_fd, &observed, 1) == 1 &&
            observed == 's', "重开后 slave 到 master 可真实读写");

cleanup:
    if (writer_started && !writer_joined && master_fd != NULL) {
        // 失败清理必须先永久关闭 master，唤醒仍等待 slave->consumed 的 writer。
        fd_close(master_fd);
        master_fd = NULL;
    }
    if (writer_started && !writer_joined) {
        pthread_join(writer, NULL);
        writer_joined = true;
    }
    alarm(0);
    if (queued_fd != NULL)
        fd_close(queued_fd);
    if (reopened_fd != NULL)
        fd_close(reopened_fd);
    if (slave_fd != NULL)
        fd_close(slave_fd);
    if (master_fd != NULL)
        fd_close(master_fd);
    if (extra_slave_refs != 0) {
        lock(&ttys_lock);
        for (unsigned ref = 0; ref < extra_slave_refs; ref++)
            tty_release(slave);
        unlock(&ttys_lock);
    }
    if (writer_sync_initialized) {
        lock_destroy(&blocked.task.waiting_cond_lock);
        lock_destroy(&blocked.sighand.lock);
    }

    if (pty_num >= 0) {
        lock(&ttys_lock);
        EXPECT(pty_master.ttys[pty_num] == NULL &&
                pty_slave.ttys[pty_num] == NULL &&
                !pty_master.reserved[pty_num] &&
                !pty_slave.reserved[pty_num],
                "反向关闭测试最终完整清理槽位");
        unlock(&ttys_lock);
    }

    current = NULL;
    lock_destroy(&group.lock);
}

static void test_full_table_and_reuse(void) {
    reset_driver_observations();
    struct task task = make_credential_task(6101, 6201);
    current = &task;
    struct tty_driver driver = {.ops = &tracked_ops};
    struct tty **ttys = calloc(TEST_MAX_PTYS, sizeof(*ttys));
    EXPECT(ttys != NULL, "分配满槽测试指针表");
    if (ttys == NULL) {
        current = NULL;
        return;
    }

    unsigned allocated = 0;
    for (; allocated < TEST_MAX_PTYS; allocated++) {
        struct tty *tty = pty_open_fake(&driver);
        if (tty == NULL || IS_ERR(tty)) {
            EXPECT(false, "4096 个合法 PTY 槽应全部可分配");
            break;
        }
        ttys[allocated] = tty;
        EXPECT(tty->num == (int) allocated,
                "持有全部前序对象时按最低空槽顺序分配");
    }

    bool replacement_created = false;
    if (allocated == TEST_MAX_PTYS) {
        struct tty *overflow = pty_open_fake(&driver);
        EXPECT(IS_ERR(overflow) && PTR_ERR(overflow) == _ENOSPC,
                "第 4097 次分配返回 ENOSPC 而不越界");

        lock(&ttys_lock);
        bool all_published = true;
        for (unsigned index = 0; index < TEST_MAX_PTYS; index++) {
            if (driver.ttys[index] != ttys[index] ||
                    driver.reserved[index]) {
                all_published = false;
                break;
            }
        }
        unlock(&ttys_lock);
        EXPECT(all_published,
                "满表失败不破坏既有发布对象或留下 reservation");

        unsigned reused_index = TEST_MAX_PTYS / 2;
        release_tty(&ttys[reused_index]);
        struct tty *replacement = pty_open_fake(&driver);
        EXPECT(replacement != NULL && !IS_ERR(replacement) &&
                replacement->num == (int) reused_index,
                "释放中间槽后立即复用最低空槽");
        if (replacement != NULL && !IS_ERR(replacement)) {
            ttys[reused_index] = replacement;
            replacement_created = true;
        }
    }

    lock(&ttys_lock);
    for (unsigned index = 0; index < allocated; index++) {
        if (ttys[index] != NULL && !IS_ERR(ttys[index])) {
            tty_release(ttys[index]);
            ttys[index] = NULL;
        }
    }
    bool all_empty = true;
    for (unsigned index = 0; index < TEST_MAX_PTYS; index++) {
        if (driver.ttys[index] != NULL ||
                driver.reserved[index]) {
            all_empty = false;
            break;
        }
    }
    unlock(&ttys_lock);
    EXPECT(all_empty, "释放满表后公开表与 reservation bitmap 均清空");

    unsigned expected_objects = allocated + replacement_created;
    EXPECT(atomic_load_explicit(&init_calls, memory_order_relaxed) ==
            expected_objects, "满槽与复用只初始化成功创建的对象");
    expect_cleanups(expected_objects, "满槽与复用对象");
    free(ttys);
    current = NULL;
}

int main(void) {
    test_failed_init_rollback();
    test_reserve_then_publish();
    test_devpts_bounded_paths();
    test_concurrent_driver_binding();
    test_ptmx_pair_close_wakes_blocked_write();
    test_ptmx_slave_close_preserves_master_write();
    test_full_table_and_reuse();

    unsigned count = atomic_load_explicit(&failures,
            memory_order_relaxed);
    if (count != 0) {
        fprintf(stderr, "PTY 生命周期测试共发现 %u 项失败\n", count);
        return 1;
    }
    puts("PTY 生命周期测试通过");
    return 0;
}

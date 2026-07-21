#define DEFAULT_CHANNEL debug
#include "debug.h"
#include <string.h>
#include "kernel/calls.h"
#include "fs/poll.h"
#include "fs/tty.h"
#include "fs/devices.h"

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

struct tty_driver *tty_drivers[256] = {
    [TTY_CONSOLE_MAJOR] = NULL, // will be filled in by create_stdio
    [TTY_PSEUDO_MASTER_MAJOR] = &pty_master,
    [TTY_PSEUDO_SLAVE_MAJOR] = &pty_slave,
};

// lock this before locking a tty
lock_t ttys_lock = LOCK_INITIALIZER;

struct tty *tty_alloc(struct tty_driver *driver, int type, int num) {
    struct tty *tty = calloc(1, sizeof(struct tty));
    if (tty == NULL)
        return NULL;

    tty->refcount = 0;
    tty->driver = driver;
    tty->type = type;
    tty->num = num;
    list_init(&tty->fds);

    tty->termios.iflags = ICRNL_ | IXON_;
    tty->termios.oflags = OPOST_ | ONLCR_;
    tty->termios.cflags = 0;
    tty->termios.lflags = ISIG_ | ICANON_ | ECHO_ | ECHOE_ | ECHOK_ | ECHOCTL_ | ECHOKE_ | IEXTEN_;
    // from include/asm-generic/termios.h
    memcpy(tty->termios.cc, "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0\0\0", 19);
    lock_init(&tty->lock);
    lock_init(&tty->fds_lock);
    lock_init(&tty->input_lock);
    cond_init(&tty->produced);
    cond_init(&tty->consumed);
    return tty;
}

void tty_destroy_unpublished(struct tty *tty) {
    cond_destroy(&tty->produced);
    cond_destroy(&tty->consumed);
    lock_destroy(&tty->input_lock);
    lock_destroy(&tty->fds_lock);
    lock_destroy(&tty->lock);
    free(tty);
}

// 调用方持有 ttys_lock。
static struct tty *tty_get_locked(
        struct tty_driver *driver, int type, int num) {
    if (num < 0 || (unsigned) num >= driver->limit ||
            driver->ttys == NULL || driver->reserved == NULL)
        return ERR_PTR(_ENXIO);
    if (driver->reserved[num])
        return ERR_PTR(_EAGAIN);
    struct tty *tty = driver->ttys[num];
    // master 只能由 /dev/ptmx 的预留事务创建，普通字符设备不得抢占空槽。
    if (driver == &pty_master && tty == NULL)
        return ERR_PTR(_ENXIO);
    if (tty == NULL) {
        driver->reserved[num] = true;
        tty = tty_alloc(driver, type, num);
        if (tty == NULL) {
            driver->reserved[num] = false;
            return ERR_PTR(_ENOMEM);
        }

        tty->refcount = 1;

        if (driver->ops->init) {
            int err = driver->ops->init(tty);
            if (err < 0) {
                tty_destroy_unpublished(tty);
                driver->reserved[num] = false;
                return ERR_PTR(err);
            }
        }
        assert(driver->ttys[num] == NULL);
        driver->ttys[num] = tty;
        driver->reserved[num] = false;
    } else {
        lock(&tty->lock);
        tty->refcount++;
        unlock(&tty->lock);
    }
    return tty;
}

struct tty *tty_get(struct tty_driver *driver, int type, int num) {
    lock(&ttys_lock);
    struct tty *tty = tty_get_locked(driver, type, num);
    unlock(&ttys_lock);
    return tty;
}

static struct tty *tty_get_for_open(
        struct tty_driver *driver, int type, int num) {
    lock(&ttys_lock);
    struct tty *tty = tty_get_locked(driver, type, num);
    if (!IS_ERR(tty)) {
        lock(&tty->lock);
        tty->open_count++;
        unlock(&tty->lock);
    }
    unlock(&ttys_lock);
    return tty;
}

// 调用方持有 tty 锁；master 路径会切换到 slave 锁，避免 master→slave 嵌套。
static struct tty *tty_lock_slave_side(struct tty *tty) {
    if (tty->driver != &pty_master)
        return tty;
    struct tty *slave = tty->pty.other;
    assert(slave != NULL);
    unlock(&tty->lock);
    lock(&slave->lock);
    return slave;
}

static void tty_unlock_slave_side(struct tty *tty, struct tty *slave) {
    if (slave == tty)
        return;
    // slave 状态快照与 master 上的后续等待必须原子交接，避免 close/reopen
    // 落在两把锁都未持有的窗口并丢失 produced 通知。
    lock(&tty->lock);
    unlock(&slave->lock);
}

static void tty_poll_wakeup(struct tty *tty, int events) {
    unlock(&tty->lock);
    struct fd *fd;
    lock(&tty->fds_lock);
    list_for_each_entry(&tty->fds, fd, tty_other_fds) {
        poll_wakeup(fd, events);
    }
    unlock(&tty->fds_lock);
    lock(&tty->lock);
}

void tty_release(struct tty *tty) {
    assert(lock_owned_by_current(&ttys_lock));
    lock(&tty->lock);
    assert(tty->refcount > 0);
    if (--tty->refcount == 0) {
        assert(tty->open_count == 0);
        struct tty_driver *driver = tty->driver;
        assert(driver->ttys[tty->num] == tty);
        assert(!driver->reserved[tty->num]);
        // 先使阻塞输入退出，再撤销发布；对端 write 不会在满缓冲区永久卡住 cleanup。
        tty->hung_up = true;
        notify(&tty->produced);
        notify(&tty->consumed);
        driver->ttys[tty->num] = NULL;
        // cleanup 可能临时交出全局锁；在它完全退出前禁止复用同一槽。
        driver->reserved[tty->num] = true;
        unlock(&tty->lock);
        if (driver->ops->cleanup)
            driver->ops->cleanup(tty);
        assert(lock_owned_by_current(&ttys_lock));
        assert(driver->reserved[tty->num]);
        driver->reserved[tty->num] = false;
        lock(&tty->fds_lock);
        assert(list_empty(&tty->fds));
        unlock(&tty->fds_lock);
        tty_destroy_unpublished(tty);
    } else {
        unlock(&tty->lock);
    }
}

// must call with tty lock
static void tty_set_controlling(struct tgroup *group, struct tty *tty) {
    lock(&group->lock);
    if (group->tty == NULL) {
        tty->refcount++;
        group->tty = tty;
        tty->session = group->sid;
        tty->fg_group = group->pgid;
    }
    unlock(&group->lock);
}

// by default, /dev/console is /dev/tty1
int console_major = TTY_CONSOLE_MAJOR;
int console_minor = 1;

int tty_open(struct tty *tty, struct fd *fd) {
    lock(&tty->lock);
    assert(tty->open_count > 0);
    tty->ever_opened = true;
    unlock(&tty->lock);
    fd->tty = tty;

    lock(&tty->fds_lock);
    list_add(&tty->fds, &fd->tty_other_fds);
    unlock(&tty->fds_lock);

    // Linux 不会因 open(/dev/ptmx) 自动把 master 设为 controlling tty；
    // session leader 仍可随后用 TIOCSCTTY 显式选择它。
    if (!(fd->flags & O_NOCTTY_) && tty->driver != &pty_master) {
        // Make this our controlling terminal if:
        // - the terminal doesn't already have a session
        // - we're a session leader
        lock(&pids_lock);
        lock(&tty->lock);
        if (tty->session == 0 && current->group->sid ==
                current->group->leader->pid)
            tty_set_controlling(current->group, tty);
        unlock(&tty->lock);
        unlock(&pids_lock);
    }

    return 0;
}

static int tty_device_open(int major, int minor, struct fd *fd) {
    struct tty *tty;
    if (major == TTY_ALTERNATE_MAJOR) {
        if (minor == DEV_TTY_MINOR) {
            lock(&ttys_lock);
            lock(&current->group->lock);
            tty = current->group->tty;
            unlock(&current->group->lock);
            if (tty != NULL) {
                // PTY master 只有 /dev/ptmx 能创建成功文件；不要让必失败 open 污染在途计数。
                if (tty->driver == &pty_master) {
                    unlock(&ttys_lock);
                    return _EIO;
                }
                lock(&tty->lock);
                tty->refcount++;
                tty->open_count++;
                unlock(&tty->lock);
            }
            unlock(&ttys_lock);
            if (tty == NULL)
                return _ENXIO;
        } else if (minor == DEV_CONSOLE_MINOR) {
            return tty_device_open(console_major, console_minor, fd);
        } else if (minor == DEV_PTMX_MINOR) {
            return ptmx_open(fd);
        } else {
            return _ENXIO;
        }
    } else {
        struct tty_driver *driver = tty_drivers[major];
        assert(driver != NULL);
        if (driver == &pty_master)
            return _EIO;
        tty = tty_get_for_open(driver, major, minor);
        if (IS_ERR(tty))
            return PTR_ERR(tty);
    }

    if (tty->driver->ops->open) {
        int err = tty->driver->ops->open(tty);
        if (err < 0) {
            lock(&ttys_lock);
            lock(&tty->lock);
            assert(tty->open_count > 0);
            tty->open_count--;
            bool lost_last_open = tty->open_count == 0 &&
                    tty->ever_opened;
            unlock(&tty->lock);
            if (lost_last_open && tty->driver->ops->close)
                tty->driver->ops->close(tty);
            tty_release(tty);
            unlock(&ttys_lock);
            return err;
        }
    }

    return tty_open(tty, fd);
}

static int tty_close(struct fd *fd) {
    if (fd->tty != NULL) {
        struct tty *tty = fd->tty;
        lock(&tty->fds_lock);
        list_remove_safe(&fd->tty_other_fds);
        unlock(&tty->fds_lock);
        lock(&ttys_lock);
        lock(&tty->lock);
        assert(tty->open_count > 0);
        tty->open_count--;
        unlock(&tty->lock);
        if (tty->driver->ops->close)
            tty->driver->ops->close(tty);
        tty_release(tty);
        unlock(&ttys_lock);
    }
    return 0;
}

static void tty_input_wakeup(struct tty *tty) {
    notify(&tty->produced);
    tty_poll_wakeup(tty, POLL_READ);
}

static int tty_push_char(struct tty *tty, char ch, bool flag, int blocking) {
    while (tty->bufsize >= sizeof(tty->buf)) {
        if (tty->hung_up)
            return _EIO;
        if (!blocking)
            return _EAGAIN;
        if (wait_for(&tty->consumed, &tty->lock, NULL))
            return _EINTR;
    }
    if (tty->hung_up)
        return _EIO;
    tty->buf[tty->bufsize] = ch;
    tty->buf_flag[tty->bufsize++] = flag;
    return 0;
}

static void tty_echo(struct tty *tty, const char *data, size_t size) {
    // 宿主输出可能永久阻塞；driver write 期间不能阻挡 hangup/cleanup 取得 tty 锁。
    assert(lock_owned_by_current(&tty->lock));
    unlock(&tty->lock);
    tty->driver->ops->write(tty, data, size, false);
    lock(&tty->lock);
}

static bool tty_send_input_signal(struct tty *tty, char ch,
        dword_t lflags, const unsigned char *cc,
        pid_t_ signal_group, sigset_t_ *queue) {
    if (!(lflags & ISIG_))
        return false;
    int sig;
    if (ch == '\0')
        return false; // '\0' is used to disable cc entries
    else if (ch == cc[VINTR_])
        sig = SIGINT_;
    else if (ch == cc[VQUIT_])
        sig = SIGQUIT_;
    else if (ch == cc[VSUSP_])
        sig = SIGTSTP_;
    else
        return false;

    if (signal_group != 0) {
        if (!(lflags & NOFLSH_))
            tty->bufsize = 0;
        sigset_add(queue, sig);
    }
    return true;
}

ssize_t tty_input(struct tty *tty, const char *input, size_t size, bool blocking) {
    int err = 0;
    size_t done_size = 0;
    sigset_t_ queue = 0; // to prevent having to lock tty->lock and pids_lock at the same time

    // 回显会临时释放 tty 锁；独立事务锁防止另一输入在半条行规操作中插入。
    lock(&tty->input_lock);
    lock(&tty->lock);
    if (tty->hung_up) {
        unlock(&tty->lock);
        unlock(&tty->input_lock);
        return _EIO;
    }
    dword_t lflags = tty->termios.lflags;
    dword_t iflags = tty->termios.iflags;
    unsigned char cc[sizeof(tty->termios.cc)];
    memcpy(cc, tty->termios.cc, sizeof(cc));
    pid_t_ signal_group = tty->fg_group;

#define SHOULD_ECHOCTL(ch) \
    (lflags & ECHOCTL_ && \
     ((0 <= ch && ch < ' ') || ch == '\x7f') && \
     !(ch == '\t' || ch == '\n' || ch == cc[VSTART_] || ch == cc[VSTOP_]))

    if (lflags & ICANON_) {
        for (size_t i = 0; i < size; i++) {
            if (tty->hung_up) {
                err = _EIO;
                break;
            }
            done_size++;
            char ch = input[i];
            bool echo = lflags & ECHO_;

            if (iflags & INLCR_ && ch == '\n')
                ch = '\r';
            else if (iflags & ICRNL_ && ch == '\r')
                ch = '\n';
            if (iflags & IGNCR_ && ch == '\r')
                continue;

            if (ch == '\0') {
                // '\0' is used to disable cc entries
                goto no_special;
            } else if (ch == cc[VERASE_] || ch == cc[VKILL_]) {
                // FIXME ECHOE and ECHOK are supposed to enable these
                // ECHOKE enables erasing the line instead of echoing the kill char and outputting a newline
                echo = lflags & ECHOK_;
                int count = tty->bufsize;
                if (ch == cc[VERASE_] && tty->bufsize > 0) {
                    echo = lflags & ECHOE_;
                    count = 1;
                }
                if (!(lflags & ECHO_))
                    echo = false;
                for (int i = 0; i < count; i++) {
                    // don't delete past a flag
                    if (tty->bufsize == 0 ||
                            tty->buf_flag[tty->bufsize - 1])
                        break;
                    tty->bufsize--;
                    char erased = tty->buf[tty->bufsize];
                    if (echo) {
                        tty_echo(tty, "\b \b", 3);
                        if (SHOULD_ECHOCTL(erased))
                            tty_echo(tty, "\b \b", 3);
                    }
                }
                echo = false;
            } else if (ch == cc[VEOF_]) {
                ch = '\0';
                goto canon_wake;
            } else if (ch == '\n' || ch == cc[VEOL_]) {
                // echo it now, before the read call goes through
                if (echo)
                    tty_echo(tty, "\r\n", 2);
canon_wake:
                err = tty_push_char(tty, ch, /*flag*/true, blocking);
                if (err < 0) {
                    done_size--;
                    break;
                }
                echo = false;
                tty_input_wakeup(tty);
            } else {
                if (!tty_send_input_signal(tty, ch,
                        lflags, cc, signal_group, &queue)) {
no_special:
                    err = tty_push_char(tty, ch, /*flag*/false, blocking);
                    if (err < 0) {
                        done_size--;
                        break;
                    }
                }
            }

            if (echo) {
                if (SHOULD_ECHOCTL(ch)) {
                    tty_echo(tty, "^", 1);
                    ch ^= '\100';
                }
                tty_echo(tty, &ch, 1);
            }
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            done_size++;
            if (tty_send_input_signal(tty, input[i],
                    lflags, cc, signal_group, &queue))
                continue;
            while (tty->bufsize >= sizeof(tty->buf)) {
                if (tty->hung_up) {
                    err = _EIO;
                    break;
                }
                err = _EAGAIN;
                if (!blocking)
                    break;
                err = wait_for(&tty->consumed, &tty->lock, NULL);
                if (err < 0)
                    break;
            }
            if (err < 0) {
                done_size--;
                break;
            }
            assert(tty->bufsize < sizeof(tty->buf));
            tty->buf[tty->bufsize++] = input[i];
        }
        if (tty->bufsize > 0)
            tty_input_wakeup(tty);
    }

    assert(tty->bufsize <= sizeof(tty->buf));
    unlock(&tty->lock);

    if (signal_group != 0) {
        for (int sig = 1; sig <= NUM_SIGS; sig++) {
            if (sigset_has(queue, sig))
                send_group_signal(signal_group, sig, SIGINFO_NIL);
        }
    }

    unlock(&tty->input_lock);
    return done_size > 0 ? (ssize_t) done_size : err;
}

// expects bufsize <= tty->bufsize
static void tty_read_into_buf(struct tty *tty, void *buf, size_t bufsize) {
    assert(bufsize <= tty->bufsize);
    memcpy(buf, tty->buf, bufsize);
    tty->bufsize -= bufsize;
    memmove(tty->buf, tty->buf + bufsize, tty->bufsize); // magic!
    memmove(tty->buf_flag, tty->buf_flag + bufsize, tty->bufsize);
    notify(&tty->consumed);
}

static size_t tty_canon_size(struct tty *tty) {
    bool *flag_ptr = memchr(tty->buf_flag, true, tty->bufsize);
    if (flag_ptr == NULL)
        return -1;
    return flag_ptr - tty->buf_flag + 1;
}

static bool pty_is_half_closed_master(struct tty *tty) {
    if (tty->driver != &pty_master)
        return false;

    struct tty *slave = tty_lock_slave_side(tty);
    bool half_closed = slave->ever_opened &&
            (slave->open_count == 0 || slave->hung_up);
    tty_unlock_slave_side(tty, slave);
    // 检查 slave 时曾临时释放 master；合并窗口内发生的 master hangup。
    return half_closed || tty->hung_up;
}

// 调用方持有 tty 锁。PTY master 的对端关闭终态是 EIO，其他 hangup 是 EOF。
static bool tty_read_terminal_result(struct tty *tty, int *result) {
    if (pty_is_half_closed_master(tty)) {
        *result = _EIO;
        return true;
    }
    if (tty->hung_up) {
        *result = 0;
        return true;
    }
    return false;
}

static bool tty_is_current(struct tty *tty) {
    lock(&current->group->lock);
    bool is_current = current->group->tty == tty;
    unlock(&current->group->lock);
    return is_current;
}

static ssize_t tty_read(struct fd *fd, void *buf, size_t bufsize) {
    // important because otherwise we'll block
    if (bufsize == 0)
        return 0;

    int err = 0;
    struct tty *tty = fd->tty;
    lock(&pids_lock);
    lock(&tty->lock);
    bool terminal = tty_read_terminal_result(tty, &err);
    bool master_has_pending = tty->driver == &pty_master &&
            (tty->bufsize > 0 ||
            (tty->pty.packet_mode && tty->packet_flags != 0));
    if (terminal && !master_has_pending) {
        unlock(&pids_lock);
        goto error;
    }

    pid_t_ current_pgid = current->group->pgid;
    bool background = !terminal && tty_is_current(tty) &&
            tty->fg_group != 0 &&
            current_pgid != tty->fg_group;
    if (background) {
        // 信号路径会取得 pids_lock；先按 pids→tty 的全局锁序完整退出临界区。
        unlock(&tty->lock);
        unlock(&pids_lock);
        return try_self_signal(SIGTTIN_) ? _EINTR : _EIO;
    }
    unlock(&pids_lock);

    int bufsize_extra = 0;
    if (tty->driver == &pty_master && tty->pty.packet_mode) {
        char *cbuf = buf;
        *cbuf++ = tty->packet_flags;
        bufsize--;
        bufsize_extra++;
        buf = cbuf;
        if (tty->packet_flags != 0) {
            bufsize = 0;
            goto out;
        }

        // check again in case bufsize was 1
        if (bufsize == 0)
            goto out;
    }

    // wait loop(s)
    if (tty->termios.lflags & ICANON_) {
        size_t canon_size;
        while ((canon_size = tty_canon_size(tty)) == (size_t) -1) {
            if (tty_read_terminal_result(tty, &err))
                goto error;
            err = _EAGAIN;
            if (fd->flags & O_NONBLOCK_)
                goto error;
            err = wait_for(&tty->produced, &tty->lock, NULL);
            if (err < 0)
                goto error;
        }
        // null byte means eof was typed
        if (tty->buf[canon_size-1] == '\0')
            canon_size--;

        if (bufsize > canon_size)
            bufsize = canon_size;
    } else {
        dword_t min = tty->termios.cc[VMIN_];
        dword_t time = tty->termios.cc[VTIME_];

        struct timespec timeout;
        // time is in tenths of a second
        timeout.tv_sec = time / 10;
        timeout.tv_nsec = (time % 10) * 100000000;
        struct timespec *timeout_ptr = &timeout;
        if (time == 0)
            timeout_ptr = NULL;

        while (tty->bufsize < min) {
            if (tty_read_terminal_result(tty, &err)) {
                // Linux 会先排空 slave→master 的既有数据，再对 master 返回 EIO。
                if (tty->driver == &pty_master && tty->bufsize > 0)
                    break;
                goto error;
            }
            err = _EAGAIN;
            if (fd->flags & O_NONBLOCK_)
                goto error;
            // there should be no timeout for the first character read
            err = wait_for(&tty->produced, &tty->lock, tty->bufsize == 0 ? NULL : timeout_ptr);
            if (err == _ETIMEDOUT)
                break;
            if (err == _EINTR)
                goto error;
        }
    }

    if (bufsize > tty->bufsize)
        bufsize = tty->bufsize;
    tty_read_into_buf(tty, buf, bufsize);
    if (tty->bufsize > 0 && tty->buf[0] == '\0' && tty->buf_flag[0]) {
        // remove the eof so the next read can succeed
        char dummy;
        tty_read_into_buf(tty, &dummy, 1);
    }

out:
    unlock(&tty->lock);
    return bufsize + bufsize_extra;
error:
    unlock(&tty->lock);
    return err;
}

static ssize_t tty_write(struct fd *fd, const void *buf, size_t bufsize) {
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    if (tty->hung_up) {
        unlock(&tty->lock);
        return _EIO;
    }

    bool blocking = !(fd->flags & O_NONBLOCK_);
    dword_t oflags = tty->termios.oflags;
    // we have to unlock it now to avoid lock ordering problems with ptys
    // the code below is safe because it only accesses tty->driver which is immutable
    // I reviewed real driver and ios driver and they're safe
    unlock(&tty->lock);

    int err = 0;
    char *postbuf = NULL;
    size_t postbufsize = bufsize;
    if (oflags & OPOST_) {
        postbuf = malloc(bufsize * 2);
        postbufsize = 0;
        const char *cbuf = buf;
        for (size_t i = 0; i < bufsize; i++) {
            char ch = cbuf[i];
            if (ch == '\r' && oflags & ONLRET_)
                continue;
            else if (ch == '\r' && oflags & OCRNL_)
                ch = '\n';
            else if (ch == '\n' && oflags & ONLCR_)
                postbuf[postbufsize++] = '\r';
            postbuf[postbufsize++] = ch;
        }
        buf = postbuf;
    }
    err = tty->driver->ops->write(tty, buf, postbufsize, blocking);
    if (postbuf)
        free(postbuf);
    if (err < 0)
        return err;
    return bufsize;
}

static int tty_poll(struct fd *fd) {
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    int types = 0;
    types |= POLL_WRITE; // FIXME now that we have ptys, you can't always write without blocking
    if (tty->hung_up) {
        types |= POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP;
    } else if (pty_is_half_closed_master(tty)) {
        types |= POLL_READ | POLL_HUP;
    } else if (tty->termios.lflags & ICANON_) {
        if (tty_canon_size(tty) != (size_t) -1)
            types |= POLL_READ;
    } else {
        if (tty->bufsize > 0)
            types |= POLL_READ;
    }
    if (tty->driver == &pty_master && tty->packet_flags != 0)
        types |= POLL_PRI;
    unlock(&tty->lock);
    return types;
}

static ssize_t tty_ioctl_size(int cmd) {
    switch (cmd) {
        case TCGETS_: case TCSETS_: case TCSETSF_: case TCSETSW_:
            return sizeof(struct termios_);
        case TIOCGWINSZ_: case TIOCSWINSZ_:
            return sizeof(struct winsize_);
        case TIOCGPGRP_: case TIOCSPGRP_:
        case TIOCSPTLCK_: case TIOCGPTN_:
        case TIOCPKT_: case TIOCGPKT_:
        case FIONREAD_:
            return sizeof(dword_t);
        case TCFLSH_: case TIOCSCTTY_:
            return 0;
    }
    return -1;
}

static int tiocsctty(struct tty *tty, int force) {
    int err = 0;
    unlock(&tty->lock); //aaaaaaaa
    // it's safe because literally nothing happens between that unlock and the last lock, and repulsive for the same reason
    // locking is ***hard**
    lock(&pids_lock);
    lock(&tty->lock);
    struct tgroup *group = current->group;
    if (group->sid != group->leader->pid) {
        err = _EPERM;
        goto out;
    }
    // do nothing if this is already our controlling tty
    if (group->tty == tty && group->sid == tty->session)
        goto out;
    // must not already have a tty
    if (group->tty != NULL) {
        err = _EPERM;
        goto out;
    }

    if (tty->session) {
        if (force == 1 && superuser()) {
            // steal it
            struct pid *pid = pid_get(tty->session);
            struct tgroup *tgroup;
            list_for_each_entry(&pid->session, tgroup, session) {
                lock(&tgroup->lock);
                if (tgroup->tty == tty) {
                    tgroup->tty = NULL;
                    tty->refcount--;
                }
                unlock(&tgroup->lock);
            }
        } else {
            err = _EPERM;
            goto out;
        }
    }

    tty_set_controlling(group, tty);
out:
    unlock(&pids_lock);
    return err;
}

static int tiocgpgrp(struct tty *tty, pid_t_ *fg_group) {
    int err = 0;
    struct tty *slave = tty_lock_slave_side(tty);

    if (tty == slave && (!tty_is_current(slave) || slave->fg_group == 0)) {
        err = _ENOTTY;
        goto error_no_ctrl_tty;
    }
    *fg_group = slave->fg_group;
    STRACE("tty group = %d\n", slave->fg_group);

error_no_ctrl_tty:
    tty_unlock_slave_side(tty, slave);
    return err;
}

// These ioctls are separated out because they have to operate on the slave
// side of a pseudoterminal pair even if the master is specified
static int tty_mode_ioctl(struct tty *in_tty, int cmd, void *arg) {
    int err = 0;
    struct tty *tty = tty_lock_slave_side(in_tty);

    switch (cmd) {
        case TCGETS_:
            *(struct termios_ *) arg = tty->termios;
            break;
        case TCSETSF_:
            tty->bufsize = 0;
            notify(&tty->consumed);
            fallthrough;
        case TCSETSW_:
            // we have no output buffer currently
        case TCSETS_:
            tty->termios = *(struct termios_ *) arg;
            break;

        case TIOCGWINSZ_:
            *(struct winsize_ *) arg = tty->winsize;
            break;
        case TIOCSWINSZ_:
            tty_set_winsize(tty, *(struct winsize_ *) arg);
            break;

        default:
            err = _ENOTTY;
            break;
    }

    tty_unlock_slave_side(in_tty, tty);
    return err;
}

static int tty_ioctl(struct fd *fd, int cmd, void *arg) {
    int err = 0;
    struct tty *tty = fd->tty;
    lock(&tty->lock);
    if (tty->hung_up) {
        unlock(&tty->lock);
        if (cmd == TIOCSPGRP_)
            return _ENOTTY;
        return _EIO;
    }

    switch (cmd) {
        case TCFLSH_:
            // only input flushing is currently useful
            switch ((uintptr_t) arg) {
                case TCIFLUSH_:
                case TCIOFLUSH_:
                    tty->bufsize = 0;
                    notify(&tty->consumed);
                    break;
                case TCOFLUSH_:
                    break;
                default:
                    err = _EINVAL;
                    break;
            };
            break;

        case TIOCSCTTY_: {
            // Linux 的作业控制始终绑定 PTY slave；master ioctl 只是一条入口路径。
            struct tty *real_tty = tty_lock_slave_side(tty);
            err = tiocsctty(real_tty, (uintptr_t) arg);
            tty_unlock_slave_side(tty, real_tty);
            break;
        }

        case TIOCGPGRP_:
            err = tiocgpgrp(tty, (pid_t_ *) arg);
            break;

        case TIOCSPGRP_: {
            // 与 TIOCSCTTY/TIOCGPGRP 一致，PTY master 入口必须操作真实 slave。
            struct tty *real_tty = tty_lock_slave_side(tty);
            // see "aaaaaaaa" comment above
            unlock(&real_tty->lock);
            lock(&pids_lock);
            lock(&real_tty->lock);
            pid_t_ sid = current->group->sid;
            unlock(&pids_lock);
            if (!tty_is_current(real_tty) || sid != real_tty->session) {
                err = _ENOTTY;
            } else {
                // TODO group must be in the right session
                real_tty->fg_group = *(dword_t *) arg;
                STRACE("tty group set to = %d\n", real_tty->fg_group);
            }
            tty_unlock_slave_side(tty, real_tty);
            break;
        }

        case FIONREAD_:
            *(dword_t *) arg = tty->bufsize;
            break;

        default:
            err = tty_mode_ioctl(tty, cmd, arg);
            if (err == _ENOTTY && tty->driver->ops->ioctl)
                err = tty->driver->ops->ioctl(tty, cmd, arg);
    }

    unlock(&tty->lock);
    return err;
}

void tty_set_winsize(struct tty *tty, struct winsize_ winsize) {
    assert(lock_owned_by_current(&tty->lock));
    tty->winsize = winsize;
    pid_t_ fg_group = tty->fg_group;
    if (fg_group != 0) {
        // 进程表的锁序是 pids_lock→tty；发送信号时临时放开 tty 锁。
        unlock(&tty->lock);
        send_group_signal(fg_group, SIGWINCH_, SIGINFO_NIL);
        lock(&tty->lock);
    }
}

void tty_hangup(struct tty *tty) {
    tty->hung_up = true;
    tty_input_wakeup(tty);
    notify(&tty->consumed);
}

void tty_notify_peer_closed(struct tty *tty) {
    assert(lock_owned_by_current(&tty->lock));
    notify(&tty->produced);
    tty_poll_wakeup(tty, POLL_READ | POLL_HUP);
}

struct dev_ops tty_dev = {
    .open = tty_device_open,
    .fd.close = tty_close,
    .fd.read = tty_read,
    .fd.write = tty_write,
    .fd.poll = tty_poll,
    .fd.ioctl_size = tty_ioctl_size,
    .fd.ioctl = tty_ioctl,
};

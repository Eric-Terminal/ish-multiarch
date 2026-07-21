#include "debug.h"
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>

#include "kernel/calls.h"
#include "fs/tty.h"
#include "fs/devices.h"

// Only /dev/tty1 will be connected, the rest will go to a black hole.
#define REAL_TTY_NUM 1

void real_tty_reset_term(void);

static void real_tty_cancel_input(void *opaque) {
    struct tty *tty = opaque;
    assert(lock_owned_by_current(&tty->input_lock));
    unlock(&tty->input_lock);
}

static void *real_tty_read_thread(void *_tty) {
    struct tty *tty = _tty;
    char ch;
    for (;;) {
        int err = read(STDIN_FILENO, &ch, 1);
        if (err != 1) {
            printk("tty read returned %d\n", err);
            if (err < 0)
                printk("error: %s\n", strerror(errno));
            continue;
        }
        if (ch == '\x1c') {
            // ^\ (so ^C still works for emulated SIGINT)
            real_tty_reset_term();
            raise(SIGINT);
        }
        int cancel_error = pthread_setcancelstate(
                PTHREAD_CANCEL_DISABLE, NULL);
        if (cancel_error != 0)
            die("无法保护真实终端输入临界区：%s",
                    strerror(cancel_error));
        // real_tty_write 会在无 tty 锁的宿主 write 窗口恢复可取消；取消时归还输入事务锁。
        pthread_cleanup_push(real_tty_cancel_input, tty);
        tty_input(tty, &ch, 1, 0);
        pthread_cleanup_pop(0);
        cancel_error = pthread_setcancelstate(
                PTHREAD_CANCEL_ENABLE, NULL);
        if (cancel_error != 0)
            die("无法恢复真实终端读取线程取消状态：%s",
                    strerror(cancel_error));
        // 某些宿主不会仅因重新启用 deferred cancel 就立刻交付挂起请求。
        pthread_testcancel();
    }
    return NULL;
}

static struct termios_ termios_from_real(struct termios real) {
    struct termios_ fake = {};
#define FLAG(t, x) \
    if (real.c_##t##flag & x) \
        fake.t##flags |= x##_
    FLAG(o, OPOST);
    FLAG(o, ONLCR);
    FLAG(o, OCRNL);
    FLAG(o, ONOCR);
    FLAG(o, ONLRET);
    FLAG(i, INLCR);
    FLAG(i, IGNCR);
    FLAG(i, ICRNL);
    FLAG(l, ISIG);
    FLAG(l, ICANON);
    FLAG(l, ECHO);
    FLAG(l, ECHOE);
    FLAG(l, ECHOK);
    FLAG(l, NOFLSH);
    FLAG(l, ECHOCTL);
#undef FLAG

#define CC(x) \
    fake.cc[V##x##_] = real.c_cc[V##x]
    CC(INTR);
    CC(QUIT);
    CC(ERASE);
    CC(KILL);
    CC(EOF);
    CC(TIME);
    CC(MIN);
    CC(START);
    CC(STOP);
    CC(SUSP);
    CC(EOL);
    CC(REPRINT);
    CC(DISCARD);
    CC(WERASE);
    CC(LNEXT);
    CC(EOL2);
#undef CC
    return fake;
}

static struct termios old_termios;
static bool real_tty_is_open;
static bool real_tty_term_changed;
static int real_tty_init(struct tty *tty) {
    int create_error;
    if (tty->num != REAL_TTY_NUM)
        return 0;

    struct winsize winsz;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsz) < 0) {
        if (errno == ENOTTY)
            goto notty;
        return errno_map();
    }
    tty->winsize.col = winsz.ws_col;
    tty->winsize.row = winsz.ws_row;
    tty->winsize.xpixel = winsz.ws_xpixel;
    tty->winsize.ypixel = winsz.ws_ypixel;

    struct termios termios;
    if (tcgetattr(STDIN_FILENO, &termios) < 0)
        return errno_map();
    tty->termios = termios_from_real(termios);

    old_termios = termios;
    cfmakeraw(&termios);
#ifdef NO_CRLF
    termios.c_oflag |= OPOST | ONLCR;
#endif
    if (tcsetattr(STDIN_FILENO, TCSANOW, &termios) < 0)
        return errno_map();
    real_tty_term_changed = true;
notty:

    create_error = pthread_create(
            &tty->thread, NULL, real_tty_read_thread, tty);
    if (create_error != 0) {
        if (real_tty_term_changed &&
                tcsetattr(STDIN_FILENO, TCSANOW, &old_termios) < 0 &&
                errno != ENOTTY) {
            int restore_error = errno;
            die("创建真实终端读取线程失败，且无法恢复终端：%s",
                    strerror(restore_error));
        }
        real_tty_term_changed = false;
        return err_map(create_error);
    }
    real_tty_is_open = true;
    return 0;
}

static int real_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    if (tty->num != REAL_TTY_NUM)
        return len;
    int previous_cancel_state = PTHREAD_CANCEL_ENABLE;
    if (!blocking) {
        int cancel_error = pthread_setcancelstate(
                PTHREAD_CANCEL_ENABLE, &previous_cancel_state);
        if (cancel_error != 0)
            return err_map(cancel_error);
    }
    int result = (int) write(STDOUT_FILENO, buf, len);
    if (!blocking) {
        int cancel_error = pthread_setcancelstate(
                previous_cancel_state, NULL);
        if (cancel_error != 0)
            die("无法恢复真实终端写入线程取消状态：%s",
                    strerror(cancel_error));
    }
    return result;
}

void real_tty_reset_term(void) {
    if (!real_tty_is_open || !real_tty_term_changed) return;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &old_termios) < 0 && errno != ENOTTY) {
        printk("failed to reset terminal: %s\n", strerror(errno));
        abort();
    }
}

static void real_tty_cleanup(struct tty *tty) {
    if (tty->num != REAL_TTY_NUM)
        return;
    assert(!lock_owned_by_current(&pids_lock));
    int cancel_error = pthread_cancel(tty->thread);
    if (cancel_error != 0)
        die("无法取消真实终端读取线程：%s", strerror(cancel_error));

    // reader 可能仍在 tty_input；等待时释放全局锁，由 reservation 阻止同槽复用。
    unlock(&ttys_lock);
    int join_error = pthread_join(tty->thread, NULL);
    lock(&ttys_lock);
    if (join_error != 0)
        die("无法回收真实终端读取线程：%s", strerror(join_error));

    real_tty_reset_term();
    real_tty_is_open = false;
    real_tty_term_changed = false;
}

struct tty_driver_ops real_tty_ops = {
    .init = real_tty_init,
    .write = real_tty_write,
    .cleanup = real_tty_cleanup,
};
DEFINE_TTY_DRIVER(real_tty_driver, &real_tty_ops, TTY_CONSOLE_MAJOR, 64);

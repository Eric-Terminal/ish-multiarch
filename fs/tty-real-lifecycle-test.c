#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fs/tty.h"
#include "fs/devices.h"
#include "kernel/errno.h"
#include "kernel/task.h"

static atomic_uint failures = ATOMIC_VAR_INIT(0);

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "真实终端生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed); \
    } \
} while (0)

struct release_call {
    struct tty *tty;
};

static void *release_tty_main(void *opaque) {
    struct release_call *call = opaque;
    lock(&ttys_lock);
    tty_release(call->tty);
    unlock(&ttys_lock);
    return NULL;
}

static void test_blocked_echo_cancel(void) {
    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    EXPECT(saved_stdin >= 0 && saved_stdout >= 0,
            "保存阻塞回显测试的标准流");
    if (saved_stdin < 0 || saved_stdout < 0)
        goto close_saved;

    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    EXPECT(pipe(input_pipe) == 0, "建立阻塞回显输入管道");
    EXPECT(pipe(output_pipe) == 0, "建立阻塞回显输出管道");
    if (input_pipe[0] < 0 || output_pipe[0] < 0)
        goto close_pipes;

    int output_flags = fcntl(output_pipe[1], F_GETFL);
    EXPECT(output_flags >= 0 &&
            fcntl(output_pipe[1], F_SETFL,
            output_flags | O_NONBLOCK) == 0,
            "临时启用输出管道非阻塞填充");
    char fill[4096];
    memset(fill, 'z', sizeof(fill));
    ssize_t fill_result;
    do {
        fill_result = write(output_pipe[1], fill, sizeof(fill));
    } while (fill_result > 0);
    EXPECT(fill_result < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK),
            "把宿主输出管道确定性填满");
    EXPECT(fcntl(output_pipe[1], F_SETFL, output_flags) == 0,
            "恢复输出管道阻塞模式");

    EXPECT(dup2(input_pipe[0], STDIN_FILENO) == STDIN_FILENO,
            "把阻塞回显 reader 接到输入管道");
    EXPECT(dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO,
            "把真实终端回显接到已满输出管道");

    struct tty *tty = tty_get(
            &real_tty_driver, TTY_CONSOLE_MAJOR, 1);
    EXPECT(tty != NULL && !IS_ERR(tty),
            "创建宿主回显会阻塞的真实终端对象");
    if (tty == NULL || IS_ERR(tty))
        goto restore_streams;

    signal(SIGALRM, SIG_DFL);
    alarm(5);
    EXPECT(write(input_pipe[1], "a", 1) == 1,
            "向 reader 发送会触发回显的普通字符");
    int available = 1;
    while (available != 0) {
        EXPECT(ioctl(input_pipe[0], FIONREAD, &available) == 0,
                "观察 reader 已消费回显字符");
        if (available != 0)
            sched_yield();
    }

    bool echo_blocked = false;
    while (!echo_blocked) {
        if (trylock(&tty->lock) == 0) {
            int input_result = trylock(&tty->input_lock);
            echo_blocked = input_result != 0;
            if (input_result == 0)
                unlock(&tty->input_lock);
            unlock(&tty->lock);
        }
        if (!echo_blocked)
            sched_yield();
    }
    EXPECT(echo_blocked,
            "宿主 write 阻塞时只持 input lock 而不持 tty lock");

    struct release_call call = {.tty = tty};
    pthread_t releaser;
    int create_error = pthread_create(
            &releaser, NULL, release_tty_main, &call);
    EXPECT(create_error == 0, "建立阻塞回显释放线程");
    if (create_error == 0) {
        EXPECT(pthread_join(releaser, NULL) == 0,
                "取消阻塞宿主 write 后 reader 与 cleanup 有界退出");
    } else {
        lock(&ttys_lock);
        tty_release(tty);
        unlock(&ttys_lock);
    }
    alarm(0);

    lock(&ttys_lock);
    EXPECT(real_tty_driver.ttys[1] == NULL &&
            !real_tty_driver.reserved[1],
            "阻塞回显取消后同步对象与槽位完整回收");
    unlock(&ttys_lock);

restore_streams:
    alarm(0);
    EXPECT(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO,
            "恢复阻塞回显测试的标准输入");
    EXPECT(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO,
            "恢复阻塞回显测试的标准输出");

close_pipes:
    if (input_pipe[0] >= 0)
        close(input_pipe[0]);
    if (input_pipe[1] >= 0)
        close(input_pipe[1]);
    if (output_pipe[0] >= 0)
        close(output_pipe[0]);
    if (output_pipe[1] >= 0)
        close(output_pipe[1]);

close_saved:
    if (saved_stdin >= 0)
        close(saved_stdin);
    if (saved_stdout >= 0)
        close(saved_stdout);
}

int main(void) {
    int saved_stdin = dup(STDIN_FILENO);
    EXPECT(saved_stdin >= 0, "保存标准输入");
    if (saved_stdin < 0)
        return 1;

    int input_pipe[2];
    int pipe_error = pipe(input_pipe);
    EXPECT(pipe_error == 0, "建立阻塞读取管道");
    if (pipe_error != 0) {
        close(saved_stdin);
        return 1;
    }
    int redirect_result = dup2(input_pipe[0], STDIN_FILENO);
    EXPECT(redirect_result == STDIN_FILENO,
            "把真实终端 reader 接到测试管道");
    if (redirect_result != STDIN_FILENO) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(saved_stdin);
        return 1;
    }

    struct tty *tty = tty_get(
            &real_tty_driver, TTY_CONSOLE_MAJOR, 1);
    EXPECT(tty != NULL && !IS_ERR(tty), "创建带 reader 的真实终端对象");
    if (tty != NULL && !IS_ERR(tty)) {
        lock(&tty->lock);
        tty->fg_group = 7201;
        // 取消回显后，reader 在本批输入中始终禁用取消，必然走到 pids_lock 闸门。
        tty->termios.lflags &= ~ECHO_;
        unlock(&tty->lock);

        signal(SIGALRM, SIG_DFL);
        alarm(5);
        lock(&ttys_lock);
        EXPECT(real_tty_driver.ttys[1] == tty &&
                !real_tty_driver.reserved[1],
                "reader 启动后对象已完整发布");
        unlock(&ttys_lock);

        lock(&pids_lock);
        EXPECT(write(input_pipe[1], "\003", 1) == 1,
                "向 reader 发送终端控制字符");
        int available = 1;
        while (available != 0) {
            EXPECT(ioctl(input_pipe[0], FIONREAD, &available) == 0,
                    "观察 reader 已消费控制字符");
            if (available != 0)
                sched_yield();
        }
        bool input_active = false;
        while (!input_active) {
            int input_result = trylock(&tty->input_lock);
            input_active = input_result != 0;
            if (input_result == 0)
                unlock(&tty->input_lock);
            if (!input_active)
                sched_yield();
        }
        EXPECT(input_active,
                "reader 已持输入事务锁并将稳定等待 pids_lock");

        struct release_call call = {.tty = tty};
        pthread_t releaser;
        int create_error = pthread_create(
                &releaser, NULL, release_tty_main, &call);
        EXPECT(create_error == 0, "建立真实终端释放线程");
        if (create_error == 0) {
            bool reserved = false;
            while (!reserved) {
                lock(&ttys_lock);
                reserved = real_tty_driver.ttys[1] == NULL &&
                        real_tty_driver.reserved[1];
                unlock(&ttys_lock);
                if (!reserved)
                    sched_yield();
            }
            EXPECT(reserved, "join 窗口保持槽位 reservation");
            struct tty *during_cleanup = tty_get(
                    &real_tty_driver, TTY_CONSOLE_MAJOR, 1);
            EXPECT(IS_ERR(during_cleanup) &&
                    PTR_ERR(during_cleanup) == _EAGAIN,
                    "cleanup 窗口拒绝复用真实终端槽位");
            unlock(&pids_lock);
            EXPECT(pthread_join(releaser, NULL) == 0,
                    "释放 pids_lock 后 reader 与释放线程有界退出");
        } else {
            unlock(&pids_lock);
            lock(&ttys_lock);
            tty_release(tty);
            unlock(&ttys_lock);
        }

        lock(&ttys_lock);
        EXPECT(real_tty_driver.ttys[1] == NULL &&
                !real_tty_driver.reserved[1],
                "cleanup 等待 reader 退出后完整清理槽位");
        unlock(&ttys_lock);
        alarm(0);
    }

    close(input_pipe[0]);
    close(input_pipe[1]);
    EXPECT(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO,
            "恢复标准输入");
    close(saved_stdin);

    test_blocked_echo_cancel();

    unsigned count = atomic_load_explicit(
            &failures, memory_order_relaxed);
    if (count != 0) {
        fprintf(stderr, "真实终端生命周期测试共发现 %u 项失败\n", count);
        return 1;
    }
    puts("真实终端生命周期测试通过");
    return 0;
}

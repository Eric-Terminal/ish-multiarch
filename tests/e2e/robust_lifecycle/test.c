#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct shared_mutex {
    pthread_mutex_t mutex;
};

static volatile sig_atomic_t timeout_child = -1;
static int shared_mutex_failure_stage;

static void timeout_handler(int signal_number) {
    (void) signal_number;
    if (timeout_child > 0)
        kill((pid_t) timeout_child, SIGKILL);
    _exit(124);
}

static struct shared_mutex *make_shared_mutex(void) {
    shared_mutex_failure_stage = 1;
    struct shared_mutex *shared = mmap(NULL, sizeof(*shared),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        return NULL;

    shared_mutex_failure_stage = 2;
    pthread_mutexattr_t attributes;
    int result = pthread_mutexattr_init(&attributes);
    if (result != 0) {
        munmap(shared, sizeof(*shared));
        errno = result;
        return NULL;
    }
    shared_mutex_failure_stage = 3;
    result = pthread_mutexattr_setrobust(
            &attributes, PTHREAD_MUTEX_ROBUST);
    if (result == 0) {
        shared_mutex_failure_stage = 4;
        result = pthread_mutexattr_setpshared(
                &attributes, PTHREAD_PROCESS_SHARED);
    }
    if (result == 0) {
        shared_mutex_failure_stage = 5;
        result = pthread_mutex_init(&shared->mutex, &attributes);
    }
    (void) pthread_mutexattr_destroy(&attributes);
    if (result != 0) {
        munmap(shared, sizeof(*shared));
        errno = result;
        return NULL;
    }
    shared_mutex_failure_stage = 0;
    return shared;
}

static int recover_mutex(struct shared_mutex *shared) {
    alarm(10);
    int result = pthread_mutex_lock(&shared->mutex);
    alarm(0);
    if (result != EOWNERDEAD)
        return result == 0 ? EINVAL : result;
    result = pthread_mutex_consistent(&shared->mutex);
    if (result == 0)
        result = pthread_mutex_unlock(&shared->mutex);
    return result;
}

static int wait_for_marker(int fd) {
    char marker;
    ssize_t size;
    do {
        size = read(fd, &marker, sizeof(marker));
    } while (size < 0 && errno == EINTR);
    return size == 1 && marker == 'L' ? 0 : -1;
}

static int test_exit_cleanup(void) {
    struct shared_mutex *shared = make_shared_mutex();
    int pipe_fds[2];
    if (shared == NULL) {
        fprintf(stderr,
                "退出清理详情：初始化 robust mutex 失败（stage=%d error=%d）\n",
                shared_mutex_failure_stage, errno);
        return -1;
    }
    if (pipe(pipe_fds) != 0) {
        fprintf(stderr, "退出清理详情：创建管道失败（%d）\n", errno);
        return -1;
    }

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(pipe_fds[0]);
        if (pthread_mutex_lock(&shared->mutex) != 0 ||
                write(pipe_fds[1], "L", 1) != 1)
            _exit(20);
        _exit(0);
    }

    close(pipe_fds[1]);
    timeout_child = child;
    alarm(10);
    int status = 0;
    int marker_result = wait_for_marker(pipe_fds[0]);
    pid_t waited = waitpid(child, &status, 0);
    if (waited == child)
        timeout_child = -1;
    int recover_result = recover_mutex(shared);
    if (marker_result != 0 || waited != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            recover_result != 0) {
        fprintf(stderr,
                "退出清理详情：marker=%d waited=%d status=%#x recover=%d\n",
                marker_result, (int) waited, status, recover_result);
        return -1;
    }
    timeout_child = -1;
    close(pipe_fds[0]);
    pthread_mutex_destroy(&shared->mutex);
    return munmap(shared, sizeof(*shared));
}

static int test_exec_cleanup(const char *program) {
    struct shared_mutex *shared = make_shared_mutex();
    int pipe_fds[2];
    if (shared == NULL || pipe(pipe_fds) != 0)
        return -1;
    if (fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) != 0)
        return -1;

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(pipe_fds[0]);
        if (pthread_mutex_lock(&shared->mutex) != 0 ||
                write(pipe_fds[1], "L", 1) != 1)
            _exit(30);
        execl(program, program, "replacement", NULL);
        _exit(31);
    }

    close(pipe_fds[1]);
    timeout_child = child;
    // CLOEXEC 管道的 EOF 表示旧映像已经跨过成功 exec 的提交边界。
    alarm(10);
    char discarded;
    if (wait_for_marker(pipe_fds[0]) != 0 ||
            read(pipe_fds[0], &discarded, sizeof(discarded)) != 0 ||
            recover_mutex(shared) != 0) {
        alarm(10);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        alarm(0);
        return -1;
    }
    alarm(10);
    int kill_result = kill(child, SIGKILL);
    int status;
    pid_t waited = waitpid(child, &status, 0);
    alarm(0);
    if (kill_result != 0 || waited != child ||
            !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        return -1;
    timeout_child = -1;
    close(pipe_fds[0]);
    pthread_mutex_destroy(&shared->mutex);
    return munmap(shared, sizeof(*shared));
}

static void *return_thread_value(void *opaque) {
    return opaque;
}

static int test_clear_child_tid(void) {
    pthread_t thread;
    void *expected = (void *) (uintptr_t) 0x1234;
    void *observed = NULL;
    int result = pthread_create(
            &thread, NULL, return_thread_value, expected);
    if (result == 0) {
        alarm(10);
        result = pthread_join(thread, &observed);
        alarm(0);
    }
    return result == 0 && observed == expected ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "replacement") == 0) {
        // 新映像保持存活，避免把旧 robust 修复误归因于随后退出。
        for (;;)
            pause();
    }

    struct sigaction action = {
        .sa_handler = timeout_handler,
    };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) {
        fprintf(stderr, "i386 robust 生命周期测试失败：安装超时处理器\n");
        return 1;
    }
    if (test_exit_cleanup() != 0) {
        fprintf(stderr, "i386 robust 生命周期测试失败：退出清理\n");
        return 1;
    }
    if (test_exec_cleanup(argv[0]) != 0) {
        fprintf(stderr, "i386 robust 生命周期测试失败：exec 清理\n");
        return 1;
    }
    if (test_clear_child_tid() != 0) {
        fprintf(stderr, "i386 robust 生命周期测试失败：clear-child-tid\n");
        return 1;
    }
    puts("i386 robust 生命周期：通过");
    return 0;
}

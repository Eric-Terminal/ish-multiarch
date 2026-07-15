#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_FUTEX_WAIT 0
#define TEST_FUTEX_WAKE 1

static volatile sig_atomic_t active_child = -1;

static void timeout_handler(int signal_number) {
    (void) signal_number;
    if (active_child > 0)
        kill((pid_t) active_child, SIGKILL);
    _exit(124);
}

static int write_marker(int fd) {
    const char marker = 'R';
    ssize_t size;
    do {
        size = write(fd, &marker, sizeof(marker));
    } while (size < 0 && errno == EINTR);
    return size == 1 ? 0 : -1;
}

static int read_marker(int fd) {
    char marker;
    ssize_t size;
    do {
        size = read(fd, &marker, sizeof(marker));
    } while (size < 0 && errno == EINTR);
    return size == 1 && marker == 'R' ? 0 : -1;
}

static int futex_wait(volatile uint32_t *address, uint32_t expected) {
    const struct timespec timeout = {
        .tv_sec = 5,
    };
    long result;
    do {
        result = syscall(SYS_futex, (uint32_t *) address,
                TEST_FUTEX_WAIT, expected, &timeout, NULL, 0);
    } while (result < 0 && errno == EINTR);
    return (int) result;
}

static int futex_wake(volatile uint32_t *address) {
    long result;
    do {
        result = syscall(SYS_futex, (uint32_t *) address,
                TEST_FUTEX_WAKE, 1, NULL, NULL, 0);
    } while (result < 0 && errno == EINTR);
    return (int) result;
}

static int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wake_queued_waiter(volatile uint32_t *address) {
    int64_t deadline = monotonic_milliseconds();
    if (deadline < 0)
        return -1;
    deadline += 5000;

    for (;;) {
        int result = futex_wake(address);
        if (result == 1)
            return 0;
        if (result < 0)
            return -1;
        int64_t now = monotonic_milliseconds();
        if (now < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        const struct timespec pause = {
            .tv_nsec = 1000000,
        };
        nanosleep(&pause, NULL);
    }
}

static int finish_child(pid_t child, int expected_status) {
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    active_child = -1;
    if (waited != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != expected_status) {
        errno = ECHILD;
        return -1;
    }
    return 0;
}

static int abort_child(pid_t child) {
    int saved_errno = errno == 0 ? EIO : errno;
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
    active_child = -1;
    errno = saved_errno;
    return -1;
}

static int wait_in_child(volatile uint32_t *address, int ready_fd) {
    if (write_marker(ready_fd) != 0)
        return 20;
    return futex_wait(address, 0) == 0 ? 0 : 21;
}

static int test_anonymous_shared_futex(void) {
    volatile uint32_t *word = mmap(NULL, sizeof(*word),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int ready[2];
    if (word == MAP_FAILED || pipe(ready) != 0)
        return -1;
    __atomic_store_n(word, 0, __ATOMIC_RELEASE);

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(ready[0]);
        int status = wait_in_child(word, ready[1]);
        close(ready[1]);
        _exit(status);
    }

    active_child = child;
    close(ready[1]);
    if (read_marker(ready[0]) != 0 || wake_queued_waiter(word) != 0) {
        close(ready[0]);
        munmap((void *) word, sizeof(*word));
        return abort_child(child);
    }
    close(ready[0]);
    int result = finish_child(child, 0);
    munmap((void *) word, sizeof(*word));
    return result;
}

static int test_file_shared_futex(void) {
    char path[] = "/tmp/futex-shared-XXXXXX";
    long page_size = sysconf(_SC_PAGESIZE);
    int fd = mkstemp(path);
    if (page_size <= 0 || fd < 0)
        return -1;
    if (ftruncate(fd, page_size * 2) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    volatile uint32_t *word = mmap(NULL, (size_t) page_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_size);
    volatile uint32_t *different_offset = mmap(NULL, (size_t) page_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int ready[2];
    if (word == MAP_FAILED || different_offset == MAP_FAILED ||
            pipe(ready) != 0) {
        if (word != MAP_FAILED)
            munmap((void *) word, (size_t) page_size);
        if (different_offset != MAP_FAILED)
            munmap((void *) different_offset, (size_t) page_size);
        close(fd);
        unlink(path);
        return -1;
    }
    __atomic_store_n(word, 0, __ATOMIC_RELEASE);
    __atomic_store_n(different_offset, 0, __ATOMIC_RELEASE);

    pid_t child = fork();
    if (child < 0) {
        munmap((void *) word, (size_t) page_size);
        munmap((void *) different_offset, (size_t) page_size);
        close(fd);
        unlink(path);
        return -1;
    }
    if (child == 0) {
        close(ready[0]);
        close(fd);
        int child_fd = open(path, O_RDWR);
        if (child_fd < 0)
            _exit(30);
        volatile uint32_t *child_word = mmap(NULL, (size_t) page_size,
                PROT_READ | PROT_WRITE, MAP_SHARED, child_fd, page_size);
        close(child_fd);
        if (child_word == MAP_FAILED)
            _exit(31);
        if (munmap((void *) word, (size_t) page_size) != 0)
            _exit(32);
        if (munmap((void *) different_offset, (size_t) page_size) != 0)
            _exit(33);
        int status = wait_in_child(child_word, ready[1]);
        munmap((void *) child_word, (size_t) page_size);
        close(ready[1]);
        _exit(status == 0 ? 0 : status + 20);
    }

    active_child = child;
    close(ready[1]);
    int marker_result = read_marker(ready[0]);
    close(ready[0]);
    unlink(path);
    // 同一 inode 的另一文件页不能命中已排队的等待者。
    if (marker_result != 0 || futex_wake(different_offset) != 0 ||
            wake_queued_waiter(word) != 0) {
        munmap((void *) word, (size_t) page_size);
        munmap((void *) different_offset, (size_t) page_size);
        close(fd);
        return abort_child(child);
    }
    int result = finish_child(child, 0);
    munmap((void *) word, (size_t) page_size);
    munmap((void *) different_offset, (size_t) page_size);
    close(fd);
    return result;
}

struct delayed_marker {
    int fd;
    int result;
};

static void *write_delayed_marker(void *opaque) {
    struct delayed_marker *delayed = opaque;
    struct timespec delay = {
        .tv_nsec = 500000000,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
    delayed->result = write_marker(delayed->fd);
    return NULL;
}

struct robust_shared {
    pthread_mutex_t mutex;
};

static int init_robust_mutex(struct robust_shared *shared) {
    pthread_mutexattr_t attributes;
    int result = pthread_mutexattr_init(&attributes);
    if (result != 0) {
        errno = result;
        return -1;
    }
    result = pthread_mutexattr_setrobust(
            &attributes, PTHREAD_MUTEX_ROBUST);
    if (result == 0)
        result = pthread_mutexattr_setpshared(
                &attributes, PTHREAD_PROCESS_SHARED);
    if (result == 0)
        result = pthread_mutex_init(&shared->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (result != 0)
        errno = result;
    return result == 0 ? 0 : -1;
}

static int test_queued_robust_wake(void) {
    struct robust_shared *shared = mmap(NULL, sizeof(*shared),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int ready[2];
    int release[2];
    if (shared == MAP_FAILED || init_robust_mutex(shared) != 0 ||
            pipe(ready) != 0 || pipe(release) != 0)
        return -1;

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(ready[0]);
        close(release[1]);
        if (pthread_mutex_lock(&shared->mutex) != 0 ||
                write_marker(ready[1]) != 0)
            _exit(40);
        if (read_marker(release[0]) != 0)
            _exit(41);
        _exit(0);
    }

    active_child = child;
    close(ready[1]);
    close(release[0]);
    if (read_marker(ready[0]) != 0) {
        close(ready[0]);
        close(release[1]);
        return abort_child(child);
    }
    close(ready[0]);

    struct delayed_marker delayed = {
        .fd = release[1],
        .result = -1,
    };
    pthread_t releaser;
    int thread_result = pthread_create(
            &releaser, NULL, write_delayed_marker, &delayed);
    if (thread_result != 0) {
        errno = thread_result;
        close(release[1]);
        return abort_child(child);
    }

    alarm(8);
    int lock_result = pthread_mutex_lock(&shared->mutex);
    alarm(0);
    pthread_join(releaser, NULL);
    close(release[1]);
    int child_result = finish_child(child, 0);
    int consistent_result = lock_result == EOWNERDEAD ?
            pthread_mutex_consistent(&shared->mutex) : EINVAL;
    int unlock_result = consistent_result == 0 ?
            pthread_mutex_unlock(&shared->mutex) : EINVAL;
    pthread_mutex_destroy(&shared->mutex);
    munmap(shared, sizeof(*shared));
    if (delayed.result != 0 || child_result != 0 ||
            lock_result != EOWNERDEAD || consistent_result != 0 ||
            unlock_result != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

struct clear_tid_worker {
    volatile uint32_t *address;
    int ready_fd;
    int release_fd;
};

static void *run_clear_tid_worker(void *opaque) {
    struct clear_tid_worker *worker = opaque;
    long tid = syscall(SYS_set_tid_address, (uint32_t *) worker->address);
    if (tid <= 0)
        _exit(50);
    __atomic_store_n(worker->address, (uint32_t) tid, __ATOMIC_RELEASE);
    if (write_marker(worker->ready_fd) != 0 ||
            read_marker(worker->release_fd) != 0)
        _exit(51);
    return NULL;
}

static int test_queued_clear_child_tid_wake(void) {
    volatile uint32_t *clear_tid = mmap(NULL, sizeof(*clear_tid),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int ready[2];
    int release[2];
    int finish[2];
    if (clear_tid == MAP_FAILED || pipe(ready) != 0 ||
            pipe(release) != 0 || pipe(finish) != 0)
        return -1;
    __atomic_store_n(clear_tid, 0, __ATOMIC_RELEASE);

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(ready[0]);
        close(release[1]);
        close(finish[1]);
        struct clear_tid_worker worker = {
            .address = clear_tid,
            .ready_fd = ready[1],
            .release_fd = release[0],
        };
        pthread_t thread;
        if (pthread_create(&thread, NULL, run_clear_tid_worker, &worker) != 0)
            _exit(52);
        if (read_marker(finish[0]) != 0)
            _exit(53);
        _exit(0);
    }

    active_child = child;
    close(ready[1]);
    close(release[0]);
    close(finish[0]);
    if (read_marker(ready[0]) != 0) {
        close(ready[0]);
        close(release[1]);
        close(finish[1]);
        return abort_child(child);
    }
    close(ready[0]);
    uint32_t expected = __atomic_load_n(clear_tid, __ATOMIC_ACQUIRE);
    if (expected == 0) {
        close(release[1]);
        close(finish[1]);
        errno = EIO;
        return abort_child(child);
    }

    struct delayed_marker delayed = {
        .fd = release[1],
        .result = -1,
    };
    pthread_t releaser;
    int thread_result = pthread_create(
            &releaser, NULL, write_delayed_marker, &delayed);
    if (thread_result != 0) {
        errno = thread_result;
        close(release[1]);
        close(finish[1]);
        return abort_child(child);
    }

    int wait_result = futex_wait(clear_tid, expected);
    pthread_join(releaser, NULL);
    close(release[1]);
    int finish_result = write_marker(finish[1]);
    close(finish[1]);
    int child_result = finish_child(child, 0);
    uint32_t final_tid = __atomic_load_n(clear_tid, __ATOMIC_ACQUIRE);
    munmap((void *) clear_tid, sizeof(*clear_tid));
    if (delayed.result != 0 || wait_result != 0 ||
            finish_result != 0 || child_result != 0 || final_tid != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int main(void) {
    struct sigaction action = {
        .sa_handler = timeout_handler,
    };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) {
        fprintf(stderr, "i386 共享 futex 测试失败：安装超时处理器\n");
        return 1;
    }
    if (test_anonymous_shared_futex() != 0) {
        fprintf(stderr, "i386 共享 futex 测试失败：匿名跨 mm WAIT/WAKE（%s）\n",
                strerror(errno));
        return 1;
    }
    if (test_file_shared_futex() != 0) {
        fprintf(stderr, "i386 共享 futex 测试失败：文件跨映射 WAIT/WAKE（%s）\n",
                strerror(errno));
        return 1;
    }
    if (test_queued_robust_wake() != 0) {
        fprintf(stderr, "i386 共享 futex 测试失败：robust 排队唤醒（%s）\n",
                strerror(errno));
        return 1;
    }
    if (test_queued_clear_child_tid_wake() != 0) {
        fprintf(stderr, "i386 共享 futex 测试失败：clear-child-tid 排队唤醒（%s）\n",
                strerror(errno));
        return 1;
    }
    puts("i386 共享 futex 跨 mm：通过");
    return 0;
}

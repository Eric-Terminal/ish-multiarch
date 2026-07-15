#define _GNU_SOURCE

#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REPORT_MAGIC UINT32_C(0x45584543)

struct exec_report {
    uint32_t magic;
    int ok;
    int observed_pid;
    int observed_tid;
    int expected_pid;
    int detail;
};

static volatile sig_atomic_t active_child = -1;
static volatile sig_atomic_t preserved_cookie = 0x3947;

static pid_t current_tid(void) {
    return (pid_t) syscall(SYS_gettid);
}

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] = "i386 多线程 exec 测试超时\n";
    (void) write(STDERR_FILENO, message, sizeof(message) - 1);
    if (active_child > 0)
        kill((pid_t) active_child, SIGKILL);
    _exit(124);
}

static void preserved_handler(int signal_number) {
    (void) signal_number;
    preserved_cookie = 0x3947;
}

static int write_all(int fd, const void *buffer, size_t size) {
    const char *cursor = buffer;
    while (size > 0) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        cursor += written;
        size -= (size_t) written;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t size) {
    char *cursor = buffer;
    while (size > 0) {
        ssize_t received = read(fd, cursor, size);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (received == 0) {
            errno = EPIPE;
            return -1;
        }
        cursor += received;
        size -= (size_t) received;
    }
    return 0;
}

static int parse_int(const char *text, int *value) {
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
            parsed < INT_MIN || parsed > INT_MAX)
        return -1;
    *value = (int) parsed;
    return 0;
}

static int send_report(int fd, int ok, pid_t observed_pid,
        pid_t observed_tid, pid_t expected_pid, int detail) {
    const struct exec_report report = {
        .magic = REPORT_MAGIC,
        .ok = ok,
        .observed_pid = observed_pid,
        .observed_tid = observed_tid,
        .expected_pid = expected_pid,
        .detail = detail,
    };
    return write_all(fd, &report, sizeof(report));
}

static int wait_for_child(pid_t child, int *status) {
    pid_t waited;
    do {
        waited = waitpid(child, status, 0);
    } while (waited < 0 && errno == EINTR);
    active_child = -1;
    return waited == child ? 0 : -1;
}

static int report_is_success(const struct exec_report *report,
        pid_t expected_pid) {
    return report->magic == REPORT_MAGIC && report->ok == 1 &&
            report->observed_pid == expected_pid &&
            report->observed_tid == expected_pid &&
            report->expected_pid == expected_pid;
}

static int run_nonleader_replacement(int argc, char **argv) {
    int expected_pid;
    int old_worker_tid;
    int report_fd;
    int release_fd;
    if (argc != 6 || parse_int(argv[2], &expected_pid) != 0 ||
            parse_int(argv[3], &old_worker_tid) != 0 ||
            parse_int(argv[4], &report_fd) != 0 ||
            parse_int(argv[5], &release_fd) != 0)
        return 90;

    pid_t observed_pid = getpid();
    pid_t observed_tid = current_tid();
    errno = 0;
    int old_tid_result = (int) syscall(
            SYS_tgkill, expected_pid, old_worker_tid, 0);
    int old_tid_errno = errno;
    int ok = observed_pid == expected_pid &&
            observed_tid == expected_pid &&
            old_worker_tid != expected_pid &&
            old_tid_result == -1 && old_tid_errno == ESRCH;
    if (send_report(report_fd, ok, observed_pid, observed_tid,
                expected_pid, old_tid_errno) != 0)
        return 91;

    // 保持新映像存活，确保父进程观察到的 owner-death 来自旧 leader。
    char release;
    if (read_all(release_fd, &release, sizeof(release)) != 0 ||
            release != 'G')
        return 92;
    return ok ? 0 : 93;
}

static int run_leader_replacement(int argc, char **argv) {
    int expected_pid;
    int peer_tid;
    int report_fd;
    if (argc != 5 || parse_int(argv[2], &expected_pid) != 0 ||
            parse_int(argv[3], &peer_tid) != 0 ||
            parse_int(argv[4], &report_fd) != 0)
        return 94;

    pid_t observed_pid = getpid();
    pid_t observed_tid = current_tid();
    errno = 0;
    int peer_result = (int) syscall(SYS_tgkill, expected_pid, peer_tid, 0);
    int peer_errno = errno;
    int ok = observed_pid == expected_pid &&
            observed_tid == expected_pid && peer_tid != expected_pid &&
            peer_result == -1 && peer_errno == ESRCH;
    if (send_report(report_fd, ok, observed_pid, observed_tid,
                expected_pid, peer_errno) != 0)
        return 95;
    return ok ? 0 : 96;
}

struct nonleader_context {
    const char *program;
    pid_t expected_pid;
    int report_fd;
    int release_fd;
};

static void *exec_from_nonleader(void *opaque) {
    const struct nonleader_context *context = opaque;
    pid_t old_worker_tid = current_tid();
    char expected_pid[32];
    char old_tid[32];
    char report_fd[32];
    char release_fd[32];
    snprintf(expected_pid, sizeof(expected_pid), "%d", context->expected_pid);
    snprintf(old_tid, sizeof(old_tid), "%d", old_worker_tid);
    snprintf(report_fd, sizeof(report_fd), "%d", context->report_fd);
    snprintf(release_fd, sizeof(release_fd), "%d", context->release_fd);
    execl(context->program, context->program, "--nonleader-replacement",
            expected_pid, old_tid, report_fd, release_fd, (char *) NULL);
    _exit(61);
}

static int init_shared_robust_mutex(pthread_mutex_t *mutex) {
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
        result = pthread_mutex_init(mutex, &attributes);
    (void) pthread_mutexattr_destroy(&attributes);
    if (result != 0)
        errno = result;
    return result == 0 ? 0 : -1;
}

static int test_nonleader_exec(const char *program) {
    pthread_mutex_t *shared_mutex = mmap(NULL, sizeof(*shared_mutex),
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int report_pipe[2];
    int release_pipe[2];
    if (shared_mutex == MAP_FAILED ||
            init_shared_robust_mutex(shared_mutex) != 0 ||
            pipe(report_pipe) != 0 || pipe(release_pipe) != 0)
        return -1;

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(report_pipe[0]);
        close(release_pipe[1]);
        if (pthread_mutex_lock(shared_mutex) != 0)
            _exit(62);
        struct nonleader_context context = {
            .program = program,
            .expected_pid = getpid(),
            .report_fd = report_pipe[1],
            .release_fd = release_pipe[0],
        };
        pthread_t worker;
        if (pthread_create(&worker, NULL, exec_from_nonleader, &context) != 0)
            _exit(63);
        for (;;)
            pause();
    }

    close(report_pipe[1]);
    close(release_pipe[0]);
    active_child = child;
    alarm(12);
    struct exec_report report = {0};
    int report_result = read_all(report_pipe[0], &report, sizeof(report));
    // 新映像仍存活时即可恢复，证明 exec 已完成被清理线程的 robust 收尾。
    int lock_result = report_result == 0 ?
            pthread_mutex_lock(shared_mutex) : EIO;
    int consistent_result = lock_result == EOWNERDEAD ?
            pthread_mutex_consistent(shared_mutex) : EINVAL;
    int unlock_result = consistent_result == 0 ?
            pthread_mutex_unlock(shared_mutex) : EINVAL;
    int release_result = write_all(release_pipe[1], "G", 1);
    close(release_pipe[1]);
    int status = 0;
    int wait_result = wait_for_child(child, &status);
    alarm(0);
    close(report_pipe[0]);
    int destroy_result = consistent_result == 0 ?
            pthread_mutex_destroy(shared_mutex) : EINVAL;
    munmap(shared_mutex, sizeof(*shared_mutex));

    int ok = report_result == 0 && report_is_success(&report, child) &&
            lock_result == EOWNERDEAD && consistent_result == 0 &&
            unlock_result == 0 && release_result == 0 &&
            wait_result == 0 && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0 && destroy_result == 0;
    if (!ok) {
        fprintf(stderr,
                "非 leader exec 详情：report=%d magic=%#x ok=%d pid=%d "
                "tid=%d expected=%d robust=%d/%d/%d status=%#x\n",
                report_result, report.magic, report.ok,
                report.observed_pid, report.observed_tid,
                report.expected_pid, lock_result, consistent_result,
                unlock_result, status);
        errno = EIO;
        return -1;
    }
    return 0;
}

struct blocked_peer_context {
    int ready_fd;
    int block_fd;
};

static void *run_blocked_peer(void *opaque) {
    const struct blocked_peer_context *context = opaque;
    pid_t tid = current_tid();
    if (write_all(context->ready_fd, &tid, sizeof(tid)) != 0)
        _exit(64);
    // 管道让 peer 稳定停留到 exec 提交，不依赖调度时序或固定延迟。
    char marker;
    if (read_all(context->block_fd, &marker, sizeof(marker)) != 0)
        return NULL;
    return NULL;
}

static void run_leader_exec_child(const char *program, int report_fd) {
    int ready_pipe[2];
    int block_pipe[2];
    if (pipe(ready_pipe) != 0 || pipe(block_pipe) != 0)
        _exit(65);
    struct blocked_peer_context context = {
        .ready_fd = ready_pipe[1],
        .block_fd = block_pipe[0],
    };
    pthread_t peer;
    if (pthread_create(&peer, NULL, run_blocked_peer, &context) != 0)
        _exit(66);

    pid_t peer_tid;
    if (read_all(ready_pipe[0], &peer_tid, sizeof(peer_tid)) != 0 ||
            syscall(SYS_tgkill, getpid(), peer_tid, 0) != 0)
        _exit(67);
    char expected_pid[32];
    char old_peer_tid[32];
    char inherited_report_fd[32];
    snprintf(expected_pid, sizeof(expected_pid), "%d", getpid());
    snprintf(old_peer_tid, sizeof(old_peer_tid), "%d", peer_tid);
    snprintf(inherited_report_fd, sizeof(inherited_report_fd), "%d", report_fd);
    execl(program, program, "--leader-replacement", expected_pid,
            old_peer_tid, inherited_report_fd, (char *) NULL);
    _exit(68);
}

static int collect_simple_child(pid_t child, int report_fd,
        struct exec_report *report) {
    active_child = child;
    alarm(12);
    int report_result = read_all(report_fd, report, sizeof(*report));
    int status = 0;
    int wait_result = wait_for_child(child, &status);
    alarm(0);
    if (report_result != 0 || wait_result != 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int test_leader_exec(const char *program) {
    int report_pipe[2];
    if (pipe(report_pipe) != 0)
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(report_pipe[0]);
        run_leader_exec_child(program, report_pipe[1]);
    }

    close(report_pipe[1]);
    struct exec_report report = {0};
    int collect_result = collect_simple_child(child, report_pipe[0], &report);
    close(report_pipe[0]);
    if (collect_result != 0 || !report_is_success(&report, child)) {
        fprintf(stderr,
                "leader exec 详情：collect=%d magic=%#x ok=%d pid=%d "
                "tid=%d expected=%d peer_errno=%d\n",
                collect_result, report.magic, report.ok,
                report.observed_pid, report.observed_tid,
                report.expected_pid, report.detail);
        errno = EIO;
        return -1;
    }
    return 0;
}

struct failed_exec_context {
    const char *bad_elf;
    int cloexec_fd;
    pid_t pid_before;
    pid_t tid_before;
    pid_t tid_after;
    int missing_errno;
    int bad_elf_errno;
    int state_ok;
};

static int failed_exec_state_is_preserved(
        const struct failed_exec_context *context) {
    struct sigaction observed;
    int flags = fcntl(context->cloexec_fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0 &&
            sigaction(SIGUSR1, NULL, &observed) == 0 &&
            observed.sa_handler == preserved_handler &&
            preserved_cookie == 0x3947;
}

static void *run_failed_exec(void *opaque) {
    struct failed_exec_context *context = opaque;
    context->pid_before = getpid();
    context->tid_before = current_tid();

    execl("/tmp/i386-thread-exec-file-that-does-not-exist",
            "missing", (char *) NULL);
    context->missing_errno = errno;
    int state_after_missing = failed_exec_state_is_preserved(context);

    // 已打开但格式无效的映像覆盖文件解析后的失败边界。
    execl(context->bad_elf, context->bad_elf, (char *) NULL);
    context->bad_elf_errno = errno;
    context->tid_after = current_tid();
    context->state_ok = state_after_missing &&
            failed_exec_state_is_preserved(context) &&
            getpid() == context->pid_before &&
            context->tid_after == context->tid_before;
    return NULL;
}

static void run_failed_exec_child(int report_fd) {
    static const char invalid_image[] = "这不是 ELF 映像\n";
    char bad_elf[] = "/tmp/i386-thread-exec-XXXXXX";
    int bad_fd = mkstemp(bad_elf);
    int cloexec_fd = open("/dev/null", O_RDONLY);
    struct sigaction action = {
        .sa_handler = preserved_handler,
    };
    sigemptyset(&action.sa_mask);
    if (bad_fd < 0 || cloexec_fd < 0 ||
            write_all(bad_fd, invalid_image, sizeof(invalid_image) - 1) != 0 ||
            fchmod(bad_fd, 0700) != 0 || close(bad_fd) != 0 ||
            fcntl(cloexec_fd, F_SETFD, FD_CLOEXEC) != 0 ||
            sigaction(SIGUSR1, &action, NULL) != 0)
        _exit(69);

    struct failed_exec_context context = {
        .bad_elf = bad_elf,
        .cloexec_fd = cloexec_fd,
    };
    pthread_t worker;
    int create_result = pthread_create(
            &worker, NULL, run_failed_exec, &context);
    int join_result = create_result == 0 ? pthread_join(worker, NULL) : EINVAL;
    pid_t leader_pid = getpid();
    pid_t leader_tid = current_tid();
    int ok = create_result == 0 && join_result == 0 &&
            context.missing_errno == ENOENT &&
            context.bad_elf_errno == ENOEXEC && context.state_ok &&
            context.tid_before != leader_tid &&
            leader_pid == leader_tid && leader_pid == context.pid_before;
    int detail = (context.missing_errno & 0xffff) |
            ((context.bad_elf_errno & 0xffff) << 16);
    (void) unlink(bad_elf);
    (void) close(cloexec_fd);
    if (send_report(report_fd, ok, leader_pid, leader_tid,
                context.pid_before, detail) != 0)
        _exit(70);
    _exit(ok ? 0 : 71);
}

static int test_failed_exec(void) {
    int report_pipe[2];
    if (pipe(report_pipe) != 0)
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(report_pipe[0]);
        run_failed_exec_child(report_pipe[1]);
    }

    close(report_pipe[1]);
    struct exec_report report = {0};
    int collect_result = collect_simple_child(child, report_pipe[0], &report);
    close(report_pipe[0]);
    if (collect_result != 0 || !report_is_success(&report, child)) {
        fprintf(stderr,
                "失败 exec 详情：collect=%d magic=%#x ok=%d pid=%d "
                "tid=%d expected=%d errno_pair=%#x\n",
                collect_result, report.magic, report.ok,
                report.observed_pid, report.observed_tid,
                report.expected_pid, report.detail);
        errno = EIO;
        return -1;
    }
    return 0;
}

static int create_post_commit_failure_image(char *path) {
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;

    Elf32_Ehdr header = {0};
    memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS32;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC;
    header.e_machine = EM_386;
    header.e_version = EV_CURRENT;
    header.e_entry = UINT32_C(0x08048000);
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf32_Phdr);
    header.e_phnum = 1;
    const Elf32_Phdr segment = {
        .p_type = PT_LOAD,
        .p_vaddr = UINT32_C(0x08048000),
        .p_flags = PF_R | PF_X,
        .p_align = UINT32_C(0x1000),
        // 零长度 PT_LOAD 通过格式识别，但提交新映像后的 mmap 必须失败。
    };
    int result = write_all(fd, &header, sizeof(header));
    if (result == 0)
        result = write_all(fd, &segment, sizeof(segment));
    if (result == 0 && fchmod(fd, 0700) != 0)
        result = -1;
    if (close(fd) != 0)
        result = -1;
    return result;
}

static void *run_post_commit_failure(void *opaque) {
    const char *path = opaque;
    execl(path, path, (char *) NULL);
    _exit(72);
}

static int test_post_commit_failure_is_fatal(void) {
    char path[] = "/tmp/i386-thread-exec-commit-XXXXXX";
    if (create_post_commit_failure_image(path) != 0)
        return -1;

    pid_t child = fork();
    if (child < 0) {
        unlink(path);
        return -1;
    }
    if (child == 0) {
        pthread_t worker;
        if (pthread_create(&worker, NULL,
                    run_post_commit_failure, path) != 0)
            _exit(73);
        for (;;)
            pause();
    }

    active_child = child;
    alarm(12);
    int status = 0;
    int wait_result = wait_for_child(child, &status);
    alarm(0);
    unlink(path);
    if (wait_result != 0 || !WIFSIGNALED(status) ||
            WTERMSIG(status) != SIGSEGV) {
        fprintf(stderr,
                "提交后失败详情：wait=%d status=%#x signal=%d\n",
                wait_result, status,
                WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        errno = EIO;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--nonleader-replacement") == 0)
        return run_nonleader_replacement(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--leader-replacement") == 0)
        return run_leader_replacement(argc, argv);

    struct sigaction timeout_action = {
        .sa_handler = timeout_handler,
    };
    struct sigaction ignore_action = {
        .sa_handler = SIG_IGN,
    };
    sigemptyset(&timeout_action.sa_mask);
    sigemptyset(&ignore_action.sa_mask);
    if (sigaction(SIGALRM, &timeout_action, NULL) != 0 ||
            sigaction(SIGPIPE, &ignore_action, NULL) != 0) {
        fprintf(stderr, "i386 多线程 exec 测试失败：安装信号处理器\n");
        return 1;
    }
    if (test_nonleader_exec(argv[0]) != 0) {
        fprintf(stderr, "i386 多线程 exec 测试失败：非 leader 身份接管\n");
        return 1;
    }
    if (test_leader_exec(argv[0]) != 0) {
        fprintf(stderr, "i386 多线程 exec 测试失败：leader 清理 peer\n");
        return 1;
    }
    if (test_failed_exec() != 0) {
        fprintf(stderr, "i386 多线程 exec 测试失败：失败路径保持旧状态\n");
        return 1;
    }
    if (test_post_commit_failure_is_fatal() != 0) {
        fprintf(stderr, "i386 多线程 exec 测试失败：提交后错误必须致死\n");
        return 1;
    }
    puts("i386 多线程 exec：通过");
    return 0;
}

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/sock.h"
#include "fs/sockrestart.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define TEST_TIMEOUT_SECONDS 20
#define STEP_TIMEOUT_SECONDS 3
#define POLL_WAIT_TIMEOUT_SECONDS 30
#define HEALTHY_TOKEN UINT64_C(0x6865616c746879)
#define BROKEN_TOKEN UINT64_C(0x62726f6b656e)
#define TAIL_TOKEN UINT64_C(0x7461696c)
#define BROKEN_MOD_TOKEN UINT64_C(0x6d6f646966696564)
#define BROKEN_ADD_TOKEN UINT64_C(0x72656164646564)

struct fixture {
    struct task task;
    struct tgroup group;
};

struct poll_call {
    struct task *task;
    struct poll *poll;
    int result;
    int types;
    uint64_t token;
};

static int failures;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "socket 重登记失败测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        EXPECT(false, message); \
        goto cleanup; \
    } \
} while (0)

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] =
            "socket 重登记失败测试失败：超过硬超时\n";
    (void) write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

static void wake_handler(int signal_number) {
    (void) signal_number;
}

static int open_host_fds(void) {
    int count = 0;
    int limit = getdtablesize();
    for (int number = 0; number < limit; number++) {
        errno = 0;
        if (fcntl(number, F_GETFD) >= 0 || errno != EBADF)
            count++;
    }
    return count;
}

static bool fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    list_init(&fixture->group.threads);
    signal_group_pending_init(&fixture->group);
    fixture->group.limits[RLIMIT_NOFILE_] =
            (struct rlimit_) {32, 32};

    fixture->task.group = &fixture->group;
    fixture->task.sighand = sighand_new();
    if (fixture->task.sighand == NULL)
        return false;
    fixture->task.files = fdtable_new(4);
    if (IS_ERR(fixture->task.files)) {
        fixture->task.files = NULL;
        return false;
    }
    list_init(&fixture->task.queue);
    list_init(&fixture->task.sockrestart.listen);
    lock_init(&fixture->task.waiting_cond_lock);
    fixture->task.waiting_poll_notify_fd = -1;
    task_thread_store(&fixture->task, pthread_self());
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL)
        fdtable_release(fixture->task.files);
    if (fixture->task.sighand != NULL)
        sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static fd_t create_listener(struct fixture *fixture) {
    fd_t number = socket_create_task(&fixture->task,
            AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    struct fd *listener = f_get_task(&fixture->task, number);
    if (number < 0 || listener == NULL)
        return -1;

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(listener->real_fd,
                (const struct sockaddr *) &address,
                sizeof(address)) < 0 ||
            sys_listen(number, 4) < 0)
        return -1;
    return number;
}

static bool timespec_before(
        const struct timespec *left, const struct timespec *right) {
    return left->tv_sec < right->tv_sec ||
            (left->tv_sec == right->tv_sec &&
            left->tv_nsec < right->tv_nsec);
}

static struct timespec wait_deadline(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += STEP_TIMEOUT_SECONDS;
    return deadline;
}

static bool wait_until_healthy_rearmed(struct poll *poll) {
    struct timespec deadline = wait_deadline();
    for (;;) {
        lock(&poll->lock);
        bool rearmed = !list_empty(&poll->poll_fds) &&
                list_first_entry(&poll->poll_fds,
                        struct poll_fd, fds)->triggered_types == 0;
        unlock(&poll->lock);
        if (rearmed)
            return true;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!timespec_before(&now, &deadline))
            return false;
        sched_yield();
    }
}

static bool wait_until_poll_blocked(struct poll *poll) {
    struct timespec deadline = wait_deadline();
    for (;;) {
        lock(&poll->lock);
        bool blocked = poll->waiters == 1 &&
                poll->notify_pipe[0] >= 0 &&
                poll->notify_pipe[1] >= 0;
        unlock(&poll->lock);
        if (blocked)
            return true;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!timespec_before(&now, &deadline))
            return false;
        sched_yield();
    }
}

static int capture_event(
        void *opaque, int types, union poll_fd_info info) {
    struct poll_call *call = opaque;
    call->types = types;
    call->token = info.num;
    return 1;
}

static void *run_poll_wait(void *opaque) {
    struct poll_call *call = opaque;
    current = call->task;
    task_thread_store(call->task, pthread_self());
    // 长于进程硬超时，确保恢复失败必须主动唤醒既有等待者。
    struct timespec timeout = {.tv_sec = POLL_WAIT_TIMEOUT_SECONDS};
    call->result = poll_wait(
            call->poll, capture_event, call, &timeout);
    current = NULL;
    return NULL;
}

static void *run_resume(void *opaque) {
    (void) opaque;
    sockrestart_on_resume();
    return NULL;
}

static bool status_identity_equal(
        const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev &&
            left->st_ino == right->st_ino &&
            (left->st_mode & S_IFMT) == (right->st_mode & S_IFMT);
}

static bool expect_immediate_failure_event(
        struct task *task, struct poll *poll, uint64_t token,
        const char *message) {
    struct poll_call call = {
        .task = task,
        .poll = poll,
        .result = -1,
    };
    struct timespec immediate = {0};
    call.result = poll_wait(
            poll, capture_event, &call, &immediate);
    bool matches = call.result == 1 && call.token == token &&
            (call.types & (POLL_ERR | POLL_HUP)) ==
                    (POLL_ERR | POLL_HUP);
    if (!matches)
        fprintf(stderr,
                "socket 重登记失败测试诊断：result=%d types=%#x "
                "token=%#llx expected=%#llx\n",
                call.result, call.types,
                (unsigned long long) call.token,
                (unsigned long long) token);
    EXPECT(matches, message);
    return matches;
}

static int test_partial_rearm_failure(void) {
    int previous_failures = failures;
    int baseline = open_host_fds();
    struct fixture fixture;
    bool fixture_ready = false;
    fd_t listener_number = -1;
    struct fd *listener = NULL;
    struct poll *broken_poll = NULL;
    struct poll *healthy_poll = NULL;
    struct poll *tail_poll = NULL;
    bool broken_poll_locked = false;
    bool snapshot_pending = false;
    pthread_t resume_thread = {};
    bool resume_started = false;
    bool resume_joined = false;
    pthread_t wait_thread = {};
    bool wait_started = false;
    bool wait_joined = false;
    int target_fd = -1;
    int sentinel_pair[2] = {-1, -1};
    bool sentinel_installed = false;
    struct stat sentinel_status = {0};
    struct poll_call wait_call = {
        .result = -1,
    };

    REQUIRE(fixture_init(&fixture), "初始化监听恢复失败夹具");
    fixture_ready = true;
    listener_number = create_listener(&fixture);
    listener = f_get_task(&fixture.task, listener_number);
    REQUIRE(listener_number >= 0 && listener != NULL,
            "创建待恢复的非阻塞 INET listener");

    uint64_t initial_generation;
    lock(&listener->lock);
    initial_generation = listener->socket.listen_generation;
    unlock(&listener->lock);
    REQUIRE(initial_generation != 0, "listener 已发布非零代次");

    broken_poll = poll_create();
    if (IS_ERR(broken_poll))
        broken_poll = NULL;
    healthy_poll = poll_create();
    if (IS_ERR(healthy_poll))
        healthy_poll = NULL;
    tail_poll = poll_create();
    if (IS_ERR(tail_poll))
        tail_poll = NULL;
    REQUIRE(broken_poll != NULL && healthy_poll != NULL &&
                    tail_poll != NULL,
            "创建成功、失败与尾部三个 poll 后端");

    // fd 的登记链头插；恢复顺序固定为 healthy、broken、tail。
    REQUIRE(poll_add_fd(tail_poll, listener, POLL_READ,
                    (union poll_fd_info) {.num = TAIL_TOKEN}) == 0 &&
                    poll_add_fd(broken_poll, listener, POLL_READ,
                    (union poll_fd_info) {.num = BROKEN_TOKEN}) == 0 &&
                    poll_add_fd(healthy_poll, listener, POLL_READ,
                    (union poll_fd_info) {.num = HEALTHY_TOKEN}) == 0,
            "同一 listener 登记到三个 poll 后端");
    lock(&listener->poll_lock);
    struct poll_fd *first_entry = list_first_entry(
            &listener->poll_fds, struct poll_fd, polls);
    struct poll_fd *second_entry = list_next_entry(first_entry, polls);
    struct poll_fd *third_entry = list_next_entry(second_entry, polls);
    bool expected_order = list_size(&listener->poll_fds) == 3 &&
            first_entry->poll == healthy_poll &&
            second_entry->poll == broken_poll &&
            third_entry->poll == tail_poll;
    unlock(&listener->poll_lock);
    REQUIRE(expected_order, "三个后端按预定顺序重登记");

    lock(&healthy_poll->lock);
    struct poll_fd *healthy_entry = list_first_entry(
            &healthy_poll->poll_fds, struct poll_fd, fds);
    healthy_entry->triggered_types = POLL_READ;
    unlock(&healthy_poll->lock);
    lock(&tail_poll->lock);
    struct poll_fd *tail_entry = list_first_entry(
            &tail_poll->poll_fds, struct poll_fd, fds);
    tail_entry->triggered_types = POLL_READ;
    unlock(&tail_poll->lock);

    sockrestart_on_suspend();
    snapshot_pending = true;
    target_fd = listener->real_fd;
    REQUIRE(close(target_fd) == 0, "模拟挂起后宿主 listener 失效");

    lock(&broken_poll->lock);
    broken_poll_locked = true;
    REQUIRE(close(broken_poll->real.fd) == 0,
            "关闭第二个 poll 的宿主后端以注入重登记失败");
    broken_poll->real.fd = -1;

    REQUIRE(pthread_create(&resume_thread, NULL,
                    run_resume, NULL) == 0,
            "启动锁外 listener 恢复线程");
    resume_started = true;
    snapshot_pending = false;
    REQUIRE(wait_until_healthy_rearmed(healthy_poll),
            "首个 poll 已成功重登记并在失败锁前停住");

    struct stat restored_status;
    REQUIRE(fstat(target_fd, &restored_status) == 0 &&
                    S_ISSOCK(restored_status.st_mode),
            "listener 已在原 raw fd 号完成恢复");
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sentinel_pair) == 0 &&
                    sentinel_pair[0] != target_fd &&
                    sentinel_pair[1] != target_fd &&
                    fstat(sentinel_pair[0], &sentinel_status) == 0 &&
                    dup2(sentinel_pair[0], target_fd) == target_fd,
            "在重登记部分成功窗口安装外来 sentinel");
    sentinel_installed = true;

    wait_call = (struct poll_call) {
        .task = &fixture.task,
        .poll = healthy_poll,
        .result = -1,
    };
    REQUIRE(pthread_create(&wait_thread, NULL,
                    run_poll_wait, &wait_call) == 0,
            "启动等待失败关闭通知的 poll 线程");
    wait_started = true;
    REQUIRE(wait_until_poll_blocked(healthy_poll),
            "poll 等待者已完成首次扫描并进入宿主等待");

    unlock(&broken_poll->lock);
    broken_poll_locked = false;
    REQUIRE(pthread_join(resume_thread, NULL) == 0,
            "重登记失败后恢复线程及时退出");
    resume_joined = true;
    REQUIRE(pthread_join(wait_thread, NULL) == 0,
            "ERR/HUP 通知唤醒阻塞 poll 线程");
    wait_joined = true;
    task_thread_store(&fixture.task, pthread_self());
    current = &fixture.task;

    EXPECT(wait_call.result == 1 &&
                    wait_call.token == HEALTHY_TOKEN &&
                    (wait_call.types & (POLL_ERR | POLL_HUP)) ==
                            (POLL_ERR | POLL_HUP),
            "部分重登记失败向既有等待者投递 ERR/HUP");

    lock(&listener->lock);
    bool failed_closed = listener->real_fd == -1 &&
            !listener->socket.host_listening &&
            !listener->socket.guest_listening &&
            listener->socket.listen_generation != initial_generation;
    unlock(&listener->lock);
    EXPECT(failed_closed,
            "重登记失败把 listener 转为可观察的失败关闭状态");
    lock(&tail_poll->lock);
    bool tail_skipped = tail_entry->triggered_types == POLL_READ;
    unlock(&tail_poll->lock);
    EXPECT(tail_skipped,
            "首个重登记错误后不再把外来 fd 注册到尾部后端");

    struct stat target_status;
    EXPECT(fstat(target_fd, &target_status) == 0 &&
                    status_identity_equal(
                            &target_status, &sentinel_status),
            "失败回滚没有关闭或替换外来 sentinel");
    (void) expect_immediate_failure_event(&fixture.task,
            healthy_poll, HEALTHY_TOKEN,
            "有效宿主后端上的失败 socket 持续返回 ERR/HUP");

    EXPECT(poll_mod_fd(broken_poll, listener, POLL_WRITE,
                    (union poll_fd_info) {
                        .num = BROKEN_MOD_TOKEN,
                    }) == 0,
            "失败 socket 的 MOD 不访问失效宿主后端");
    lock(&broken_poll->lock);
    struct poll_fd *broken_entry = list_first_entry(
            &broken_poll->poll_fds, struct poll_fd, fds);
    bool modified = broken_entry->enabled &&
            broken_entry->types == POLL_WRITE &&
            broken_entry->info.num == BROKEN_MOD_TOKEN;
    unlock(&broken_poll->lock);
    EXPECT(modified,
            "失败 socket 的 MOD 更新逻辑事件与 token");
    EXPECT(poll_del_fd(broken_poll, listener) == 0 &&
                    !poll_has_fd(broken_poll, listener),
            "失败 socket 的 DEL 不访问失效宿主后端");
    EXPECT(poll_add_fd(broken_poll, listener, POLL_READ,
                    (union poll_fd_info) {
                        .num = BROKEN_ADD_TOKEN,
                    }) == 0,
            "失败 socket 的 ADD 不访问失效宿主后端");
    lock(&broken_poll->lock);
    broken_entry = list_first_entry(
            &broken_poll->poll_fds, struct poll_fd, fds);
    bool added = broken_entry->enabled &&
            broken_entry->types == POLL_READ &&
            broken_entry->info.num == BROKEN_ADD_TOKEN;
    unlock(&broken_poll->lock);
    EXPECT(added,
            "失败 socket 的 ADD 建立完整逻辑登记");

    EXPECT(f_close_task(&fixture.task, listener_number) == 0,
            "最终 CLOSE 清理失败 socket");
    listener_number = -1;
    listener = NULL;
    EXPECT(list_empty(&healthy_poll->poll_fds) &&
                    list_empty(&broken_poll->poll_fds) &&
                    list_empty(&tail_poll->poll_fds),
            "最终 CLOSE 从三个 poll 后端清除全部逻辑登记");
    EXPECT(fstat(target_fd, &target_status) == 0 &&
                    status_identity_equal(
                            &target_status, &sentinel_status),
            "最终 CLOSE 仍不关闭外来 sentinel");

cleanup:
    if (broken_poll_locked) {
        unlock(&broken_poll->lock);
    }
    if (resume_started && !resume_joined) {
        (void) pthread_join(resume_thread, NULL);
        snapshot_pending = false;
    }
    if (wait_started && !wait_joined) {
        (void) pthread_join(wait_thread, NULL);
    }
    if (fixture_ready) {
        task_thread_store(&fixture.task, pthread_self());
        current = &fixture.task;
    }
    if (snapshot_pending)
        sockrestart_on_resume();
    if (fixture_ready && listener_number >= 0)
        (void) f_close_task(&fixture.task, listener_number);
    if (healthy_poll != NULL)
        poll_destroy(healthy_poll);
    if (broken_poll != NULL)
        poll_destroy(broken_poll);
    if (tail_poll != NULL)
        poll_destroy(tail_poll);
    if (sentinel_installed && target_fd >= 0 &&
            fcntl(target_fd, F_GETFD) >= 0)
        (void) close(target_fd);
    if (sentinel_pair[0] >= 0)
        (void) close(sentinel_pair[0]);
    if (sentinel_pair[1] >= 0)
        (void) close(sentinel_pair[1]);
    if (fixture_ready)
        fixture_destroy(&fixture);
    EXPECT(open_host_fds() == baseline,
            "测试结束后宿主 fd 数量回到基线");
    return failures == previous_failures ? 0 : 1;
}

int main(void) {
    struct sigaction timeout_action = {
        .sa_handler = timeout_handler,
    };
    sigemptyset(&timeout_action.sa_mask);
    sigaction(SIGALRM, &timeout_action, NULL);
    struct sigaction wake_action = {
        .sa_handler = wake_handler,
    };
    sigemptyset(&wake_action.sa_mask);
    sigaction(SIGUSR1, &wake_action, NULL);
    alarm(TEST_TIMEOUT_SECONDS);

    (void) test_partial_rearm_failure();
    int result = failures == 0 ? 0 : 1;
    alarm(0);
    return result;
}

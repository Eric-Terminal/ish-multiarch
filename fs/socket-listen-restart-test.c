#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

#define TEST_TIMEOUT_SECONDS 30
#define POLL_WAIT_TIMEOUT_SECONDS 2
#define LISTENER_EVENT_TOKEN UINT64_C(0x6c697374656e6572)

static int failures;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, \
                "socket 监听恢复测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

struct fixture {
    struct task task;
    struct tgroup group;
};

static bool test_filesystem_mounted;

struct guest_unix_address {
    struct sockaddr_storage storage;
    size_t length;
};

struct scan_gate {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool entered;
    bool released;
};

struct poll_wait_call {
    struct task *task;
    struct poll *poll;
    int result;
    int event_types;
    uint64_t event_token;
};

struct late_punt_hook {
    atomic_uint calls;
};

static void timeout_handler(int signal_number) {
    (void) signal_number;
    static const char message[] =
            "socket 监听恢复测试失败：超过硬超时\n";
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
            (struct rlimit_) {16, 16};

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
    if (!test_filesystem_mounted) {
        lock(&mounts_lock);
        int mount_error = do_mount(&tmpfs, "", "", "", 0);
        unlock(&mounts_lock);
        if (mount_error < 0)
            return false;
        test_filesystem_mounted = true;
    }
    fixture->task.fs = fs_info_new();
    return fixture->task.fs != NULL;
}

static void fixture_destroy(struct fixture *fixture) {
    current = &fixture->task;
    if (fixture->task.files != NULL)
        fdtable_release(fixture->task.files);
    if (fixture->task.fs != NULL)
        fs_info_release(fixture->task.fs);
    if (fixture->task.sighand != NULL)
        sighand_release(fixture->task.sighand);
    pthread_mutex_destroy(&fixture->task.waiting_cond_lock.m);
    pthread_mutex_destroy(&fixture->group.lock.m);
    current = NULL;
}

static void scan_gate_init(struct scan_gate *gate) {
    memset(gate, 0, sizeof(*gate));
    pthread_mutex_init(&gate->lock, NULL);
    pthread_cond_init(&gate->cond, NULL);
}

static void scan_gate_release(struct scan_gate *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->released = true;
    pthread_cond_broadcast(&gate->cond);
    pthread_mutex_unlock(&gate->lock);
}

static bool scan_gate_wait_until_entered(struct scan_gate *gate) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += POLL_WAIT_TIMEOUT_SECONDS;

    pthread_mutex_lock(&gate->lock);
    int error = 0;
    while (!gate->entered && error == 0)
        error = pthread_cond_timedwait(
                &gate->cond, &gate->lock, &deadline);
    bool entered = gate->entered;
    pthread_mutex_unlock(&gate->lock);
    return entered;
}

static void scan_gate_destroy(struct scan_gate *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->lock);
}

static int scan_gate_poll(struct fd *fd) {
    struct scan_gate *gate = fd->data;
    pthread_mutex_lock(&gate->lock);
    gate->entered = true;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->released)
        pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
    return 0;
}

static const struct fd_ops scan_gate_ops = {
    .poll = scan_gate_poll,
};

static int capture_poll_event(
        void *opaque, int types, union poll_fd_info info) {
    struct poll_wait_call *call = opaque;
    call->event_types = types;
    call->event_token = info.num;
    return info.num == LISTENER_EVENT_TOKEN ? 1 : 0;
}

static void *run_poll_wait(void *opaque) {
    struct poll_wait_call *call = opaque;
    current = call->task;
    task_thread_store(call->task, pthread_self());
    struct timespec timeout = {
        .tv_sec = POLL_WAIT_TIMEOUT_SECONDS,
    };
    call->result = poll_wait(call->poll,
            capture_poll_event, call, &timeout);
    current = NULL;
    return NULL;
}

static void inject_late_punt(void *opaque) {
    struct late_punt_hook *hook = opaque;
    atomic_fetch_add_explicit(
            &hook->calls, 1, memory_order_relaxed);
    // poll 已完成首次消费但尚未注销 task；resume 会把 punt 精确写进窗口。
    sockrestart_on_resume();
}

static bool listener_state_matches(struct fd *listener,
        dword_t backlog, uint64_t generation) {
    lock(&listener->lock);
    bool matches = listener->socket.host_listening &&
            listener->socket.guest_listening &&
            listener->socket.listen_backlog == backlog &&
            listener->socket.listen_generation == generation;
    unlock(&listener->lock);
    return matches;
}

static bool wait_listener_ready(int raw_fd) {
    struct pollfd event = {
        .fd = raw_fd,
        .events = POLLIN,
    };
    int result;
    do {
        result = poll(&event, 1, 2000);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (event.revents & POLLIN) != 0;
}

static fd_t create_inet_listener(struct fixture *fixture,
        struct sockaddr_in *address, dword_t backlog) {
    fd_t number = socket_create_task(&fixture->task,
            AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    struct fd *listener = f_get_task(&fixture->task, number);
    if (number < 0 || listener == NULL)
        return -1;
    *address = (struct sockaddr_in) {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(listener->real_fd,
                (const struct sockaddr *) address,
                sizeof(*address)) < 0)
        return -1;
    socklen_t address_length = sizeof(*address);
    if (getsockname(listener->real_fd,
                (struct sockaddr *) address, &address_length) < 0 ||
            sys_listen(number, (int) backlog) < 0)
        return -1;
    return number;
}

static struct guest_unix_address guest_unix_address(
        const void *name, size_t name_length) {
    struct guest_unix_address address = {0};
    struct sockaddr_ *wire = (struct sockaddr_ *) &address.storage;
    wire->family = AF_LOCAL_;
    EXPECT(name_length <= SOCKADDR_DATA_MAX,
            "Unix guest 地址长度不超过 ABI 上限");
    if (name_length <= SOCKADDR_DATA_MAX)
        memcpy((byte_t *) &address.storage +
                offsetof(struct sockaddr_, data), name, name_length);
    address.length = offsetof(struct sockaddr_, data) + name_length;
    return address;
}

static fd_t create_unix_listener(struct fixture *fixture,
        const struct guest_unix_address *address, dword_t backlog) {
    fd_t number = socket_create_task(&fixture->task,
            AF_LOCAL_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    struct socket_ref listener = {0};
    int error = socket_ref_get_task(&fixture->task, number, &listener);
    if (number < 0 || error < 0)
        return -1;
    error = socket_bind_ref_task(&fixture->task, &listener,
            &address->storage, address->length);
    socket_ref_release(&listener);
    if (error < 0 || sys_listen(number, (int) backlog) < 0)
        return -1;
    return number;
}

static fd_t connect_unix_listener(struct fixture *fixture,
        const struct guest_unix_address *address) {
    fd_t number = socket_create_task(
            &fixture->task, AF_LOCAL_, SOCK_STREAM_, 0);
    struct socket_ref connector = {0};
    int error = socket_ref_get_task(&fixture->task, number, &connector);
    if (number < 0 || error < 0)
        return -1;
    error = socket_connect_ref_task(&fixture->task, &connector,
            &address->storage, address->length);
    socket_ref_release(&connector);
    return error == 0 ? number : -1;
}

static bool unix_backing_identity_matches(struct fd *listener) {
    struct stat status;
    lock(&listener->lock);
    bool owned = listener->socket.unix_backing_owned;
    uint64_t device = listener->socket.unix_backing_device;
    uint64_t inode = listener->socket.unix_backing_inode;
    char path[sizeof(listener->socket.unix_backing_path)];
    memcpy(path, listener->socket.unix_backing_path, sizeof(path));
    unlock(&listener->lock);
    return owned && lstat(path, &status) == 0 &&
            S_ISSOCK(status.st_mode) &&
            (uint64_t) status.st_dev == device &&
            (uint64_t) status.st_ino == inode;
}

static bool listener_is_failed_closed(
        struct fd *listener, uint64_t old_generation) {
    lock(&listener->lock);
    bool failed = listener->real_fd == -1 &&
            !listener->socket.host_listening &&
            !listener->socket.guest_listening &&
            listener->socket.listen_generation != old_generation &&
            list_null(&listener->sockrestart.listen);
    unlock(&listener->lock);
    return failed;
}

static int test_listen_restart(void) {
    int baseline = open_host_fds();
    struct fixture fixture;
    bool fixture_ready = false;
    struct poll *poll = NULL;
    struct fd *gate_fd = NULL;
    struct scan_gate gate;
    bool gate_ready = false;
    pthread_t waiter = {};
    bool waiter_started = false;
    bool waiter_joined = false;
    bool late_punt_hook_installed = false;
    bool snapshot_pending = false;
    int client = -1;
    int punt_client = -1;
    fd_t accepted = -1;
    if (!fixture_init(&fixture)) {
        fprintf(stderr, "socket 监听恢复测试失败：夹具初始化失败\n");
        failures++;
        goto cleanup;
    }
    fixture_ready = true;
    fd_t listener_number = socket_create_task(&fixture.task,
            AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    struct fd *listener = f_get_task(&fixture.task, listener_number);
    if (listener_number != 0 || listener == NULL) {
        fprintf(stderr,
                "socket 监听恢复测试失败：创建非阻塞监听 socket\n");
        failures++;
        goto cleanup;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(listener->real_fd,
                (const struct sockaddr *) &address,
                sizeof(address)) < 0) {
        fprintf(stderr,
                "socket 监听恢复测试失败：绑定环回地址：%s\n",
                strerror(errno));
        failures++;
        goto cleanup;
    }
    socklen_t address_length = sizeof(address);
    if (getsockname(listener->real_fd,
                (struct sockaddr *) &address,
                &address_length) < 0) {
        fprintf(stderr,
                "socket 监听恢复测试失败：取得监听地址：%s\n",
                strerror(errno));
        failures++;
        goto cleanup;
    }
    EXPECT(sys_listen(listener_number, 2) == 0,
            "首次 listen 成功");
    uint64_t generation;
    lock(&listener->lock);
    generation = listener->socket.listen_generation;
    bool initial_state = listener->socket.host_listening &&
            listener->socket.guest_listening &&
            listener->socket.listen_backlog == 2 && generation != 0;
    unlock(&listener->lock);
    EXPECT(initial_state,
            "首次 listen 保存 backlog、guest/host 状态与非零代次");
    EXPECT(sys_listen(listener_number, 9) == 0,
            "重复 listen 成功更新 backlog");
    EXPECT(listener_state_matches(listener, 9, generation),
            "重复 listen 保留代次并更新完整监听状态");
    EXPECT(!list_null(&listener->sockrestart.listen) &&
                    listener->sockrestart.listen.next !=
                            &listener->sockrestart.listen &&
                    listener->sockrestart.listen.prev !=
                            &listener->sockrestart.listen,
            "重复 listen 未把同一个恢复登记节点插入自身");
    poll = poll_create();
    if (IS_ERR(poll)) {
        poll = NULL;
        fprintf(stderr,
                "socket 监听恢复测试失败：创建 poll 后端\n");
        failures++;
        goto cleanup;
    }
    scan_gate_init(&gate);
    gate_ready = true;
    gate_fd = fd_create(&scan_gate_ops);
    if (gate_fd == NULL) {
        fprintf(stderr,
                "socket 监听恢复测试失败：创建 poll 扫描闸门\n");
        failures++;
        goto cleanup;
    }
    gate_fd->data = &gate;
    // poll 头插登记；先放闸门、后放监听器，确保首次扫描先检查监听器。
    EXPECT(poll_add_fd(poll, gate_fd, POLL_READ,
                    (union poll_fd_info) {.num = 0}) == 0,
            "登记 poll 扫描闸门");
    EXPECT(poll_add_fd(poll, listener, POLL_READ,
                    (union poll_fd_info) {
                        .num = LISTENER_EVENT_TOKEN,
                    }) == 0,
            "在替换前登记监听 socket 的可读事件");
    if (failures != 0)
        goto cleanup;
    EXPECT(fd_refcount_read(listener) == 1,
            "poll 登记不改变监听 fd 的表项所有权");
    sockrestart_on_suspend();
    snapshot_pending = true;
    EXPECT(fd_refcount_read(listener) == 2,
            "重复 listen 在挂起时只产生一个监听快照引用");
    int old_raw_fd = listener->real_fd;
    if (close(old_raw_fd) < 0) {
        fprintf(stderr,
                "socket 监听恢复测试失败：模拟宿主释放 raw fd：%s\n",
                strerror(errno));
        failures++;
        goto cleanup;
    }
    errno = 0;
    EXPECT(fcntl(old_raw_fd, F_GETFD) < 0 && errno == EBADF,
            "挂起后的旧 raw listener 已真实失效");
    sockrestart_on_resume();
    snapshot_pending = false;
    EXPECT(fd_refcount_read(listener) == 1,
            "恢复完成后释放唯一监听快照引用");
    EXPECT(listener_state_matches(listener, 9, generation),
            "恢复不改变 guest/host 状态、backlog 与监听代次");
    int restored_flags = fcntl(listener->real_fd, F_GETFL);
    EXPECT(restored_flags >= 0 && (restored_flags & O_NONBLOCK) != 0,
            "replacement 保持宿主 O_NONBLOCK");
    struct sockaddr_in restored_address = {0};
    socklen_t restored_address_length = sizeof(restored_address);
    EXPECT(getsockname(listener->real_fd,
                    (struct sockaddr *) &restored_address,
                    &restored_address_length) == 0 &&
                    restored_address.sin_family == AF_INET &&
                    restored_address.sin_port == address.sin_port &&
                    restored_address.sin_addr.s_addr ==
                            address.sin_addr.s_addr,
            "replacement 已重新绑定原有环回地址");
    struct poll_wait_call wait_call = {
        .task = &fixture.task,
        .poll = poll,
        .result = -1,
    };
    struct late_punt_hook late_punt = {};
    poll_set_listen_wait_exit_test_hook(
            inject_late_punt, &late_punt);
    late_punt_hook_installed = true;
    if (pthread_create(&waiter, NULL,
                run_poll_wait, &wait_call) != 0) {
        fprintf(stderr,
                "socket 监听恢复测试失败：启动 poll 等待线程\n");
        failures++;
        goto cleanup;
    }
    waiter_started = true;
    if (!scan_gate_wait_until_entered(&gate)) {
        fprintf(stderr,
                "socket 监听恢复测试失败：poll 未进入受控首次扫描\n");
        failures++;
        goto cleanup;
    }
    // 此时监听器已在首次扫描中被判定为未就绪；后续唤醒必须来自重登记后端。
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(client,
                (const struct sockaddr *) &address,
                sizeof(address)) < 0) {
        fprintf(stderr,
                "socket 监听恢复测试失败：连接恢复后的监听器：%s\n",
                strerror(errno));
        failures++;
        goto cleanup;
    }
    scan_gate_release(&gate);
    EXPECT(pthread_join(waiter, NULL) == 0,
            "poll 等待线程及时退出");
    waiter_joined = true;
    task_thread_store(&fixture.task, pthread_self());
    current = &fixture.task;
    poll_set_listen_wait_exit_test_hook(NULL, NULL);
    late_punt_hook_installed = false;
    EXPECT(wait_call.result == 1 &&
                    wait_call.event_token == LISTENER_EVENT_TOKEN &&
                    (wait_call.event_types & POLL_READ) != 0,
            "替换前的 poll 登记在 replacement 后收到新连接事件");
    EXPECT(atomic_load_explicit(
                    &late_punt.calls, memory_order_relaxed) == 1 &&
                    !sockrestart_should_restart_listen_wait(&fixture.task),
            "poll 注销后消费晚到 punt，下一次普通 EINTR 不会被误吞");
    accepted = sys_accept(listener_number, 0, 0);
    EXPECT(accepted >= 0,
            "恢复后的监听器可由 guest sys_accept 接受连接");
    if (accepted >= 0) {
        EXPECT(f_close_task(&fixture.task, accepted) == 0,
                "关闭恢复路径接受的 guest socket");
    }
    sockrestart_begin_listen_wait(&fixture.task, listener);
    sockrestart_on_resume();
    sockrestart_end_listen_wait(&fixture.task, listener);
    punt_client = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(punt_client >= 0 && connect(punt_client,
                    (const struct sockaddr *) &address,
                    sizeof(address)) == 0 &&
            wait_listener_ready(listener->real_fd),
            "构造恢复唤醒与成功 accept 的竞态输入");
    accepted = sys_accept(listener_number, 0, 0);
    EXPECT(accepted >= 0 &&
            !sockrestart_should_restart_listen_wait(&fixture.task),
            "成功 accept 无条件消费恢复 punt 标志");
    if (accepted >= 0) {
        EXPECT(f_close_task(&fixture.task, accepted) == 0,
                "关闭 punt 回归接受的 guest socket");
        accepted = -1;
    }
cleanup:
    if (gate_ready)
        scan_gate_release(&gate);
    if (waiter_started && !waiter_joined)
        pthread_join(waiter, NULL);
    if (late_punt_hook_installed)
        poll_set_listen_wait_exit_test_hook(NULL, NULL);
    if (client >= 0)
        close(client);
    if (punt_client >= 0)
        close(punt_client);
    if (fixture_ready && accepted >= 0)
        f_close_task(&fixture.task, accepted);
    if (snapshot_pending)
        sockrestart_on_resume();
    if (poll != NULL)
        poll_destroy(poll);
    if (gate_fd != NULL)
        fd_close(gate_fd);
    if (gate_ready)
        scan_gate_destroy(&gate);
    if (fixture_ready)
        fixture_destroy(&fixture);
    EXPECT(open_host_fds() == baseline,
            "测试结束后宿主 fd 数量回到基线");
    return failures == 0 ? 0 : 1;
}

static bool test_live_listener_preserved(void) {
    int previous_failures = failures;
    int baseline = open_host_fds();
    struct fixture fixture;
    bool fixture_ready = false;
    fd_t listener_number = -1;
    fd_t accepted = -1;
    int client = -1;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "保留有效 listener 的夹具初始化成功");
        goto cleanup;
    }
    fixture_ready = true;
    struct sockaddr_in address;
    listener_number = create_inet_listener(&fixture, &address, 5);
    struct fd *listener = f_get_task(&fixture.task, listener_number);
    if (listener_number < 0 || listener == NULL) {
        EXPECT(false, "创建用于保留测试的 INET listener");
        goto cleanup;
    }
    int raw_fd = listener->real_fd;
    struct stat before = {0};
    EXPECT(fstat(raw_fd, &before) == 0,
            "记录仍有效 listener 的宿主身份");
    sockrestart_on_suspend();
    sockrestart_on_resume();

    struct stat after = {0};
    EXPECT(listener->real_fd == raw_fd &&
                    fstat(raw_fd, &after) == 0 &&
                    before.st_dev == after.st_dev &&
                    before.st_ino == after.st_ino,
            "恢复保留仍有效的原 listener，而非无条件替换");
    client = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(client >= 0 && connect(client,
                    (const struct sockaddr *) &address,
                    sizeof(address)) == 0 &&
                    wait_listener_ready(raw_fd),
            "保留后的 listener 仍可建立连接");
    accepted = sys_accept(listener_number, 0, 0);
    EXPECT(accepted >= 0,
            "保留后的 listener 仍可由 guest accept");

cleanup:
    if (fixture_ready && accepted >= 0)
        (void) f_close_task(&fixture.task, accepted);
    if (client >= 0)
        close(client);
    if (fixture_ready)
        fixture_destroy(&fixture);
    EXPECT(open_host_fds() == baseline,
            "有效 listener 保留测试结束后宿主 fd 回到基线");
    return failures == previous_failures;
}

static bool test_unix_listener_restart(bool abstract_name) {
    int previous_failures = failures;
    int baseline = open_host_fds();
    struct fixture fixture;
    bool fixture_ready = false;
    struct poll *poll = NULL;
    struct fd *gate_fd = NULL;
    struct scan_gate gate;
    bool gate_ready = false;
    pthread_t waiter = {};
    bool waiter_started = false;
    bool waiter_joined = false;
    fd_t listener_number = -1;
    fd_t connector = -1;
    fd_t accepted = -1;
    char backing_path[sizeof(((struct sockaddr_un *) 0)->sun_path)] = {0};
    char pathname[80];
    if (!fixture_init(&fixture)) {
        EXPECT(false, "Unix listener 恢复夹具初始化成功");
        goto cleanup;
    }
    fixture_ready = true;
    struct guest_unix_address address;
    if (abstract_name) {
        char name[80] = {0};
        snprintf(name + 1, sizeof(name) - 1,
                "listen-restart-abstract-%ld", (long) getpid());
        address = guest_unix_address(name, 1 + strlen(name + 1));
        pathname[0] = '\0';
    } else {
        snprintf(pathname, sizeof(pathname),
                "/listen-restart-path-%ld", (long) getpid());
        address = guest_unix_address(pathname, strlen(pathname) + 1);
    }
    listener_number = create_unix_listener(&fixture, &address, 7);
    struct fd *listener = f_get_task(&fixture.task, listener_number);
    if (listener_number < 0 || listener == NULL) {
        EXPECT(false, "创建 Unix listener");
        goto cleanup;
    }
    lock(&listener->lock);
    memcpy(backing_path, listener->socket.unix_backing_path,
            sizeof(backing_path));
    unlock(&listener->lock);
    EXPECT(unix_backing_identity_matches(listener),
            "Unix listener 初始后备节点身份有效");
    poll = poll_create();
    if (IS_ERR(poll)) {
        poll = NULL;
        EXPECT(false, "创建 Unix listener 的 poll 后端");
        goto cleanup;
    }
    scan_gate_init(&gate);
    gate_ready = true;
    gate_fd = fd_create(&scan_gate_ops);
    if (gate_fd == NULL) {
        EXPECT(false, "创建 Unix listener 的扫描闸门");
        goto cleanup;
    }
    gate_fd->data = &gate;
    EXPECT(poll_add_fd(poll, gate_fd, POLL_READ,
                    (union poll_fd_info) {.num = 0}) == 0 &&
                    poll_add_fd(poll, listener, POLL_READ,
                    (union poll_fd_info) {
                        .num = LISTENER_EVENT_TOKEN,
                    }) == 0,
            "替换前登记 Unix listener 的 poll 事件");
    if (failures != previous_failures)
        goto cleanup;
    sockrestart_on_suspend();
    int old_raw_fd = listener->real_fd;
    EXPECT(close(old_raw_fd) == 0,
            "模拟挂起时 Unix listener 的宿主 fd 失效");
    sockrestart_on_resume();

    lock(&listener->lock);
    bool recovered = listener->real_fd == old_raw_fd &&
            listener->socket.host_listening &&
            listener->socket.guest_listening &&
            strcmp(listener->socket.unix_backing_path,
                    backing_path) == 0;
    unlock(&listener->lock);
    EXPECT(recovered && unix_backing_identity_matches(listener),
            "Unix listener 原位重建并刷新后备节点身份");
    if (!recovered)
        goto cleanup;
    struct poll_wait_call wait_call = {
        .task = &fixture.task,
        .poll = poll,
        .result = -1,
    };
    if (pthread_create(&waiter, NULL,
                run_poll_wait, &wait_call) != 0) {
        EXPECT(false, "启动 Unix listener 的 poll 等待线程");
        goto cleanup;
    }
    waiter_started = true;
    if (!scan_gate_wait_until_entered(&gate)) {
        EXPECT(false, "Unix listener poll 进入受控首次扫描");
        goto cleanup;
    }
    connector = connect_unix_listener(&fixture, &address);
    EXPECT(connector >= 0,
            "恢复后的 Unix listener 可按 guest 名称连接");
    scan_gate_release(&gate);
    EXPECT(pthread_join(waiter, NULL) == 0,
            "Unix listener poll 等待线程及时退出");
    waiter_joined = true;
    task_thread_store(&fixture.task, pthread_self());
    current = &fixture.task;
    EXPECT(wait_call.result == 1 &&
                    wait_call.event_token == LISTENER_EVENT_TOKEN &&
                    (wait_call.event_types & POLL_READ) != 0,
            "Unix listener replacement 重登记后收到连接事件");
    accepted = sys_accept(listener_number, 0, 0);
    EXPECT(accepted >= 0,
            "恢复后的 Unix listener 可完成 guest accept");

cleanup:
    if (gate_ready)
        scan_gate_release(&gate);
    if (waiter_started && !waiter_joined) {
        (void) pthread_join(waiter, NULL);
        task_thread_store(&fixture.task, pthread_self());
        current = &fixture.task;
    }
    if (poll != NULL)
        poll_destroy(poll);
    if (gate_fd != NULL)
        fd_close(gate_fd);
    if (gate_ready)
        scan_gate_destroy(&gate);
    if (fixture_ready && accepted >= 0)
        (void) f_close_task(&fixture.task, accepted);
    if (fixture_ready && connector >= 0)
        (void) f_close_task(&fixture.task, connector);
    if (fixture_ready && listener_number >= 0) {
        EXPECT(f_close_task(&fixture.task, listener_number) == 0,
                "关闭恢复后的 Unix listener");
        errno = 0;
        EXPECT(backing_path[0] != '\0' &&
                        lstat(backing_path, &(struct stat) {0}) < 0 &&
                        errno == ENOENT,
                "Unix listener 最终关闭只清理当前拥有的后备节点");
    }
    if (fixture_ready && !abstract_name && pathname[0] != '\0')
        EXPECT(file_unlinkat_task(&fixture.task, AT_FDCWD_,
                        pathname, false) == 0,
                "显式清理 pathname Unix guest 名称节点");
    if (fixture_ready)
        fixture_destroy(&fixture);
    EXPECT(open_host_fds() == baseline,
            "Unix listener 恢复测试结束后宿主 fd 回到基线");
    return failures == previous_failures;
}

static bool test_occupied_target_fails_closed(void) {
    int previous_failures = failures;
    int baseline = open_host_fds();
    struct fixture fixture;
    bool fixture_ready = false;
    fd_t listener_number = -1;
    int sentinel = -1;
    int sentinel_client = -1;
    int sentinel_accepted = -1;
    if (!fixture_init(&fixture)) {
        EXPECT(false, "目标 fd 冲突测试夹具初始化成功");
        goto cleanup;
    }
    fixture_ready = true;
    struct sockaddr_in address;
    listener_number = create_inet_listener(&fixture, &address, 3);
    struct fd *listener = f_get_task(&fixture.task, listener_number);
    if (listener_number < 0 || listener == NULL) {
        EXPECT(false, "创建目标 fd 冲突测试 listener");
        goto cleanup;
    }
    uint64_t generation = listener->socket.listen_generation;
    int target = listener->real_fd;
    sockrestart_on_suspend();
    EXPECT(close(target) == 0,
            "释放待恢复 listener 的原目标 fd");
    struct sockaddr_in sentinel_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t sentinel_address_length = sizeof(sentinel_address);
    sentinel = socket(AF_INET, SOCK_STREAM, 0);
    if (sentinel < 0 || bind(sentinel,
                (const struct sockaddr *) &sentinel_address,
                sizeof(sentinel_address)) < 0 ||
            getsockname(sentinel,
                    (struct sockaddr *) &sentinel_address,
                    &sentinel_address_length) < 0 ||
            listen(sentinel, 1) < 0) {
        EXPECT(false, "创建与原目标同类型的外来 listener");
        sockrestart_on_resume();
        goto cleanup;
    }
    if (sentinel != target) {
        int original = sentinel;
        sentinel = dup2(original, target);
        close(original);
        if (sentinel < 0) {
            EXPECT(false, "将 sentinel 安装到待恢复 fd 号");
            sockrestart_on_resume();
            goto cleanup;
        }
    }
    EXPECT(sentinel == target,
            "以外来 listener 精确占用待恢复 fd 号");

    sockrestart_on_resume();
    EXPECT(listener_is_failed_closed(listener, generation),
            "目标 fd 冲突时 listener 进入可观察的失败关闭状态");
    EXPECT(sys_accept(listener_number, 0, 0) == _EINVAL,
            "失败关闭的 listener 不再接受 guest 连接");
    sentinel_client = socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(sentinel_client >= 0 && connect(sentinel_client,
                    (const struct sockaddr *) &sentinel_address,
                    sizeof(sentinel_address)) == 0 &&
                    wait_listener_ready(sentinel),
            "恢复过程不关闭或覆盖同类型外来 listener");
    sentinel_accepted = accept(sentinel, NULL, NULL);
    EXPECT(sentinel_accepted >= 0,
            "外来 listener 在恢复拒绝后仍可接受连接");
    EXPECT(f_close_task(&fixture.task, listener_number) == 0 &&
                    fcntl(sentinel, F_GETFD) >= 0,
            "关闭 guest fd 仍不关闭外来 listener");
    listener_number = -1;

cleanup:
    if (fixture_ready && listener_number >= 0)
        (void) f_close_task(&fixture.task, listener_number);
    if (sentinel >= 0)
        close(sentinel);
    if (sentinel_client >= 0)
        close(sentinel_client);
    if (sentinel_accepted >= 0)
        close(sentinel_accepted);
    if (fixture_ready)
        fixture_destroy(&fixture);
    EXPECT(open_host_fds() == baseline,
            "目标 fd 冲突测试结束后宿主 fd 回到基线");
    return failures == previous_failures;
}

static bool test_unix_path_collision_fails_closed(void) {
    int previous_failures = failures;
    int baseline = open_host_fds();
    struct fixture fixture;
    bool fixture_ready = false;
    fd_t listener_number = -1;
    int replacement = -1;
    char pathname[80] = {0};
    char backing_path[sizeof(((struct sockaddr_un *) 0)->sun_path)] = {0};
    if (!fixture_init(&fixture)) {
        EXPECT(false, "Unix 路径冲突测试夹具初始化成功");
        goto cleanup;
    }
    fixture_ready = true;
    snprintf(pathname, sizeof(pathname),
            "/listen-restart-conflict-%ld", (long) getpid());
    struct guest_unix_address address =
            guest_unix_address(pathname, strlen(pathname) + 1);
    listener_number = create_unix_listener(&fixture, &address, 4);
    struct fd *listener = f_get_task(&fixture.task, listener_number);
    if (listener_number < 0 || listener == NULL) {
        EXPECT(false, "创建 Unix 路径冲突测试 listener");
        goto cleanup;
    }
    uint64_t generation = listener->socket.listen_generation;
    int target = listener->real_fd;
    lock(&listener->lock);
    memcpy(backing_path, listener->socket.unix_backing_path,
            sizeof(backing_path));
    unlock(&listener->lock);

    sockrestart_on_suspend();
    EXPECT(close(target) == 0 && unlink(backing_path) == 0,
            "释放 Unix listener fd 与原后备节点");
    int reservation = open("/dev/null", O_RDONLY);
    if (reservation >= 0 && reservation != target) {
        int original = reservation;
        reservation = dup2(original, target);
        close(original);
    }
    replacement = open(backing_path,
            O_CREAT | O_EXCL | O_RDWR, 0600);
    if (reservation >= 0)
        close(reservation);
    struct stat replacement_before = {0};
    EXPECT(replacement >= 0 &&
                    fstat(replacement, &replacement_before) == 0 &&
                    S_ISREG(replacement_before.st_mode) &&
                    fcntl(target, F_GETFD) < 0 && errno == EBADF,
            "用不同身份的普通文件替换后备路径并保持目标 fd 空闲");

    sockrestart_on_resume();
    struct stat replacement_after = {0};
    lock(&listener->lock);
    bool ownership_cleared =
            !listener->socket.unix_backing_owned;
    unlock(&listener->lock);
    EXPECT(listener_is_failed_closed(listener, generation) &&
                    ownership_cleared,
            "后备路径身份冲突时 listener 失败关闭并撤销所有权");
    EXPECT(sys_accept(listener_number, 0, 0) == _EINVAL,
            "后备路径冲突后的 listener 不再接受 guest 连接");
    EXPECT(lstat(backing_path, &replacement_after) == 0 &&
                    S_ISREG(replacement_after.st_mode) &&
                    replacement_before.st_dev == replacement_after.st_dev &&
                    replacement_before.st_ino == replacement_after.st_ino,
            "恢复过程不删除或替换外来的同名节点");

cleanup:
    if (fixture_ready && listener_number >= 0)
        (void) f_close_task(&fixture.task, listener_number);
    if (backing_path[0] != '\0') {
        struct stat status;
        EXPECT(lstat(backing_path, &status) == 0 && S_ISREG(status.st_mode),
                "listener 最终关闭仍保留外来的后备路径节点");
    }
    if (replacement >= 0)
        close(replacement);
    if (backing_path[0] != '\0')
        (void) unlink(backing_path);
    if (fixture_ready && pathname[0] != '\0')
        (void) file_unlinkat_task(
                &fixture.task, AT_FDCWD_, pathname, false);
    if (fixture_ready)
        fixture_destroy(&fixture);
    EXPECT(open_host_fds() == baseline,
            "Unix 路径冲突测试结束后宿主 fd 回到基线");
    return failures == previous_failures;
}

int main(void) {
    struct sigaction action = {
        .sa_handler = timeout_handler,
    };
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    struct sigaction wake_action = {
        .sa_handler = wake_handler,
    };
    sigemptyset(&wake_action.sa_mask);
    sigaction(SIGUSR1, &wake_action, NULL);
    alarm(TEST_TIMEOUT_SECONDS);

    (void) test_listen_restart();
    (void) test_live_listener_preserved();
    (void) test_unix_listener_restart(false);
    (void) test_unix_listener_restart(true);
    (void) test_occupied_target_fails_closed();
    (void) test_unix_path_collision_fails_closed();
    int result = failures == 0 ? 0 : 1;
    alarm(0);
    return result;
}

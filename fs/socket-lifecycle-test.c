#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/sock.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define LENGTH_PAGE UINT32_C(0x1000)
#define UNMAPPED_PAGE UINT32_C(0x2000)
#define ACCEPT_ADDRESS (LENGTH_PAGE + UINT32_C(0x080))
#define UNIX_ADDRESS (LENGTH_PAGE + UINT32_C(0x100))

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "socket 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct close_probe {
    int calls;
};

static int probe_close(struct fd *fd) {
    struct close_probe *probe = fd->data;
    probe->calls++;
    return 0;
}

static const struct fd_ops probe_ops = {
    .close = probe_close,
};

static struct fd *probe_fd_create(struct close_probe *probe) {
    struct fd *fd = fd_create(&probe_ops);
    if (fd != NULL)
        fd->data = probe;
    return fd;
}

static void task_fixture_init(struct task *task, struct tgroup *group) {
    memset(task, 0, sizeof(*task));
    memset(group, 0, sizeof(*group));
    lock_init(&group->lock);
    lock_init(&task->waiting_cond_lock);
    group->limits[RLIMIT_NOFILE_] = (struct rlimit_) {8, 8};
    task->group = group;
    task->files = fdtable_new(2);
}

static int map_length_page(struct task *task) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(
            task->mem, PAGE(LENGTH_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error;
}

struct connect_call {
    struct task *task;
    struct socket_ref *socket;
    struct sockaddr_max_ address;
    size_t address_length;
    atomic_bool finished;
    int_t result;
};

struct accept_call {
    struct task *task;
    fd_t listener;
    atomic_bool finished;
    int_t result;
};

static void *run_connect(void *opaque) {
    struct connect_call *call = opaque;
    current = call->task;
    call->result = socket_connect_ref_task(call->task,
            call->socket, &call->address, call->address_length);
    atomic_store_explicit(
            &call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static void *run_accept(void *opaque) {
    struct accept_call *call = opaque;
    current = call->task;
    call->result = sys_accept(call->listener, 0, 0);
    atomic_store_explicit(
            &call->finished, true, memory_order_release);
    current = NULL;
    return NULL;
}

static bool wait_for_completion(atomic_bool *finished, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (atomic_load_explicit(finished, memory_order_acquire))
            return true;
        nanosleep(&interval, NULL);
    }
    return atomic_load_explicit(finished, memory_order_acquire);
}

static bool wait_for_listener(int listener, int timeout_ms) {
    struct pollfd event = {
        .fd = listener,
        .events = POLLIN,
    };
    int result;
    do {
        result = poll(&event, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (event.revents & POLLIN) != 0;
}

static bool wait_for_accept_install_gate(
        int listener, struct fdtable *table, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        struct pollfd event = {
            .fd = listener,
            .events = POLLIN,
        };
        int poll_result = poll(&event, 1, 0);
        if (poll_result == 0 || (event.revents & POLLIN) == 0) {
            int lock_result = trylock(&table->lock);
            if (lock_result == EBUSY)
                return true;
            if (lock_result == 0)
                unlock(&table->lock);
        }
        nanosleep(&interval, NULL);
    }
    return false;
}

static bool wait_for_task_condition(struct task *task,
        cond_t *condition, unsigned timeout_ms) {
    const struct timespec interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed++) {
        lock(&task->waiting_cond_lock);
        bool registered = task->waiting_cond == condition;
        unlock(&task->waiting_cond_lock);
        if (registered)
            return true;
        nanosleep(&interval, NULL);
    }
    return false;
}

int main(void) {
    struct task task;
    struct tgroup group;
    task_fixture_init(&task, &group);
    CHECK(!IS_ERR(task.files), "测试 fd 表创建成功");
    task_set_mm(&task, mm_new());
    CHECK(task.mm != NULL && map_length_page(&task) == 0,
            "测试用户地址空间创建成功");
    current = &task;

    struct socket_ref retained = {0};
    CHECK(socket_ref_get_task(&task, 99, &retained) == _EBADF &&
            retained.fd == NULL,
            "缺失 fd 返回 EBADF 且不产生引用");

    struct close_probe ordinary_probe = {0};
    struct fd *ordinary = probe_fd_create(&ordinary_probe);
    CHECK(ordinary != NULL, "普通 fd 创建成功");
    CHECK(f_install_task(&task, ordinary, 0) == 0,
            "普通 fd 安装到首个槽位");
    CHECK(socket_ref_get_task(&task, 0, &retained) == _ENOTSOCK &&
            retained.fd == NULL && ordinary_probe.calls == 0 &&
            f_get_task(&task, 0) == ordinary,
            "非 socket 返回 ENOTSOCK 且不消耗表中引用");

    int_t socket_number = socket_create_task(
            &task, AF_INET_, SOCK_STREAM_, 0);
    CHECK(socket_number == 1, "测试 socket 安装到第二个槽位");
    CHECK(socket_ref_get_task(&task, socket_number, &retained) == 0 &&
            retained.fd != NULL,
            "socket retained 引用获取成功");
    int original_host_fd = retained.fd->real_fd;

    CHECK(f_close_task(&task, socket_number) == 0 &&
            f_get_task(&task, socket_number) == NULL &&
            fcntl(original_host_fd, F_GETFD) >= 0,
            "关闭表项后 retained 引用继续保持原 socket 存活");

    struct close_probe replacement_probe = {0};
    struct fd *replacement = probe_fd_create(&replacement_probe);
    CHECK(replacement != NULL, "替换 fd 创建成功");
    CHECK(f_install_task(&task, replacement, 0) == socket_number &&
            f_get_task(&task, socket_number) == replacement,
            "空槽位被普通 fd 以同号复用");

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0, "host 回环监听 socket 创建成功");
    struct sockaddr_in listener_address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    CHECK(bind(listener, (const struct sockaddr *) &listener_address,
                    sizeof(listener_address)) == 0 &&
            listen(listener, 1) == 0,
            "host 回环监听 socket 启动成功");
    socklen_t listener_length = sizeof(listener_address);
    CHECK(getsockname(listener,
                    (struct sockaddr *) &listener_address,
                    &listener_length) == 0,
            "host 回环监听地址查询成功");

    unsigned char guest_address[16] = {0};
    uint16_t guest_family = AF_INET_;
    memcpy(guest_address, &guest_family, sizeof(guest_family));
    memcpy(guest_address + 2, &listener_address.sin_port,
            sizeof(listener_address.sin_port));
    memcpy(guest_address + 4, &listener_address.sin_addr,
            sizeof(listener_address.sin_addr));

    CHECK(socket_connect_ref_task(&task, &retained,
                    guest_address, sizeof(guest_address)) == 0,
            "retained 引用连接原 socket");
    int accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0 &&
            f_get_task(&task, socket_number) == replacement &&
            replacement_probe.calls == 0,
            "连接由原 socket 建立且同号替换对象未受影响");
    close(accepted);
    close(listener);

    CHECK(fcntl(original_host_fd, F_GETFD) >= 0,
            "释放 retained 引用前原 host socket 仍然有效");
    socket_ref_release(&retained);
    CHECK(retained.fd == NULL, "释放后清空 retained 引用");
    errno = 0;
    CHECK(fcntl(original_host_fd, F_GETFD) == -1 && errno == EBADF,
            "最后一个 retained 引用释放原 host socket");
    CHECK(f_get_task(&task, socket_number) == replacement &&
            replacement_probe.calls == 0,
            "释放原 socket 不得误伤同号替换对象");

    int_t accept_listener_number = socket_create_task(
            &task, AF_INET_, SOCK_STREAM_, 0);
    CHECK(accept_listener_number == 2,
            "accept 回归监听 socket 安装到第三个槽位");
    struct fd *accept_listener = f_get_task(
            &task, accept_listener_number);
    CHECK(accept_listener != NULL, "accept 回归监听 socket 可查询");
    struct sockaddr_in accept_address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    CHECK(bind(accept_listener->real_fd,
                    (const struct sockaddr *) &accept_address,
                    sizeof(accept_address)) == 0 &&
            listen(accept_listener->real_fd, 1) == 0,
            "accept 回归监听 socket 启动成功");
    socklen_t accept_address_length = sizeof(accept_address);
    CHECK(getsockname(accept_listener->real_fd,
                    (struct sockaddr *) &accept_address,
                    &accept_address_length) == 0,
            "accept 回归监听地址查询成功");

    int accept_client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(accept_client >= 0 &&
            connect(accept_client,
                    (const struct sockaddr *) &accept_address,
                    sizeof(accept_address)) == 0,
            "accept 回归 host 客户端连接成功");
    dword_t guest_address_length = sizeof(struct sockaddr_in);
    CHECK(user_write(LENGTH_PAGE, &guest_address_length,
                    sizeof(guest_address_length)) == 0,
            "写入 accept 地址容量");

    int accepted_host_fd = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    CHECK(accepted_host_fd >= 0 && close(accepted_host_fd) == 0,
            "探测 accept 将取得的 host fd");
    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 3;
    unlock(&group.lock);
    CHECK(sys_accept(accept_listener_number,
                    UNMAPPED_PAGE, LENGTH_PAGE) == _EFAULT,
            "accept 在 sockaddr 写回失败时返回 EFAULT");
    errno = 0;
    CHECK(fcntl(accepted_host_fd, F_GETFD) == -1 && errno == EBADF &&
            f_get_task(&task, 0) == ordinary &&
            f_get_task(&task, socket_number) == replacement &&
            f_get_task(&task, accept_listener_number) == accept_listener,
            "写回失败关闭 raw client 且不改变 guest fd 表");

    int negative_length_client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(negative_length_client >= 0 &&
            connect(negative_length_client,
                    (const struct sockaddr *) &accept_address,
                    sizeof(accept_address)) == 0,
            "accept 负长度回归 host 客户端连接成功");
    int negative_length_host_fd = fcntl(
            negative_length_client, F_DUPFD_CLOEXEC, 0);
    CHECK(negative_length_host_fd >= 0 &&
            close(negative_length_host_fd) == 0,
            "探测 accept 负长度将取得的 host fd");
    sdword_t negative_address_length = -1;
    CHECK(user_write(LENGTH_PAGE, &negative_address_length,
                    sizeof(negative_address_length)) == 0 &&
            sys_accept(accept_listener_number,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == _EINVAL,
            "accept 按 Linux 语义拒绝负的地址容量");
    errno = 0;
    CHECK(fcntl(negative_length_host_fd, F_GETFD) == -1 &&
            errno == EBADF,
            "accept 负长度失败关闭 accepted host fd");
    close(negative_length_client);

    int created_host_fd = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    CHECK(created_host_fd >= 0 && close(created_host_fd) == 0,
            "探测 socket 创建将取得的 host fd");
    CHECK(socket_create_task(&task, AF_INET_, SOCK_STREAM_, 0) ==
                    _EMFILE,
            "fd 表已满时 socket 创建返回 EMFILE");
    errno = 0;
    CHECK(fcntl(created_host_fd, F_GETFD) == -1 && errno == EBADF,
            "socket 安装失败后 raw fd 已关闭");

    int socketpair_host_fds[2];
    socketpair_host_fds[0] = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    socketpair_host_fds[1] = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    CHECK(socketpair_host_fds[0] >= 0 &&
            socketpair_host_fds[1] >= 0 &&
            close(socketpair_host_fds[0]) == 0 &&
            close(socketpair_host_fds[1]) == 0,
            "探测 socketpair 将取得的两个 host fd");
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0,
                    LENGTH_PAGE) == _EMFILE,
            "fd 表已满时 socketpair 首端安装返回 EMFILE");
    errno = 0;
    bool socketpair_first_closed =
            fcntl(socketpair_host_fds[0], F_GETFD) == -1 &&
            errno == EBADF;
    errno = 0;
    bool socketpair_second_closed =
            fcntl(socketpair_host_fds[1], F_GETFD) == -1 &&
            errno == EBADF;
    CHECK(socketpair_first_closed && socketpair_second_closed &&
            f_get_task(&task, 0) == ordinary &&
            f_get_task(&task, socket_number) == replacement &&
            f_get_task(&task, accept_listener_number) == accept_listener,
            "socketpair 安装失败关闭两端且不改变 guest fd 表");
    CHECK(f_close_task(&task, accept_listener_number) == 0,
            "accept 回归监听 socket 清理成功");

    socketpair_host_fds[0] = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    socketpair_host_fds[1] = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    CHECK(socketpair_host_fds[0] >= 0 &&
            socketpair_host_fds[1] >= 0 &&
            close(socketpair_host_fds[0]) == 0 &&
            close(socketpair_host_fds[1]) == 0,
            "探测 socketpair 第二端失败时的两个 host fd");
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0,
                    LENGTH_PAGE) == _EMFILE &&
            f_get_task(&task, 2) == NULL,
            "仅剩一个槽位时 socketpair 回滚已安装首端");
    errno = 0;
    socketpair_first_closed =
            fcntl(socketpair_host_fds[0], F_GETFD) == -1 &&
            errno == EBADF;
    errno = 0;
    socketpair_second_closed =
            fcntl(socketpair_host_fds[1], F_GETFD) == -1 &&
            errno == EBADF;
    CHECK(socketpair_first_closed && socketpair_second_closed,
            "socketpair 第二端安装失败关闭并回滚两端");

    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 4;
    unlock(&group.lock);
    socketpair_host_fds[0] = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    socketpair_host_fds[1] = fcntl(
            accept_client, F_DUPFD_CLOEXEC, 0);
    CHECK(socketpair_host_fds[0] >= 0 &&
            socketpair_host_fds[1] >= 0 &&
            close(socketpair_host_fds[0]) == 0 &&
            close(socketpair_host_fds[1]) == 0,
            "探测 socketpair 写回失败时的两个 host fd");
    CHECK(sys_socketpair(AF_LOCAL_, SOCK_STREAM_, 0,
                    UNMAPPED_PAGE) == _EFAULT &&
            f_get_task(&task, 2) == NULL &&
            f_get_task(&task, 3) == NULL,
            "socketpair guest 写回失败精确回滚已安装两端");
    errno = 0;
    socketpair_first_closed =
            fcntl(socketpair_host_fds[0], F_GETFD) == -1 &&
            errno == EBADF;
    errno = 0;
    socketpair_second_closed =
            fcntl(socketpair_host_fds[1], F_GETFD) == -1 &&
            errno == EBADF;
    CHECK(socketpair_first_closed && socketpair_second_closed,
            "socketpair guest 写回失败关闭两端 host fd");

    CHECK(sys_socketpair(AF_LOCAL_,
                    SOCK_STREAM_ | SOCK_NONBLOCK_ | SOCK_CLOEXEC_,
                    0, LENGTH_PAGE) == 0,
            "socketpair 原子发布双端成功");
    sdword_t installed_pair[2] = {-1, -1};
    CHECK(user_read(LENGTH_PAGE, installed_pair,
                    sizeof(installed_pair)) == 0 &&
            installed_pair[0] == 2 && installed_pair[1] == 3 &&
            installed_pair[0] != installed_pair[1],
            "socketpair 返回两个互异最低空槽");
    struct fd *installed_pair_first = f_get_task(
            &task, installed_pair[0]);
    struct fd *installed_pair_second = f_get_task(
            &task, installed_pair[1]);
    CHECK(installed_pair_first != NULL &&
            installed_pair_second != NULL &&
            installed_pair_first->socket.unix_peer ==
                    installed_pair_second &&
            installed_pair_second->socket.unix_peer ==
                    installed_pair_first &&
            (f_getfl_task(&task, installed_pair[0]) &
                    SOCK_NONBLOCK_) != 0 &&
            (f_getfl_task(&task, installed_pair[1]) &
                    SOCK_NONBLOCK_) != 0 &&
            f_getfd_task(&task, installed_pair[0]) == FD_CLOEXEC_ &&
            f_getfd_task(&task, installed_pair[1]) == FD_CLOEXEC_,
            "socketpair 发布前建立 peer 并向双端应用描述符标志");
    CHECK(f_close_task(&task, installed_pair[1]) == 0 &&
            f_close_task(&task, installed_pair[0]) == 0,
            "socketpair 原子发布成功项清理完成");
    close(accept_client);

    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 8;
    unlock(&group.lock);
    int_t unix_listener_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_STREAM_, 0);
    int_t unix_connector_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_STREAM_, 0);
    CHECK(unix_listener_number == 2 && unix_connector_number == 3,
            "AF_UNIX 握手回归 socket 安装成功");
    struct fd *unix_listener = f_get_task(
            &task, unix_listener_number);
    struct socket_ref unix_connector = {0};
    CHECK(unix_listener != NULL &&
            socket_ref_get_task(&task, unix_connector_number,
                    &unix_connector) == 0,
            "AF_UNIX 握手回归取得两端对象");

    struct sockaddr_max_ guest_unix_address = {
        .family = AF_LOCAL_,
    };
    snprintf(guest_unix_address.data + 1,
            sizeof(guest_unix_address.data) - 1,
            "lifecycle-%ld", (long) getpid());
    size_t guest_unix_address_length =
            offsetof(struct sockaddr_max_, data) + 1 +
            strlen(guest_unix_address.data + 1);
    CHECK(user_write(UNIX_ADDRESS, &guest_unix_address,
                    guest_unix_address_length) == 0 &&
            sys_bind(unix_listener_number, UNIX_ADDRESS,
                    (uint_t) guest_unix_address_length) == 0 &&
            sys_listen(unix_listener_number, 1) == 0,
            "AF_UNIX 抽象监听 socket 启动成功");
    struct sockaddr_un backing_address = {0};
    socklen_t backing_address_length = sizeof(backing_address);
    CHECK(getsockname(unix_listener->real_fd,
                    (struct sockaddr *) &backing_address,
                    &backing_address_length) == 0,
            "取得 AF_UNIX host 后备路径");

    int unix_accepted_host_fd = fcntl(
            unix_connector.fd->real_fd, F_DUPFD_CLOEXEC, 0);
    CHECK(unix_accepted_host_fd >= 0 &&
            close(unix_accepted_host_fd) == 0,
            "探测 AF_UNIX accept 将取得的 host fd");
    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 4;
    unlock(&group.lock);
    struct connect_call connect_call = {
        .task = &task,
        .socket = &unix_connector,
        .address = guest_unix_address,
        .address_length = guest_unix_address_length,
        .result = _EIO,
    };
    struct accept_call accept_call = {
        .task = &task,
        .listener = unix_listener_number,
        .result = _EIO,
    };
    atomic_init(&connect_call.finished, false);
    atomic_init(&accept_call.finished, false);
    pthread_t connector_thread;
    pthread_t accept_thread;

    // 卡住最终 fd 安装，使连接线程确定进入 peer 条件等待后再放行取消路径。
    lock(&group.lock);
    CHECK(pthread_create(&connector_thread, NULL,
                    run_connect, &connect_call) == 0,
            "启动 AF_UNIX 连接等待线程");
    CHECK(wait_for_listener(unix_listener->real_fd, 1000),
            "AF_UNIX 连接进入 host accept 队列");
    CHECK(pthread_create(&accept_thread, NULL,
                    run_accept, &accept_call) == 0,
            "启动 AF_UNIX accept 线程");
    CHECK(wait_for_accept_install_gate(unix_listener->real_fd,
                    task.files, 1000),
            "AF_UNIX accept 在满表安装点等待门闩");
    CHECK(wait_for_task_condition(&task,
                    &unix_connector.fd->socket.unix_got_peer, 1000),
            "AF_UNIX connect 已登记 peer 条件等待");
    unlock(&group.lock);

    CHECK(wait_for_completion(&accept_call.finished, 1000) &&
            pthread_join(accept_thread, NULL) == 0,
            "AF_UNIX accept 取消路径及时返回");
    bool connector_woke_without_rescue =
            wait_for_completion(&connect_call.finished, 250);
    if (!connector_woke_without_rescue) {
        // 防止回归时测试进程本身挂死；该唤醒只用于收尾，断言仍会失败。
        notify(&unix_connector.fd->socket.unix_got_peer);
        if (!wait_for_completion(&connect_call.finished, 1000)) {
            pthread_cancel(connector_thread);
            pthread_detach(connector_thread);
            fprintf(stderr,
                    "socket 生命周期测试失败：AF_UNIX connect 救援后仍未退出（第 %d 行）\n",
                    __LINE__);
            return 1;
        }
    }
    CHECK(pthread_join(connector_thread, NULL) == 0 &&
            connector_woke_without_rescue &&
            accept_call.result == _EMFILE && connect_call.result == 0 &&
            unix_connector.fd->socket.unix_peer_handshake_done &&
            unix_connector.fd->socket.unix_peer == NULL,
            "AF_UNIX 满表取消唤醒真实 connect waiter");
    errno = 0;
    CHECK(fcntl(unix_accepted_host_fd, F_GETFD) == -1 &&
            errno == EBADF,
            "AF_UNIX accept 满表失败关闭 accepted host fd");
    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 8;
    unlock(&group.lock);
    socket_ref_release(&unix_connector);
    CHECK(f_close_task(&task, unix_connector_number) == 0 &&
            f_close_task(&task, unix_listener_number) == 0,
            "AF_UNIX 握手回归 socket 清理成功");
    unlink(backing_address.sun_path);

    CHECK(f_close_task(&task, socket_number) == 0 &&
            replacement_probe.calls == 1,
            "替换对象在自身表项关闭时恰好释放一次");
    CHECK(f_close_task(&task, socket_number) == _EBADF &&
            replacement_probe.calls == 1,
            "重复关闭空槽位不得再次释放替换对象");

    current = NULL;
    fdtable_release(task.files);
    mm_release(task.mm);
    CHECK(pthread_mutex_destroy(&task.waiting_cond_lock.m) == 0,
            "测试任务等待登记锁清理成功");
    CHECK(ordinary_probe.calls == 1 && replacement_probe.calls == 1,
            "夹具清理后每个普通 fd 均恰好释放一次");
    return 0;
}

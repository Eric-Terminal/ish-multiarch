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
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
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
#define UNIX_SECOND_ADDRESS (LENGTH_PAGE + UINT32_C(0x200))

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

struct bind_call {
    struct task *task;
    struct socket_ref socket;
    struct sockaddr_max_ address;
    size_t address_length;
    atomic_uint *ready;
    atomic_bool *start;
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

static void *run_bind(void *opaque) {
    struct bind_call *call = opaque;
    current = call->task;
    atomic_fetch_add_explicit(call->ready, 1, memory_order_release);
    const struct timespec interval = {.tv_nsec = 1000000};
    while (!atomic_load_explicit(call->start, memory_order_acquire))
        nanosleep(&interval, NULL);
    call->result = socket_bind_ref_task(call->task, &call->socket,
            &call->address, call->address_length);
    socket_ref_release(&call->socket);
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

static int test_resume_releases_closed_listener(void) {
    struct task task;
    struct tgroup group;
    task_fixture_init(&task, &group);
    CHECK(!IS_ERR(task.files), "恢复回归 fd 表创建成功");
    current = &task;
    fd_t number = socket_create_task(
            &task, AF_INET_, SOCK_STREAM_, 0);
    struct fd *listener = f_get_task(&task, number);
    CHECK(number == 0 && listener != NULL,
            "恢复回归监听 socket 创建成功");
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    CHECK(bind(listener->real_fd,
                    (const struct sockaddr *) &address,
                    sizeof(address)) == 0 &&
            sys_listen(number, 1) == 0,
            "恢复回归监听 socket 启动成功");
    int host_fd = listener->real_fd;
    sockrestart_on_suspend();
    CHECK(f_close_task(&task, number) == 0 &&
                    fcntl(host_fd, F_GETFD) >= 0,
            "挂起快照在 guest close 后保留监听 socket");
    sockrestart_on_resume();
    errno = 0;
    CHECK(fcntl(host_fd, F_GETFD) < 0 && errno == EBADF,
            "恢复在锁外释放最终快照引用与 host socket");
    number = socket_create_task(&task, AF_INET_, SOCK_STREAM_, 0);
    listener = f_get_task(&task, number);
    address.sin_port = 0;
    CHECK(number == 0 && listener != NULL &&
                    bind(listener->real_fd,
                            (const struct sockaddr *) &address,
                            sizeof(address)) == 0 &&
                    sys_listen(number, 1) == 0,
            "快照失败回归 listener 启动成功");
    uint64_t generation = listener->socket.listen_generation;
    struct rlimit original_limit;
    CHECK(getrlimit(RLIMIT_NOFILE, &original_limit) == 0,
            "读取宿主 fd 额度");
    struct rlimit limited = original_limit;
    if (limited.rlim_cur > 64)
        limited.rlim_cur = 64;
    CHECK(setrlimit(RLIMIT_NOFILE, &limited) == 0,
            "临时收紧宿主 fd 额度");
    int fillers[64];
    int filler_count = 0;
    while (filler_count < 64 &&
            (fillers[filler_count] = open("/dev/null", O_RDONLY)) >= 0)
        filler_count++;
    bool host_limit_reached = errno == EMFILE;
    sockrestart_on_suspend();
    lock(&listener->lock);
    bool failed_closed = listener->real_fd == -1 &&
            !listener->socket.host_listening &&
            !listener->socket.guest_listening &&
            listener->socket.listen_generation != generation;
    unlock(&listener->lock);
    sockrestart_on_resume();
    int restore_error = setrlimit(RLIMIT_NOFILE, &original_limit);
    for (int index = 0; index < filler_count; index++)
        close(fillers[index]);
    CHECK(host_limit_reached && failed_closed && restore_error == 0,
            "身份锚点遇宿主 EMFILE 时可观察地失败关闭");
    CHECK(f_close_task(&task, number) == 0,
            "关闭快照失败后的 guest listener");
    current = NULL;
    fdtable_release(task.files);
    pthread_mutex_destroy(&task.waiting_cond_lock.m);
    pthread_mutex_destroy(&group.lock.m);
    return 0;
}

int main(void) {
    if (test_resume_releases_closed_listener() != 0)
        return 1;
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
    CHECK(sys_getsockopt(99, SOL_SOCKET_, SO_DOMAIN_,
                    UNMAPPED_PAGE, UNMAPPED_PAGE) == _EBADF,
            "getsockopt 在读取用户长度前拒绝缺失 fd");
    CHECK(sys_getsockopt(0, SOL_SOCKET_, SO_DOMAIN_,
                    UNMAPPED_PAGE, UNMAPPED_PAGE) == _ENOTSOCK,
            "getsockopt 对稳定非套接字返回 ENOTSOCK");
    CHECK(sys_getsockopt(socket_number, SOL_SOCKET_, SO_DOMAIN_,
                    UNMAPPED_PAGE, UNMAPPED_PAGE) == _EFAULT,
            "getsockopt 对不可读长度地址返回 EFAULT");
    sdword_t negative_option_length = -1;
    CHECK(user_write(LENGTH_PAGE, &negative_option_length,
                    sizeof(negative_option_length)) == 0 &&
            sys_getsockopt(socket_number, SOL_SOCKET_, SO_DOMAIN_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == _EINVAL,
            "getsockopt 拒绝负的 guest optlen 且不建立无界 VLA");
    dword_t option_length = sizeof(dword_t);
    dword_t option_value = UINT32_C(0xa5a5a5a5);
    CHECK(user_write(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            user_write(ACCEPT_ADDRESS, &option_value,
                    sizeof(option_value)) == 0 &&
            sys_getsockopt(socket_number, SOL_SOCKET_, SO_DOMAIN_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == 0 &&
            user_read(ACCEPT_ADDRESS, &option_value,
                    sizeof(option_value)) == 0 &&
            user_read(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            option_value == AF_INET_ &&
            option_length == sizeof(option_value),
            "getsockopt 从 retained socket 返回 guest domain");
    byte_t short_domain[4] = {0xa5, 0xa5, 0xa5, 0xa5};
    option_length = 1;
    CHECK(user_write(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            user_write(ACCEPT_ADDRESS, short_domain,
                    sizeof(short_domain)) == 0 &&
            sys_getsockopt(socket_number, SOL_SOCKET_, SO_DOMAIN_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == 0 &&
            user_read(ACCEPT_ADDRESS, short_domain,
                    sizeof(short_domain)) == 0 &&
            user_read(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            short_domain[0] == AF_INET_ &&
            short_domain[1] == 0xa5 && option_length == 1,
            "i386 getsockopt 接受一字节 SOL_SOCKET 短缓冲");
    option_length = 8;
    CHECK(user_write(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            sys_getsockopt(socket_number, SOL_SOCKET_, SO_DOMAIN_,
                    UNMAPPED_PAGE, LENGTH_PAGE) == _EFAULT &&
            user_read(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            option_length == 8,
            "i386 SOL_SOCKET 值 fault 时不得提前改写 optlen");
    option_length = sizeof(option_value);
    option_value = 0;
    CHECK(user_write(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            sys_getsockopt(socket_number, SOL_SOCKET_, SO_PROTOCOL_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == 0 &&
            user_read(ACCEPT_ADDRESS, &option_value,
                    sizeof(option_value)) == 0 &&
            option_value == IPPROTO_TCP,
            "i386 SO_PROTOCOL 返回默认 stream 的 Linux TCP 协议号");
    char congestion = '\0';
    option_length = sizeof(congestion);
    CHECK(user_write(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            sys_getsockopt(socket_number, IPPROTO_TCP, TCP_CONGESTION_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == 0 &&
            user_read(ACCEPT_ADDRESS,
                    &congestion, sizeof(congestion)) == 0 &&
            user_read(LENGTH_PAGE, &option_length,
                    sizeof(option_length)) == 0 &&
            congestion == 'c' &&
            option_length == sizeof(congestion),
            "getsockopt 按 guest 容量截断 TCP 拥塞算法名称");
    struct sockaddr_ invalid_address = {
        .family = UINT16_C(0x7fff),
    };
    CHECK(user_write(UNIX_ADDRESS, &invalid_address,
                    sizeof(invalid_address)) == 0,
            "i386 connect 错误顺序地址写入成功");
    CHECK(sys_connect(99, UNMAPPED_PAGE, 1) == _EBADF,
            "i386 connect 在地址复制前拒绝缺失 fd");
    CHECK(sys_connect(0, UNMAPPED_PAGE, 1) == _EFAULT,
            "i386 connect 对非套接字仍先复制地址");
    CHECK(sys_connect(0, UNIX_ADDRESS,
                    sizeof(invalid_address)) == _ENOTSOCK,
            "i386 connect 在地址复制后拒绝稳定非套接字");
    CHECK(sys_connect(socket_number, UNMAPPED_PAGE, 1) == _EFAULT,
            "i386 connect 的短 sockaddr 保持复制故障优先");
    CHECK(sys_connect(socket_number, UNIX_ADDRESS, 1) == _EINVAL,
            "i386 connect 复制短 sockaddr 后由协议拒绝");
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
            sys_listen(accept_listener_number, 1) == 0,
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
    group.limits[RLIMIT_NOFILE_].cur = 4;
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

    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 3;
    unlock(&group.lock);

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
    byte_t socketpair_payload = 0x5a;
    dword_t socketpair_source_capacity = sizeof(struct sockaddr_);
    CHECK(user_write(ACCEPT_ADDRESS, &socketpair_payload,
                    sizeof(socketpair_payload)) == 0 &&
            sys_sendto(installed_pair[0], ACCEPT_ADDRESS,
                    sizeof(socketpair_payload), 0, 0, 0) ==
                    sizeof(socketpair_payload) &&
            user_write(LENGTH_PAGE, &socketpair_source_capacity,
                    sizeof(socketpair_source_capacity)) == 0 &&
            sys_recvfrom(installed_pair[1], ACCEPT_ADDRESS,
                    sizeof(socketpair_payload), 0,
                    UNIX_ADDRESS, LENGTH_PAGE) ==
                    sizeof(socketpair_payload) &&
            user_read(LENGTH_PAGE, &socketpair_source_capacity,
                    sizeof(socketpair_source_capacity)) == 0 &&
            socketpair_source_capacity == 0,
            "AF_UNIX stream 的零长度源地址不得在消费数据后变成 EINVAL");
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

    struct {
        sdword_t seconds;
        sdword_t microseconds;
    } i386_timeout = {1, 250000};
    CHECK(user_write(UNIX_SECOND_ADDRESS, &i386_timeout,
                    sizeof(i386_timeout)) == 0 &&
            sys_setsockopt(unix_listener_number,
                    SOL_SOCKET_, SO_RCVTIMEO_OLD_,
                    UNIX_SECOND_ADDRESS, sizeof(i386_timeout)) == 0,
            "i386 旧 timeval32 socket option 按八字节解析");
    struct timeval host_timeout = {0};
    socklen_t host_timeout_length = sizeof(host_timeout);
    CHECK(getsockopt(unix_listener->real_fd,
                    SOL_SOCKET, SO_RCVTIMEO,
                    &host_timeout, &host_timeout_length) == 0 &&
            host_timeout.tv_sec == 1 && host_timeout.tv_usec == 250000,
            "i386 timeval32 已显式转换到 host timeval");
    dword_t timeout_length = sizeof(i386_timeout);
    struct {
        int64_t seconds;
        int64_t microseconds;
    } i386_timeout_new = {0};
    CHECK(user_write(LENGTH_PAGE, &timeout_length,
                    sizeof(timeout_length)) == 0 &&
            sys_getsockopt(unix_listener_number,
                    SOL_SOCKET_, SO_RCVTIMEO_OLD_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == 0 &&
            user_read(ACCEPT_ADDRESS, &i386_timeout,
                    sizeof(i386_timeout)) == 0 &&
            user_read(LENGTH_PAGE, &timeout_length,
                    sizeof(timeout_length)) == 0 &&
            timeout_length == sizeof(i386_timeout) &&
            i386_timeout.seconds == 1 &&
            i386_timeout.microseconds == 250000,
            "i386 OLD timeout getter 返回八字节 timeval32 wire");
    timeout_length = sizeof(i386_timeout_new);
    CHECK(user_write(LENGTH_PAGE, &timeout_length,
                    sizeof(timeout_length)) == 0 &&
            sys_getsockopt(unix_listener_number,
                    SOL_SOCKET_, SO_RCVTIMEO_NEW_,
                    ACCEPT_ADDRESS, LENGTH_PAGE) == 0 &&
            user_read(ACCEPT_ADDRESS, &i386_timeout_new,
                    sizeof(i386_timeout_new)) == 0 &&
            user_read(LENGTH_PAGE, &timeout_length,
                    sizeof(timeout_length)) == 0 &&
            timeout_length == sizeof(i386_timeout_new) &&
            i386_timeout_new.seconds == 1 &&
            i386_timeout_new.microseconds == 250000,
            "i386 NEW timeout getter 扩展为十六字节 timeval64 wire");

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
            unix_listener->socket.unix_name_len ==
                    guest_unix_address_length -
                    offsetof(struct sockaddr_max_, data) &&
            memcmp(unix_listener->socket.unix_name,
                    guest_unix_address.data,
                    unix_listener->socket.unix_name_len) == 0,
            "AF_UNIX 抽象监听名称绑定并保存成功");

    struct sockaddr_max_ second_unix_address = {
        .family = AF_LOCAL_,
    };
    snprintf(second_unix_address.data + 1,
            sizeof(second_unix_address.data) - 1,
            "rollback-%ld", (long) getpid());
    size_t second_unix_address_length =
            offsetof(struct sockaddr_max_, data) + 1 +
            strlen(second_unix_address.data + 1);
    CHECK(user_write(UNIX_SECOND_ADDRESS, &second_unix_address,
                    second_unix_address_length) == 0 &&
            sys_bind(unix_listener_number, UNIX_SECOND_ADDRESS,
                    (uint_t) second_unix_address_length) < 0 &&
            unix_listener->socket.unix_name_len ==
                    guest_unix_address_length -
                    offsetof(struct sockaddr_max_, data) &&
            memcmp(unix_listener->socket.unix_name,
                    guest_unix_address.data,
                    unix_listener->socket.unix_name_len) == 0,
            "AF_UNIX 重复 bind 失败回滚新名称且保留原名称");
    int_t rollback_socket_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_STREAM_, 0);
    CHECK(rollback_socket_number == 4 &&
            sys_bind(rollback_socket_number, UNIX_SECOND_ADDRESS,
                    (uint_t) second_unix_address_length) == 0,
            "AF_UNIX 失败事务释放名称供新 socket 立即绑定");
    struct fd *rollback_socket = f_get_task(
            &task, rollback_socket_number);
    struct sockaddr_un rollback_backing = {0};
    socklen_t rollback_backing_length = sizeof(rollback_backing);
    CHECK(rollback_socket != NULL &&
            getsockname(rollback_socket->real_fd,
                    (struct sockaddr *) &rollback_backing,
                    &rollback_backing_length) == 0 &&
            f_close_task(&task, rollback_socket_number) == 0,
            "AF_UNIX 回滚验证 socket 关闭成功");
    errno = 0;
    CHECK(lstat(rollback_backing.sun_path, &(struct stat) {0}) == -1 &&
            errno == ENOENT,
            "AF_UNIX 最终关闭按身份删除内部 host 后备节点");

    struct sockaddr_ autobind_address = {.family = AF_LOCAL_};
    CHECK(user_write(UNIX_SECOND_ADDRESS, &autobind_address,
                    sizeof(autobind_address.family)) == 0,
            "AF_UNIX autobind 地址写入成功");
    int_t autobind_socket_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_DGRAM_, 0);
    CHECK(autobind_socket_number == 4 &&
            sys_bind(autobind_socket_number, UNIX_SECOND_ADDRESS,
                    sizeof(autobind_address.family)) == 0,
            "AF_UNIX addrlen=2 执行 Linux autobind");
    struct fd *autobind_socket = f_get_task(
            &task, autobind_socket_number);
    CHECK(autobind_socket != NULL &&
            autobind_socket->socket.unix_name_len == 6 &&
            autobind_socket->socket.unix_name[0] == '\0',
            "AF_UNIX autobind 保存 NUL 加五位名称");
    char autobind_name[6];
    memcpy(autobind_name, autobind_socket->socket.unix_name,
            sizeof(autobind_name));
    CHECK(sys_bind(autobind_socket_number, UNIX_SECOND_ADDRESS,
                    sizeof(autobind_address.family)) == 0 &&
            autobind_socket->socket.unix_name_len == sizeof(autobind_name) &&
            memcmp(autobind_socket->socket.unix_name,
                    autobind_name, sizeof(autobind_name)) == 0,
            "AF_UNIX 已绑定 socket 的 autobind 为幂等成功");
    struct sockaddr_un autobind_backing = {0};
    socklen_t autobind_backing_length = sizeof(autobind_backing);
    CHECK(getsockname(autobind_socket->real_fd,
                    (struct sockaddr *) &autobind_backing,
                    &autobind_backing_length) == 0 &&
            f_close_task(&task, autobind_socket_number) == 0,
            "AF_UNIX autobind socket 清理成功");
    errno = 0;
    CHECK(lstat(autobind_backing.sun_path, &(struct stat) {0}) == -1 &&
            errno == ENOENT,
            "AF_UNIX autobind 内部 host 后备节点已清理");

    int_t concurrent_socket_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_DGRAM_, 0);
    CHECK(concurrent_socket_number == 4,
            "AF_UNIX 并发 bind 测试 socket 创建成功");
    atomic_uint bind_ready;
    atomic_bool bind_start;
    atomic_init(&bind_ready, 0);
    atomic_init(&bind_start, false);
    struct bind_call bind_calls[2] = {0};
    for (size_t index = 0; index < 2; index++) {
        bind_calls[index].task = &task;
        bind_calls[index].address.family = AF_LOCAL_;
        snprintf(bind_calls[index].address.data + 1,
                sizeof(bind_calls[index].address.data) - 1,
                "bind-race-%zu-%ld", index, (long) getpid());
        bind_calls[index].address_length =
                offsetof(struct sockaddr_max_, data) + 1 +
                strlen(bind_calls[index].address.data + 1);
        bind_calls[index].ready = &bind_ready;
        bind_calls[index].start = &bind_start;
        bind_calls[index].result = _EIO;
        CHECK(socket_ref_get_task(&task, concurrent_socket_number,
                        &bind_calls[index].socket) == 0,
                "AF_UNIX 并发 bind 取得独立强引用");
    }
    pthread_t bind_threads[2];
    CHECK(pthread_create(&bind_threads[0], NULL,
                    run_bind, &bind_calls[0]) == 0 &&
            pthread_create(&bind_threads[1], NULL,
                    run_bind, &bind_calls[1]) == 0,
            "AF_UNIX 并发 bind 线程启动成功");
    const struct timespec bind_interval = {.tv_nsec = 1000000};
    for (unsigned elapsed = 0; elapsed < 1000 &&
            atomic_load_explicit(&bind_ready, memory_order_acquire) != 2;
            elapsed++)
        nanosleep(&bind_interval, NULL);
    bool both_binds_ready = atomic_load_explicit(
            &bind_ready, memory_order_acquire) == 2;
    atomic_store_explicit(&bind_start, true, memory_order_release);
    int first_bind_join = pthread_join(bind_threads[0], NULL);
    int second_bind_join = pthread_join(bind_threads[1], NULL);
    CHECK(both_binds_ready && first_bind_join == 0 &&
            second_bind_join == 0,
            "AF_UNIX 两个 bind 在同一起点后及时退出");
    int winner = bind_calls[0].result == 0 ? 0 : 1;
    int loser = 1 - winner;
    struct fd *concurrent_socket = f_get_task(
            &task, concurrent_socket_number);
    CHECK(bind_calls[winner].result == 0 &&
            bind_calls[loser].result == _EINVAL &&
            concurrent_socket != NULL &&
            concurrent_socket->socket.unix_name_len ==
                    bind_calls[winner].address_length -
                    offsetof(struct sockaddr_max_, data) &&
            memcmp(concurrent_socket->socket.unix_name,
                    bind_calls[winner].address.data,
                    concurrent_socket->socket.unix_name_len) == 0,
            "AF_UNIX 并发 bind 只提交赢家名称和元数据");
    int_t loser_socket_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_DGRAM_, 0);
    struct socket_ref loser_socket;
    CHECK(loser_socket_number == 5 &&
            socket_ref_get_task(&task, loser_socket_number,
                    &loser_socket) == 0 &&
            socket_bind_ref_task(&task, &loser_socket,
                    &bind_calls[loser].address,
                    bind_calls[loser].address_length) == 0,
            "AF_UNIX 并发败者的预留名称立即可重用");
    socket_ref_release(&loser_socket);
    CHECK(f_close_task(&task, loser_socket_number) == 0 &&
            f_close_task(&task, concurrent_socket_number) == 0,
            "AF_UNIX 并发 bind 测试 socket 清理成功");

    CHECK(sys_listen(unix_listener_number, 1) == 0,
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

    // 卡住 RLIMIT 快照，验证 connect 不依赖应用层 accept 完成。
    lock(&group.lock);
    CHECK(pthread_create(&connector_thread, NULL,
                    run_connect, &connect_call) == 0,
            "启动 AF_UNIX 连接等待线程");
    CHECK(wait_for_listener(unix_listener->real_fd, 1000),
            "AF_UNIX 连接进入 host accept 队列");
    CHECK(wait_for_completion(&connect_call.finished, 1000) &&
            pthread_join(connector_thread, NULL) == 0 &&
            connect_call.result == 0,
            "AF_UNIX stream connect 在进入 backlog 后立即返回");
    CHECK(pthread_create(&accept_thread, NULL,
                    run_accept, &accept_call) == 0,
            "启动 AF_UNIX accept 线程");
    CHECK(wait_for_listener(unix_listener->real_fd, 1000) &&
            !atomic_load_explicit(
                    &accept_call.finished, memory_order_acquire),
            "AF_UNIX accept 在预留前等待且未消费 pending 连接");
    unlock(&group.lock);

    CHECK(wait_for_completion(&accept_call.finished, 1000) &&
            pthread_join(accept_thread, NULL) == 0,
            "AF_UNIX accept 满表路径及时返回");
    CHECK(accept_call.result == _EMFILE &&
            unix_connector.fd->socket.unix_peer == NULL &&
            wait_for_listener(unix_listener->real_fd, 1000),
            "AF_UNIX 满表失败保留 pending 连接与在途 peer 引用");
    lock(&group.lock);
    group.limits[RLIMIT_NOFILE_].cur = 8;
    unlock(&group.lock);
    fd_t unix_accepted_number =
            sys_accept(unix_listener_number, 0, 0);
    CHECK(unix_accepted_number >= 0 &&
            unix_connector.fd->socket.unix_peer != NULL,
            "放宽 fd 上限后接受原 pending 连接");
    CHECK(f_close_task(&task, unix_accepted_number) == 0,
            "AF_UNIX 满表回归 accepted socket 清理成功");
    errno = 0;
    CHECK(fcntl(unix_accepted_host_fd, F_GETFD) == -1 &&
            errno == EBADF,
            "AF_UNIX accepted host fd 随 guest close 释放");
    struct socket_address retained_abstract_name;
    struct socket_ref listener_name_socket;
    CHECK(socket_ref_get_task(&task, unix_listener_number,
                    &listener_name_socket) == 0 &&
            socket_address_prepare_task(&task, &listener_name_socket,
                    &guest_unix_address, guest_unix_address_length,
                    &retained_abstract_name) == 0,
            "AF_UNIX 关闭重绑前保留旧名称查询令牌");
    socket_ref_release(&listener_name_socket);
    socket_ref_release(&unix_connector);
    CHECK(f_close_task(&task, unix_connector_number) == 0 &&
            f_close_task(&task, unix_listener_number) == 0,
            "AF_UNIX 握手回归 socket 清理成功");
    errno = 0;
    CHECK(lstat(backing_address.sun_path, &(struct stat) {0}) == -1 &&
            errno == ENOENT,
            "AF_UNIX 监听 socket 最终关闭清理内部 host 后备节点");
    int_t rebound_socket_number = socket_create_task(
            &task, AF_LOCAL_, SOCK_STREAM_, 0);
    struct socket_ref rebound_socket;
    CHECK(rebound_socket_number == unix_listener_number &&
            socket_ref_get_task(&task, rebound_socket_number,
                    &rebound_socket) == 0 &&
            socket_bind_ref_task(&task, &rebound_socket,
                    &guest_unix_address,
                    guest_unix_address_length) == 0,
            "AF_UNIX owner 关闭后不受旧 lookup 强引用阻塞同名重绑");
    socket_ref_release(&rebound_socket);
    CHECK(f_close_task(&task, rebound_socket_number) == 0,
            "AF_UNIX 同名重绑 socket 清理成功");
    socket_address_release(&retained_abstract_name);

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

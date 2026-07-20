#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "util/timer.h"
#include "debug.h"

#define SOCKET_TYPE_MASK 0xf
#define I386_LINUX_MAX_RW_COUNT UINT32_C(0x7ffff000)
#define I386_LINUX_IOV_MAX UINT32_C(1024)
#define I386_SOCKET_CONTROL_LIMIT UINT32_C(2048)
#define I386_SCM_MAX_FDS SOCKET_SCM_MAX_FDS
#define I386_LINUX_MSG_CMSG_CLOEXEC UINT32_C(0x40000000)
#define I386_LINUX_MSG_CMSG_COMPAT UINT32_C(0x80000000)

const struct fd_ops socket_fdops;

static lock_t peer_lock = LOCK_INITIALIZER;
static lock_t unix_bound_lock = LOCK_INITIALIZER;
// 数据报发送、读关闭、host dummy 与 SCM 队列共享同一线性化顺序。
static lock_t unix_scm_io_lock = LOCK_INITIALIZER;
static lock_t unix_scm_dummy_lock = LOCK_INITIALIZER;
static int unix_scm_dummy_fd = -1;
static struct list unix_scm_inflight =
        LIST_INITIALIZER(unix_scm_inflight);
static struct list unix_scm_vertices =
        LIST_INITIALIZER(unix_scm_vertices);
static atomic_bool unix_scm_gc_requested = ATOMIC_VAR_INIT(false);
static atomic_bool unix_scm_gc_running = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t unix_scm_edge_generation =
        ATOMIC_VAR_INIT(0);
static struct list unix_bound_sockets;

struct unix_bound_name;

struct unix_pending_peer {
    struct list links;
    struct fd *peer;
    struct fd *listener;
    bool connect_active;
    bool host_error_pending;
    bool accepted;
    bool cancel_requested;
    bool linked;
    bool handshake_finished;
    uint64_t generation;
    char peer_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    bool listener_cred_valid;
    struct ucred_ listener_cred;
};

struct unix_bound_name {
    struct list links;
    size_t queued_messages;
    size_t active_connects;
    bool owner_open;
    bool closing;
    bool listener_cred_valid;
    struct ucred_ listener_cred;
    struct fd *owner;
    uint8_t name_length;
    char name[SOCKADDR_DATA_MAX + 1];
    char backing_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    struct list pending_peers;
    cond_t connects_drained;
};

struct unix_scm_receiver {
    struct fd *owner;
    struct fd *logical;
    struct fd *queued;
    bool pending;
};

static void unix_socket_finish_peer_handshake(
        struct fd *peer, struct fd *accepted);
void socket_scm_release(struct scm *scm);
static int unix_scm_receiver_retain(struct fd *sender,
        const struct socket_address *address,
        struct unix_scm_receiver *receiver);
static void unix_scm_receiver_release(
        struct unix_scm_receiver *receiver);
static bool socket_message_may_wait(struct fd *socket, dword_t flags);
static bool socket_message_send_would_block(
        struct fd *socket, int host_error);
static const struct timer_time *socket_message_deadline(
        struct fd *socket, bool receive,
        struct timer_time *deadline);
static int socket_message_wait(struct fd *socket, int events,
        const struct timer_time *deadline);
static int socket_message_send_error(int error, dword_t flags);
static int socket_message_send_host_error(
        struct fd *socket, int host_error, dword_t flags);
static bool close_host_scm_rights(struct msghdr *message);
static int unix_bound_drain_messages_locked(
        struct fd *fd, struct list *discarded_scm);
static void socket_scm_release_list(struct list *scm_list);
static void socket_scm_unpublish_locked(struct scm *scm);

static struct fd *sock_fd_allocate(
        int domain, int type, int protocol, int guest_protocol) {
    struct fd *fd = adhoc_fd_create(&socket_fdops);
    if (fd == NULL)
        return NULL;
    fd->stat.mode = S_IFSOCK | 0666;
    fd->socket.domain = domain;
    fd->socket.type = type & SOCKET_TYPE_MASK;
    fd->socket.protocol = protocol;
    fd->socket.guest_protocol = guest_protocol;
    if (domain == AF_LOCAL_) {
        list_init(&fd->socket.unix_scm);
        list_init(&fd->socket.unix_pending_scm);
        list_init(&fd->socket.unix_scm_vertex);
        atomic_init(&fd->socket.unix_scm_incoming, 0);
        lock_init(&fd->socket.unix_recv_lock);
    }
    return fd;
}

static void sock_configure_host_fd(int sock_fd) {
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    (void) setsockopt(sock_fd, SOL_SOCKET,
            SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#else
    (void) sock_fd;
#endif
}

static int unix_scm_dummy_get(void) {
    lock(&unix_scm_dummy_lock);
    if (unix_scm_dummy_fd < 0)
        unix_scm_dummy_fd = open(".", O_RDONLY | O_CLOEXEC);
    int result = unix_scm_dummy_fd < 0 ? errno_map() : unix_scm_dummy_fd;
    unlock(&unix_scm_dummy_lock);
    return result;
}

static struct fd *sock_fd_wrap(
        int sock_fd, int domain, int type,
        int protocol, int guest_protocol) {
    // 进入本函数后无条件接管 raw host fd，任何失败路径都在这里释放。
    sock_configure_host_fd(sock_fd);
    struct fd *fd = sock_fd_allocate(
            domain, type, protocol, guest_protocol);
    if (fd == NULL) {
        close(sock_fd);
        return NULL;
    }
    fd->real_fd = sock_fd;
    return fd;
}

static fd_t sock_fd_create_task(struct task *task,
        int sock_fd, int domain, int type,
        int protocol, int guest_protocol) {
    struct fd *fd = sock_fd_wrap(
            sock_fd, domain, type, protocol, guest_protocol);
    if (fd == NULL)
        return _ENOMEM;
    return f_install_task(task, fd, type & ~SOCKET_TYPE_MASK);
}

int_t socket_create_task(struct task *task,
        dword_t domain, dword_t type, dword_t protocol) {
    dword_t flags = type & ~SOCKET_TYPE_MASK;
    if ((flags & ~(SOCK_NONBLOCK_ | SOCK_CLOEXEC_)) != 0)
        return _EINVAL;
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EAFNOSUPPORT;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return _EINVAL;

    int guest_protocol = (int) protocol;
    if (protocol == 0 &&
            (domain == AF_INET_ || domain == AF_INET6_)) {
        if ((type & SOCKET_TYPE_MASK) == SOCK_STREAM_)
            guest_protocol = IPPROTO_TCP;
        else if ((type & SOCKET_TYPE_MASK) == SOCK_DGRAM_)
            guest_protocol = IPPROTO_UDP;
    }

    // host 的受限 raw socket 适配不能改变 guest 可见协议。
    int host_protocol = (int) protocol;
    if ((type & SOCKET_TYPE_MASK) == SOCK_RAW_ &&
            protocol == IPPROTO_RAW)
        host_protocol = IPPROTO_ICMP;

    int sock = socket(real_domain, real_type, host_protocol);
    if (sock < 0)
        return errno_map();

#ifdef __APPLE__
    if (domain == AF_INET_ && type == SOCK_DGRAM_) {
        // in some cases, such as ICMP, datagram sockets on mac can default to
        // including the IP header like raw sockets
        int one = 1;
        setsockopt(sock, IPPROTO_IP, IP_STRIPHDR, &one, sizeof(one));
    }
#endif

    fd_t f = sock_fd_create_task(task, sock, domain, type,
            host_protocol, guest_protocol);
    return f;
}

int_t sys_socket(dword_t domain, dword_t type, dword_t protocol) {
    STRACE("socket(%d, %d, %d)", domain, type, protocol);
    return socket_create_task(current, domain, type, protocol);
}

static void inode_release_if_exist(struct inode_data *inode) {
    if (inode != NULL)
        inode_release(inode);
}

int socket_ref_get_task(struct task *task, fd_t sock_fd,
        struct socket_ref *socket) {
    assert(task != NULL && socket != NULL);
    socket->fd = NULL;
    struct fd *fd = f_get_task_retain(task, sock_fd);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &socket_fdops) {
        fd_close(fd);
        return _ENOTSOCK;
    }
    socket->fd = fd;
    return 0;
}

void socket_ref_release(struct socket_ref *socket) {
    assert(socket != NULL && socket->fd != NULL);
    struct fd *fd = socket->fd;
    socket->fd = NULL;
    fd_close(fd);
}

static uint32_t unix_socket_next_id(void) {
    static uint32_t next_id = 0;
    static lock_t next_id_lock = LOCK_INITIALIZER;
    lock(&next_id_lock);
    uint32_t id = ++next_id;
    unlock(&next_id_lock);
    return id;
}

static int unix_socket_lookup_path_task(struct task *task,
        const char *path_raw, uint32_t *socket_id,
        struct inode_data **retained) {
    char path[MAX_PATH];
    int err = path_normalize_task(
            task, AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);

    if (err < 0)
        goto out;

    if (!S_ISSOCK(stat.mode)) {
        // 路径存在但没有可连接的 Unix socket，Linux 返回 ECONNREFUSED。
        err = _ECONNREFUSED;
        goto out;
    }

    // Look up the socket ID for the inode number.
    struct inode_data *inode = inode_get(
            mount, stat.inode_device, stat.inode);
    lock(&inode->lock);
    if (inode->socket_id == 0)
        inode->socket_id = unix_socket_next_id();
    unlock(&inode->lock);
    *socket_id = inode->socket_id;
    if (retained != NULL)
        *retained = inode;
    else
        inode_release(inode);
    mount_release(mount);
    return 0;

out:
    mount_release(mount);
    return err;
}

// 抽象名称允许内嵌 NUL，哈希和比较都必须使用调用方给出的精确长度。
static uint32_t bytes_hash(const char *bytes, size_t length) {
    uint32_t hash = 5381;
    for (size_t index = 0; index < length; index++)
        hash = 33 * hash ^ (byte_t) bytes[index];
    return hash;
}

// The abstract socket namespace is a lot simpler than it sounds: if the first
// byte of the path is a null byte, then it gets looked up in this hashtable
// instead of the filesystem.

struct unix_abstract {
    unsigned refcount;
    uint32_t hash;
    uint32_t socket_id;
    size_t name_length;
    int socket_type;
    bool linked;
    bool published;
    char name[SOCKADDR_DATA_MAX];
    struct list links;
};
#define ABSTRACT_HASH_SIZE 1024
static struct list abstract_hash[ABSTRACT_HASH_SIZE];
static lock_t unix_abstract_lock = LOCK_INITIALIZER;

static struct unix_abstract *unix_abstract_find_locked(
        const char *name, size_t name_length,
        int socket_type, uint32_t hash) {
    struct unix_abstract *sock_tmp;
    struct list *bucket = &abstract_hash[hash % ABSTRACT_HASH_SIZE];
    if (list_null(bucket))
        list_init(bucket);
    list_for_each_entry(bucket, sock_tmp, links) {
        if (sock_tmp->hash == hash &&
                sock_tmp->socket_type == socket_type &&
                sock_tmp->name_length == name_length &&
                memcmp(sock_tmp->name, name, name_length) == 0)
            return sock_tmp;
    }
    return NULL;
}

static int unix_abstract_lookup(
        const char *name, size_t name_length, int socket_type,
        uint32_t *socket_id,
        struct unix_abstract **retained) {
    uint32_t hash = 33 * bytes_hash(name, name_length) ^
            (uint32_t) socket_type;
    lock(&unix_abstract_lock);
    struct unix_abstract *sock = unix_abstract_find_locked(
            name, name_length, socket_type, hash);
    // 预留只排斥其他 bind；host bind 成功前不向 connect 发布。
    if (sock == NULL || !sock->published) {
        unlock(&unix_abstract_lock);
        return _ECONNREFUSED;
    }
    if (retained != NULL) {
        assert(sock->refcount != UINT_MAX);
        sock->refcount++;
        *retained = sock;
    }
    *socket_id = sock->socket_id;
    unlock(&unix_abstract_lock);
    return 0;
}

static int unix_abstract_reserve(const char *name, size_t name_length,
        int socket_type,
        struct unix_abstract **reserved, uint32_t *socket_id) {
    uint32_t hash = 33 * bytes_hash(name, name_length) ^
            (uint32_t) socket_type;
    lock(&unix_abstract_lock);
    if (unix_abstract_find_locked(
            name, name_length, socket_type, hash) != NULL) {
        unlock(&unix_abstract_lock);
        return _EADDRINUSE;
    }
    struct unix_abstract *sock = malloc(sizeof(*sock));
    if (sock == NULL) {
        unlock(&unix_abstract_lock);
        return _ENOMEM;
    }
    *sock = (struct unix_abstract) {
        .refcount = 1,
        .hash = hash,
        .socket_id = unix_socket_next_id(),
        .name_length = name_length,
        .socket_type = socket_type,
        .linked = true,
    };
    memcpy(sock->name, name, name_length);
    struct list *bucket = &abstract_hash[hash % ABSTRACT_HASH_SIZE];
    list_add(bucket, &sock->links);
    *reserved = sock;
    *socket_id = sock->socket_id;
    unlock(&unix_abstract_lock);
    return 0;
}

static void unix_abstract_publish(struct unix_abstract *name) {
    lock(&unix_abstract_lock);
    assert(name->linked && !name->published && name->refcount != 0);
    name->published = true;
    unlock(&unix_abstract_lock);
}

static void unix_abstract_release(struct unix_abstract *name) {
    lock(&unix_abstract_lock);
    assert(name->refcount != 0);
    if (--name->refcount == 0) {
        assert(!name->linked);
        free(name);
    }
    unlock(&unix_abstract_lock);
}

static void unix_abstract_unpublish(struct unix_abstract *name) {
    lock(&unix_abstract_lock);
    assert(name->refcount != 0 && name->linked);
    list_remove(&name->links);
    name->linked = false;
    name->published = false;
    if (--name->refcount == 0)
        free(name);
    unlock(&unix_abstract_lock);
}

struct unix_bind_transaction {
    struct inode_data *inode;
    struct unix_abstract *abstract;
    struct mount *created_mount;
    bool created_path;
    qword_t created_device;
    qword_t created_host_inode;
    qword_t created_inode;
    char path[MAX_PATH];
    uint8_t name_length;
    char name[SOCKADDR_DATA_MAX + 1];
    struct unix_bound_name *bound_name;
};

static int unix_socket_reserve_path_task(struct task *task,
        const char *path_raw, struct unix_bind_transaction *transaction,
        uint32_t *socket_id) {
    char path[MAX_PATH];
    int err = path_normalize_task(
            task, AT_PWD, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    transaction->created_mount = mount;

    struct statbuf stat = {0};
    err = mount->fs->stat(mount, path, &stat);
    if (err == 0)
        return _EADDRINUSE;
    if (err != _ENOENT)
        return err;

    mode_t_ mode = 0777;
    struct fs_info *fs = task->fs;
    lock(&fs->lock);
    mode &= ~fs->umask;
    unlock(&fs->lock);
    if (mount->fs->mknod_identity == NULL)
        return _EPERM;
    err = mount->fs->mknod_identity(mount, path,
            S_IFSOCK | mode, 0,
            &transaction->created_device,
            &transaction->created_host_inode,
            &transaction->created_inode);
    if (err < 0)
        return err;
    transaction->created_path = true;
    strcpy(transaction->path, path);
    transaction->inode = inode_get(
            mount, transaction->created_device,
            transaction->created_inode);
    if (transaction->inode == NULL)
        return _ENOMEM;
    lock(&transaction->inode->lock);
    if (transaction->inode->socket_id == 0)
        transaction->inode->socket_id = unix_socket_next_id();
    *socket_id = transaction->inode->socket_id;
    unlock(&transaction->inode->lock);
    return 0;
}

static void unix_bind_rollback(
        struct unix_bind_transaction *transaction) {
    if (transaction->created_path) {
        assert(transaction->created_mount->fs->unlink_if_identity != NULL);
        int error = transaction->created_mount->fs->unlink_if_identity(
                transaction->created_mount, transaction->path,
                transaction->created_device,
                transaction->created_host_inode,
                transaction->created_inode);
        if (error < 0)
            TRACE("回滚 Unix socket 名称失败：%d\n", error);
    }
    inode_release_if_exist(transaction->inode);
    if (transaction->abstract != NULL)
        unix_abstract_unpublish(transaction->abstract);
    if (transaction->bound_name != NULL) {
        cond_destroy(&transaction->bound_name->connects_drained);
        free(transaction->bound_name);
    }
    if (transaction->created_mount != NULL)
        mount_release(transaction->created_mount);
    *transaction = (struct unix_bind_transaction) {0};
}

static void unix_bind_commit(struct unix_bind_transaction *transaction,
        struct fd *socket) {
    assert(socket->socket.unix_name_inode == NULL &&
            socket->socket.unix_name_abstract == NULL);
    assert(transaction->bound_name != NULL);
    lock(&unix_bound_lock);
    if (list_null(&unix_bound_sockets))
        list_init(&unix_bound_sockets);
    socket->socket.unix_name_inode = transaction->inode;
    socket->socket.unix_name_abstract = transaction->abstract;
    socket->socket.unix_name_len = transaction->name_length;
    memcpy(socket->socket.unix_name,
            transaction->name, transaction->name_length);
    socket->socket.unix_bound_name = transaction->bound_name;
    transaction->bound_name->owner = socket;
    transaction->bound_name->owner_open = true;
    list_add(&unix_bound_sockets, &transaction->bound_name->links);
    unlock(&unix_bound_lock);
    // 同一临界区内先完整写入 socket 元数据，再对 connect 发布名称。
    if (transaction->abstract != NULL)
        unix_abstract_publish(transaction->abstract);
    transaction->inode = NULL;
    transaction->abstract = NULL;
    transaction->bound_name = NULL;
    if (transaction->created_mount != NULL)
        mount_release(transaction->created_mount);
    *transaction = (struct unix_bind_transaction) {0};
}

static void unix_bound_maybe_free_locked(struct unix_bound_name *name) {
    if (name->owner_open || name->queued_messages != 0 ||
            name->active_connects != 0 ||
            !list_empty(&name->pending_peers))
        return;
    list_remove(&name->links);
    cond_destroy(&name->connects_drained);
    free(name);
}

static struct unix_bound_name *unix_bound_message_begin(struct fd *socket) {
    if (socket->socket.domain != AF_LOCAL_ ||
            socket->socket.type == SOCK_STREAM_)
        return NULL;
    lock(&unix_bound_lock);
    struct unix_bound_name *name = socket->socket.unix_bound_name;
    if (name != NULL) {
        assert(name->owner_open && name->queued_messages != SIZE_MAX);
        name->queued_messages++;
    }
    unlock(&unix_bound_lock);
    return name;
}

static void unix_bound_message_cancel(struct unix_bound_name *name) {
    if (name == NULL)
        return;
    lock(&unix_bound_lock);
    assert(name->queued_messages != 0);
    name->queued_messages--;
    unix_bound_maybe_free_locked(name);
    unlock(&unix_bound_lock);
}

static bool unix_bound_owner_begin_close(struct fd *socket) {
    lock(&unix_bound_lock);
    struct unix_bound_name *name = socket->socket.unix_bound_name;
    if (name != NULL) {
        assert(name->owner_open && !name->closing);
        name->closing = true;
    }
    unlock(&unix_bound_lock);
    return name != NULL && socket->socket.domain == AF_LOCAL_ &&
            socket->socket.type == SOCK_STREAM_;
}

static void unix_bound_owner_finish_close(struct fd *socket) {
    struct list canceled_peers;
    list_init(&canceled_peers);

    lock(&unix_bound_lock);
    struct unix_bound_name *name = socket->socket.unix_bound_name;
    socket->socket.unix_bound_name = NULL;
    if (name != NULL) {
        assert(name->owner_open && name->closing);
        while (name->active_connects != 0)
            (void) wait_for_ignore_signals(
                    &name->connects_drained, &unix_bound_lock, NULL);
        while (!list_empty(&name->pending_peers)) {
            struct unix_pending_peer *pending = list_first_entry(
                    &name->pending_peers,
                    struct unix_pending_peer, links);
            assert(pending->linked && !pending->connect_active &&
                    !pending->accepted && pending->peer != NULL);
            list_remove(&pending->links);
            pending->linked = false;
            list_add_tail(&canceled_peers, &pending->links);
        }
        name->owner_open = false;
        name->owner = NULL;
        unix_bound_maybe_free_locked(name);
    }
    unlock(&unix_bound_lock);

    while (!list_empty(&canceled_peers)) {
        struct unix_pending_peer *pending = list_first_entry(
                &canceled_peers, struct unix_pending_peer, links);
        list_remove(&pending->links);
        struct fd *peer = pending->peer;
        pending->peer = NULL;
        unix_socket_finish_peer_handshake(peer, NULL);
    }
}

static bool unix_bound_name_copy(const char *backing_path,
        void *guest_address, uint_t *guest_length, bool consume) {
    lock(&unix_bound_lock);
    if (list_null(&unix_bound_sockets)) {
        unlock(&unix_bound_lock);
        return false;
    }
    struct unix_bound_name *name;
    list_for_each_entry(&unix_bound_sockets, name, links) {
        assert(name != NULL);
        if (strcmp(name->backing_path, backing_path) != 0)
            continue;
        struct sockaddr_ *guest = guest_address;
        guest->family = PF_LOCAL_;
        memcpy((byte_t *) guest_address + offsetof(struct sockaddr_, data),
                name->name, name->name_length);
        *guest_length = (uint_t) (offsetof(struct sockaddr_, data) +
                name->name_length);
        if (consume && name->queued_messages != 0) {
            name->queued_messages--;
            unix_bound_maybe_free_locked(name);
        }
        unlock(&unix_bound_lock);
        return true;
    }
    unlock(&unix_bound_lock);
    return false;
}

const char *sock_tmp_prefix = "/tmp/ishsock";

static int unix_socket_transport_path(struct fd *socket,
        char path[sizeof(((struct sockaddr_un *) 0)->sun_path)]) {
    assert(socket != NULL && socket->socket.domain == AF_LOCAL_);
    lock(&socket->lock);
    if (socket->socket.unix_backing_path[0] != '\0') {
        strcpy(path, socket->socket.unix_backing_path);
        unlock(&socket->lock);
        return 0;
    }

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };
    int bind_error = EADDRINUSE;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        uint32_t socket_id = unix_socket_next_id();
        int path_length = snprintf(address.sun_path,
                sizeof(address.sun_path), "%s%d.%u",
                sock_tmp_prefix, getpid(), socket_id);
        if (path_length < 0 ||
                (size_t) path_length >= sizeof(address.sun_path)) {
            unlock(&socket->lock);
            return _ENAMETOOLONG;
        }
        socklen_t address_length = (socklen_t) (
                offsetof(struct sockaddr_un, sun_path) + path_length);
#ifdef __APPLE__
        address.sun_len = (uint8_t) address_length;
#endif
        if (bind(socket->real_fd,
                (const struct sockaddr *) &address,
                address_length) == 0) {
            bind_error = 0;
            break;
        }
        bind_error = errno;
        if (bind_error != EADDRINUSE)
            break;
    }
    if (bind_error != 0) {
        unlock(&socket->lock);
        return err_map(bind_error);
    }

    strcpy(socket->socket.unix_backing_path, address.sun_path);
    struct stat host_stat;
    if (lstat(address.sun_path, &host_stat) == 0) {
        socket->socket.unix_backing_owned = true;
        socket->socket.unix_backing_device =
                (uint64_t) host_stat.st_dev;
        socket->socket.unix_backing_inode =
                (uint64_t) host_stat.st_ino;
    }
    strcpy(path, address.sun_path);
    unlock(&socket->lock);
    return 0;
}

static int sockaddr_prepare_task(struct task *task,
        int socket_type, void *sockaddr, uint_t *sockaddr_len,
        struct unix_bind_transaction *bind,
        struct socket_address *lookup) {
    // Make sure we can read things without overflowing buffers
    if (*sockaddr_len < 2)
        return _EINVAL;
    if (*sockaddr_len > sizeof(struct sockaddr_storage))
        return _EINVAL;

    struct sockaddr *real_addr = sockaddr;
    struct sockaddr_ *fake_addr = sockaddr;
    real_addr->sa_family = sock_family_to_real(fake_addr->family);

    switch (real_addr->sa_family) {
        case PF_INET:
            if (*sockaddr_len < sizeof(struct sockaddr_in))
                return _EINVAL;
            *sockaddr_len = sizeof(struct sockaddr_in);
            break;
        case PF_INET6:
            // Linux 保留 RFC2133 的 24 字节兼容输入；host 侧补零 scope_id。
            if (*sockaddr_len < offsetof(struct sockaddr_in6, sin6_scope_id))
                return _EINVAL;
            *sockaddr_len = sizeof(struct sockaddr_in6);
            break;

        case PF_LOCAL: {
            // First pull out the path, being careful to not overflow anything.
            char path[SOCKADDR_DATA_MAX + 1];
            size_t path_size = *sockaddr_len - offsetof(struct sockaddr_, data);
            if (path_size > SOCKADDR_DATA_MAX)
                return _EINVAL;
            memcpy(path, fake_addr->data, path_size);
            path[path_size] = '\0';

            uint32_t socket_id = 0;
            int err;
            int namespace_type = socket_type == SOCK_RAW_ ?
                    SOCK_DGRAM_ : socket_type;
            if (path_size == 0) {
                if (bind == NULL)
                    return _ENOENT;
                // Linux 的 addrlen==2 会生成五位抽象名称。
                do {
                    uint32_t name_id = unix_socket_next_id();
                    path[0] = '\0';
                    snprintf(path + 1, 6, "%05x", name_id & 0xfffff);
                    path_size = 6;
                    err = unix_abstract_reserve(path + 1,
                            path_size - 1, namespace_type,
                            &bind->abstract,
                            &socket_id);
                } while (err == _EADDRINUSE);
            } else if (path[0] != '\0') {
                STRACE(" unix socket %s", path);
                err = bind == NULL ?
                        unix_socket_lookup_path_task(
                                task, path, &socket_id,
                                lookup != NULL ?
                                        &lookup->lookup_inode : NULL) :
                        unix_socket_reserve_path_task(
                                task, path, bind, &socket_id);
            } else {
                STRACE(" unix abstract socket %s", path + 1);
                err = bind == NULL ?
                        unix_abstract_lookup(path + 1, path_size - 1,
                                namespace_type, &socket_id,
                                lookup != NULL ?
                                        &lookup->lookup_abstract : NULL) :
                        unix_abstract_reserve(path + 1, path_size - 1,
                                namespace_type, &bind->abstract,
                                &socket_id);
            }
            if (err < 0)
                return err;
            size_t stored_size = path[0] == '\0' ?
                    path_size : strlen(path) + 1;
            if (bind != NULL) {
                assert(stored_size <= sizeof(bind->name));
                bind->name_length = (uint8_t) stored_size;
                memcpy(bind->name, path, stored_size);
            }
            if (lookup != NULL) {
                assert(stored_size <= sizeof(lookup->unix_name));
                lookup->unix_name_valid = true;
                lookup->unix_name_length = (uint8_t) stored_size;
                memcpy(lookup->unix_name, path, stored_size);
            }

            struct sockaddr_un *real_addr_un = sockaddr;
            int path_len = snprintf(real_addr_un->sun_path,
                    sizeof(real_addr_un->sun_path), "%s%d.%u",
                    sock_tmp_prefix, getpid(), socket_id);
            if (path_len < 0 ||
                    (size_t) path_len >= sizeof(real_addr_un->sun_path))
                return _ENAMETOOLONG;
            *sockaddr_len = (uint_t) (
                    offsetof(struct sockaddr_un, sun_path) + path_len);
            break;
        }
        default:
            return _EINVAL;
    }
#ifdef __APPLE__
    real_addr->sa_len = (uint8_t) *sockaddr_len;
#endif
    return 0;
}

static int sockaddr_read(addr_t sockaddr_addr, int socket_type,
        void *sockaddr, uint_t *sockaddr_len) {
    if (*sockaddr_len < 2 ||
            *sockaddr_len > sizeof(struct sockaddr_max_))
        return _EINVAL;
    if (user_read(sockaddr_addr, sockaddr, *sockaddr_len))
        return _EFAULT;
    return sockaddr_prepare_task(
            current, socket_type, sockaddr, sockaddr_len, NULL, NULL);
}

static int sockaddr_from_real(void *sockaddr, size_t storage_size,
        uint_t *sockaddr_len, bool consume_unix_message) {
    uint_t host_length = *sockaddr_len;
    if (host_length == 0)
        return 0;
    size_t available = host_length < storage_size ?
            host_length : storage_size;
    if (available < offsetof(struct sockaddr, sa_data))
        return _EINVAL;

    struct sockaddr_storage host_storage = {0};
    memcpy(&host_storage, sockaddr, available);
    const struct sockaddr *real_addr =
            (const struct sockaddr *) &host_storage;
    struct sockaddr_storage guest_storage = {0};
    memcpy(&guest_storage, &host_storage, available);
    struct sockaddr_ *fake_addr = (struct sockaddr_ *) &guest_storage;
    fake_addr->family = sock_family_from_real(real_addr->sa_family);
    switch (fake_addr->family) {
        case PF_LOCAL_: {
            char host_path[sizeof(((struct sockaddr_un *) 0)->sun_path)] = {0};
            size_t path_offset = offsetof(struct sockaddr_un, sun_path);
            size_t path_length = available > path_offset ?
                    available - path_offset : 0;
            if (path_length >= sizeof(host_path))
                path_length = sizeof(host_path) - 1;
            const struct sockaddr_un *host =
                    (const struct sockaddr_un *) &host_storage;
            memcpy(host_path, host->sun_path, path_length);
            memset(&guest_storage, 0, sizeof(guest_storage));
            bool found = unix_bound_name_copy(host_path,
                    &guest_storage, sockaddr_len,
                    consume_unix_message);
            if (!found) {
                fake_addr = (struct sockaddr_ *) &guest_storage;
                fake_addr->family = PF_LOCAL_;
                *sockaddr_len = offsetof(struct sockaddr_, data);
            }
            break;
        }
        case PF_INET_:
        case PF_INET6_:
            break;
        default:
            return _EINVAL;
    }
    size_t copied = storage_size < sizeof(guest_storage) ?
            storage_size : sizeof(guest_storage);
    memcpy(sockaddr, &guest_storage, copied);
    return 0;
}

static int sockaddr_copy_to_user(addr_t sockaddr_addr,
        const void *sockaddr, size_t storage_size,
        uint_t buffer_len, uint_t sockaddr_len) {
    if (buffer_len > sockaddr_len)
        buffer_len = sockaddr_len;
    if (buffer_len > storage_size)
        buffer_len = (uint_t) storage_size;
    // The address is supposed to be truncated if the specified length is too
    // short, instead of returning an error.
    if (user_write(sockaddr_addr, sockaddr, buffer_len))
        return _EFAULT;
    return 0;
}

static int socket_address_prepare_raw_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length,
        struct socket_address *prepared) {
    assert(task != NULL && socket != NULL && socket->fd != NULL &&
            address != NULL && prepared != NULL);
    if (address_length > sizeof(prepared->storage))
        return _EINVAL;
    *prepared = (struct socket_address) {0};
    memcpy(&prepared->storage, address, address_length);
    uint_t length = (uint_t) address_length;
    int error = sockaddr_prepare_task(
            task, socket->fd->socket.type,
            &prepared->storage, &length, NULL, prepared);
    if (error < 0) {
        socket_address_release(prepared);
        return error;
    }
    prepared->length = (socklen_t) length;
    return 0;
}

int socket_address_prepare_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length,
        struct socket_address *prepared) {
    assert(task != NULL && socket != NULL && socket->fd != NULL &&
            address != NULL && prepared != NULL);
    if (address_length > sizeof(prepared->storage))
        return _EINVAL;
    *prepared = (struct socket_address) {0};
    if (address_length == 0)
        return 0;

    // Linux 的 INET stream sendto 忽略 msg_name；connect 走原始转换入口。
    if ((socket->fd->socket.domain == AF_INET_ ||
            socket->fd->socket.domain == AF_INET6_) &&
            socket->fd->socket.type == SOCK_STREAM_)
        return 0;
    if (address_length < sizeof(uint16_t))
        return _EINVAL;

    uint16_t family;
    memcpy(&family, address, sizeof(family));
    if (socket->fd->socket.domain == AF_INET6_) {
        if (family == 0 && socket->fd->socket.type != SOCK_RAW_)
            return 0;
        if (family == 0) {
            struct sockaddr_storage normalized = {0};
            memcpy(&normalized, address, address_length);
            uint16_t inet6_family = AF_INET6_;
            memcpy(&normalized, &inet6_family, sizeof(inet6_family));
            return socket_address_prepare_raw_task(task, socket,
                    &normalized, address_length, prepared);
        }
        if (family == AF_INET_) {
            const byte_t *wire = address;
            struct sockaddr_in6 mapped = {0};
            mapped.sin6_family = AF_INET6;
            memcpy(&mapped.sin6_port, wire + 2,
                    sizeof(mapped.sin6_port));
            mapped.sin6_addr.s6_addr[10] = 0xff;
            mapped.sin6_addr.s6_addr[11] = 0xff;
            memcpy(&mapped.sin6_addr.s6_addr[12], wire + 4, 4);
#ifdef __APPLE__
            mapped.sin6_len = sizeof(mapped);
#endif
            memcpy(&prepared->storage, &mapped, sizeof(mapped));
            prepared->length = sizeof(mapped);
            return 0;
        }
    }

    if (socket->fd->socket.domain == AF_INET_ && family == 0) {
        struct sockaddr_storage normalized = {0};
        memcpy(&normalized, address, address_length);
        uint16_t inet_family = AF_INET_;
        memcpy(&normalized, &inet_family, sizeof(inet_family));
        return socket_address_prepare_raw_task(task, socket,
                &normalized, address_length, prepared);
    }
    return socket_address_prepare_raw_task(
            task, socket, address, address_length, prepared);
}

int socket_address_validate_for_socket(const struct socket_ref *socket,
        const void *address, size_t address_length) {
    assert(socket != NULL && socket->fd != NULL && address != NULL);
    if (address_length > sizeof(struct sockaddr_storage))
        return _EINVAL;
    if (address_length == 0) {
        if (socket->fd->socket.domain == AF_LOCAL_ ||
                socket->fd->socket.type == SOCK_STREAM_)
            return 0;
        // UDP/ping 与 rawv6 按非空 msg_name 校验；raw4 只看 namelen。
        return socket->fd->socket.domain == AF_INET_ &&
                socket->fd->socket.type == SOCK_RAW_ ? 0 : _EINVAL;
    }
    if ((socket->fd->socket.domain == AF_INET_ ||
            socket->fd->socket.domain == AF_INET6_) &&
            socket->fd->socket.type == SOCK_STREAM_)
        return 0;
    if (socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type == SOCK_STREAM_) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        return getpeername(socket->fd->real_fd,
                (struct sockaddr *) &peer, &peer_length) == 0 ?
                _EISCONN : _EOPNOTSUPP;
    }
    if (address_length < sizeof(uint16_t))
        return _EINVAL;
    uint16_t family;
    memcpy(&family, address, sizeof(family));
    if (socket->fd->socket.domain == AF_LOCAL_)
        return address_length <= offsetof(struct sockaddr_, data) ||
                family != AF_LOCAL_ ? _EINVAL : 0;

    bool udp = socket->fd->socket.type == SOCK_DGRAM_ &&
            (socket->fd->socket.protocol == 0 ||
            socket->fd->socket.protocol == IPPROTO_UDP);
    bool ping = socket_sendto_prefix_size(socket) != 0;
    if (socket->fd->socket.domain == AF_INET_) {
        if (address_length < sizeof(struct sockaddr_in))
            return _EINVAL;
        if (ping)
            return family == AF_INET_ ? 0 : _EAFNOSUPPORT;
        if (family != AF_INET_ && family != 0)
            return _EAFNOSUPPORT;
        if (udp) {
            uint16_t port;
            memcpy(&port, (const byte_t *) address + 2, sizeof(port));
            if (port == 0)
                return _EINVAL;
        }
        return 0;
    }

    assert(socket->fd->socket.domain == AF_INET6_);
    if (ping) {
        if (address_length < sizeof(struct sockaddr_in6))
            return _EINVAL;
        return family == AF_INET6_ ? 0 : _EAFNOSUPPORT;
    }
    if (socket->fd->socket.type == SOCK_RAW_) {
        if (address_length < offsetof(
                struct sockaddr_in6, sin6_scope_id))
            return _EINVAL;
        if (family != 0 && family != AF_INET6_)
            return _EAFNOSUPPORT;
        uint16_t protocol;
        memcpy(&protocol, (const byte_t *) address + 2,
                sizeof(protocol));
        protocol = ntohs(protocol);
        if (protocol > UINT8_MAX ||
                (protocol != 0 &&
                socket->fd->socket.protocol != IPPROTO_RAW &&
                protocol != socket->fd->socket.protocol))
            return _EINVAL;
        return 0;
    }
    if (family == AF_INET6_) {
        if (address_length < offsetof(
                struct sockaddr_in6, sin6_scope_id))
            return _EINVAL;
        if (udp) {
            uint16_t port;
            memcpy(&port, (const byte_t *) address + 2, sizeof(port));
            if (port == 0)
                return _EINVAL;
        }
        return 0;
    }
    if (family == AF_INET_) {
        if (address_length < sizeof(struct sockaddr_in))
            return _EINVAL;
        int ipv6_only = 0;
        socklen_t option_length = sizeof(ipv6_only);
        if (getsockopt(socket->fd->real_fd, IPPROTO_IPV6,
                IPV6_V6ONLY, &ipv6_only, &option_length) < 0)
            return errno_map();
        if (ipv6_only)
            return _ENETUNREACH;
        if (udp) {
            uint16_t port;
            memcpy(&port, (const byte_t *) address + 2, sizeof(port));
            if (port == 0)
                return _EINVAL;
        }
        return 0;
    }
    return family == 0 ? 0 : _EINVAL;
}

static int socket_bind_address_validate(const struct socket_ref *socket,
        const void *address, size_t address_length) {
    assert(socket != NULL && socket->fd != NULL && address != NULL);
    uint16_t family;
    if (socket->fd->socket.domain == AF_INET_) {
        if (address_length < sizeof(struct sockaddr_in))
            return _EINVAL;
        memcpy(&family, address, sizeof(family));
        if (family == AF_INET_)
            return 0;
        uint32_t ipv4_address;
        memcpy(&ipv4_address, (const byte_t *) address + 4,
                sizeof(ipv4_address));
        return family == 0 && ipv4_address == INADDR_ANY ?
                0 : _EAFNOSUPPORT;
    }
    if (socket->fd->socket.domain == AF_INET6_) {
        if (address_length < offsetof(
                struct sockaddr_in6, sin6_scope_id))
            return _EINVAL;
        memcpy(&family, address, sizeof(family));
        return family == AF_INET6_ ? 0 : _EAFNOSUPPORT;
    }
    if (address_length < sizeof(uint16_t))
        return _EINVAL;
    memcpy(&family, address, sizeof(family));
    return family == AF_LOCAL_ ? 0 : _EINVAL;
}

void socket_address_release(struct socket_address *address) {
    assert(address != NULL);
    inode_release_if_exist(address->lookup_inode);
    if (address->lookup_abstract != NULL)
        unix_abstract_release(address->lookup_abstract);
    address->lookup_inode = NULL;
    address->lookup_abstract = NULL;
}

int_t socket_bind_ref_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length) {
    assert(task != NULL && socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops && address != NULL);
    if (address_length < 2 ||
            address_length > sizeof(struct sockaddr_storage))
        return _EINVAL;
    int error = socket_bind_address_validate(
            socket, address, address_length);
    if (error < 0)
        return error;

    bool unix_autobind = socket->fd->socket.domain == AF_LOCAL_ &&
            address_length == sizeof(uint16_t);
    struct sockaddr_storage sockaddr = {0};
    memcpy(&sockaddr, address, address_length);
    if (socket->fd->socket.domain == AF_INET_) {
        uint16_t family;
        memcpy(&family, &sockaddr, sizeof(family));
        if (family == 0) {
            family = AF_INET_;
            memcpy(&sockaddr, &family, sizeof(family));
        }
    }
    uint_t sockaddr_len = (uint_t) address_length;
    struct unix_bind_transaction transaction = {0};

    // fd 锁在这里承担 Linux unix bindlock 的职责，保证同一
    // socket 的 host bind、名称发布与元数据提交不会交叉。
    lock(&socket->fd->lock);
    if (socket->fd->socket.domain == AF_LOCAL_ &&
            (socket->fd->socket.unix_name_inode != NULL ||
            socket->fd->socket.unix_name_abstract != NULL ||
            socket->fd->socket.unix_backing_path[0] != '\0')) {
        unlock(&socket->fd->lock);
        return unix_autobind ? 0 : _EINVAL;
    }
    error = sockaddr_prepare_task(
            task, socket->fd->socket.type,
            &sockaddr, &sockaddr_len, &transaction, NULL);
    if (error < 0) {
        unlock(&socket->fd->lock);
        unix_bind_rollback(&transaction);
        return error;
    }

    if (socket->fd->socket.domain == AF_LOCAL_) {
        transaction.bound_name = calloc(1, sizeof(*transaction.bound_name));
        if (transaction.bound_name == NULL) {
            unlock(&socket->fd->lock);
            unix_bind_rollback(&transaction);
            return _ENOMEM;
        }
        list_init(&transaction.bound_name->links);
        list_init(&transaction.bound_name->pending_peers);
        cond_init(&transaction.bound_name->connects_drained);
        transaction.bound_name->name_length = transaction.name_length;
        memcpy(transaction.bound_name->name,
                transaction.name, transaction.name_length);
        const struct sockaddr_un *host_address =
                (const struct sockaddr_un *) &sockaddr;
        strcpy(transaction.bound_name->backing_path,
                host_address->sun_path);
    }

    if (bind(socket->fd->real_fd,
            (void *) &sockaddr, (socklen_t) sockaddr_len) < 0) {
        error = errno_map();
        unlock(&socket->fd->lock);
        unix_bind_rollback(&transaction);
        return error;
    }
    if (socket->fd->socket.domain == AF_LOCAL_) {
        const struct sockaddr_un *host_address =
                (const struct sockaddr_un *) &sockaddr;
        struct stat host_stat;
        if (lstat(host_address->sun_path, &host_stat) == 0) {
            socket->fd->socket.unix_backing_owned = true;
            socket->fd->socket.unix_backing_device =
                    (uint64_t) host_stat.st_dev;
            socket->fd->socket.unix_backing_inode =
                    (uint64_t) host_stat.st_ino;
        }
        strcpy(socket->fd->socket.unix_backing_path,
                host_address->sun_path);
        unix_bind_commit(&transaction, socket->fd);
    } else {
        uint16_t port;
        memcpy(&port, (const byte_t *) address + 2, sizeof(port));
        socket->fd->socket.inet_explicitly_bound = port != 0;
    }
    unlock(&socket->fd->lock);
    if (socket->fd->socket.domain != AF_LOCAL_)
        unix_bind_rollback(&transaction);
    return 0;
}

int_t sys_bind(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("bind(%d, 0x%x, %d)", sock_fd, sockaddr_addr, sockaddr_len);
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;
    if (sockaddr_len < 2 ||
            sockaddr_len > sizeof(struct sockaddr_storage)) {
        result = _EINVAL;
        goto out;
    }
    struct sockaddr_storage sockaddr = {0};
    if (user_read(sockaddr_addr, &sockaddr, sockaddr_len)) {
        result = _EFAULT;
        goto out;
    }
    result = socket_bind_ref_task(
            current, &socket, &sockaddr, sockaddr_len);
out:
    socket_ref_release(&socket);
    return result;
}

static void fill_cred_task(struct task *task, struct ucred_ *cred) {
    struct task_credentials credentials;
    task_credentials_snapshot(task, &credentials);
    cred->pid = task->pid;
    cred->uid = credentials.euid;
    cred->gid = credentials.egid;
}

static int unix_bound_connect_begin(const struct socket_address *address,
        struct fd *peer, struct unix_bound_name **target,
        struct unix_pending_peer **pending,
        struct ucred_ *listener_cred, bool *listener_cred_valid,
        uint64_t generation) {
    struct unix_pending_peer *candidate = calloc(1, sizeof(*candidate));
    if (candidate == NULL)
        return _ENOMEM;
    list_init(&candidate->links);
    int path_error = unix_socket_transport_path(
            peer, candidate->peer_path);
    if (path_error < 0) {
        free(candidate);
        return path_error;
    }

    const struct sockaddr_un *host_address =
            (const struct sockaddr_un *) &address->storage;
    lock(&unix_bound_lock);
    struct unix_bound_name *name = NULL;
    if (!list_null(&unix_bound_sockets)) {
        struct unix_bound_name *entry;
        list_for_each_entry(&unix_bound_sockets, entry, links) {
            if (entry->owner_open && !entry->closing &&
                    strcmp(entry->backing_path,
                            host_address->sun_path) == 0) {
                name = entry;
                break;
            }
        }
    }
    if (name == NULL) {
        unlock(&unix_bound_lock);
        free(candidate);
        return _ECONNREFUSED;
    }

    candidate->peer = fd_retain(peer);
    candidate->listener = name->owner;
    candidate->connect_active = true;
    candidate->generation = generation;
    candidate->listener_cred = name->listener_cred;
    candidate->listener_cred_valid = name->listener_cred_valid;
    name->active_connects++;
    list_add_tail(&name->pending_peers, &candidate->links);
    candidate->linked = true;
    assert(peer->socket.unix_pending_connect == NULL);
    peer->socket.unix_pending_connect = candidate;
    *target = name;
    *pending = candidate;
    *listener_cred = name->listener_cred;
    *listener_cred_valid = name->listener_cred_valid;
    unlock(&unix_bound_lock);
    return 0;
}

static void unix_bound_connect_complete(struct unix_bound_name *target,
        struct unix_pending_peer *pending,
        bool connected, bool asynchronous) {
    struct fd *canceled_peer = NULL;
    bool free_pending = false;
    lock(&unix_bound_lock);
    assert(target->active_connects != 0);
    assert(pending->connect_active);
    pending->connect_active = false;
    pending->host_error_pending = connected && asynchronous;
    if ((!connected || pending->cancel_requested) &&
            !pending->accepted) {
        assert(pending->linked);
        list_remove(&pending->links);
        pending->linked = false;
        canceled_peer = pending->peer;
        pending->peer = NULL;
        pending->listener = NULL;
    } else if (pending->accepted && pending->handshake_finished) {
        free_pending = true;
    }
    target->active_connects--;
    if (target->active_connects == 0)
        notify(&target->connects_drained);
    unlock(&unix_bound_lock);

    if (canceled_peer != NULL) {
        unix_socket_finish_peer_handshake(canceled_peer, NULL);
    }
    if (free_pending) {
        pending->listener = NULL;
        free(pending);
    }
}

static bool unix_bound_cancel_peer(
        struct fd *peer, uint64_t generation) {
    struct unix_pending_peer *canceled = NULL;
    lock(&unix_bound_lock);
    struct unix_pending_peer *pending =
            peer->socket.unix_pending_connect;
    if (pending != NULL && pending->generation == generation &&
            pending->host_error_pending) {
        pending->cancel_requested = true;
        if (!pending->connect_active && pending->linked &&
                !pending->accepted && pending->peer == peer) {
            list_remove(&pending->links);
            pending->linked = false;
            pending->peer = NULL;
            pending->listener = NULL;
            canceled = pending;
        }
    }
    unlock(&unix_bound_lock);
    if (canceled == NULL)
        return false;
    unix_socket_finish_peer_handshake(peer, NULL);
    return true;
}

enum unix_connect_error_phase {
    UNIX_CONNECT_ERROR_NONE,
    UNIX_CONNECT_ERROR_DEFERRED,
    UNIX_CONNECT_ERROR_ASYNC,
};

static enum unix_connect_error_phase unix_bound_connect_error_snapshot(
        struct fd *peer, uint64_t *generation) {
    enum unix_connect_error_phase phase = UNIX_CONNECT_ERROR_NONE;
    *generation = 0;
    lock(&unix_bound_lock);
    struct unix_pending_peer *pending =
            peer->socket.unix_pending_connect;
    if (pending != NULL) {
        phase = pending->host_error_pending ?
                UNIX_CONNECT_ERROR_ASYNC :
                UNIX_CONNECT_ERROR_DEFERRED;
        *generation = pending->generation;
    }
    unlock(&unix_bound_lock);
    return phase;
}

static struct fd *unix_bound_accept_peer(
        struct fd *listener, const struct sockaddr_storage *address,
        socklen_t address_length) {
    if (address_length <= offsetof(struct sockaddr_un, sun_path))
        return NULL;
    const struct sockaddr_un *peer_address =
            (const struct sockaddr_un *) address;
    if (peer_address->sun_family != AF_UNIX)
        return NULL;
    size_t path_length = address_length -
            offsetof(struct sockaddr_un, sun_path);
    if (path_length >= sizeof(peer_address->sun_path))
        path_length = sizeof(peer_address->sun_path) - 1;
    char peer_path[sizeof(peer_address->sun_path)];
    memcpy(peer_path, peer_address->sun_path, path_length);
    peer_path[path_length] = '\0';

    struct fd *peer = NULL;
    struct ucred_ listener_cred = {0};
    bool listener_cred_valid = false;
    lock(&unix_bound_lock);
    struct unix_bound_name *name = listener->socket.unix_bound_name;
    if (name != NULL) {
        struct unix_pending_peer *pending;
        list_for_each_entry(&name->pending_peers, pending, links) {
            if (strcmp(pending->peer_path, peer_path) != 0)
                continue;
            assert(pending->linked);
            list_remove(&pending->links);
            pending->linked = false;
            peer = pending->peer;
            pending->peer = NULL;
            pending->accepted = true;
            listener_cred = pending->listener_cred;
            listener_cred_valid = pending->listener_cred_valid;
            if (!listener_cred_valid && name->listener_cred_valid) {
                listener_cred = name->listener_cred;
                listener_cred_valid = true;
            }
            break;
        }
    }
    unlock(&unix_bound_lock);
    if (peer != NULL && listener_cred_valid) {
        lock(&peer_lock);
        peer->socket.unix_peer_cred = listener_cred;
        peer->socket.unix_peer_cred_valid = true;
        unlock(&peer_lock);
    }
    return peer;
}

static void unix_socket_finish_peer_handshake(
        struct fd *peer, struct fd *accepted) {
    if (peer == NULL)
        return;
    struct list rejected_scm;
    list_init(&rejected_scm);
    struct unix_pending_peer *finished_pending = NULL;
    bool free_pending = false;
    lock(&unix_scm_io_lock);
    lock(&unix_bound_lock);
    finished_pending = peer->socket.unix_pending_connect;
    if (finished_pending != NULL) {
        peer->socket.unix_pending_connect = NULL;
        finished_pending->handshake_finished = true;
        finished_pending->listener = NULL;
        free_pending = !finished_pending->connect_active;
    }
    unlock(&unix_bound_lock);
    lock(&peer_lock);
    if (accepted != NULL) {
        accepted->socket.unix_peer = peer;
        peer->socket.unix_peer = accepted;
        accepted->socket.unix_peer_name_valid = true;
        accepted->socket.unix_peer_name_len =
                peer->socket.unix_name_len;
        memcpy(accepted->socket.unix_peer_name,
                peer->socket.unix_name,
                peer->socket.unix_name_len);
        peer->socket.unix_peer_name_valid = true;
        peer->socket.unix_peer_name_len =
                accepted->socket.unix_name_len;
        memcpy(peer->socket.unix_peer_name,
                accepted->socket.unix_name,
                accepted->socket.unix_name_len);
        accepted->socket.unix_peer_cred = peer->socket.unix_cred;
        accepted->socket.unix_peer_cred_valid = true;
        if (!peer->socket.unix_peer_cred_valid) {
            peer->socket.unix_peer_cred = accepted->socket.unix_cred;
            peer->socket.unix_peer_cred_valid = true;
        }
        peer->socket.unix_peer_handshake_pending = false;
        peer->socket.unix_peer_handshake_rejected = false;
        while (!list_empty(&peer->socket.unix_pending_scm)) {
            struct scm *scm = list_first_entry(
                    &peer->socket.unix_pending_scm,
                    struct scm, queue);
            list_remove(&scm->queue);
            list_add_tail(&accepted->socket.unix_scm, &scm->queue);
            assert(scm->inflight);
            scm->receiver = accepted;
        }
    } else {
        peer->socket.unix_peer_handshake_pending = false;
        peer->socket.unix_peer_handshake_rejected = true;
        while (!list_empty(&peer->socket.unix_pending_scm)) {
            struct scm *scm = list_first_entry(
                    &peer->socket.unix_pending_scm,
                    struct scm, queue);
            list_remove(&scm->queue);
            socket_scm_unpublish_locked(scm);
            list_add_tail(&rejected_scm, &scm->queue);
        }
    }
    unlock(&peer_lock);
    unlock(&unix_scm_io_lock);

    while (!list_empty(&rejected_scm)) {
        struct scm *scm = list_first_entry(
                &rejected_scm, struct scm, queue);
        list_remove(&scm->queue);
        socket_scm_release(scm);
    }
    // 待 accept 注册表携带一份强引用，由成功、拒绝或 listener 关闭消费。
    fd_close(peer);
    if (free_pending)
        free(finished_pending);
}

static int socket_connect_prepared(
        struct task *task, struct fd *sock,
        const struct socket_address *address) {
    bool unix_stream = sock->socket.domain == AF_LOCAL_ &&
            sock->socket.type == SOCK_STREAM_;
    struct unix_bound_name *target = NULL;
    struct unix_pending_peer *pending = NULL;
    struct ucred_ listener_cred = {0};
    bool listener_cred_valid = false;
    uint64_t connect_generation = 0;
    if (unix_stream) {
        struct sockaddr_storage connected_address;
        socklen_t connected_length = sizeof(connected_address);
        if (getpeername(sock->real_fd,
                (struct sockaddr *) &connected_address,
                &connected_length) == 0)
            return _EISCONN;
        lock(&peer_lock);
        bool pending_connect =
                sock->socket.unix_peer_handshake_pending;
        if (!pending_connect) {
            sock->socket.unix_peer_handshake_pending = true;
            sock->socket.unix_peer_handshake_rejected = false;
            connect_generation = ++sock->socket.unix_connect_generation;
            if (connect_generation == 0)
                connect_generation = ++sock->socket.unix_connect_generation;
        } else {
            connect_generation = sock->socket.unix_connect_generation;
        }
        unlock(&peer_lock);
        if (pending_connect) {
            connected_length = sizeof(connected_address);
            if (getpeername(sock->real_fd,
                    (struct sockaddr *) &connected_address,
                    &connected_length) == 0)
                return _EISCONN;
            enum unix_connect_error_phase error_phase =
                    unix_bound_connect_error_snapshot(
                            sock, &connect_generation);
            if (error_phase != UNIX_CONNECT_ERROR_ASYNC)
                return _EALREADY;
            int host_error = 0;
            socklen_t error_length = sizeof(host_error);
            if (getsockopt(sock->real_fd, SOL_SOCKET, SO_ERROR,
                    &host_error, &error_length) < 0)
                return errno_map();
            if (host_error == 0)
                return _EALREADY;
            (void) unix_bound_cancel_peer(
                    sock, connect_generation);
            return err_map(host_error);
        }
        lock(&sock->lock);
        fill_cred_task(task, &sock->socket.unix_cred);
        unlock(&sock->lock);
        int begin_error = unix_bound_connect_begin(
                address, sock, &target, &pending,
                &listener_cred, &listener_cred_valid,
                connect_generation);
        if (begin_error < 0) {
            lock(&peer_lock);
            sock->socket.unix_peer_handshake_pending = false;
            unlock(&peer_lock);
            return begin_error;
        }
    }

    bool serialize_connect = sock->socket.domain == AF_INET_ ||
            sock->socket.domain == AF_INET6_;
    bool serialize_unix_datagram =
            sock->socket.domain == AF_LOCAL_ &&
            sock->socket.type != SOCK_STREAM_;
    if (serialize_connect)
        lock(&sock->lock);
    // AF_UNIX 数据报 connect 只更新对端关联，不进入可阻塞的传输等待。
    if (serialize_unix_datagram)
        lock(&unix_scm_io_lock);
    int err = connect(sock->real_fd,
            (const struct sockaddr *) &address->storage,
            address->length);
    int host_error = errno;
    if (serialize_connect && sock->socket.type == SOCK_STREAM_)
        sock->socket.inet_connect_pending = err < 0 &&
                (host_error == EINPROGRESS || host_error == EALREADY ||
                host_error == EINTR);
    if (serialize_unix_datagram)
        unlock(&unix_scm_io_lock);
    if (serialize_connect)
        unlock(&sock->lock);
    bool asynchronous = err < 0 &&
            (host_error == EINPROGRESS || host_error == EALREADY);
    if (unix_stream)
        unix_bound_connect_complete(
                target, pending, err == 0 || asynchronous,
                asynchronous);
    if (err < 0 && host_error == ENOENT &&
            (address->lookup_inode != NULL ||
            address->lookup_abstract != NULL))
        return _ECONNREFUSED;
    if (err < 0)
        return err_map(host_error);

    if (sock->socket.domain == AF_LOCAL_ && address->unix_name_valid) {
        lock(&peer_lock);
        sock->socket.unix_peer_handshake_rejected = false;
        sock->socket.unix_peer_name_valid = true;
        sock->socket.unix_peer_name_len = address->unix_name_length;
        memcpy(sock->socket.unix_peer_name,
                address->unix_name, address->unix_name_length);
        if (unix_stream && listener_cred_valid) {
            sock->socket.unix_peer_cred = listener_cred;
            sock->socket.unix_peer_cred_valid = true;
        }
        unlock(&peer_lock);
    }

    return err;
}

static int socket_connect_address_prepare_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length,
        struct socket_address *prepared) {
    uint16_t family;
    memcpy(&family, address, sizeof(family));
    if (socket->fd->socket.domain == AF_INET_) {
        if (address_length < sizeof(struct sockaddr_in))
            return _EINVAL;
        if (family != AF_INET_)
            return _EAFNOSUPPORT;
    } else if (socket->fd->socket.domain == AF_INET6_) {
        if (family == AF_INET_) {
            if (address_length < sizeof(struct sockaddr_in))
                return _EINVAL;
            int ipv6_only = 0;
            socklen_t option_length = sizeof(ipv6_only);
            if (getsockopt(socket->fd->real_fd, IPPROTO_IPV6,
                    IPV6_V6ONLY, &ipv6_only, &option_length) < 0)
                return errno_map();
            if (ipv6_only)
                return _EAFNOSUPPORT;
            const byte_t *wire = address;
            struct sockaddr_in6 mapped = {0};
            mapped.sin6_family = AF_INET6;
            memcpy(&mapped.sin6_port, wire + 2,
                    sizeof(mapped.sin6_port));
            mapped.sin6_addr.s6_addr[10] = 0xff;
            mapped.sin6_addr.s6_addr[11] = 0xff;
            memcpy(&mapped.sin6_addr.s6_addr[12], wire + 4, 4);
#ifdef __APPLE__
            mapped.sin6_len = sizeof(mapped);
#endif
            *prepared = (struct socket_address) {
                .length = sizeof(mapped),
            };
            memcpy(&prepared->storage, &mapped, sizeof(mapped));
            return 0;
        }
        if (address_length < offsetof(
                struct sockaddr_in6, sin6_scope_id))
            return _EINVAL;
        if (family != AF_INET6_)
            return _EAFNOSUPPORT;
    }
    return socket_address_prepare_raw_task(
            task, socket, address, address_length, prepared);
}

#ifdef __APPLE__
struct copied_socket_option {
    int level;
    int option;
};

static int socket_copy_option(int source, int destination,
        const struct copied_socket_option *option) {
    byte_t value[256];
    socklen_t value_length = sizeof(value);
    if (getsockopt(source, option->level, option->option,
            value, &value_length) < 0) {
        if (errno == ENOPROTOOPT || errno == EINVAL)
            return 0;
        return errno_map();
    }
    if (setsockopt(destination, option->level, option->option,
            value, value_length) < 0)
        return errno_map();
    return 0;
}

static int socket_recreate_unbound_inet(struct fd *fd) {
    static const struct copied_socket_option common_options[] = {
        {SOL_SOCKET, SO_REUSEADDR},
        {SOL_SOCKET, SO_BROADCAST},
        {SOL_SOCKET, SO_KEEPALIVE},
        {SOL_SOCKET, SO_LINGER},
#ifdef SO_REUSEPORT
        {SOL_SOCKET, SO_REUSEPORT},
#endif
        {SOL_SOCKET, SO_SNDBUF},
        {SOL_SOCKET, SO_RCVBUF},
#ifdef SO_TIMESTAMP
        {SOL_SOCKET, SO_TIMESTAMP},
#endif
        {SOL_SOCKET, SO_RCVTIMEO},
        {SOL_SOCKET, SO_SNDTIMEO},
    };
    static const struct copied_socket_option ipv4_options[] = {
        {IPPROTO_IP, IP_TOS},
        {IPPROTO_IP, IP_TTL},
        {IPPROTO_IP, IP_RECVTTL},
        {IPPROTO_IP, IP_RECVTOS},
    };
    static const struct copied_socket_option ipv6_options[] = {
        {IPPROTO_IPV6, IPV6_UNICAST_HOPS},
        {IPPROTO_IPV6, IPV6_TCLASS},
        {IPPROTO_IPV6, IPV6_V6ONLY},
    };

    int real_domain = sock_family_to_real(fd->socket.domain);
    int real_type = sock_type_to_real(
            fd->socket.type, fd->socket.protocol);
    if (real_domain < 0 || real_type < 0)
        return _EINVAL;
    int replacement = socket(real_domain, real_type,
            fd->socket.protocol);
    if (replacement < 0)
        return errno_map();
    sock_configure_host_fd(replacement);
    if (fd->socket.domain == AF_INET_ &&
            fd->socket.type == SOCK_DGRAM_) {
        int strip_header = 1;
        (void) setsockopt(replacement, IPPROTO_IP,
                IP_STRIPHDR, &strip_header, sizeof(strip_header));
    }

    int result = 0;
    int status_flags = fcntl(fd->real_fd, F_GETFL);
    if (status_flags < 0 ||
            fcntl(replacement, F_SETFL, status_flags) < 0) {
        result = errno_map();
        goto out;
    }
    for (size_t index = 0;
            index < array_size(common_options); index++) {
        result = socket_copy_option(fd->real_fd, replacement,
                &common_options[index]);
        if (result < 0)
            goto out;
    }
    const struct copied_socket_option *family_options =
            fd->socket.domain == AF_INET_ ?
            ipv4_options : ipv6_options;
    size_t family_option_count = fd->socket.domain == AF_INET_ ?
            array_size(ipv4_options) : array_size(ipv6_options);
    for (size_t index = 0; index < family_option_count; index++) {
        result = socket_copy_option(fd->real_fd, replacement,
                &family_options[index]);
        if (result < 0)
            goto out;
    }
    if (dup2(replacement, fd->real_fd) < 0) {
        result = errno_map();
        goto out;
    }
out:
    close(replacement);
    return result;
}
#endif

static int socket_disconnect_inet(struct fd *socket) {
    int error = 0;
    lock(&socket->lock);
#ifdef __APPLE__
    // Darwin 的 stream disconnectx 只执行 shutdown，不能像 Linux 一样重新 connect。
    if (socket->socket.type == SOCK_STREAM_) {
        unlock(&socket->lock);
        return _EOPNOTSUPP;
    }
    if (socket->socket.type == SOCK_DGRAM_ &&
            !socket->socket.inet_explicitly_bound) {
        error = socket_recreate_unbound_inet(socket);
    } else {
        int result = disconnectx(
                socket->real_fd, SAE_ASSOCID_ANY, SAE_CONNID_ANY);
        if (result < 0 && (errno == ENOTCONN || errno == EINVAL))
            result = 0;
        if (result < 0)
            error = errno_map();
    }
#else
    struct sockaddr address = {.sa_family = AF_UNSPEC};
    if (connect(socket->real_fd, &address, sizeof(address)) < 0)
        error = errno_map();
#endif
    if (error == 0)
        socket->socket.inet_connect_pending = false;
    unlock(&socket->lock);
    if (error < 0)
        return error;

    lock(&peer_lock);
    struct fd *peer = socket->socket.unix_peer;
    if (peer != NULL && peer->socket.unix_peer == socket)
        peer->socket.unix_peer = NULL;
    socket->socket.unix_peer = NULL;
    socket->socket.unix_peer_name_valid = false;
    socket->socket.unix_peer_cred_valid = false;
    socket->socket.unix_peer_handshake_pending = false;
    socket->socket.unix_peer_handshake_rejected = false;
    unlock(&peer_lock);
    return 0;
}

int_t socket_connect_ref_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length) {
    assert(task != NULL && socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    if (address_length < 2 ||
            address_length > sizeof(struct sockaddr_storage))
        return _EINVAL;
    uint16_t family;
    memcpy(&family, address, sizeof(family));
    if (family == 0 &&
            (socket->fd->socket.domain == AF_INET_ ||
            socket->fd->socket.domain == AF_INET6_))
        return socket_disconnect_inet(socket->fd);
    struct socket_address prepared = {0};
    int_t result = socket_connect_address_prepare_task(
            task, socket, address, address_length, &prepared);
    if (result < 0)
        return result;
    result = socket_connect_prepared(
            task, socket->fd, &prepared);
    socket_address_release(&prepared);
    return result;
}

int_t socket_connect_retained_task(struct task *task,
        struct fd *fd, const void *address, size_t address_length) {
    assert(task != NULL && fd != NULL && address != NULL);
    if (fd->ops != &socket_fdops)
        return _ENOTSOCK;
    const struct socket_ref socket = {.fd = fd};
    return socket_connect_ref_task(
            task, &socket, address, address_length);
}

int_t sys_connect(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len) {
    STRACE("connect(%d, 0x%x, %d)", sock_fd, sockaddr_addr, sockaddr_len);
    struct fd *fd = f_get_task_retain(current, sock_fd);
    if (fd == NULL)
        return _EBADF;
    int_t result;
    if (sockaddr_len > sizeof(struct sockaddr_storage)) {
        result = _EINVAL;
        goto out;
    }
    struct sockaddr_storage sockaddr = {0};
    if (sockaddr_len != 0 &&
            user_read(sockaddr_addr, &sockaddr, sockaddr_len)) {
        result = _EFAULT;
        goto out;
    }
    result = socket_connect_retained_task(
            current, fd, &sockaddr, sockaddr_len);
out:
    fd_close(fd);
    return result;
}

int_t sys_listen(fd_t sock_fd, int_t backlog) {
    STRACE("listen(%d, %d)", sock_fd, backlog);
    struct socket_ref listener;
    int_t result = socket_ref_get_task(current, sock_fd, &listener);
    if (result < 0)
        return result;
    struct fd *sock = listener.fd;
    bool unix_socket = sock->socket.domain == AF_LOCAL_;
    struct ucred_ listener_cred = {0};
    if (unix_socket)
        fill_cred_task(current, &listener_cred);

    lock(&sock->lock);
    if (unix_socket) {
        sock->socket.unix_cred = listener_cred;
        lock(&unix_bound_lock);
    }
    if (listen(sock->real_fd, backlog) < 0) {
        result = errno_map();
        if (unix_socket)
            unlock(&unix_bound_lock);
        unlock(&sock->lock);
        goto out;
    }
    if (unix_socket) {
        struct unix_bound_name *name = sock->socket.unix_bound_name;
        if (name != NULL && name->owner_open && !name->closing) {
            name->listener_cred = listener_cred;
            name->listener_cred_valid = true;
        }
        unlock(&unix_bound_lock);
    }
    if (!sock->socket.listening) {
        sockrestart_begin_listen(sock);
        sock->socket.listening = true;
    }
    unlock(&sock->lock);
    result = 0;
out:
    socket_ref_release(&listener);
    return result;
}

int_t sys_accept(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("accept(%d, 0x%x, 0x%x)", sock_fd, sockaddr_addr, sockaddr_len_addr);
    struct socket_ref listener;
    int_t result = socket_ref_get_task(
            current, sock_fd, &listener);
    if (result < 0)
        return result;
    sdword_t guest_sockaddr_len = 0;

    struct sockaddr_storage sockaddr = {0};
    socklen_t host_sockaddr_len = sizeof(sockaddr);
    int client;
    do {
        host_sockaddr_len = sizeof(sockaddr);
        sockrestart_begin_listen_wait(listener.fd);
        errno = 0;
        client = accept(listener.fd->real_fd,
                (void *) &sockaddr, &host_sockaddr_len);
        sockrestart_end_listen_wait(listener.fd);
    } while (sockrestart_should_restart_listen_wait() && errno == EINTR);
    if (client < 0) {
        result = errno_map();
        goto out_listener;
    }
    sock_configure_host_fd(client);

    struct fd *client_fd = sock_fd_allocate(
            listener.fd->socket.domain, listener.fd->socket.type,
            listener.fd->socket.protocol,
            listener.fd->socket.guest_protocol);
    if (client_fd == NULL) {
        struct fd *peer = listener.fd->socket.domain == AF_LOCAL_ ?
                unix_bound_accept_peer(listener.fd,
                        &sockaddr, host_sockaddr_len) : NULL;
        unix_socket_finish_peer_handshake(peer, NULL);
        close(client);
        result = _ENOMEM;
        goto out_listener;
    }
    // 从这里起 wrapper 独占 raw client 的关闭责任。
    client_fd->real_fd = client;
    if (listener.fd->socket.domain == AF_LOCAL_) {
        lock(&listener.fd->lock);
        client_fd->socket.unix_name_len =
                listener.fd->socket.unix_name_len;
        memcpy(client_fd->socket.unix_name,
                listener.fd->socket.unix_name,
                listener.fd->socket.unix_name_len);
        unlock(&listener.fd->lock);
    }
    struct fd *connecting_peer =
            listener.fd->socket.domain == AF_LOCAL_ ?
            unix_bound_accept_peer(listener.fd,
                    &sockaddr, host_sockaddr_len) : NULL;

    if (sockaddr_addr != 0) {
        if (user_get(sockaddr_len_addr, guest_sockaddr_len)) {
            result = _EFAULT;
            goto reject_client;
        }
        if (guest_sockaddr_len < 0) {
            result = _EINVAL;
            goto reject_client;
        }
        uint_t returned_length = host_sockaddr_len;
        int err = sockaddr_from_real(
                &sockaddr, sizeof(sockaddr), &returned_length, false);
        if (err < 0) {
            result = err;
            goto reject_client;
        }
        if (user_put(sockaddr_len_addr, returned_length)) {
            result = _EFAULT;
            goto reject_client;
        }
        err = sockaddr_copy_to_user(sockaddr_addr, &sockaddr,
                sizeof(sockaddr), (uint_t) guest_sockaddr_len,
                returned_length);
        if (err < 0) {
            result = err;
            goto reject_client;
        }
    }

    if (listener.fd->socket.domain == AF_LOCAL_) {
        fill_cred_task(current, &client_fd->socket.unix_cred);
        unix_socket_finish_peer_handshake(
                connecting_peer, client_fd);
    }

    fd_retain(client_fd);
    result = f_install_task(current, client_fd, 0);
    if (result < 0) {
        fd_close(client_fd);
        goto out_listener;
    }

    fd_close(client_fd);
    goto out_listener;

reject_client:
    unix_socket_finish_peer_handshake(connecting_peer, NULL);
    fd_close(client_fd);
out_listener:
    socket_ref_release(&listener);
    return result;
}

static dword_t copy_unix_name(void *sockaddr, const struct fd *sock) {
    struct sockaddr_ *fake_addr = sockaddr;
    fake_addr->family = PF_LOCAL_;
    size_t name_len = sock->socket.unix_name_len;
    assert(name_len <= SOCKADDR_DATA_MAX + 1);
    memcpy((byte_t *) sockaddr + offsetof(struct sockaddr_, data),
            sock->socket.unix_name, name_len);
    return (dword_t) (offsetof(struct sockaddr_, data) + name_len);
}

int_t socket_getname_ref(const struct socket_ref *socket, bool peer,
        struct sockaddr_storage *address, dword_t *length) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    assert(address != NULL && length != NULL);
    *address = (struct sockaddr_storage) {0};
    *length = 0;

    struct fd *sock = socket->fd;
    if (sock->socket.domain == AF_LOCAL_) {
        if (!peer) {
            lock(&sock->lock);
            *length = copy_unix_name(address, sock);
            unlock(&sock->lock);
            return 0;
        }

        lock(&peer_lock);
        if (!sock->socket.unix_peer_name_valid) {
            unlock(&peer_lock);
            return _ENOTCONN;
        }
        struct sockaddr_ *guest = (struct sockaddr_ *) address;
        guest->family = PF_LOCAL_;
        memcpy((byte_t *) address + offsetof(struct sockaddr_, data),
                sock->socket.unix_peer_name,
                sock->socket.unix_peer_name_len);
        *length = (dword_t) (offsetof(struct sockaddr_, data) +
                sock->socket.unix_peer_name_len);
        unlock(&peer_lock);
        return 0;
    }

    socklen_t host_length = sizeof(*address);
    lock(&sock->lock);
    int host_result = peer ?
            getpeername(sock->real_fd,
                    (struct sockaddr *) address, &host_length) :
            getsockname(sock->real_fd,
                    (struct sockaddr *) address, &host_length);
    if (host_result < 0) {
        int_t result = errno_map();
        unlock(&sock->lock);
        return result;
    }
    *length = (dword_t) host_length;
    int_t result = sockaddr_from_real(
            address, sizeof(*address), length, false);
    unlock(&sock->lock);
    return result;
}

static int_t sys_getname(fd_t sock_fd, addr_t sockaddr_addr,
        addr_t sockaddr_len_addr, bool peer) {
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;

    struct sockaddr_storage address;
    dword_t true_length;
    result = socket_getname_ref(
            &socket, peer, &address, &true_length);
    if (result < 0)
        goto out;

    sdword_t capacity;
    if (user_get(sockaddr_len_addr, capacity)) {
        result = _EFAULT;
        goto out;
    }
    if (capacity < 0) {
        result = _EINVAL;
        goto out;
    }
    if (user_put(sockaddr_len_addr, true_length)) {
        result = _EFAULT;
        goto out;
    }
    result = sockaddr_copy_to_user(sockaddr_addr, &address,
            sizeof(address), (dword_t) capacity, true_length);
out:
    socket_ref_release(&socket);
    return result;
}

int_t sys_getsockname(fd_t sock_fd, addr_t sockaddr_addr,
        addr_t sockaddr_len_addr) {
    STRACE("getsockname(%d, 0x%x, 0x%x)",
            sock_fd, sockaddr_addr, sockaddr_len_addr);
    return sys_getname(
            sock_fd, sockaddr_addr, sockaddr_len_addr, false);
}

int_t sys_getpeername(fd_t sock_fd, addr_t sockaddr_addr,
        addr_t sockaddr_len_addr) {
    STRACE("getpeername(%d, 0x%x, 0x%x)",
            sock_fd, sockaddr_addr, sockaddr_len_addr);
    return sys_getname(
            sock_fd, sockaddr_addr, sockaddr_len_addr, true);
}

int_t sys_socketpair(dword_t domain, dword_t type, dword_t protocol, addr_t sockets_addr) {
    STRACE("socketpair(%d, %d, %d, 0x%x)", domain, type, protocol, sockets_addr);
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EINVAL;
    int real_type = sock_type_to_real(type, protocol);
    if (real_type < 0)
        return _EINVAL;

    int sockets[2];
    int err = socketpair(real_domain, real_type, protocol, sockets);
    if (err < 0)
        return errno_map();

    struct fd *socket_fds[2];
    socket_fds[0] = sock_fd_wrap(
            sockets[0], domain, type, protocol, protocol);
    if (socket_fds[0] == NULL) {
        close(sockets[1]);
        return _ENOMEM;
    }
    socket_fds[1] = sock_fd_wrap(
            sockets[1], domain, type, protocol, protocol);
    if (socket_fds[1] == NULL) {
        fd_close(socket_fds[0]);
        return _ENOMEM;
    }

    fill_cred_task(current, &socket_fds[0]->socket.unix_cred);
    fill_cred_task(current, &socket_fds[1]->socket.unix_cred);
    socket_fds[0]->socket.unix_peer_cred =
            socket_fds[1]->socket.unix_cred;
    socket_fds[1]->socket.unix_peer_cred =
            socket_fds[0]->socket.unix_cred;
    socket_fds[0]->socket.unix_peer_cred_valid = true;
    socket_fds[1]->socket.unix_peer_cred_valid = true;
    // 两端发布到 fdtable 前先完成 peer 关系，避免并发观察到半初始化对象。
    lock(&peer_lock);
    socket_fds[0]->socket.unix_peer = socket_fds[1];
    socket_fds[1]->socket.unix_peer = socket_fds[0];
    socket_fds[0]->socket.unix_peer_name_valid = true;
    socket_fds[1]->socket.unix_peer_name_valid = true;
    unlock(&peer_lock);

    int fake_sockets[2];
    qword_t generations[2];
    // 局部强引用与安装代数共同保护 guest 写回期间的 close/reuse 回滚。
    struct fd *install_fds[2] = {
        fd_retain(socket_fds[0]),
        fd_retain(socket_fds[1]),
    };
    err = f_install_pair_task_tracked(current, install_fds,
            type & ~SOCKET_TYPE_MASK, fake_sockets, generations);
    if (err < 0) {
        fd_close(socket_fds[1]);
        fd_close(socket_fds[0]);
        return err;
    }

    if (user_put(sockets_addr, fake_sockets)) {
        f_close_task_if_matches(current, fake_sockets[1],
                socket_fds[1], generations[1]);
        f_close_task_if_matches(current, fake_sockets[0],
                socket_fds[0], generations[0]);
        fd_close(socket_fds[1]);
        fd_close(socket_fds[0]);
        return _EFAULT;
    }

    STRACE(" [%d, %d]", fake_sockets[0], fake_sockets[1]);
    fd_close(socket_fds[1]);
    fd_close(socket_fds[0]);
    return 0;
}

int socket_sendto_validate(const struct socket_ref *socket,
        size_t length, dword_t flags) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    bool ping = socket_sendto_prefix_size(socket) != 0;
    if (ping) {
        if (length > UINT16_MAX)
            return _EMSGSIZE;
        if (length < 8)
            return _EINVAL;
        if (flags & MSG_OOB_)
            return _EOPNOTSUPP;
    }
    bool ipv4_datagram = socket->fd->socket.domain == AF_INET_ &&
            ((socket->fd->socket.type == SOCK_DGRAM_ &&
            (socket->fd->socket.protocol == 0 ||
            socket->fd->socket.protocol == IPPROTO_UDP)) ||
            socket->fd->socket.type == SOCK_RAW_);
    if (ipv4_datagram) {
        if (length > UINT16_MAX)
            return _EMSGSIZE;
        if (flags & MSG_OOB_)
            return _EOPNOTSUPP;
    }
    if ((socket->fd->socket.domain == AF_LOCAL_ ||
            (socket->fd->socket.domain == AF_INET6_ &&
            socket->fd->socket.type == SOCK_RAW_)) &&
            (flags & MSG_OOB_))
        return _EOPNOTSUPP;
    return 0;
}

size_t socket_sendto_prefix_size(const struct socket_ref *socket) {
    assert(socket != NULL && socket->fd != NULL);
    return socket->fd->socket.type == SOCK_DGRAM_ &&
            ((socket->fd->socket.domain == AF_INET_ &&
            socket->fd->socket.protocol == IPPROTO_ICMP) ||
            (socket->fd->socket.domain == AF_INET6_ &&
            socket->fd->socket.protocol == IPPROTO_ICMPV6)) ? 8 : 0;
}

int socket_sendto_prefix_validate(const struct socket_ref *socket,
        const void *prefix, size_t prefix_size) {
    size_t required = socket_sendto_prefix_size(socket);
    assert(required != 0 && prefix != NULL && prefix_size == required);
    const byte_t *header = prefix;
    bool supported = header[1] == 0 &&
            ((socket->fd->socket.domain == AF_INET_ &&
            (header[0] == 8 || header[0] == 42)) ||
            (socket->fd->socket.domain == AF_INET6_ &&
            (header[0] == 128 || header[0] == 160)));
    return supported ? 0 : _EINVAL;
}

size_t socket_sendto_transaction_size(const struct socket_ref *socket,
        size_t length, dword_t flags) {
    size_t limit = SOCKET_IO_TRANSACTION_LIMIT;
    if (socket->fd->socket.type == SOCK_STREAM_ &&
            (flags & MSG_DONTWAIT_))
        limit = SOCKET_STREAM_NONBLOCK_TRANSACTION_LIMIT;
    return length < limit ? length : limit;
}

int socket_sendto_transaction_validate(const struct socket_ref *socket,
        size_t length) {
    assert(socket != NULL && socket->fd != NULL);
    return socket->fd->socket.type != SOCK_STREAM_ &&
            length > SOCKET_IO_TRANSACTION_LIMIT ? _EMSGSIZE : 0;
}

static int socket_inet_stream_destination_check(
        const struct socket_ref *socket, dword_t flags) {
    bool may_wait = socket_message_may_wait(socket->fd, flags);
    struct timer_time wait_deadline;
    const struct timer_time *wait_deadline_pointer = may_wait ?
            socket_message_deadline(
                    socket->fd, false, &wait_deadline) : NULL;
    bool readiness_observed = false;
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        lock(&socket->fd->lock);
        if (getpeername(socket->fd->real_fd,
                (struct sockaddr *) &peer, &peer_length) == 0) {
            socket->fd->socket.inet_connect_pending = false;
            unlock(&socket->fd->lock);
            return 0;
        }

        bool pending = socket->fd->socket.inet_connect_pending;
        int host_error = 0;
        socklen_t error_length = sizeof(host_error);
        if (getsockopt(socket->fd->real_fd, SOL_SOCKET, SO_ERROR,
                &host_error, &error_length) < 0) {
            int error = errno_map();
            unlock(&socket->fd->lock);
            return error;
        }
        if (host_error != 0 || readiness_observed)
            socket->fd->socket.inet_connect_pending = false;
        unlock(&socket->fd->lock);

        if (host_error != 0)
            return err_map(host_error);
        if (!pending || readiness_observed)
            return _EPIPE;
        if (!may_wait)
            return _EAGAIN;

        int error = socket_message_wait(socket->fd, POLL_WRITE,
                wait_deadline_pointer);
        if (error < 0)
            return error;
        readiness_observed = true;
    }
}

static int socket_sendto_destination_validate(
        const struct socket_ref *socket,
        const struct socket_address *address, dword_t flags) {
    assert(socket != NULL && socket->fd != NULL);
    if ((socket->fd->socket.domain == AF_INET_ ||
            socket->fd->socket.domain == AF_INET6_) &&
            socket->fd->socket.type == SOCK_STREAM_)
        return socket_inet_stream_destination_check(socket, flags);
    if (socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type != SOCK_STREAM_ &&
            (address == NULL || address->length == 0)) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        return getpeername(socket->fd->real_fd,
                (struct sockaddr *) &peer, &peer_length) == 0 ?
                0 : _ENOTCONN;
    }
    if (socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type == SOCK_STREAM_) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        if (address != NULL && address->length != 0)
            return getpeername(socket->fd->real_fd,
                    (struct sockaddr *) &peer, &peer_length) == 0 ?
                    _EISCONN : _EOPNOTSUPP;
        if (getpeername(socket->fd->real_fd,
                (struct sockaddr *) &peer, &peer_length) == 0)
            return 0;
        lock(&peer_lock);
        bool connected_before =
                socket->fd->socket.unix_peer_name_valid;
        unlock(&peer_lock);
        return connected_before ? _EPIPE : _ENOTCONN;
    }
    bool requires_destination = socket->fd->socket.type == SOCK_RAW_ ||
            socket_sendto_prefix_size(socket) != 0 ||
            (socket->fd->socket.type == SOCK_DGRAM_ &&
            (socket->fd->socket.protocol == 0 ||
            socket->fd->socket.protocol == IPPROTO_UDP));
    if (!requires_destination ||
            (socket->fd->socket.domain != AF_INET_ &&
            socket->fd->socket.domain != AF_INET6_) ||
            (address != NULL && address->length != 0))
        return 0;
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    return getpeername(socket->fd->real_fd,
            (struct sockaddr *) &peer, &peer_length) == 0 ?
            0 : _EDESTADDRREQ;
}

int socket_sendto_destination_check(const struct socket_ref *socket,
        const struct socket_address *address, dword_t flags) {
    int error = socket_sendto_destination_validate(
            socket, address, flags);
    return error < 0 ? socket_message_send_error(error, flags) : 0;
}

ssize_t socket_sendto_ref(const struct socket_ref *socket,
        const void *buffer, size_t length, dword_t flags,
        const struct socket_address *address) {
    int error = socket_sendto_validate(socket, length, flags);
    if (error < 0)
        return error;
    size_t prefix_size = socket_sendto_prefix_size(socket);
    if (prefix_size != 0) {
        error = socket_sendto_prefix_validate(
                socket, buffer, prefix_size);
        if (error < 0)
            return error;
    }
    error = socket_sendto_transaction_validate(socket, length);
    if (error < 0)
        return error;
    error = socket_sendto_destination_check(socket, address, flags);
    if (error < 0)
        return error;
    int real_flags = sock_flags_to_real(flags);
    bool serialize_unix_datagram =
            socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type != SOCK_STREAM_;
    bool may_wait = socket_message_may_wait(socket->fd, flags);
    struct timer_time wait_deadline;
    const struct timer_time *wait_deadline_pointer =
            serialize_unix_datagram && may_wait ?
            socket_message_deadline(
                    socket->fd, false, &wait_deadline) : NULL;
    struct unix_scm_receiver receiver = {0};
    if (serialize_unix_datagram) {
        lock(&unix_scm_io_lock);
        error = unix_scm_receiver_retain(
                socket->fd, address, &receiver);
        if (error < 0)
            goto out;
    }

    byte_t empty = 0;
    bool has_address = address != NULL && address->length != 0;
    for (;;) {
        bool receiver_discards = serialize_unix_datagram &&
                receiver.owner->socket.unix_read_shutdown;
        if (receiver_discards) {
            error = socket_message_send_error(_EPIPE, flags);
            break;
        }
        struct unix_bound_name *queued_name =
                unix_bound_message_begin(socket->fd);
        ssize_t sent = sendto(socket->fd->real_fd,
                length != 0 ? buffer : &empty, length,
                serialize_unix_datagram ?
                        real_flags | MSG_DONTWAIT : real_flags,
                has_address ?
                        (const struct sockaddr *) &address->storage : NULL,
                has_address ? address->length : 0);
        int host_error = errno;
        if (sent < 0)
            unix_bound_message_cancel(queued_name);
        if (sent >= 0) {
            error = (int) sent;
            break;
        }
        if (serialize_unix_datagram && may_wait &&
                socket_message_send_would_block(
                        socket->fd, host_error)) {
            unlock(&unix_scm_io_lock);
            error = socket_message_wait(socket->fd, POLL_WRITE,
                    wait_deadline_pointer);
            if (error < 0)
                goto release_receiver;
            lock(&unix_scm_io_lock);
            continue;
        }
        if (host_error == ENOENT && address != NULL &&
                (address->lookup_inode != NULL ||
                address->lookup_abstract != NULL))
            error = _ECONNREFUSED;
        else
            error = socket_message_send_host_error(
                    socket->fd, host_error, flags);
        break;
    }

out:
    if (serialize_unix_datagram)
        unlock(&unix_scm_io_lock);
release_receiver:
    unix_scm_receiver_release(&receiver);
    return error;
}

ssize_t socket_recvfrom_ref(const struct socket_ref *socket,
        void *buffer, size_t length, dword_t flags,
        struct socket_address *address) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    if (socket->fd->socket.domain == AF_LOCAL_) {
        dword_t message_flags;
        struct scm *scm = NULL;
        ssize_t result = socket_recvmsg_ref(socket,
                buffer, length, flags, address,
                &message_flags, &scm);
        if (scm != NULL)
            socket_scm_release(scm);
        return result;
    }
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    if ((socket->fd->socket.domain == AF_INET_ ||
            socket->fd->socket.domain == AF_INET6_) &&
            socket->fd->socket.type == SOCK_STREAM_ &&
            (flags & MSG_TRUNC_))
        real_flags &= ~MSG_TRUNC;
    bool track_unix_source = socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type != SOCK_STREAM_;
    struct socket_address discarded_address;
    struct socket_address *received_address = address != NULL ?
            address : (track_unix_source ? &discarded_address : NULL);
    if (received_address != NULL) {
        *received_address = (struct socket_address) {0};
        received_address->length = sizeof(received_address->storage);
    }
    byte_t empty = 0;
    ssize_t result = recvfrom(socket->fd->real_fd,
            length != 0 ? buffer : &empty, length,
            real_flags,
            received_address != NULL ?
                    (struct sockaddr *) &received_address->storage : NULL,
            received_address != NULL ? &received_address->length : NULL);
    if (result < 0)
        return errno_map();
    if (received_address != NULL && received_address->length != 0) {
        uint_t address_length = (uint_t) received_address->length;
        int error = sockaddr_from_real(&received_address->storage,
                sizeof(received_address->storage), &address_length,
                track_unix_source && !(flags & MSG_PEEK_));
        if (error < 0)
            return error;
        received_address->length = (socklen_t) address_length;
    }
    return result;
}

int_t sys_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len,
        dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len) {
    STRACE("sendto(%d, 0x%x, %u, %u, 0x%x, %u)",
            sock_fd, buffer_addr, len, flags,
            sockaddr_addr, sockaddr_len);
    dword_t length = len < I386_LINUX_MAX_RW_COUNT ?
            len : I386_LINUX_MAX_RW_COUNT;
    qword_t available = UINT64_C(1) + UINT32_MAX - (qword_t) buffer_addr;
    if ((qword_t) length > available)
        return _EFAULT;

    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;

    struct sockaddr_storage guest_address = {0};
    sdword_t signed_length = 0;
    struct socket_address address;
    struct socket_address *address_pointer = NULL;
    void *buffer = NULL;
    if (sockaddr_addr != 0) {
        signed_length = (sdword_t) sockaddr_len;
        if (signed_length < 0 ||
                (size_t) signed_length > sizeof(address.storage)) {
            result = _EINVAL;
            goto out;
        }
        if (signed_length != 0 && user_read(sockaddr_addr,
                &guest_address, (size_t) signed_length)) {
            result = _EFAULT;
            goto out;
        }
    }

    result = socket_sendto_validate(&socket, length, flags);
    if (result < 0)
        goto out;
    byte_t prefix[8];
    size_t prefix_size = socket_sendto_prefix_size(&socket);
    if (prefix_size != 0) {
        assert(prefix_size <= sizeof(prefix));
        if (user_read(buffer_addr, prefix, prefix_size)) {
            result = _EFAULT;
            goto out;
        }
        result = socket_sendto_prefix_validate(
                &socket, prefix, prefix_size);
        if (result < 0)
            goto out;
    }
    bool defer_unix_lookup = sockaddr_addr != 0 &&
            socket.fd->socket.domain == AF_LOCAL_ &&
            socket.fd->socket.type != SOCK_STREAM_;
    if (sockaddr_addr != 0) {
        result = socket_address_validate_for_socket(&socket,
                &guest_address, (size_t) signed_length);
        if (result < 0)
            goto out;
        if (!defer_unix_lookup) {
            result = socket_address_prepare_task(current,
                    &socket, &guest_address,
                    (size_t) signed_length, &address);
            if (result < 0)
                goto out;
            address_pointer = &address;
        }
    }
    result = socket_sendto_transaction_validate(&socket, length);
    if (result < 0)
        goto out;
    if (!defer_unix_lookup) {
        result = socket_sendto_destination_check(
                &socket, address_pointer, flags);
        if (result < 0)
            goto out;
    }
    size_t transaction_length = socket_sendto_transaction_size(
            &socket, length, flags);
    buffer = transaction_length == 0 ? NULL : malloc(transaction_length);
    if (transaction_length != 0 && buffer == NULL) {
        result = _ENOMEM;
        goto out;
    }
    if (transaction_length != 0) {
        memcpy(buffer, prefix, prefix_size);
        size_t remaining = transaction_length - prefix_size;
        if (remaining != 0 && user_read(
                buffer_addr + (addr_t) prefix_size,
                (byte_t *) buffer + prefix_size, remaining)) {
            result = _EFAULT;
            goto out;
        }
    }
    if (defer_unix_lookup) {
        result = socket_address_prepare_task(current,
                &socket, &guest_address,
                (size_t) signed_length, &address);
        if (result < 0)
            goto out;
        address_pointer = &address;
    }
    result = (int_t) socket_sendto_ref(
            &socket, buffer, transaction_length, flags, address_pointer);
out:
    free(buffer);
    if (address_pointer != NULL)
        socket_address_release(address_pointer);
    socket_ref_release(&socket);
    return result;
}

int_t sys_recvfrom(fd_t sock_fd, addr_t buffer_addr, dword_t len,
        dword_t flags, addr_t sockaddr_addr, addr_t sockaddr_len_addr) {
    STRACE("recvfrom(%d, 0x%x, %u, %u, 0x%x, 0x%x)",
            sock_fd, buffer_addr, len, flags,
            sockaddr_addr, sockaddr_len_addr);
    dword_t length = len < I386_LINUX_MAX_RW_COUNT ?
            len : I386_LINUX_MAX_RW_COUNT;
    qword_t available = UINT64_C(1) + UINT32_MAX - (qword_t) buffer_addr;
    if ((qword_t) length > available)
        return _EFAULT;
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;
    bool full_datagram_length = (flags & MSG_TRUNC_) != 0 &&
            socket.fd->socket.type != SOCK_STREAM_;
    size_t transaction_length = full_datagram_length ?
            SOCKET_IO_TRANSACTION_LIMIT :
            (length < SOCKET_IO_TRANSACTION_LIMIT ?
                    length : SOCKET_IO_TRANSACTION_LIMIT);
    void *buffer = transaction_length == 0 ? NULL :
            malloc(transaction_length);
    if (transaction_length != 0 && buffer == NULL) {
        result = _ENOMEM;
        goto out;
    }
    struct socket_address source;
    ssize_t received = socket_recvfrom_ref(&socket,
            buffer, transaction_length, flags,
            sockaddr_addr != 0 ? &source : NULL);
    if (received < 0) {
        free(buffer);
        result = (int_t) received;
        goto out;
    }
    size_t payload_size = (size_t) received < length ?
            (size_t) received : length;
    // MSG_TRUNC 可能返回大于内部事务缓冲区的真实报文长度。
    if (payload_size > transaction_length)
        payload_size = transaction_length;
    if ((socket.fd->socket.domain == AF_INET_ ||
            socket.fd->socket.domain == AF_INET6_) &&
            socket.fd->socket.type == SOCK_STREAM_ &&
            (flags & MSG_TRUNC_))
        payload_size = 0;
    if (payload_size != 0 && user_write(
            buffer_addr, buffer, payload_size)) {
        free(buffer);
        result = _EFAULT;
        goto out;
    }
    free(buffer);

    if (sockaddr_addr != 0) {
        sdword_t capacity;
        if (user_get(sockaddr_len_addr, capacity)) {
            result = _EFAULT;
            goto out;
        }
        if (capacity < 0) {
            result = _EINVAL;
            goto out;
        }
        dword_t true_length = (dword_t) source.length;
        if (user_put(sockaddr_len_addr, true_length)) {
            result = _EFAULT;
            goto out;
        }
        size_t copied = (dword_t) capacity < true_length ?
                (dword_t) capacity : true_length;
        if (copied != 0 && user_write(
                sockaddr_addr, &source.storage, copied)) {
            result = _EFAULT;
            goto out;
        }
    }
    result = (int_t) received;
out:
    socket_ref_release(&socket);
    return result;
}

int_t sys_send(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_sendto(sock_fd, buf, len, flags, 0, 0);
}

int_t sys_recv(fd_t sock_fd, addr_t buf, dword_t len, int_t flags) {
    return sys_recvfrom(sock_fd, buf, len, flags, 0, 0);
}

int_t socket_shutdown_ref(
        const struct socket_ref *socket, sdword_t how) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    if (how < SHUT_RD || how > SHUT_RDWR)
        return _EINVAL;
    struct list discarded_scm;
    list_init(&discarded_scm);
    bool darwin_unix_datagram_shutdown = false;
#ifdef __APPLE__
    darwin_unix_datagram_shutdown =
            socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type != SOCK_STREAM_;
#endif
    int_t result;
    if (darwin_unix_datagram_shutdown) {
        bool request_read = how == SHUT_RD || how == SHUT_RDWR;
        bool request_write = how == SHUT_WR || how == SHUT_RDWR;
        lock(&socket->fd->socket.unix_recv_lock);
        lock(&unix_scm_io_lock);
        int error = 0;
        if (request_read &&
                !socket->fd->socket.unix_read_shutdown) {
            struct sockaddr_storage peer;
            socklen_t peer_length = sizeof(peer);
            if (getpeername(socket->fd->real_fd,
                    (struct sockaddr *) &peer, &peer_length) == 0)
                error = unix_bound_drain_messages_locked(
                        socket->fd, &discarded_scm);
        }
        bool need_read = request_read &&
                !socket->fd->socket.unix_read_shutdown;
        bool need_write = request_write &&
                !socket->fd->socket.unix_write_shutdown;
        int effective_how = need_read ?
                (need_write ? SHUT_RDWR : SHUT_RD) : SHUT_WR;
        if (error == 0 && (need_read || need_write) &&
                shutdown(socket->fd->real_fd, effective_how) < 0)
            error = errno_map();
        if (error == 0) {
            if (request_read)
                socket->fd->socket.unix_read_shutdown = true;
            if (request_write)
                socket->fd->socket.unix_write_shutdown = true;
        }
        unlock(&unix_scm_io_lock);
        unlock(&socket->fd->socket.unix_recv_lock);
        result = error;
    } else {
        bool inet_socket = socket->fd->socket.domain == AF_INET_ ||
                socket->fd->socket.domain == AF_INET6_;
        if (inet_socket)
            lock(&socket->fd->lock);
        int error = shutdown(socket->fd->real_fd, (int) how);
        result = error == 0 ? 0 : errno_map();
#if !defined(__APPLE__)
        if (inet_socket && result == 0 && how != SHUT_WR &&
                socket->fd->socket.listening) {
            sockrestart_end_listen(socket->fd);
            socket->fd->socket.listening = false;
        }
#endif
        if (inet_socket)
            unlock(&socket->fd->lock);
    }
    socket_scm_release_list(&discarded_scm);
    return result;
}

int_t sys_shutdown(fd_t sock_fd, dword_t how) {
    STRACE("shutdown(%d, %d)", sock_fd, how);
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;
    result = socket_shutdown_ref(&socket, (sdword_t) how);
    socket_ref_release(&socket);
    return result;
}

#define DEFAULT_TCP_CONGESTION "cubic"
#define LINUX_TCP_CA_NAME_MAX 16
#define LINUX_ICMP6_FILTER_SIZE 32

struct linux_socket_linger {
    sdword_t enabled;
    sdword_t seconds;
};

struct linux_socket_timeval {
    int64_t seconds;
    int64_t microseconds;
};

struct linux_socket_timeval32 {
    int32_t seconds;
    int32_t microseconds;
};

_Static_assert(sizeof(struct linux_socket_linger) == 8,
        "Linux linger wire 必须固定为 8 字节");
_Static_assert(sizeof(struct linux_socket_timeval) == 16,
        "AArch64 与 i386 NEW timeval wire 必须固定为 16 字节");
_Static_assert(sizeof(struct linux_socket_timeval32) == 8,
        "i386 OLD timeval wire 必须固定为 8 字节");
_Static_assert(sizeof(struct ucred_) == 12,
        "Linux ucred wire 必须固定为 12 字节");

static bool socket_option_is_timeout(sdword_t level, sdword_t option) {
    return level == SOL_SOCKET_ &&
            (option == SO_RCVTIMEO_OLD_ ||
            option == SO_SNDTIMEO_OLD_ ||
            option == SO_RCVTIMEO_NEW_ ||
            option == SO_SNDTIMEO_NEW_);
}

static bool socket_option_is_old_timeout(sdword_t level, sdword_t option) {
    return level == SOL_SOCKET_ &&
            (option == SO_RCVTIMEO_OLD_ || option == SO_SNDTIMEO_OLD_);
}

static size_t socket_timeout_wire_size(sdword_t level, sdword_t option,
        enum socket_guest_abi guest_abi) {
    if (guest_abi == SOCKET_GUEST_I386 &&
            socket_option_is_old_timeout(level, option))
        return sizeof(struct linux_socket_timeval32);
    return sizeof(struct linux_socket_timeval);
}

static bool socket_ip_option_allows_empty(sdword_t option) {
    return option == IP_TOS_ || option == IP_HDRINCL_ ||
            option == IP_RETOPTS_ ||
            option == IP_MTU_DISCOVER_ || option == IP_RECVERR_ ||
            option == IP_RECVTTL_ || option == IP_RECVTOS_;
}

static bool socket_ip_option_known(sdword_t option) {
    return option == IP_TOS_ || option == IP_TTL_ ||
            option == IP_HDRINCL_ || option == IP_RETOPTS_ ||
            option == IP_MTU_DISCOVER_ || option == IP_RECVERR_ ||
            option == IP_RECVTTL_ || option == IP_RECVTOS_;
}

ssize_t socket_setsockopt_value_size(
        sdword_t level, sdword_t option, sdword_t value_length,
        enum socket_guest_abi guest_abi) {
    if (value_length < 0)
        return _EINVAL;
    if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        if (value_length < 1)
            return _EINVAL;
        return value_length < LINUX_TCP_CA_NAME_MAX - 1 ?
                value_length : LINUX_TCP_CA_NAME_MAX - 1;
    }
    if (level == IPPROTO_ICMPV6 && option == ICMP6_FILTER_) {
        return value_length < LINUX_ICMP6_FILTER_SIZE ?
                value_length : LINUX_ICMP6_FILTER_SIZE;
    }

    bool known_level = level == SOL_SOCKET_ || level == IPPROTO_IP ||
            level == IPPROTO_TCP || level == IPPROTO_IPV6;
    if (!known_level)
        return 0;
    if (level == IPPROTO_IP && !socket_ip_option_known(option))
        return 0;
    if (level == IPPROTO_IP && value_length < (sdword_t) sizeof(sdword_t)) {
        if (value_length == 0)
            return socket_ip_option_allows_empty(option) ? 0 : _EINVAL;
        return 1;
    }
    if (value_length < (sdword_t) sizeof(sdword_t))
        return _EINVAL;
    if (level == SOL_SOCKET_ && option == SO_LINGER_)
        return value_length < (sdword_t) sizeof(struct linux_socket_linger) ?
                sizeof(sdword_t) : sizeof(struct linux_socket_linger);
    if (socket_option_is_timeout(level, option)) {
        size_t wire_size = socket_timeout_wire_size(
                level, option, guest_abi);
        return value_length < (sdword_t) wire_size ?
                sizeof(sdword_t) : (ssize_t) wire_size;
    }
    return sizeof(sdword_t);
}

static int_t socket_set_timeout_ref(const struct socket_ref *socket,
        bool receive, const struct linux_socket_timeval *guest) {
    if (guest->microseconds < 0 || guest->microseconds >= 1000000)
        return _EDOM;
    struct timeval host = {0};
    if (guest->seconds >= 0) {
        int64_t maximum_seconds = sizeof(host.tv_sec) < sizeof(int64_t) ?
                INT32_MAX : INT64_MAX;
        int64_t seconds = guest->seconds < maximum_seconds ?
                guest->seconds : maximum_seconds;
        host.tv_sec = (__typeof__(host.tv_sec)) seconds;
        host.tv_usec = (__typeof__(host.tv_usec)) guest->microseconds;
    }
    int option = receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    if (setsockopt(socket->fd->real_fd, SOL_SOCKET,
            option, &host, sizeof(host)) < 0)
        return errno_map();
    return 0;
}

static int_t socket_setsockopt_locked(const struct socket_ref *socket,
        sdword_t level, sdword_t option,
        const void *value, sdword_t value_length,
        enum socket_guest_abi guest_abi) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    ssize_t copied = socket_setsockopt_value_size(
            level, option, value_length, guest_abi);
    if (copied < 0)
        return (int_t) copied;
    assert(copied == 0 || value != NULL);

    if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        char name[LINUX_TCP_CA_NAME_MAX] = {0};
        memcpy(name, value, (size_t) copied);
#if defined(__APPLE__)
        return strcmp(name, DEFAULT_TCP_CONGESTION) == 0 ? 0 : _ENOENT;
#else
        if (setsockopt(socket->fd->real_fd, IPPROTO_TCP,
                TCP_CONGESTION, name, (socklen_t) copied) < 0)
            return errno_map();
        return 0;
#endif
    }

    if (level == IPPROTO_ICMPV6 && option == ICMP6_FILTER_)
        return 0;
    if (level == IPPROTO_IP &&
            (option == IP_MTU_DISCOVER_ || option == IP_RECVERR_))
        return 0;
    if (level == IPPROTO_IPV6 && option == IPV6_RECVERR_)
        return 0;
    if (level == IPPROTO_TCP && option == TCP_DEFER_ACCEPT_)
        return 0;

    if (socket_option_is_timeout(level, option)) {
        size_t wire_size = socket_timeout_wire_size(
                level, option, guest_abi);
        if (value_length < (sdword_t) wire_size)
            return _EINVAL;
        struct linux_socket_timeval timeout = {0};
        if (wire_size == sizeof(struct linux_socket_timeval32)) {
            struct linux_socket_timeval32 timeout32;
            memcpy(&timeout32, value, sizeof(timeout32));
            timeout.seconds = timeout32.seconds;
            timeout.microseconds = timeout32.microseconds;
        } else {
            memcpy(&timeout, value, sizeof(timeout));
        }
        bool receive = option == SO_RCVTIMEO_OLD_ ||
                option == SO_RCVTIMEO_NEW_;
        return socket_set_timeout_ref(socket, receive, &timeout);
    }

    if (level == SOL_SOCKET_ && option == SO_LINGER_) {
        if (value_length < (sdword_t) sizeof(struct linux_socket_linger))
            return _EINVAL;
        struct linux_socket_linger guest;
        memcpy(&guest, value, sizeof(guest));
        struct linger host = {
            .l_onoff = guest.enabled,
            .l_linger = guest.seconds,
        };
        if (setsockopt(socket->fd->real_fd, SOL_SOCKET,
                SO_LINGER, &host, sizeof(host)) < 0)
            return errno_map();
        return 0;
    }

    if (level == SOL_SOCKET_ &&
            (option == SO_TYPE_ || option == SO_ERROR_ ||
            option == SO_PROTOCOL_ || option == SO_DOMAIN_))
        return _ENOPROTOOPT;

    int real_option = sock_opt_to_real(option, level);
    int real_level = sock_level_to_real(level);
    if (real_option < 0 || real_level < 0)
        return _ENOPROTOOPT;
    if (real_option == 0)
        return 0;

    sdword_t integer = 0;
    if (copied == 1)
        integer = *(const uint8_t *) value;
    else if (copied >= (ssize_t) sizeof(integer))
        memcpy(&integer, value, sizeof(integer));
    if (setsockopt(socket->fd->real_fd, real_level,
            real_option, &integer, sizeof(integer)) < 0)
        return errno_map();
    return 0;
}

int_t socket_setsockopt_ref(const struct socket_ref *socket,
        sdword_t level, sdword_t option,
        const void *value, sdword_t value_length,
        enum socket_guest_abi guest_abi) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    lock(&socket->fd->lock);
    int_t result = socket_setsockopt_locked(socket,
            level, option, value, value_length, guest_abi);
    unlock(&socket->fd->lock);
    return result;
}

int_t sys_setsockopt(fd_t sock_fd, dword_t level, dword_t option,
        addr_t value_addr, dword_t value_len) {
    STRACE("setsockopt(%d, %d, %d, 0x%x, %d)",
            sock_fd, level, option, value_addr, value_len);
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;
    sdword_t signed_level = (sdword_t) level;
    sdword_t signed_option = (sdword_t) option;
    sdword_t signed_length = (sdword_t) value_len;
    ssize_t copied = socket_setsockopt_value_size(
            signed_level, signed_option, signed_length,
            SOCKET_GUEST_I386);
    if (copied < 0) {
        result = (int_t) copied;
        goto out;
    }
    byte_t value[LINUX_ICMP6_FILTER_SIZE] = {0};
    assert((size_t) copied <= sizeof(value));
    if (copied != 0 && user_read(
            value_addr, value, (size_t) copied)) {
        result = _EFAULT;
        goto out;
    }
    result = socket_setsockopt_ref(&socket,
            signed_level, signed_option, value, signed_length,
            SOCKET_GUEST_I386);
out:
    socket_ref_release(&socket);
    return result;
}

static void socket_option_store(struct socket_option_result *result,
        const void *value, size_t value_length, sdword_t capacity) {
    assert(result != NULL && capacity >= 0);
    assert(value_length <= sizeof(result->value));
    size_t copied = (size_t) capacity < value_length ?
            (size_t) capacity : value_length;
    if (copied != 0)
        memcpy(result->value, value, copied);
    result->length = (dword_t) copied;
}

static int_t socket_host_getsockopt_ref(
        const struct socket_ref *socket, int level, int option,
        void *value, socklen_t *value_length) {
    lock(&socket->fd->lock);
    int host_result = getsockopt(socket->fd->real_fd,
            level, option, value, value_length);
    int_t result = host_result < 0 ? errno_map() : 0;
    unlock(&socket->fd->lock);
    return result;
}

static int_t socket_get_error_ref(
        const struct socket_ref *socket, sdword_t *guest_error) {
    struct fd *sock = socket->fd;
    bool inet_socket = sock->socket.domain == AF_INET_ ||
            sock->socket.domain == AF_INET6_;
    uint64_t connect_generation = 0;
    enum unix_connect_error_phase error_phase =
            unix_bound_connect_error_snapshot(
                    sock, &connect_generation);
    lock(&peer_lock);
    bool handshake_pending =
            sock->socket.unix_peer_handshake_pending;
    unlock(&peer_lock);

    int host_error = 0;
    if (error_phase == UNIX_CONNECT_ERROR_ASYNC ||
            (error_phase == UNIX_CONNECT_ERROR_NONE &&
            !handshake_pending)) {
        socklen_t host_length = sizeof(host_error);
        if (inet_socket)
            lock(&sock->lock);
        int host_result = getsockopt(sock->real_fd,
                SOL_SOCKET, SO_ERROR, &host_error, &host_length);
        if (host_result < 0) {
            int_t result = errno_map();
            if (inet_socket)
                unlock(&sock->lock);
            return result;
        }
        if (inet_socket && host_error != 0)
            sock->socket.inet_connect_pending = false;
        if (inet_socket)
            unlock(&sock->lock);
    }
    if (host_error != 0 &&
            error_phase == UNIX_CONNECT_ERROR_ASYNC)
        (void) unix_bound_cancel_peer(sock, connect_generation);
    *guest_error = host_error == 0 ? 0 : -err_map(host_error);
    return 0;
}

static int_t socket_get_timeout_ref(const struct socket_ref *socket,
        sdword_t option, sdword_t capacity,
        enum socket_guest_abi guest_abi,
        struct socket_option_result *result) {
    struct timeval host = {0};
    socklen_t host_length = sizeof(host);
    int host_option = option == SO_RCVTIMEO_OLD_ ||
            option == SO_RCVTIMEO_NEW_ ? SO_RCVTIMEO : SO_SNDTIMEO;
    int_t error = socket_host_getsockopt_ref(socket,
            SOL_SOCKET, host_option, &host, &host_length);
    if (error < 0)
        return error;

    struct linux_socket_timeval timeout = {
        .seconds = host.tv_sec,
        .microseconds = host.tv_usec,
    };
    if (socket_timeout_wire_size(
            SOL_SOCKET_, option, guest_abi) ==
            sizeof(struct linux_socket_timeval32)) {
        struct linux_socket_timeval32 timeout32 = {
            .seconds = (int32_t) timeout.seconds,
            .microseconds = (int32_t) timeout.microseconds,
        };
        socket_option_store(result,
                &timeout32, sizeof(timeout32), capacity);
    } else {
        socket_option_store(result,
                &timeout, sizeof(timeout), capacity);
    }
    return 0;
}

int_t socket_getsockopt_ref(const struct socket_ref *socket,
        sdword_t level, sdword_t option, sdword_t capacity,
        enum socket_guest_abi guest_abi,
        struct socket_option_result *result) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    assert(result != NULL);
    *result = (struct socket_option_result) {
        .copy_order = level == SOL_SOCKET_ ?
                SOCKET_OPTION_VALUE_FIRST :
                SOCKET_OPTION_LENGTH_FIRST,
    };
    if (capacity < 0)
        return _EINVAL;

    struct fd *sock = socket->fd;
    if (level == SOL_SOCKET_ &&
            (option == SO_DOMAIN_ || option == SO_TYPE_ ||
            option == SO_PROTOCOL_ || option == SO_ACCEPTCONN_)) {
        sdword_t integer;
        lock(&sock->lock);
        if (option == SO_DOMAIN_)
            integer = sock->socket.domain;
        else if (option == SO_TYPE_)
            integer = sock->socket.type;
        else if (option == SO_PROTOCOL_)
            integer = sock->socket.guest_protocol;
        else
            integer = sock->socket.listening;
        unlock(&sock->lock);
        socket_option_store(
                result, &integer, sizeof(integer), capacity);
        return 0;
    }

    if (level == SOL_SOCKET_ && option == SO_PEERCRED_) {
        struct ucred_ credentials;
        lock(&peer_lock);
        if (sock->socket.domain != AF_LOCAL_ ||
                !sock->socket.unix_peer_cred_valid) {
            credentials = (struct ucred_) {
                .pid = 0,
                .uid = (uid_t_) -1,
                .gid = (uid_t_) -1,
            };
        } else {
            credentials = sock->socket.unix_peer_cred;
        }
        unlock(&peer_lock);
        socket_option_store(result, &credentials,
                sizeof(credentials), capacity);
        return 0;
    }

    if (level == SOL_SOCKET_ && option == SO_ERROR_) {
        sdword_t guest_error;
        int_t error = socket_get_error_ref(socket, &guest_error);
        if (error < 0)
            return error;
        socket_option_store(result, &guest_error,
                sizeof(guest_error), capacity);
        return 0;
    }

    if (level == SOL_SOCKET_ && option == SO_LINGER_) {
        struct linger host = {0};
        socklen_t host_length = sizeof(host);
        int_t error = socket_host_getsockopt_ref(socket,
                SOL_SOCKET, SO_LINGER, &host, &host_length);
        if (error < 0)
            return error;
        struct linux_socket_linger guest = {
            .enabled = host.l_onoff,
            .seconds = host.l_linger,
        };
        socket_option_store(
                result, &guest, sizeof(guest), capacity);
        return 0;
    }

    if (socket_option_is_timeout(level, option))
        return socket_get_timeout_ref(socket,
                option, capacity, guest_abi, result);

    if (level == IPPROTO_TCP && option == TCP_CONGESTION_) {
        char congestion[LINUX_TCP_CA_NAME_MAX] = {0};
        size_t name_length = strlen(DEFAULT_TCP_CONGESTION);
        assert(name_length < sizeof(congestion));
        memcpy(congestion, DEFAULT_TCP_CONGESTION, name_length);
        socket_option_store(result,
                congestion, sizeof(congestion), capacity);
        return 0;
    }

#if defined(__APPLE__)
    if (level == IPPROTO_TCP && option == TCP_INFO_) {
        struct tcp_connection_info connection = {0};
        socklen_t host_length = sizeof(connection);
        int_t error = socket_host_getsockopt_ref(socket,
                IPPROTO_TCP, TCP_CONNECTION_INFO,
                &connection, &host_length);
        if (error < 0)
            return error;

        // Darwin 的状态编号与 Linux 不同，且 iOS 不公开 tcp_fsm.h。
        static const uint8_t state_table[] = {
            7, 10, 2, 3, 1, 8, 4, 11, 9, 5, 6,
        };
        uint8_t state = connection.tcpi_state < array_size(state_table) ?
                state_table[connection.tcpi_state] : 7;
        struct tcp_info_ info = {
            .state = state,
            .options = connection.tcpi_options,
            .snd_wscale = connection.tcpi_snd_wscale,
            .rcv_wscale = connection.tcpi_rcv_wscale,
            .rto = connection.tcpi_rto * 1000,
            .snd_mss = connection.tcpi_maxseg,
            .rtt = connection.tcpi_srtt * 1000,
            .rttvar = connection.tcpi_rttvar * 1000,
            .snd_ssthresh = connection.tcpi_snd_ssthresh,
            .snd_cwnd = connection.tcpi_maxseg == 0 ? 0 :
                    connection.tcpi_snd_cwnd /
                    connection.tcpi_maxseg,
            .total_retrans = connection.tcpi_txretransmitpackets,
        };
        socket_option_store(result, &info, sizeof(info), capacity);
        return 0;
    }
#endif

    int host_option = sock_opt_to_real(option, level);
    int host_level = sock_level_to_real(level);
    if (host_option < 0 || host_level < 0)
        return _ENOPROTOOPT;
    if (host_option == 0) {
        sdword_t zero = 0;
        socket_option_store(result, &zero, sizeof(zero), capacity);
        return 0;
    }

    byte_t host_value[SOCKET_OPTION_VALUE_MAX] = {0};
    socklen_t host_length = sizeof(host_value);
    if (level == IPPROTO_TCP && option == TCP_INFO_)
        host_length = sizeof(struct tcp_info_);
    int_t error = socket_host_getsockopt_ref(socket,
            host_level, host_option, host_value, &host_length);
    if (error < 0)
        return error;
    if (host_length > sizeof(host_value))
        host_length = sizeof(host_value);

    if (level == IPPROTO_IP && capacity > 0 && capacity < 4 &&
            host_length >= sizeof(sdword_t)) {
        sdword_t integer;
        memcpy(&integer, host_value, sizeof(integer));
        if (integer >= 0 && integer <= UINT8_MAX) {
            uint8_t short_value = (uint8_t) integer;
            socket_option_store(result,
                    &short_value, sizeof(short_value), capacity);
            return 0;
        }
    }
    socket_option_store(result,
            host_value, host_length, capacity);
    return 0;
}

static int_t socket_getsockopt_copy_to_user(
        const struct socket_option_result *option,
        addr_t value_addr, addr_t len_addr) {
    if (option->copy_order == SOCKET_OPTION_VALUE_FIRST) {
        if (option->length != 0 && user_write(
                value_addr, option->value, option->length))
            return _EFAULT;
        if (user_put(len_addr, option->length))
            return _EFAULT;
    } else {
        if (user_put(len_addr, option->length))
            return _EFAULT;
        if (option->length != 0 && user_write(
                value_addr, option->value, option->length))
            return _EFAULT;
    }
    return 0;
}

int_t sys_getsockopt(fd_t sock_fd, dword_t level, dword_t option,
        addr_t value_addr, dword_t len_addr) {
    STRACE("getsockopt(%d, %d, %d, %#x, %#x)",
            sock_fd, level, option, value_addr, len_addr);
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;
    sdword_t capacity;
    if (user_get(len_addr, capacity)) {
        result = _EFAULT;
        goto out;
    }
    struct socket_option_result value;
    result = socket_getsockopt_ref(&socket,
            (sdword_t) level, (sdword_t) option, capacity,
            SOCKET_GUEST_I386, &value);
    if (result == 0)
        result = socket_getsockopt_copy_to_user(
                &value, value_addr, len_addr);
out:
    socket_ref_release(&socket);
    return result;
}

static struct scm *socket_scm_allocate_task(
        struct task *task, unsigned count) {
    struct scm *scm = calloc(1,
            sizeof(*scm) + count * sizeof(*scm->fds));
    if (scm == NULL)
        return NULL;
    list_init(&scm->queue);
    list_init(&scm->inflight_queue);
    if (task != NULL) {
        struct task_credentials credentials;
        task_credentials_snapshot(task, &credentials);
        scm->sender_uid = credentials.uid;
        scm->sender_nofile = rlimit_task(task, RLIMIT_NOFILE_);
        // 项目尚未建模 capability；有效 uid 0 对应现有 superuser 语义。
        scm->sender_limit_exempt = credentials.euid == 0;
    }
    return scm;
}

static qword_t socket_scm_inflight_for_uid_locked(uid_t_ uid) {
    qword_t count = 0;
    struct scm *scm;
    list_for_each_entry(
            &unix_scm_inflight, scm, inflight_queue) {
        if (scm->sender_uid != uid)
            continue;
        if (UINT64_MAX - count < scm->num_fds)
            return UINT64_MAX;
        count += scm->num_fds;
    }
    return count;
}

static int socket_scm_send_check_locked(const struct scm *scm) {
    assert(scm != NULL && !scm->inflight);
    if (scm->sender_limit_exempt)
        return 0;
    qword_t inflight =
            socket_scm_inflight_for_uid_locked(scm->sender_uid);
    return inflight > scm->sender_nofile ? _ETOOMANYREFS : 0;
}

static bool socket_scm_graph_fd(const struct fd *fd) {
    return fd != NULL && fd->ops == &socket_fdops &&
            fd->socket.domain == AF_LOCAL_;
}

void socket_scm_ref_drop_prepare(
        struct fd *fd, struct socket_scm_ref_drop *drop) {
    *drop = (struct socket_scm_ref_drop) {0};
    if (!socket_scm_graph_fd(fd))
        return;
    drop->tracked = true;
    drop->edge_generation = atomic_load(
            &unix_scm_edge_generation);
    drop->incoming = atomic_load(
            &fd->socket.unix_scm_incoming);
}

void socket_scm_ref_drop_complete(
        const struct socket_scm_ref_drop *drop,
        unsigned remaining) {
    if (drop->tracked &&
            ((drop->incoming != 0 &&
                    remaining <= drop->incoming) ||
            atomic_load(&unix_scm_edge_generation) !=
                    drop->edge_generation))
        atomic_store(&unix_scm_gc_requested, true);
}

static bool socket_scm_edge_add_locked(struct fd *fd) {
    if (!socket_scm_graph_fd(fd))
        return false;
    unsigned previous = atomic_fetch_add(
            &fd->socket.unix_scm_incoming, 1);
    assert(previous != UINT_MAX);
    if (previous == 0)
        list_add_tail(&unix_scm_vertices,
                &fd->socket.unix_scm_vertex);
    atomic_fetch_add(&unix_scm_edge_generation, 1);
    // detached SCM 引用发布为图边后，只有失去最后非图引用才可能成环。
    return fd_refcount_read(fd) <= previous + 1;
}

static void socket_scm_edge_remove_locked(struct fd *fd) {
    if (!socket_scm_graph_fd(fd))
        return;
    unsigned previous = atomic_fetch_sub(
            &fd->socket.unix_scm_incoming, 1);
    assert(previous != 0);
    if (previous == 1)
        list_remove(&fd->socket.unix_scm_vertex);
}

static void socket_scm_publish_locked(
        struct scm *scm, struct fd *receiver) {
    assert(scm != NULL && receiver != NULL && !scm->inflight);
    scm->receiver = receiver;
    scm->inflight = true;
    bool candidate_edge = false;
    for (unsigned index = 0; index < scm->num_fds; index++)
        candidate_edge |= socket_scm_edge_add_locked(scm->fds[index]);
    list_add_tail(&unix_scm_inflight, &scm->inflight_queue);
    // 新边只有从既有图顶点出发且目标同时失去最后外部根时才可能闭环。
    if (candidate_edge && socket_scm_graph_fd(receiver) &&
            atomic_load(&receiver->socket.unix_scm_incoming) != 0)
        atomic_store(&unix_scm_gc_requested, true);
}

static void socket_scm_unpublish_locked(struct scm *scm) {
    assert(scm != NULL && scm->inflight);
    list_remove(&scm->inflight_queue);
    for (unsigned index = 0; index < scm->num_fds; index++)
        socket_scm_edge_remove_locked(scm->fds[index]);
    scm->receiver = NULL;
    scm->inflight = false;
}

qword_t socket_scm_inflight_count(uid_t_ uid) {
    lock(&unix_scm_io_lock);
    qword_t count = socket_scm_inflight_for_uid_locked(uid);
    unlock(&unix_scm_io_lock);
    return count;
}

static bool socket_scm_receiver_live_locked(const struct scm *scm) {
    struct fd *receiver = scm->receiver;
    assert(socket_scm_graph_fd(receiver));
    unsigned incoming = atomic_load(
            &receiver->socket.unix_scm_incoming);
    return incoming == 0 || receiver->socket.unix_scm_gc_live;
}

static void socket_scm_collect_once(void) {
    struct list garbage;
    list_init(&garbage);

    lock(&unix_scm_io_lock);
    if (list_empty(&unix_scm_vertices)) {
        unlock(&unix_scm_io_lock);
        return;
    }

    struct fd *vertex;
    list_for_each_entry(
            &unix_scm_vertices, vertex, socket.unix_scm_vertex) {
        unsigned incoming = atomic_load(
                &vertex->socket.unix_scm_incoming);
        unsigned references = fd_refcount_read(vertex);
        // 不等于而非仅大于：计数异常时宁可延期，也不能提前回收。
        vertex->socket.unix_scm_gc_live = references != incoming;
        vertex->socket.unix_scm_gc_collect = false;
    }

    bool changed;
    do {
        changed = false;
        struct scm *scm;
        list_for_each_entry(
                &unix_scm_inflight, scm, inflight_queue) {
            if (!socket_scm_receiver_live_locked(scm))
                continue;
            for (unsigned index = 0;
                    index < scm->num_fds; index++) {
                struct fd *transferred = scm->fds[index];
                unsigned incoming = socket_scm_graph_fd(transferred) ?
                        atomic_load(&transferred->socket.unix_scm_incoming) :
                        0;
                if (!socket_scm_graph_fd(transferred) ||
                        incoming == 0 ||
                        transferred->socket.unix_scm_gc_live)
                    continue;
                transferred->socket.unix_scm_gc_live = true;
                changed = true;
            }
        }
    } while (changed);

    list_for_each_entry(
            &unix_scm_vertices, vertex, socket.unix_scm_vertex) {
        if (vertex->socket.unix_scm_gc_live)
            continue;
        vertex->socket.unix_scm_gc_collect = true;
        atomic_fetch_or(&vertex->refcount,
                FD_REFCOUNT_ACQUIRE_BLOCKED);
    }

    bool retry = false;
    list_for_each_entry(
            &unix_scm_vertices, vertex, socket.unix_scm_vertex) {
        if (!vertex->socket.unix_scm_gc_collect)
            continue;
        unsigned incoming = atomic_load(
                &vertex->socket.unix_scm_incoming);
        if (fd_refcount_read(vertex) != incoming) {
            retry = true;
            break;
        }
    }
    if (retry) {
        list_for_each_entry(
                &unix_scm_vertices, vertex,
                socket.unix_scm_vertex) {
            if (vertex->socket.unix_scm_gc_collect)
                atomic_fetch_and(&vertex->refcount,
                        ~FD_REFCOUNT_ACQUIRE_BLOCKED);
            vertex->socket.unix_scm_gc_live = false;
            vertex->socket.unix_scm_gc_collect = false;
        }
        unlock(&unix_scm_io_lock);
        return;
    }

    struct scm *scm, *temporary;
    list_for_each_entry_safe(
            &unix_scm_inflight, scm, temporary,
            inflight_queue) {
        if (!scm->receiver->socket.unix_scm_gc_collect)
            continue;
        list_remove(&scm->queue);
        socket_scm_unpublish_locked(scm);
        list_add_tail(&garbage, &scm->queue);
    }
    list_for_each_entry(
            &unix_scm_vertices, vertex, socket.unix_scm_vertex) {
        vertex->socket.unix_scm_gc_live = false;
        vertex->socket.unix_scm_gc_collect = false;
    }
    unlock(&unix_scm_io_lock);

    socket_scm_release_list(&garbage);
}

static void socket_scm_try_collect(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(
            &unix_scm_gc_running, &expected, true))
        return;
    do {
        atomic_store(&unix_scm_gc_requested, false);
        socket_scm_collect_once();
    } while (atomic_exchange(&unix_scm_gc_requested, false));
    atomic_store(&unix_scm_gc_running, false);
    if (atomic_load(&unix_scm_gc_requested))
        socket_scm_try_collect();
}

void socket_scm_collect_now(void) {
    atomic_store(&unix_scm_gc_requested, true);
    socket_scm_try_collect();
}

void socket_scm_collect_checkpoint(void) {
    if (atomic_load(&unix_scm_gc_requested))
        socket_scm_try_collect();
}

void socket_scm_release(struct scm *scm) {
    assert(scm != NULL && !scm->inflight);
    for (unsigned i = 0; i < scm->num_fds; i++)
        if (scm->fds[i] != NULL)
            fd_close(scm->fds[i]);
    free(scm);
}

int socket_scm_create_task(struct task *task,
        const fd_t *numbers, unsigned count, struct scm **scm_pointer) {
    assert(task != NULL && scm_pointer != NULL &&
            count <= SOCKET_SCM_MAX_FDS);
    *scm_pointer = NULL;
    if (count == 0)
        return 0;
    struct scm *scm = socket_scm_allocate_task(task, count);
    if (scm == NULL)
        return _ENOMEM;
    for (unsigned index = 0; index < count; index++) {
        struct fd *transferred =
                f_get_task_retain(task, numbers[index]);
        if (transferred == NULL) {
            socket_scm_release(scm);
            return _EBADF;
        }
        scm->fds[scm->num_fds++] = transferred;
    }
    *scm_pointer = scm;
    return 0;
}

static struct scm *socket_scm_clone(const struct scm *source) {
    struct scm *clone =
            socket_scm_allocate_task(NULL, source->num_fds);
    if (clone == NULL)
        return NULL;
    for (unsigned index = 0; index < source->num_fds; index++)
        clone->fds[clone->num_fds++] =
                fd_retain(source->fds[index]);
    return clone;
}

struct i386_message_iov {
    struct iovec_ *guest;
    struct iovec *host;
    size_t count;
    size_t transaction_size;
};

static void i386_message_iov_destroy(struct i386_message_iov *iov) {
    if (iov->host != NULL) {
        for (size_t index = 0; index < iov->count; index++)
            free(iov->host[index].iov_base);
    }
    free(iov->host);
    free(iov->guest);
    *iov = (struct i386_message_iov) {0};
}

static int i386_message_iov_prepare(const struct socket_ref *socket,
        addr_t guest_address, uint_t count, dword_t flags,
        bool copy_from_guest, struct i386_message_iov *iov) {
    *iov = (struct i386_message_iov) {0};
    if (count > I386_LINUX_IOV_MAX)
        return _EMSGSIZE;
    iov->count = count;
    if (count == 0)
        return 0;

    iov->guest = calloc(count, sizeof(*iov->guest));
    iov->host = calloc(count, sizeof(*iov->host));
    if (iov->guest == NULL || iov->host == NULL) {
        i386_message_iov_destroy(iov);
        return _ENOMEM;
    }
    if (user_read(guest_address, iov->guest,
            count * sizeof(*iov->guest))) {
        i386_message_iov_destroy(iov);
        return _EFAULT;
    }

    uint64_t requested = 0;
    for (size_t index = 0; index < count; index++)
        requested += iov->guest[index].len;
    if (copy_from_guest && socket->fd->socket.type != SOCK_STREAM_ &&
            requested > SOCKET_IO_TRANSACTION_LIMIT) {
        i386_message_iov_destroy(iov);
        return _EMSGSIZE;
    }

    size_t limit = SOCKET_IO_TRANSACTION_LIMIT;
    if (socket->fd->socket.type == SOCK_STREAM_ &&
            (flags & MSG_DONTWAIT_))
        limit = SOCKET_STREAM_NONBLOCK_TRANSACTION_LIMIT;
    iov->transaction_size = requested < limit ?
            (size_t) requested : limit;
    size_t remaining = iov->transaction_size;
    for (size_t index = 0; index < count; index++) {
        size_t length = iov->guest[index].len < remaining ?
                iov->guest[index].len : remaining;
        iov->host[index].iov_len = length;
        if (length != 0) {
            iov->host[index].iov_base = malloc(length);
            if (iov->host[index].iov_base == NULL) {
                i386_message_iov_destroy(iov);
                return _ENOMEM;
            }
            if (copy_from_guest && user_read(iov->guest[index].base,
                    iov->host[index].iov_base, length)) {
                i386_message_iov_destroy(iov);
                return _EFAULT;
            }
        }
        remaining -= length;
    }
    return 0;
}

static int i386_message_control_read(const struct msghdr_ *message,
        uint8_t **control) {
    *control = NULL;
    if (message->msg_controllen == 0)
        return 0;
    if (message->msg_controllen > I386_SOCKET_CONTROL_LIMIT)
        return _EINVAL;
    if (message->msg_control == 0)
        return _EFAULT;
    *control = malloc(message->msg_controllen);
    if (*control == NULL)
        return _ENOMEM;
    if (user_read(message->msg_control,
            *control, message->msg_controllen)) {
        free(*control);
        *control = NULL;
        return _EFAULT;
    }
    return 0;
}

static int i386_scm_rights_count(const uint8_t *control,
        size_t control_length, unsigned *count) {
    *count = 0;
    for (size_t offset = 0;
            control_length - offset >= sizeof(struct cmsghdr_);) {
        struct cmsghdr_ header;
        memcpy(&header, control + offset, sizeof(header));
        size_t remaining = control_length - offset;
        if (header.len < sizeof(header) || header.len > remaining)
            return _EINVAL;
        if (header.level == SOL_SOCKET_) {
            if (header.type != SCM_RIGHTS_)
                return _EINVAL;
            size_t payload_length = header.len - sizeof(header);
            size_t message_count = payload_length / sizeof(fd_t);
            if (message_count > I386_SCM_MAX_FDS - *count)
                return _EINVAL;
            *count += (unsigned) message_count;
        }
        size_t aligned_length =
                (header.len + sizeof(dword_t) - 1) &
                ~(sizeof(dword_t) - 1);
        if (aligned_length > remaining)
            break;
        offset += aligned_length;
    }
    return 0;
}

static int i386_scm_retain_rights(const uint8_t *control,
        size_t control_length, struct scm *scm) {
    for (size_t offset = 0;
            control_length - offset >= sizeof(struct cmsghdr_);) {
        struct cmsghdr_ header;
        memcpy(&header, control + offset, sizeof(header));
        if (header.level == SOL_SOCKET_ && header.type == SCM_RIGHTS_) {
            size_t message_count =
                    (header.len - sizeof(header)) / sizeof(fd_t);
            const uint8_t *wire_fds = control + offset + sizeof(header);
            for (size_t index = 0; index < message_count; index++) {
                fd_t number;
                memcpy(&number, wire_fds + index * sizeof(number),
                        sizeof(number));
                STRACE(" sending fd %d", number);
                struct fd *transferred =
                        f_get_task_retain(current, number);
                if (transferred == NULL)
                    return _EBADF;
                scm->fds[scm->num_fds++] = transferred;
            }
        }
        size_t remaining = control_length - offset;
        size_t aligned_length =
                (header.len + sizeof(dword_t) - 1) &
                ~(sizeof(dword_t) - 1);
        if (aligned_length > remaining)
            break;
        offset += aligned_length;
    }
    return 0;
}

static int unix_scm_receiver_retain(struct fd *sender,
        const struct socket_address *address,
        struct unix_scm_receiver *receiver) {
    *receiver = (struct unix_scm_receiver) {0};
    bool explicit_destination =
            address != NULL && address->length != 0;
    if (sender->socket.type == SOCK_STREAM_ ||
            !explicit_destination) {
        bool pending = false;
        lock(&peer_lock);
        if (sender->socket.unix_peer != NULL)
            receiver->owner =
                    fd_try_retain(sender->socket.unix_peer);
        if (receiver->owner == NULL &&
                sender->socket.type == SOCK_STREAM_ &&
                sender->socket.unix_peer_handshake_pending &&
                !sender->socket.unix_peer_handshake_rejected)
            pending = true;
        unlock(&peer_lock);
        if (pending) {
            lock(&unix_bound_lock);
            struct unix_pending_peer *state =
                    sender->socket.unix_pending_connect;
            if (state != NULL && !state->handshake_finished &&
                    state->listener != NULL)
                receiver->logical =
                        fd_try_retain(state->listener);
            if (receiver->logical != NULL) {
                receiver->owner = fd_retain(sender);
                receiver->pending = true;
            }
            unlock(&unix_bound_lock);
        }
        if (receiver->owner != NULL)
            return 0;
        if (sender->socket.type == SOCK_STREAM_)
            return _EPIPE;
    }

    {
        struct sockaddr_storage peer = {0};
        socklen_t peer_length = 0;
        if (explicit_destination) {
            peer = address->storage;
            peer_length = address->length;
        } else {
            peer_length = sizeof(peer);
            if (getpeername(sender->real_fd,
                    (struct sockaddr *) &peer, &peer_length) < 0)
                return _ENOTCONN;
        }
        if (peer_length <= offsetof(struct sockaddr_un, sun_path) ||
                ((struct sockaddr *) &peer)->sa_family != AF_UNIX)
            return _EINVAL;
        const struct sockaddr_un *unix_peer =
                (const struct sockaddr_un *) &peer;
        lock(&unix_bound_lock);
        if (!list_null(&unix_bound_sockets)) {
            struct unix_bound_name *name;
            list_for_each_entry(&unix_bound_sockets, name, links) {
                if (name->owner_open && !name->closing &&
                        name->owner != NULL &&
                        strcmp(name->backing_path,
                                unix_peer->sun_path) == 0) {
                    receiver->owner = fd_try_retain(name->owner);
                    break;
                }
            }
        }
        unlock(&unix_bound_lock);
        return receiver->owner != NULL ? 0 : _ECONNREFUSED;
    }
}

static void unix_scm_receiver_release(
        struct unix_scm_receiver *receiver) {
    if (receiver->queued != NULL)
        fd_close(receiver->queued);
    if (receiver->logical != NULL)
        fd_close(receiver->logical);
    if (receiver->owner != NULL)
        fd_close(receiver->owner);
    *receiver = (struct unix_scm_receiver) {0};
}

// 调用方持有 unix_scm_io_lock 和 receiver 强引用，host dummy 与队列同序发布。
static bool unix_scm_receiver_queue(
        struct unix_scm_receiver *receiver, struct scm *scm) {
    assert(receiver->owner != NULL);
    if (receiver->pending) {
        bool queued = false;
        lock(&peer_lock);
        struct fd *accepted =
                receiver->owner->socket.unix_peer;
        if (accepted != NULL)
            receiver->queued = fd_try_retain(accepted);
        if (receiver->queued != NULL) {
            list_add_tail(&receiver->queued->socket.unix_scm,
                    &scm->queue);
            socket_scm_publish_locked(scm, receiver->queued);
            queued = true;
        } else if (receiver->owner->socket.unix_peer_handshake_pending &&
                !receiver->owner->socket.unix_peer_handshake_rejected) {
            assert(receiver->logical != NULL);
            list_add_tail(&receiver->owner->socket.unix_pending_scm,
                    &scm->queue);
            socket_scm_publish_locked(scm, receiver->logical);
            queued = true;
        }
        unlock(&peer_lock);
        return queued;
    }
    list_add_tail(&receiver->owner->socket.unix_scm, &scm->queue);
    socket_scm_publish_locked(scm, receiver->owner);
    return true;
}

static bool close_host_scm_rights(struct msghdr *message) {
    bool found = false;
    struct cmsghdr *header;
    for (header = CMSG_FIRSTHDR(message); header != NULL;
            header = CMSG_NXTHDR(message, header)) {
        if (header->cmsg_level != SOL_SOCKET ||
                header->cmsg_type != SCM_RIGHTS ||
                header->cmsg_len < CMSG_LEN(0))
            continue;
        size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const uint8_t *wire_fds = CMSG_DATA(header);
        for (size_t index = 0; index < count; index++) {
            int received;
            memcpy(&received, wire_fds + index * sizeof(received),
                    sizeof(received));
            close(received);
            found = true;
        }
    }
    return found;
}

static dword_t socket_recvmsg_output_flags(int host_flags) {
    // Linux 只回传协议产生的状态位；Apple 会把 MSG_PEEK 等输入位带回来。
    return (dword_t) sock_flags_from_real(host_flags) &
            (MSG_OOB_ | MSG_CTRUNC_ | MSG_TRUNC_ |
            MSG_EOR_ | MSG_ERRQUEUE_);
}

static bool socket_message_may_wait(struct fd *socket, dword_t flags) {
    return !(flags & MSG_DONTWAIT_) &&
            !(fd_getflags(socket) & O_NONBLOCK_);
}

static bool socket_message_would_block(int host_error) {
    return host_error == EAGAIN
#if EWOULDBLOCK != EAGAIN
            || host_error == EWOULDBLOCK
#endif
            ;
}

static bool socket_message_apple_local_enobufs(
        struct fd *socket, int host_error) {
#ifdef __APPLE__
    // Darwin 用 ENOBUFS 表示本地 socket 发送队列暂时无空间。
    return socket->socket.domain == AF_LOCAL_ && host_error == ENOBUFS;
#else
    (void) socket;
    (void) host_error;
    return false;
#endif
}

static bool socket_message_apple_stream_scm_backpressure(
        struct fd *socket, int host_error) {
#ifdef __APPLE__
    // Darwin 的本地 stream 在满队列附带 SCM_RIGHTS 时会返回 EMSGSIZE。
    return socket->socket.domain == AF_LOCAL_ &&
            socket->socket.type == SOCK_STREAM_ &&
            host_error == EMSGSIZE;
#else
    (void) socket;
    (void) host_error;
    return false;
#endif
}

static bool socket_message_send_would_block(
        struct fd *socket, int host_error) {
    return socket_message_would_block(host_error) ||
            socket_message_apple_local_enobufs(socket, host_error) ||
            socket_message_apple_stream_scm_backpressure(
                    socket, host_error);
}

static const struct timer_time *socket_message_deadline(
        struct fd *socket, bool receive,
        struct timer_time *deadline) {
    struct timeval timeout = {0};
    socklen_t length = sizeof(timeout);
    int option = receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    if (getsockopt(socket->real_fd, SOL_SOCKET,
            option, &timeout, &length) < 0 ||
            (timeout.tv_sec == 0 && timeout.tv_usec == 0))
        return NULL;
    struct timer_time duration = {
        .sec = timeout.tv_sec,
        .nsec = (int64_t) timeout.tv_usec * INT64_C(1000),
    };
    *deadline = timer_time_add(
            timer_time_from_timespec(timespec_now(CLOCK_MONOTONIC)),
            duration);
    return deadline;
}

static int socket_message_wait_ready(
        void *context, int types, union poll_fd_info info) {
    (void) info;
    int requested = *(const int *) context;
    return (types & (requested | POLL_ERR | POLL_HUP)) != 0;
}

static int socket_message_wait(struct fd *socket, int events,
        const struct timer_time *deadline) {
    struct poll *wait = poll_create();
    if (IS_ERR(wait))
        return PTR_ERR(wait);
    int result = poll_add_fd(wait, socket, events,
            (union poll_fd_info) {.ptr = socket});
    if (result == 0) {
        result = deadline != NULL ?
                poll_wait_until_signal_safe(wait,
                        socket_message_wait_ready, &events, deadline) :
                poll_wait_signal_safe(wait,
                        socket_message_wait_ready, &events, NULL);
        if (result > 0)
            result = 0;
        else if (result == 0)
            result = _EAGAIN;
    }
    poll_destroy(wait);
    return result;
}

static int socket_message_send_error(int error, dword_t flags) {
    if (error == _EPIPE && !(flags & MSG_NOSIGNAL_))
        send_signal(current, SIGPIPE_, SIGINFO_NIL);
    return error;
}

static int socket_message_send_host_error(
        struct fd *socket, int host_error, dword_t flags) {
    int error = socket_message_apple_local_enobufs(socket, host_error) ?
            _EAGAIN : err_map(host_error);
    return socket_message_send_error(error, flags);
}

static int socket_message_scm_send_host_error(
        struct fd *socket, int host_error, dword_t flags) {
    if (socket_message_apple_stream_scm_backpressure(
            socket, host_error))
        return _EAGAIN;
    return socket_message_send_host_error(socket, host_error, flags);
}

ssize_t socket_sendmsg_ref(const struct socket_ref *socket,
        const void *buffer, size_t length, dword_t flags,
        const struct socket_address *address, struct scm **scm_pointer) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops);
    struct scm *scm = scm_pointer != NULL ? *scm_pointer : NULL;
    int error = socket_sendto_validate(socket, length, flags);
    if (error < 0)
        return error;
    size_t prefix_size = socket_sendto_prefix_size(socket);
    if (prefix_size != 0) {
        error = socket_sendto_prefix_validate(
                socket, buffer, prefix_size);
        if (error < 0)
            return error;
    }
    error = socket_sendto_transaction_validate(socket, length);
    if (error < 0)
        return error;
    error = socket_sendto_destination_check(socket, address, flags);
    if (error < 0)
        return error;
    if (scm != NULL && socket->fd->socket.domain != AF_LOCAL_)
        return _EINVAL;

    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    bool serialize_unix_datagram =
            socket->fd->socket.domain == AF_LOCAL_ &&
            socket->fd->socket.type != SOCK_STREAM_;
    bool may_wait = socket_message_may_wait(socket->fd, flags);
    struct timer_time wait_deadline;
    const struct timer_time *wait_deadline_pointer =
            (scm != NULL || serialize_unix_datagram) && may_wait ?
            socket_message_deadline(
                    socket->fd, false, &wait_deadline) : NULL;
    byte_t empty = 0;
    struct iovec vector = {
        .iov_base = length != 0 ? (void *) buffer : &empty,
        .iov_len = length,
    };
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
    };
    bool has_address = address != NULL && address->length != 0;
    if (has_address) {
        message.msg_name = (void *) &address->storage;
        message.msg_namelen = address->length;
    }

    int dummy_fd = -1;
    if (scm != NULL && !(socket->fd->socket.type == SOCK_STREAM_ &&
            length == 0)) {
        dummy_fd = unix_scm_dummy_get();
        if (dummy_fd < 0)
            return dummy_fd;
    }
    bool scm_locked = false;
    struct scm *rejected_scm = NULL;
    struct unix_scm_receiver receiver = {0};
    char host_control[CMSG_SPACE(sizeof(int))] = {0};
    if (scm != NULL || serialize_unix_datagram) {
        lock(&unix_scm_io_lock);
        scm_locked = true;
        error = unix_scm_receiver_retain(
                socket->fd, address, &receiver);
        if (error < 0) {
            error = socket_message_send_error(error, flags);
            goto out;
        }
    }
    if (scm != NULL) {
        if (socket->fd->socket.type == SOCK_STREAM_ && length == 0) {
            ssize_t checked = sendmsg(
                    socket->fd->real_fd, &message, real_flags);
            if (checked < 0) {
                error = socket_message_scm_send_host_error(
                        socket->fd, errno, flags);
                goto out;
            }
            rejected_scm = scm;
            *scm_pointer = NULL;
            error = (int) checked;
            goto out;
        }
        assert(dummy_fd >= 0);
        message.msg_control = host_control;
        message.msg_controllen = sizeof(host_control);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(dummy_fd));
        memcpy(CMSG_DATA(header), &dummy_fd, sizeof(dummy_fd));
    }

    for (;;) {
        bool receiver_discards = serialize_unix_datagram &&
                receiver.owner->socket.unix_read_shutdown;
        if (receiver_discards) {
            error = socket_message_send_error(_EPIPE, flags);
            break;
        }
        if (scm != NULL) {
            error = socket_scm_send_check_locked(scm);
            if (error < 0)
                break;
        }
        struct unix_bound_name *queued_name =
                unix_bound_message_begin(socket->fd);
        int attempt_flags = scm_locked ?
                real_flags | MSG_DONTWAIT : real_flags;
        ssize_t sent = sendmsg(
                socket->fd->real_fd, &message, attempt_flags);
        int host_error = errno;
        if (sent < 0)
            unix_bound_message_cancel(queued_name);
        if (sent >= 0 && scm != NULL) {
            if (!unix_scm_receiver_queue(&receiver, scm))
                rejected_scm = scm;
            *scm_pointer = NULL;
        }
        if (sent >= 0) {
            error = (int) sent;
            break;
        }
        if (scm_locked && may_wait &&
                socket_message_send_would_block(
                        socket->fd, host_error)) {
            unlock(&unix_scm_io_lock);
            scm_locked = false;
            error = socket_message_wait(socket->fd, POLL_WRITE,
                    wait_deadline_pointer);
            if (error < 0)
                goto out;
            lock(&unix_scm_io_lock);
            scm_locked = true;
            continue;
        }
        if (host_error == ENOENT && address != NULL &&
                (address->lookup_inode != NULL ||
                address->lookup_abstract != NULL))
            error = _ECONNREFUSED;
        else
            error = scm != NULL ?
                    socket_message_scm_send_host_error(
                            socket->fd, host_error, flags) :
                    socket_message_send_host_error(
                            socket->fd, host_error, flags);
        break;
    }

out:
    if (scm_locked)
        unlock(&unix_scm_io_lock);
    if (rejected_scm != NULL)
        socket_scm_release(rejected_scm);
    unix_scm_receiver_release(&receiver);
    return error;
}

ssize_t socket_recvmsg_ref(const struct socket_ref *socket,
        void *buffer, size_t length, dword_t flags,
        struct socket_address *address, dword_t *message_flags,
        struct scm **scm_pointer) {
    assert(socket != NULL && socket->fd != NULL &&
            socket->fd->ops == &socket_fdops &&
            message_flags != NULL && scm_pointer != NULL);
    *message_flags = 0;
    *scm_pointer = NULL;
    int real_flags = sock_flags_to_real(flags);
    if (real_flags < 0)
        return _EINVAL;
    bool may_wait = socket_message_may_wait(socket->fd, flags);
    if ((socket->fd->socket.domain == AF_INET_ ||
            socket->fd->socket.domain == AF_INET6_) &&
            socket->fd->socket.type == SOCK_STREAM_ &&
            (flags & MSG_TRUNC_))
        real_flags &= ~MSG_TRUNC;

    bool unix_socket = socket->fd->socket.domain == AF_LOCAL_;
    struct timer_time wait_deadline;
    const struct timer_time *wait_deadline_pointer =
            unix_socket && may_wait ?
            socket_message_deadline(
                    socket->fd, true, &wait_deadline) : NULL;
    bool unix_recv_locked = false;
    if (unix_socket) {
        lock(&socket->fd->socket.unix_recv_lock);
        unix_recv_locked = true;
    }

    bool track_unix_source = unix_socket &&
            socket->fd->socket.type != SOCK_STREAM_;
    struct socket_address discarded_address;
    struct socket_address *received_address = address != NULL ?
            address : (track_unix_source ? &discarded_address : NULL);
    if (received_address != NULL) {
        *received_address = (struct socket_address) {0};
        received_address->length = sizeof(received_address->storage);
    }

    byte_t empty = 0;
    struct iovec vector = {
        .iov_base = length != 0 ? buffer : &empty,
        .iov_len = length,
    };
    struct msghdr message = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
    };
    if (received_address != NULL) {
        message.msg_name = &received_address->storage;
        message.msg_namelen = sizeof(received_address->storage);
    }
    // AF_UNIX 必须接收并关闭 host dummy，才能与内部 SCM 队列保持一一对应。
    char host_control[CMSG_SPACE(sizeof(int))] = {0};
    if (unix_socket) {
        message.msg_control = host_control;
        message.msg_controllen = sizeof(host_control);
    }

    ssize_t result;
    ssize_t received;
retry_receive:
    if (received_address != NULL)
        message.msg_namelen = sizeof(received_address->storage);
    if (unix_socket) {
        memset(host_control, 0, sizeof(host_control));
        message.msg_controllen = sizeof(host_control);
        message.msg_flags = 0;
    }
    received = recvmsg(socket->fd->real_fd, &message,
            unix_socket ? real_flags | MSG_DONTWAIT : real_flags);
    if (received < 0) {
        int host_error = errno;
        if (unix_socket && may_wait &&
                socket_message_would_block(host_error)) {
            unlock(&socket->fd->socket.unix_recv_lock);
            unix_recv_locked = false;
            int wait_error =
                    socket_message_wait(socket->fd, POLL_READ,
                            wait_deadline_pointer);
            if (wait_error < 0) {
                result = wait_error;
                goto out;
            }
            lock(&socket->fd->socket.unix_recv_lock);
            unix_recv_locked = true;
            goto retry_receive;
        }
        result = err_map(host_error);
        goto out;
    }

    // host fd 必须先关闭；即使后续地址转换失败，也不能破坏 dummy 与队列的对应。
    bool host_rights = unix_socket && close_host_scm_rights(&message);
    if (host_rights) {
        bool found = false;
        lock(&unix_scm_io_lock);
        if (!list_empty(&socket->fd->socket.unix_scm)) {
            struct scm *queued = list_first_entry(
                    &socket->fd->socket.unix_scm, struct scm, queue);
            found = true;
            if (flags & MSG_PEEK_)
                *scm_pointer = socket_scm_clone(queued);
            else {
                list_remove(&queued->queue);
                socket_scm_unpublish_locked(queued);
                *scm_pointer = queued;
            }
        }
        unlock(&unix_scm_io_lock);
        if (!found) {
            result = _EIO;
            goto out;
        }
        if (*scm_pointer == NULL) {
            result = _ENOMEM;
            goto out;
        }
    }

    bool copied_unix_stream_name = false;
    if (unix_socket && socket->fd->socket.type == SOCK_STREAM_ &&
            received > 0 && address != NULL &&
            message.msg_namelen == 0) {
        lock(&peer_lock);
        if (socket->fd->socket.unix_peer_name_valid &&
                socket->fd->socket.unix_peer_name_len != 0) {
            struct sockaddr_ *guest =
                    (struct sockaddr_ *) &received_address->storage;
            guest->family = AF_LOCAL_;
            memcpy(guest->data,
                    socket->fd->socket.unix_peer_name,
                    socket->fd->socket.unix_peer_name_len);
            received_address->length =
                    offsetof(struct sockaddr_, data) +
                    socket->fd->socket.unix_peer_name_len;
            copied_unix_stream_name = true;
        }
        unlock(&peer_lock);
    }
    if (!copied_unix_stream_name &&
            received_address != NULL && received_address->length != 0) {
        uint_t address_length = (uint_t) message.msg_namelen;
        int error = sockaddr_from_real(&received_address->storage,
                sizeof(received_address->storage), &address_length,
                track_unix_source && !(flags & MSG_PEEK_));
        if (error < 0) {
            result = error;
            goto out;
        }
        received_address->length = (socklen_t) address_length;
    }
    if (unix_recv_locked) {
        unlock(&socket->fd->socket.unix_recv_lock);
        unix_recv_locked = false;
    }
    *message_flags = socket_recvmsg_output_flags(message.msg_flags);
    result = received;

out:
    if (unix_recv_locked)
        unlock(&socket->fd->socket.unix_recv_lock);
    if (result < 0 && *scm_pointer != NULL) {
        socket_scm_release(*scm_pointer);
        *scm_pointer = NULL;
    }
    return result;
}

struct scm_delivery {
    unsigned count;
    fd_t numbers[I386_SCM_MAX_FDS];
    qword_t generations[I386_SCM_MAX_FDS];
    struct fd *objects[I386_SCM_MAX_FDS];
};

static int scm_delivery_install(struct task *task, struct scm *scm,
        unsigned count, int flags, struct scm_delivery *delivery) {
    for (unsigned index = 0; index < count; index++) {
        struct fd *object = scm->fds[index];
        struct fd *rollback_reference = fd_retain(object);
        scm->fds[index] = NULL;
        qword_t generation = 0;
        fd_t number = f_install_task_tracked(
                task, object, flags, &generation);
        if (number < 0) {
            fd_close(rollback_reference);
            return number;
        }
        delivery->numbers[delivery->count] = number;
        delivery->generations[delivery->count] = generation;
        delivery->objects[delivery->count] = rollback_reference;
        delivery->count++;
    }
    return 0;
}

static void scm_delivery_finish(
        struct task *task, struct scm_delivery *delivery, bool rollback) {
    for (unsigned index = 0; index < delivery->count; index++) {
        if (rollback)
            f_close_task_if_matches(task, delivery->numbers[index],
                    delivery->objects[index], delivery->generations[index]);
        fd_close(delivery->objects[index]);
    }
    delivery->count = 0;
}

int_t sys_sendmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    STRACE("sendmsg(%d, %#x, %d)", sock_fd, msghdr_addr, flags);
    dword_t guest_flags = (dword_t) flags;
    if (guest_flags & I386_LINUX_MSG_CMSG_COMPAT)
        return _EINVAL;
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;

    struct msghdr msg = {0};
    struct msghdr_ msg_fake;
    struct i386_message_iov iov = {0};
    uint8_t *guest_control = NULL;
    struct scm *scm = NULL;
    struct scm *rejected_scm = NULL;
    bool scm_io_locked = false;
    struct unix_scm_receiver scm_receiver = {0};
    if (user_get(msghdr_addr, msg_fake)) {
        result = _EFAULT;
        goto out;
    }

    // msg_name
    struct sockaddr_max_ msg_name;
    if (msg_fake.msg_name != 0) {
        result = sockaddr_read(msg_fake.msg_name, socket.fd->socket.type,
                &msg_name, &msg_fake.msg_namelen);
        if (result < 0)
            goto out;
        msg.msg_name = &msg_name;
        msg.msg_namelen = msg_fake.msg_namelen;
    }

    result = i386_message_iov_prepare(&socket, msg_fake.msg_iov,
            msg_fake.msg_iovlen, (dword_t) flags, true, &iov);
    if (result < 0)
        goto out;
    msg.msg_iov = iov.host;
    msg.msg_iovlen = iov.count;

    // msg_control
    result = i386_message_control_read(&msg_fake, &guest_control);
    if (result < 0)
        goto out;
    unsigned rights_count = 0;
    if (socket.fd->socket.domain == AF_LOCAL_ && guest_control != NULL) {
        result = i386_scm_rights_count(guest_control,
                msg_fake.msg_controllen, &rights_count);
        if (result < 0)
            goto out;
        if (rights_count != 0) {
            scm = socket_scm_allocate_task(current, rights_count);
            if (scm == NULL) {
                result = _ENOMEM;
                goto out;
            }
            result = i386_scm_retain_rights(guest_control,
                    msg_fake.msg_controllen, scm);
            if (result < 0)
                goto out;
        }
    }

    int real_flags = sock_flags_to_real(
            guest_flags & ~I386_LINUX_MSG_CMSG_CLOEXEC);
    if (real_flags < 0) {
        result = _EINVAL;
        goto out;
    }
    bool serialize_unix_datagram =
            socket.fd->socket.domain == AF_LOCAL_ &&
            socket.fd->socket.type != SOCK_STREAM_;
    bool may_wait = socket_message_may_wait(socket.fd, guest_flags);
    struct timer_time wait_deadline;
    const struct timer_time *wait_deadline_pointer =
            (scm != NULL || serialize_unix_datagram) && may_wait ?
            socket_message_deadline(
                    socket.fd, false, &wait_deadline) : NULL;

    // 只传一个 host dummy fd；真实 guest 引用由内部 SCM 容器携带。
    char real_msg_control[CMSG_SPACE(sizeof(int))] = {0};
    int dummy_fd = -1;
    if (scm != NULL && !(socket.fd->socket.type == SOCK_STREAM_ &&
            iov.transaction_size == 0)) {
        dummy_fd = unix_scm_dummy_get();
        if (dummy_fd < 0) {
            result = dummy_fd;
            goto out;
        }
    }
    if (scm != NULL || serialize_unix_datagram) {
        lock(&unix_scm_io_lock);
        scm_io_locked = true;
        struct socket_address destination = {0};
        const struct socket_address *destination_pointer = NULL;
        if (msg.msg_name != NULL && msg.msg_namelen != 0) {
            memcpy(&destination.storage,
                    msg.msg_name, msg.msg_namelen);
            destination.length = msg.msg_namelen;
            destination_pointer = &destination;
        }
        result = unix_scm_receiver_retain(socket.fd,
                destination_pointer, &scm_receiver);
        if (result < 0) {
            result = socket_message_send_error(
                    result, guest_flags);
            goto out;
        }
    }
    if (scm != NULL) {
        if (socket.fd->socket.type == SOCK_STREAM_ &&
                iov.transaction_size == 0) {
            ssize_t checked = sendmsg(
                    socket.fd->real_fd, &msg, real_flags);
            if (checked < 0) {
                result = socket_message_scm_send_host_error(
                        socket.fd, errno, guest_flags);
                goto out;
            }
            rejected_scm = scm;
            scm = NULL;
            result = (int_t) checked;
            goto out;
        }
        assert(dummy_fd >= 0);
        msg.msg_control = real_msg_control;
        msg.msg_controllen = sizeof(real_msg_control);
        struct cmsghdr *real_cmsg = CMSG_FIRSTHDR(&msg);
        real_cmsg->cmsg_level = SOL_SOCKET;
        real_cmsg->cmsg_type = SCM_RIGHTS;
        real_cmsg->cmsg_len = CMSG_LEN(sizeof(dummy_fd));
        memcpy(CMSG_DATA(real_cmsg), &dummy_fd, sizeof(dummy_fd));
    }

    for (;;) {
        bool receiver_discards = serialize_unix_datagram &&
                scm_receiver.owner->socket.unix_read_shutdown;
        if (receiver_discards) {
            result = socket_message_send_error(
                    _EPIPE, guest_flags);
            break;
        }
        if (scm != NULL) {
            result = socket_scm_send_check_locked(scm);
            if (result < 0)
                break;
        }
        struct unix_bound_name *queued_name =
                unix_bound_message_begin(socket.fd);
        ssize_t sent = sendmsg(socket.fd->real_fd, &msg,
                scm_io_locked ?
                        real_flags | MSG_DONTWAIT : real_flags);
        int host_error = errno;
        if (sent < 0)
            unix_bound_message_cancel(queued_name);
        if (sent >= 0) {
            result = (int_t) sent;
            if (scm != NULL) {
                if (!unix_scm_receiver_queue(&scm_receiver, scm))
                    rejected_scm = scm;
                scm = NULL;
            }
            break;
        }
        if (scm_io_locked && may_wait &&
                socket_message_send_would_block(
                        socket.fd, host_error)) {
            unlock(&unix_scm_io_lock);
            scm_io_locked = false;
            result = socket_message_wait(socket.fd, POLL_WRITE,
                    wait_deadline_pointer);
            if (result < 0)
                goto out;
            lock(&unix_scm_io_lock);
            scm_io_locked = true;
            continue;
        }
        result = scm != NULL ?
                socket_message_scm_send_host_error(
                        socket.fd, host_error, guest_flags) :
                socket_message_send_host_error(
                        socket.fd, host_error, guest_flags);
        break;
    }

out:
    if (scm_io_locked)
        unlock(&unix_scm_io_lock);
    if (rejected_scm != NULL)
        socket_scm_release(rejected_scm);
    unix_scm_receiver_release(&scm_receiver);
    if (scm != NULL)
        socket_scm_release(scm);
    free(guest_control);
    i386_message_iov_destroy(&iov);
    socket_ref_release(&socket);
    return result;
}

int_t sys_recvmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags) {
    STRACE("recvmsg(%d, %#x, %d)", sock_fd, msghdr_addr, flags);
    dword_t guest_flags = (dword_t) flags;
    if (guest_flags & I386_LINUX_MSG_CMSG_COMPAT)
        return _EINVAL;
    struct socket_ref socket;
    int_t result = socket_ref_get_task(current, sock_fd, &socket);
    if (result < 0)
        return result;

    struct msghdr msg = {0};
    struct msghdr_ msg_fake;
    struct i386_message_iov iov = {0};
    struct scm_delivery delivery = {0};
    struct scm *received_scm = NULL;
    uint8_t *guest_control_wire = NULL;
    bool unix_recv_locked = false;
    if (user_get(msghdr_addr, msg_fake)) {
        result = _EFAULT;
        goto out;
    }

    // msg_name
    struct sockaddr_storage msg_name = {0};
    dword_t guest_name_capacity = msg_fake.msg_namelen;
    bool wants_name = msg_fake.msg_name != 0;
    bool track_unix_source = socket.fd->socket.domain == AF_LOCAL_ &&
            socket.fd->socket.type != SOCK_STREAM_;
    if (wants_name || track_unix_source) {
        msg.msg_name = &msg_name;
        msg.msg_namelen = sizeof(msg_name);
    }

    // AF_UNIX 始终接收 host dummy，guest 未提供空间时也必须同步丢弃内部 SCM。
    char real_msg_control[CMSG_SPACE(sizeof(int))] = {0};
    if (socket.fd->socket.domain == AF_LOCAL_ ||
            msg_fake.msg_controllen != 0) {
        msg.msg_control = real_msg_control;
        msg.msg_controllen = sizeof(real_msg_control);
    }

    int real_flags = sock_flags_to_real(
            guest_flags & ~I386_LINUX_MSG_CMSG_CLOEXEC);
    if (real_flags < 0) {
        result = _EINVAL;
        goto out;
    }

    bool may_wait = socket_message_may_wait(socket.fd, guest_flags);
    struct timer_time wait_deadline;
    const struct timer_time *wait_deadline_pointer =
            socket.fd->socket.domain == AF_LOCAL_ && may_wait ?
            socket_message_deadline(
                    socket.fd, true, &wait_deadline) : NULL;
    result = i386_message_iov_prepare(&socket, msg_fake.msg_iov,
            msg_fake.msg_iovlen, (dword_t) flags, false, &iov);
    if (result < 0)
        goto out;
    msg.msg_iov = iov.host;
    msg.msg_iovlen = iov.count;

    if (socket.fd->socket.domain == AF_LOCAL_) {
        lock(&socket.fd->socket.unix_recv_lock);
        unix_recv_locked = true;
    }
    ssize_t received;
retry_i386_receive:
    if (msg.msg_name != NULL)
        msg.msg_namelen = sizeof(msg_name);
    if (socket.fd->socket.domain == AF_LOCAL_) {
        memset(real_msg_control, 0, sizeof(real_msg_control));
        msg.msg_controllen = sizeof(real_msg_control);
        msg.msg_flags = 0;
    }
    received = recvmsg(socket.fd->real_fd, &msg,
            socket.fd->socket.domain == AF_LOCAL_ ?
                    real_flags | MSG_DONTWAIT : real_flags);
    if (received < 0) {
        int host_error = errno;
        if (socket.fd->socket.domain == AF_LOCAL_ && may_wait &&
                socket_message_would_block(host_error)) {
            unlock(&socket.fd->socket.unix_recv_lock);
            unix_recv_locked = false;
            result = socket_message_wait(socket.fd, POLL_READ,
                    wait_deadline_pointer);
            if (result < 0)
                goto out;
            lock(&socket.fd->socket.unix_recv_lock);
            unix_recv_locked = true;
            goto retry_i386_receive;
        }
        result = err_map(host_error);
        goto out;
    }
    result = (int_t) received;

    int post_receive_error = 0;
    bool copied_unix_stream_name = false;
    if (socket.fd->socket.domain == AF_LOCAL_ &&
            socket.fd->socket.type == SOCK_STREAM_ &&
            received > 0 && wants_name && msg.msg_namelen == 0) {
        lock(&peer_lock);
        if (socket.fd->socket.unix_peer_name_valid &&
                socket.fd->socket.unix_peer_name_len != 0) {
            struct sockaddr_ *guest =
                    (struct sockaddr_ *) &msg_name;
            guest->family = AF_LOCAL_;
            memcpy(guest->data,
                    socket.fd->socket.unix_peer_name,
                    socket.fd->socket.unix_peer_name_len);
            msg.msg_namelen = (socklen_t) (
                    offsetof(struct sockaddr_, data) +
                    socket.fd->socket.unix_peer_name_len);
            copied_unix_stream_name = true;
        }
        unlock(&peer_lock);
    }
    if (msg.msg_name != NULL && !copied_unix_stream_name) {
        uint_t returned_length = (uint_t) msg.msg_namelen;
        post_receive_error = sockaddr_from_real(msg.msg_name,
                sizeof(msg_name), &returned_length,
                track_unix_source && !(flags & MSG_PEEK_));
        if (post_receive_error == 0)
            msg.msg_namelen = (socklen_t) returned_length;
    }

    // host 消息一旦消费，就先完成 dummy 与内部队列的配对，再释放接收顺序锁。
    bool host_rights = socket.fd->socket.domain == AF_LOCAL_ &&
            close_host_scm_rights(&msg);
    if (host_rights) {
        bool found = false;
        lock(&unix_scm_io_lock);
        if (!list_empty(&socket.fd->socket.unix_scm)) {
            struct scm *queued = list_first_entry(
                    &socket.fd->socket.unix_scm, struct scm, queue);
            found = true;
            if (flags & MSG_PEEK_)
                received_scm = socket_scm_clone(queued);
            else {
                list_remove(&queued->queue);
                socket_scm_unpublish_locked(queued);
                received_scm = queued;
            }
        }
        unlock(&unix_scm_io_lock);
        if (!found && post_receive_error == 0)
            post_receive_error = _EIO;
        else if (found && received_scm == NULL &&
                post_receive_error == 0)
            post_receive_error = _ENOMEM;
    }
    if (unix_recv_locked) {
        unlock(&socket.fd->socket.unix_recv_lock);
        unix_recv_locked = false;
    }

    // msg_iovec (changed)
    // copy as many bytes as were received, according to the return value
    size_t remaining = (size_t) received < iov.transaction_size ?
            (size_t) received : iov.transaction_size;
    for (size_t index = 0; index < iov.count; index++) {
        size_t chunk_size = iov.host[index].iov_len < remaining ?
                iov.host[index].iov_len : remaining;
        if (chunk_size > 0 && post_receive_error == 0 &&
                user_write(iov.guest[index].base,
                        iov.host[index].iov_base, chunk_size))
            post_receive_error = _EFAULT;
        remaining -= chunk_size;
    }

    // msg_control (changed)
    dword_t guest_control_capacity = msg_fake.msg_control != 0 ?
            msg_fake.msg_controllen : 0;
    msg_fake.msg_controllen = 0;
    bool guest_control_truncated = false;
    if (received_scm != NULL) {
        if (post_receive_error < 0) {
            socket_scm_release(received_scm);
            received_scm = NULL;
        } else {
            unsigned deliverable = 0;
            if (guest_control_capacity >=
                    sizeof(struct cmsghdr_) + sizeof(fd_t)) {
                deliverable = (unsigned) ((guest_control_capacity -
                        sizeof(struct cmsghdr_)) / sizeof(fd_t));
                if (deliverable > received_scm->num_fds)
                    deliverable = received_scm->num_fds;
            }
            guest_control_truncated =
                    deliverable < received_scm->num_fds;
            if (deliverable != 0) {
                size_t wire_length = sizeof(struct cmsghdr_) +
                        deliverable * sizeof(fd_t);
                guest_control_wire = calloc(1, wire_length);
                if (guest_control_wire == NULL) {
                    post_receive_error = _ENOMEM;
                } else {
                    struct cmsghdr_ *guest_header =
                            (struct cmsghdr_ *) guest_control_wire;
                    guest_header->len = (dword_t) wire_length;
                    guest_header->level = SOL_SOCKET_;
                    guest_header->type = SCM_RIGHTS_;
                    int install_flags = (guest_flags &
                            I386_LINUX_MSG_CMSG_CLOEXEC) ? O_CLOEXEC_ : 0;
                    post_receive_error = scm_delivery_install(current,
                            received_scm, deliverable,
                            install_flags, &delivery);
                    for (unsigned index = 0;
                            index < delivery.count; index++) {
                        STRACE(" receiving fd %d",
                                delivery.numbers[index]);
                        memcpy(guest_header->data +
                                index * sizeof(fd_t),
                                &delivery.numbers[index], sizeof(fd_t));
                    }
                    if (post_receive_error == 0 &&
                            user_write(msg_fake.msg_control,
                                    guest_control_wire, wire_length))
                        post_receive_error = _EFAULT;
                    if (post_receive_error == 0)
                        msg_fake.msg_controllen = (dword_t) wire_length;
                }
            }
            socket_scm_release(received_scm);
            received_scm = NULL;
        }
    }

    // msg_name (changed)
    if (post_receive_error == 0 && msg.msg_name != NULL) {
        uint_t returned_length = (uint_t) msg.msg_namelen;
        if (wants_name) {
            int name_error = sockaddr_copy_to_user(
                    msg_fake.msg_name, msg.msg_name,
                    sizeof(msg_name), guest_name_capacity,
                    returned_length);
            if (name_error < 0)
                post_receive_error = name_error;
        }
        msg.msg_namelen = (socklen_t) returned_length;
    }
    msg_fake.msg_namelen = wants_name ? msg.msg_namelen : 0;

    // msg_flags (changed)
    msg_fake.msg_flags = socket_recvmsg_output_flags(msg.msg_flags);
    msg_fake.msg_flags |= guest_flags &
            I386_LINUX_MSG_CMSG_CLOEXEC;
    if (guest_control_truncated)
        msg_fake.msg_flags |= MSG_CTRUNC_;

    if (post_receive_error == 0 && user_put(msghdr_addr, msg_fake))
        post_receive_error = _EFAULT;
    if (post_receive_error < 0) {
        result = post_receive_error;
        goto out;
    }

    scm_delivery_finish(current, &delivery, false);
out:
    if (unix_recv_locked)
        unlock(&socket.fd->socket.unix_recv_lock);
    if (delivery.count != 0)
        scm_delivery_finish(current, &delivery, true);
    if (received_scm != NULL)
        socket_scm_release(received_scm);
    free(guest_control_wire);
    i386_message_iov_destroy(&iov);
    socket_ref_release(&socket);
    return result;
}

struct mmsghdr_ {
    struct msghdr_ hdr;
    uint_t len;
};

int_t sys_sendmmsg(fd_t sock_fd, addr_t msg_vec, uint_t vec_len, int_t flags) {
    int num_sent = 0;
    for (unsigned i = 0; i < vec_len; i++) {
        addr_t msghdr = msg_vec + i * sizeof(struct mmsghdr_);
        int_t res = sys_sendmsg(sock_fd, msghdr, flags);
        if (res >= 0) {
            addr_t msg_len_addr = msghdr + offsetof(struct mmsghdr_, len);
            if (user_put(msg_len_addr, res))
                res = _EFAULT;
        }
        if (res < 0) {
            // From the man page:
            // If an error occurs after at least one message has been sent, the
            // call succeeds, and returns the number of messages sent.  The
            // error code is lost.
            if (num_sent > 0)
                break;
            return res;
        }
        num_sent++;
        if (res == 0) {
            // This means the socket is non-blocking and can't be written to anymore.
            break;
        }
    }
    return num_sent;
}

static void sock_translate_err(struct fd *fd, int *err) {
    // on ios, when the device goes to sleep, all connected sockets are killed.
    // reads/writes return ENOTCONN, which I'm pretty sure is a violation of
    // posix. so instead, detect this and return ECONNRESET.
    if (*err == _ENOTCONN) {
        struct sockaddr addr;
        socklen_t len = sizeof(addr);
        if (getpeername(fd->real_fd, &addr, &len) < 0 && errno == EINVAL) {
            *err = _ECONNRESET;
        }
    }
}

static ssize_t sock_read(struct fd *fd, void *buf, size_t size) {
    int err;
    if (fd->socket.domain == AF_LOCAL_) {
        struct socket_ref socket = {.fd = fd};
        err = (int) socket_recvfrom_ref(
                &socket, buf, size, 0, NULL);
    } else {
        err = realfs_read(fd, buf, size);
    }
    sock_translate_err(fd, &err);
    return err;
}

static ssize_t sock_write(struct fd *fd, const void *buf, size_t size) {
    int err;
    if (fd->socket.domain == AF_LOCAL_ &&
            fd->socket.type != SOCK_STREAM_) {
        const struct socket_ref socket = {.fd = fd};
        err = (int) socket_sendto_ref(
                &socket, buf, size, 0, NULL);
    } else {
        err = realfs_write(fd, buf, size);
    }
    sock_translate_err(fd, &err);
    return err;
}

// 调用方按 unix_recv_lock -> unix_scm_io_lock 持锁；host I/O 必须非阻塞。
static int unix_bound_drain_messages_locked(
        struct fd *fd, struct list *discarded_scm) {
    assert(fd->socket.domain == AF_LOCAL_ &&
            fd->socket.type != SOCK_STREAM_);
    for (;;) {
        byte_t discarded;
        struct iovec vector = {
            .iov_base = &discarded,
            .iov_len = sizeof(discarded),
        };
        struct sockaddr_storage source = {0};
        char control[CMSG_SPACE(sizeof(int))] = {0};
        struct msghdr message = {
            .msg_name = &source,
            .msg_namelen = sizeof(source),
            .msg_iov = &vector,
            .msg_iovlen = 1,
            .msg_control = control,
            .msg_controllen = sizeof(control),
        };
        ssize_t result = recvmsg(fd->real_fd, &message,
                MSG_DONTWAIT | MSG_TRUNC);
        if (result < 0) {
            int host_error = errno;
            if (host_error == EINTR)
                continue;
            return socket_message_would_block(host_error) ?
                    0 : err_map(host_error);
        }
        if (close_host_scm_rights(&message) &&
                !list_empty(&fd->socket.unix_scm)) {
            struct scm *scm = list_first_entry(
                    &fd->socket.unix_scm, struct scm, queue);
            list_remove(&scm->queue);
            socket_scm_unpublish_locked(scm);
            list_add_tail(discarded_scm, &scm->queue);
        }
        if (message.msg_namelen != 0) {
            uint_t converted_length = (uint_t) message.msg_namelen;
            (void) sockaddr_from_real(&source, sizeof(source),
                    &converted_length, true);
        }
    }
}

static void socket_scm_release_list(struct list *scm_list) {
    while (!list_empty(scm_list)) {
        struct scm *scm = list_first_entry(
                scm_list, struct scm, queue);
        list_remove(&scm->queue);
        socket_scm_release(scm);
    }
}

static int sock_close(struct fd *fd) {
    if (fd->socket.domain == AF_LOCAL_) {
        assert(atomic_load(&fd->socket.unix_scm_incoming) == 0);
        assert(list_empty(&fd->socket.unix_scm_vertex));
    }
    sockrestart_end_listen(fd);
    bool close_host_early = unix_bound_owner_begin_close(fd);
    int close_result = 0;
    if (close_host_early) {
        if (close(fd->real_fd) < 0)
            close_result = errno_map();
        fd->real_fd = -1;
    }
    struct list discarded_scm;
    list_init(&discarded_scm);
    bool unix_datagram = fd->socket.domain == AF_LOCAL_ &&
            fd->socket.type != SOCK_STREAM_;
    if (unix_datagram)
        lock(&fd->socket.unix_recv_lock);
    if (fd->socket.domain == AF_LOCAL_)
        lock(&unix_scm_io_lock);
    if (unix_datagram && !fd->socket.unix_read_shutdown)
        (void) unix_bound_drain_messages_locked(
                fd, &discarded_scm);
    if (fd->socket.domain == AF_LOCAL_) {
        while (!list_empty(&fd->socket.unix_scm)) {
            struct scm *scm = list_first_entry(
                    &fd->socket.unix_scm, struct scm, queue);
            list_remove(&scm->queue);
            socket_scm_unpublish_locked(scm);
            list_add_tail(&discarded_scm, &scm->queue);
        }
        while (!list_empty(&fd->socket.unix_pending_scm)) {
            struct scm *scm = list_first_entry(
                    &fd->socket.unix_pending_scm,
                    struct scm, queue);
            list_remove(&scm->queue);
            socket_scm_unpublish_locked(scm);
            list_add_tail(&discarded_scm, &scm->queue);
        }
        unlock(&unix_scm_io_lock);
    }
    if (unix_datagram)
        unlock(&fd->socket.unix_recv_lock);
    unix_bound_owner_finish_close(fd);
    inode_release_if_exist(fd->socket.unix_name_inode);
    if (fd->socket.unix_name_abstract != NULL)
        unix_abstract_unpublish(fd->socket.unix_name_abstract);
    if (fd->socket.unix_backing_owned) {
        struct stat host_stat;
        if (lstat(fd->socket.unix_backing_path, &host_stat) == 0 &&
                (uint64_t) host_stat.st_dev ==
                        fd->socket.unix_backing_device &&
                (uint64_t) host_stat.st_ino ==
                        fd->socket.unix_backing_inode)
            (void) unlink(fd->socket.unix_backing_path);
    }
    lock(&peer_lock);
    struct fd *peer = fd->socket.unix_peer;
    if (peer != NULL)
        peer->socket.unix_peer = NULL;
    unlock(&peer_lock);
    socket_scm_release_list(&discarded_scm);
    if (fd->real_fd >= 0)
        return realfs_close(fd);
    return close_result;
}

const struct fd_ops socket_fdops = {
    .read = sock_read,
    .write = sock_write,
    .close = sock_close,
    .poll = realfs_poll,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
    .ioctl_size = realfs_ioctl_size,
    .ioctl = realfs_ioctl,
};

#if is_gcc(8) || is_clang(21)
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static struct socket_call {
    syscall_t func;
    int args;
} socket_calls[] = {
    {NULL},
    {(syscall_t) sys_socket, 3},
    {(syscall_t) sys_bind, 3},
    {(syscall_t) sys_connect, 3},
    {(syscall_t) sys_listen, 2},
    {(syscall_t) sys_accept, 3},
    {(syscall_t) sys_getsockname, 3},
    {(syscall_t) sys_getpeername, 3},
    {(syscall_t) sys_socketpair, 4},
    {(syscall_t) sys_send, 4}, // send
    {(syscall_t) sys_recv, 4}, // recv
    {(syscall_t) sys_sendto, 6},
    {(syscall_t) sys_recvfrom, 6},
    {(syscall_t) sys_shutdown, 2},
    {(syscall_t) sys_setsockopt, 5},
    {(syscall_t) sys_getsockopt, 5},
    {(syscall_t) sys_sendmsg, 3},
    {(syscall_t) sys_recvmsg, 3},
    {NULL}, // accept4
    {NULL}, // recvmmsg
    {(syscall_t) sys_sendmmsg, 4},
};

int_t sys_socketcall(dword_t call_num, addr_t args_addr) {
    STRACE("%d ", call_num);
    if (call_num < 1 || call_num >= sizeof(socket_calls)/sizeof(socket_calls[0]))
        return _EINVAL;
    struct socket_call call = socket_calls[call_num];
    if (call.func == NULL) {
        FIXME("socketcall %d", call_num);
        return _ENOSYS;
    }

    dword_t args[6];
    if (user_read(args_addr, args, sizeof(dword_t) * call.args))
        return _EFAULT;
    return call.func(args[0], args[1], args[2], args[3], args[4], args[5]);
}

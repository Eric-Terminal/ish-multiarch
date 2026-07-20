#ifndef FD_H
#define FD_H
#include <dirent.h>
#include <limits.h>
#include "kernel/memory.h"
#include "util/list.h"
#include "util/sync.h"
#include "util/bits.h"
#include "fs/stat.h"
#include "fs/proc.h"
#include "fs/sockrestart.h"

struct task;
struct unix_pending_peer;

// 高位与引用数共用一次 CAS，使 GC 封锁和弱引用获取不可交错穿透。
#define FD_REFCOUNT_ACQUIRE_BLOCKED (UINT_MAX / 2 + 1)
#define FD_REFCOUNT_VALUE_MASK (FD_REFCOUNT_ACQUIRE_BLOCKED - 1)

// FIXME almost everything that uses the structs in this file does so without any kind of sane locking

struct fd {
    atomic_uint refcount;
    unsigned flags;
    // provider 原子 open 的结果；只描述该打开是否创建了最终对象。
    bool opened_created;
    // generic open 已记录 guest 访问模式时，provider 不得泄漏提升能力。
    bool logical_access_mode;
    mode_t_ type; // just the S_IFMT part, it can't change
    const struct fd_ops *ops;
    struct list poll_fds;
    lock_t poll_lock;
    off_t_ offset;

    // fd data
    union {
        // tty
        struct {
            struct tty *tty;
            // links together fds pointing to the same tty
            // locked by the tty
            struct list tty_other_fds;
        };
        struct {
            struct poll *poll;
        } epollfd;
        struct {
            uint64_t val;
        } eventfd;
        struct {
            struct timer *timer;
            uint64_t expirations;
        } timerfd;
        struct {
            int domain;
            int type;
            // host 侧为兼容受限 socket 可能使用不同协议。
            int protocol;
            // Linux guest 观察到的 sk_protocol，与 host 适配分离。
            int guest_protocol;
            bool inet_explicitly_bound;
            // 非阻塞 TCP connect 尚未由成功或 SO_ERROR 终结。
            bool inet_connect_pending;
            // host 与 guest 的 listener 生命周期会在 shutdown 时分离。
            bool host_listening;
            bool guest_listening;
            dword_t listen_backlog;
            uint64_t listen_generation;

            // These are only used as strong references, to keep the inode
            // alive while there is a listener.
            struct inode_data *unix_name_inode;
            struct unix_abstract *unix_name_abstract;
            uint8_t unix_name_len;
            // pathname 在未传终止 NUL 时需要额外一字节保存 Linux 返回形式。
            char unix_name[109];
            // 内部 host 后备节点不属于 guest 命名空间，最终关闭时按身份清理。
            bool unix_backing_owned;
            uint64_t unix_backing_device;
            uint64_t unix_backing_inode;
            uint32_t unix_backing_socket_id;
            char unix_backing_path[108];
            // 反向名称对象会被已排队的数据报保活，源 fd 关闭后仍可回报名称。
            struct unix_bound_name *unix_bound_name;
            struct fd *unix_peer; // locked by peer_lock, for simplicity
            // DGRAM 路由允许非对称 connect；目标维护弱反向链并在最终关闭时拆除。
            struct fd *unix_dgram_peer;
            struct list unix_dgram_senders;
            struct list unix_dgram_peer_link;
            // 对端关闭后，本地查询仍必须返回建连时确认的 Unix 名称。
            bool unix_peer_name_valid;
            uint8_t unix_peer_name_len;
            char unix_peer_name[109];
            // DGRAM 的 guest 名称可能同名重绑，实际路由身份另存 host 后备路径。
            bool unix_peer_transport_valid;
            char unix_peer_transport_path[108];
            bool unix_peer_handshake_pending;
            bool unix_peer_handshake_rejected;
            // 只传播 connect 成功后、accept 前发生的 shutdown 调用。
            unsigned unix_pending_peer_shutdown;
            uint64_t unix_connect_generation;
            struct unix_pending_peer *unix_pending_connect;
            bool unix_peer_cred_valid;
            struct ucred_ {
                pid_t_ pid;
                uid_t_ uid;
                uid_t_ gid;
            } unix_peer_cred;
            // SCM 队列由 socket 层的全局顺序锁保护，确保与 host dummy 同序。
            struct list unix_scm;
            // connect 已完成而 accept 尚未发生时暂存发送的 SCM_RIGHTS。
            struct list unix_pending_scm;
            // hidden transport 在 guest bind 前发送的匿名源地址逐记录保序。
            struct unix_anonymous_source *unix_anonymous_sources_head;
            struct unix_anonymous_source *unix_anonymous_sources_tail;
            // host 消息与内部 SCM 队列必须按同一个接收顺序配对。
            lock_t unix_recv_lock;
            // 只有 SCM 强引用存在时进入 vertex 链，链本身不持有引用。
            struct list unix_scm_vertex;
            atomic_uint unix_scm_incoming;
            bool unix_scm_gc_live;
            bool unix_scm_gc_collect;
            // 位 0/1 分别记录 Linux guest 的读/写关闭；32 位原子在
            // arm64 与 arm64_32 上都能直接参与并发收发状态机。
            atomic_uint guest_shutdown;
            // 非流式 Unix connect 每次成功后递增，使阻塞 send 能发现换 peer。
            atomic_uint unix_route_generation;
            // 每次实际消费一条记录后递增，封住 ENOBUFS 到 poll 登记的窗口。
            atomic_uint unix_capacity_generation;
            // 正数 Linux errno；recv/send/SO_ERROR 中只能有一个消费者。
            atomic_int guest_error;
            // DGRAM 已连接对端死亡只由下一次无地址发送消费，不进入 SO_ERROR。
            atomic_int unix_send_error;
            // host 路由无法恢复且也无法安全断开时，永久禁止继续 Unix I/O。
            atomic_bool unix_transport_failed;
            struct ucred_ unix_cred;
        } socket;

        // See app/Pasteboard.m
        struct {
            // UIPasteboard.changeCount
            uint64_t generation;
            // Buffer for written data
            void* buffer;
            // its capacity
            size_t buffer_cap;
            // length of actual data stored in the buffer
            size_t buffer_len;
        } clipboard;

        // can fit anything in here
        void *data;
    };
    // fs data
    union {
        struct {
            struct proc_entry entry;
            unsigned dir_index;
            struct proc_data data;
        } proc;
        struct {
            int num;
        } devpts;
        struct {
            struct tmp_dirent *dirent;
            struct tmp_dirent *dir_pos;
        } tmpfs;
        void *fs_data;
    };

    // fs/inode data
    struct mount *mount;
    int real_fd; // seeks on this fd require the lock TODO think about making a special lock just for that
    DIR *dir;
    struct inode_data *inode;
    ino_t fake_inode;
    struct statbuf stat; // for adhoc fs
    struct fd_sockrestart sockrestart; // argh

    // these are used for a variety of things related to the fd
    lock_t lock;
    cond_t cond;
};

static inline unsigned fd_refcount_read(const struct fd *fd) {
    return atomic_load(&fd->refcount) & FD_REFCOUNT_VALUE_MASK;
}

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
struct fd *fd_retain(struct fd *fd);
// 弱注册表只能在对象尚未进入最终关闭时获取强引用。
struct fd *fd_try_retain(struct fd *fd);
int fd_close(struct fd *fd);

int fd_getflags(struct fd *fd);
int fd_setflags(struct fd *fd, int flags);

// Linux fcntl 命令值；架构服务只应复用无指针的公共语义。
#define F_DUPFD_ 0
#define F_GETFD_ 1
#define F_SETFD_ 2
#define F_GETFL_ 3
#define F_SETFL_ 4
#define F_DUPFD_CLOEXEC_ 1030
#define FD_CLOEXEC_ 1

#define NAME_MAX 255
struct dir_entry {
    qword_t inode;
    char name[NAME_MAX + 1];
};

#define LSEEK_SET 0
#define LSEEK_CUR 1
#define LSEEK_END 2

struct fd_ops {
    /* 仅真实普通文件 provider 可声明，供 lazy 文件页缓存读取。 */
    bool page_cacheable;

    // required for files
    // TODO make optional for non-files
    ssize_t (*read)(struct fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct fd *fd, const void *buf, size_t bufsize);
    ssize_t (*pread)(struct fd *fd, void *buf, size_t bufsize, off_t off);
    ssize_t (*pwrite)(struct fd *fd, const void *buf, size_t bufsize, off_t off);
    /* pager 写回专用：严格按 off 写入，不得受 O_APPEND 或顺序 offset 影响。 */
    ssize_t (*page_pwrite)(
            struct fd *fd, const void *buf, size_t bufsize, off_t off);
    off_t_ (*lseek)(struct fd *fd, off_t_ off, int whence);

    // 调用者必须持有 fd->lock；实现不得重复获取同一把锁。
    // 目录描述符必须提供该操作。
    int (*readdir)(struct fd *fd, struct dir_entry *entry);
    // Return an opaque value representing the current point in the directory stream
    // optional, fd->offset will be used instead
    off_t_ (*telldir)(struct fd *fd);
    // Seek to the location represented by a pointer returned from telldir
    // optional, fd->offset will be used instead
    void (*seekdir)(struct fd *fd, off_t_ ptr);

    // map the file
    int (*mmap)(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags);

    // returns a bitmask of operations that won't block
    int (*poll)(struct fd *fd);

    // returns the size needed for the output of ioctl, 0 if the arg is not a
    // pointer, -1 for invalid command
    ssize_t (*ioctl_size)(int cmd);
    // if ioctl_size returns non-zero, arg must point to ioctl_size valid bytes
    int (*ioctl)(struct fd *fd, int cmd, void *arg);

    /* 调用者持有 fd->lock；fdatasync 缺失时上层可退化为更强的 fsync。 */
    int (*fsync)(struct fd *fd);
    int (*fdatasync)(struct fd *fd);
    int (*close)(struct fd *fd);

    // handle F_GETFL, i.e. return open flags for this fd
    int (*getflags)(struct fd *fd);
    // handle F_SETFL, i.e. set O_NONBLOCK
    int (*setflags)(struct fd *fd, dword_t arg);
};

struct fdtable {
    atomic_uint refcount;
    unsigned size;
    struct fd **files;
    bits_t *cloexec;
    // 安装事务可先预留槽位而不向 guest 发布文件对象。
    bits_t *reserved;
    qword_t *generations;
    lock_t lock;
};

#define FD_RESERVATION_MAX 2

// 事务持有原 fdtable；numbers 在事务结束前可于锁外只读。
struct fd_reservation {
    struct fdtable *table;
    fd_t numbers[FD_RESERVATION_MAX];
    unsigned count;
};

struct fdtable *fdtable_new(int size);
void fdtable_release(struct fdtable *table);
struct fdtable *fdtable_copy(struct fdtable *table);
void fdtable_free(struct fdtable *table);
void fdtable_do_cloexec(struct fdtable *table);
// 调用者必须在访问返回指针期间持有 table->lock。
struct fd *fdtable_get(struct fdtable *table, fd_t f);

// 返回借用指针；与兼容接口相同，不延长 fd 生命周期。
struct fd *f_get_task(struct task *task, fd_t f);
// 返回独立引用；调用方完成操作后必须 fd_close。
struct fd *f_get_task_retain(struct task *task, fd_t f);
struct fd *f_get(fd_t f);
// 原子预留一或两个最低空槽；预留项对 guest 不可见。
// 调用方必须拥有活跃 task，并与该 task 的 exec/退出生命周期串行。
int f_reserve_task(struct task *task, unsigned count,
        struct fd_reservation *reservation);
// 只撤销本事务仍拥有的预留，不处理任何 fd 引用。
void f_reservation_cancel(struct fd_reservation *reservation);
// 接管 count 个 fd 引用并原子发布；flags 同时作用于全部发布项。
int f_reservation_publish(struct fd_reservation *reservation,
        struct fd *fds[FD_RESERVATION_MAX], int flags,
        qword_t generations[FD_RESERVATION_MAX]);
// 接管 fd 引用：成功时交给目标表，失败时销毁；flags 只处理 O_CLOEXEC 与 O_NONBLOCK。
fd_t f_install_task(struct task *task, struct fd *fd, int flags);
// tracked 版本额外返回本次安装的槽位代数，供跨回调的精确失败回滚使用。
fd_t f_install_task_tracked(struct task *task, struct fd *fd,
        int flags, qword_t *generation);
typedef int (*fd_receive_number_writer_t)(void *opaque, fd_t number);
// 先预留编号，锁外写入 guest，成功后才发布接收的 fd。
fd_t f_receive_task(struct task *task, struct fd *fd,
        int flags, fd_receive_number_writer_t write_number,
        void *opaque);
// 在同一 fdtable 临界区选择并发布两个互异槽位；无论成败都接管两个引用。
int f_install_pair_task_tracked(struct task *task,
        struct fd *fds[2], int flags, fd_t installed[2],
        qword_t generations[2]);
fd_t f_install(struct fd *fd, int flags);
int f_close_task(struct task *task, fd_t f);
int f_close(fd_t f);
// 指针与安装代数都吻合时才关闭，避免同对象 ABA 复用误伤新表项。
bool f_close_task_if_matches(
        struct task *task, fd_t f, struct fd *expected,
        qword_t generation);

// 复制操作只修改目标 task 的描述符表，不改变共享文件对象的状态 flags。
fd_t f_dupfd_task(struct task *task, fd_t old_fd,
        fd_t minimum, int flags);
fd_t f_dup2_task(struct task *task, fd_t old_fd, fd_t new_fd);
fd_t f_dup3_task(struct task *task, fd_t old_fd,
        fd_t new_fd, int flags);
int f_getfd_task(struct task *task, fd_t fd);
int f_setfd_task(struct task *task, fd_t fd, int flags);
int f_getfl_task(struct task *task, fd_t fd);
int f_setfl_task(struct task *task, fd_t fd, int flags);

#endif

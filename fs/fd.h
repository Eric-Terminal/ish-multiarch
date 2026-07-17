#ifndef FD_H
#define FD_H
#include <dirent.h>
#include "kernel/memory.h"
#include "util/list.h"
#include "util/sync.h"
#include "util/bits.h"
#include "fs/stat.h"
#include "fs/proc.h"
#include "fs/sockrestart.h"

struct task;

// FIXME almost everything that uses the structs in this file does so without any kind of sane locking

struct fd {
    atomic_uint refcount;
    unsigned flags;
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
            int protocol;

            // These are only used as strong references, to keep the inode
            // alive while there is a listener.
            struct inode_data *unix_name_inode;
            struct unix_abstract *unix_name_abstract;
            uint8_t unix_name_len;
            char unix_name[108];
            struct fd *unix_peer; // locked by peer_lock, for simplicity
            cond_t unix_got_peer;
            // Queue of struct scm for sending file descriptors
            // locked by fd->lock
            struct list unix_scm;
            struct ucred_ {
                pid_t_ pid;
                uid_t_ uid;
                uid_t_ gid;
            } unix_cred;
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

typedef sdword_t fd_t;
#define AT_FDCWD_ -100

struct fd *fd_create(const struct fd_ops *ops);
struct fd *fd_retain(struct fd *fd);
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

    int (*fsync)(struct fd *fd);
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
    qword_t *generations;
    lock_t lock;
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
// 接管 fd 引用：成功时交给目标表，失败时销毁；flags 只处理 O_CLOEXEC 与 O_NONBLOCK。
fd_t f_install_task(struct task *task, struct fd *fd, int flags);
// tracked 版本额外返回本次安装的槽位代数，供跨回调的精确失败回滚使用。
fd_t f_install_task_tracked(struct task *task, struct fd *fd,
        int flags, qword_t *generation);
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

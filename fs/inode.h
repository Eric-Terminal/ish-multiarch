#ifndef FS_INODE_H
#define FS_INODE_H
#include <sys/types.h>
#include "misc.h"
#include "util/list.h"
#include "util/sync.h"
struct mount;
struct fd;
struct guest_file_pager;

struct inode_data {
    unsigned refcount;
    // 对应 Linux inode sequence：释放后也绝不复用，供共享 futex 建键。
    qword_t futex_sequence;
    // 文件系统 provider 提供的无损设备身份，不使用 guest ABI 的 st_dev。
    qword_t device;
    ino_t number;
    struct mount *mount;
    struct list chain;

    struct list posix_locks;
    cond_t posix_unlock;

    uint32_t socket_id;

    /*
     * 两个弱字段受 lock 共同保护；pager 归零并完成最终写回后才摘除。
     * context 只供拥有 pager 实现的 kernel 层解释。
     */
    struct guest_file_pager *file_pager;
    void *file_pager_context;
    /*
     * i386 realfs 的共享 host mmap 与独立 AArch64 pager 不能同时存在。
     * 计数按 host mmap 的 data 后备对象登记，fork 共享不重复计数。
     */
    unsigned host_shared_mapping_count;
    cond_t file_pager_changed;
    /*
     * 同 inode 内容 I/O 域。允许持有时短暂取 inode->lock 复查弱槽；
     * 不得持 inode->lock 或 inodes_lock 等待此锁。
     */
    lock_t file_io_lock;
    lock_t lock;
};

struct inode_data *inode_get(
        struct mount *mount, qword_t device, ino_t inode);
void inode_retain(struct inode_data *inode);
void inode_release(struct inode_data *inode);

/* 成功时返回一份由 host 共享映射 token 持有的 inode 强引用。 */
bool inode_try_begin_host_shared_mapping(struct inode_data *inode);
void inode_end_host_shared_mapping(struct inode_data *inode);

// generic_open must lock out anything trying to destroy an inode between
// opening the file and acquiring a reference to its inode. For this purpose
// only, the inodes_lock and inode_get_unlocked are made available. Think
// carefully before using them for anything else.
// mount->lock nests inside this.
// To quote @dril: i despise this lock. id love nothing more than to kick it
// through the wall and shatter it into 100 deadlocks. But i need it
extern lock_t inodes_lock;
struct inode_data *inode_get_unlocked(
        struct mount *mount, qword_t device, ino_t inode);

// calls mount->fs->inode_orphaned if this inode is orphaned, while holding indoes_lock
void inode_check_orphaned(struct mount *mount, ino_t ino);

// file locking stuff (maybe should go in kernel/calls.h?)

#define F_RDLCK_ 0
#define F_WRLCK_ 1
#define F_UNLCK_ 2

struct file_lock {
    off_t_ start;
    off_t_ end;
    int type;
    pid_t_ pid;
    void *owner;
    struct list locks;
};

struct flock_ {
    word_t type;
    word_t whence;
    off_t_ start;
    off_t_ len;
    pid_t_ pid;
} __attribute__((packed));
struct flock32_ {
    word_t type;
    word_t whence;
    dword_t start;
    dword_t len;
    pid_t_ pid;
} __attribute__((packed));

int fcntl_getlk(struct fd *fd, struct flock_ *flock);
// cmd should be either F_SETLK or F_SETLKW
int fcntl_setlk(struct fd *fd, struct flock_ *flock, bool block);

// locks the inode internally
void file_lock_remove_owned_by(struct fd *fd, void *owner);

#endif

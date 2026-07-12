#ifndef FS_H
#define FS_H

#include "misc.h"
#include "util/list.h"
#include "fs/stat.h"
#include "fs/dev.h"
#include "fs/fake-db.h"
#include "fs/fix_path.h"
#include "kernel/memory.h"
#include <dirent.h>
#include <sqlite3.h>

struct task;
struct dir_entry;

// 回调在目标 fd 的位置锁内执行：返回正记录长度表示已提交，负值表示需要回滚。
typedef sqword_t (*file_dirent_emit_t)(void *opaque,
        const struct dir_entry *entry, unsigned long next_position);

struct fs_info {
    atomic_uint refcount;
    mode_t_ umask;
    struct fd *pwd;
    struct fd *root;
    lock_t lock;
};
struct fs_info *fs_info_new(void);
struct fs_info *fs_info_copy(struct fs_info *fs);
void fs_info_release(struct fs_info *fs);

void fs_chdir(struct fs_info *fs, struct fd *pwd);

// fd_ops 与 fs_ops 仍可读取线程局部 current；调用方必须在 task 自己的执行线程内使用这些入口。
ssize_t file_read_task(struct task *task, fd_t fd, void *buffer, size_t size);
ssize_t file_read_fd(struct fd *fd, void *buffer, size_t size);
ssize_t file_write_task(struct task *task, fd_t fd, const void *buffer, size_t size);
ssize_t file_write_fd(struct fd *fd, const void *buffer, size_t size);
sqword_t file_lseek_task(
        struct task *task, fd_t fd, sqword_t offset, int whence);
sqword_t file_getdents_task(struct task *task, fd_t fd,
        file_dirent_emit_t emit, void *opaque);
int file_write_check_task(struct task *task, fd_t fd);
int file_read_check_fd(struct fd *fd);
int file_write_check_fd(struct fd *fd);
int file_fstat_task(struct task *task, fd_t fd, struct statbuf *stat);
int file_statat_task(struct task *task, fd_t dirfd, const char *path,
        int flags, struct statbuf *stat);
ssize_t fs_getcwd_task(struct task *task, char *buffer, size_t size);
int file_chdir_task(struct task *task, const char *path);
fd_t file_openat_task(struct task *task, fd_t dirfd,
        const char *path, int flags, mode_t_ mode);
int file_unlinkat_task(struct task *task, fd_t dirfd,
        const char *path, bool remove_directory);

#define MAX_PATH 4096
#define MAX_NAME 256

struct attr {
    enum attr_type {
        attr_uid,
        attr_gid,
        attr_mode,
        attr_size,
    } type;
    union {
        uid_t_ uid;
        uid_t_ gid;
        mode_t_ mode;
        off_t_ size;
    };
};
#define make_attr(_type, thing) \
    ((struct attr) {.type = attr_##_type, ._type = thing})

#define AT_SYMLINK_NOFOLLOW_ 0x100
#define AT_NO_AUTOMOUNT_ 0x800
#define AT_EMPTY_PATH_ 0x1000
#define AT_STATX_FORCE_SYNC_ 0x2000
#define AT_STATX_DONT_SYNC_ 0x4000
#define AT_STATX_SYNC_TYPE_ (AT_STATX_FORCE_SYNC_ | AT_STATX_DONT_SYNC_)
#define AT_STATAT_SUPPORTED_FLAGS_ (AT_SYMLINK_NOFOLLOW_ | \
        AT_NO_AUTOMOUNT_ | AT_EMPTY_PATH_ | AT_STATX_SYNC_TYPE_)
#define AT_REMOVEDIR_ 0x200

struct fd *generic_open(const char *path, int flags, int mode);
struct fd *generic_open_exec(const char *path);
struct fd *generic_openat_task(struct task *task, struct fd *at,
        const char *path, int flags, int mode);
struct fd *generic_openat_exec_task(struct task *task,
        struct fd *at, const char *path);
struct fd *generic_open_directory_task(struct task *task, const char *path);
struct fd *generic_openat(struct fd *at, const char *path, int flags, int mode);
int generic_getpath(struct fd *fd, char *buf);
int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw);
int generic_unlinkat_task(struct task *task,
        struct fd *at, const char *path);
int generic_unlinkat(struct fd *at, const char *path);
int generic_rmdirat_task(struct task *task,
        struct fd *at, const char *path);
int generic_rmdirat(struct fd *at, const char *path);
int generic_renameat(struct fd *src_at, const char *src, struct fd *dst_at, const char *dst);
int generic_symlinkat(const char *target, struct fd *at, const char *link);
int generic_mknodat(struct fd *at, const char *path, mode_t_ mode, dev_t_ dev);
int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size);
#define AC_R 4
#define AC_W 2
#define AC_X 1
#define AC_F 0
int generic_accessat(struct fd *dirfd, const char *path, int mode);
int generic_statat_task(struct task *task, struct fd *at,
        const char *path, struct statbuf *stat, bool follow_links);
int generic_statat(struct fd *at, const char *path,
        struct statbuf *stat, bool follow_links);
int generic_setattrat(struct fd *at, const char *path, struct attr attr, bool follow_links);
int generic_utime(struct fd *at, const char *path, struct timespec atime, struct timespec mtime, bool follow_links);
ssize_t generic_readlinkat(struct fd *at, const char *path, char *buf, size_t bufsize);
int generic_mkdirat(struct fd *at, const char *path, mode_t_ mode);

int access_check_task(struct task *task, struct statbuf *stat, int check);
int access_check(struct statbuf *stat, int check);

struct mount {
    const char *point;
    const char *source;
    const char *info;
    int flags;
    const struct fs_ops *fs;
    unsigned refcount;
    struct list mounts;

    int root_fd;
    union {
        void *data;
        struct fakefs_db fakefs;
    };
};
extern lock_t mounts_lock;

// returns a reference, which must be released
struct mount *mount_find(char *path);
void mount_retain(struct mount *mount);
void mount_release(struct mount *mount);

// must hold mounts_lock while calling these, or traversing mounts
int do_mount(const struct fs_ops *fs, const char *source, const char *point, const char *info, int flags);
int do_umount(const char *point);
int mount_remove(struct mount *mount);
extern struct list mounts;

bool mount_param_flag(const char *info, const char *flag);

// open flags
#define O_ACCMODE_ 3
#define O_RDONLY_ 0
#define O_WRONLY_ (1 << 0)
#define O_RDWR_ (1 << 1)
#define O_CREAT_ (1 << 6)
#define O_EXCL_ (1 << 7)
#define O_NOCTTY_ (1 << 8)
#define O_TRUNC_ (1 << 9)
#define O_APPEND_ (1 << 10)
#define O_NONBLOCK_ (1 << 11)
#define O_DIRECTORY_ (1 << 16)
#define O_CLOEXEC_ (1 << 19)

// generic ioctls
#define FIONREAD_ 0x541b
#define FIONBIO_ 0x5421
#define FIONCLEX_ 0x5450
#define FIOCLEX_ 0x5451

// All operations are optional unless otherwise specified
struct fs_ops {
    const char *name;
    int magic;

    int (*mount)(struct mount *mount);
    int (*umount)(struct mount *mount);
    int (*statfs)(struct mount *mount, struct statfsbuf *stat);

    struct fd *(*open)(struct mount *mount, const char *path, int flags, int mode); // required
    ssize_t (*readlink)(struct mount *mount, const char *path, char *buf, size_t bufsize);

    // These return _EPERM if not present
    int (*link)(struct mount *mount, const char *src, const char *dst);
    int (*unlink)(struct mount *mount, const char *path);
    int (*rmdir)(struct mount *mount, const char *path);
    int (*rename)(struct mount *mount, const char *src, const char *dst);
    int (*symlink)(struct mount *mount, const char *target, const char *link);
    int (*mknod)(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev);
    int (*mkdir)(struct mount *mount, const char *path, mode_t_ mode);

    // There's a close function in both the fs and fd to handle device files
    // where, for instance, there's a real_fd needed for getpath and also a tty
    // reference, and both need to be released when the fd is closed.
    // If they are the same function, it will only be called once.
    int (*close)(struct fd *fd);

    int (*stat)(struct mount *mount, const char *path, struct statbuf *stat); // required
    int (*fstat)(struct fd *fd, struct statbuf *stat); // required
    int (*setattr)(struct mount *mount, const char *path, struct attr attr);
    int (*fsetattr)(struct fd *fd, struct attr attr);
    int (*utime)(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime);
    // Returns the path of the file descriptor, null terminated, buf must be at least MAX_PATH+1
    int (*getpath)(struct fd *fd, char *buf); // required

    int (*flock)(struct fd *fd, int operation);

    // If present, called when all references to an inode_data for this
    // filesystem go away.
    void (*inode_orphaned)(struct mount *mount, ino_t inode);
};

struct mount *find_mount_and_trim_path(char *path);

// adhoc fs
struct fd *adhoc_fd_create(const struct fd_ops *ops);
// this is for the "wtf is apple smoking" section
bool is_adhoc_fd(struct fd *fd);

// filesystems
extern const struct fs_ops procfs;
extern const struct fs_ops fakefs;
extern const struct fs_ops devptsfs;
extern const struct fs_ops tmpfs;
void fs_register(const struct fs_ops *fs);

#endif

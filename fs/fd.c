#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/resource.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/fd.h"
#include "fs/inode.h"

struct fd *fd_create(const struct fd_ops *ops) {
    struct fd *fd = malloc(sizeof(struct fd));
    if (fd == NULL)
        return NULL;
    *fd = (struct fd) {};
    fd->ops = ops;
    fd->refcount = 1;
    fd->flags = 0;
    fd->mount = NULL;
    fd->offset = 0;
    list_init(&fd->poll_fds);
    lock_init(&fd->poll_lock);
    lock_init(&fd->lock);
    cond_init(&fd->cond);
    return fd;
}

struct fd *fd_retain(struct fd *fd) {
    fd->refcount++;
    return fd;
}

struct fd *fd_try_retain(struct fd *fd) {
    unsigned references = atomic_load(&fd->refcount);
    while (references != 0) {
        assert(references != UINT_MAX);
        if (atomic_compare_exchange_weak(
                &fd->refcount, &references, references + 1))
            return fd;
    }
    return NULL;
}

int fd_close(struct fd *fd) {
    int err = 0;
    if (--fd->refcount == 0) {
        poll_cleanup_fd(fd);
        if (fd->ops->close)
            err = fd->ops->close(fd);
        // see comment in close in kernel/fs.h
        if (fd->mount && fd->mount->fs->close && fd->mount->fs->close != fd->ops->close) {
            int new_err = fd->mount->fs->close(fd);
            if (new_err < 0)
                err = new_err;
        }

        if (fd->inode)
            inode_release(fd->inode);
        if (fd->mount)
            mount_release(fd->mount);
        free(fd);
    }
    return err;
}

static int fdtable_resize(struct fdtable *table, unsigned size);

struct fdtable *fdtable_new(int size) {
    struct fdtable *fdt = malloc(sizeof(struct fdtable));
    if (fdt == NULL)
        return ERR_PTR(_ENOMEM);
    fdt->refcount = 1;
    fdt->size = 0;
    fdt->files = NULL;
    fdt->cloexec = NULL;
    fdt->reserved = NULL;
    fdt->generations = NULL;
    lock_init(&fdt->lock);
    int err = fdtable_resize(fdt, size);
    if (err < 0) {
        free(fdt);
        return ERR_PTR(err);
    }
    return fdt;
}

static int fdtable_close(struct fdtable *table, fd_t f);

// FIXME this looks like it has the classic refcount UAF
void fdtable_release(struct fdtable *table) {
    lock(&table->lock);
    if (--table->refcount == 0) {
        for (fd_t f = 0; (unsigned) f < table->size; f++)
            fdtable_close(table, f);
        free(table->files);
        free(table->cloexec);
        free(table->reserved);
        free(table->generations);
        unlock(&table->lock);
        free(table);
    } else {
        unlock(&table->lock);
    }
}

static int fdtable_resize(struct fdtable *table, unsigned size) {
    // currently the only legitimate use of this is to expand the table
    assert(size > table->size);

    struct fd **files = malloc(sizeof(struct fd *) * size);
    if (files == NULL)
        return _ENOMEM;
    memset(files, 0, sizeof(struct fd *) * size);
    if (table->files)
        memcpy(files, table->files, sizeof(struct fd *) * table->size);

    bits_t *cloexec = malloc(BITS_SIZE(size));
    if (cloexec == NULL) {
        free(files);
        return _ENOMEM;
    }
    memset(cloexec, 0, BITS_SIZE(size));
    if (table->cloexec)
        memcpy(cloexec, table->cloexec, BITS_SIZE(table->size));

    bits_t *reserved = malloc(BITS_SIZE(size));
    if (reserved == NULL) {
        free(cloexec);
        free(files);
        return _ENOMEM;
    }
    memset(reserved, 0, BITS_SIZE(size));
    if (table->reserved)
        memcpy(reserved, table->reserved, BITS_SIZE(table->size));

    qword_t *generations = calloc(size, sizeof(*generations));
    if (generations == NULL) {
        free(reserved);
        free(cloexec);
        free(files);
        return _ENOMEM;
    }
    if (table->generations)
        memcpy(generations, table->generations,
                sizeof(*generations) * table->size);

    free(table->files);
    table->files = files;
    free(table->cloexec);
    table->cloexec = cloexec;
    free(table->reserved);
    table->reserved = reserved;
    free(table->generations);
    table->generations = generations;
    table->size = size;
    return 0;
}

struct fdtable *fdtable_copy(struct fdtable *table) {
    lock(&table->lock);
    int size = table->size;
    struct fdtable *new_table = fdtable_new(size);
    if (IS_ERR(new_table)) {
        unlock(&table->lock);
        return new_table;
    }
    memcpy(new_table->files, table->files, sizeof(struct fd *) * size);
    for (fd_t f = 0; f < size; f++)
        if (new_table->files[f])
            new_table->files[f]->refcount++;
    memcpy(new_table->cloexec, table->cloexec, BITS_SIZE(size));
    memcpy(new_table->generations, table->generations,
            sizeof(*table->generations) * size);
    unlock(&table->lock);
    return new_table;
}

static int fdtable_expand(struct task *task, fd_t max) {
    struct fdtable *table = task->files;
    unsigned size = max + 1;
    if (size > rlimit_task(task, RLIMIT_NOFILE_))
        return _EMFILE;
    if (table->size >= size)
        return 0;
    return fdtable_resize(table, max + 1);
}

struct fd *fdtable_get(struct fdtable *table, fd_t f) {
    if (f < 0 || (unsigned) f >= table->size)
        return NULL;
    return table->files[f];
}

struct fd *f_get_task(struct task *task, fd_t f) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    unlock(&table->lock);
    return fd;
}

struct fd *f_get_task_retain(struct task *task, fd_t f) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, f);
    if (fd != NULL)
        fd_retain(fd);
    unlock(&table->lock);
    return fd;
}

struct fd *f_get(fd_t f) {
    return f_get_task(current, f);
}

static fd_t f_install_find(struct task *task, fd_t start) {
    assert(start >= 0);
    struct fdtable *table = task->files;
    unsigned size = rlimit_task(task, RLIMIT_NOFILE_);
    if (size > table->size)
        size = table->size;

    fd_t f;
    for (f = start; (unsigned) f < size; f++)
        if (table->files[f] == NULL &&
                !bit_test(f, table->reserved))
            break;
    if ((unsigned) f >= size) {
        int err = fdtable_expand(task, f);
        if (err < 0)
            f = err;
    }
    return f;
}

static void f_install_publish(struct fdtable *table,
        fd_t f, struct fd *fd, qword_t *generation) {
    assert(f >= 0 && (unsigned) f < table->size &&
            table->files[f] == NULL);
    table->files[f] = fd;
    bit_clear(f, table->cloexec);
    table->generations[f]++;
    if (generation != NULL)
        *generation = table->generations[f];
}

static fd_t f_install_start(struct task *task, struct fd *fd,
        fd_t start, qword_t *generation) {
    fd_t f = f_install_find(task, start);
    if (f >= 0)
        f_install_publish(task->files, f, fd, generation);
    else
        fd_close(fd);
    return f;
}

static void f_install_finish(
        struct fdtable *table, fd_t f, struct fd *fd, int flags) {
    if (flags & O_CLOEXEC_)
        bit_set(f, table->cloexec);
    if (flags & O_NONBLOCK_)
        fd_setflags(fd, O_NONBLOCK_);
}

static int fdtable_close(struct fdtable *table, fd_t f);

fd_t f_install_task_tracked(struct task *task, struct fd *fd,
        int flags, qword_t *generation) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    fd_t f = f_install_start(task, fd, 0, generation);
    if (f >= 0)
        f_install_finish(table, f, fd, flags);
    unlock(&table->lock);
    return f;
}

fd_t f_receive_task(struct task *task, struct fd *fd,
        int flags, fd_receive_number_writer_t write_number,
        void *opaque) {
    assert(task != NULL && fd != NULL && write_number != NULL);
    struct fdtable *table = task->files;
    lock(&table->lock);
    fd_t number = f_install_find(task, 0);
    if (number >= 0)
        bit_set(number, table->reserved);
    unlock(&table->lock);
    if (number < 0)
        goto reject;

    int error = write_number(opaque, number);
    lock(&table->lock);
    assert((unsigned) number < table->size &&
            table->files[number] == NULL &&
            bit_test(number, table->reserved));
    bit_clear(number, table->reserved);
    if (error >= 0) {
        f_install_publish(table, number, fd, NULL);
        f_install_finish(table, number, fd, flags);
    }
    unlock(&table->lock);
    if (error < 0) {
        number = error;
        goto reject;
    }
    return number;

reject:
    fd_close(fd);
    return number;
}

int f_install_pair_task_tracked(struct task *task,
        struct fd *fds[2], int flags, fd_t installed[2],
        qword_t generations[2]) {
    assert(task != NULL && fds != NULL && fds[0] != NULL &&
            fds[1] != NULL && installed != NULL && generations != NULL);
    struct fdtable *table = task->files;
    lock(&table->lock);
    fd_t first = f_install_start(
            task, fds[0], 0, &generations[0]);
    if (first < 0) {
        unlock(&table->lock);
        fd_close(fds[1]);
        return first;
    }
    f_install_finish(table, first, fds[0], flags);

    fd_t second = f_install_start(
            task, fds[1], 0, &generations[1]);
    if (second < 0) {
        fdtable_close(table, first);
        unlock(&table->lock);
        return second;
    }
    f_install_finish(table, second, fds[1], flags);
    installed[0] = first;
    installed[1] = second;
    unlock(&table->lock);
    return 0;
}

fd_t f_install_task(struct task *task, struct fd *fd, int flags) {
    return f_install_task_tracked(task, fd, flags, NULL);
}

fd_t f_install(struct fd *fd, int flags) {
    return f_install_task(current, fd, flags);
}

static int fdtable_close(struct fdtable *table, fd_t f) {
    struct fd *fd = fdtable_get(table, f);
    if (fd == NULL)
        return _EBADF;
    if (fd->inode != NULL) // temporary hack for files like sockets that right now don't have inodes but will eventually
        file_lock_remove_owned_by(fd, table);
    int err = fd_close(fd);
    table->files[f] = NULL;
    bit_clear(f, table->cloexec);
    return err;
}

int f_close_task(struct task *task, fd_t f) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    int err = fdtable_close(table, f);
    unlock(&table->lock);
    return err;
}

bool f_close_task_if_matches(
        struct task *task, fd_t f, struct fd *expected,
        qword_t generation) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    struct fd *actual = fdtable_get(table, f);
    bool matches = actual != NULL && actual == expected &&
            table->generations[f] == generation;
    if (matches)
        fdtable_close(table, f);
    unlock(&table->lock);
    return matches;
}

int f_close(fd_t f) {
    return f_close_task(current, f);
}

dword_t sys_close(fd_t f) {
    STRACE("close(%d)", f);
    return f_close(f);
}

void fdtable_do_cloexec(struct fdtable *table) {
    lock(&table->lock);
    for (fd_t f = 0; (unsigned) f < table->size; f++)
        if (bit_test(f, table->cloexec))
            fdtable_close(table, f);
    unlock(&table->lock);
}

#define F_GETLK_ 5
#define F_SETLK_ 6
#define F_SETLKW_ 7
#define F_GETLK64_ 12
#define F_SETLK64_ 13
#define F_SETLKW64_ 14

fd_t f_dupfd_task(struct task *task, fd_t old_fd,
        fd_t minimum, int flags) {
    if (flags & ~O_CLOEXEC_)
        return _EINVAL;
    struct fdtable *table = task->files;
    lock(&table->lock);
    struct fd *fd = fdtable_get(table, old_fd);
    if (fd == NULL) {
        unlock(&table->lock);
        return _EBADF;
    }
    if (minimum < 0 || (rlim_t_) minimum >=
            rlimit_task(task, RLIMIT_NOFILE_)) {
        unlock(&table->lock);
        return _EINVAL;
    }

    fd_retain(fd);
    fd_t duplicated = f_install_start(task, fd, minimum, NULL);
    if (duplicated >= 0 && (flags & O_CLOEXEC_))
        bit_set(duplicated, table->cloexec);
    unlock(&table->lock);
    return duplicated;
}

static fd_t f_dup_to_task(struct task *task, fd_t old_fd,
        fd_t new_fd, int flags, bool reject_same) {
    if (flags & ~O_CLOEXEC_)
        return _EINVAL;
    if (reject_same && old_fd == new_fd)
        return _EINVAL;

    struct fdtable *table = task->files;
    lock(&table->lock);
    if (new_fd < 0 || (rlim_t_) new_fd >=
            rlimit_task(task, RLIMIT_NOFILE_)) {
        unlock(&table->lock);
        return _EBADF;
    }
    struct fd *fd = fdtable_get(table, old_fd);
    if (fd == NULL) {
        unlock(&table->lock);
        return _EBADF;
    }
    if (old_fd == new_fd) {
        unlock(&table->lock);
        return new_fd;
    }

    int error = fdtable_expand(task, new_fd);
    if (error < 0) {
        unlock(&table->lock);
        return error;
    }
    if (bit_test(new_fd, table->reserved)) {
        unlock(&table->lock);
        return _EBUSY;
    }
    fd_retain(fd);
    if (fdtable_get(table, new_fd) != NULL)
        fdtable_close(table, new_fd);
    table->files[new_fd] = fd;
    table->generations[new_fd]++;
    if (flags & O_CLOEXEC_)
        bit_set(new_fd, table->cloexec);
    else
        bit_clear(new_fd, table->cloexec);
    unlock(&table->lock);
    return new_fd;
}

fd_t f_dup2_task(struct task *task, fd_t old_fd, fd_t new_fd) {
    return f_dup_to_task(task, old_fd, new_fd, 0, false);
}

fd_t f_dup3_task(struct task *task, fd_t old_fd,
        fd_t new_fd, int flags) {
    return f_dup_to_task(task, old_fd, new_fd, flags, true);
}

int f_getfd_task(struct task *task, fd_t fd) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    if (fdtable_get(table, fd) == NULL) {
        unlock(&table->lock);
        return _EBADF;
    }
    int flags = bit_test(fd, table->cloexec) ? FD_CLOEXEC_ : 0;
    unlock(&table->lock);
    return flags;
}

int f_setfd_task(struct task *task, fd_t fd, int flags) {
    struct fdtable *table = task->files;
    lock(&table->lock);
    if (fdtable_get(table, fd) == NULL) {
        unlock(&table->lock);
        return _EBADF;
    }
    if (flags & FD_CLOEXEC_)
        bit_set(fd, table->cloexec);
    else
        bit_clear(fd, table->cloexec);
    unlock(&table->lock);
    return 0;
}

int f_getfl_task(struct task *task, fd_t fd_number) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    int flags = fd_getflags(fd);
    fd_close(fd);
    return flags;
}

int f_setfl_task(struct task *task, fd_t fd_number, int flags) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    int error = fd_setflags(fd, flags);
    fd_close(fd);
    return error;
}

dword_t sys_dup(fd_t f) {
    STRACE("dup(%d)", f);
    return f_dupfd_task(current, f, 0, 0);
}

dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags) {
    STRACE("dup3(%d, %d, %d)", f, new_f, flags);
    return f_dup3_task(current, f, new_f, flags);
}

dword_t sys_dup2(fd_t f, fd_t new_f) {
    STRACE("dup2(%d, %d)", f, new_f);
    return f_dup2_task(current, f, new_f);
}

int fd_getflags(struct fd *fd) {
    lock(&fd->lock);
    int flags = fd->ops->getflags ?
            fd->ops->getflags(fd) : (int) fd->flags;
    unlock(&fd->lock);
    return flags;
}

#define FD_ALLOWED_FLAGS (O_APPEND_ | O_NONBLOCK_)
int fd_setflags(struct fd *fd, int flags) {
    lock(&fd->lock);
    int error;
    if (fd->ops->setflags) {
        error = fd->ops->setflags(fd, flags);
    } else {
        fd->flags = (fd->flags & ~FD_ALLOWED_FLAGS) |
                (flags & FD_ALLOWED_FLAGS);
        error = 0;
    }
    unlock(&fd->lock);
    return error;
}

dword_t sys_fcntl(fd_t f, dword_t cmd, dword_t arg) {
    struct flock32_ flock32;
    struct flock_ flock;
    int err;
    switch (cmd) {
        case F_DUPFD_:
            STRACE("fcntl(%d, F_DUPFD, %d)", f, arg);
            return f_dupfd_task(current, f, (fd_t) arg, 0);

        case F_DUPFD_CLOEXEC_:
            STRACE("fcntl(%d, F_DUPFD_CLOEXEC, %d)", f, arg);
            return f_dupfd_task(current, f, (fd_t) arg, O_CLOEXEC_);

        case F_GETFD_:
            STRACE("fcntl(%d, F_GETFD)", f);
            return f_getfd_task(current, f);
        case F_SETFD_:
            STRACE("fcntl(%d, F_SETFD, 0x%x)", f, arg);
            return f_setfd_task(current, f, arg);

        case F_GETFL_:
            STRACE("fcntl(%d, F_GETFL)", f);
            return f_getfl_task(current, f);
        case F_SETFL_:
            STRACE("fcntl(%d, F_SETFL, %#x)", f, arg);
            return f_setfl_task(current, f, arg);
    }

    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    switch (cmd) {

        case F_GETLK_:
            STRACE("fcntl(%d, F_GETLK, %#x)", f, arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0) {
                flock32.type = flock.type;
                flock32.whence = flock.whence;
                flock32.start = flock.start;
                flock32.len = flock.len;
                flock32.pid = flock.pid;
                if (user_write(arg, &flock32, sizeof(flock32)))
                    return _EFAULT;
            }
            return err;

        case F_GETLK64_:
            STRACE("fcntl(%d, F_GETLK64, %#x)", f, arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            err = fcntl_getlk(fd, &flock);
            if (err >= 0)
                if (user_write(arg, &flock, sizeof(flock)))
                    return _EFAULT;
            return err;

        case F_SETLK_:
        case F_SETLKW_:
            STRACE("fcntl(%d, F_SETLK%*s, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock32, sizeof(flock32)))
                return _EFAULT;
            flock.type = flock32.type;
            flock.whence = flock32.whence;
            flock.start = flock32.start;
            flock.len = flock32.len;
            flock.pid = flock32.pid;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW64_);

        case F_SETLK64_:
        case F_SETLKW64_:
            STRACE("fcntl(%d, F_SETLK%*s64, %#x)", f, cmd == F_SETLKW_, "W", arg);
            if (user_read(arg, &flock, sizeof(flock)))
                return _EFAULT;
            return fcntl_setlk(fd, &flock, cmd == F_SETLKW_);

        default:
            STRACE("fcntl(%d, %d)", f, cmd);
            return _EINVAL;
    }
}

dword_t sys_fcntl32(fd_t fd, dword_t cmd, dword_t arg) {
    switch (cmd) {
        case F_GETLK64_:
        case F_SETLK64_:
        case F_SETLKW64_:
            return _EINVAL;
    }
    return sys_fcntl(fd, cmd, arg);
}

#include <sys/stat.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"

// 调用者持有 fd->lock，保证读取、输出与必要的游标回滚不可被 lseek 穿插。
static off_t_ fd_telldir_locked(struct fd *fd) {
    off_t_ off = fd->offset;
    if (fd->ops->telldir)
        off = fd->ops->telldir(fd);
    return off;
}

static void fd_seekdir_locked(struct fd *fd, off_t_ off) {
    fd->offset = off;
    if (fd->ops->seekdir)
        fd->ops->seekdir(fd, off);
}

struct linux_dirent_ {
    dword_t inode;
    dword_t offset;
    word_t reclen;
    char name[];
} __attribute__((packed));

struct linux_dirent64_ {
    qword_t inode;
    qword_t offset;
    word_t reclen;
    byte_t type;
    char name[];
} __attribute__((packed));

size_t fill_dirent_32(void *dirent_data, ino_t inode, off_t_ offset, const char *name, int type) {
    struct linux_dirent_ *dirent = dirent_data;
    dirent->inode = (dword_t) inode;
    dirent->offset = (dword_t) offset;
    dirent->reclen = offsetof(struct linux_dirent_, name) +
        strlen(name) + 2; // name, null terminator, type
    strcpy(dirent->name, name);
    *((char *) dirent + dirent->reclen - 1) = type;
    return dirent->reclen;
}

size_t fill_dirent_64(void *dirent_data, ino_t inode, off_t_ offset, const char *name, int type) {
    struct linux_dirent64_ *dirent = dirent_data;
    dirent->inode = inode;
    dirent->offset = (qword_t) offset;
    dirent->reclen = offsetof(struct linux_dirent64_, name) +
        strlen(name) + 1; // name, null terminator
    dirent->type = type;
    strcpy(dirent->name, name);
    return dirent->reclen;
}

sqword_t file_getdents_task(struct task *task, fd_t fd_number,
        file_dirent_emit_t emit, void *opaque) {
    struct fd *fd = f_get_task_retain(task, fd_number);
    if (fd == NULL)
        return _EBADF;
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL) {
        fd_close(fd);
        return _ENOTDIR;
    }

    sqword_t completed = 0;
    lock(&fd->lock);
    while (true) {
        off_t_ before = fd_telldir_locked(fd);
        struct dir_entry entry;
        int error = fd->ops->readdir(fd, &entry);
        if (error <= 0) {
            if (error < 0) {
                fd_seekdir_locked(fd, before);
                if (completed == 0)
                    completed = error;
            }
            break;
        }

        off_t_ next = fd_telldir_locked(fd);
        sqword_t emitted = emit(opaque, &entry, next);
        assert(emitted != 0);
        if (emitted < 0) {
            fd_seekdir_locked(fd, before);
            if (completed == 0)
                completed = emitted;
            break;
        }
        completed += emitted;
    }
    unlock(&fd->lock);
    fd_close(fd);
    return completed;
}

struct legacy_getdents_context {
    addr_t address;
    dword_t remaining;
    size_t (*fill)(void *, ino_t, off_t_, const char *, int);
    unsigned printed;
};

static sqword_t emit_legacy_dirent(void *opaque,
        const struct dir_entry *entry, off_t_ next_position) {
    struct legacy_getdents_context *context = opaque;
    byte_t data[sizeof(struct linux_dirent64_) + NAME_MAX + 4];
    size_t length = context->fill(data, entry->inode,
            next_position, entry->name, 0);
    if (length > context->remaining)
        return _EINVAL;
    if (context->printed < 20) {
        STRACE(" {inode=%llu, offset=%lld, name=%s, type=0, reclen=%zu}",
                (unsigned long long) entry->inode,
                (long long) next_position,
                entry->name, length);
        context->printed++;
    }
    if (user_write(context->address, data, length))
        return _EFAULT;
    context->address += length;
    context->remaining -= length;
    return (sqword_t) length;
}

int_t sys_getdents_common(fd_t f, addr_t dirents, dword_t count,
        size_t (*fill_dirent)(void *, ino_t, off_t_, const char *, int)) {
    STRACE("getdents(%d, %#x, %#x)", f, dirents, count);
    struct legacy_getdents_context context = {
        .address = dirents,
        .remaining = count,
        .fill = fill_dirent,
    };
    return (int_t) file_getdents_task(
            current, f, emit_legacy_dirent, &context);
}

int_t sys_getdents(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_32);
}

int_t sys_getdents64(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_64);
}

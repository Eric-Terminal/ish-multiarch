#include <sys/stat.h>
#include <string.h>
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "util/refcount.h"
#include "debug.h"

// ========================
// ======== INODES ========
// ========================

struct tmp_inode {
    struct refcount refcount;
    lock_t lock;

    struct statbuf stat;
    union {
        void *file_data;
        //char *symlink_data;
    };
};

static struct tmp_inode *tmp_inode_new(mode_t_ mode) {
    struct tmp_inode *node = malloc(sizeof(struct tmp_inode));
    if (node == NULL)
        return NULL;
    refcount_init(node);
    lock_init(&node->lock);

    node->stat = (struct statbuf) {};
    static _Atomic ino_t next_inode = 1;
    node->stat.inode = next_inode++;

    node->stat.mode = mode;
    node->stat.uid = current->euid;
    node->stat.gid = current->egid;
    if (S_ISREG(mode))
        node->file_data = NULL;
    return node;
}

DEFINE_REFCOUNT_STATIC(tmp_inode)

static void tmp_inode_cleanup(struct tmp_inode *inode) {
    if (S_ISREG(inode->stat.mode)) {
        free(inode->file_data);
    }
    free(inode);
}

// ===================================
// ======== DIRECTORY ENTRIES ========
// ===================================

struct tmp_dirent {
    char name[MAX_NAME + 1];
    struct tmp_inode *inode;
    off_t_ index;

    struct tmp_dirent *parent;
    struct list children;
    off_t_ next_index;

    struct refcount refcount;
    lock_t lock;
    struct list dir;
};

DEFINE_REFCOUNT_STATIC(tmp_dirent)

static void tmp_dirent_cleanup(struct tmp_dirent *dirent) {
    list_remove_safe(&dirent->dir);
    tmp_inode_release(dirent->inode);
    if (dirent->parent != NULL)
        tmp_dirent_release(dirent->parent);
    free(dirent);
}

static void tmp_dirent_init(struct tmp_dirent *dirent) {
    refcount_init(dirent);
    list_init(&dirent->children);
    dirent->next_index = 0;
    lock_init(&dirent->lock);
}

// 无论成功或失败都消费 child 的所有权。
static int tmpfs_dir_link(struct tmp_dirent *dir, const char *name, struct tmp_inode *child, struct tmp_dirent **dirent_out) {
    if (!S_ISDIR(dir->inode->stat.mode)) {
        tmp_inode_release(child);
        return _ENOTDIR;
    }
    if (dir->next_index == INT64_MAX) {
        tmp_inode_release(child);
        return _EOVERFLOW;
    }
    struct tmp_dirent *new_dirent = malloc(sizeof(struct tmp_dirent));
    if (new_dirent == NULL) {
        tmp_inode_release(child);
        return _ENOMEM;
    }

    tmp_dirent_init(new_dirent);
    strcpy(new_dirent->name, name);
    new_dirent->inode = tmp_inode_retain(child);
    new_dirent->index = dir->next_index++;
    new_dirent->parent = tmp_dirent_retain(dir);
    list_add_tail(&dir->children, &new_dirent->dir);
    tmp_inode_release(child);

    if (dirent_out)
        *dirent_out = tmp_dirent_retain(new_dirent);
    return 0;
}

static void tmpfs_fd_seekdir(struct fd *fd, struct tmp_dirent *dirent) {
    if (dirent != NULL)
        tmp_dirent_retain(dirent);
    if (fd->tmpfs.dir_pos != NULL)
        tmp_dirent_release(fd->tmpfs.dir_pos);
    fd->tmpfs.dir_pos = dirent;
}

static struct tmp_dirent *tmpfs_dir_lookup(struct tmp_dirent *dir, const char *name) {
    if (!S_ISDIR(dir->inode->stat.mode))
        return ERR_PTR(_ENOTDIR);
    struct tmp_dirent *dirent = NULL;
    struct tmp_dirent *d;
    list_for_each_entry(&dir->children, d, dir) {
        if (d->inode == NULL)
            continue;
        if (strcmp(d->name, name) == 0) {
            dirent = d;
            break;
        }
    }
    if (dirent == NULL)
        return ERR_PTR(_ENOENT);
    return tmp_dirent_retain(dirent);
}

// TODO: should this function even exist? can't tmpfs_dir_link check for existence?
static int tmpfs_dir_lookup_existence(struct tmp_dirent *dir, const char *name) {
    struct tmp_dirent *dirent = tmpfs_dir_lookup(dir, name);
    if (dirent == ERR_PTR(_ENOENT))
        return 0;
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    tmp_dirent_release(dirent);
    return _EEXIST;
}

static struct tmp_dirent *__tmpfs_lookup(struct mount *mount, const char *path, bool parent, const char **filename_out) {
    struct tmp_dirent *root = mount->data;
    struct tmp_dirent *dirent = tmp_dirent_retain(root); // strong reference

    char component[MAX_NAME + 1] = {};
    int err = 0;
    while (path_next_component(&path, component, &err)) {
        if (parent && *path == '\0')
            break;

        lock(&dirent->lock);
        struct tmp_dirent *child = tmpfs_dir_lookup(dirent, component);
        unlock(&dirent->lock);

        tmp_dirent_release(dirent);
        if (IS_ERR(child))
            return child;
        dirent = child;
    }

    if (parent && filename_out)
        *filename_out = path - strlen(component);

    if (err < 0)
        return ERR_PTR(err);
    return dirent;
}
static struct tmp_dirent *tmpfs_lookup(struct mount *mount, const char *path) {
    return __tmpfs_lookup(mount, path, false, NULL);
}
static struct tmp_dirent *tmpfs_lookup_parent(struct mount *mount, const char *path, const char **filename_out) {
    if (path[0] == '\0' || strcmp(path, "/") == 0)
        return NULL;
    return __tmpfs_lookup(mount, path, true, filename_out);
}

/* 调用者持有 inode 锁；失败时文件内容和逻辑大小必须保持不变。 */
static int tmpfs_file_resize(struct tmp_inode *file, qword_t requested_size) {
    assert(S_ISREG(file->stat.mode));
    assert(file->stat.size <= SIZE_MAX);
    if (requested_size > SIZE_MAX)
        return _EFBIG;

    size_t size = (size_t) requested_size;
    size_t old_size = (size_t) file->stat.size;
    if (size == old_size)
        return 0;
    if (size == 0) {
        free(file->file_data);
        file->file_data = NULL;
        file->stat.size = 0;
        return 0;
    }
    if (size < old_size) {
        void *new_data = realloc(file->file_data, size);
        if (new_data != NULL)
            file->file_data = new_data;
        file->stat.size = size;
        return 0;
    }

    void *new_data = realloc(file->file_data, size);
    if (new_data == NULL)
        return _ENOMEM;
    if (size > old_size)
        memset((char *) new_data + old_size, 0, size - old_size);
    file->file_data = new_data;
    file->stat.size = size;
    return 0;
}

// ========================
// ======== FS OPS ========
// ========================

extern const struct fd_ops tmpfs_fdops;

static int tmpfs_mount(struct mount *mount) {
    struct tmp_inode *root_inode = tmp_inode_new(S_IFDIR | 0777);
    if (root_inode == NULL)
        return _ENOMEM;

    struct tmp_dirent *root = malloc(sizeof(struct tmp_dirent));
    if (root == NULL) {
        free(root_inode);
        return _ENOMEM;
    }

    tmp_dirent_init(root);
    strcpy(root->name, "");
    root->inode = root_inode;
    root->parent = NULL;

    mount->data = root;
    return 0;
}

#if 0
// This is the only place where a tmpfs directory tree is recursively freed.
static void tmpfs_unmount_tree(struct tmp_inode *tree) {
    assert(refcount_get(tree) == 1); // otherwise mount_remove should have returned EBUSY
    if (S_ISDIR(tree->stat.mode)) {
        struct tmp_dirent *dirent, *tmp;
        list_for_each_entry_safe(&tree->dir.entries, dirent, tmp, dir) {
            if (dirent->inode != NULL)
                tmpfs_unmount_tree(dirent->inode);
            tmp_dirent_release(dirent);
        }
    }
    tmp_inode_release(tree);
}
#endif

static int tmpfs_umount(struct mount *UNUSED(mount)) {
    // big fat fuckin TODO
    // struct tmp_inode *root = mount->data;
    // tmpfs_unmount_tree(root);
    TODO("tmpfs umount");
    return 0;
}

static struct fd *tmpfs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct tmp_dirent *dirent;
    bool opened_created = false;
    if (flags & O_CREAT_) {
        // FIXME: will create a file when given a path that ends with a slash
        const char *filename;
        struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
        if (IS_ERR(parent))
            return ERR_PTR(PTR_ERR(parent));
        if (parent == NULL) {
            if (flags & O_EXCL_)
                return ERR_PTR(_EEXIST);
            dirent = tmpfs_lookup(mount, path);
            goto opened;
        }
        lock(&parent->lock);
        int err = 0;

        dirent = tmpfs_dir_lookup(parent, filename);
        if (flags & O_EXCL_ && !IS_ERR(dirent)) {
            err = _EEXIST;
            goto out_creat;
        }

        if (dirent == ERR_PTR(_ENOENT)) {
            struct tmp_inode *inode = tmp_inode_new(S_IFREG | mode);
            if (inode == NULL) {
                err = _ENOMEM;
                goto out_creat;
            }
            dirent = NULL;
            err = tmpfs_dir_link(parent, filename, inode, &dirent);
            if (err < 0) {
                goto out_creat;
            }
            opened_created = true;
        }

out_creat:
        if (err < 0) {
            if (dirent != NULL && !IS_ERR(dirent))
                tmp_dirent_release(dirent);
            dirent = ERR_PTR(err);
        }
        unlock(&parent->lock);
        tmp_dirent_release(parent);
    } else {
        dirent = tmpfs_lookup(mount, path);
    }
opened:
    if (IS_ERR(dirent))
        return ERR_PTR(PTR_ERR(dirent));

    struct fd *fd = fd_create(&tmpfs_fdops);
    if (fd == NULL) {
        tmp_dirent_release(dirent);
        return ERR_PTR(_ENOMEM);
    }
    fd->tmpfs.dirent = dirent;
    fd->opened_created = opened_created;

    fd->tmpfs.dir_pos = NULL;
    lock(&dirent->lock);
    if (!list_empty(&dirent->children)) {
        tmpfs_fd_seekdir(fd, list_first_entry(&dirent->children, struct tmp_dirent, dir));
    }
    unlock(&dirent->lock);
    return fd;
}

static int tmpfs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    lock(&inode->lock);
    *stat = dirent->inode->stat;
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return 0;
}

static int tmpfs_setattr(struct mount *mount,
        const char *path, struct attr attr) {
    if (attr.type != attr_size)
        return _EPERM;
    if (attr.size < 0)
        return _EINVAL;

    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    lock(&inode->lock);
    int error = S_ISREG(inode->stat.mode) ?
            tmpfs_file_resize(inode, (qword_t) attr.size) :
            (S_ISDIR(inode->stat.mode) ? _EISDIR : _EINVAL);
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return error;
}

static int tmpfs_close(struct fd *fd) {
    // shouldn't need locking as this is the last reference to the fd
    tmpfs_fd_seekdir(fd, NULL);
    tmp_dirent_release(fd->tmpfs.dirent);
    fd->tmpfs.dirent = NULL;
    return 0;
}

static int tmpfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EEXIST;
    lock(&parent->lock);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(S_IFDIR | mode);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;

    err = tmpfs_dir_link(parent, filename, inode, NULL);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_mknod_identity(struct mount *mount, const char *path,
        mode_t_ mode, dev_t_ dev, qword_t *host_device,
        qword_t *host_inode, qword_t *inode_number) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(
            mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EEXIST;
    lock(&parent->lock);
    int error = tmpfs_dir_lookup_existence(parent, filename);
    if (error < 0)
        goto out;
    struct tmp_inode *inode = tmp_inode_new(mode);
    if (inode == NULL) {
        error = _ENOMEM;
        goto out;
    }
    inode->stat.rdev = dev;
    *host_device = inode->stat.inode_device;
    *host_inode = inode->stat.inode;
    *inode_number = inode->stat.inode;
    error = tmpfs_dir_link(parent, filename, inode, NULL);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return error;
}

static int tmpfs_mknod(struct mount *mount, const char *path,
        mode_t_ mode, dev_t_ dev) {
    qword_t host_device;
    qword_t host_inode;
    qword_t inode;
    return tmpfs_mknod_identity(mount, path, mode, dev,
            &host_device, &host_inode, &inode);
}

static int tmpfs_unlink_common(struct mount *mount, const char *path,
        bool match_identity, qword_t host_device,
        qword_t host_inode, qword_t inode_number) {
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        if (!match_identity)
            return _EISDIR;
        struct tmp_dirent *root = mount->data;
        struct tmp_inode *inode = root->inode;
        lock(&inode->lock);
        bool matches = inode->stat.inode_device == host_device &&
                inode->stat.inode == host_inode &&
                inode->stat.inode == inode_number;
        unlock(&inode->lock);
        return matches ? _EISDIR : 0;
    }
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(
            mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    assert(parent != NULL);
    lock(&parent->lock);
    struct tmp_dirent *dirent = tmpfs_dir_lookup(parent, filename);
    if (IS_ERR(dirent)) {
        int error = PTR_ERR(dirent);
        unlock(&parent->lock);
        tmp_dirent_release(parent);
        return match_identity && error == _ENOENT ? 0 : error;
    }
    struct tmp_inode *inode = dirent->inode;
    lock(&inode->lock);
    bool matches = inode->stat.inode_device == host_device &&
            inode->stat.inode == host_inode &&
            inode->stat.inode == inode_number;
    bool directory = S_ISDIR(inode->stat.mode);
    unlock(&inode->lock);
    if (match_identity && !matches) {
        tmp_dirent_release(dirent);
        unlock(&parent->lock);
        tmp_dirent_release(parent);
        return 0;
    }
    if (directory) {
        tmp_dirent_release(dirent);
        unlock(&parent->lock);
        tmp_dirent_release(parent);
        return _EISDIR;
    }
    list_remove(&dirent->dir);
    unlock(&parent->lock);
    // 依次释放目录树所有权与本次 lookup 的临时引用。
    tmp_dirent_release(dirent);
    tmp_dirent_release(dirent);
    tmp_dirent_release(parent);
    return 0;
}

static int tmpfs_unlink(struct mount *mount, const char *path) {
    return tmpfs_unlink_common(mount, path, false, 0, 0, 0);
}

static int tmpfs_unlink_if_identity(struct mount *mount, const char *path,
        qword_t host_device, qword_t host_inode, qword_t inode) {
    return tmpfs_unlink_common(mount, path, true,
            host_device, host_inode, inode);
}

// ========================
// ======== FD OPS ========
// ========================

static struct tmp_inode *tmpfs_fd_inode(struct fd *fd) {
    return fd->tmpfs.dirent->inode;
}

static int tmpfs_getpath(struct fd *fd, char *buf) {
    struct tmp_dirent *dirent = fd->tmpfs.dirent;
    struct tmp_dirent *root_dirent = fd->mount->data;
    char *p = buf + MAX_PATH - 1;
    *p = '\0';
    while (dirent != root_dirent) {
        size_t name_len = strlen(dirent->name);
        p -= name_len + 1;
        if (p < buf)
            return _ENAMETOOLONG;
        p[0] = '/';
        memcpy(&p[1], dirent->name, name_len);
    }
    memmove(buf, p, strlen(p) + 1);
    return 0;
}

static int tmpfs_fstat(struct fd *fd, struct statbuf *stat) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock);
    *stat = inode->stat;
    unlock(&inode->lock);
    return 0;
}

static int tmpfs_fsetattr(struct fd *fd, struct attr attr) {
    if (attr.type != attr_size)
        return _EPERM;
    if (attr.size < 0)
        return _EINVAL;

    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock);
    int error = S_ISREG(inode->stat.mode) ?
            tmpfs_file_resize(inode, (qword_t) attr.size) : _EINVAL;
    unlock(&inode->lock);
    return error;
}

static ssize_t tmpfs_read_at_locked(struct tmp_inode *inode,
        void *buf, size_t bufsize, qword_t offset) {
    if (S_ISDIR(inode->stat.mode))
        return _EISDIR;
    assert(S_ISREG(inode->stat.mode));
    if (offset >= inode->stat.size) {
        bufsize = 0;
    } else {
        qword_t remaining = inode->stat.size - offset;
        if (remaining < bufsize)
            bufsize = (size_t) remaining;
    }
    if (bufsize != 0)
        memcpy(buf, inode->file_data + (size_t) offset, bufsize);
    return (ssize_t) bufsize;
}

static ssize_t tmpfs_read(struct fd *fd, void *buf, size_t bufsize) {
    if (fd->offset < 0)
        return _EINVAL;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock);
    ssize_t res = tmpfs_read_at_locked(
            inode, buf, bufsize, (qword_t) fd->offset);
    if (res > 0)
        fd->offset += (off_t_) res;
    unlock(&inode->lock);
    return res;
}

static ssize_t tmpfs_pread(struct fd *fd, void *buf,
        size_t bufsize, off_t offset) {
    if (offset < 0)
        return _EINVAL;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock);
    ssize_t res = tmpfs_read_at_locked(
            inode, buf, bufsize, (qword_t) offset);
    unlock(&inode->lock);
    return res;
}

static ssize_t tmpfs_write_at_locked(struct tmp_inode *inode,
        const void *buf, size_t bufsize, qword_t offset) {
    assert(S_ISREG(inode->stat.mode));
    if (bufsize == 0)
        return 0;
    if (offset > SIZE_MAX)
        return _EFBIG;
    size_t host_offset = (size_t) offset;
    size_t new_size;
    if (__builtin_add_overflow(host_offset, bufsize, &new_size))
        return _EFBIG;
#if SIZE_MAX > INT64_MAX
    if (new_size > INT64_MAX)
        return _EFBIG;
#endif
    if (inode->stat.size < new_size) {
        int error = tmpfs_file_resize(inode, new_size);
        if (error < 0)
            return error;
    }
    memcpy(inode->file_data + host_offset, buf, bufsize);
    return (ssize_t) bufsize;
}

static ssize_t tmpfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    qword_t offset;
    if ((fd->flags & O_APPEND_) != 0) {
        offset = inode->stat.size;
    } else if (fd->offset < 0) {
        res = _EFBIG;
        goto out;
    } else {
        offset = (qword_t) fd->offset;
    }
    res = tmpfs_write_at_locked(inode, buf, bufsize, offset);
    if (res > 0)
        fd->offset = (off_t_) (offset + (qword_t) res);

out:
    unlock(&inode->lock);
    return res;
}

static ssize_t tmpfs_pwrite_common(struct fd *fd,
        const void *buf, size_t bufsize, off_t offset,
        bool honor_append) {
    if (offset < 0)
        return _EINVAL;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock);
    ssize_t res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));
    qword_t positioned = honor_append && (fd->flags & O_APPEND_) != 0 ?
            inode->stat.size : (qword_t) offset;
    res = tmpfs_write_at_locked(inode, buf, bufsize, positioned);
out:
    unlock(&inode->lock);
    return res;
}

static ssize_t tmpfs_pwrite(struct fd *fd,
        const void *buf, size_t bufsize, off_t offset) {
    return tmpfs_pwrite_common(fd, buf, bufsize, offset, true);
}

static ssize_t tmpfs_page_pwrite(struct fd *fd,
        const void *buf, size_t bufsize, off_t offset) {
    return tmpfs_pwrite_common(fd, buf, bufsize, offset, false);
}

static off_t_ tmpfs_lseek(struct fd *fd, off_t_ off, int whence) {
    off_t_ size = 0;
    if (whence == LSEEK_END) {
        struct tmp_inode *inode = tmpfs_fd_inode(fd);
        lock(&inode->lock);
        assert(inode->stat.size <= INT64_MAX);
        size = (off_t_) inode->stat.size;
        unlock(&inode->lock);
    }

    int err = generic_seek(fd, off, whence, size);
    if (err < 0)
        return err;

    return fd->offset;
}

static int tmpfs_fsync(struct fd *fd) {
    use(fd);
    return 0;
}

static int tmpfs_readdir(struct fd *fd, struct dir_entry *entry) {
    struct tmp_dirent *parent = fd->tmpfs.dirent;
    if (!S_ISDIR(parent->inode->stat.mode))
        return _ENOTDIR;

    lock(&parent->lock);
    int res;
    struct tmp_dirent *dirent = fd->tmpfs.dir_pos;
    if (dirent == NULL) {
        res = 0;
        goto out;
    }
    if (list_null(&dirent->dir)) {
        off_t_ removed_index = dirent->index;
        dirent = NULL;
        struct tmp_dirent *candidate;
        list_for_each_entry(&parent->children, candidate, dir) {
            if (candidate->index > removed_index) {
                dirent = candidate;
                break;
            }
        }
        if (dirent == NULL) {
            tmpfs_fd_seekdir(fd, NULL);
            res = 0;
            goto out;
        }
    }
    struct tmp_dirent *next_dirent = list_next_entry(dirent, dir);
    if (&next_dirent->dir == &parent->children) // end of list
        next_dirent = NULL;
    entry->inode = dirent->inode->stat.inode;
    strcpy(entry->name, dirent->name);
    tmpfs_fd_seekdir(fd, next_dirent);
    res = 1;

out:
    unlock(&parent->lock);
    return res;
}

static off_t_ tmpfs_telldir(struct fd *fd) {
    if (fd->tmpfs.dir_pos == NULL)
        return INT64_MAX;
    return fd->tmpfs.dir_pos->index;
}

static void tmpfs_seekdir(struct fd *fd, off_t_ ptr) {
    struct tmp_dirent *dir = fd->tmpfs.dirent;
    lock(&dir->lock);
    assert(S_ISDIR(dir->inode->stat.mode));
    struct tmp_dirent *child;
    list_for_each_entry(&dir->children, child, dir) {
        if (child->index >= ptr)
            break;
    }
    if (&child->dir == &dir->children)
        child = NULL;
    tmpfs_fd_seekdir(fd, child);
    unlock(&dir->lock);
}

const struct fs_ops tmpfs = {
    .name = "tmpfs", .magic = 0x01021994,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .getpath = tmpfs_getpath,
    .unlink = tmpfs_unlink,
    .mknod = tmpfs_mknod,
    .mknod_identity = tmpfs_mknod_identity,
    .unlink_if_identity = tmpfs_unlink_if_identity,
    .mkdir = tmpfs_mkdir,
};

const struct fd_ops tmpfs_fdops = {
    .page_cacheable = true,
    .read = tmpfs_read,
    .pread = tmpfs_pread,
    .write = tmpfs_write,
    .pwrite = tmpfs_pwrite,
    .page_pwrite = tmpfs_page_pwrite,
    .lseek = tmpfs_lseek,
    .fsync = tmpfs_fsync,
    .fdatasync = tmpfs_fsync,
    .readdir = tmpfs_readdir,
    .telldir = tmpfs_telldir,
    .seekdir = tmpfs_seekdir,
};

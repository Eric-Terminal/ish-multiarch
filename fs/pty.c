#include <string.h>
#include <sys/stat.h>
#include "kernel/task.h"
#include "kernel/errno.h"
#include "fs/tty.h"
#include "fs/devices.h"

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

#define MAX_PTYS (1 << 12)

// 调用方必须持有 ttys_lock。
static int pty_reserve_next_locked(void) {
    for (int pty_num = 0; pty_num < MAX_PTYS; pty_num++) {
        if (pty_slave.ttys[pty_num] == NULL &&
                !pty_slave.reserved[pty_num]) {
            pty_slave.reserved[pty_num] = true;
            return pty_num;
        }
    }
    return _ENOSPC;
}

// 调用方必须持有 ttys_lock，且只能发布完整初始化的对象。
static void pty_publish_locked(int pty_num, struct tty *tty) {
    assert(pty_num >= 0 && pty_num < MAX_PTYS);
    assert(pty_slave.reserved[pty_num]);
    assert(pty_slave.ttys[pty_num] == NULL);
    pty_slave.ttys[pty_num] = tty;
    pty_slave.reserved[pty_num] = false;
}

// 调用方必须持有 ttys_lock。
static void pty_cancel_locked(int pty_num) {
    assert(pty_num >= 0 && pty_num < MAX_PTYS);
    assert(pty_slave.reserved[pty_num]);
    assert(pty_slave.ttys[pty_num] == NULL);
    pty_slave.reserved[pty_num] = false;
}

// the master holds a reference to the slave, so the slave will always be cleaned up second
// when the master cleans up it hangs up the slave, making any operation that references the master unreachable

static void pty_slave_init_inode(struct tty *tty) {
    tty->pty.uid = current->euid;
    // TODO make these mount options
    tty->pty.gid = current->egid;
    tty->pty.perms = 0620;
    tty->pty.visible = true;
}

static int pty_master_init(struct tty *tty) {
    tty->termios.iflags = 0;
    tty->termios.oflags = 0;
    tty->termios.lflags = 0;

    struct tty *slave = tty_alloc(&pty_slave, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (slave == NULL)
        return _ENOMEM;
    slave->refcount = 1;
    tty->pty.other = slave;
    slave->pty.other = tty;
    slave->pty.locked = true;
    pty_slave_init_inode(slave);
    pty_publish_locked(tty->num, slave);
    return 0;
}


static void pty_hangup(struct tty *tty) {
    if (tty == NULL)
        return;
    lock(&tty->lock);
    tty_hangup(tty);
    unlock(&tty->lock);
}

static struct tty *pty_hangup_other(struct tty *tty) {
    assert(lock_owned_by_current(&ttys_lock));
    lock(&tty->lock);
    struct tty *other = tty->pty.other;
    unlock(&tty->lock);
    if (other == NULL)
        return NULL;
    pty_hangup(other);
    return other;
}

static void pty_slave_cleanup(struct tty *tty) {
    pty_hangup_other(tty);
}

static void pty_master_cleanup(struct tty *tty) {
    struct tty *slave = pty_hangup_other(tty);
    assert(slave != NULL);
    lock(&slave->lock);
    slave->pty.visible = false;
    slave->pty.other = NULL;
    unlock(&slave->lock);
    tty_release(slave);
}

static int pty_slave_open(struct tty *tty) {
    lock(&ttys_lock);
    lock(&tty->lock);
    struct tty *master = tty->pty.other;
    int err = master == NULL || tty->pty.locked ? _EIO : 0;
    if (err == 0) {
        lock(&master->lock);
        bool paired = pty_slave.ttys[tty->num] == tty &&
                pty_master.ttys[tty->num] == master &&
                master->pty.other == tty &&
                master->open_count > 0 &&
                !master->hung_up && !tty->hung_up &&
                tty->pty.visible;
        if (!paired)
            err = _EIO;
        unlock(&master->lock);
    }
    unlock(&tty->lock);
    unlock(&ttys_lock);
    return err;
}

static int pty_slave_close(struct tty *tty) {
    lock(&tty->lock);
    bool half_closed = tty->ever_opened && tty->open_count == 0;
    struct tty *master = tty->pty.other;
    unlock(&tty->lock);
    if (half_closed && master != NULL) {
        // Linux 允许 master 在 slave 重开前继续排队写入；这里只唤醒读端观察 EIO/HUP。
        lock(&master->lock);
        tty_notify_peer_closed(master);
        unlock(&master->lock);
    }
    return 0;
}

static int pty_master_close(struct tty *tty) {
    lock(&tty->lock);
    bool hangup = tty->ever_opened && tty->open_count == 0;
    if (hangup)
        tty_hangup(tty);
    unlock(&tty->lock);
    if (hangup) {
        struct tty *slave = pty_hangup_other(tty);
        if (slave != NULL) {
            lock(&slave->lock);
            slave->pty.visible = false;
            unlock(&slave->lock);
        }
    }
    return 0;
}

static int pty_master_ioctl(struct tty *tty, int cmd, void *arg) {
    struct tty *slave = tty->pty.other;
    switch (cmd) {
        case TIOCSPTLCK_: {
            // ioctl 入口持有 master 锁；先放开它以保持 slave→master 的唯一嵌套顺序。
            unlock(&tty->lock);
            lock(&slave->lock);
            slave->pty.locked = !!*(dword_t *) arg;
            unlock(&slave->lock);
            lock(&tty->lock);
            break;
        }
        case TIOCGPTN_:
            *(dword_t *) arg = slave->num;
            break;
        case TIOCPKT_:
            tty->pty.packet_mode = !!*(dword_t *) arg;
            break;
        case TIOCGPKT_:
            *(dword_t *) arg = tty->pty.packet_mode;
            break;
        default:
            return _ENOTTY;
    }
    return 0;
}

static int pty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    struct tty *other;
    bool release_other = false;
    if (tty->driver == &pty_slave) {
        // slave 不拥有 master；按槽位取得临时引用，不能把 slave 锁带进可能
        // 阻塞的 tty_input，否则 master poll 会反向等待 slave 锁。
        assert(!lock_owned_by_current(&tty->lock));
        other = tty_get(
                &pty_master, TTY_PSEUDO_MASTER_MAJOR, tty->num);
        if (!IS_ERR(other))
            release_other = true;
    } else {
        // master 的生命周期引用始终保住 slave，反向指针不会提前失效。
        other = tty->pty.other;
    }

    int result = other == NULL || IS_ERR(other) ? _EIO :
            (int) tty_input(other, buf, len, blocking);
    if (release_other) {
        lock(&ttys_lock);
        tty_release(other);
        unlock(&ttys_lock);
    }
    return result;
}

static int pty_return_eio(struct tty *UNUSED(tty)) {
    return _EIO;
}

const struct tty_driver_ops pty_master_ops = {
    .init = pty_master_init,
    .open = pty_return_eio,
    .close = pty_master_close,
    .write = pty_write,
    .ioctl = pty_master_ioctl,
    .cleanup = pty_master_cleanup,
};
DEFINE_TTY_DRIVER(pty_master, &pty_master_ops, TTY_PSEUDO_MASTER_MAJOR, MAX_PTYS);

const struct tty_driver_ops pty_slave_ops = {
    .init = pty_return_eio,
    .open = pty_slave_open,
    .close = pty_slave_close,
    .write = pty_write,
    .cleanup = pty_slave_cleanup,
};
DEFINE_TTY_DRIVER(pty_slave, &pty_slave_ops, TTY_PSEUDO_SLAVE_MAJOR, MAX_PTYS);

int ptmx_open(struct fd *fd) {
    lock(&ttys_lock);
    int pty_num = pty_reserve_next_locked();
    if (pty_num < 0) {
        unlock(&ttys_lock);
        return pty_num;
    }

    struct tty *master = tty_alloc(
            &pty_master, TTY_PSEUDO_MASTER_MAJOR, pty_num);
    if (master == NULL) {
        pty_cancel_locked(pty_num);
        unlock(&ttys_lock);
        return _ENOMEM;
    }
    int err = pty_master_init(master);
    if (err < 0) {
        tty_destroy_unpublished(master);
        pty_cancel_locked(pty_num);
        unlock(&ttys_lock);
        return err;
    }
    master->refcount = 1;
    master->open_count = 1;
    master->ever_opened = true;
    assert(pty_master.ttys[pty_num] == NULL);
    pty_master.ttys[pty_num] = master;
    unlock(&ttys_lock);
    return tty_open(master, fd);
}

struct tty *pty_open_fake(struct tty_driver *driver) {
    if (driver == NULL || driver->ops == NULL)
        return ERR_PTR(_EINVAL);
    lock(&ttys_lock);
    if (driver->ttys == NULL) {
        driver->ttys = pty_slave.ttys;
        driver->reserved = pty_slave.reserved;
        driver->limit = pty_slave.limit;
        driver->major = TTY_PSEUDO_SLAVE_MAJOR;
    } else if (driver->ttys != pty_slave.ttys ||
            driver->reserved != pty_slave.reserved ||
            driver->limit != pty_slave.limit ||
            driver->major != TTY_PSEUDO_SLAVE_MAJOR) {
        unlock(&ttys_lock);
        return ERR_PTR(_EINVAL);
    }

    int pty_num = pty_reserve_next_locked();
    if (pty_num < 0) {
        unlock(&ttys_lock);
        return ERR_PTR(pty_num);
    }

    struct tty *tty = tty_alloc(driver, TTY_PSEUDO_SLAVE_MAJOR, pty_num);
    if (tty == NULL) {
        pty_cancel_locked(pty_num);
        unlock(&ttys_lock);
        return ERR_PTR(_ENOMEM);
    }
    // init 可能把对象交给主线程；创建者引用必须在此之前成立。
    tty->refcount = 1;
    tty->ever_opened = true;
    pty_slave_init_inode(tty);
    if (driver->ops->init) {
        int err = driver->ops->init(tty);
        if (err < 0) {
            tty_destroy_unpublished(tty);
            pty_cancel_locked(pty_num);
            unlock(&ttys_lock);
            return ERR_PTR(err);
        }
    }
    pty_publish_locked(pty_num, tty);
    unlock(&ttys_lock);
    return tty;
}

static const struct fd_ops devpts_fdops;

static bool devpts_pty_exists(int pty_num) {
    if (pty_num < 0 || pty_num >= MAX_PTYS)
        return false;
    lock(&ttys_lock);
    struct tty *tty = pty_slave.ttys[pty_num];
    bool exists = false;
    if (tty != NULL) {
        lock(&tty->lock);
        exists = tty->pty.visible;
        unlock(&tty->lock);
    }
    unlock(&ttys_lock);
    return exists;
}

// this has a slightly weird error returning convention
// I'm lucky that ENOENT is -2 and not -1
static int devpts_get_pty_num(const char *path) {
    if (strcmp(path, "") == 0)
        return -1; // root
    if (path[0] != '/' || path[1] == '\0' || strchr(path + 1, '/') != NULL)
        return _ENOENT;

    int pty_num = 0;
    for (const unsigned char *digit = (const unsigned char *) path + 1;
            *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9')
            return _ENOENT;
        int value = *digit - '0';
        if (pty_num > (MAX_PTYS - 1 - value) / 10)
            return _ENOENT;
        pty_num = pty_num * 10 + value;
    }
    if (!devpts_pty_exists(pty_num))
        return _ENOENT;
    return pty_num;
}

static struct fd *devpts_open(struct mount *UNUSED(mount), const char *path, int UNUSED(flags), int UNUSED(mode)) {
    int pty_num = devpts_get_pty_num(path);
    if (pty_num == _ENOENT)
        return ERR_PTR(_ENOENT);
    struct fd *fd = fd_create(&devpts_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    fd->devpts.num = pty_num;
    return fd;
}

static int devpts_getpath(struct fd *fd, char *buf) {
    if (fd->devpts.num == -1)
        strcpy(buf, "");
    else
        sprintf(buf, "/%d", fd->devpts.num);
    return 0;
}

static int devpts_stat_num(int pty_num, struct statbuf *stat) {
    if (pty_num == -1) {
        // root
        stat->mode = S_IFDIR | 0755;
        stat->inode = 1;
        return 0;
    } else {
        if (pty_num < 0 || pty_num >= MAX_PTYS)
            return _ENOENT;
        lock(&ttys_lock);
        struct tty *tty = pty_slave.ttys[pty_num];
        if (tty == NULL) {
            unlock(&ttys_lock);
            return _ENOENT;
        }
        lock(&tty->lock);
        if (!tty->pty.visible) {
            unlock(&tty->lock);
            unlock(&ttys_lock);
            return _ENOENT;
        }

        stat->mode = S_IFCHR | tty->pty.perms;
        stat->uid = tty->pty.uid;
        stat->gid = tty->pty.gid;
        stat->inode = pty_num + 3;
        stat->rdev = dev_make(TTY_PSEUDO_SLAVE_MAJOR, pty_num);

        unlock(&tty->lock);
        unlock(&ttys_lock);
        return 0;
    }
}

static int devpts_setattr_num(int pty_num, struct attr attr) {
    if (pty_num == -1)
        return _EROFS;
    if (pty_num < 0 || pty_num >= MAX_PTYS)
        return _ENOENT;
    if (attr.type == attr_size)
        return _EINVAL;

    lock(&ttys_lock);
    struct tty *tty = pty_slave.ttys[pty_num];
    if (tty == NULL) {
        unlock(&ttys_lock);
        return _ENOENT;
    }
    lock(&tty->lock);
    if (!tty->pty.visible) {
        unlock(&tty->lock);
        unlock(&ttys_lock);
        return _ENOENT;
    }

    switch (attr.type) {
        case attr_uid:
            tty->pty.uid = attr.uid;
            break;
        case attr_gid:
            tty->pty.gid = attr.gid;
            break;
        case attr_mode:
            tty->pty.perms = attr.mode;
            break;
        case attr_size:
            __builtin_unreachable();
    }

    unlock(&tty->lock);
    unlock(&ttys_lock);
    return 0;
}

static int devpts_fstat(struct fd *fd, struct statbuf *stat) {
    return devpts_stat_num(fd->devpts.num, stat);
}

static int devpts_stat(struct mount *UNUSED(mount), const char *path, struct statbuf *stat) {
    int pty_num = devpts_get_pty_num(path);
    if (pty_num == _ENOENT)
        return _ENOENT;
    return devpts_stat_num(pty_num, stat);
}

static int devpts_setattr(struct mount *UNUSED(mount), const char *path, struct attr attr) {
    int pty_num = devpts_get_pty_num(path);
    if (pty_num == _ENOENT)
        return _ENOENT;
    return devpts_setattr_num(pty_num, attr);
}

static int devpts_fsetattr(struct fd *fd, struct attr attr) {
    return devpts_setattr_num(fd->devpts.num, attr);
}

static int devpts_readdir(struct fd *fd, struct dir_entry *entry) {
    assert(fd->devpts.num == -1); // there shouldn't be anything to list but the root

    if (fd->offset < 0 || fd->offset >= MAX_PTYS)
        return 0;
    int pty_num = (int) fd->offset;
    while (pty_num < MAX_PTYS && !devpts_pty_exists(pty_num))
        pty_num++;
    if (pty_num >= MAX_PTYS)
        return 0;
    fd->offset = pty_num + 1;
    sprintf(entry->name, "%d", pty_num);
    entry->inode = pty_num + 3;
    return 1;
}

const struct fs_ops devptsfs = {
    .name = "devpts", .magic = 0x1cd1,
    .open = devpts_open,
    .getpath = devpts_getpath,
    .stat = devpts_stat,
    .fstat = devpts_fstat,
    .setattr = devpts_setattr,
    .fsetattr = devpts_fsetattr,
};

static const struct fd_ops devpts_fdops = {
    .readdir = devpts_readdir,
};

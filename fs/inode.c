#include <stdlib.h>
#include "util/list.h"
#include "kernel/fs.h"
#include "fs/inode.h"
#include "debug.h"

lock_t inodes_lock = LOCK_INITIALIZER;
#define INODES_HASH_SIZE (1 << 10)
static struct list inodes_hash[INODES_HASH_SIZE];
static qword_t last_futex_sequence;

int current_pid(void);

static size_t inode_hash(qword_t device, ino_t ino) {
    qword_t number = (qword_t) ino;
    qword_t hash = device ^ (device >> 32) ^ number ^ (number >> 32);
    return (size_t) (hash & (INODES_HASH_SIZE - 1));
}

static struct inode_data *inode_get_data(
        struct mount *mount, qword_t device, ino_t ino) {
    size_t index = inode_hash(device, ino);
    if (list_null(&inodes_hash[index]))
        list_init(&inodes_hash[index]);
    struct inode_data *inode;
    list_for_each_entry(&inodes_hash[index], inode, chain) {
        if (inode->mount == mount && inode->device == device &&
                inode->number == ino)
            return inode;
    }
    return NULL;
}

static bool inode_number_is_live(struct mount *mount, ino_t ino) {
    for (size_t index = 0; index < INODES_HASH_SIZE; index++) {
        if (list_null(&inodes_hash[index]))
            continue;
        struct inode_data *inode;
        list_for_each_entry(&inodes_hash[index], inode, chain) {
            if (inode->mount == mount && inode->number == ino)
                return true;
        }
    }
    return false;
}

struct inode_data *inode_get_unlocked(
        struct mount *mount, qword_t device, ino_t ino) {
    struct inode_data *inode = inode_get_data(mount, device, ino);
    if (inode == NULL) {
        inode = malloc(sizeof(struct inode_data));
        if (last_futex_sequence == UINT64_MAX)
            die("inode futex 序列号已耗尽");
        inode->refcount = 0;
        inode->futex_sequence = ++last_futex_sequence;
        inode->device = device;
        inode->number = ino;
        mount_retain(mount);
        inode->mount = mount;
        inode->socket_id = 0;
        inode->file_pager = NULL;
        cond_init(&inode->posix_unlock);
        list_init(&inode->posix_locks);
        list_init(&inode->chain);
        lock_init(&inode->lock);
        list_add(&inodes_hash[inode_hash(device, ino)], &inode->chain);
    }

    inode_retain(inode);
    return inode;
}

struct inode_data *inode_get(
        struct mount *mount, qword_t device, ino_t ino) {
    lock(&inodes_lock);
    struct inode_data *data = inode_get_unlocked(mount, device, ino);
    unlock(&inodes_lock);
    return data;
}

void inode_check_orphaned(struct mount *mount, ino_t ino) {
    lock(&inodes_lock);
    // fakefs 的删除通知只有虚拟 inode 号，任一设备实例存活时都不得回收。
    if (!inode_number_is_live(mount, ino))
        mount->fs->inode_orphaned(mount, ino);
    unlock(&inodes_lock);
}

void inode_retain(struct inode_data *inode) {
    lock(&inode->lock);
    inode->refcount++;
    unlock(&inode->lock);
}

void inode_release(struct inode_data *inode) {
    lock(&inodes_lock);
    lock(&inode->lock);
    if (--inode->refcount == 0) {
        assert(inode->file_pager == NULL);
        unlock(&inode->lock);
        list_remove(&inode->chain);
        if (inode->mount->fs->inode_orphaned &&
                !inode_number_is_live(inode->mount, inode->number))
            inode->mount->fs->inode_orphaned(inode->mount, inode->number);
        unlock(&inodes_lock);
        mount_release(inode->mount);
        free(inode);
    } else {
        unlock(&inode->lock);
        unlock(&inodes_lock);
    }
}

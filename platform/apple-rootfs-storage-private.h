#ifndef PLATFORM_APPLE_ROOTFS_STORAGE_PRIVATE_H
#define PLATFORM_APPLE_ROOTFS_STORAGE_PRIVATE_H

#include <stdbool.h>

// 仅供 Apple rootfs 安装器与目录管理器共享，不属于对外 API。
int ish_apple_rootfs_sync_directory(int directory);
int ish_apple_rootfs_remove_entry_at(int parent, const char *name);

/*
 * runtime 持有共享生命周期锁；复制和删除持有排他锁。
 * 排他请求不会等待活动 runtime，冲突时返回 EBUSY。
 * require_valid_root 供 runtime/复制验证收据；删除可锁住并清理损坏项。
 */
int ish_apple_rootfs_lock_managed_root(
        const char *persistent_parent,
        const char *root_name,
        bool exclusive,
        bool require_valid_root,
        int *lock_out);
int ish_apple_rootfs_unlock_managed_root(int lock);

/*
 * 复制器与 seed 安装器共享同一目标锁、staging 和排他发布协议。
 * 名称语法由调用方的托管 root 目录层继续收紧。
 */
int ish_apple_rootfs_copy_managed_root(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *destination_name);

#endif

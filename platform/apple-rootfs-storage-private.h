#ifndef PLATFORM_APPLE_ROOTFS_STORAGE_PRIVATE_H
#define PLATFORM_APPLE_ROOTFS_STORAGE_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>

#include "tools/fakefs.h"

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

bool ish_apple_rootfs_copy_operation_token_is_valid(const char *token);
// token 查找与目标发布必须串行；成功后用 unlock_managed_root 释放。
int ish_apple_rootfs_lock_copy_catalog(
        const char *persistent_parent,
        int *lock_out);

/*
 * 复制器与 seed 安装器共享同一目标锁、staging 和排他发布协议。
 * 名称语法由调用方的托管 root 目录层继续收紧。
 */
int ish_apple_rootfs_copy_managed_root(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *destination_name);

/*
 * 将 fakefs_import 产生的私有临时目录复制到托管 staging，并原子发布。
 * imported_root 必须是调用方独占、且只含 fakefs 数据库及 data 的普通目录。
 */
int ish_apple_rootfs_publish_imported_root(
        const char *seed_root,
        const char *persistent_parent,
        const char *imported_root,
        const char *destination_name,
        struct progress progress);

/*
 * 在调用方持有复制目录锁及 source_name 排他生命周期锁时执行幂等复制。
 * operation_token、source_name 与 destination_name 会一同写入目标 root。
 */
int ish_apple_rootfs_copy_claimed_managed_root_for_operation(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *destination_name,
        const char *operation_token);

/*
 * 查找已经发布且凭据完整匹配的复制操作。
 * 同一 token 出现在其他 source 或多个目标时返回 EEXIST。
 * 调用方须在整个查找与后续发布期间持有复制目录锁。
 */
int ish_apple_rootfs_find_managed_copy_operation(
        const char *persistent_parent,
        const char *source_name,
        const char *operation_token,
        char *destination_name,
        size_t destination_capacity,
        bool *found);

#endif

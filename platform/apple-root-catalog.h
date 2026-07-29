#ifndef PLATFORM_APPLE_ROOT_CATALOG_H
#define PLATFORM_APPLE_ROOT_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#include "platform/apple-rootfs-seed.h"

#define ISH_APPLE_ROOT_NAME_CAPACITY 64
#define ISH_APPLE_ROOT_PATH_CAPACITY 1024

struct ish_apple_root_entry {
    char name[ISH_APPLE_ROOT_NAME_CAPACITY];
};

// 托管名称仅包含 aarch64、aarch64-2、aarch64-3……。
bool ish_apple_root_catalog_is_managed_name(const char *name);

// 安装事务使用的 staging、lock 和 owner 名称不会作为普通 root 暴露。
bool ish_apple_root_catalog_is_private_name(const char *name);

/*
 * 枚举带有效安装收据的托管 AArch64 root，并按编号自然排序。
 * entries 可为 NULL；此时 capacity 必须为 0，count 返回所需条目数。
 * 缓冲区不足返回 ERANGE，同时 count 仍返回所需条目数。
 */
int ish_apple_root_catalog_list(
        const char *seed_root,
        const char *persistent_parent,
        struct ish_apple_root_entry *entries,
        size_t capacity,
        size_t *count);

/*
 * 准备本次启动使用的 root。preferred_name 有效时优先复用或重建该名称；
 * 否则复用编号最低的托管 root，没有可复用项时安装最低空闲名称。
 */
int ish_apple_root_catalog_prepare(
        const char *seed_root,
        const char *persistent_parent,
        const char *preferred_name,
        char active_name[ISH_APPLE_ROOT_NAME_CAPACITY],
        enum ish_apple_rootfs_seed_result *result);

/*
 * 在 runtime 使用 root 的整个生命周期持有共享 claim。
 * 成功后必须把 claim_file 交给 release_active；复制和删除会与它互斥。
 */
int ish_apple_root_catalog_claim_active(
        const char *persistent_parent,
        const char *name,
        int *claim_file);
int ish_apple_root_catalog_release_active(int claim_file);

// 始终安装一个新的托管 root，选择最低的未占用名称。
int ish_apple_root_catalog_create(
        const char *seed_root,
        const char *persistent_parent,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY]);

/*
 * 复制一个非活动的托管 root，并选择最低空闲名称原子发布。
 * active_name 必须是有效托管名称；source_name 与活动 root 相同时返回 EBUSY。
 * final 名称及父目录同步完成后才视为成功；owner 收尾可由后续操作续清理。
 */
int ish_apple_root_catalog_copy(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *active_name,
        char destination_name[ISH_APPLE_ROOT_NAME_CAPACITY]);

/*
 * 先在同一父目录原子隐藏，再安全递归删除；中断后会在后续目录操作中续清理。
 * active_name 与 selected_name 用于保护本次运行和下次启动使用的 root。
 */
int ish_apple_root_catalog_delete(
        const char *persistent_parent,
        const char *name,
        const char *active_name,
        const char *selected_name);

// 根据父目录和已校验名称生成 fakefs data 路径。
int ish_apple_root_catalog_data_path(
        const char *persistent_parent,
        const char *name,
        char *path,
        size_t capacity);

#endif

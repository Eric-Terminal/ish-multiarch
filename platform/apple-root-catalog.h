#ifndef PLATFORM_APPLE_ROOT_CATALOG_H
#define PLATFORM_APPLE_ROOT_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#include "platform/apple-rootfs-seed.h"

#define ISH_APPLE_ROOT_NAME_CAPACITY 64

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

// 始终安装一个新的托管 root，选择最低的未占用名称。
int ish_apple_root_catalog_create(
        const char *seed_root,
        const char *persistent_parent,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY]);

// 根据父目录和已校验名称生成 fakefs data 路径。
int ish_apple_root_catalog_data_path(
        const char *persistent_parent,
        const char *name,
        char *path,
        size_t capacity);

#endif

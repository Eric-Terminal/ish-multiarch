#ifndef PLATFORM_APPLE_ROOTFS_SEED_H
#define PLATFORM_APPLE_ROOTFS_SEED_H

enum ish_apple_rootfs_seed_result {
    ISH_APPLE_ROOTFS_SEED_INSTALLED,
    ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT,
};

// seed_root 应指向签名 bundle 中的只读种子，persistent_parent 应位于应用私有容器。
// 返回 0 表示成功并写入 result；失败时返回正数 POSIX errno，不挂载或启动 guest。
// 已存在且带有效 receipt 的 root 只会被复用，不会因 seed 更新而覆盖。
int ish_apple_rootfs_seed_install(
        const char *seed_root,
        const char *persistent_parent,
        const char *root_name,
        enum ish_apple_rootfs_seed_result *result);

#endif

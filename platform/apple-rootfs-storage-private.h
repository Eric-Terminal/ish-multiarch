#ifndef PLATFORM_APPLE_ROOTFS_STORAGE_PRIVATE_H
#define PLATFORM_APPLE_ROOTFS_STORAGE_PRIVATE_H

// 仅供 Apple rootfs 安装器与目录管理器共享，不属于对外 API。
int ish_apple_rootfs_sync_directory(int directory);
int ish_apple_rootfs_remove_entry_at(int parent, const char *name);

#endif

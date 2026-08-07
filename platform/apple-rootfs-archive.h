#ifndef PLATFORM_APPLE_ROOTFS_ARCHIVE_H
#define PLATFORM_APPLE_ROOTFS_ARCHIVE_H

#include "platform/apple-rootfs-seed.h"
#include "sdk/iSHApple/Headers/iSHAppleRootFS.h"

#pragma GCC visibility push(hidden)

// 返回正 POSIX errno；公共包装负责转为负 Linux errno。
int ish_apple_rootfs_archive_install(
        const struct ish_apple_rootfs_archive_spec_v1 *spec,
        const struct ish_apple_rootfs_archive_callbacks_v1 *callbacks,
        enum ish_apple_rootfs_seed_result *result);

#pragma GCC visibility pop

#endif

#ifndef ISH_APPLE_ROOTFS_H
#define ISH_APPLE_ROOTFS_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_ROOTFS_INSTALL_RESULT_INSTALLED INT32_C(0)
#define ISH_APPLE_ROOTFS_INSTALL_RESULT_ALREADY_PRESENT INT32_C(1)

ISH_APPLE_EXTERN_C_BEGIN

/*
 * 把签名 bundle 中的只读 seed 安装到应用私有目录。所有返回值统一为 0 或
 * 负 Linux errno；成功时 disposition_out 写入上述两种结果之一。
 */
ISH_APPLE_API int32_t ish_apple_rootfs_install_seed(
        const char *ISH_APPLE_NONNULL seed_root,
        const char *ISH_APPLE_NONNULL persistent_parent,
        const char *ISH_APPLE_NONNULL root_name,
        int32_t *ISH_APPLE_NONNULL disposition_out);

ISH_APPLE_EXTERN_C_END

#endif

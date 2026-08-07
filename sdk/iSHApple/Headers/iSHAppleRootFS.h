#ifndef ISH_APPLE_ROOTFS_H
#define ISH_APPLE_ROOTFS_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_ROOTFS_INSTALL_RESULT_INSTALLED INT32_C(0)
#define ISH_APPLE_ROOTFS_INSTALL_RESULT_ALREADY_PRESENT INT32_C(1)

#define ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VERIFY UINT32_C(1)
#define ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT UINT32_C(2)
#define ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VALIDATE_SEED UINT32_C(3)
#define ISH_APPLE_ROOTFS_ARCHIVE_PHASE_PUBLISH UINT32_C(4)
#define ISH_APPLE_ROOTFS_ARCHIVE_PHASE_COMPLETE UINT32_C(5)

#define ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_PATH_TRUNCATED UINT32_C(1)

#define ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CONTINUE INT32_C(0)
#define ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CANCEL INT32_C(1)

#define ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_PATH_BYTES UINT32_C(512)

struct ish_apple_rootfs_archive_spec_v1 {
    uint32_t version;
    uint32_t structure_size;
    const char *ISH_APPLE_NONNULL archive_path;
    const char *ISH_APPLE_NONNULL expected_sha256;
    const char *ISH_APPLE_NONNULL persistent_parent;
    const char *ISH_APPLE_NONNULL root_name;
    uint64_t expected_uncompressed_bytes;
    uint64_t expected_entry_count;
    uint64_t reserved[2];
};

struct ish_apple_rootfs_archive_progress_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint32_t phase;
    uint32_t flags;
    uint64_t compressed_bytes_completed;
    uint64_t compressed_bytes_total;
    uint64_t extracted_bytes_completed;
    uint64_t extracted_bytes_total;
    uint64_t entries_completed;
    uint64_t entries_total;
    char current_path[ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_PATH_BYTES];
    uint64_t reserved[4];
};

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

/*
 * progress 返回非零会在下一个安全检查点取消安装。回调只在调用线程同步执行，
 * progress 指针及 current_path 仅在本次回调返回前有效。
 */
typedef int32_t (*ish_apple_rootfs_archive_progress_callback)(
        void *ISH_APPLE_NULLABLE context,
        const struct ish_apple_rootfs_archive_progress_v1
                *ISH_APPLE_NONNULL progress);

struct ish_apple_rootfs_archive_callbacks_v1 {
    uint32_t version;
    uint32_t structure_size;
    void *ISH_APPLE_NULLABLE context;
    ish_apple_rootfs_archive_progress_callback
            ISH_APPLE_NULLABLE progress;
    uint64_t reserved[2];
};

/*
 * 安装由 apple-rootfs-seed-archive.py 生成的 gzip/USTAR seed。函数先校验
 * 整个压缩文件的 SHA-256，再在托管 staging 中安全解包、验证 AArch64
 * manifest/fakefs/hardlink，最后原子发布；不会联网获取或补装任何文件。
 */
ISH_APPLE_API int32_t ish_apple_rootfs_install_archive(
        const struct ish_apple_rootfs_archive_spec_v1
                *ISH_APPLE_NONNULL spec,
        const struct ish_apple_rootfs_archive_callbacks_v1
                *ISH_APPLE_NULLABLE callbacks,
        int32_t *ISH_APPLE_NONNULL disposition_out);

ISH_APPLE_EXTERN_C_END

#endif

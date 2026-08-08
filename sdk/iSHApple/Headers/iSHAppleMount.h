#ifndef ISH_APPLE_MOUNT_H
#define ISH_APPLE_MOUNT_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_MOUNT_ACCESS_READ_ONLY 1
#define ISH_APPLE_MOUNT_ACCESS_READ_WRITE 2

#define ISH_APPLE_MOUNT_STATE_STAGED 1
#define ISH_APPLE_MOUNT_STATE_ACTIVE 2
#define ISH_APPLE_MOUNT_STATE_DRAINING 3
#define ISH_APPLE_MOUNT_STATE_REMOVED 4

#define ISH_APPLE_MOUNT_REMOVE_FORCE 1U
#define ISH_APPLE_MOUNT_GUEST_DIRECTORY_BYTES_MAX 4095U

ISH_APPLE_EXTERN_C_BEGIN

struct ish_apple_mount_id {
    uint64_t high;
    uint64_t low;
};

struct ish_apple_mount_spec_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t reserved[2];
    struct ish_apple_mount_id mount_id;
    int32_t access;
    int32_t host_directory_fd;
    const char *ISH_APPLE_NONNULL guest_directory;
};

struct ish_apple_mount_info_v1 {
    uint32_t version;
    uint32_t structure_size;
    struct ish_apple_mount_id mount_id;
    int32_t access;
    int32_t state;
    uint64_t active_leases;
    uint64_t active_references;
    uint32_t guest_directory_bytes;
    uint32_t reserved_0;
    uint64_t reserved[2];
};

typedef struct ish_apple_mount_lease ish_apple_mount_lease;

/*
 * guest_directory 必须位于 /mnt/ 下。SDK 复制已打开的目录 fd，调用方
 * 可在函数返回后关闭原 fd；宿主绝对路径不会进入 guest 或公共状态。
 */
ISH_APPLE_API int32_t ish_apple_mount_add(
        const struct ish_apple_mount_spec_v1 *ISH_APPLE_NONNULL spec);

/*
 * 普通移除先拒绝新 lease，并在 lease 或内核引用仍活跃时返回 EBUSY。
 * FORCE 忽略宿主 lease，但不会破坏仍被 guest fd/cwd 引用的 mount；此时
 * 同样返回 EBUSY，调用方应取消相关作业后重试。
 */
ISH_APPLE_API int32_t ish_apple_mount_remove(
        struct ish_apple_mount_id mount_id,
        uint32_t flags);

/*
 * count_out 始终返回当前条目总数。entries 为 NULL 且 capacity 为 0 可用于
 * 查询数量；容量不足时填满可用部分并返回 ENOSPC。
 */
ISH_APPLE_API int32_t ish_apple_mount_list(
        struct ish_apple_mount_info_v1 *ISH_APPLE_NULLABLE entries,
        uint32_t capacity,
        uint32_t *ISH_APPLE_NONNULL count_out);

/* required_bytes_out 包含结尾 NUL；buffer 为 NULL 时只查询长度。 */
ISH_APPLE_API int32_t ish_apple_mount_copy_guest_directory(
        struct ish_apple_mount_id mount_id,
        char *ISH_APPLE_NULLABLE buffer,
        uint32_t capacity,
        uint32_t *ISH_APPLE_NONNULL required_bytes_out);

ISH_APPLE_API int32_t ish_apple_mount_lease_acquire(
        struct ish_apple_mount_id mount_id,
        ish_apple_mount_lease *ISH_APPLE_NULLABLE *ISH_APPLE_NONNULL lease_out);
ISH_APPLE_API void ish_apple_mount_lease_retain(
        ish_apple_mount_lease *ISH_APPLE_NONNULL lease);
ISH_APPLE_API void ish_apple_mount_lease_release(
        ish_apple_mount_lease *ISH_APPLE_NONNULL lease);

ISH_APPLE_EXTERN_C_END

#endif

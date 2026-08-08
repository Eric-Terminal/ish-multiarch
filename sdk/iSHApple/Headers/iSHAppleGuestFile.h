#ifndef ISH_APPLE_GUEST_FILE_H
#define ISH_APPLE_GUEST_FILE_H

#include "iSHAppleDefines.h"

#define ISH_APPLE_GUEST_FILE_PATH_BYTES_MAX 4095U
#define ISH_APPLE_GUEST_FILE_NAME_BYTES_MAX 255U

#define ISH_APPLE_GUEST_FILE_REQUEST_NOFOLLOW 1U

#define ISH_APPLE_GUEST_FILE_REMOVE_RECURSIVE 1U
#define ISH_APPLE_GUEST_FILE_MKDIR_PARENTS 1U

ISH_APPLE_EXTERN_C_BEGIN

/*
 * request_id 用于把不兼容文件系统错误归属到一次 Agent 工具调用，必须非零。
 * path 必须是 guest 中的绝对路径；SDK 不接受宿主路径，也不会绕过 fakefs。
 */
struct ish_apple_guest_file_request_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint32_t flags;
    uint32_t reserved_0;
    uint64_t request_id;
    uint64_t reserved[2];
    const char *ISH_APPLE_NONNULL path;
};

/* 固定宽度的 Linux stat 快照；时间字段使用 guest 秒与纳秒值。 */
struct ish_apple_guest_file_info_v1 {
    uint32_t version;
    uint32_t structure_size;
    uint64_t request_id;
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint32_t mode;
    uint32_t link_count;
    uint32_t user_id;
    uint32_t group_id;
    uint32_t block_size;
    uint32_t reserved_0;
    int64_t access_time_seconds;
    int64_t modification_time_seconds;
    int64_t status_change_time_seconds;
    uint32_t access_time_nanoseconds;
    uint32_t modification_time_nanoseconds;
    uint32_t status_change_time_nanoseconds;
    uint32_t reserved_1;
    uint64_t reserved[2];
};

struct ish_apple_guest_file_directory_entry_v1 {
    struct ish_apple_guest_file_info_v1 info;
    uint32_t name_bytes;
    uint32_t reserved_0;
    char name[ISH_APPLE_GUEST_FILE_NAME_BYTES_MAX + 1];
};

ISH_APPLE_API int32_t ish_apple_guest_file_stat(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        struct ish_apple_guest_file_info_v1 *ISH_APPLE_NONNULL info_out);

/*
 * cursor 是目录 provider 的不透明位置，首次传 0。每页返回下一位置与 eof；
 * 目录发生并发修改时，与 Linux readdir/telldir 一样不保证快照一致性。
 */
ISH_APPLE_API int32_t ish_apple_guest_file_list(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        uint64_t cursor,
        struct ish_apple_guest_file_directory_entry_v1 *ISH_APPLE_NONNULL entries,
        uint32_t capacity,
        uint32_t *ISH_APPLE_NONNULL count_out,
        uint64_t *ISH_APPLE_NONNULL next_cursor_out,
        int32_t *ISH_APPLE_NONNULL eof_out);

/* offset 与 total_size 均保留 64 位；单次复制长度使用跨 ABI 固定的 uint32_t。 */
ISH_APPLE_API int32_t ish_apple_guest_file_read(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        uint64_t offset,
        void *ISH_APPLE_NULLABLE bytes,
        uint32_t capacity,
        uint32_t *ISH_APPLE_NONNULL count_out,
        uint64_t *ISH_APPLE_NONNULL total_size_out,
        int32_t *ISH_APPLE_NONNULL eof_out);

/*
 * write 原子替换完整普通文件；已存在文件保留 mode/uid/gid，新文件使用 mode。
 * bytes 先写入同目录临时文件并 fsync，成功 rename 前目标保持不变。
 */
ISH_APPLE_API int32_t ish_apple_guest_file_write(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        const void *ISH_APPLE_NULLABLE bytes,
        uint32_t length,
        uint32_t mode);

/* edit 流式复制未修改区间，不会把原文件整体载入宿主内存。 */
ISH_APPLE_API int32_t ish_apple_guest_file_edit(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        uint64_t offset,
        uint64_t removed_length,
        const void *ISH_APPLE_NULLABLE replacement,
        uint32_t replacement_length);

/* 递归删除 "/" 会清空 guest 根目录内容，但保留根目录命名空间锚点。 */
ISH_APPLE_API int32_t ish_apple_guest_file_remove(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        uint32_t flags);
ISH_APPLE_API int32_t ish_apple_guest_file_rename(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        const char *ISH_APPLE_NONNULL destination);
ISH_APPLE_API int32_t ish_apple_guest_file_mkdir(
        const struct ish_apple_guest_file_request_v1 *ISH_APPLE_NONNULL request,
        uint32_t mode,
        uint32_t flags);

ISH_APPLE_EXTERN_C_END

#endif

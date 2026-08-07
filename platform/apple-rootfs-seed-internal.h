#ifndef PLATFORM_APPLE_ROOTFS_SEED_INTERNAL_H
#define PLATFORM_APPLE_ROOTFS_SEED_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sqlite3.h>

#include "tools/fakefs.h"

#ifndef __APPLE__
#error "Apple rootfs seed 安装器只能构建到 Apple 平台"
#endif

#define ROOT_NAME_LIMIT 128
#define MANIFEST_LIMIT (64 * 1024)
#define HARDLINK_MANIFEST_LIMIT (16 * 1024 * 1024)
#define COPY_BUFFER_SIZE (16 * 1024)
#define COPY_TREE_DEPTH_LIMIT 256
#define REMOVE_TREE_DEPTH_LIMIT 512
#define OWNER_TOKEN_BYTES 16
#define OWNER_TOKEN_HEX_LENGTH (OWNER_TOKEN_BYTES * 2)
#define OWNER_RECORD_LIMIT 512
#define COPY_OPERATION_TOKEN_LIMIT 128
#define COPY_OPERATION_RECORD_LIMIT 512

#pragma GCC visibility push(hidden)

extern const char ish_apple_rootfs_manifest_name[
        sizeof("rootfs-manifest.txt")];
extern const char ish_apple_rootfs_hardlink_manifest_name[
        sizeof("rootfs-hardlinks.tsv")];
extern const char ish_apple_rootfs_install_receipt_name[
        sizeof("rootfs-installation.txt")];
extern const char ish_apple_rootfs_copy_operation_marker_prefix[
        sizeof(".ish-copy-operation.")];
extern const char ish_apple_rootfs_owner_format[
        sizeof("format=ish-rootfs-install-owner-v2")];
extern const char ish_apple_rootfs_receipt_format[
        sizeof("format=ish-rootfs-install-v1\n")];
extern const char ish_apple_rootfs_copy_operation_format[
        sizeof("format=ish-rootfs-copy-operation-v1")];

struct seed_manifest {
    char archive_sha256[65];
};

struct hardlink_entry {
    char *canonical;
    char *member;
    sqlite3_int64 database_inode;
};

struct hardlink_manifest {
    char *storage;
    struct hardlink_entry *entries;
    size_t count;
};

struct relative_parent {
    int directory;
    char leaf[NAME_MAX + 1];
};

struct copied_hardlink {
    dev_t device;
    ino_t inode;
    char *destination_path;
};

struct root_copy_context {
    int destination_root;
    struct copied_hardlink *hardlinks;
    size_t hardlink_count;
    size_t hardlink_capacity;
    struct progress progress;
    uintmax_t total_bytes;
    uintmax_t copied_bytes;
    double progress_span;
};

struct staging_owner {
    char staging_name[NAME_MAX + 1];
    uintmax_t staging_device;
    uintmax_t staging_inode;
    uintmax_t marker_device;
    uintmax_t marker_inode;
};

enum owner_state {
    OWNER_MISSING,
    OWNER_VALID,
    OWNER_UNKNOWN,
};

enum ish_apple_rootfs_seed_test_phase {
    ISH_APPLE_ROOTFS_SEED_TEST_NONE,
    ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC,
    ISH_APPLE_ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC,
    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC,
    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_UNLINK,
    ISH_APPLE_ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC,
};

#ifdef ISH_APPLE_ROOTFS_SEED_TESTING
extern int ish_apple_rootfs_seed_test_fail_phase;
extern int ish_apple_rootfs_seed_test_force_sparse_fallback;
extern size_t ish_apple_rootfs_seed_test_write_limit;
extern unsigned ish_apple_rootfs_seed_test_sparse_fallback_count;
extern unsigned ish_apple_rootfs_seed_test_limited_write_count;
#endif

int ish_apple_rootfs_errno_or_io(void);
int ish_apple_rootfs_sync_directory_internal(int directory);
int ish_apple_rootfs_sync_directory_phase(int directory, int phase);
int ish_apple_rootfs_sqlite_error(sqlite3 *database);
bool ish_apple_rootfs_name_is_valid(const char *name);
int ish_apple_rootfs_format_copy_operation_marker_name(
        char output[NAME_MAX + 1], const char *operation_token);
int ish_apple_rootfs_format_private_name(
        char output[NAME_MAX + 1], const char *root_name,
        const char *suffix);

int ish_apple_rootfs_report_root_copy_progress(
        struct root_copy_context *context,
        uintmax_t copied,
        const char *path);
int ish_apple_rootfs_report_copy_stage(
        struct progress progress,
        double fraction,
        const char *message);
int ish_apple_rootfs_create_regular_at(
        int directory, const char *name,
        const void *bytes, size_t length, bool synchronize);
int ish_apple_rootfs_read_regular_at(
        int directory, const char *name, size_t limit,
        char **bytes_out, size_t *length_out);
int ish_apple_rootfs_copy_regular_at(
        int source_directory, int destination_directory,
        const char *name);
int ish_apple_rootfs_copy_directory_contents(
        int source, int destination, unsigned depth);
void ish_apple_rootfs_copy_context_destroy(
        struct root_copy_context *context);
void ish_apple_rootfs_stat_times(
        const struct stat *metadata,
        struct timespec times[2]);
int ish_apple_rootfs_copy_managed_root_contents(
        int source,
        int destination,
        const char *destination_path,
        unsigned depth,
        struct root_copy_context *context);

int ish_apple_rootfs_take_line(
        char **cursor, char *end, char **line, size_t *length);
bool ish_apple_rootfs_line_equals(
        const char *line, size_t length, const char *expected);
int ish_apple_rootfs_format_copy_operation_record(
        char output[COPY_OPERATION_RECORD_LIMIT + 1],
        const char *source_name,
        const char *destination_name,
        const char *operation_token);
bool ish_apple_rootfs_sha256_is_valid(
        const char *digest, size_t length);
int ish_apple_rootfs_validate_seed_top(
        int seed, struct seed_manifest *manifest);
bool ish_apple_rootfs_relative_path_is_valid(const char *path);
int ish_apple_rootfs_open_relative_parent(
        int root, const char *path, struct relative_parent *parent);
int ish_apple_rootfs_close_relative_parent(
        struct relative_parent *parent, int error);
int ish_apple_rootfs_open_regular_relative(
        int root, const char *path, int flags, int *file_out);
void ish_apple_rootfs_hardlink_manifest_destroy(
        struct hardlink_manifest *manifest);
int ish_apple_rootfs_parse_hardlink_manifest(
        char *bytes, size_t length, struct hardlink_manifest *manifest);

int ish_apple_rootfs_validate_busybox_elf(int data_directory);
int ish_apple_rootfs_validate_and_update_database(
        int staging_directory,
        int data_directory,
        struct hardlink_manifest *hardlinks);
int ish_apple_rootfs_prepare_copied_database(int staging);

int ish_apple_rootfs_write_receipt_at(
        int directory, const struct seed_manifest *manifest);
int ish_apple_rootfs_validate_opened_root(int root);
int ish_apple_rootfs_open_existing_root(
        int parent, const char *root_name,
        bool *present, int *root_out);
int ish_apple_rootfs_inspect_existing_root(
        int parent, const char *root_name, bool *present);
int ish_apple_rootfs_write_copy_operation_marker(
        int root,
        const char *source_name,
        const char *destination_name,
        const char *operation_token);
int ish_apple_rootfs_inspect_copy_operation_marker(
        int root,
        const char *source_name,
        const char *destination_name,
        const char *operation_token,
        bool *present,
        bool *matches);
int ish_apple_rootfs_verify_named_root_identity(
        int parent, const char *root_name, int root);
int ish_apple_rootfs_entry_metadata(
        int parent, const char *name,
        bool *exists, struct stat *metadata);
int ish_apple_rootfs_unlink_owner_marker(
        int parent, const char *owner_name,
        const struct staging_owner *owner, int sync_phase);
int ish_apple_rootfs_remove_owned_staging(
        int parent, const char *owner_name,
        const struct staging_owner *owner);
int ish_apple_rootfs_recover_staging(
        int parent, const char *owner_name, const char *root_name);
int ish_apple_rootfs_cleanup_staging_if_owned(
        int parent, const char *owner_name, const char *root_name);
int ish_apple_rootfs_create_staging(
        int parent, const char *root_name, const char *owner_name,
        int *staging_out, struct staging_owner *owner_out);
int ish_apple_rootfs_rollback_unsynchronized_publish(
        int parent, const char *root_name,
        const struct staging_owner *owner);

int ish_apple_rootfs_build_staging_root(int seed, int staging);
int ish_apple_rootfs_finalize_seed_staging(
        int staging, struct seed_manifest *manifest_out);
int ish_apple_rootfs_build_copied_root(
        int source,
        int staging,
        const char *source_name,
        const char *destination_name,
        const char *operation_token);
int ish_apple_rootfs_build_imported_root(
        int seed, int imported, int staging,
        struct progress progress);

#pragma GCC visibility pop

#endif

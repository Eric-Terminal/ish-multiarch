#include "platform/apple-rootfs-archive.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <zlib.h>

#include "platform/apple-rootfs-seed-internal.h"
#include "platform/apple-rootfs-sha256.h"

#define TAR_BLOCK_BYTES 512u
#define ARCHIVE_IO_BYTES (64u * 1024u)

struct archive_install_context {
    const struct ish_apple_rootfs_archive_spec_v1 *spec;
    const struct ish_apple_rootfs_archive_callbacks_v1 *callbacks;
    uint64_t compressed_total;
    uint64_t compressed_completed;
    uint64_t extracted_completed;
    uint64_t entries_completed;
    bool builder_invoked;
};

static int report_progress(
        struct archive_install_context *context,
        uint32_t phase,
        const char *path,
        bool cancellable) {
    if (context->callbacks == NULL ||
            context->callbacks->progress == NULL)
        return 0;
    struct ish_apple_rootfs_archive_progress_v1 progress = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = (uint32_t) sizeof(progress),
        .phase = phase,
        .compressed_bytes_completed = context->compressed_completed,
        .compressed_bytes_total = context->compressed_total,
        .extracted_bytes_completed = context->extracted_completed,
        .extracted_bytes_total = context->spec->expected_uncompressed_bytes,
        .entries_completed = context->entries_completed,
        .entries_total = context->spec->expected_entry_count,
    };
    if (path != NULL) {
        int count = snprintf(progress.current_path,
                sizeof(progress.current_path), "%s", path);
        if (count < 0)
            return EIO;
        if ((size_t) count >= sizeof(progress.current_path))
            progress.flags |=
                    ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_PATH_TRUNCATED;
    }
    int32_t decision = context->callbacks->progress(
            context->callbacks->context, &progress);
    return cancellable && decision !=
            ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CONTINUE ? ECANCELED : 0;
}

static bool same_source_metadata(
        const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev &&
            left->st_ino == right->st_ino &&
            left->st_size == right->st_size &&
            left->st_mtimespec.tv_sec == right->st_mtimespec.tv_sec &&
            left->st_mtimespec.tv_nsec == right->st_mtimespec.tv_nsec;
}

static int verify_archive(
        int archive,
        const struct stat *expected_metadata,
        const char expected_sha256[65],
        struct archive_install_context *context) {
    struct ish_apple_rootfs_sha256 sha256;
    ish_apple_rootfs_sha256_initialize(&sha256);
    unsigned char bytes[ARCHIVE_IO_BYTES];
    int error = report_progress(
            context, ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VERIFY, NULL, true);
    while (error == 0) {
        ssize_t count = read(archive, bytes, sizeof(bytes));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = ish_apple_rootfs_errno_or_io();
        } else if (count == 0) {
            break;
        } else {
            ish_apple_rootfs_sha256_update(
                    &sha256, bytes, (size_t) count);
            context->compressed_completed += (uint64_t) count;
            error = report_progress(context,
                    ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VERIFY,
                    NULL, true);
        }
    }

    unsigned char digest[32];
    char hexadecimal[65];
    ish_apple_rootfs_sha256_finish(&sha256, digest);
    ish_apple_rootfs_sha256_hex(digest, hexadecimal);
    unsigned difference = 0;
    for (size_t index = 0; index < 64; index++)
        difference |= (unsigned char) hexadecimal[index] ^
                (unsigned char) expected_sha256[index];
    if (error == 0 && difference != 0)
        error = EBADMSG;

    struct stat verified_metadata;
    if (error == 0 && fstat(archive, &verified_metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && !same_source_metadata(
            expected_metadata, &verified_metadata))
        error = EAGAIN;
    if (error == 0 && lseek(archive, 0, SEEK_SET) < 0)
        error = ish_apple_rootfs_errno_or_io();
    return error;
}

static int gzip_error(gzFile stream) {
    int status = Z_OK;
    (void) gzerror(stream, &status);
    if (status == Z_ERRNO)
        return ish_apple_rootfs_errno_or_io();
    if (status == Z_MEM_ERROR)
        return ENOMEM;
    if (status == Z_DATA_ERROR)
        return EBADMSG;
    return EIO;
}

static void update_compressed_progress(
        gzFile stream, struct archive_install_context *context) {
    z_off_t offset = gzoffset(stream);
    if (offset >= 0) {
        uint64_t value = (uint64_t) offset;
        context->compressed_completed = value > context->compressed_total ?
                context->compressed_total : value;
    }
}

static int read_gzip_exact(
        gzFile stream,
        void *bytes,
        size_t length,
        struct archive_install_context *context,
        const char *path,
        bool payload) {
    unsigned char *cursor = bytes;
    while (length != 0) {
        unsigned request = length > UINT_MAX ? UINT_MAX : (unsigned) length;
        int count = gzread(stream, cursor, request);
        if (count < 0)
            return gzip_error(stream);
        if (count == 0)
            return EBADMSG;
        cursor += (size_t) count;
        length -= (size_t) count;
        update_compressed_progress(stream, context);
        if (payload) {
            if (UINT64_MAX - context->extracted_completed <
                    (uint64_t) count)
                return EFBIG;
            context->extracted_completed += (uint64_t) count;
        }
        int error = report_progress(context,
                ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT,
                path, true);
        if (error != 0)
            return error;
    }
    return 0;
}

static bool zero_block(const unsigned char block[TAR_BLOCK_BYTES]) {
    for (size_t index = 0; index < TAR_BLOCK_BYTES; index++) {
        if (block[index] != 0)
            return false;
    }
    return true;
}

static int parse_octal(
        const unsigned char *bytes, size_t length, uint64_t *value_out) {
    size_t index = 0;
    while (index < length && bytes[index] == ' ')
        index++;
    if (index == length)
        return EBADMSG;
    uint64_t value = 0;
    bool found = false;
    while (index < length && bytes[index] >= '0' && bytes[index] <= '7') {
        if (value > (UINT64_MAX - 7) / 8)
            return EFBIG;
        value = value * 8 + (uint64_t) (bytes[index] - '0');
        found = true;
        index++;
    }
    while (index < length &&
            (bytes[index] == '\0' || bytes[index] == ' '))
        index++;
    if (!found || index != length)
        return EBADMSG;
    *value_out = value;
    return 0;
}

static int verify_tar_header(
        const unsigned char header[TAR_BLOCK_BYTES]) {
    if (memcmp(header + 257, "ustar\0", 6) != 0 ||
            memcmp(header + 263, "00", 2) != 0)
        return EBADMSG;
    uint64_t expected;
    int error = parse_octal(header + 148, 8, &expected);
    unsigned sum = 0;
    for (size_t index = 0; index < TAR_BLOCK_BYTES; index++)
        sum += index >= 148 && index < 156 ?
                (unsigned) ' ' : (unsigned) header[index];
    return error == 0 && expected == sum ? 0 : EBADMSG;
}

static int tar_text_length(
        const unsigned char *bytes, size_t capacity, size_t *length_out) {
    size_t length = 0;
    while (length < capacity && bytes[length] != '\0')
        length++;
    for (size_t index = length; index < capacity; index++) {
        if (bytes[index] != '\0')
            return EBADMSG;
    }
    *length_out = length;
    return 0;
}

static int tar_path(
        const unsigned char header[TAR_BLOCK_BYTES],
        bool directory,
        char path[PATH_MAX + 1]) {
    size_t name_length;
    size_t prefix_length;
    int error = tar_text_length(header, 100, &name_length);
    if (error == 0)
        error = tar_text_length(header + 345, 155, &prefix_length);
    if (error != 0 || name_length == 0)
        return error == 0 ? EBADMSG : error;
    size_t length = name_length + (prefix_length == 0 ? 0 : prefix_length + 1);
    if (length > PATH_MAX)
        return ENAMETOOLONG;
    size_t offset = 0;
    if (prefix_length != 0) {
        memcpy(path, header + 345, prefix_length);
        offset = prefix_length;
        path[offset++] = '/';
    }
    memcpy(path + offset, header, name_length);
    path[length] = '\0';
    if (directory && length != 0 && path[length - 1] == '/')
        path[--length] = '\0';
    if (!ish_apple_rootfs_relative_path_is_valid(path))
        return EBADMSG;
    unsigned depth = 0;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' && ++depth > COPY_TREE_DEPTH_LIMIT)
            return ELOOP;
    }
    if (strcmp(path, "meta.db") != 0 &&
            strcmp(path, ish_apple_rootfs_manifest_name) != 0 &&
            strcmp(path, ish_apple_rootfs_hardlink_manifest_name) != 0 &&
            strcmp(path, "data") != 0 &&
            strncmp(path, "data/", 5) != 0)
        return EBADMSG;
    return 0;
}

static int close_parent(
        struct relative_parent *parent, int error, bool synchronize) {
    if (synchronize && error == 0)
        error = ish_apple_rootfs_sync_directory_internal(parent->directory);
    return ish_apple_rootfs_close_relative_parent(parent, error);
}

static int create_directory(int staging, const char *path) {
    struct relative_parent parent = {.directory = -1};
    int error = ish_apple_rootfs_open_relative_parent(
            staging, path, &parent);
    if (error == 0 && mkdirat(parent.directory, parent.leaf, 0700) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (parent.directory >= 0)
        error = close_parent(&parent, error, true);
    return error;
}

static int write_all(int file, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    while (length != 0) {
        ssize_t count = write(file, cursor, length);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return ish_apple_rootfs_errno_or_io();
        }
        if (count == 0)
            return EIO;
        cursor += (size_t) count;
        length -= (size_t) count;
    }
    return 0;
}

static int extract_regular(
        gzFile stream,
        int staging,
        const char *path,
        uint64_t size,
        struct archive_install_context *context) {
    struct relative_parent parent = {.directory = -1};
    int error = ish_apple_rootfs_open_relative_parent(
            staging, path, &parent);
    int file = -1;
    if (error == 0) {
        file = openat(parent.directory, parent.leaf,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
        if (file < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    unsigned char bytes[ARCHIVE_IO_BYTES];
    uint64_t remaining = size;
    while (error == 0 && remaining != 0) {
        size_t count = remaining < sizeof(bytes) ?
                (size_t) remaining : sizeof(bytes);
        error = read_gzip_exact(
                stream, bytes, count, context, path, true);
        if (error == 0)
            error = write_all(file, bytes, count);
        remaining -= error == 0 ? (uint64_t) count : 0;
    }
    if (file >= 0 && error == 0 && fsync(file) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (file >= 0 && close(file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0 && file >= 0)
        (void) unlinkat(parent.directory, parent.leaf, 0);
    if (parent.directory >= 0)
        error = close_parent(&parent, error, true);
    return error;
}

static int discard_padding(
        gzFile stream,
        uint64_t size,
        struct archive_install_context *context) {
    size_t padding = (size_t) ((TAR_BLOCK_BYTES -
            (size % TAR_BLOCK_BYTES)) % TAR_BLOCK_BYTES);
    unsigned char bytes[TAR_BLOCK_BYTES];
    int error = padding == 0 ? 0 : read_gzip_exact(
            stream, bytes, padding, context, NULL, false);
    for (size_t index = 0; error == 0 && index < padding; index++) {
        if (bytes[index] != 0)
            error = EBADMSG;
    }
    return error;
}

static int verify_tar_end(
        gzFile stream, struct archive_install_context *context) {
    unsigned char bytes[TAR_BLOCK_BYTES];
    int count;
    while ((count = gzread(stream, bytes, sizeof(bytes))) > 0) {
        update_compressed_progress(stream, context);
        for (int index = 0; index < count; index++) {
            if (bytes[index] != 0)
                return EBADMSG;
        }
        int error = report_progress(context,
                ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT, NULL, true);
        if (error != 0)
            return error;
    }
    if (count < 0)
        return gzip_error(stream);
    update_compressed_progress(stream, context);
    return gzeof(stream) ? 0 : EBADMSG;
}

static int extract_tar(
        gzFile stream,
        int staging,
        struct archive_install_context *context) {
    unsigned char header[TAR_BLOCK_BYTES];
    unsigned zero_headers = 0;
    int error = report_progress(
            context, ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT, NULL, true);
    while (error == 0 && zero_headers < 2) {
        error = read_gzip_exact(
                stream, header, sizeof(header), context, NULL, false);
        if (error != 0)
            break;
        if (zero_block(header)) {
            zero_headers++;
            continue;
        }
        if (zero_headers != 0) {
            error = EBADMSG;
            break;
        }
        error = verify_tar_header(header);
        bool directory = header[156] == '5';
        bool regular = header[156] == '0' || header[156] == '\0';
        if (error == 0 && !directory && !regular)
            error = EBADMSG;
        char path[PATH_MAX + 1];
        if (error == 0)
            error = tar_path(header, directory, path);
        uint64_t size = 0;
        if (error == 0)
            error = parse_octal(header + 124, 12, &size);
        if (error == 0 && directory && size != 0)
            error = EBADMSG;
        if (error == 0 && context->entries_completed >=
                context->spec->expected_entry_count)
            error = EFBIG;
        if (error == 0 && regular &&
                (size > context->spec->expected_uncompressed_bytes ||
                 context->extracted_completed >
                    context->spec->expected_uncompressed_bytes - size))
            error = EFBIG;
        if (error == 0)
            error = directory ? create_directory(staging, path) :
                    extract_regular(stream, staging, path, size, context);
        if (error == 0 && regular)
            error = discard_padding(stream, size, context);
        if (error == 0) {
            context->entries_completed++;
            error = report_progress(context,
                    ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT,
                    path, true);
        }
    }
    if (error == 0)
        error = verify_tar_end(stream, context);
    if (error == 0 &&
            (context->entries_completed !=
                    context->spec->expected_entry_count ||
             context->extracted_completed !=
                    context->spec->expected_uncompressed_bytes))
        error = EBADMSG;
    return error;
}

static int archive_staging_builder(int staging, void *opaque_context) {
    struct archive_install_context *context = opaque_context;
    context->builder_invoked = true;
    int archive = open(context->spec->archive_path,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (archive < 0)
        return ish_apple_rootfs_errno_or_io();
    struct stat metadata;
    int error = 0;
    if (fstat(archive, &metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    else if (!S_ISREG(metadata.st_mode) || metadata.st_size <= 0)
        error = EINVAL;
    else
        context->compressed_total = (uint64_t) metadata.st_size;
    if (error == 0)
        error = verify_archive(archive, &metadata,
                context->spec->expected_sha256, context);

    int gzip_file = -1;
    gzFile stream = NULL;
    if (error == 0) {
        gzip_file = fcntl(archive, F_DUPFD_CLOEXEC, 0);
        if (gzip_file < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0) {
        stream = gzdopen(gzip_file, "rb");
        if (stream == NULL)
            error = ENOMEM;
        else
            gzip_file = -1;
    }
    context->compressed_completed = 0;
    if (error == 0)
        error = extract_tar(stream, staging, context);
    if (stream != NULL) {
        int close_status = gzclose(stream);
        if (close_status != Z_OK && error == 0)
            error = close_status == Z_ERRNO ?
                    ish_apple_rootfs_errno_or_io() : EIO;
    }
    if (gzip_file >= 0 && close(gzip_file) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    struct stat final_metadata;
    if (error == 0 && fstat(archive, &final_metadata) < 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error == 0 && !same_source_metadata(&metadata, &final_metadata))
        error = EAGAIN;
    if (close(archive) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();

    if (error == 0)
        error = report_progress(context,
                ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VALIDATE_SEED,
                NULL, true);
    if (error == 0)
        error = ish_apple_rootfs_finalize_seed_staging(staging, NULL);
    if (error == 0)
        error = report_progress(context,
                ISH_APPLE_ROOTFS_ARCHIVE_PHASE_PUBLISH,
                NULL, true);
    return error;
}

int ish_apple_rootfs_archive_install(
        const struct ish_apple_rootfs_archive_spec_v1 *spec,
        const struct ish_apple_rootfs_archive_callbacks_v1 *callbacks,
        enum ish_apple_rootfs_seed_result *result) {
    if (spec == NULL || result == NULL || spec->archive_path == NULL ||
            spec->expected_sha256 == NULL ||
            spec->persistent_parent == NULL || spec->root_name == NULL ||
            spec->expected_uncompressed_bytes == 0 ||
            spec->expected_entry_count == 0 ||
            !ish_apple_rootfs_sha256_is_valid(
                    spec->expected_sha256,
                    strnlen(spec->expected_sha256, 65)))
        return EINVAL;
    struct archive_install_context context = {
        .spec = spec,
        .callbacks = callbacks,
    };
    int error = report_progress(
            &context, ISH_APPLE_ROOTFS_ARCHIVE_PHASE_VERIFY, NULL, true);
    if (error == 0)
        error = ish_apple_rootfs_install_with_builder(
                spec->persistent_parent,
                spec->root_name,
                archive_staging_builder,
                &context,
                result);
    if (error == 0) {
        if (context.builder_invoked) {
            context.compressed_completed = context.compressed_total;
            context.extracted_completed = spec->expected_uncompressed_bytes;
            context.entries_completed = spec->expected_entry_count;
        }
        (void) report_progress(&context,
                ISH_APPLE_ROOTFS_ARCHIVE_PHASE_COMPLETE,
                NULL, false);
    }
    return error;
}

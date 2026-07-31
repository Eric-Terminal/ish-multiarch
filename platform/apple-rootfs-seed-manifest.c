#include "platform/apple-rootfs-seed-internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

#include "fs/fake-db.h"
#include "platform/apple-rootfs-storage-private.h"
#include "util/fchdir.h"

// 负责种子清单、硬链接清单与受限相对路径解析。

int ish_apple_rootfs_take_line(
        char **cursor, char *end, char **line, size_t *length) {
    if (*cursor == end)
        return EINVAL;
    char *newline = memchr(*cursor, '\n', (size_t) (end - *cursor));
    if (newline == NULL)
        return EINVAL;
    *line = *cursor;
    *length = (size_t) (newline - *cursor);
    *newline = '\0';
    *cursor = newline + 1;
    return 0;
}

bool ish_apple_rootfs_line_equals(const char *line, size_t length, const char *expected) {
    return strlen(expected) == length && memcmp(line, expected, length) == 0;
}

static bool line_has_nonempty_value(
        const char *line, size_t length, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    return length > prefix_length &&
            memcmp(line, prefix, prefix_length) == 0;
}

int ish_apple_rootfs_format_copy_operation_record(
        char output[COPY_OPERATION_RECORD_LIMIT + 1],
        const char *source_name,
        const char *destination_name,
        const char *operation_token) {
    if (!ish_apple_rootfs_name_is_valid(source_name) ||
            !ish_apple_rootfs_name_is_valid(destination_name) ||
            !ish_apple_rootfs_copy_operation_token_is_valid(
                    operation_token))
        return EINVAL;
    int length = snprintf(output, COPY_OPERATION_RECORD_LIMIT + 1,
            "%s\ntoken=%s\nsource=%s\ndestination=%s\n",
            ish_apple_rootfs_copy_operation_format, operation_token,
            source_name, destination_name);
    return length < 0 || length > COPY_OPERATION_RECORD_LIMIT ?
            EOVERFLOW : 0;
}

bool ish_apple_rootfs_sha256_is_valid(const char *digest, size_t length) {
    if (length != 64)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
                (digest[i] >= 'a' && digest[i] <= 'f')))
            return false;
    }
    return true;
}

static int parse_seed_manifest(
        char *bytes, size_t length, struct seed_manifest *manifest) {
    if (memchr(bytes, '\0', length) != NULL)
        return EINVAL;
    char *cursor = bytes;
    char *end = bytes + length;
    char *line;
    size_t line_length;
    int error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !ish_apple_rootfs_line_equals(line, line_length,
            "format=ish-fakefs-v3"))
        return EINVAL;
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !ish_apple_rootfs_line_equals(line, line_length,
            "packager=apple-aarch64-rootfs-v1"))
        return EINVAL;
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !ish_apple_rootfs_line_equals(line, line_length,
            "guest_arch=aarch64"))
        return EINVAL;
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !ish_apple_rootfs_line_equals(line, line_length,
            "source_kind=official"))
        return EINVAL;
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_has_nonempty_value(line, line_length,
            "alpine_version="))
        return EINVAL;
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    static const char digest_prefix[] = "archive_sha256=";
    size_t digest_prefix_length = sizeof(digest_prefix) - 1;
    if (error != 0 || line_length != digest_prefix_length + 64 ||
            memcmp(line, digest_prefix, digest_prefix_length) != 0 ||
            !ish_apple_rootfs_sha256_is_valid(line + digest_prefix_length, 64))
        return EINVAL;
    memcpy(manifest->archive_sha256, line + digest_prefix_length, 64);
    manifest->archive_sha256[64] = '\0';
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !line_has_nonempty_value(line, line_length,
            "source_url=https://"))
        return EINVAL;
    error = ish_apple_rootfs_take_line(&cursor, end, &line, &line_length);
    if (error != 0 || !ish_apple_rootfs_line_equals(line, line_length,
            "hardlinks=rootfs-hardlinks.tsv") || cursor != end)
        return EINVAL;
    return 0;
}

int ish_apple_rootfs_validate_seed_top(int seed, struct seed_manifest *manifest) {
    unsigned found = 0;
    int iterator_fd = dup(seed);
    if (iterator_fd < 0)
        return ish_apple_rootfs_errno_or_io();
    DIR *iterator = fdopendir(iterator_fd);
    if (iterator == NULL) {
        int error = ish_apple_rootfs_errno_or_io();
        close(iterator_fd);
        return error;
    }

    int error = 0;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
            continue;
        unsigned bit;
        bool expects_directory = false;
        if (strcmp(entry->d_name, "meta.db") == 0) {
            bit = 1u << 0;
        } else if (strcmp(entry->d_name, "data") == 0) {
            bit = 1u << 1;
            expects_directory = true;
        } else if (strcmp(entry->d_name, ish_apple_rootfs_manifest_name) == 0) {
            bit = 1u << 2;
        } else if (strcmp(entry->d_name, ish_apple_rootfs_hardlink_manifest_name) == 0) {
            bit = 1u << 3;
        } else {
            error = EINVAL;
            break;
        }
        if ((found & bit) != 0) {
            error = EINVAL;
            break;
        }
        struct stat metadata;
        if (fstatat(seed, entry->d_name, &metadata,
                AT_SYMLINK_NOFOLLOW) < 0) {
            error = ish_apple_rootfs_errno_or_io();
            break;
        }
        if ((expects_directory && !S_ISDIR(metadata.st_mode)) ||
                (!expects_directory && !S_ISREG(metadata.st_mode))) {
            error = EINVAL;
            break;
        }
        found |= bit;
        errno = 0;
    }
    if (entry == NULL && errno != 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (closedir(iterator) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    if (error != 0)
        return error;
    if (found != 0x0f)
        return EINVAL;

    char *manifest_bytes = NULL;
    size_t manifest_length = 0;
    error = ish_apple_rootfs_read_regular_at(seed, ish_apple_rootfs_manifest_name, MANIFEST_LIMIT,
            &manifest_bytes, &manifest_length);
    if (error == 0)
        error = parse_seed_manifest(manifest_bytes,
                manifest_length, manifest);
    free(manifest_bytes);
    return error;
}

bool ish_apple_rootfs_relative_path_is_valid(const char *path) {
    if (path == NULL || path[0] == '\0' || path[0] == '/')
        return false;
    size_t path_length = strlen(path);
    if (path_length > PATH_MAX || path[path_length - 1] == '/')
        return false;
    const char *component = path;
    while (*component != '\0') {
        const char *separator = strchr(component, '/');
        size_t length = separator == NULL ? strlen(component) :
                (size_t) (separator - component);
        if (length == 0 || length > NAME_MAX ||
                (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.'))
            return false;
        if (separator == NULL)
            break;
        component = separator + 1;
    }
    return true;
}

int ish_apple_rootfs_open_relative_parent(
        int root, const char *path, struct relative_parent *parent) {
    if (!ish_apple_rootfs_relative_path_is_valid(path))
        return EINVAL;
    int directory = dup(root);
    if (directory < 0)
        return ish_apple_rootfs_errno_or_io();

    const char *component = path;
    const char *separator;
    while ((separator = strchr(component, '/')) != NULL) {
        size_t length = (size_t) (separator - component);
        char name[NAME_MAX + 1];
        memcpy(name, component, length);
        name[length] = '\0';
        int child = openat(directory, name,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) {
            int error = ish_apple_rootfs_errno_or_io();
            close(directory);
            return error;
        }
        if (close(directory) < 0) {
            int error = ish_apple_rootfs_errno_or_io();
            close(child);
            return error;
        }
        directory = child;
        component = separator + 1;
    }
    size_t leaf_length = strlen(component);
    memcpy(parent->leaf, component, leaf_length + 1);
    parent->directory = directory;
    return 0;
}

int ish_apple_rootfs_close_relative_parent(
        struct relative_parent *parent, int error) {
    if (close(parent->directory) < 0 && error == 0)
        error = ish_apple_rootfs_errno_or_io();
    parent->directory = -1;
    return error;
}

int ish_apple_rootfs_open_regular_relative(
        int root, const char *path, int flags, int *file_out) {
    struct relative_parent parent = {.directory = -1};
    int error = ish_apple_rootfs_open_relative_parent(root, path, &parent);
    int file = -1;
    if (error == 0) {
        file = openat(parent.directory, parent.leaf,
                flags | O_CLOEXEC | O_NOFOLLOW);
        if (file < 0)
            error = ish_apple_rootfs_errno_or_io();
    }
    if (error == 0) {
        struct stat metadata;
        if (file < 0)
            error = EIO;
        else if (fstat(file, &metadata) < 0)
            error = ish_apple_rootfs_errno_or_io();
        else if (!S_ISREG(metadata.st_mode))
            error = EINVAL;
    }
    if (parent.directory >= 0)
        error = ish_apple_rootfs_close_relative_parent(&parent, error);
    if (error != 0) {
        if (file >= 0)
            close(file);
        return error;
    }
    *file_out = file;
    return 0;
}

static int compare_string_pointers(const void *left, const void *right) {
    const char *const *left_string = left;
    const char *const *right_string = right;
    return strcmp(*left_string, *right_string);
}

void ish_apple_rootfs_hardlink_manifest_destroy(struct hardlink_manifest *manifest) {
    free(manifest->entries);
    free(manifest->storage);
    *manifest = (struct hardlink_manifest) {0};
}

int ish_apple_rootfs_parse_hardlink_manifest(
        char *bytes, size_t length, struct hardlink_manifest *manifest) {
    manifest->storage = bytes;
    if (memchr(bytes, '\0', length) != NULL)
        return EINVAL;
    if (length == 0)
        return 0;
    if (bytes[length - 1] != '\n')
        return EINVAL;
    size_t line_count = 0;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] == '\n') {
            if (line_count == SIZE_MAX / sizeof(*manifest->entries))
                return EOVERFLOW;
            line_count++;
        }
    }
    if (line_count == 0)
        return EINVAL;
    manifest->entries = calloc(line_count, sizeof(*manifest->entries));
    if (manifest->entries == NULL)
        return ENOMEM;

    char *cursor = bytes;
    char *end = bytes + length;
    while (cursor < end) {
        char *newline = memchr(cursor, '\n', (size_t) (end - cursor));
        if (newline == NULL)
            return EINVAL;
        char *tab = memchr(cursor, '\t', (size_t) (newline - cursor));
        if (tab == NULL || memchr(tab + 1, '\t',
                (size_t) (newline - tab - 1)) != NULL)
            return EINVAL;
        *tab = '\0';
        *newline = '\0';
        if (!ish_apple_rootfs_relative_path_is_valid(cursor) ||
                !ish_apple_rootfs_relative_path_is_valid(tab + 1))
            return EINVAL;
        struct hardlink_entry *current =
                &manifest->entries[manifest->count];
        *current = (struct hardlink_entry) {
            .canonical = cursor,
            .member = tab + 1,
        };
        if (manifest->count == 0 || strcmp(current->canonical,
                manifest->entries[manifest->count - 1].canonical) != 0) {
            if (manifest->count != 0 && strcmp(current->canonical,
                    manifest->entries[manifest->count - 1].canonical) <= 0)
                return EINVAL;
            if (strcmp(current->canonical, current->member) != 0)
                return EINVAL;
        } else if (strcmp(current->member,
                manifest->entries[manifest->count - 1].member) <= 0) {
            return EINVAL;
        }
        manifest->count++;
        cursor = newline + 1;
    }
    if (manifest->count != line_count)
        return EINVAL;

    char **members = malloc(manifest->count * sizeof(*members));
    if (members == NULL)
        return ENOMEM;
    for (size_t i = 0; i < manifest->count; i++)
        members[i] = manifest->entries[i].member;
    qsort(members, manifest->count, sizeof(*members), compare_string_pointers);
    int error = 0;
    for (size_t i = 1; i < manifest->count; i++) {
        if (strcmp(members[i - 1], members[i]) == 0) {
            error = EINVAL;
            break;
        }
    }
    free(members);
    return error;
}

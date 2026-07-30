#include "platform/apple-root-catalog.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/apple-rootfs-storage-private.h"

#define DELETION_TOKEN_BYTES 16
#define DELETION_TOKEN_HEX_LENGTH (DELETION_TOKEN_BYTES * 2)

struct root_candidate {
    uint64_t index;
    char name[ISH_APPLE_ROOT_NAME_CAPACITY];
};

static int parse_managed_name(const char *name, uint64_t *index) {
    static const char base[] = "aarch64";
    if (name == NULL)
        return EINVAL;
    if (strcmp(name, base) == 0) {
        if (index != NULL)
            *index = 1;
        return 0;
    }

    size_t base_length = sizeof(base) - 1;
    if (strncmp(name, base, base_length) != 0 ||
            name[base_length] != '-' || name[base_length + 1] == '\0' ||
            name[base_length + 1] == '0')
        return EINVAL;

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(
            name + base_length + 1, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || parsed < 2)
        return EINVAL;
    if (index != NULL)
        *index = parsed;
    return 0;
}

bool ish_apple_root_catalog_is_managed_name(const char *name) {
    return parse_managed_name(name, NULL) == 0;
}

static bool has_private_suffix(const char *name, const char *suffix) {
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    if (name_length <= suffix_length + 1 || name[0] != '.' ||
            strcmp(name + name_length - suffix_length, suffix) != 0)
        return false;

    char root_name[ISH_APPLE_ROOT_NAME_CAPACITY];
    size_t root_length = name_length - suffix_length - 1;
    if (root_length == 0 || root_length >= sizeof(root_name))
        return false;
    memcpy(root_name, name + 1, root_length);
    root_name[root_length] = '\0';
    return ish_apple_root_catalog_is_managed_name(root_name);
}

static bool has_hexadecimal_token(const char *value, size_t length) {
    if (strlen(value) != length)
        return false;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (!((*cursor >= '0' && *cursor <= '9') ||
                (*cursor >= 'a' && *cursor <= 'f')))
            return false;
    }
    return true;
}

static bool is_deletion_name(const char *name) {
    static const char marker[] = ".deleting.";
    if (name == NULL || name[0] != '.')
        return false;
    const char *marker_position = strstr(name + 1, marker);
    if (marker_position == NULL || marker_position == name + 1 ||
            !has_hexadecimal_token(
                marker_position + sizeof(marker) - 1,
                DELETION_TOKEN_HEX_LENGTH))
        return false;

    char root_name[ISH_APPLE_ROOT_NAME_CAPACITY];
    size_t root_length = (size_t) (marker_position - (name + 1));
    if (root_length >= sizeof(root_name))
        return false;
    memcpy(root_name, name + 1, root_length);
    root_name[root_length] = '\0';
    return ish_apple_root_catalog_is_managed_name(root_name);
}

bool ish_apple_root_catalog_is_private_name(const char *name) {
    if (name == NULL || name[0] != '.')
        return false;
    if (has_private_suffix(name, ".install.lock") ||
            has_private_suffix(name, ".lifecycle.lock") ||
            has_private_suffix(name, ".installing.owner") ||
            strcmp(name, ".copy-operation.lock") == 0 ||
            is_deletion_name(name))
        return true;

    static const char marker[] = ".installing.";
    const char *marker_position = strstr(name + 1, marker);
    if (marker_position == NULL || marker_position == name + 1 ||
            strlen(marker_position + sizeof(marker) - 1) != 32)
        return false;

    char root_name[ISH_APPLE_ROOT_NAME_CAPACITY];
    size_t root_length = (size_t) (marker_position - (name + 1));
    if (root_length >= sizeof(root_name))
        return false;
    memcpy(root_name, name + 1, root_length);
    root_name[root_length] = '\0';
    if (!ish_apple_root_catalog_is_managed_name(root_name))
        return false;

    return has_hexadecimal_token(
            marker_position + sizeof(marker) - 1,
            DELETION_TOKEN_HEX_LENGTH);
}

static int open_parent_directory(const char *persistent_parent) {
    int directory = open(persistent_parent,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    return directory < 0 ? -errno : directory;
}

static void cleanup_deletions(const char *persistent_parent) {
    int parent = open_parent_directory(persistent_parent);
    if (parent < 0)
        return;
    int iterator_file = dup(parent);
    if (iterator_file < 0) {
        close(parent);
        return;
    }
    DIR *iterator = fdopendir(iterator_file);
    if (iterator == NULL) {
        close(iterator_file);
        close(parent);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(iterator)) != NULL) {
        if (is_deletion_name(entry->d_name))
            (void) ish_apple_rootfs_remove_entry_at(
                    parent, entry->d_name);
    }
    (void) ish_apple_rootfs_sync_directory(parent);
    closedir(iterator);
    close(parent);
}

static int compare_candidates(const void *left, const void *right) {
    const struct root_candidate *first = left;
    const struct root_candidate *second = right;
    if (first->index < second->index)
        return -1;
    if (first->index > second->index)
        return 1;
    return 0;
}

static int copy_candidate_name(
        uint64_t index,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY]) {
    int length = index == 1 ?
            snprintf(name, ISH_APPLE_ROOT_NAME_CAPACITY, "aarch64") :
            snprintf(name, ISH_APPLE_ROOT_NAME_CAPACITY,
                    "aarch64-%" PRIu64, index);
    return length < 0 || length >= ISH_APPLE_ROOT_NAME_CAPACITY ?
            ENAMETOOLONG : 0;
}

static int collect_candidates(
        const char *persistent_parent,
        struct root_candidate **candidates_out,
        size_t *count_out) {
    *candidates_out = NULL;
    *count_out = 0;
    DIR *directory = opendir(persistent_parent);
    if (directory == NULL)
        return errno;

    struct root_candidate *candidates = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0)
                error = errno;
            break;
        }
        uint64_t index;
        if (parse_managed_name(entry->d_name, &index) != 0)
            continue;
        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 4 : capacity * 2;
            if (next_capacity < capacity ||
                    next_capacity > SIZE_MAX / sizeof(*candidates)) {
                error = ENOMEM;
                break;
            }
            void *grown = realloc(
                    candidates, next_capacity * sizeof(*candidates));
            if (grown == NULL) {
                error = ENOMEM;
                break;
            }
            candidates = grown;
            capacity = next_capacity;
        }
        candidates[count].index = index;
        memcpy(candidates[count].name, entry->d_name,
                strlen(entry->d_name) + 1);
        count++;
    }
    if (closedir(directory) < 0 && error == 0)
        error = errno;
    if (error != 0) {
        free(candidates);
        return error;
    }

    qsort(candidates, count, sizeof(*candidates), compare_candidates);
    *candidates_out = candidates;
    *count_out = count;
    return 0;
}

static int candidate_exists(
        const char *persistent_parent,
        const char *name,
        bool *exists) {
    *exists = false;
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s",
            persistent_parent, name);
    if (length < 0 || (size_t) length >= sizeof(path))
        return ENAMETOOLONG;
    struct stat metadata;
    if (lstat(path, &metadata) == 0) {
        *exists = true;
        return 0;
    }
    if (errno == ENOENT) {
        return 0;
    }
    return errno == 0 ? EIO : errno;
}

static int try_existing_candidate(
        const char *seed_root,
        const char *persistent_parent,
        const char *name,
        enum ish_apple_rootfs_seed_result *result) {
    bool exists;
    int error = candidate_exists(persistent_parent, name, &exists);
    if (error != 0 || !exists)
        return error == 0 ? ENOENT : error;
    return ish_apple_rootfs_seed_install(
            seed_root, persistent_parent, name, result);
}

int ish_apple_root_catalog_list(
        const char *seed_root,
        const char *persistent_parent,
        struct ish_apple_root_entry *entries,
        size_t capacity,
        size_t *count) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            count == NULL || (entries == NULL && capacity != 0))
        return EINVAL;

    cleanup_deletions(persistent_parent);
    struct root_candidate *candidates;
    size_t candidate_count;
    int error = collect_candidates(
            persistent_parent, &candidates, &candidate_count);
    if (error != 0)
        return error;

    size_t valid_count = 0;
    int first_error = 0;
    for (size_t index = 0; index < candidate_count; index++) {
        enum ish_apple_rootfs_seed_result result;
        error = try_existing_candidate(
                seed_root, persistent_parent,
                candidates[index].name, &result);
        if (error != 0) {
            if (error != EEXIST && first_error == 0)
                first_error = error;
            continue;
        }
        if (entries != NULL && valid_count < capacity) {
            memcpy(entries[valid_count].name, candidates[index].name,
                    strlen(candidates[index].name) + 1);
        }
        valid_count++;
    }
    free(candidates);
    *count = valid_count;
    if (valid_count == 0 && first_error != 0)
        return first_error;
    return valid_count > capacity ? ERANGE : 0;
}

static int install_new_candidate(
        const char *seed_root,
        const char *persistent_parent,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY],
        enum ish_apple_rootfs_seed_result *result) {
    for (uint64_t index = 1; ; index++) {
        int error = copy_candidate_name(index, name);
        if (error != 0)
            return error;
        bool exists;
        error = candidate_exists(persistent_parent, name, &exists);
        if (error != 0)
            return error;
        if (exists) {
            if (index == UINT64_MAX)
                return ENOSPC;
            continue;
        }
        error = ish_apple_rootfs_seed_install(
                seed_root, persistent_parent, name, result);
        if (error == EEXIST) {
            if (index == UINT64_MAX)
                return ENOSPC;
            continue;
        }
        if (error == 0 &&
                *result == ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT) {
            if (index == UINT64_MAX)
                return ENOSPC;
            continue;
        }
        return error;
    }
}

int ish_apple_root_catalog_prepare(
        const char *seed_root,
        const char *persistent_parent,
        const char *preferred_name,
        char active_name[ISH_APPLE_ROOT_NAME_CAPACITY],
        enum ish_apple_rootfs_seed_result *result) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            active_name == NULL || result == NULL)
        return EINVAL;

    cleanup_deletions(persistent_parent);
    if (preferred_name != NULL &&
            ish_apple_root_catalog_is_managed_name(preferred_name)) {
        int error = ish_apple_rootfs_seed_install(
                seed_root, persistent_parent, preferred_name, result);
        if (error == 0) {
            memcpy(active_name, preferred_name, strlen(preferred_name) + 1);
            return 0;
        }
        if (error != EEXIST)
            return error;
    }

    struct root_candidate *candidates;
    size_t candidate_count;
    int error = collect_candidates(
            persistent_parent, &candidates, &candidate_count);
    if (error != 0)
        return error;
    int first_error = 0;
    for (size_t index = 0; index < candidate_count; index++) {
        if (preferred_name != NULL &&
                strcmp(preferred_name, candidates[index].name) == 0)
            continue;
        error = try_existing_candidate(
                seed_root, persistent_parent,
                candidates[index].name, result);
        if (error == 0) {
            memcpy(active_name, candidates[index].name,
                    strlen(candidates[index].name) + 1);
            free(candidates);
            return 0;
        }
        if (error != EEXIST && first_error == 0)
            first_error = error;
    }
    free(candidates);
    if (first_error != 0)
        return first_error;
    return install_new_candidate(
            seed_root, persistent_parent, active_name, result);
}

int ish_apple_root_catalog_create(
        const char *seed_root,
        const char *persistent_parent,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY]) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            name == NULL)
        return EINVAL;
    cleanup_deletions(persistent_parent);
    enum ish_apple_rootfs_seed_result result;
    return install_new_candidate(
            seed_root, persistent_parent, name, &result);
}

int ish_apple_root_catalog_import_fakefs(
        const char *seed_root,
        const char *persistent_parent,
        const char *imported_root,
        char name[ISH_APPLE_ROOT_NAME_CAPACITY],
        struct progress progress) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            imported_root == NULL || imported_root[0] == '\0' ||
            name == NULL)
        return EINVAL;

    cleanup_deletions(persistent_parent);
    for (uint64_t index = 1; ; index++) {
        int error = copy_candidate_name(index, name);
        if (error != 0)
            return error;
        bool exists;
        error = candidate_exists(persistent_parent, name, &exists);
        if (error != 0)
            return error;
        if (!exists) {
            error = ish_apple_rootfs_publish_imported_root(
                    seed_root, persistent_parent,
                    imported_root, name, progress);
            if (error != EEXIST)
                return error;
        }
        if (index == UINT64_MAX)
            return ENOSPC;
    }
}

int ish_apple_root_catalog_claim_active(
        const char *persistent_parent,
        const char *name,
        int *claim_file) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_root_catalog_is_managed_name(name) ||
            claim_file == NULL)
        return EINVAL;
    return ish_apple_rootfs_lock_managed_root(
            persistent_parent, name, false, true, claim_file);
}

int ish_apple_root_catalog_release_active(int claim_file) {
    return ish_apple_rootfs_unlock_managed_root(claim_file);
}

int ish_apple_root_catalog_copy(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *active_name,
        char destination_name[ISH_APPLE_ROOT_NAME_CAPACITY]) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_root_catalog_is_managed_name(source_name) ||
            destination_name == NULL)
        return EINVAL;
    bool has_active_name = active_name != NULL && active_name[0] != '\0';
    if (has_active_name &&
            !ish_apple_root_catalog_is_managed_name(active_name))
        return EINVAL;
    if (has_active_name && strcmp(source_name, active_name) == 0)
        return EBUSY;

    cleanup_deletions(persistent_parent);
    for (uint64_t index = 1; ; index++) {
        int error = copy_candidate_name(index, destination_name);
        if (error != 0)
            return error;
        bool exists;
        error = candidate_exists(
                persistent_parent, destination_name, &exists);
        if (error != 0)
            return error;
        if (!exists && strcmp(destination_name, source_name) != 0) {
            error = ish_apple_rootfs_copy_managed_root(
                    seed_root, persistent_parent,
                    source_name, destination_name);
            if (error != EEXIST)
                return error;
        }
        if (index == UINT64_MAX)
            return ENOSPC;
    }
}

int ish_apple_root_catalog_copy_resumable(
        const char *seed_root,
        const char *persistent_parent,
        const char *source_name,
        const char *active_name,
        const char *operation_token,
        char destination_name[ISH_APPLE_ROOT_NAME_CAPACITY]) {
    if (seed_root == NULL || seed_root[0] == '\0' ||
            persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_root_catalog_is_managed_name(source_name) ||
            !ish_apple_rootfs_copy_operation_token_is_valid(
                    operation_token) ||
            destination_name == NULL)
        return EINVAL;
    bool has_active_name = active_name != NULL && active_name[0] != '\0';
    if (has_active_name &&
            !ish_apple_root_catalog_is_managed_name(active_name))
        return EINVAL;
    if (has_active_name && strcmp(source_name, active_name) == 0)
        return EBUSY;

    cleanup_deletions(persistent_parent);
    int operation_lock = -1;
    int error = ish_apple_rootfs_lock_copy_catalog(
            persistent_parent, &operation_lock);
    if (error != 0)
        return error;
    int source_lock = -1;
    error = ish_apple_rootfs_lock_managed_root(
            persistent_parent, source_name, true, true, &source_lock);
    if (error == EEXIST)
        error = EINVAL;
    if (error != 0) {
        (void) ish_apple_rootfs_unlock_managed_root(operation_lock);
        return error;
    }

    bool completed = false;
    bool found = false;
    error = ish_apple_rootfs_find_managed_copy_operation(
            persistent_parent, source_name, operation_token,
            destination_name, ISH_APPLE_ROOT_NAME_CAPACITY, &found);
    if (error == 0 && found) {
        if (!ish_apple_root_catalog_is_managed_name(destination_name)) {
            error = EEXIST;
        } else {
            error =
                    ish_apple_rootfs_copy_claimed_managed_root_for_operation(
                            seed_root, persistent_parent,
                            source_name, destination_name,
                            operation_token);
            completed = error == 0;
        }
    }

    for (uint64_t index = 1;
            error == 0 && !found && !completed; index++) {
        error = copy_candidate_name(index, destination_name);
        if (error != 0)
            break;
        bool exists;
        error = candidate_exists(
                persistent_parent, destination_name, &exists);
        if (error != 0)
            break;
        if (!exists && strcmp(destination_name, source_name) != 0) {
            error =
                    ish_apple_rootfs_copy_claimed_managed_root_for_operation(
                            seed_root, persistent_parent,
                            source_name, destination_name,
                            operation_token);
            if (error == 0) {
                completed = true;
                break;
            }
            if (error != EEXIST)
                break;

            error = ish_apple_rootfs_find_managed_copy_operation(
                    persistent_parent, source_name, operation_token,
                    destination_name, ISH_APPLE_ROOT_NAME_CAPACITY, &found);
            if (error != 0)
                break;
            if (found) {
                if (!ish_apple_root_catalog_is_managed_name(
                            destination_name)) {
                    error = EEXIST;
                    break;
                }
                error =
                        ish_apple_rootfs_copy_claimed_managed_root_for_operation(
                                seed_root, persistent_parent,
                                source_name, destination_name,
                                operation_token);
                completed = error == 0;
                break;
            }
        }
        if (index == UINT64_MAX) {
            error = ENOSPC;
            break;
        }
    }

    int unlock_error =
            ish_apple_rootfs_unlock_managed_root(source_lock);
    if (error == 0)
        error = unlock_error;
    unlock_error =
            ish_apple_rootfs_unlock_managed_root(operation_lock);
    if (error == 0)
        error = unlock_error;
    return completed ? 0 : error;
}

int ish_apple_root_catalog_delete(
        const char *persistent_parent,
        const char *name,
        const char *active_name,
        const char *selected_name) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_root_catalog_is_managed_name(name))
        return EINVAL;
    if ((active_name != NULL && strcmp(name, active_name) == 0) ||
            (selected_name != NULL && strcmp(name, selected_name) == 0))
        return EBUSY;

    int lifecycle_lock = -1;
    int error = ish_apple_rootfs_lock_managed_root(
            persistent_parent, name, true, false, &lifecycle_lock);
    if (error != 0)
        return error;
    int parent = open_parent_directory(persistent_parent);
    if (parent < 0) {
        (void) ish_apple_rootfs_unlock_managed_root(lifecycle_lock);
        return -parent;
    }

    unsigned char random[DELETION_TOKEN_BYTES];
    arc4random_buf(random, sizeof(random));
    static const char hexadecimal[] = "0123456789abcdef";
    char token[DELETION_TOKEN_HEX_LENGTH + 1];
    for (size_t index = 0; index < sizeof(random); index++) {
        token[index * 2] = hexadecimal[random[index] >> 4u];
        token[index * 2 + 1] = hexadecimal[random[index] & 0x0fu];
    }
    token[DELETION_TOKEN_HEX_LENGTH] = '\0';

    char tombstone[NAME_MAX + 1];
    int length = snprintf(tombstone, sizeof(tombstone),
            ".%s.deleting.%s", name, token);
    error = 0;
    if (length < 0 || (size_t) length >= sizeof(tombstone))
        error = ENAMETOOLONG;
    else if (renameat(parent, name, parent, tombstone) < 0)
        error = errno;
    else
        error = ish_apple_rootfs_sync_directory(parent);

    if (error == 0) {
        (void) ish_apple_rootfs_remove_entry_at(parent, tombstone);
        (void) ish_apple_rootfs_sync_directory(parent);
    }
    if (close(parent) < 0 && error == 0)
        error = errno;
    int unlock_error =
            ish_apple_rootfs_unlock_managed_root(lifecycle_lock);
    if (error == 0)
        error = unlock_error;
    return error;
}

int ish_apple_root_catalog_data_path(
        const char *persistent_parent,
        const char *name,
        char *path,
        size_t capacity) {
    if (persistent_parent == NULL || persistent_parent[0] == '\0' ||
            !ish_apple_root_catalog_is_managed_name(name) ||
            path == NULL || capacity == 0)
        return EINVAL;
    int length = snprintf(path, capacity, "%s/%s/data",
            persistent_parent, name);
    return length < 0 || (size_t) length >= capacity ? ENAMETOOLONG : 0;
}

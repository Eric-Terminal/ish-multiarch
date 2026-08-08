#include "sdk/iSHApple/Headers/iSHAppleMount.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "platform/apple-runtime-mount.h"
#include "platform/apple-watch-runtime-private.h"
#include "util/list.h"
#include "util/sync.h"

struct apple_mount_entry {
    struct list links;
    struct ish_apple_mount_id id;
    char *guest_directory;
    int host_directory_fd;
    int32_t access;
    int32_t state;
    uint64_t active_leases;
    bool startup_transaction;
};

struct ish_apple_mount_lease {
    atomic_uint refcount;
    struct apple_mount_entry *entry;
};

static struct list apple_mount_entries =
        LIST_INITIALIZER(apple_mount_entries);
static lock_t apple_mount_lock = LOCK_INITIALIZER;

static bool mount_id_equal(
        struct ish_apple_mount_id left,
        struct ish_apple_mount_id right) {
    return left.high == right.high && left.low == right.low;
}

static bool mount_id_is_zero(struct ish_apple_mount_id id) {
    return id.high == 0 && id.low == 0;
}

static bool reserved_zero(const uint64_t reserved[2]) {
    return reserved[0] == 0 && reserved[1] == 0;
}

static bool guest_path_has_dot_component(const char *path) {
    const char *component = path;
    while (*component != '\0') {
        while (*component == '/')
            component++;
        const char *end = component;
        while (*end != '\0' && *end != '/')
            end++;
        size_t length = (size_t) (end - component);
        if ((length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' &&
                        component[1] == '.'))
            return true;
        component = end;
    }
    return false;
}

static int validate_guest_directory(const char *path) {
    if (path == NULL)
        return _EINVAL;
    size_t length = strnlen(
            path, (size_t) ISH_APPLE_MOUNT_GUEST_DIRECTORY_BYTES_MAX + 1);
    if (length > ISH_APPLE_MOUNT_GUEST_DIRECTORY_BYTES_MAX)
        return _ENAMETOOLONG;
    if (length <= strlen("/mnt/") ||
            strncmp(path, "/mnt/", strlen("/mnt/")) != 0 ||
            path[length - 1] == '/' ||
            guest_path_has_dot_component(path))
        return _EINVAL;
    if (strcmp(path, "/mnt/shared") == 0 ||
            strncmp(path, "/mnt/shared/", strlen("/mnt/shared/")) == 0)
        return _EBUSY;

    for (size_t index = 1; index < length; index++) {
        if (path[index] == '/' && path[index - 1] == '/')
            return _EINVAL;
    }
    return 0;
}

static int validate_mount_spec(
        const struct ish_apple_mount_spec_v1 *spec) {
    if (spec == NULL)
        return _EINVAL;
    if (spec->version != ISH_APPLE_ABI_VERSION)
        return _ENOTSUP;
    if (spec->structure_size < sizeof(*spec) ||
            !reserved_zero(spec->reserved) ||
            mount_id_is_zero(spec->mount_id) ||
            (spec->access != ISH_APPLE_MOUNT_ACCESS_READ_ONLY &&
                    spec->access != ISH_APPLE_MOUNT_ACCESS_READ_WRITE) ||
            spec->host_directory_fd < 0)
        return _EINVAL;
    return validate_guest_directory(spec->guest_directory);
}

static int duplicate_host_directory(int source_fd, int *duplicate_out) {
    *duplicate_out = -1;
    struct stat status;
    if (fstat(source_fd, &status) < 0)
        return errno_map();
    if (!S_ISDIR(status.st_mode))
        return _ENOTDIR;
    int duplicate = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0)
        return errno_map();
    if (fstat(duplicate, &status) < 0) {
        int error = errno_map();
        close(duplicate);
        return error;
    }
    if (!S_ISDIR(status.st_mode)) {
        close(duplicate);
        return _ENOTDIR;
    }
    *duplicate_out = duplicate;
    return 0;
}

static void destroy_entry(struct apple_mount_entry *entry) {
    if (entry->host_directory_fd >= 0)
        close(entry->host_directory_fd);
    free(entry->guest_directory);
    free(entry);
}

static int create_entry(
        const struct ish_apple_mount_spec_v1 *spec,
        bool startup,
        struct apple_mount_entry **entry_out) {
    int error = validate_mount_spec(spec);
    if (error < 0)
        return error;

    struct apple_mount_entry *entry = calloc(1, sizeof(*entry));
    if (entry == NULL)
        return _ENOMEM;
    entry->guest_directory = strdup(spec->guest_directory);
    if (entry->guest_directory == NULL) {
        free(entry);
        return _ENOMEM;
    }
    error = duplicate_host_directory(
            spec->host_directory_fd, &entry->host_directory_fd);
    if (error < 0) {
        destroy_entry(entry);
        return error;
    }

    entry->id = spec->mount_id;
    entry->access = spec->access;
    entry->state = ISH_APPLE_MOUNT_STATE_STAGED;
    entry->startup_transaction = startup;
    *entry_out = entry;
    return 0;
}

static struct apple_mount_entry *find_entry_locked(
        struct ish_apple_mount_id id) {
    struct apple_mount_entry *entry;
    list_for_each_entry(&apple_mount_entries, entry, links) {
        if (mount_id_equal(entry->id, id))
            return entry;
    }
    return NULL;
}

static bool guest_directory_registered_locked(const char *path) {
    struct apple_mount_entry *entry;
    list_for_each_entry(&apple_mount_entries, entry, links) {
        if (strcmp(entry->guest_directory, path) == 0)
            return true;
    }
    return false;
}

static struct mount *find_exact_mount_locked(const char *point) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(mount->point, point) == 0)
            return mount;
    }
    return NULL;
}

static int ensure_one_guest_directory(
        struct task *task, const char *path) {
    struct fd *directory = generic_openat_task(
            task,
            AT_PWD,
            path,
            O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_,
            0);
    if (!IS_ERR(directory))
        return fd_close(directory);
    if (PTR_ERR(directory) != _ENOENT)
        return (int) PTR_ERR(directory);

    int error = generic_mkdirat_task(task, AT_PWD, path, 0755);
    if (error < 0 && error != _EEXIST)
        return error;
    directory = generic_openat_task(
            task,
            AT_PWD,
            path,
            O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_,
            0);
    if (IS_ERR(directory))
        return (int) PTR_ERR(directory);
    return fd_close(directory);
}

static int ensure_guest_directory(
        struct task *task, const char *guest_directory) {
    size_t length = strlen(guest_directory);
    char prefix[MAX_PATH];
    for (size_t index = 1; index <= length; index++) {
        if (guest_directory[index] != '/' &&
                guest_directory[index] != '\0')
            continue;
        memcpy(prefix, guest_directory, index);
        prefix[index] = '\0';
        int error = ensure_one_guest_directory(task, prefix);
        if (error < 0)
            return error;
    }
    return 0;
}

static int mount_entry_locked(
        struct apple_mount_entry *entry, struct task *task) {
    struct task *saved_current = current;
    current = task;
    int error = ensure_guest_directory(task, entry->guest_directory);
    current = saved_current;
    if (error < 0)
        return error;

    char source[64];
    int written = snprintf(
            source,
            sizeof(source),
            "apple-fd:%016" PRIx64 "%016" PRIx64,
            entry->id.high,
            entry->id.low);
    if (written < 0 || (size_t) written >= sizeof(source))
        return _EOVERFLOW;

    lock(&mounts_lock);
    if (find_exact_mount_locked(entry->guest_directory) != NULL) {
        unlock(&mounts_lock);
        return _EBUSY;
    }
    int flags = entry->access == ISH_APPLE_MOUNT_ACCESS_READ_ONLY ?
            MS_READONLY_ : 0;
    error = realfs_mount_from_fd_locked(
            entry->host_directory_fd,
            source,
            entry->guest_directory,
            "",
            flags);
    unlock(&mounts_lock);
    if (error < 0)
        return error;

    close(entry->host_directory_fd);
    entry->host_directory_fd = -1;
    entry->state = ISH_APPLE_MOUNT_STATE_ACTIVE;
    return 0;
}

static int snapshot_runtime_task(
        struct task *snapshot, struct fs_info **retained_fs) {
    memset(snapshot, 0, sizeof(*snapshot));
    *retained_fs = NULL;

    lock(&pids_lock);
    struct task *init = pid_get_task(1);
    if (init == NULL) {
        unlock(&pids_lock);
        return _ESHUTDOWN;
    }
    struct fs_info *fs = init->fs;
    fs_info_retain(fs);
    snapshot->uid = init->uid;
    snapshot->gid = init->gid;
    snapshot->euid = init->euid;
    snapshot->egid = init->egid;
    snapshot->suid = init->suid;
    snapshot->sgid = init->sgid;
    unlock(&pids_lock);

    snapshot->fs = fs;
    *retained_fs = fs;
    return 0;
}

static int unmount_entry_locked(
        struct apple_mount_entry *entry, bool force) {
    if (entry->state == ISH_APPLE_MOUNT_STATE_REMOVED)
        return 0;
    if (entry->state == ISH_APPLE_MOUNT_STATE_STAGED)
        return _EAGAIN;
    if (!force && entry->active_leases != 0)
        return _EBUSY;

    lock(&mounts_lock);
    struct mount *mount = find_exact_mount_locked(entry->guest_directory);
    int error = mount == NULL ? 0 : mount_remove(mount);
    unlock(&mounts_lock);
    if (error < 0)
        return error;
    entry->state = ISH_APPLE_MOUNT_STATE_REMOVED;
    return 0;
}

static void remove_and_destroy_entry_locked(
        struct apple_mount_entry *entry) {
    list_remove(&entry->links);
    destroy_entry(entry);
}

int ish_apple_mount_prepare_startup(
        const struct ish_apple_mount_spec_v1 *specs,
        uint32_t count) {
    if (count != 0 && specs == NULL)
        return _EINVAL;

    lock(&apple_mount_lock);
    if (!list_empty(&apple_mount_entries)) {
        unlock(&apple_mount_lock);
        return _EALREADY;
    }

    int error = 0;
    for (uint32_t index = 0; index < count; index++) {
        struct apple_mount_entry *entry;
        error = create_entry(&specs[index], true, &entry);
        if (error < 0)
            break;
        if (find_entry_locked(entry->id) != NULL ||
                guest_directory_registered_locked(entry->guest_directory)) {
            destroy_entry(entry);
            error = _EEXIST;
            break;
        }
        list_add_tail(&apple_mount_entries, &entry->links);
    }

    if (error < 0) {
        struct apple_mount_entry *entry, *temporary;
        list_for_each_entry_safe(
                &apple_mount_entries, entry, temporary, links) {
            remove_and_destroy_entry_locked(entry);
        }
    }
    unlock(&apple_mount_lock);
    return error;
}

int ish_apple_mount_activate_startup(struct task *task) {
    if (task == NULL)
        return _EINVAL;
    lock(&apple_mount_lock);
    int error = 0;
    struct apple_mount_entry *entry;
    list_for_each_entry(&apple_mount_entries, entry, links) {
        if (!entry->startup_transaction ||
                entry->state != ISH_APPLE_MOUNT_STATE_STAGED)
            continue;
        error = mount_entry_locked(entry, task);
        if (error < 0)
            break;
    }
    unlock(&apple_mount_lock);
    return error;
}

void ish_apple_mount_finish_startup(int success) {
    lock(&apple_mount_lock);
    struct apple_mount_entry *entry, *temporary;
    list_for_each_entry_safe(
            &apple_mount_entries, entry, temporary, links) {
        if (!entry->startup_transaction)
            continue;
        if (success) {
            entry->startup_transaction = false;
            continue;
        }
        if (entry->state != ISH_APPLE_MOUNT_STATE_STAGED)
            (void) unmount_entry_locked(entry, true);
        remove_and_destroy_entry_locked(entry);
    }
    unlock(&apple_mount_lock);
}

int32_t ish_apple_mount_add(
        const struct ish_apple_mount_spec_v1 *spec) {
    int error = validate_mount_spec(spec);
    if (error < 0)
        return error;
    error = ish_watch_runtime_operation_availability();
    if (error < 0)
        return error;

    struct apple_mount_entry *entry;
    error = create_entry(spec, false, &entry);
    if (error < 0)
        return error;
    struct task task_snapshot;
    struct fs_info *retained_fs;
    error = snapshot_runtime_task(&task_snapshot, &retained_fs);
    if (error < 0) {
        destroy_entry(entry);
        return error;
    }

    lock(&apple_mount_lock);
    if (find_entry_locked(entry->id) != NULL ||
            guest_directory_registered_locked(entry->guest_directory)) {
        error = _EEXIST;
    } else {
        list_add_tail(&apple_mount_entries, &entry->links);
        error = mount_entry_locked(entry, &task_snapshot);
        if (error < 0) {
            remove_and_destroy_entry_locked(entry);
            entry = NULL;
        }
    }
    unlock(&apple_mount_lock);
    fs_info_release(retained_fs);
    if (error < 0 && entry != NULL)
        destroy_entry(entry);
    return error;
}

int32_t ish_apple_mount_remove(
        struct ish_apple_mount_id mount_id,
        uint32_t flags) {
    if (mount_id_is_zero(mount_id) ||
            (flags & ~ISH_APPLE_MOUNT_REMOVE_FORCE) != 0)
        return _EINVAL;

    lock(&apple_mount_lock);
    struct apple_mount_entry *entry = find_entry_locked(mount_id);
    if (entry == NULL) {
        unlock(&apple_mount_lock);
        return _ENOENT;
    }
    entry->state = entry->state == ISH_APPLE_MOUNT_STATE_ACTIVE ?
            ISH_APPLE_MOUNT_STATE_DRAINING : entry->state;
    int error = unmount_entry_locked(
            entry, (flags & ISH_APPLE_MOUNT_REMOVE_FORCE) != 0);
    if (error == 0 && entry->active_leases == 0)
        remove_and_destroy_entry_locked(entry);
    unlock(&apple_mount_lock);
    return error;
}

static uint64_t mount_reference_count_locked(
        const struct apple_mount_entry *entry,
        int32_t *state_out) {
    lock(&mounts_lock);
    struct mount *mount = find_exact_mount_locked(entry->guest_directory);
    uint64_t references = mount == NULL ?
            0 : (uint64_t) mount->refcount;
    unlock(&mounts_lock);
    *state_out = mount == NULL &&
            entry->state != ISH_APPLE_MOUNT_STATE_STAGED ?
            ISH_APPLE_MOUNT_STATE_REMOVED : entry->state;
    return references;
}

int32_t ish_apple_mount_list(
        struct ish_apple_mount_info_v1 *entries,
        uint32_t capacity,
        uint32_t *count_out) {
    if (count_out == NULL || (capacity != 0 && entries == NULL))
        return _EINVAL;

    lock(&apple_mount_lock);
    uint64_t count = 0;
    struct apple_mount_entry *entry;
    list_for_each_entry(&apple_mount_entries, entry, links)
        count++;
    if (count > UINT32_MAX) {
        unlock(&apple_mount_lock);
        return _EOVERFLOW;
    }
    *count_out = (uint32_t) count;

    uint32_t index = 0;
    list_for_each_entry(&apple_mount_entries, entry, links) {
        if (index >= capacity)
            break;
        int32_t state;
        uint64_t references = mount_reference_count_locked(entry, &state);
        size_t guest_bytes = strlen(entry->guest_directory);
        entries[index] = (struct ish_apple_mount_info_v1) {
            .version = ISH_APPLE_ABI_VERSION,
            .structure_size =
                    (uint32_t) sizeof(entries[index]),
            .mount_id = entry->id,
            .access = entry->access,
            .state = state,
            .active_leases = entry->active_leases,
            .active_references = references,
            .guest_directory_bytes = (uint32_t) guest_bytes,
        };
        index++;
    }
    int result = entries != NULL && capacity < *count_out ? _ENOSPC : 0;
    unlock(&apple_mount_lock);
    return result;
}

int32_t ish_apple_mount_copy_guest_directory(
        struct ish_apple_mount_id mount_id,
        char *buffer,
        uint32_t capacity,
        uint32_t *required_bytes_out) {
    if (mount_id_is_zero(mount_id) || required_bytes_out == NULL ||
            (buffer == NULL && capacity != 0))
        return _EINVAL;

    lock(&apple_mount_lock);
    struct apple_mount_entry *entry = find_entry_locked(mount_id);
    if (entry == NULL) {
        unlock(&apple_mount_lock);
        return _ENOENT;
    }
    size_t required = strlen(entry->guest_directory) + 1;
    *required_bytes_out = (uint32_t) required;
    if (buffer == NULL) {
        unlock(&apple_mount_lock);
        return 0;
    }
    if ((size_t) capacity < required) {
        unlock(&apple_mount_lock);
        return _ENOSPC;
    }
    memcpy(buffer, entry->guest_directory, required);
    unlock(&apple_mount_lock);
    return 0;
}

int32_t ish_apple_mount_lease_acquire(
        struct ish_apple_mount_id mount_id,
        ish_apple_mount_lease **lease_out) {
    if (mount_id_is_zero(mount_id) || lease_out == NULL)
        return _EINVAL;
    *lease_out = NULL;
    struct ish_apple_mount_lease *lease = malloc(sizeof(*lease));
    if (lease == NULL)
        return _ENOMEM;

    lock(&apple_mount_lock);
    struct apple_mount_entry *entry = find_entry_locked(mount_id);
    int error = 0;
    if (entry == NULL) {
        error = _ENOENT;
    } else if (entry->state != ISH_APPLE_MOUNT_STATE_ACTIVE) {
        error = entry->state == ISH_APPLE_MOUNT_STATE_REMOVED ?
                _ESTALE : _EBUSY;
    } else {
        lock(&mounts_lock);
        bool mounted = find_exact_mount_locked(
                entry->guest_directory) != NULL;
        unlock(&mounts_lock);
        if (!mounted) {
            error = _ESTALE;
        } else if (entry->active_leases == UINT64_MAX) {
            error = _EOVERFLOW;
        } else {
            entry->active_leases++;
            atomic_init(&lease->refcount, 1);
            lease->entry = entry;
            *lease_out = lease;
        }
    }
    unlock(&apple_mount_lock);
    if (error < 0)
        free(lease);
    return error;
}

void ish_apple_mount_lease_retain(ish_apple_mount_lease *lease) {
    if (lease != NULL)
        atomic_fetch_add_explicit(
                &lease->refcount, 1, memory_order_relaxed);
}

void ish_apple_mount_lease_release(ish_apple_mount_lease *lease) {
    if (lease == NULL || atomic_fetch_sub_explicit(
            &lease->refcount, 1, memory_order_acq_rel) != 1)
        return;

    lock(&apple_mount_lock);
    struct apple_mount_entry *entry = lease->entry;
    if (entry->active_leases != 0)
        entry->active_leases--;
    if (entry->active_leases == 0 &&
            entry->state == ISH_APPLE_MOUNT_STATE_DRAINING) {
        int error = unmount_entry_locked(entry, false);
        if (error == 0)
            remove_and_destroy_entry_locked(entry);
    } else if (entry->active_leases == 0 &&
            entry->state == ISH_APPLE_MOUNT_STATE_REMOVED) {
        remove_and_destroy_entry_locked(entry);
    }
    unlock(&apple_mount_lock);
    free(lease);
}

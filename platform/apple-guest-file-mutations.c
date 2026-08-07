#include "platform/apple-guest-file-mutations.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

struct child_name {
    struct child_name *next;
    char value[NAME_MAX + 1];
};

struct collect_names_context {
    struct child_name *head;
    struct child_name **tail;
    int error;
};

static sqword_t collect_child_name(
        void *opaque,
        const struct dir_entry *entry,
        off_t_ next_position) {
    (void) next_position;
    if (strcmp(entry->name, ".") == 0 ||
            strcmp(entry->name, "..") == 0)
        return 1;
    struct collect_names_context *context = opaque;
    struct child_name *child = malloc(sizeof(*child));
    if (child == NULL) {
        context->error = _ENOMEM;
        return context->error;
    }
    child->next = NULL;
    strcpy(child->value, entry->name);
    *context->tail = child;
    context->tail = &child->next;
    return 1;
}

static void free_child_names(struct child_name *head) {
    while (head != NULL) {
        struct child_name *next = head->next;
        free(head);
        head = next;
    }
}

static int remove_path_recursive(struct task *task, const char *path) {
    struct statbuf stat;
    int error = file_statat_task(
            task,
            AT_FDCWD_,
            path,
            AT_SYMLINK_NOFOLLOW_,
            &stat);
    if (error < 0)
        return error;
    if (!S_ISDIR(stat.mode))
        return file_unlinkat_task(
                task, AT_FDCWD_, path, false);

    fd_t directory = file_openat_task(
            task,
            AT_FDCWD_,
            path,
            O_RDONLY_ | O_DIRECTORY_ | O_NOFOLLOW_,
            0);
    if (directory < 0)
        return directory;
    struct collect_names_context names = {0};
    names.tail = &names.head;
    sqword_t collected = file_getdents_task(
            task, directory, collect_child_name, &names);
    int close_error = f_close_task(task, directory);
    if (names.error < 0)
        error = names.error;
    else if (collected < 0)
        error = (int) collected;
    else if (close_error < 0)
        error = close_error;

    size_t parent_length = strlen(path);
    for (struct child_name *child = names.head;
            error == 0 && child != NULL;
            child = child->next) {
        char child_path[MAX_PATH];
        int length = parent_length == 1 ?
                snprintf(
                        child_path,
                        sizeof(child_path),
                        "/%s",
                        child->value) :
                snprintf(
                        child_path,
                        sizeof(child_path),
                        "%s/%s",
                        path,
                        child->value);
        if (length < 0 || length >= (int) sizeof(child_path)) {
            error = _ENAMETOOLONG;
            break;
        }
        error = remove_path_recursive(task, child_path);
        if (error == _ENOENT)
            error = 0;
    }
    free_child_names(names.head);
    if (error < 0)
        return error;
    return file_unlinkat_task(
            task, AT_FDCWD_, path, true);
}

int ish_apple_guest_file_remove_task(
        struct task *task,
        const char *path,
        bool recursive) {
    if (recursive)
        return remove_path_recursive(task, path);

    struct statbuf stat;
    int error = file_statat_task(
            task,
            AT_FDCWD_,
            path,
            AT_SYMLINK_NOFOLLOW_,
            &stat);
    if (error < 0)
        return error;
    return file_unlinkat_task(
            task, AT_FDCWD_, path, S_ISDIR(stat.mode));
}

int ish_apple_guest_file_rename_task(
        struct task *task,
        const char *source,
        const char *destination) {
    return file_renameat_task(
            task,
            AT_FDCWD_,
            source,
            AT_FDCWD_,
            destination);
}

static int ensure_directory(
        struct task *task,
        const char *path,
        uint32_t mode) {
    int error = file_mkdirat_task(
            task, AT_FDCWD_, path, (mode_t_) mode);
    if (error != _EEXIST)
        return error;
    struct statbuf stat;
    error = file_statat_task(
            task, AT_FDCWD_, path, 0, &stat);
    if (error < 0)
        return error;
    return S_ISDIR(stat.mode) ? 0 : _ENOTDIR;
}

int ish_apple_guest_file_mkdir_task(
        struct task *task,
        const char *path,
        uint32_t mode,
        bool parents) {
    if (!parents)
        return file_mkdirat_task(
                task, AT_FDCWD_, path, (mode_t_) mode);

    char mutable_path[MAX_PATH];
    strcpy(mutable_path, path);
    for (char *separator = mutable_path + 1;
            (separator = strchr(separator, '/')) != NULL;
            separator++) {
        if (separator == mutable_path + 1)
            continue;
        *separator = '\0';
        int error = ensure_directory(task, mutable_path, mode);
        *separator = '/';
        if (error < 0)
            return error;
    }
    return ensure_directory(task, mutable_path, mode);
}

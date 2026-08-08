#ifndef PLATFORM_APPLE_GUEST_FILE_MUTATIONS_H
#define PLATFORM_APPLE_GUEST_FILE_MUTATIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#pragma GCC visibility push(hidden)

struct task;

int ish_apple_guest_file_remove_task(
        struct task *task, const char *path, bool recursive);
int ish_apple_guest_file_rename_task(
        struct task *task, const char *source, const char *destination);
int ish_apple_guest_file_mkdir_task(
        struct task *task, const char *path, uint32_t mode, bool parents);

#ifdef ISH_APPLE_GUEST_FILES_TESTING
void ish_apple_guest_file_test_fail_copy_after(
        size_t copied_bytes, int error);
int ish_apple_guest_file_test_after_copy_write(size_t copied_bytes);
#else
static inline int ish_apple_guest_file_test_after_copy_write(
        size_t copied_bytes) {
    (void) copied_bytes;
    return 0;
}
#endif

#pragma GCC visibility pop

#endif

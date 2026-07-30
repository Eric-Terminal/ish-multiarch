#ifndef ISH_APPLE_WATCH_GUEST_FILES_PRIVATE_H
#define ISH_APPLE_WATCH_GUEST_FILES_PRIVATE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum ish_watch_guest_file_test_event {
    ISH_WATCH_GUEST_FILE_TEST_PARENT_OPENED = 1,
    ISH_WATCH_GUEST_FILE_TEST_TEMPORARY_OPENED = 2,
};

#ifdef ISH_APPLE_WATCH_GUEST_FILES_TESTING
typedef void (*ish_watch_guest_file_test_event_hook)(
        int32_t file_id,
        int event,
        const char *temporary_name);

ssize_t ish_watch_guest_file_test_read_current(
        int32_t file_id, void *buffer, size_t capacity);
int ish_watch_guest_file_test_replace_current(
        int32_t file_id,
        const void *bytes,
        size_t length,
        int remove_file);
void ish_watch_guest_file_test_set_write_behavior(
        size_t maximum_chunk,
        size_t fail_after,
        int error);
void ish_watch_guest_file_test_set_event_hook(
        ish_watch_guest_file_test_event_hook hook);
#endif

#endif

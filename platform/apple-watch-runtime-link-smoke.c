#include "platform/apple-watch-runtime.h"

static int (*volatile start_entry)(
        const char *, const char *, const char *, const char *,
        const char *) =
        ish_watch_runtime_start;
static int (*volatile phase_entry)(void) =
        ish_watch_runtime_current_phase;
static int (*volatile error_entry)(void) =
        ish_watch_runtime_last_error;
static size_t (*volatile output_entry)(
        void *, size_t, uint64_t *) =
        ish_watch_runtime_read_output;
static ssize_t (*volatile input_entry)(const void *, size_t) =
        ish_watch_runtime_send_input;
static int (*volatile window_entry)(uint16_t, uint16_t) =
        ish_watch_runtime_set_window_size;
static ssize_t (*volatile guest_file_read_entry)(
        int32_t, void *, size_t) =
        ish_watch_guest_file_read;
static int (*volatile guest_file_replace_entry)(
        int32_t, const void *, size_t, int) =
        ish_watch_guest_file_replace;
static int (*volatile session_create_entry)(
        const char *, uint16_t, uint16_t, ish_watch_session_id *) =
        ish_watch_session_create;
static int (*volatile session_status_entry)(
        ish_watch_session_id, struct ish_watch_session_status *) =
        ish_watch_session_status;
static ssize_t (*volatile session_output_entry)(
        ish_watch_session_id, void *, size_t, uint64_t *) =
        ish_watch_session_read_output;
static ssize_t (*volatile session_input_entry)(
        ish_watch_session_id, const void *, size_t) =
        ish_watch_session_send_input;
static int (*volatile session_window_entry)(
        ish_watch_session_id, uint16_t, uint16_t) =
        ish_watch_session_set_window_size;
static int (*volatile session_close_entry)(ish_watch_session_id) =
        ish_watch_session_close;

int main(void) {
    // 只引用公开外观，验证静态归档能抽取它及其传递依赖。
    return start_entry == NULL || phase_entry == NULL ||
            error_entry == NULL || output_entry == NULL ||
            input_entry == NULL || window_entry == NULL ||
            guest_file_read_entry == NULL ||
            guest_file_replace_entry == NULL ||
            session_create_entry == NULL ||
            session_status_entry == NULL ||
            session_output_entry == NULL ||
            session_input_entry == NULL ||
            session_window_entry == NULL ||
            session_close_entry == NULL;
}

#include "platform/apple-watch-runtime.h"

static int (*volatile start_entry)(
        const char *, const char *, const char *) =
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

int main(void) {
    // 只引用公开外观，验证静态归档能抽取它及其传递依赖。
    return start_entry == NULL || phase_entry == NULL ||
            error_entry == NULL || output_entry == NULL ||
            input_entry == NULL || window_entry == NULL;
}

#include "sdk/iSHApple/Headers/iSHApple.h"

#include <stddef.h>

static int32_t (*volatile runtime_start_entry)(
        const struct ish_apple_runtime_spec_v1 *) =
        ish_apple_runtime_start;
static int32_t (*volatile runtime_phase_entry)(void) =
        ish_apple_runtime_current_phase;
static int32_t (*volatile rootfs_seed_entry)(
        const char *, const char *, const char *, int32_t *) =
        ish_apple_rootfs_install_seed;
static int32_t (*volatile command_start_entry)(
        const struct ish_apple_command_spec_v1 *,
        const struct ish_apple_command_callbacks_v1 *,
        ish_apple_command_session **) =
        ish_apple_command_session_start;
static int32_t (*volatile command_write_entry)(
        ish_apple_command_session *,
        const void *, uint32_t, uint32_t *) =
        ish_apple_command_session_write_stdin;
static int32_t (*volatile command_cancel_entry)(
        ish_apple_command_session *) =
        ish_apple_command_session_cancel;
static int32_t (*volatile command_wait_entry)(
        ish_apple_command_session *,
        struct ish_apple_command_result_v1 *) =
        ish_apple_command_session_wait;

int main(void) {
    // 保留真实入口的链接依赖，但验证程序本身不启动 guest。
    return runtime_start_entry == NULL ||
            runtime_phase_entry == NULL ||
            rootfs_seed_entry == NULL ||
            command_start_entry == NULL ||
            command_write_entry == NULL ||
            command_cancel_entry == NULL ||
            command_wait_entry == NULL;
}

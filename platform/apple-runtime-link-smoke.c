#include "sdk/iSHApple/Headers/iSHApple.h"

#include <stddef.h>

static int32_t (*volatile runtime_start_entry)(
        const struct ish_apple_runtime_spec_v1 *) =
        ish_apple_runtime_start;
static int32_t (*volatile runtime_phase_entry)(void) =
        ish_apple_runtime_current_phase;
static int32_t (*volatile runtime_start_v2_entry)(
        const struct ish_apple_runtime_spec_v2 *) =
        ish_apple_runtime_start_v2;
static int32_t (*volatile mount_add_entry)(
        const struct ish_apple_mount_spec_v1 *) =
        ish_apple_mount_add;
static int32_t (*volatile mount_list_entry)(
        struct ish_apple_mount_info_v1 *, uint32_t, uint32_t *) =
        ish_apple_mount_list;
static int32_t (*volatile mount_remove_entry)(
        struct ish_apple_mount_id, uint32_t) =
        ish_apple_mount_remove;
static int32_t (*volatile mount_path_entry)(
        struct ish_apple_mount_id, char *, uint32_t, uint32_t *) =
        ish_apple_mount_copy_guest_directory;
static int32_t (*volatile mount_lease_entry)(
        struct ish_apple_mount_id, ish_apple_mount_lease **) =
        ish_apple_mount_lease_acquire;
static int32_t (*volatile rootfs_seed_entry)(
        const char *, const char *, const char *, int32_t *) =
        ish_apple_rootfs_install_seed;
static int32_t (*volatile diagnostics_drain_entry)(
        uint32_t, uint64_t,
        struct ish_apple_diagnostic_event_v1 *,
        uint32_t, uint32_t *) =
        ish_apple_diagnostics_drain;
static int32_t (*volatile diagnostics_clear_entry)(
        uint32_t, uint64_t, uint32_t *) =
        ish_apple_diagnostics_clear;
static int32_t (*volatile guest_file_stat_entry)(
        const struct ish_apple_guest_file_request_v1 *,
        struct ish_apple_guest_file_info_v1 *) =
        ish_apple_guest_file_stat;
static int32_t (*volatile guest_file_read_entry)(
        const struct ish_apple_guest_file_request_v1 *, uint64_t,
        void *, uint32_t, uint32_t *, uint64_t *, int32_t *) =
        ish_apple_guest_file_read;
static int32_t (*volatile guest_file_write_entry)(
        const struct ish_apple_guest_file_request_v1 *,
        const void *, uint32_t, uint32_t) =
        ish_apple_guest_file_write;
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
static int32_t (*volatile terminal_start_entry)(
        const struct ish_apple_terminal_spec_v1 *,
        ish_apple_terminal_session **) =
        ish_apple_terminal_session_start;
static int32_t (*volatile terminal_read_entry)(
        ish_apple_terminal_session *, void *, uint32_t,
        uint32_t *, uint64_t *) =
        ish_apple_terminal_session_read_output;
static int32_t (*volatile terminal_cancel_entry)(
        ish_apple_terminal_session *) =
        ish_apple_terminal_session_cancel;
static int32_t (*volatile terminal_result_entry)(
        ish_apple_terminal_session *,
        struct ish_apple_terminal_result_v1 *) =
        ish_apple_terminal_session_copy_result;

int main(void) {
    // 保留真实入口的链接依赖，但验证程序本身不启动 guest。
    return runtime_start_entry == NULL ||
            runtime_start_v2_entry == NULL ||
            runtime_phase_entry == NULL ||
            mount_add_entry == NULL ||
            mount_list_entry == NULL ||
            mount_remove_entry == NULL ||
            mount_path_entry == NULL ||
            mount_lease_entry == NULL ||
            rootfs_seed_entry == NULL ||
            diagnostics_drain_entry == NULL ||
            diagnostics_clear_entry == NULL ||
            guest_file_stat_entry == NULL ||
            guest_file_read_entry == NULL ||
            guest_file_write_entry == NULL ||
            command_start_entry == NULL ||
            command_write_entry == NULL ||
            command_cancel_entry == NULL ||
            command_wait_entry == NULL ||
            terminal_start_entry == NULL ||
            terminal_read_entry == NULL ||
            terminal_cancel_entry == NULL ||
            terminal_result_entry == NULL;
}

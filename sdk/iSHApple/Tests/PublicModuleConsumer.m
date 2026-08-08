@import iSHApple;

static int32_t (*volatile runtime_start_entry)(
        const struct ish_apple_runtime_spec_v1 *) =
        ish_apple_runtime_start;
static int32_t (*volatile runtime_phase_entry)(void) =
        ish_apple_runtime_current_phase;
static int32_t (*volatile runtime_start_v2_entry)(
        const struct ish_apple_runtime_spec_v2 *) =
        ish_apple_runtime_start_v2;
static int32_t (*volatile runtime_error_entry)(void) =
        ish_apple_runtime_last_error;
static int32_t (*volatile runtime_capabilities_entry)(
        struct ish_apple_runtime_capabilities_v1 *) =
        ish_apple_runtime_copy_capabilities;
static int32_t (*volatile rootfs_install_entry)(
        const char *, const char *, const char *, int32_t *) =
        ish_apple_rootfs_install_seed;
static int32_t (*volatile rootfs_archive_install_entry)(
        const struct ish_apple_rootfs_archive_spec_v1 *,
        const struct ish_apple_rootfs_archive_callbacks_v1 *,
        int32_t *) = ish_apple_rootfs_install_archive;
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
static int32_t (*volatile guest_file_list_entry)(
        const struct ish_apple_guest_file_request_v1 *, uint64_t,
        struct ish_apple_guest_file_directory_entry_v1 *, uint32_t,
        uint32_t *, uint64_t *, int32_t *) =
        ish_apple_guest_file_list;
static int32_t (*volatile guest_file_read_entry)(
        const struct ish_apple_guest_file_request_v1 *, uint64_t,
        void *, uint32_t, uint32_t *, uint64_t *, int32_t *) =
        ish_apple_guest_file_read;
static int32_t (*volatile guest_file_write_entry)(
        const struct ish_apple_guest_file_request_v1 *,
        const void *, uint32_t, uint32_t) =
        ish_apple_guest_file_write;
static int32_t (*volatile guest_file_copy_entry)(
        const struct ish_apple_guest_file_request_v1 *, const char *) =
        ish_apple_guest_file_copy;
static int32_t (*volatile guest_file_edit_entry)(
        const struct ish_apple_guest_file_request_v1 *, uint64_t,
        uint64_t, const void *, uint32_t) =
        ish_apple_guest_file_edit;
static int32_t (*volatile guest_file_remove_entry)(
        const struct ish_apple_guest_file_request_v1 *, uint32_t) =
        ish_apple_guest_file_remove;
static int32_t (*volatile guest_file_rename_entry)(
        const struct ish_apple_guest_file_request_v1 *, const char *) =
        ish_apple_guest_file_rename;
static int32_t (*volatile guest_file_mkdir_entry)(
        const struct ish_apple_guest_file_request_v1 *,
        uint32_t, uint32_t) =
        ish_apple_guest_file_mkdir;
static int32_t (*volatile mount_add_entry)(
        const struct ish_apple_mount_spec_v1 *) =
        ish_apple_mount_add;
static int32_t (*volatile mount_remove_entry)(
        struct ish_apple_mount_id, uint32_t) =
        ish_apple_mount_remove;
static int32_t (*volatile mount_list_entry)(
        struct ish_apple_mount_info_v1 *, uint32_t, uint32_t *) =
        ish_apple_mount_list;
static int32_t (*volatile mount_path_entry)(
        struct ish_apple_mount_id, char *, uint32_t, uint32_t *) =
        ish_apple_mount_copy_guest_directory;
static int32_t (*volatile mount_lease_entry)(
        struct ish_apple_mount_id, ish_apple_mount_lease **) =
        ish_apple_mount_lease_acquire;
static void (*volatile mount_lease_retain_entry)(
        ish_apple_mount_lease *) =
        ish_apple_mount_lease_retain;
static void (*volatile mount_lease_release_entry)(
        ish_apple_mount_lease *) =
        ish_apple_mount_lease_release;

static int32_t (*volatile command_start_entry)(
        const struct ish_apple_command_spec_v1 *,
        const struct ish_apple_command_callbacks_v1 *,
        ish_apple_command_session **) =
        ish_apple_command_session_start;
static ish_apple_command_session *(*volatile command_retain_entry)(
        ish_apple_command_session *) =
        ish_apple_command_session_retain;
static void (*volatile command_release_entry)(
        ish_apple_command_session *) =
        ish_apple_command_session_release;
static int32_t (*volatile command_write_entry)(
        ish_apple_command_session *, const void *, uint32_t, uint32_t *) =
        ish_apple_command_session_write_stdin;
static int32_t (*volatile command_close_entry)(
        ish_apple_command_session *) =
        ish_apple_command_session_close_stdin;
static int32_t (*volatile command_interrupt_entry)(
        ish_apple_command_session *) =
        ish_apple_command_session_interrupt;
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
static ish_apple_terminal_session *(*volatile terminal_retain_entry)(
        ish_apple_terminal_session *) =
        ish_apple_terminal_session_retain;
static void (*volatile terminal_release_entry)(
        ish_apple_terminal_session *) =
        ish_apple_terminal_session_release;
static int32_t (*volatile terminal_read_entry)(
        ish_apple_terminal_session *, void *, uint32_t,
        uint32_t *, uint64_t *) =
        ish_apple_terminal_session_read_output;
static int32_t (*volatile terminal_write_entry)(
        ish_apple_terminal_session *, const void *, uint32_t,
        uint32_t *) =
        ish_apple_terminal_session_write_input;
static int32_t (*volatile terminal_finish_input_entry)(
        ish_apple_terminal_session *) =
        ish_apple_terminal_session_finish_input;
static int32_t (*volatile terminal_resize_entry)(
        ish_apple_terminal_session *, uint16_t, uint16_t) =
        ish_apple_terminal_session_resize;
static int32_t (*volatile terminal_interrupt_entry)(
        ish_apple_terminal_session *) =
        ish_apple_terminal_session_interrupt;
static int32_t (*volatile terminal_cancel_entry)(
        ish_apple_terminal_session *) =
        ish_apple_terminal_session_cancel;
static int32_t (*volatile terminal_result_entry)(
        ish_apple_terminal_session *,
        struct ish_apple_terminal_result_v1 *) =
        ish_apple_terminal_session_copy_result;

int main(void) {
    /*
     * 只通过成品 module map 导入并强引用公共入口，确保链接器会从静态 SDK
     * 中抽取完整运行时闭包；测试本身不启动 guest。
     */
    return runtime_start_entry == 0 ||
            runtime_start_v2_entry == 0 ||
            runtime_phase_entry == 0 ||
            runtime_error_entry == 0 ||
            runtime_capabilities_entry == 0 ||
            rootfs_install_entry == 0 ||
            rootfs_archive_install_entry == 0 ||
            diagnostics_drain_entry == 0 ||
            diagnostics_clear_entry == 0 ||
            guest_file_stat_entry == 0 ||
            guest_file_list_entry == 0 ||
            guest_file_read_entry == 0 ||
            guest_file_write_entry == 0 ||
            guest_file_copy_entry == 0 ||
            guest_file_edit_entry == 0 ||
            guest_file_remove_entry == 0 ||
            guest_file_rename_entry == 0 ||
            guest_file_mkdir_entry == 0 ||
            mount_add_entry == 0 ||
            mount_remove_entry == 0 ||
            mount_list_entry == 0 ||
            mount_path_entry == 0 ||
            mount_lease_entry == 0 ||
            mount_lease_retain_entry == 0 ||
            mount_lease_release_entry == 0 ||
            command_start_entry == 0 ||
            command_retain_entry == 0 ||
            command_release_entry == 0 ||
            command_write_entry == 0 ||
            command_close_entry == 0 ||
            command_interrupt_entry == 0 ||
            command_cancel_entry == 0 ||
            command_wait_entry == 0 ||
            terminal_start_entry == 0 ||
            terminal_retain_entry == 0 ||
            terminal_release_entry == 0 ||
            terminal_read_entry == 0 ||
            terminal_write_entry == 0 ||
            terminal_finish_input_entry == 0 ||
            terminal_resize_entry == 0 ||
            terminal_interrupt_entry == 0 ||
            terminal_cancel_entry == 0 ||
            terminal_result_entry == 0;
}

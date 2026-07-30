@import iSHApple;

static int32_t (*volatile runtime_start_entry)(
        const struct ish_apple_runtime_spec_v1 *) =
        ish_apple_runtime_start;
static int32_t (*volatile runtime_phase_entry)(void) =
        ish_apple_runtime_current_phase;
static int32_t (*volatile runtime_error_entry)(void) =
        ish_apple_runtime_last_error;
static int32_t (*volatile rootfs_install_entry)(
        const char *, const char *, const char *, int32_t *) =
        ish_apple_rootfs_install_seed;

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

int main(void) {
    /*
     * 只通过成品 module map 导入并强引用公共入口，确保链接器会从静态 SDK
     * 中抽取完整运行时闭包；测试本身不启动 guest。
     */
    return runtime_start_entry == 0 ||
            runtime_phase_entry == 0 ||
            runtime_error_entry == 0 ||
            rootfs_install_entry == 0 ||
            command_start_entry == 0 ||
            command_retain_entry == 0 ||
            command_release_entry == 0 ||
            command_write_entry == 0 ||
            command_close_entry == 0 ||
            command_interrupt_entry == 0 ||
            command_cancel_entry == 0 ||
            command_wait_entry == 0;
}

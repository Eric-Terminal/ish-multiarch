#ifndef ISH_APPLE_WATCH_RUNTIME_PRIVATE_H
#define ISH_APPLE_WATCH_RUNTIME_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

#include "util/sync.h"

#define WATCH_CONSOLE_NUMBER 1
#define WATCH_COMMAND_LIMIT 4096

struct tgroup;
struct tty;
struct command_arguments;

// Apple 可见 session、结构化命令与托管文件入口共用，避免 prepared task 交错。
extern lock_t ish_watch_prepared_task_lock;

int ish_watch_runtime_operation_availability(void);
int exec_shell_command(
        const char *command,
        size_t command_length) __attribute__((visibility("hidden")));
void ish_watch_terminal_install_console(void)
        __attribute__((visibility("hidden")));
void ish_watch_session_handle_exit(
        struct tgroup *group,
        int32_t wait_status,
        struct tty *controlling_tty) __attribute__((visibility("hidden")));
int ish_watch_session_create_process(
        const struct command_arguments *arguments,
        uint16_t columns,
        uint16_t rows,
        uint64_t *session_id) __attribute__((visibility("hidden")));

#endif

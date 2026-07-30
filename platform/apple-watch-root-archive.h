#ifndef PLATFORM_APPLE_WATCH_ROOT_ARCHIVE_H
#define PLATFORM_APPLE_WATCH_ROOT_ARCHIVE_H

#include <stdbool.h>

#include "platform/apple-root-catalog.h"
#include "tools/fakefs.h"

#define ISH_APPLE_WATCH_ROOT_ARCHIVE_MESSAGE_CAPACITY 256

struct ish_apple_watch_root_archive_error {
    int kind;
    int code;
    int line;
    char message[ISH_APPLE_WATCH_ROOT_ARCHIVE_MESSAGE_CAPACITY];
};

// 仅接受 Shared 目录中的普通 .tar、.tar.gz 或 .tgz 文件名。
bool ish_apple_watch_root_archive_is_supported_name(const char *name);

// 启动时清理上次崩溃遗留的私有 snapshot、fakefs 与 export partial。
int ish_apple_watch_root_archive_cleanup(
        const char *persistent_parent);

int ish_apple_watch_root_archive_import(
        const char *seed_root,
        const char *persistent_parent,
        const char *shared_directory,
        const char *archive_name,
        char root_name[ISH_APPLE_ROOT_NAME_CAPACITY],
        struct progress progress,
        struct ish_apple_watch_root_archive_error *error_out);

/*
 * 非活动 root 直接在宿主导出；active_name 非空且匹配时返回 EBUSY。
 * output_name 必须是安全的 .tar.gz 文件名，且不会覆盖 Shared 现有条目。
 */
int ish_apple_watch_root_archive_export(
        const char *persistent_parent,
        const char *root_name,
        const char *active_name,
        const char *shared_directory,
        const char *output_name,
        struct progress progress,
        struct ish_apple_watch_root_archive_error *error_out);

#endif

#ifndef ISH_APPLE_WATCH_RUNTIME_PRIVATE_H
#define ISH_APPLE_WATCH_RUNTIME_PRIVATE_H

#include "util/sync.h"

// Apple 可见 session、结构化命令与托管文件入口共用，避免 prepared task 交错。
extern lock_t ish_watch_prepared_task_lock;

int ish_watch_runtime_operation_availability(void);

#endif

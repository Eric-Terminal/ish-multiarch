#ifndef ISH_APPLE_WATCH_RUNTIME_PRIVATE_H
#define ISH_APPLE_WATCH_RUNTIME_PRIVATE_H

#include "util/sync.h"

// Watch session 与托管 guest 文件入口共用此锁，避免 prepared task 事务交错。
extern lock_t ish_watch_prepared_task_lock;

int ish_watch_runtime_operation_availability(void);

#endif

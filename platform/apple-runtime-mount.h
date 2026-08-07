#ifndef PLATFORM_APPLE_RUNTIME_MOUNT_H
#define PLATFORM_APPLE_RUNTIME_MOUNT_H

#include <stdint.h>

struct ish_apple_mount_spec_v1;
struct task;

/* runtime v2 在启动前复制所有 fd；失败时不留下部分注册。 */
int ish_apple_mount_prepare_startup(
        const struct ish_apple_mount_spec_v1 *specs,
        uint32_t count) __attribute__((visibility("hidden")));

/* 调用时首个 task 尚未发布，允许在其 guest namespace 中建立 mount point。 */
int ish_apple_mount_activate_startup(struct task *task)
        __attribute__((visibility("hidden")));

/* success 仅解除启动事务标记；失败会卸载并释放本次启动的所有条目。 */
void ish_apple_mount_finish_startup(int success)
        __attribute__((visibility("hidden")));

#endif

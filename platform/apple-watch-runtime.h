#ifndef PLATFORM_APPLE_WATCH_RUNTIME_H
#define PLATFORM_APPLE_WATCH_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Watch runtime 与现有 iSH 全局内核状态一致：每个宿主进程只能启动一次。
enum ish_watch_runtime_phase {
    ISH_WATCH_RUNTIME_IDLE = 0,
    ISH_WATCH_RUNTIME_PREPARING = 1,
    ISH_WATCH_RUNTIME_RUNNING = 2,
    ISH_WATCH_RUNTIME_STOPPED = 3,
    ISH_WATCH_RUNTIME_FAILED = 4,
};

// 路径均属于宿主文件系统。成功返回 0，失败返回负 Linux errno。
int ish_watch_runtime_start(
        const char *seed_root,
        const char *persistent_parent,
        const char *socket_prefix);

int ish_watch_runtime_current_phase(void);
int ish_watch_runtime_last_error(void);

// 输出是可见 console 的原始字节；dropped_bytes 返回并清零溢出计数。
size_t ish_watch_runtime_read_output(
        void *buffer, size_t capacity, uint64_t *dropped_bytes);

// 输入返回已消费字节数，失败返回负 Linux errno；调用方负责续送剩余字节。
ssize_t ish_watch_runtime_send_input(const void *bytes, size_t length);
// 窗口尺寸只作用于可见 console。成功返回 0，失败返回负 Linux errno。
int ish_watch_runtime_set_window_size(uint16_t columns, uint16_t rows);

#ifdef ISH_APPLE_WATCH_RUNTIME_TESTING
void ish_watch_runtime_test_append_output(const void *bytes, size_t length);
#endif

#endif

#ifndef KERNEL_INIT_H
#define KERNEL_INIT_H

#include <stdbool.h>

#include "fs/tty.h"

struct task;

// Incredibly sloppy. Please do not reference as an example of good API design.
int mount_root(const struct fs_ops *fs, const char *source);
void set_console_device(int major, int minor);
int become_first_process(void);
int become_new_init_child(void);
/*
 * 宿主入口可在 begin 与 commit/cancel 之间完成 stdio 和 exec。
 * 成功 begin 后 current 指向尚未发布的 task；子进程事务还会阻止 PID 1
 * 在事务期间开始退出。
 * 调用方必须在同一 host 线程恰好调用一次 commit 或 cancel。
 */
int begin_first_process(void);
int begin_new_init_child(void);
void cancel_prepared_process(void);
// commit 失败时事务仍保持，调用方必须 cancel。
int commit_prepared_process(void);
// 子进程专用入口额外校验当前事务类型。
void cancel_new_init_child(void);
int commit_new_init_child(void);
// runtime 停止后回收无父进程的 PID 1；执行线程尚未完全退出时返回 EAGAIN。
int reap_stopped_first_process(void);
// do_exit 用这对入口把 PID 1 的 exiting 发布与上述事务串行。
bool init_child_lifecycle_begin_exit(struct task *task);
void init_child_lifecycle_end_exit(bool held);
void create_some_device_nodes(void);
int create_stdio(const char *file, int major, int minor);
/*
 * 将三个互异的宿主 fd 精确安装为目标 task 的 0/1/2。
 * 调用方必须与目标 task 的启动和 fdtable 修改串行；本函数无论成败都接管
 * 三个宿主 fd，失败时会撤销本次已经安装的全部描述符。
 */
int create_host_stdio(
        struct task *task,
        int stdin_fd, int stdout_fd, int stderr_fd);
int create_piped_stdio(void);

#endif

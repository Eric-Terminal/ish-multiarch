#ifndef KERNEL_INIT_H
#define KERNEL_INIT_H

#include "fs/tty.h"

struct task;

// Incredibly sloppy. Please do not reference as an example of good API design.
int mount_root(const struct fs_ops *fs, const char *source);
void set_console_device(int major, int minor);
int become_first_process(void);
int become_new_init_child(void);
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

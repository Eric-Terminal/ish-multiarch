#ifndef FS_PIPE_H
#define FS_PIPE_H

#include "fs/fd.h"

struct task;

struct file_pipe_result {
    fd_t fds[2];
    qword_t generations[2];
    // 强引用用于在 guest 写回失败时辨认原表项，调用方必须释放。
    struct fd *ends[2];
};

// 成功时返回两个已安装描述符；失败时目标 task 不保留任何一端。
int file_pipe2_task(
        struct task *task, int flags, struct file_pipe_result *result);
void file_pipe_result_release(struct file_pipe_result *result);
void file_pipe_result_rollback(
        struct task *task, struct file_pipe_result *result);

#endif

// iOS 挂起应用后可能销毁监听 fd 背后的 socket，使既有操作立即失败，
// 而阻塞的 accept 也不会自行醒来。这里记录监听 socket 与等待任务：
// 挂起时保存地址和配置，恢复时保留仍有效的 socket，或仅在原 raw fd
// 仍空闲时原位重建，最后唤醒相关等待并让其重新检查状态。
// https://developer.apple.com/library/archive/technotes/tn2277/_index.html
#ifndef FS_SOCKRESTART_H
#define FS_SOCKRESTART_H
#include <stdbool.h>
#include "util/list.h"
struct fd;
struct task;

void sockrestart_begin_listen(struct fd *sock);
void sockrestart_end_listen(struct fd *sock);
void sockrestart_begin_listen_wait(struct task *task, struct fd *sock);
void sockrestart_end_listen_wait(struct task *task, struct fd *sock);
bool sockrestart_should_restart_listen_wait(struct task *task);
void sockrestart_on_suspend(void);
void sockrestart_on_resume(void);

struct fd_sockrestart {
    struct list listen;
};

struct task_sockrestart {
    int count;
    bool punt;
    struct list listen;
};

#endif

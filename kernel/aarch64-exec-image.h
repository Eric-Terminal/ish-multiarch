#ifndef KERNEL_AARCH64_EXEC_IMAGE_H
#define KERNEL_AARCH64_EXEC_IMAGE_H

#include "misc.h"

struct fd;
struct guest_file_source;
struct task;

struct ish_aarch64_exec_image {
    void *data;
    size_t size;
    struct guest_file_source *file_source;
};

struct ish_aarch64_exec_images {
    struct ish_aarch64_exec_image main;
    struct ish_aarch64_exec_image interpreter;
};

/*
 * 在目标 task 自己的执行线程内建立两份完整快照；main_fd 保持借用，
 * 调用方须已校验主程序执行权限。images 是仅写输出，成功结果在复用前
 * 必须 destroy；成功和失败都不会泄漏 guest 文件系统的 fd 生命周期。
 */
int ish_aarch64_exec_images_read(struct task *task, struct fd *main_fd,
        struct ish_aarch64_exec_images *images);
void ish_aarch64_exec_images_destroy(
        struct ish_aarch64_exec_images *images);

#endif

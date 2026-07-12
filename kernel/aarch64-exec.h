#ifndef KERNEL_AARCH64_EXEC_H
#define KERNEL_AARCH64_EXEC_H

#include "misc.h"

struct fd;
struct task;

#define ISH_AARCH64_EXEC_ARG_MAX UINT32_C(131072)

struct ish_aarch64_exec_identity {
    dword_t uid;
    dword_t euid;
    dword_t gid;
    dword_t egid;
    dword_t secure;
};

/*
 * arguments/environment 是由 count 个 NUL 结尾字符串连续组成的 host 快照；
 * 成功时 task 接管完整候选，调用方仍持有 main_fd 与输入缓冲区。
 */
int ish_aarch64_exec_stage(struct task *task, struct fd *main_fd,
        const char *executable,
        size_t argument_count, const char *arguments,
        size_t environment_count, const char *environment,
        const struct ish_aarch64_exec_identity *identity);

#endif

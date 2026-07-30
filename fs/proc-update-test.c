#include <stdio.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/proc.h"
#include "kernel/errno.h"
#include "kernel/fs.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "proc 可更新节点测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

extern const struct fd_ops procfs_fdops;

static unsigned update_calls;
static int update_result;

static int accept_update(
        struct proc_entry *UNUSED(entry),
        struct proc_data *UNUSED(data)) {
    update_calls++;
    return update_result;
}

int main(void) {
    struct proc_dir_entry writable_meta = {
        .name = "writable",
        .update = accept_update,
    };
    struct fd writable = {
        .proc.entry.meta = &writable_meta,
    };
    CHECK(procfs.fsetattr(
                    &writable, make_attr(size, 0)) == 0 &&
                update_calls == 0,
            "可更新普通节点接受 O_TRUNC 的零长度 no-op");
    static const char json[] = "true";
    CHECK(procfs_fdops.pwrite(
                    &writable, json, sizeof(json) - 1, 0) ==
                    (ssize_t) (sizeof(json) - 1) &&
                update_calls == 1,
            "截断 no-op 后的正文仍交给更新回调");
    update_result = _EINVAL;
    CHECK(procfs_fdops.pwrite(
                    &writable, json, sizeof(json) - 1, 0) == _EINVAL &&
                update_calls == 2,
            "更新回调的拒绝结果必须传播给写入方");
    CHECK(procfs.fsetattr(
                    &writable, make_attr(size, 1)) == _EPERM,
            "可更新普通节点仍拒绝非零长度属性更新");
    CHECK(procfs.fsetattr(
                    &writable, make_attr(mode, 0600)) == _EPERM,
            "可更新普通节点仍拒绝其他属性更新");

    struct proc_dir_entry readonly_meta = {
        .name = "readonly",
    };
    struct fd readonly = {
        .proc.entry.meta = &readonly_meta,
    };
    CHECK(procfs.fsetattr(
                    &readonly, make_attr(size, 0)) == _EPERM,
            "只读普通节点拒绝零长度属性更新");

    struct proc_dir_entry directory_meta = {
        .name = "directory",
        .mode = S_IFDIR,
        .update = accept_update,
    };
    struct fd directory = {
        .proc.entry.meta = &directory_meta,
    };
    CHECK(procfs.fsetattr(
                    &directory, make_attr(size, 0)) == _EPERM,
            "目录节点不能借助更新回调接受截断");
    return 0;
}

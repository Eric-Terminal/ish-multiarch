#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/proc.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "proc fd 生命周期测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static struct task *path_task;
static bool path_callback_saw_released_locks;

static int test_getpath(struct fd *UNUSED(fd), char *buffer) {
    bool pid_lock_released = trylock(&pids_lock) == 0;
    if (pid_lock_released)
        unlock(&pids_lock);

    bool files_lock_released = trylock(&path_task->files->lock) == 0;
    if (files_lock_released)
        unlock(&path_task->files->lock);

    path_callback_saw_released_locks =
            pid_lock_released && files_lock_released;
    strcpy(buffer, "/fixture");
    return 0;
}

static const struct fd_ops test_fd_ops = {0};

static const struct fs_ops test_fs = {
    .getpath = test_getpath,
};

static bool find_child(struct proc_entry *directory,
        const char *name, struct proc_entry *child) {
    unsigned long index = 0;
    struct proc_entry candidate = {0};
    while (proc_dir_read(directory, &index, &candidate)) {
        if (candidate.meta->name != NULL &&
                strcmp(candidate.meta->name, name) == 0) {
            *child = candidate;
            return true;
        }
    }
    return false;
}

int main(void) {
    struct task *task = task_create_(NULL);
    CHECK(task != NULL, "创建测试任务");

    struct tgroup group = {0};
    list_init(&group.threads);
    list_init(&group.session);
    list_init(&group.pgroup);
    lock_init(&group.lock);
    cond_init(&group.stopped_cond);
    cond_init(&group.child_exit);
    group.leader = task;
    group.sid = task->pid;
    group.pgid = task->pid;
    group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {1, 1};
    task->group = &group;
    task->tgid = task->pid;
    task->files = fdtable_new(1);
    CHECK(!IS_ERR(task->files), "创建测试 fd 表");
    task_publish(task);

    struct mount mount = {
        .point = "",
        .fs = &test_fs,
    };
    struct fd *fd = fd_create(&test_fd_ops);
    CHECK(fd != NULL, "创建测试文件对象");
    fd->mount = &mount;
    mount_retain(&mount);
    CHECK(f_install_task(task, fd, 0) == 0, "安装测试文件对象");

    struct proc_entry process_directory = {
        .meta = &proc_pid,
        .pid = task->pid,
    };
    struct proc_entry fd_directory = {0};
    CHECK(find_child(&process_directory, "fd", &fd_directory),
            "找到目标任务的 fd 目录");

    unsigned long index = 0;
    struct proc_entry fd_link = {0};
    CHECK(proc_dir_read(&fd_directory, &index, &fd_link) &&
            fd_link.fd == 0 && fd_link.meta->readlink != NULL,
            "枚举 fd 软链接目录项");

    path_task = task;
    char path[MAX_PATH + 1] = {0};
    CHECK(fd_link.meta->readlink(&fd_link, path) == 0 &&
            strcmp(path, "/fixture") == 0,
            "有效 fd 返回稳定路径");
    CHECK(path_callback_saw_released_locks,
            "路径回调期间不持有 PID 与 fd 表锁");

    CHECK(f_close_task(task, 0) == 0, "关闭已枚举的 fd");
    CHECK(fd_link.meta->readlink(&fd_link, path) == _ENOENT,
            "已关闭的陈旧 fd 目录项返回 ENOENT");
    CHECK(mount.refcount == 0, "关闭陈旧目录项释放文件对象引用");

    fdtable_release(task->files);
    cond_destroy(&task->pause);
    cond_destroy(&task->ptrace.cond);
    lock(&pids_lock);
    lock(&group.lock);
    list_remove(&task->group_links);
    list_remove(&group.session);
    list_remove(&group.pgroup);
    task_destroy(task);
    unlock(&group.lock);
    unlock(&pids_lock);
    cond_destroy(&group.child_exit);
    cond_destroy(&group.stopped_cond);
    return 0;
}

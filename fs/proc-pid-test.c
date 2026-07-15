#include <stdio.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/proc.h"
#include "kernel/calls.h"
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

static bool root_contains_process(pid_t_ pid) {
    struct proc_entry root = {
        .meta = &proc_root,
    };
    unsigned long index = 0;
    struct proc_entry candidate = {0};
    while (proc_dir_read(&root, &index, &candidate)) {
        if (candidate.meta == &proc_pid && candidate.pid == pid)
            return true;
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

    lock(&pids_lock);
    task->exiting = true;
    unlock(&pids_lock);
    unsigned long stale_index = 0;
    struct proc_entry stale_fd_link = {0};
    bool stale_directory_finished =
            !proc_dir_read(&fd_directory, &stale_index, &stale_fd_link);
    lock(&pids_lock);
    task->exiting = false;
    unlock(&pids_lock);
    CHECK(stale_directory_finished,
            "任务消失后陈旧 fd 目录结束枚举");

    struct task *execer = task_create_(NULL);
    CHECK(execer != NULL, "创建 exec 换位窗口任务");
    execer->group = &group;
    execer->tgid = task->pid;
    execer->uid = 123;
    execer->gid = 456;
    strcpy(execer->comm, "exec-window");
    execer->sighand = sighand_new();
    CHECK(execer->sighand != NULL, "创建 execer 信号表");
    task_publish(execer);

    // 旧 TGID 槽仍指向退出中的 leader，进程访问应跟随 execer。
    lock(&pids_lock);
    lock(&group.lock);
    group.exec_task = task;
    bool live_tid_stable =
            pid_get_process_task(execer->pid) == execer;
    group.exec_task = execer;
    task->exiting = true;
    unlock(&group.lock);
    unlock(&pids_lock);
    CHECK(live_tid_stable,
            "exec 窗口不把存活 peer TID 别名为执行者");
    current = execer;

    struct statbuf process_stat = {0};
    CHECK(proc_entry_stat(&process_directory, &process_stat) == 0 &&
            process_stat.uid == execer->uid &&
            process_stat.gid == execer->gid,
            "TGID 目录属性跟随换位中的进程代表");
    CHECK(root_contains_process(task->pid),
            "proc 根目录保留换位中的 TGID");
    CHECK(sys_getpgid(task->pid) == group.pgid,
            "getpgid 通过 TGID 找到换位中的进程");

    struct proc_entry stat_entry = {0};
    CHECK(find_child(&process_directory, "stat", &stat_entry),
            "找到换位中进程的 stat 文件");
    char stat_text[4096];
    struct proc_data stat_data = {
        .data = stat_text,
        .capacity = sizeof(stat_text),
    };
    CHECK(stat_entry.meta->show(&stat_entry, &stat_data) == 0 &&
            stat_data.size < sizeof(stat_text),
            "读取换位中进程的 stat");
    stat_text[stat_data.size] = '\0';
    char expected_prefix[32];
    snprintf(expected_prefix, sizeof(expected_prefix), "%d ", task->pid);
    CHECK(strncmp(stat_text, expected_prefix,
                    strlen(expected_prefix)) == 0 &&
            strstr(stat_text, "(exec-window)") != NULL,
            "stat 使用请求 TGID 并读取 execer 状态");

    lock(&pids_lock);
    task->exiting = false;
    execer->exiting = true;
    group.exec_task = task;
    bool exiting_tid_hidden =
            pid_get_process_task(execer->pid) == NULL;
    execer->exiting = false;
    task->exiting = true;
    group.exec_task = execer;
    unlock(&pids_lock);
    CHECK(exiting_tid_hidden,
            "退出中的非 leader TID 不别名为其他组成员");

    fdtable_release(task->files);
    sighand_release(execer->sighand);
    cond_destroy(&execer->pause);
    cond_destroy(&execer->ptrace.cond);
    cond_destroy(&task->pause);
    cond_destroy(&task->ptrace.cond);
    lock(&pids_lock);
    lock(&group.lock);
    group.exec_task = NULL;
    list_remove(&execer->group_links);
    list_remove(&task->group_links);
    list_remove(&group.session);
    list_remove(&group.pgroup);
    task_destroy(execer);
    task_destroy(task);
    unlock(&group.lock);
    unlock(&pids_lock);
    current = NULL;
    cond_destroy(&group.child_exit);
    cond_destroy(&group.stopped_cond);
    return 0;
}

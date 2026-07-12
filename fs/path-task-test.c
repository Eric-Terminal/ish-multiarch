#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "目标任务路径测试失败：%s（第 %d 行）\n", message, __LINE__); \
        return 1; \
    } \
} while (0)

struct open_probe {
    char last_path[MAX_PATH];
    char last_stat_path[MAX_PATH];
    char last_fstat_path[MAX_PATH];
    char last_unlink_path[MAX_PATH];
    char last_rmdir_path[MAX_PATH];
    int last_flags;
    int last_mode;
    unsigned opens;
    unsigned closes;
    unsigned unlinks;
    unsigned rmdirs;
    uid_t_ owner;
    uid_t_ group;
    mode_t_ file_mode;
};

struct file_state {
    struct open_probe *probe;
    const char *path;
    bool owned_path;
};

struct task_fixture {
    struct task task;
    struct tgroup group;
    struct fs_info fs;
    struct fd *pwd;
    struct fd *root;
};

static int probe_close(struct fd *fd) {
    struct file_state *state = fd->data;
    if (state != NULL && state->owned_path) {
        state->probe->closes++;
        free((void *) state->path);
        free(state);
    }
    return 0;
}

static const struct fd_ops probe_fd_ops = {
    .close = probe_close,
};

static struct fd *probe_open(struct mount *mount,
        const char *path, int flags, int mode) {
    struct open_probe *probe = mount->data;
    probe->opens++;
    strcpy(probe->last_path, path);
    probe->last_flags = flags;
    probe->last_mode = mode;

    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    struct file_state *state = malloc(sizeof(*state));
    if (state == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    char *saved_path = strdup(path);
    if (saved_path == NULL) {
        free(state);
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    *state = (struct file_state) {
        .probe = probe,
        .path = saved_path,
        .owned_path = true,
    };
    fd->data = state;
    return fd;
}

static int probe_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct open_probe *probe = mount->data;
    strcpy(probe->last_stat_path, path);
    mode_t_ mode = S_IFDIR |
            (strcmp(path, "/target/guard") == 0 ? 0100 : 0711);
    if (strcmp(path, "/target/created") == 0)
        mode = S_IFREG | 0711;
    *stat = (struct statbuf) {
        .inode = 1,
        .mode = mode,
        .uid = probe->owner,
        .gid = probe->group,
    };
    return 0;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct file_state *state = fd->data;
    strcpy(state->probe->last_fstat_path, state->path);
    *stat = (struct statbuf) {
        .inode = 2,
        .mode = state->probe->file_mode,
        .uid = state->probe->owner,
        .gid = state->probe->group,
    };
    return 0;
}

static int probe_getpath(struct fd *fd, char *buffer) {
    struct file_state *state = fd->data;
    strcpy(buffer, state->path);
    return 0;
}

static ssize_t probe_readlink(struct mount *mount,
        const char *path, char *buffer, size_t size) {
    (void) mount;
    const char *target;
    if (strcmp(path, "/target/link") == 0)
        target = "/absolute";
    else if (strcmp(path, "/target/escape-link") == 0)
        target = "/../escape";
    else
        return _EINVAL;
    size_t length = strlen(target);
    if (length > size)
        length = size;
    memcpy(buffer, target, length);
    return (ssize_t) length;
}

static int probe_unlink(struct mount *mount, const char *path) {
    struct open_probe *probe = mount->data;
    probe->unlinks++;
    strcpy(probe->last_unlink_path, path);
    return 0;
}

static int probe_rmdir(struct mount *mount, const char *path) {
    struct open_probe *probe = mount->data;
    probe->rmdirs++;
    strcpy(probe->last_rmdir_path, path);
    return 0;
}

static const struct fs_ops probe_fs = {
    .open = probe_open,
    .readlink = probe_readlink,
    .unlink = probe_unlink,
    .rmdir = probe_rmdir,
    .stat = probe_stat,
    .fstat = probe_fstat,
    .getpath = probe_getpath,
};

static struct fd *make_path_fd(struct mount *mount,
        struct file_state *state, struct open_probe *probe,
        const char *path) {
    struct fd *fd = fd_create(&probe_fd_ops);
    if (fd == NULL)
        return NULL;
    *state = (struct file_state) {
        .probe = probe,
        .path = path,
    };
    fd->data = state;
    fd->type = S_IFDIR;
    fd->mount = mount;
    mount_retain(mount);
    return fd;
}

static bool init_fixture(struct task_fixture *fixture, struct mount *mount,
        struct open_probe *probe, const char *pwd_path, const char *root_path,
        struct file_state *pwd_state, struct file_state *root_state,
        rlim_t_ nofile, mode_t_ umask, uid_t_ uid, uid_t_ gid) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {nofile, nofile};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(1);
    if (IS_ERR(fixture->task.files))
        return false;
    lock_init(&fixture->fs.lock);
    fixture->fs.umask = umask;
    fixture->task.fs = &fixture->fs;
    fixture->task.euid = uid;
    fixture->task.egid = gid;
    fixture->pwd = make_path_fd(mount, pwd_state, probe, pwd_path);
    fixture->root = make_path_fd(mount, root_state, probe, root_path);
    if (fixture->pwd == NULL || fixture->root == NULL)
        return false;
    fixture->fs.pwd = fixture->pwd;
    fixture->fs.root = fixture->root;
    return true;
}

int main(void) {
    struct open_probe probe = {
        .owner = 1000,
        .group = 100,
        .file_mode = S_IFREG | 0400,
    };
    lock(&mounts_lock);
    int mount_error = do_mount(&probe_fs, "", "", "", 0);
    unlock(&mounts_lock);
    CHECK(mount_error == 0, "测试根文件系统挂载成功");
    char root_lookup[] = "/";
    struct mount *mount = mount_find(root_lookup);
    mount->data = &probe;

    struct fs_info bootstrap_fs = {0};
    lock_init(&bootstrap_fs.lock);
    struct task bootstrap_task = {.fs = &bootstrap_fs};
    char normalized[MAX_PATH];
    CHECK(path_normalize_task(&bootstrap_task, AT_PWD, "/", normalized,
            N_SYMLINK_FOLLOW) == 0 && normalized[0] == '\0',
            "首个任务可在安装 root 前解析绝对根路径");
    CHECK(path_normalize_task(&bootstrap_task, AT_PWD, "bootstrap", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/bootstrap") == 0,
            "首个任务可在安装 cwd 前从根路径解析相对名称");

    struct task_fixture decoy;
    struct task_fixture target;
    struct file_state decoy_pwd_state;
    struct file_state decoy_root_state;
    struct file_state target_pwd_state;
    struct file_state target_root_state;
    CHECK(init_fixture(&decoy, mount, &probe, "/decoy", "/decoy-root",
            &decoy_pwd_state, &decoy_root_state, 1, 0777, 2000, 200),
            "诱饵任务初始化成功");
    CHECK(init_fixture(&target, mount, &probe, "/target", "/sandbox",
            &target_pwd_state, &target_root_state, 4, 0027, 1000, 100),
            "目标任务初始化成功");

    struct file_state explicit_state;
    struct fd *explicit_dir = make_path_fd(
            mount, &explicit_state, &probe, "/explicit");
    CHECK(explicit_dir != NULL, "显式目录 fd 创建成功");
    CHECK(f_install_task(&target.task, explicit_dir, 0) == 0,
            "显式目录 fd 安装成功");
    current = &decoy.task;

    CHECK(path_normalize_task(&target.task, AT_PWD, "child", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/target/child") == 0,
            "相对路径使用目标任务 cwd");
    CHECK(path_normalize_task(&target.task, AT_PWD, "/absolute", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/sandbox/absolute") == 0,
            "绝对路径使用目标任务 root");
    CHECK(path_normalize_task(&target.task, explicit_dir, "child", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/explicit/child") == 0,
            "相对路径使用显式目录 fd");
    CHECK(path_normalize_task(&target.task, AT_PWD, "link", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/sandbox/absolute") == 0,
            "绝对符号链接仍受目标任务 root 约束");
    CHECK(path_normalize_task(&target.task, AT_PWD, "link/child", normalized,
            N_SYMLINK_FOLLOW) == 0 &&
            strcmp(normalized, "/sandbox/absolute/child") == 0,
            "符号链接递归保留后续路径分量");
    CHECK(path_normalize_task(&target.task, AT_PWD, "/../escape", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/sandbox/escape") == 0,
            "绝对路径的 .. 不得越过目标 root");
    CHECK(path_normalize_task(&target.task, AT_PWD, "escape-link", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/sandbox/escape") == 0,
            "绝对符号链接的 .. 不得越过目标 root");
    CHECK(path_normalize_task(&target.task, AT_PWD, "guard/child", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/target/guard/child") == 0,
            "中间目录执行权限使用目标任务凭据");
    CHECK(path_normalize_task(&decoy.task, AT_PWD, "../escape", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/escape") == 0,
            "cwd 位于 root 外时保留 Linux 的相对路径行为");
    CHECK(path_normalize_task(&target.task, AT_PWD,
            "../sandbox/dir/../../escape", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/sandbox/escape") == 0,
            "从 root 外重新进入 root 后动态恢复 .. 边界");
    CHECK(path_normalize_task(&target.task, explicit_dir, "../escape", normalized,
            N_SYMLINK_FOLLOW) == 0 && strcmp(normalized, "/escape") == 0,
            "显式 dirfd 位于 root 外时允许相对回退");
    CHECK(path_normalize(AT_PWD, "child", normalized, N_SYMLINK_FOLLOW) == 0 &&
            strcmp(normalized, "/decoy/child") == 0,
            "兼容路径入口仍使用 current");
    CHECK(path_normalize_task(&target.task, AT_PWD, "", normalized,
            N_SYMLINK_FOLLOW) == _ENOENT, "空路径返回 ENOENT");

    struct statbuf permission = {
        .mode = S_IFREG | 0400,
        .uid = probe.owner,
        .gid = probe.group,
    };
    CHECK(access_check_task(&target.task, &permission, AC_R) == 0,
            "权限检查使用目标任务 euid");
    CHECK(access_check_task(&decoy.task, &permission, AC_R) == _EACCES,
            "权限检查不得使用 current 以外的凭据");
    decoy.task.euid = 0;
    CHECK(access_check(&permission, AC_W) == 0, "兼容权限入口保留超级用户语义");
    decoy.task.euid = 2000;
    const int sync_flags = (1 << 20) | (1 << 12);
    struct fd *sync_read = generic_openat_task(
            &target.task, AT_PWD, "sync-read", sync_flags, 0);
    CHECK(!IS_ERR(sync_read) && probe.last_flags == sync_flags,
            "guest O_SYNC 位不改变只读打开的权限类别");
    fd_close(sync_read);

    struct statbuf target_stat;
    memset(&target_stat, 0xff, sizeof(target_stat));
    CHECK(generic_statat(AT_PWD, "child", &target_stat, true) == 0 &&
            strcmp(probe.last_stat_path, "/decoy/child") == 0,
            "兼容 statat 入口仍使用 current 的 cwd");
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "child", 0,
            &target_stat) == 0 && target_stat.inode == 1 &&
            strcmp(probe.last_stat_path, "/target/child") == 0,
            "statat 相对路径使用目标任务 cwd");
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "link", 0,
            &target_stat) == 0 &&
            strcmp(probe.last_stat_path, "/sandbox/absolute") == 0,
            "statat 默认跟随末尾符号链接并受目标 root 约束");
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "link",
            AT_SYMLINK_NOFOLLOW_, &target_stat) == 0 &&
            strcmp(probe.last_stat_path, "/target/link") == 0,
            "statat 可选择不跟随末尾符号链接");
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "link/",
            AT_SYMLINK_NOFOLLOW_, &target_stat) == 0 &&
            strcmp(probe.last_stat_path, "/sandbox/absolute") == 0,
            "末尾斜杠强制 statat 跟随符号链接并检查目录");
    CHECK(file_statat_task(&target.task, 0, "nested", AT_NO_AUTOMOUNT_,
            &target_stat) == 0 &&
            strcmp(probe.last_stat_path, "/explicit/nested") == 0,
            "statat 使用目标任务 dirfd 并接受 NO_AUTOMOUNT");
    CHECK(file_statat_task(&target.task, 0, "nested", AT_STATX_DONT_SYNC_,
            &target_stat) == 0,
            "statat 接受由底层文件系统决定的属性同步提示");
    CHECK(file_statat_task(&target.task, 99, "relative", 0,
            &target_stat) == _EBADF, "statat 相对路径拒绝无效 dirfd");
    CHECK(file_statat_task(&target.task, 99, "/absolute", 0,
            &target_stat) == 0 &&
            strcmp(probe.last_stat_path, "/sandbox/absolute") == 0,
            "statat 绝对路径忽略无效 dirfd");
    CHECK(file_statat_task(&target.task, 0, "", AT_EMPTY_PATH_,
            &target_stat) == 0 && target_stat.inode == 2 &&
            strcmp(probe.last_fstat_path, "/explicit") == 0,
            "statat 的 EMPTY_PATH 直接查询目标任务 dirfd");
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "", AT_EMPTY_PATH_,
            &target_stat) == 0 &&
            strcmp(probe.last_fstat_path, "/target") == 0,
            "statat 的 EMPTY_PATH 可查询目标任务 cwd");
    CHECK(file_statat_task(&target.task, 99, "", AT_EMPTY_PATH_,
            &target_stat) == _EBADF,
            "statat 的 EMPTY_PATH 拒绝无效 dirfd");
    CHECK(file_statat_task(&target.task, 99, "", 0,
            &target_stat) == _ENOENT,
            "statat 空路径在检查 dirfd 前返回 ENOENT");
    memset(&target_stat, 0xff, sizeof(target_stat));
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "", INT32_MIN,
            &target_stat) == _EINVAL && target_stat.inode == 0,
            "statat 拒绝未知 flags 并清零 host 结果");

    unsigned opens_before = probe.opens;
    CHECK(file_openat_task(&target.task, 99, "",
            O_WRONLY_ | O_RDWR_, 0) == _EINVAL && probe.opens == opens_before,
            "兼容入口保留 access mode 3 的 EINVAL 优先级");

    fd_t cwd_fd = file_openat_task(&target.task, AT_FDCWD_, "created",
            O_CREAT_ | O_CLOEXEC_ | O_NONBLOCK_, S_IFREG | 0777);
    CHECK(cwd_fd == 1 && strcmp(probe.last_path, "/target/created") == 0,
            "openat 使用目标 cwd 并安装到目标表");
    CHECK(probe.last_mode == 0750 &&
            probe.last_flags == (O_CREAT_ | O_CLOEXEC_ | O_NONBLOCK_),
            "openat 使用目标 umask 且保留 flags");
    struct fd *opened = f_get_task(&target.task, cwd_fd);
    CHECK(opened != NULL && bit_test(cwd_fd, target.task.files->cloexec) &&
            (fd_getflags(opened) & O_NONBLOCK_), "目标表保存 CLOEXEC 与 NONBLOCK");
    CHECK(f_get(0) == NULL, "目标 openat 不修改 current 文件表");
    opens_before = probe.opens;
    CHECK(file_openat_task(&target.task, cwd_fd, "child", O_RDONLY_, 0) ==
            _ENOTDIR && probe.opens == opens_before,
            "相对 openat 拒绝普通文件 dirfd");
    CHECK(file_statat_task(&target.task, cwd_fd, "child", 0,
            &target_stat) == _ENOTDIR,
            "相对 statat 拒绝普通文件 dirfd");
    CHECK(file_statat_task(&target.task, cwd_fd, "", AT_EMPTY_PATH_,
            &target_stat) == 0 &&
            strcmp(probe.last_fstat_path, "/target/created") == 0,
            "statat 的 EMPTY_PATH 可直接查询普通文件 dirfd");
    CHECK(file_statat_task(&target.task, AT_FDCWD_, "created/", 0,
            &target_stat) == _ENOTDIR,
            "statat 拒绝带末尾斜杠的普通文件路径");

    fd_t explicit_fd = file_openat_task(
            &target.task, 0, "nested", O_RDONLY_, 0);
    CHECK(explicit_fd == 2 && strcmp(probe.last_path, "/explicit/nested") == 0,
            "openat 使用目标任务的显式 dirfd");
    opens_before = probe.opens;
    CHECK(file_openat_task(&target.task, 99, "relative", O_RDONLY_, 0) == _EBADF &&
            probe.opens == opens_before, "相对路径拒绝无效 dirfd");
    CHECK(file_openat_task(&target.task, 99, "", O_RDONLY_, 0) == _ENOENT &&
            probe.opens == opens_before, "空路径优先返回 ENOENT");

    fd_t absolute_fd = file_openat_task(
            &target.task, 99, "/absolute", O_RDONLY_, 0);
    CHECK(absolute_fd == 3 && strcmp(probe.last_path, "/sandbox/absolute") == 0,
            "绝对路径忽略无效 dirfd");
    unsigned closes_before = probe.closes;
    CHECK(file_openat_task(&target.task, AT_FDCWD_, "overflow",
            O_RDONLY_, 0) == _EMFILE, "目标 NOFILE 满额时返回 EMFILE");
    CHECK(probe.closes == closes_before + 1,
            "openat 安装失败销毁新建 fd 的唯一引用");

    struct fd *denied = generic_openat(AT_PWD, "denied", O_RDONLY_, 0);
    CHECK(IS_ERR(denied) && PTR_ERR(denied) == _EACCES,
            "兼容 generic_openat 仍按 current 凭据检查权限");

    CHECK(file_unlinkat_task(&target.task, 99, "", false) == _ENOENT &&
            probe.unlinks == 0, "unlinkat 空路径优先返回 ENOENT");
    CHECK(file_unlinkat_task(
            &target.task, AT_FDCWD_, "victim", false) == 0 &&
            strcmp(probe.last_unlink_path, "/target/victim") == 0,
            "unlinkat 相对路径使用目标任务 cwd");
    CHECK(file_unlinkat_task(&target.task, 0, "nested", false) == 0 &&
            strcmp(probe.last_unlink_path, "/explicit/nested") == 0,
            "unlinkat 相对路径使用目标任务显式 dirfd");
    CHECK(file_unlinkat_task(
            &target.task, 99, "relative", false) == _EBADF &&
            probe.unlinks == 2,
            "unlinkat 相对路径拒绝目标任务的无效 dirfd");
    CHECK(file_unlinkat_task(
            &target.task, cwd_fd, "child", false) == _ENOTDIR &&
            probe.unlinks == 2,
            "unlinkat 相对路径拒绝普通文件 dirfd");
    CHECK(file_unlinkat_task(
            &target.task, 99, "/absolute", false) == 0 &&
            strcmp(probe.last_unlink_path, "/sandbox/absolute") == 0,
            "unlinkat 绝对路径使用目标 root 并忽略无效 dirfd");
    CHECK(file_unlinkat_task(&target.task, AT_FDCWD_,
            "directory", true) == 0 && probe.rmdirs == 1 &&
            strcmp(probe.last_rmdir_path, "/target/directory") == 0,
            "unlinkat 的 REMOVEDIR 路径只调用目录删除操作");
    CHECK(file_unlinkat_task(
            &target.task, AT_FDCWD_, "link", true) == 0 &&
            probe.rmdirs == 2 &&
            strcmp(probe.last_rmdir_path, "/target/link") == 0,
            "unlinkat 的 REMOVEDIR 不跟随末尾符号链接");
    CHECK(generic_unlinkat(AT_PWD, "compat") == 0 &&
            probe.unlinks == 4 &&
            strcmp(probe.last_unlink_path, "/decoy/compat") == 0,
            "兼容 unlinkat 入口仍使用 current 的 cwd");

    char cwd[MAX_PATH];
    opens_before = probe.opens;
    CHECK(file_chdir_task(&target.task, "") == _ENOENT &&
            probe.opens == opens_before &&
            fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/target") == 0,
            "chdir 空路径不打开对象且保持目标 cwd");
    closes_before = probe.closes;
    probe.file_mode = S_IFDIR;
    CHECK(file_chdir_task(&target.task, "blocked") == _EACCES &&
            probe.opens == opens_before + 1 &&
            probe.closes == closes_before + 1 &&
            fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/target") == 0,
            "chdir 拒绝没有执行权限的目录且保持目标 cwd");

    opens_before = probe.opens;
    closes_before = probe.closes;
    probe.file_mode = S_IFREG;
    CHECK(file_chdir_task(&target.task, "executable") == _ENOTDIR &&
            probe.opens == opens_before + 1 &&
            probe.closes == closes_before + 1 &&
            fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/target") == 0,
            "chdir 在权限检查前以 ENOTDIR 拒绝普通文件");

    uid_t_ target_euid = target.task.euid;
    target.task.euid = 0;
    opens_before = probe.opens;
    closes_before = probe.closes;
    probe.file_mode = S_IFDIR;
    struct fd *root_directory = generic_open_directory_task(
            &target.task, "root-search");
    CHECK(!IS_ERR(root_directory) && probe.opens == opens_before + 1,
            "root 可按目录搜索语义打开 mode 000 目录");
    fd_close(root_directory);
    CHECK(probe.closes == closes_before + 1,
            "root 目录搜索探针释放唯一 fd 引用");
    target.task.euid = target_euid;

    probe.file_mode = S_IFDIR | 0100;
    CHECK(file_chdir_task(&target.task, "link") == 0 &&
            strcmp(probe.last_path, "/sandbox/absolute") == 0 &&
            probe.last_flags == O_DIRECTORY_,
            "chdir 跟随末端符号链接并允许仅有执行权限的目录");
    target.pwd = target.fs.pwd;
    CHECK(fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/sandbox/absolute") == 0 &&
            fs_getcwd_task(&decoy.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/decoy") == 0,
            "chdir 只更新目标任务 cwd，诱饵任务保持不变");

    current = NULL;
    fdtable_release(decoy.task.files);
    fdtable_release(target.task.files);
    fd_close(decoy.pwd);
    fd_close(decoy.root);
    fd_close(target.pwd);
    fd_close(target.root);
    CHECK(probe.opens == 10 && probe.closes == 10,
            "所有成功、拒绝和超限打开都恰好关闭一次");
    CHECK(mount->refcount == 1, "清理阶段仅保留测试持有的 mount 引用");
    mount_release(mount);
    lock(&mounts_lock);
    int remove_error = mount_remove(mount);
    unlock(&mounts_lock);
    CHECK(remove_error == 0 && list_empty(&mounts), "测试根文件系统卸载成功");
    return 0;
}

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/calls.h"
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

struct path_close_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool entered;
    bool released;
    unsigned observed_refcount;
    unsigned close_calls;
};

struct access_identity_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    const char *path;
    bool entered;
    bool released;
};

struct open_probe {
    char last_path[MAX_PATH];
    char last_stat_path[MAX_PATH];
    char last_fstat_path[MAX_PATH];
    char last_unlink_path[MAX_PATH];
    char last_rmdir_path[MAX_PATH];
    char last_mkdir_path[MAX_PATH];
    char last_rename_source[MAX_PATH];
    char last_rename_destination[MAX_PATH];
    int last_flags;
    int last_mode;
    int last_mkdir_mode;
    unsigned opens;
    unsigned closes;
    unsigned unlinks;
    unsigned rmdirs;
    unsigned mkdirs;
    unsigned renames;
    uid_t_ owner;
    uid_t_ group;
    mode_t_ file_mode;
    qword_t file_size;
    off_t_ requested_size;
    unsigned resize_calls;
    int resize_error;
    bool report_created;
    struct access_identity_gate *access_gate;
};

struct file_state {
    struct open_probe *probe;
    const char *path;
    bool owned_path;
    struct path_close_gate *close_gate;
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
    if (state != NULL && state->close_gate != NULL)
        state->close_gate->close_calls++;
    if (state != NULL && state->owned_path) {
        state->probe->closes++;
        free((void *) state->path);
        free(state);
    }
    return 0;
}

static void wait_for_path_close(struct fd *fd) {
    struct file_state *state = fd->data;
    struct path_close_gate *gate = state->close_gate;
    if (gate == NULL)
        return;

    assert(pthread_mutex_lock(&gate->mutex) == 0);
    gate->entered = true;
    assert(pthread_cond_signal(&gate->cond) == 0);
    while (!gate->released)
        assert(pthread_cond_wait(&gate->cond, &gate->mutex) == 0);
    gate->observed_refcount = atomic_load_explicit(
            &fd->refcount, memory_order_relaxed);
    assert(pthread_mutex_unlock(&gate->mutex) == 0);
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
    fd->opened_created = probe->report_created;
    return fd;
}

static int probe_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct open_probe *probe = mount->data;
    strcpy(probe->last_stat_path, path);
    struct access_identity_gate *gate = probe->access_gate;
    if (gate != NULL && strcmp(path, gate->path) == 0) {
        assert(pthread_mutex_lock(&gate->mutex) == 0);
        gate->entered = true;
        assert(pthread_cond_signal(&gate->cond) == 0);
        while (!gate->released)
            assert(pthread_cond_wait(&gate->cond, &gate->mutex) == 0);
        assert(pthread_mutex_unlock(&gate->mutex) == 0);
    }
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
    wait_for_path_close(fd);
    strcpy(state->probe->last_fstat_path, state->path);
    *stat = (struct statbuf) {
        .inode = 2,
        .mode = state->probe->file_mode,
        .uid = state->probe->owner,
        .gid = state->probe->group,
        .size = state->probe->file_size,
    };
    return 0;
}

static int probe_fsetattr(struct fd *fd, struct attr attr) {
    struct file_state *state = fd->data;
    struct open_probe *probe = state->probe;
    assert(lock_owned_by_current(&fd->lock));
    assert(attr.type == attr_size);
    probe->resize_calls++;
    probe->requested_size = attr.size;
    if (probe->resize_error != 0)
        return probe->resize_error;
    probe->file_size = (qword_t) attr.size;
    return 0;
}

static int probe_getpath(struct fd *fd, char *buffer) {
    struct file_state *state = fd->data;
    wait_for_path_close(fd);
    strcpy(buffer, state->path);
    return 0;
}

static ssize_t probe_readlink(struct mount *mount,
        const char *path, char *buffer, size_t size) {
    (void) mount;
    const char *target;
    if (strcmp(path, "/target/link") == 0)
        target = "/absolute";
    else if (strcmp(path, "/target/guard-link") == 0)
        target = "guard";
    else if (strcmp(path, "/explicit/link") == 0)
        target = "explicit";
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

static int probe_mkdir(
        struct mount *mount, const char *path, mode_t_ mode) {
    struct open_probe *probe = mount->data;
    probe->mkdirs++;
    strcpy(probe->last_mkdir_path, path);
    probe->last_mkdir_mode = mode;
    return 0;
}

static int probe_rename(struct mount *mount,
        const char *source, const char *destination) {
    struct open_probe *probe = mount->data;
    probe->renames++;
    strcpy(probe->last_rename_source, source);
    strcpy(probe->last_rename_destination, destination);
    return 0;
}

static const struct fs_ops probe_fs = {
    .open = probe_open,
    .readlink = probe_readlink,
    .unlink = probe_unlink,
    .rmdir = probe_rmdir,
    .mkdir = probe_mkdir,
    .rename = probe_rename,
    .stat = probe_stat,
    .fstat = probe_fstat,
    .fsetattr = probe_fsetattr,
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
    fixture->task.uid = fixture->task.euid = fixture->task.suid = uid;
    fixture->task.gid = fixture->task.egid = fixture->task.sgid = gid;
    fixture->pwd = make_path_fd(mount, pwd_state, probe, pwd_path);
    fixture->root = make_path_fd(mount, root_state, probe, root_path);
    if (fixture->pwd == NULL || fixture->root == NULL)
        return false;
    fixture->fs.pwd = fixture->pwd;
    fixture->fs.root = fixture->root;
    return true;
}

enum path_close_race_operation {
    PATH_CLOSE_RACE_STATAT,
    PATH_CLOSE_RACE_EMPTY_STATAT,
    PATH_CLOSE_RACE_OPENAT,
};

struct path_close_race_context {
    struct task *task;
    enum path_close_race_operation operation;
    fd_t dirfd;
    sqword_t result;
};

struct faccessat_race_context {
    struct task *task;
    addr_t path;
    dword_t result;
};

static void *run_faccessat_race(void *opaque) {
    struct faccessat_race_context *context = opaque;
    current = context->task;
    context->result = sys_faccessat(
            AT_FDCWD_, context->path, AC_R, 0);
    return NULL;
}

static void *run_path_close_race(void *opaque) {
    struct path_close_race_context *context = opaque;
    struct statbuf stat;
    switch (context->operation) {
        case PATH_CLOSE_RACE_STATAT:
            context->result = file_statat_task(context->task,
                    context->dirfd, "nested", 0, &stat);
            break;
        case PATH_CLOSE_RACE_EMPTY_STATAT:
            context->result = file_statat_task(context->task,
                    context->dirfd, "", AT_EMPTY_PATH_, &stat);
            break;
        case PATH_CLOSE_RACE_OPENAT:
            context->result = file_openat_task(context->task,
                    context->dirfd, "nested", O_RDONLY_, 0);
            break;
    }
    return NULL;
}

static bool test_path_close_during_operation(struct task_fixture *fixture,
        struct mount *mount, struct open_probe *probe,
        enum path_close_race_operation operation) {
    struct path_close_gate gate = {0};
    if (pthread_mutex_init(&gate.mutex, NULL) != 0)
        return false;
    if (pthread_cond_init(&gate.cond, NULL) != 0) {
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }

    struct file_state state;
    struct fd *directory = make_path_fd(
            mount, &state, probe, "/race");
    if (directory == NULL) {
        pthread_cond_destroy(&gate.cond);
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }
    state.close_gate = &gate;
    fd_t dirfd = f_install_task(&fixture->task, directory, 0);
    if (dirfd < 0) {
        pthread_cond_destroy(&gate.cond);
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }

    struct path_close_race_context context = {
        .task = &fixture->task,
        .operation = operation,
        .dirfd = dirfd,
        .result = _EIO,
    };
    pthread_t thread;
    if (pthread_create(&thread, NULL,
            run_path_close_race, &context) != 0) {
        f_close_task(&fixture->task, dirfd);
        pthread_cond_destroy(&gate.cond);
        pthread_mutex_destroy(&gate.mutex);
        return false;
    }

    assert(pthread_mutex_lock(&gate.mutex) == 0);
    while (!gate.entered)
        assert(pthread_cond_wait(&gate.cond, &gate.mutex) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);

    int close_result = f_close_task(&fixture->task, dirfd);
    bool retained_during_close = gate.close_calls == 0;

    assert(pthread_mutex_lock(&gate.mutex) == 0);
    gate.released = true;
    assert(pthread_cond_signal(&gate.cond) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
    bool joined = pthread_join(thread, NULL) == 0;

    bool operation_succeeded = context.result == 0;
    if (operation == PATH_CLOSE_RACE_OPENAT) {
        operation_succeeded = context.result >= 0 &&
                f_close_task(&fixture->task, (fd_t) context.result) == 0;
    }
    const char *observed_path = operation == PATH_CLOSE_RACE_STATAT ?
            probe->last_stat_path : operation == PATH_CLOSE_RACE_OPENAT ?
            probe->last_path : probe->last_fstat_path;
    const char *expected_path = operation == PATH_CLOSE_RACE_EMPTY_STATAT ?
            "/race" : "/race/nested";
    bool passed = close_result == 0 && retained_during_close && joined &&
            operation_succeeded && strcmp(observed_path, expected_path) == 0 &&
            gate.observed_refcount == 1 && gate.close_calls == 1 &&
            f_get_task(&fixture->task, dirfd) == NULL;
    assert(pthread_cond_destroy(&gate.cond) == 0);
    assert(pthread_mutex_destroy(&gate.mutex) == 0);
    return passed;
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

    const enum path_close_race_operation path_close_races[] = {
        PATH_CLOSE_RACE_STATAT,
        PATH_CLOSE_RACE_EMPTY_STATAT,
        PATH_CLOSE_RACE_OPENAT,
    };
    for (size_t index = 0;
            index < array_size(path_close_races); index++) {
        CHECK(test_path_close_during_operation(&target,
                mount, &probe, path_close_races[index]),
                "并发关闭 dirfd 不得中断 statat/openat 路径解析");
    }

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
    const struct fs_access_identity real_identity = {
        .uid = 3000,
        .gid = 300,
    };
    CHECK(path_normalize_task_access(&target.task, AT_PWD,
            "guard/child", normalized, N_SYMLINK_FOLLOW,
            &real_identity) == _EACCES,
            "显式真实身份同样约束中间目录执行权限");
    CHECK(path_normalize_task(&target.task, AT_PWD,
            "guard-link/child", normalized, N_SYMLINK_FOLLOW) == 0 &&
            strcmp(normalized, "/target/guard/child") == 0,
            "符号链接递归继续使用有效身份检查中间目录");
    CHECK(path_normalize_task_access(&target.task, AT_PWD,
            "guard-link/child", normalized, N_SYMLINK_FOLLOW,
            &real_identity) == _EACCES,
            "符号链接递归不得丢失显式真实身份");
    CHECK(target.task.euid == probe.owner && target.task.egid == probe.group,
            "显式真实身份检查不得改写目标任务有效身份");
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
    lock(&pids_lock);
    decoy.task.euid = 0;
    unlock(&pids_lock);
    CHECK(access_check(&permission, AC_W) == 0, "兼容权限入口保留超级用户语义");
    lock(&pids_lock);
    decoy.task.euid = 2000;
    unlock(&pids_lock);

    const struct fs_access_identity effective_identity = {
        .uid = target.task.euid,
        .gid = target.task.egid,
    };
    CHECK(generic_accessat_task(&target.task, AT_PWD, "child", AC_R,
            &effective_identity) == 0,
            "显式有效身份可读取仅所有者可读的目标");
    CHECK(generic_accessat_task(&target.task, AT_PWD, "child", AC_R,
            &real_identity) == _EACCES,
            "显式真实身份检查最终对象权限");
    CHECK(generic_accessat_task(&target.task, AT_PWD, "guard/child", AC_F,
            &effective_identity) == 0 &&
            generic_accessat_task(&target.task, AT_PWD,
                    "guard/child", AC_F, &real_identity) == _EACCES,
            "F_OK 跳过最终权限但仍按同一身份遍历路径");
    const struct fs_access_identity root_identity = {0};
    CHECK(generic_accessat_task(&target.task, AT_PWD, "child", AC_W,
            &root_identity) == 0,
            "显式 root 身份保留超级用户访问语义");
    CHECK(target.task.euid == probe.owner && target.task.egid == probe.group,
            "完整访问检查后目标任务有效身份保持不变");

    enum { FACCESSAT_PATH = 0x10000 };
    task_set_mm(&decoy.task, mm_new());
    CHECK(decoy.task.mm != NULL, "为 faccessat 竞态测试创建用户地址空间");
    write_wrlock(&decoy.task.mem->lock);
    int map_error = pt_map_nothing(
            decoy.task.mem, PAGE(FACCESSAT_PATH), 1, P_RWX);
    write_wrunlock(&decoy.task.mem->lock);
    CHECK(map_error == 0 && user_write_string(
            FACCESSAT_PATH, "/access-race") == 0,
            "为 faccessat 竞态测试映射 guest 路径");
    lock(&pids_lock);
    decoy.task.uid = probe.owner;
    decoy.task.gid = probe.group;
    unlock(&pids_lock);

    struct access_identity_gate access_gate = {
        .path = "/decoy-root/access-race",
    };
    CHECK(pthread_mutex_init(&access_gate.mutex, NULL) == 0 &&
            pthread_cond_init(&access_gate.cond, NULL) == 0,
            "初始化 faccessat 身份观察门闩");
    probe.access_gate = &access_gate;
    struct faccessat_race_context access_context = {
        .task = &decoy.task,
        .path = FACCESSAT_PATH,
        .result = (dword_t) _EIO,
    };
    pthread_t access_thread;
    CHECK(pthread_create(&access_thread, NULL,
            run_faccessat_race, &access_context) == 0,
            "启动 faccessat 身份竞态线程");
    assert(pthread_mutex_lock(&access_gate.mutex) == 0);
    while (!access_gate.entered)
        assert(pthread_cond_wait(&access_gate.cond,
                &access_gate.mutex) == 0);
    assert(pthread_mutex_unlock(&access_gate.mutex) == 0);

    struct task_credentials observed_credentials;
    task_credentials_snapshot(&decoy.task, &observed_credentials);
    assert(pthread_mutex_lock(&access_gate.mutex) == 0);
    access_gate.released = true;
    assert(pthread_cond_signal(&access_gate.cond) == 0);
    assert(pthread_mutex_unlock(&access_gate.mutex) == 0);
    CHECK(pthread_join(access_thread, NULL) == 0 &&
            access_context.result == 0,
            "真实身份 faccessat 在门闩释放后完成");
    CHECK(observed_credentials.euid == 2000 &&
            observed_credentials.egid == 200,
            "faccessat 阻塞期间不得用真实身份覆盖有效身份");
    CHECK(decoy.task.euid == 2000 && decoy.task.egid == 200,
            "faccessat 完成后仍保持有效身份");
    probe.access_gate = NULL;
    CHECK(pthread_cond_destroy(&access_gate.cond) == 0 &&
            pthread_mutex_destroy(&access_gate.mutex) == 0,
            "销毁 faccessat 身份观察门闩");

    char link_target[16] = {0};
    CHECK(file_readlinkat_task(&target.task, AT_FDCWD_, "link",
            link_target, sizeof(link_target)) == 9 &&
            memcmp(link_target, "/absolute", 9) == 0,
            "readlinkat 相对路径使用目标任务 cwd 而非 current");
    CHECK(file_readlinkat_task(&target.task, 99, "link",
            link_target, sizeof(link_target)) == _EBADF,
            "readlinkat 相对路径拒绝目标任务中的无效 dirfd");
    memset(link_target, 0, sizeof(link_target));
    CHECK(file_readlinkat_task(&target.task, 0, "link",
            link_target, sizeof(link_target)) == 8 &&
            memcmp(link_target, "explicit", 8) == 0,
            "readlinkat 相对路径使用目标任务中的显式 dirfd");

    const int sync_flags = (1 << 20) | (1 << 12);
    struct fd *sync_read = generic_openat_task(
            &target.task, AT_PWD, "sync-read", sync_flags, 0);
    CHECK(!IS_ERR(sync_read) && probe.last_flags == sync_flags,
            "guest O_SYNC 位不改变只读打开的权限类别");
    fd_close(sync_read);

    probe.file_mode = S_IFREG | 0600;
    probe.file_size = 99;
    unsigned resize_calls_before = probe.resize_calls;
    struct fd *truncated_read = generic_openat_task(&target.task,
            AT_PWD, "created", O_RDONLY_ | O_TRUNC_, 0);
    CHECK(!IS_ERR(truncated_read) && probe.last_flags == O_RDWR_ &&
            probe.file_size == 0 &&
            probe.resize_calls == resize_calls_before + 1,
            "O_RDONLY|O_TRUNC 以可读写 provider 延迟提交截断");
    CHECK((fd_getflags(truncated_read) & O_ACCMODE_) == O_RDONLY_ &&
            (truncated_read->flags & O_TRUNC_) == 0,
            "延迟截断不泄漏 provider 能力或一次性 O_TRUNC 状态");
    fd_close(truncated_read);

    probe.file_mode = S_IFREG;
    probe.file_size = 0;
    probe.report_created = true;
    unsigned created_resize_calls = probe.resize_calls;
    struct fd *created_truncate = generic_openat_task(&target.task,
            AT_PWD, "created", O_RDONLY_ | O_CREAT_ | O_TRUNC_, 0);
    CHECK(!IS_ERR(created_truncate) && created_truncate->opened_created &&
            probe.last_flags == (O_RDWR_ | O_CREAT_) &&
            probe.resize_calls == created_resize_calls,
            "新创建文件跳过 inode 权限复核和冗余 O_TRUNC");
    fd_close(created_truncate);
    probe.report_created = false;

    probe.file_mode = S_IFREG | 0400;
    probe.file_size = 17;
    struct fd *denied_truncate = generic_openat_task(&target.task,
            AT_PWD, "created", O_RDONLY_ | O_TRUNC_, 0);
    CHECK(IS_ERR(denied_truncate) && PTR_ERR(denied_truncate) == _EACCES &&
            probe.file_size == 17 &&
            probe.resize_calls == resize_calls_before + 1,
            "O_TRUNC 增加写权限检查且拒绝后不修改文件");
    probe.file_mode = S_IFREG | 0200;
    denied_truncate = generic_openat_task(&target.task,
            AT_PWD, "created", O_RDONLY_ | O_TRUNC_, 0);
    CHECK(IS_ERR(denied_truncate) && PTR_ERR(denied_truncate) == _EACCES &&
            probe.file_size == 17 &&
            probe.resize_calls == resize_calls_before + 1,
            "O_RDONLY|O_TRUNC 同时要求读取和写入权限");

    probe.file_mode = S_IFIFO | 0666;
    struct fd *fifo = generic_openat_task(&target.task,
            AT_PWD, "created", O_RDONLY_ | O_TRUNC_, 0);
    CHECK(!IS_ERR(fifo) && S_ISFIFO(fifo->type) &&
            probe.last_flags == O_RDONLY_ &&
            probe.resize_calls == resize_calls_before + 1,
            "非普通文件按原访问模式重开且不执行 O_TRUNC");
    fd_close(fifo);

    probe.file_mode = S_IFDIR | 0777;
    struct fd *truncate_directory = generic_openat_task(&target.task,
            AT_PWD, "created", O_RDONLY_ | O_TRUNC_, 0);
    CHECK(IS_ERR(truncate_directory) &&
            PTR_ERR(truncate_directory) == _EISDIR &&
            probe.last_flags == O_RDONLY_ &&
            probe.resize_calls == resize_calls_before + 1,
            "O_TRUNC 对目录在权限检查前返回 EISDIR");

    unsigned opens_before_truncate = probe.opens;
    probe.file_mode = S_IFREG | 0600;
    CHECK(file_truncate_task(&target.task, "created", 7) == 0 &&
            probe.last_flags == (O_WRONLY_ | O_NONBLOCK_) &&
            probe.file_size == 7 &&
            probe.requested_size == 7 &&
            probe.resize_calls == resize_calls_before + 2,
            "path truncate 通过稳定可写对象提交尺寸变更");
    probe.resize_error = _EIO;
    CHECK(file_truncate_task(&target.task, "created", 3) == _EIO &&
            probe.file_size == 7 &&
            probe.resize_calls == resize_calls_before + 3,
            "path truncate 传播底层失败且保持原大小");
    probe.resize_error = 0;
    CHECK(file_truncate_task(&target.task, "created", -1) == _EINVAL &&
            probe.opens == opens_before_truncate + 2,
            "path truncate 的负长度在路径查找前返回 EINVAL");
    CHECK(file_truncate_task(&target.task, "child", 0) == _EISDIR &&
            probe.opens == opens_before_truncate + 2,
            "path truncate 对目录返回 EISDIR 且不打开对象");
    probe.file_mode = S_IFREG | 0400;

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

    CHECK(file_mkdirat_task(&target.task, 99, "", 0777) == _ENOENT &&
            probe.mkdirs == 0, "mkdirat 空路径优先返回 ENOENT");
    CHECK(file_mkdirat_task(
            &target.task, AT_FDCWD_, "directory", 0777) == 0 &&
            probe.mkdirs == 1 &&
            strcmp(probe.last_mkdir_path, "/target/directory") == 0 &&
            probe.last_mkdir_mode == 0750,
            "mkdirat 使用目标 cwd 与 umask");
    CHECK(file_mkdirat_task(&target.task, 0, "nested", 0700) == 0 &&
            probe.mkdirs == 2 &&
            strcmp(probe.last_mkdir_path, "/explicit/nested") == 0 &&
            probe.last_mkdir_mode == 0700,
            "mkdirat 使用目标任务显式 dirfd");
    CHECK(file_mkdirat_task(
            &target.task, 99, "relative", 0777) == _EBADF &&
            probe.mkdirs == 2,
            "mkdirat 相对路径拒绝目标任务的无效 dirfd");
    CHECK(file_mkdirat_task(
            &target.task, cwd_fd, "child", 0777) == _ENOTDIR &&
            probe.mkdirs == 2,
            "mkdirat 相对路径拒绝普通文件 dirfd");
    CHECK(file_mkdirat_task(
            &target.task, 99, "/absolute", 0765) == 0 &&
            probe.mkdirs == 3 &&
            strcmp(probe.last_mkdir_path, "/sandbox/absolute") == 0 &&
            probe.last_mkdir_mode == 0740,
            "mkdirat 绝对路径使用目标 root 并忽略无效 dirfd");
    CHECK(generic_mkdirat(AT_PWD, "compat", 0701) == 0 &&
            probe.mkdirs == 4 &&
            strcmp(probe.last_mkdir_path, "/decoy/compat") == 0 &&
            probe.last_mkdir_mode == 0701,
            "兼容 mkdirat 入口仍使用 current 的 cwd");

    CHECK(file_renameat_task(&target.task, 99, "",
            99, "destination") == _ENOENT && probe.renames == 0,
            "renameat 空源路径优先返回 ENOENT");
    CHECK(file_renameat_task(&target.task, 99, "source",
            99, "") == _ENOENT && probe.renames == 0,
            "renameat 空目标路径优先于 dirfd 检查");
    CHECK(file_renameat_task(&target.task, AT_FDCWD_, "source",
            AT_FDCWD_, "destination") == 0 && probe.renames == 1 &&
            strcmp(probe.last_rename_source, "/target/source") == 0 &&
            strcmp(probe.last_rename_destination,
                    "/target/destination") == 0,
            "renameat 的两端均使用目标任务 cwd");
    CHECK(file_renameat_task(&target.task, 0, "source",
            AT_FDCWD_, "destination") == 0 && probe.renames == 2 &&
            strcmp(probe.last_rename_source, "/explicit/source") == 0 &&
            strcmp(probe.last_rename_destination,
                    "/target/destination") == 0,
            "renameat 独立解析两端 dirfd");
    CHECK(file_renameat_task(&target.task, 99, "source",
            AT_FDCWD_, "destination") == _EBADF && probe.renames == 2,
            "renameat 拒绝无效源 dirfd");
    CHECK(file_renameat_task(&target.task, AT_FDCWD_, "source",
            99, "destination") == _EBADF && probe.renames == 2,
            "renameat 拒绝无效目标 dirfd");
    CHECK(file_renameat_task(&target.task, cwd_fd, "source",
            AT_FDCWD_, "destination") == _ENOTDIR &&
            probe.renames == 2,
            "renameat 拒绝普通文件源 dirfd");
    CHECK(file_renameat_task(&target.task, AT_FDCWD_, "source",
            cwd_fd, "destination") == _ENOTDIR &&
            probe.renames == 2,
            "renameat 拒绝普通文件目标 dirfd");
    CHECK(file_renameat_task(&target.task, 99, "/source",
            98, "/destination") == 0 && probe.renames == 3 &&
            strcmp(probe.last_rename_source, "/sandbox/source") == 0 &&
            strcmp(probe.last_rename_destination,
                    "/sandbox/destination") == 0,
            "renameat 绝对路径使用目标 root 并忽略两端 dirfd");
    CHECK(generic_renameat(AT_PWD, "source",
            AT_PWD, "destination") == 0 && probe.renames == 4 &&
            strcmp(probe.last_rename_source, "/decoy/source") == 0 &&
            strcmp(probe.last_rename_destination,
                    "/decoy/destination") == 0,
            "兼容 renameat 入口仍使用 current 的 cwd");

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
    lock(&pids_lock);
    target.task.euid = 0;
    unlock(&pids_lock);
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
    lock(&pids_lock);
    target.task.euid = target_euid;
    unlock(&pids_lock);

    probe.file_mode = S_IFDIR | 0100;
    CHECK(file_chdir_task(&target.task, "link") == 0 &&
            strcmp(probe.last_path, "/sandbox/absolute") == 0 &&
            probe.last_flags == O_DIRECTORY_,
            "chdir 跟随末端符号链接并允许仅有执行权限的目录");
    CHECK(fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/sandbox/absolute") == 0 &&
            fs_getcwd_task(&decoy.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/decoy") == 0,
            "chdir 只更新目标任务 cwd，诱饵任务保持不变");

    probe.file_mode = S_IFDIR;
    CHECK(file_fchdir_task(&target.task, 0) == _EACCES &&
            fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/sandbox/absolute") == 0,
            "fchdir 使用目标任务凭据拒绝不可搜索目录并保持 cwd");
    probe.file_mode = S_IFDIR | 0100;
    CHECK(file_fchdir_task(&target.task, 0) == 0 &&
            fs_getcwd_task(&target.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/explicit") == 0 &&
            fs_getcwd_task(&decoy.task, cwd, sizeof(cwd)) > 0 &&
            strcmp(cwd, "/decoy") == 0,
            "fchdir 只更新显式目标任务并保留诱饵任务 cwd");
    target.pwd = target.fs.pwd;

    current = NULL;
    mm_release(decoy.task.mm);
    fdtable_release(decoy.task.files);
    fdtable_release(target.task.files);
    fd_close(decoy.pwd);
    fd_close(decoy.root);
    fd_close(target.pwd);
    fd_close(target.root);
    CHECK(probe.opens == 21 && probe.closes == 21,
            "所有成功、拒绝和超限打开都恰好关闭一次");
    CHECK(mount->refcount == 1, "清理阶段仅保留测试持有的 mount 引用");
    mount_release(mount);
    lock(&mounts_lock);
    int remove_error = mount_remove(mount);
    unlock(&mounts_lock);
    CHECK(remove_error == 0 && list_empty(&mounts), "测试根文件系统卸载成功");
    return 0;
}

#include "sdk/iSHApple/Headers/iSHApple.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "platform/apple-runtime-mount.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Apple mount guest 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

static bool join_path(
        char destination[PATH_MAX],
        const char *parent,
        const char *name) {
    return snprintf(destination, PATH_MAX, "%s/%s", parent, name) <
            PATH_MAX;
}

static bool write_host_file(
        const char *path, const void *bytes, size_t length) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    ssize_t result = write(fd, bytes, length);
    bool closed = close(fd) == 0;
    return result == (ssize_t) length && closed;
}

static struct ish_apple_mount_spec_v1 mount_spec(
        uint64_t low,
        int directory_fd,
        const char *guest_directory,
        int32_t access) {
    return (struct ish_apple_mount_spec_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(struct ish_apple_mount_spec_v1),
        .mount_id = {.high = 20, .low = low},
        .access = access,
        .host_directory_fd = directory_fd,
        .guest_directory = guest_directory,
    };
}

static void remove_file(const char *path) {
    if (unlink(path) < 0)
        (void) path;
}

int main(void) {
    char fixture[PATH_MAX] = "/tmp/ish-apple-mount-guest.XXXXXX";
    CHECK(mkdtemp(fixture) != NULL, "创建 guest mount fixture");
    char root[PATH_MAX];
    char read_only_host[PATH_MAX];
    char read_write_host[PATH_MAX];
    CHECK(join_path(root, fixture, "root") &&
            join_path(read_only_host, fixture, "read-only") &&
            join_path(read_write_host, fixture, "read-write") &&
            mkdir(root, 0700) == 0 &&
            mkdir(read_only_host, 0700) == 0 &&
            mkdir(read_write_host, 0700) == 0,
            "创建 root 与宿主挂载目录");

    char host_path[PATH_MAX];
    static const char original[] = "read-only payload";
    CHECK(join_path(host_path, read_only_host, "original.txt") &&
            write_host_file(host_path, original, sizeof(original) - 1),
            "准备只读宿主文件");

    int read_only_fd = open(
            read_only_host, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int read_write_fd = open(
            read_write_host, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct ish_apple_mount_spec_v1 specs[2] = {
        mount_spec(1, read_only_fd, "/mnt/etos/read-only",
                ISH_APPLE_MOUNT_ACCESS_READ_ONLY),
        mount_spec(2, read_write_fd, "/mnt/etos/read-write",
                ISH_APPLE_MOUNT_ACCESS_READ_WRITE),
    };
    CHECK(read_only_fd >= 0 && read_write_fd >= 0 &&
            ish_apple_mount_prepare_startup(specs, 2) == 0,
            "复制两个启动 mount 的宿主目录 fd");
    if (read_only_fd >= 0)
        close(read_only_fd);
    if (read_write_fd >= 0)
        close(read_write_fd);

    int root_mount_error = mount_root(&realfs, root);
    int task_error = root_mount_error == 0 ? begin_first_process() :
            root_mount_error;
    CHECK(root_mount_error == 0 && task_error == 0,
            "建立未发布的真实 guest 文件系统 task");
    bool task_prepared = current != NULL;
    int activation_error = task_prepared ?
            ish_apple_mount_activate_startup(current) : _EIO;
    CHECK(task_prepared && activation_error == 0,
            "在 PID1 接受作业前激活启动 mount 列表");
    ish_apple_mount_finish_startup(activation_error == 0);
    if (!task_prepared || activation_error < 0) {
        if (task_prepared)
            cancel_prepared_process();
        if (root_mount_error == 0) {
            lock(&mounts_lock);
            (void) do_umount("");
            unlock(&mounts_lock);
        }
        return 1;
    }

    struct fd *mounted_directory = generic_open(
            "/mnt/etos/read-write", O_RDONLY_ | O_DIRECTORY_, 0);
    char mounted_guest_path[MAX_PATH] = {};
    CHECK(!IS_ERR(mounted_directory) &&
            generic_getpath(mounted_directory, mounted_guest_path) == 0 &&
            strcmp(mounted_guest_path, "/mnt/etos/read-write") == 0,
            "fd mount 的 getpath 只返回 guest 路径而不泄漏宿主目录");
    if (!IS_ERR(mounted_directory))
        fd_close(mounted_directory);

    struct fd *guest_file = generic_open(
            "/mnt/etos/read-only/original.txt", O_RDONLY_, 0);
    char guest_bytes[sizeof(original)] = {};
    CHECK(!IS_ERR(guest_file) &&
            file_read_fd(guest_file, guest_bytes, sizeof(original) - 1) ==
                    (ssize_t) sizeof(original) - 1 &&
            memcmp(guest_bytes, original, sizeof(original) - 1) == 0,
            "guest 从动态只读 mount 读取宿主内容");
    if (!IS_ERR(guest_file))
        fd_close(guest_file);
    struct fd *write_denied = generic_open(
            "/mnt/etos/read-only/original.txt", O_WRONLY_, 0);
    CHECK(IS_ERR(write_denied) && PTR_ERR(write_denied) == _EROFS,
            "只读 mount 在 guest 打开写路径时返回 EROFS");

    struct fd *held_directory = generic_open(
            "/mnt/etos/read-only", O_RDONLY_ | O_DIRECTORY_, 0);
    ish_apple_mount_lease *lease = NULL;
    CHECK(!IS_ERR(held_directory) &&
            ish_apple_mount_lease_acquire(
                    specs[0].mount_id, &lease) == 0 &&
            lease != NULL,
            "活跃作业取得 mount lease 与内核目录引用");
    CHECK(ish_apple_mount_remove(specs[0].mount_id, 0) == _EBUSY,
            "普通移除进入 draining 并等待 lease");
    if (lease != NULL)
        ish_apple_mount_lease_release(lease);
    CHECK(ish_apple_mount_remove(
                    specs[0].mount_id,
                    ISH_APPLE_MOUNT_REMOVE_FORCE) == _EBUSY,
            "强制移除不破坏 guest 已持有的 mount 引用");
    if (!IS_ERR(held_directory))
        fd_close(held_directory);
    CHECK(ish_apple_mount_remove(
                    specs[0].mount_id,
                    ISH_APPLE_MOUNT_REMOVE_FORCE) == 0,
            "guest 引用释放后完成强制卸载");

    struct fd *created = generic_open(
            "/mnt/etos/read-write/from-guest.txt",
            O_CREAT_ | O_WRONLY_,
            0600);
    static const char written[] = "guest write";
    CHECK(!IS_ERR(created) &&
            file_write_fd(created, written, sizeof(written) - 1) ==
                    (ssize_t) sizeof(written) - 1,
            "读写 mount 把 guest 变更提交到宿主目录");
    if (!IS_ERR(created))
        fd_close(created);
    CHECK(join_path(host_path, read_write_host, "from-guest.txt"),
            "生成宿主验证路径");
    char host_bytes[sizeof(written)] = {};
    int host_fd = open(host_path, O_RDONLY | O_CLOEXEC);
    CHECK(host_fd >= 0 &&
            read(host_fd, host_bytes, sizeof(written) - 1) ==
                    (ssize_t) sizeof(written) - 1 &&
            memcmp(host_bytes, written, sizeof(written) - 1) == 0,
            "宿主直接观察到 guest 的读写 mount 变更");
    if (host_fd >= 0)
        close(host_fd);
    CHECK(ish_apple_mount_remove(specs[1].mount_id, 0) == 0,
            "无活跃引用的读写 mount 可普通卸载");
    uint32_t count = 1;
    CHECK(ish_apple_mount_list(NULL, 0, &count) == 0 && count == 0,
            "两个 mount 完成后 registry 为空");

    if (task_prepared)
        cancel_prepared_process();
    lock(&mounts_lock);
    int root_unmount = do_umount("");
    unlock(&mounts_lock);
    CHECK(root_unmount == 0, "释放 task 引用后卸载测试 root");

    remove_file(host_path);
    CHECK(join_path(host_path, read_only_host, "original.txt"),
            "生成只读文件清理路径");
    remove_file(host_path);
    char guest_path[PATH_MAX];
    static const char *const guest_directories[] = {
        "mnt/etos/read-only", "mnt/etos/read-write", "mnt/etos", "mnt",
    };
    for (size_t index = 0;
            index < sizeof(guest_directories) /
                    sizeof(guest_directories[0]);
            index++) {
        if (join_path(guest_path, root, guest_directories[index]))
            (void) rmdir(guest_path);
    }
    CHECK(rmdir(root) == 0 &&
            rmdir(read_only_host) == 0 &&
            rmdir(read_write_host) == 0 &&
            rmdir(fixture) == 0,
            "卸载不删除宿主内容且 fixture 可完整清理");

    if (failures == 0)
        puts("Apple mount guest 权限与生命周期回归通过");
    return failures == 0 ? 0 : 1;
}

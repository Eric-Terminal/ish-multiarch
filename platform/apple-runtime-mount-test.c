#include "sdk/iSHApple/Headers/iSHApple.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "platform/apple-runtime-mount.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Apple mount registry 测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        failures++; \
    } \
} while (0)

static struct ish_apple_mount_spec_v1 mount_spec(
        uint64_t low,
        int directory_fd,
        const char *guest_directory) {
    return (struct ish_apple_mount_spec_v1) {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = sizeof(struct ish_apple_mount_spec_v1),
        .mount_id = {.high = 1, .low = low},
        .access = ISH_APPLE_MOUNT_ACCESS_READ_ONLY,
        .host_directory_fd = directory_fd,
        .guest_directory = guest_directory,
    };
}

int main(void) {
    char temporary[] = "/tmp/ish-apple-mount.XXXXXX";
    CHECK(mkdtemp(temporary) != NULL, "创建宿主测试目录");
    int directory_fd = open(
            temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory_fd >= 0, "打开宿主测试目录");
    if (directory_fd < 0)
        return 1;

    struct ish_apple_mount_spec_v1 invalid =
            mount_spec(1, directory_fd, "/mnt/etos/invalid");
    invalid.version++;
    CHECK(ish_apple_mount_prepare_startup(&invalid, 1) == _ENOTSUP,
            "拒绝未知 mount ABI");
    invalid = mount_spec(1, directory_fd, "/mnt/etos/invalid");
    invalid.structure_size--;
    CHECK(ish_apple_mount_prepare_startup(&invalid, 1) == _EINVAL,
            "拒绝截断的 mount 配置");
    invalid = mount_spec(1, directory_fd, "/mnt/etos/../escape");
    CHECK(ish_apple_mount_prepare_startup(&invalid, 1) == _EINVAL,
            "拒绝 guest 路径中的上级目录组件");
    invalid = mount_spec(1, directory_fd, "/mnt/shared/nested");
    CHECK(ish_apple_mount_prepare_startup(&invalid, 1) == _EBUSY,
            "保留兼容共享目录命名空间");
    invalid = mount_spec(1, directory_fd, "/outside");
    CHECK(ish_apple_mount_prepare_startup(&invalid, 1) == _EINVAL,
            "动态 mount 只能进入 guest 的 /mnt 子树");

    enum { mount_count = 13 };
    char paths[mount_count][64];
    struct ish_apple_mount_spec_v1 specs[mount_count];
    for (uint32_t index = 0; index < mount_count; index++) {
        snprintf(paths[index], sizeof(paths[index]),
                "/mnt/etos/%u", index + 1);
        specs[index] = mount_spec(
                (uint64_t) index + 1,
                directory_fd,
                paths[index]);
        specs[index].access = index % 2 == 0 ?
                ISH_APPLE_MOUNT_ACCESS_READ_ONLY :
                ISH_APPLE_MOUNT_ACCESS_READ_WRITE;
    }
    CHECK(ish_apple_mount_prepare_startup(
                    specs, mount_count) == 0,
            "启动 mount 列表不设置产品数量上限");

    uint32_t actual_count = 0;
    CHECK(ish_apple_mount_list(NULL, 0, &actual_count) == 0 &&
            actual_count == mount_count,
            "空 buffer 查询完整 mount 数量");
    struct ish_apple_mount_info_v1 first;
    CHECK(ish_apple_mount_list(&first, 1, &actual_count) == _ENOSPC &&
            actual_count == mount_count &&
            first.version == ISH_APPLE_ABI_VERSION &&
            first.structure_size == sizeof(first) &&
            first.state == ISH_APPLE_MOUNT_STATE_STAGED,
            "容量不足时返回 ENOSPC 并填充可用状态");

    uint32_t required = 0;
    CHECK(ish_apple_mount_copy_guest_directory(
                    specs[0].mount_id, NULL, 0, &required) == 0 &&
            required == strlen(paths[0]) + 1,
            "guest 路径支持无 buffer 长度查询");
    char copied[64];
    CHECK(ish_apple_mount_copy_guest_directory(
                    specs[0].mount_id,
                    copied,
                    sizeof(copied),
                    &required) == 0 &&
            strcmp(copied, paths[0]) == 0,
            "按稳定 mount ID 复制 guest 路径");
    CHECK(ish_apple_mount_remove(specs[0].mount_id, 0) == _EAGAIN,
            "启动事务尚未激活时不能部分卸载");

    close(directory_fd);
    directory_fd = -1;
    ish_apple_mount_finish_startup(0);
    CHECK(ish_apple_mount_list(NULL, 0, &actual_count) == 0 &&
            actual_count == 0,
            "启动失败原子清理全部已复制目录 fd 与注册项");

    directory_fd = open(
            temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory_fd >= 0, "重新打开宿主测试目录");
    struct ish_apple_mount_spec_v1 duplicate_id[2] = {
        mount_spec(7, directory_fd, "/mnt/etos/a"),
        mount_spec(7, directory_fd, "/mnt/etos/b"),
    };
    CHECK(ish_apple_mount_prepare_startup(
                    duplicate_id, 2) == _EEXIST &&
            ish_apple_mount_list(NULL, 0, &actual_count) == 0 &&
            actual_count == 0,
            "重复稳定 ID 让整个启动列表回滚");
    duplicate_id[1] = mount_spec(
            8, directory_fd, "/mnt/etos/a");
    CHECK(ish_apple_mount_prepare_startup(
                    duplicate_id, 2) == _EEXIST &&
            ish_apple_mount_list(NULL, 0, &actual_count) == 0 &&
            actual_count == 0,
            "重复 guest 路径让整个启动列表回滚");

    struct ish_apple_mount_spec_v1 dynamic =
            mount_spec(99, directory_fd, "/mnt/etos/dynamic");
    CHECK(ish_apple_mount_add(&dynamic) == _EAGAIN,
            "runtime 启动前拒绝动态 add");
    ish_apple_mount_lease *lease = NULL;
    CHECK(ish_apple_mount_lease_acquire(
                    dynamic.mount_id, &lease) == _ENOENT &&
            lease == NULL,
            "不存在的 mount 不能取得 lease");

    close(directory_fd);
    CHECK(rmdir(temporary) == 0, "清理宿主测试目录");
    if (failures == 0)
        puts("Apple 动态 mount registry 回归通过");
    return failures == 0 ? 0 : 1;
}

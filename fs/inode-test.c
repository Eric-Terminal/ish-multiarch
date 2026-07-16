#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/dev.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/real.h"
#include "kernel/fs.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "inode 身份测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static unsigned orphan_calls;
static ino_t last_orphan;

static void record_orphan(struct mount *mount, ino_t inode) {
    (void) mount;
    orphan_calls++;
    last_orphan = inode;
}

static const struct fs_ops inode_test_ops = {
    .inode_orphaned = record_orphan,
};

static int check_realfs_device_identity(void) {
    int host_fd = open("/dev/null", O_RDONLY);
    CHECK(host_fd >= 0, "打开宿主设备用于身份探针");

    struct stat host_stat;
    CHECK(fstat(host_fd, &host_stat) == 0, "读取宿主设备元数据");
    struct fd fd = {
        .real_fd = host_fd,
    };
    struct statbuf stat = {};
    CHECK(realfs_fstat(&fd, &stat) == 0, "读取 realfs 元数据");
    CHECK(stat.dev == dev_fake_from_real(host_stat.st_dev),
            "guest st_dev 继续使用 Linux 编码");
    CHECK(stat.inode_device == realfs_inode_device(host_stat.st_dev),
            "内部 inode 键保留宿主设备原值");
    CHECK(close(host_fd) == 0, "关闭宿主设备探针");

#ifdef __APPLE__
    const dev_t first = makedev(1, 1);
    const dev_t second = makedev(1, (1 << 20) | 1);
    CHECK(first != second &&
            dev_fake_from_real(first) == dev_fake_from_real(second),
            "构造 guest 设备号压缩碰撞");
    CHECK(realfs_inode_device(first) != realfs_inode_device(second),
            "内部设备键隔离 guest 编码碰撞");
#endif
    return 0;
}

int main(void) {
    if (check_realfs_device_identity() != 0)
        return 1;
    const qword_t first_device = UINT64_C(0x100000001);
    const qword_t second_device = UINT64_C(0x200000001);
    const ino_t number = 42;
    struct mount mount = {
        .fs = &inode_test_ops,
    };

    struct inode_data *first = inode_get(&mount, first_device, number);
    struct inode_data *same = inode_get(&mount, first_device, number);
    struct inode_data *other = inode_get(&mount, second_device, number);
    CHECK(first == same && first->futex_sequence == same->futex_sequence,
            "相同 mount、device 和 inode 共享稳定序列");
    CHECK(first != other &&
            first->futex_sequence != other->futex_sequence,
            "相同 mount 与 inode 在不同 device 上保持隔离");
    CHECK(first->device == first_device && other->device == second_device,
            "inode 对象保留完整设备身份");

    qword_t released_sequence = first->futex_sequence;
    inode_check_orphaned(&mount, number);
    CHECK(orphan_calls == 0,
            "任一设备实例存活时不回收虚拟 inode");
    inode_release(first);
    inode_release(same);
    CHECK(orphan_calls == 0,
            "另一设备实例存活时不触发 orphan 回调");

    struct inode_data *recreated =
            inode_get(&mount, first_device, number);
    CHECK(recreated->futex_sequence != released_sequence &&
            recreated->futex_sequence != other->futex_sequence,
            "引用释放后重建不复用 futex 序列");

    inode_release(other);
    CHECK(orphan_calls == 0,
            "重建的设备实例延迟 orphan 回收");
    inode_release(recreated);
    CHECK(orphan_calls == 1 && last_orphan == number,
            "最后一个设备实例释放时触发一次 orphan 回调");
    CHECK(mount.refcount == 0,
            "inode 引用全部释放后归还 mount 引用");
    return 0;
}

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/linux/mman.h"
#include "guest/memory/page-backing.h"
#include "kernel/aarch64-file-mapping-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 文件映射服务测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct file_probe {
    byte_t bytes[GUEST_MEMORY_PAGE_SIZE * 2];
    qword_t size;
    unsigned pread_calls;
    unsigned fstat_calls;
    unsigned close_calls;
    int pread_error;
    int fstat_error;
};

struct mapping_fixture {
    struct task task;
    struct tgroup group;
    struct mount mount;
};

static ssize_t probe_pread(struct fd *fd, void *buffer,
        size_t size, off_t offset) {
    struct file_probe *probe = fd->data;
    probe->pread_calls++;
    if (probe->pread_error != 0)
        return probe->pread_error;
    if (offset < 0 || (qword_t) offset >= probe->size)
        return 0;
    qword_t available = probe->size - (qword_t) offset;
    if (available < size)
        size = (size_t) available;
    memcpy(buffer, probe->bytes + (size_t) offset, size);
    return (ssize_t) size;
}

static ssize_t probe_read(struct fd *fd, void *buffer, size_t size) {
    ssize_t result = probe_pread(fd, buffer, size, (off_t) fd->offset);
    if (result > 0)
        fd->offset += (off_t_) result;
    return result;
}

static off_t_ probe_lseek(struct fd *fd, off_t_ offset, int whence) {
    if (whence == LSEEK_CUR)
        offset += fd->offset;
    else if (whence != LSEEK_SET)
        return _EINVAL;
    if (offset < 0)
        return _EINVAL;
    fd->offset = offset;
    return offset;
}

static int probe_close(struct fd *fd) {
    struct file_probe *probe = fd->data;
    probe->close_calls++;
    return 0;
}

static int probe_fstat(struct fd *fd, struct statbuf *stat) {
    struct file_probe *probe = fd->data;
    probe->fstat_calls++;
    if (probe->fstat_error != 0)
        return probe->fstat_error;
    stat->mode = S_IFREG | 0644;
    stat->size = probe->size;
    stat->inode = fd->inode->number;
    stat->inode_device = fd->inode->device;
    return 0;
}

static const struct fd_ops cacheable_ops = {
    .page_cacheable = true,
    .pread = probe_pread,
    .close = probe_close,
};

static const struct fd_ops uncacheable_ops = {
    .pread = probe_pread,
    .close = probe_close,
};

static const struct fd_ops fallback_only_ops = {
    .page_cacheable = true,
    .read = probe_read,
    .lseek = probe_lseek,
    .close = probe_close,
};

static const struct fs_ops probe_fs = {
    .fstat = probe_fstat,
};

static bool fixture_init(struct mapping_fixture *fixture, int mount_flags) {
    memset(fixture, 0, sizeof(*fixture));
    lock_init(&fixture->group.lock);
    fixture->group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    fixture->task.group = &fixture->group;
    fixture->task.files = fdtable_new(4);
    if (IS_ERR(fixture->task.files))
        return false;
    fixture->mount = (struct mount) {
        .flags = mount_flags,
        .fs = &probe_fs,
    };
    current = &fixture->task;
    return true;
}

static void fixture_destroy(struct mapping_fixture *fixture) {
    fdtable_release(fixture->task.files);
    fixture->task.files = NULL;
    current = NULL;
    assert(fixture->mount.refcount == 0);
}

static fd_t install_probe_fd(struct mapping_fixture *fixture,
        struct file_probe *probe, const struct fd_ops *ops,
        mode_t_ type, int flags, qword_t device, ino_t inode_number) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return _ENOMEM;
    fd->data = probe;
    fd->type = type;
    fd->flags = (unsigned) flags;
    fd->mount = &fixture->mount;
    mount_retain(&fixture->mount);
    fd->inode = inode_get(&fixture->mount, device, inode_number);
    return f_install_task(&fixture->task, fd, 0);
}

static sdword_t open_mapping(struct mapping_fixture *fixture,
        qword_t fd, qword_t length, qword_t protection,
        qword_t flags, qword_t offset,
        struct guest_linux_file_mapping *mapping) {
    if ((offset & GUEST_MEMORY_PAGE_MASK) != 0)
        return _EINVAL;
    const struct guest_linux_file_mapping_context context = {
        .task_opaque = &fixture->task,
    };
    const struct guest_linux_file_mapping_request request = {
        .fd = fd,
        .offset = offset,
        .length = length,
        .protection = protection,
        .flags = flags,
    };
    struct guest_linux_file_mapping_handle handle = {0};
    sdword_t result = ish_aarch64_linux_file_mapping_service.acquire(
            &context, fd, &handle);
    if (result != 0)
        return result;
    result = ish_aarch64_linux_file_mapping_service.open(
            &handle, &request, mapping);
    ish_aarch64_linux_file_mapping_service.release(&handle);
    assert(handle.opaque == NULL);
    return result;
}

static int check_error_contract(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化错误语义夹具");
    struct file_probe readable = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe write_only = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe unsupported = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe fallback_only = {.size = GUEST_MEMORY_PAGE_SIZE};
    struct file_probe mountless = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t readable_fd = install_probe_fd(&fixture, &readable,
            &cacheable_ops, S_IFREG, O_RDONLY_, 1, 10);
    fd_t write_only_fd = install_probe_fd(&fixture, &write_only,
            &cacheable_ops, S_IFREG, O_WRONLY_, 1, 11);
    fd_t unsupported_fd = install_probe_fd(&fixture, &unsupported,
            &uncacheable_ops, S_IFREG, O_RDONLY_, 1, 12);
    fd_t fallback_only_fd = install_probe_fd(&fixture, &fallback_only,
            &fallback_only_ops, S_IFREG, O_RDONLY_, 1, 13);
    struct fd *mountless_file = fd_create(&uncacheable_ops);
    CHECK(mountless_file != NULL, "创建无 mount 文件描述符");
    mountless_file->data = &mountless;
    mountless_file->type = S_IFREG;
    mountless_file->flags = O_RDONLY_;
    fd_t mountless_fd = f_install_task(
            &fixture.task, mountless_file, 0);
    CHECK(readable_fd >= 0 && write_only_fd >= 0 && unsupported_fd >= 0 &&
            fallback_only_fd >= 0 && mountless_fd >= 0,
            "安装错误语义文件描述符");

    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, UINT64_C(99), 0,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0,
            &mapping) == _EBADF,
            "有效 offset 下无效 fd 先返回 EBADF");
    CHECK(open_mapping(&fixture, UINT64_C(99), GUEST_MEMORY_PAGE_SIZE,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 1,
            &mapping) == _EINVAL,
            "未对齐 offset 先于无效 fd 返回 EINVAL");
    CHECK(open_mapping(&fixture, (qword_t) write_only_fd, 0,
            GUEST_LINUX_PROT_READ, GUEST_LINUX_MAP_PRIVATE, 0,
            &mapping) == _EINVAL,
            "有效 fd 的零长度先于访问模式返回 EINVAL");
    CHECK(open_mapping(&fixture, (qword_t) write_only_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _EACCES,
            "MAP_PRIVATE 拒绝只写 fd");
    CHECK(open_mapping(&fixture, (qword_t) readable_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE,
            (qword_t) INT64_MAX & ~GUEST_MEMORY_PAGE_MASK,
            &mapping) == _EOVERFLOW,
            "文件映射范围越过 signed 文件上限返回 EOVERFLOW");
    CHECK(open_mapping(&fixture, (qword_t) unsupported_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _ENODEV,
            "未声明 page-cache 能力的普通文件返回 ENODEV");
    CHECK(open_mapping(&fixture, (qword_t) fallback_only_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _ENODEV,
            "分页 provider 必须提供不改描述偏移的 positioned read");
    CHECK(open_mapping(&fixture, (qword_t) mountless_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _ENODEV,
            "无 mount 文件描述符安全返回 ENODEV");

    CHECK(open_mapping(&fixture, (qword_t) readable_fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_WRITE,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0 &&
            mapping.pager != NULL &&
            mapping.maximum_protection == GUEST_LINUX_PROT_MASK,
            "只读 fd 允许可写私有映射并保留最大权限");
    guest_file_pager_release(mapping.pager);
    fixture_destroy(&fixture);
    CHECK(readable.close_calls == 1 && write_only.close_calls == 1 &&
            unsupported.close_calls == 1 && fallback_only.close_calls == 1 &&
            mountless.close_calls == 1,
            "错误路径和成功路径平衡 fd 生命周期");
    return 0;
}

static int check_noexec_contract(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, MS_NOEXEC_), "初始化 noexec 夹具");
    struct file_probe probe = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t fd = install_probe_fd(&fixture, &probe,
            &cacheable_ops, S_IFREG, O_RDONLY_, 2, 20);
    CHECK(fd >= 0, "安装 noexec 文件描述符");

    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_EXEC,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == _EPERM,
            "noexec mount 拒绝当前 PROT_EXEC");
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0 &&
            mapping.maximum_protection ==
                    (GUEST_LINUX_PROT_READ | GUEST_LINUX_PROT_WRITE),
            "noexec mount 同时移除后续 mprotect 的执行权限");
    guest_file_pager_release(mapping.pager);
    fixture_destroy(&fixture);
    return 0;
}

static int check_inode_cache_and_lifetime(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化 inode 共享夹具");
    struct file_probe first = {.size = GUEST_MEMORY_PAGE_SIZE + 3};
    struct file_probe second = {.size = GUEST_MEMORY_PAGE_SIZE + 3};
    for (size_t index = 0; index < sizeof(first.bytes); index++)
        first.bytes[index] = second.bytes[index] = (byte_t) (index ^ 0x5a);
    fd_t first_fd = install_probe_fd(&fixture, &first,
            &cacheable_ops, S_IFREG, O_RDONLY_, 3, 30);
    fd_t second_fd = install_probe_fd(&fixture, &second,
            &cacheable_ops, S_IFREG, O_RDONLY_, 3, 30);
    CHECK(first_fd >= 0 && second_fd >= 0,
            "安装同 inode 的独立描述符");
    struct fd *installed = f_get_task(&fixture.task, first_fd);
    struct inode_data *inode = installed->inode;

    struct guest_linux_file_mapping left = {0};
    struct guest_linux_file_mapping right = {0};
    CHECK(open_mapping(&fixture, (qword_t) first_fd,
            GUEST_MEMORY_PAGE_SIZE * 2, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &left) == 0,
            "从第一个描述符获取 pager");
    CHECK(open_mapping(&fixture, (qword_t) second_fd,
            GUEST_MEMORY_PAGE_SIZE * 2, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &right) == 0,
            "从第二个描述符获取 pager");
    CHECK(left.pager == right.pager && inode->file_pager == left.pager,
            "同 inode 的独立 fd 共享弱登记 pager");

    struct guest_page_backing *first_page = NULL;
    struct guest_page_backing *same_page = NULL;
    CHECK(guest_file_pager_get_page(left.pager, 0, &first_page) ==
            GUEST_FILE_PAGE_OK &&
            guest_file_pager_get_page(right.pager, 0, &same_page) ==
                    GUEST_FILE_PAGE_OK &&
            first_page == same_page && first.pread_calls == 1 &&
            second.pread_calls == 0,
            "共享 pager 对同一偏移只执行一次 provider 读取");
    guest_page_backing_release(same_page);
    guest_page_backing_release(first_page);

    CHECK(f_close_task(&fixture.task, first_fd) == 0 &&
            first.close_calls == 0,
            "关闭原 guest fd 后 pager 继续保活 provider fd");
    struct file_probe replacement = {.size = GUEST_MEMORY_PAGE_SIZE};
    fd_t reused = install_probe_fd(&fixture, &replacement,
            &cacheable_ops, S_IFREG, O_RDONLY_, 4, 40);
    CHECK(reused == first_fd, "复用已经关闭的 guest fd 槽");

    struct guest_page_backing *tail = NULL;
    CHECK(guest_file_pager_get_page(left.pager,
            GUEST_MEMORY_PAGE_SIZE, &tail) == GUEST_FILE_PAGE_OK,
            "原 fd 关闭并复用后仍可读取后续文件页");
    const byte_t *tail_bytes = guest_page_backing_bytes(tail);
    CHECK(tail_bytes[0] == first.bytes[GUEST_MEMORY_PAGE_SIZE] &&
            tail_bytes[2] == first.bytes[GUEST_MEMORY_PAGE_SIZE + 2] &&
            tail_bytes[3] == 0 &&
            tail_bytes[GUEST_MEMORY_PAGE_SIZE - 1] == 0,
            "文件尾页保留有效前缀并补零");
    guest_page_backing_release(tail);

    guest_file_pager_release(left.pager);
    CHECK(inode->file_pager != NULL && first.close_calls == 0,
            "仍有映射引用时不摘除 inode 弱槽");
    guest_file_pager_release(right.pager);
    CHECK(inode->file_pager == NULL && first.close_calls == 1,
            "最后映射释放后摘弱槽并关闭 provider fd");
    fixture_destroy(&fixture);
    CHECK(second.close_calls == 1 && replacement.close_calls == 1,
            "独立描述符和复用槽各自恰好关闭一次");
    return 0;
}

static int check_fault_translation_source(void) {
    struct mapping_fixture fixture;
    CHECK(fixture_init(&fixture, 0), "初始化读取故障夹具");
    struct file_probe probe = {
        .size = GUEST_MEMORY_PAGE_SIZE,
        .fstat_error = _EIO,
    };
    fd_t fd = install_probe_fd(&fixture, &probe,
            &cacheable_ops, S_IFREG, O_RDONLY_, 5, 50);
    CHECK(fd >= 0, "安装读取故障描述符");
    struct guest_linux_file_mapping mapping = {0};
    CHECK(open_mapping(&fixture, (qword_t) fd,
            GUEST_MEMORY_PAGE_SIZE, GUEST_LINUX_PROT_READ,
            GUEST_LINUX_MAP_PRIVATE, 0, &mapping) == 0,
            "映射建立时不执行 fstat 或读取");
    CHECK(probe.fstat_calls == 0 && probe.pread_calls == 0,
            "生产 mapping service 保持 lazy I/O");
    struct guest_page_backing *page = NULL;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_IO_ERROR && page == NULL &&
            probe.fstat_calls == 1 && probe.pread_calls == 0,
            "fault 时 fstat 错误转换为 pager I/O 故障");

    probe.fstat_error = 0;
    probe.pread_error = _EIO;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_IO_ERROR && page == NULL &&
            probe.fstat_calls == 2 && probe.pread_calls == 1,
            "fault 时 positioned read 错误转换为 pager I/O 故障");
    probe.pread_error = 0;
    CHECK(guest_file_pager_get_page(mapping.pager, 0, &page) ==
            GUEST_FILE_PAGE_OK && page != NULL &&
            probe.fstat_calls == 3 && probe.pread_calls == 2,
            "provider I/O 失败不缓存并允许后续 fault 重试");
    guest_page_backing_release(page);
    guest_file_pager_release(mapping.pager);
    fixture_destroy(&fixture);
    return 0;
}

int main(void) {
    if (check_error_contract() != 0)
        return 1;
    if (check_noexec_contract() != 0)
        return 1;
    if (check_inode_cache_and_lifetime() != 0)
        return 1;
    if (check_fault_translation_source() != 0)
        return 1;
    return 0;
}

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/task.h"

#define OUTPUT_PAGE UINT32_C(0x00300000)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "目录任务语义测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct legacy_dirent64 {
    qword_t inode;
    qword_t next_offset;
    word_t length;
    byte_t type;
    char name[];
} __attribute__((packed));

struct directory_probe {
    const char *names[2];
    qword_t inodes[2];
    size_t count;
    size_t position;
    off_t_ cookie_base;
    size_t error_position;
    int error;
    unsigned seek_calls;
};

static int probe_readdir(struct fd *fd, struct dir_entry *entry) {
    struct directory_probe *probe = fd->data;
    if (probe->error != 0 &&
            probe->position == probe->error_position)
        return probe->error;
    if (probe->position == probe->count)
        return 0;
    entry->inode = probe->inodes[probe->position];
    strcpy(entry->name, probe->names[probe->position]);
    probe->position++;
    return 1;
}

static off_t_ probe_telldir(struct fd *fd) {
    struct directory_probe *probe = fd->data;
    return probe->cookie_base + (off_t_) probe->position;
}

static void probe_seekdir(struct fd *fd, off_t_ position) {
    struct directory_probe *probe = fd->data;
    assert(position >= probe->cookie_base);
    off_t_ relative = position - probe->cookie_base;
    assert((qword_t) relative <= probe->count);
    probe->seek_calls++;
    probe->position = (size_t) relative;
}

static const struct fd_ops directory_ops = {
    .readdir = probe_readdir,
    .telldir = probe_telldir,
    .seekdir = probe_seekdir,
};

static const struct fd_ops regular_ops = {};

static struct fd *make_fd(const struct fd_ops *ops,
        void *data, mode_t_ type) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    fd->data = data;
    fd->type = type;
    return fd;
}

static int map_output_page(struct task *task) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(
            task->mem, PAGE(OUTPUT_PAGE), 1, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error;
}

int main(void) {
    struct task task = {0};
    struct tgroup group = {0};
    lock_init(&group.lock);
    group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {4, 4};
    task.group = &group;
    task.files = fdtable_new(1);
    CHECK(!IS_ERR(task.files), "创建任务 fd 表");
    struct mm *mm = mm_new();
    CHECK(mm != NULL, "创建 i386 guest 地址空间");
    task_set_mm(&task, mm);
    CHECK(map_output_page(&task) == 0,
            "创建并映射 i386 guest 输出页");
    current = &task;

    struct directory_probe directory = {
        .names = {"a", "second"},
        .inodes = {UINT64_C(0x1020304050607080), 22},
        .count = 2,
        .cookie_base = INT64_C(0x100000000),
    };
    struct fd *directory_fd = make_fd(
            &directory_ops, &directory, S_IFDIR);
    struct fd *regular_fd = make_fd(&regular_ops, NULL, S_IFREG);
    CHECK(directory_fd != NULL && regular_fd != NULL,
            "创建目录与普通文件探针");
    CHECK(f_install_task(&task, directory_fd, 0) == 0 &&
            f_install_task(&task, regular_fd, 0) == 1,
            "安装目录与普通文件探针");

    CHECK(sys_getdents64(99, UINT32_MAX, UINT32_MAX) == _EBADF,
            "legacy getdents64 优先返回 EBADF");
    CHECK(sys_getdents64(1, UINT32_MAX, UINT32_MAX) == _ENOTDIR,
            "legacy getdents64 优先返回 ENOTDIR");

    byte_t sentinel[32];
    memset(sentinel, 0xa5, sizeof(sentinel));
    CHECK(user_write(OUTPUT_PAGE, sentinel, sizeof(sentinel)) == 0,
            "写入容量不足测试哨兵");
    directory.position = 0;
    directory.seek_calls = 0;
    CHECK(sys_getdents64(0, OUTPUT_PAGE, 20) == _EINVAL &&
            directory.position == 0 && directory.seek_calls == 1,
            "首条 legacy 记录容量不足返回 EINVAL 并回滚");
    byte_t output[64];
    CHECK(user_read(OUTPUT_PAGE, output, sizeof(sentinel)) == 0 &&
            memcmp(output, sentinel, sizeof(sentinel)) == 0,
            "容量不足不修改 legacy guest 输出");

    CHECK(sys_getdents64(0, OUTPUT_PAGE, 21) == 21 &&
            directory.position == 1,
            "精确容量提交第一条 legacy 记录");
    CHECK(user_read(OUTPUT_PAGE, output, 21) == 0,
            "读回第一条 legacy 记录");
    struct legacy_dirent64 *first = (void *) output;
    CHECK(first->inode == directory.inodes[0] &&
            first->next_offset == UINT64_C(0x100000001) &&
            first->length == 21 &&
            first->type == 0 && strcmp(first->name, "a") == 0,
            "legacy 适配器保持未对齐线格式和 64 位目录 cookie");
    CHECK(sys_getdents64(0, OUTPUT_PAGE, 26) == 26 &&
            directory.position == 2,
            "容量不足重试从第二条 legacy 记录继续");
    CHECK(user_read(OUTPUT_PAGE, output, 26) == 0 &&
            ((struct legacy_dirent64 *) output)->next_offset ==
                    UINT64_C(0x100000002),
            "第二条 legacy 记录继续保留 64 位目录 cookie");
    CHECK(sys_getdents64(0, UINT32_MAX, UINT32_MAX) == 0,
            "legacy EOF 不访问无效 guest 指针");

    directory.position = 0;
    directory.error_position = 0;
    directory.error = _EIO;
    CHECK(sys_getdents64(0, OUTPUT_PAGE, sizeof(output)) == _EIO &&
            directory.position == 0,
            "首条前的 legacy 后端错误原样传播");
    directory.error_position = 1;
    CHECK(sys_getdents64(0, OUTPUT_PAGE, sizeof(output)) == 21 &&
            directory.position == 1,
            "已有记录后的 legacy 后端错误返回完整前缀");
    directory.error = 0;
    CHECK(sys_getdents64(0, OUTPUT_PAGE, 26) == 26 &&
            directory.position == 2,
            "后端错误解除后 legacy 游标从失败条继续");

    directory.position = 0;
    addr_t first_fault = OUTPUT_PAGE + PAGE_SIZE - 10;
    CHECK(sys_getdents64(0, first_fault, sizeof(output)) == _EFAULT &&
            directory.position == 0,
            "首条 legacy 写 fault 返回 EFAULT 并回滚");
    CHECK(trylock(&directory_fd->lock) == 0,
            "legacy 写 fault 后释放目录位置锁");
    unlock(&directory_fd->lock);

    directory.position = 0;
    addr_t second_fault = OUTPUT_PAGE + PAGE_SIZE - 31;
    CHECK(sys_getdents64(0, second_fault, sizeof(output)) == 21 &&
            directory.position == 1,
            "第二条 legacy 写 fault 返回完整前缀并回滚失败条");
    CHECK(sys_getdents64(0, OUTPUT_PAGE, 26) == 26 &&
            directory.position == 2,
            "legacy 写 fault 后重新输出失败条目");

    current = NULL;
    fdtable_release(task.files);
    mm_release(task.mm);
    return 0;
}

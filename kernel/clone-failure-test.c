#include <stdio.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define TEST_CLONE_VM UINT32_C(0x00000100)
#define TEST_CLONE_FS UINT32_C(0x00000200)
#define TEST_CLONE_FILES UINT32_C(0x00000400)
#define TEST_CLONE_SIGHAND UINT32_C(0x00000800)
#define TEST_CLONE_THREAD UINT32_C(0x00010000)
#define TEST_CLONE_SETTLS UINT32_C(0x00080000)
#define TEST_CLONE_PARENT_SETTID UINT32_C(0x00100000)
#define TEST_CLONE_CHILD_SETTID UINT32_C(0x01000000)
#define TID_PAGE UINT32_C(0x00100000)
#define PARENT_TID_ADDRESS (TID_PAGE + 16)
#define CHILD_TID_ADDRESS (TID_PAGE + 32)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "clone 失败清理测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    struct task parent = {0};
    struct tgroup group = {0};
    struct fdtable files = {.refcount = 1};
    struct fs_info fs = {.refcount = 1};
    struct sighand sighand = {.refcount = 1};

    list_init(&group.threads);
    list_init(&group.session);
    list_init(&group.pgroup);
    lock_init(&group.lock);
    group.leader = &parent;

    list_init(&parent.group_links);
    list_add(&group.threads, &parent.group_links);
    list_init(&parent.children);
    list_init(&parent.siblings);
    list_init(&parent.queue);
    parent.pid = parent.tgid = 42;
    parent.group = &group;

    lock_init(&files.lock);
    parent.files = &files;
    lock_init(&fs.lock);
    parent.fs = &fs;
    lock_init(&sighand.lock);
    parent.sighand = &sighand;
    task_set_mm(&parent, mm_new());
    CHECK(parent.mm != NULL, "创建父任务地址空间");
    write_wrlock(&parent.mem->lock);
    int map_error = pt_map_nothing(
            parent.mem, PAGE(TID_PAGE), 1, P_RWX);
    write_wrunlock(&parent.mem->lock);
    CHECK(map_error == 0, "映射 clone TID 发布验证页");
    current = &parent;

    const dword_t shared_flags = TEST_CLONE_VM | TEST_CLONE_FS |
            TEST_CLONE_FILES | TEST_CLONE_SIGHAND |
            TEST_CLONE_THREAD;
    parent.aarch64_process =
            (struct aarch64_linux_process *) (uintptr_t) 0x1234;
    CHECK(sys_clone(TEST_CLONE_THREAD, 0, 0, 0, 0) ==
                    (dword_t) _EINVAL &&
            list_size(&group.threads) == 1 &&
            list_empty(&parent.children) && parent.mm->refcount == 1 &&
            files.refcount == 1 && fs.refcount == 1 &&
            sighand.refcount == 1,
            "AArch64 clone 在分配资源前拒绝缺少依赖的线程标志");
    parent.aarch64_process = NULL;
    static const struct {
        dword_t flags;
        addr_t parent_tid;
        addr_t tls;
        addr_t child_tid;
    } failures[] = {
        {TEST_CLONE_SETTLS, 0, UINT32_C(0x7fff0000), 0},
        {TEST_CLONE_CHILD_SETTID, 0, 0, UINT32_C(0x7fff1000)},
        {TEST_CLONE_PARENT_SETTID, UINT32_C(0x7fff2000), 0, 0},
        {TEST_CLONE_SETTLS, 0, UINT32_C(0x7fff3000), 0},
    };
    for (size_t index = 0; index < array_size(failures); index++) {
        dword_t flags = shared_flags | failures[index].flags;
        if (index == array_size(failures) - 1)
            flags &= ~TEST_CLONE_THREAD;
        dword_t result = sys_clone(flags, 0,
                failures[index].parent_tid,
                failures[index].tls,
                failures[index].child_tid);
        CHECK(result == (dword_t) _EFAULT,
                "无效 TLS/TID 地址让 clone 返回 EFAULT");
        CHECK(list_size(&group.threads) == 1 &&
                list_first_entry(&group.threads,
                        struct task, group_links) == &parent,
                "失败任务未挂入父线程组且父节点保持有效");
        CHECK(list_empty(&parent.children),
                "失败任务已从父任务 children 链移除");
        CHECK(list_empty(&group.session) && list_empty(&group.pgroup),
                "失败的新进程未发布 session 或 pgroup 节点");
        CHECK(parent.mm->refcount == 1 && files.refcount == 1 &&
                fs.refcount == 1 && sighand.refcount == 1,
                "失败路径恢复共享资源引用计数");
    }

    const dword_t parent_sentinel = UINT32_C(0x11223344);
    const dword_t child_sentinel = UINT32_C(0x55667788);
    const dword_t publication_flags = shared_flags |
            TEST_CLONE_PARENT_SETTID | TEST_CLONE_CHILD_SETTID;
    CHECK(user_put(PARENT_TID_ADDRESS, parent_sentinel) == 0 &&
            user_put(CHILD_TID_ADDRESS, child_sentinel) == 0,
            "写入 clone 发布门哨兵值");

    group.exec_task = &parent;
    CHECK(sys_clone(publication_flags, 0,
                    PARENT_TID_ADDRESS, 0, CHILD_TID_ADDRESS) ==
                    (dword_t) _EAGAIN,
            "exec 收敛期间拒绝发布新线程");
    group.exec_task = NULL;
    dword_t parent_tid_value;
    dword_t child_tid_value;
    CHECK(user_get(PARENT_TID_ADDRESS, parent_tid_value) == 0 &&
            user_get(CHILD_TID_ADDRESS, child_tid_value) == 0 &&
            parent_tid_value == parent_sentinel &&
            child_tid_value == child_sentinel,
            "exec 发布门拒绝后不污染 parent/child TID 地址");

    group.doing_group_exit = true;
    CHECK(sys_clone(publication_flags, 0,
                    PARENT_TID_ADDRESS, 0, CHILD_TID_ADDRESS) ==
                    (dword_t) _EAGAIN,
            "线程组退出期间拒绝发布新线程");
    group.doing_group_exit = false;
    CHECK(user_get(PARENT_TID_ADDRESS, parent_tid_value) == 0 &&
            user_get(CHILD_TID_ADDRESS, child_tid_value) == 0 &&
            parent_tid_value == parent_sentinel &&
            child_tid_value == child_sentinel,
            "组退出发布门拒绝后不污染 TID 地址");

    lock(&sighand.lock);
    sigset_add(&parent.pending, SIGKILL_);
    unlock(&sighand.lock);
    CHECK(sys_clone(publication_flags, 0,
                    PARENT_TID_ADDRESS, 0, CHILD_TID_ADDRESS) ==
                    (dword_t) _EINTR,
            "当前线程已有致命信号时 clone 返回 EINTR");
    lock(&sighand.lock);
    signal_discard_pending_locked(&parent, SIGKILL_);
    unlock(&sighand.lock);
    CHECK(user_get(PARENT_TID_ADDRESS, parent_tid_value) == 0 &&
            user_get(CHILD_TID_ADDRESS, child_tid_value) == 0 &&
            parent_tid_value == parent_sentinel &&
            child_tid_value == child_sentinel,
            "致命信号发布门拒绝后不污染 TID 地址");

    CHECK(list_size(&group.threads) == 1 &&
            list_empty(&parent.children) && parent.mm->refcount == 1 &&
            files.refcount == 1 && fs.refcount == 1 &&
            sighand.refcount == 1,
            "所有发布门失败路径均恢复任务与共享资源");

    current = NULL;
    mm_release(parent.mm);
    return 0;
}

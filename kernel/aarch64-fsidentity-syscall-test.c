#include <stdio.h>
#include <sys/stat.h>

#include "kernel/aarch64-syscall-service.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "AArch64 文件身份测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static qword_t invoke(struct task *task, qword_t number, qword_t id) {
    const struct guest_linux_syscall_context context = {.task_opaque = task};
    const struct guest_linux_syscall syscall = {
        .number = number, .arguments = {id},
    };
    struct guest_linux_user_fault fault;
    return ish_aarch64_linux_syscall_service.dispatch(&context, &syscall, &fault);
}

int main(void) {
    struct task task;
    current = &task;
    for (qword_t number = 151; number <= 152; number++) {
        task = (struct task) {
            .uid = 100, .euid = 101, .suid = 102, .fsuid = 103,
            .gid = 100, .egid = 101, .sgid = 102, .fsgid = 103,
        };
        uid_t_ *id = number == 151 ? &task.fsuid : &task.fsgid;
        CHECK(invoke(&task, number, UINT64_MAX) == 103 && *id == 103,
                "-1 查询返回旧身份，不修改任何凭据");
        CHECK(invoke(&task, number, 999) == 103 && *id == 103,
                "无权限修改返回旧身份而不是 EPERM 或 ENOSYS");
        CHECK(invoke(&task, number, 103) == 103 && *id == 103,
                "允许保持当前文件身份");
        for (uid_t_ allowed = 100; allowed <= 102; allowed++) {
            uid_t_ previous = *id;
            CHECK(invoke(&task, number, allowed) == previous && *id == allowed,
                    "允许切换为真实、有效或保存身份");
        }
        CHECK(task.uid == 100 && task.euid == 101 && task.suid == 102 &&
                task.gid == 100 && task.egid == 101 && task.sgid == 102,
                "文件身份修改不改变普通 UID/GID");
        task.euid = 0;
        CHECK(invoke(&task, number, UINT64_C(0x12345678abcdef01)) == 102 &&
                *id == UINT32_C(0xabcdef01),
                "root 可设置任意有效身份，忽略 AArch64 参数高 32 位");
        CHECK(invoke(&task, number, UINT64_MAX) == UINT32_C(0xabcdef01) &&
                *id == UINT32_C(0xabcdef01),
                "完整 32 位旧身份零扩展返回，root 的 -1 查询也不修改身份");
    }

    task = (struct task) {
        .uid = 100, .euid = 0, .suid = 102, .fsuid = 100,
        .gid = 200, .egid = 201, .sgid = 202, .fsgid = 200,
    };
    struct statbuf stat = {.mode = S_IFREG | 0400, .uid = 900, .gid = 901};
    CHECK(access_check_task(&task, &stat, AC_R) == _EACCES,
            "有效 UID 为 root 时，已降权的 fsuid 仍受文件权限限制");
    CHECK(invoke(&task, 151, 900) == 100 &&
            access_check_task(&task, &stat, AC_R) == 0,
            "setfsuid 立即参与文件所有者权限判断");
    stat.mode = S_IFREG | 0040;
    stat.uid = 999;
    CHECK(access_check_task(&task, &stat, AC_R) == _EACCES &&
            invoke(&task, 152, 901) == 200 &&
            access_check_task(&task, &stat, AC_R) == 0,
            "setfsgid 立即参与文件组权限判断");

    struct task *child = task_create_(&task);
    CHECK(child != NULL, "创建用于验证继承的子任务");
    CHECK(child->fsuid == 900 && child->fsgid == 901 && child->euid == 0,
            "fork 保留独立文件身份，不能重新覆盖为有效身份");
    task_abort_create(child);

    CHECK(sys_setresgid((uid_t_) -1, 202, (uid_t_) -1) == 0 &&
            task.fsgid == 202, "setresgid 同步文件系统组身份");
    CHECK(sys_setgid(203) == 0 && task.fsgid == 203,
            "setgid 同步文件系统组身份");
    CHECK(sys_setresuid((uid_t_) -1, 102, (uid_t_) -1) == 0 &&
            task.fsuid == 102, "setresuid 同步文件系统用户身份");
    CHECK(sys_setuid(100) == 0 && task.fsuid == 100,
            "setuid 同步文件系统用户身份");
    CHECK(sys_setuid(999) == _EPERM && task.fsuid == 100,
            "身份切换失败不能改变文件系统身份");

    stat = (struct statbuf) {.mode = S_IFREG | 0600};
    task.fsuid = 0;
    CHECK(access_check_task(&task, &stat, AC_X) == _EACCES,
            "root 不可执行没有任何执行位的普通文件");
    stat.mode |= 0001;
    CHECK(access_check_task(&task, &stat, AC_X) == 0,
            "任一执行位允许 root 执行普通文件");
    current = NULL;
    return 0;
}

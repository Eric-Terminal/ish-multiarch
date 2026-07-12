#include <stdio.h>

#include "fs/fd.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/task.h"

#define USER_PAGE UINT32_C(0x00100000)
#define TIMEOUT_ADDRESS (USER_PAGE + 16)
#define MASK_ADDRESS (USER_PAGE + 64)
#define PSELECT_MASK_ADDRESS (USER_PAGE + 96)
#define EVENTS_ADDRESS (USER_PAGE + 128)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "等待信号掩码测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct pselect_mask_argument {
    addr_t address;
    dword_t size;
};

static void reset_mask(struct task *task, sigset_t_ blocked) {
    task->blocked = blocked;
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->pending = 0;
}

int main(void) {
    struct task task = {0};
    struct tgroup group = {0};
    struct sighand sighand = {0};
    lock_init(&group.lock);
    lock_init(&sighand.lock);
    lock_init(&task.waiting_cond_lock);
    group.limits[RLIMIT_NOFILE_] = (struct rlimit_) {16, 16};
    task.group = &group;
    task.sighand = &sighand;
    task.files = fdtable_new(1);
    CHECK(!IS_ERR(task.files), "创建文件描述符表");
    struct mm *mm = mm_new();
    CHECK(mm != NULL, "创建 i386 用户地址空间");
    task_set_mm(&task, mm);
    current = &task;

    write_wrlock(&task.mem->lock);
    int map_error = pt_map_nothing(
            task.mem, PAGE(USER_PAGE), 1, P_RWX);
    write_wrunlock(&task.mem->lock);
    CHECK(map_error == 0, "映射等待参数页");

    const struct timespec_ immediate = {0};
    const sigset_t_ original = sig_mask(SIGUSR1_);
    const sigset_t_ temporary = sig_mask(SIGUSR2_);
    CHECK(user_put(TIMEOUT_ADDRESS, immediate) == 0 &&
            user_put(MASK_ADDRESS, temporary) == 0,
            "写入等待超时和信号掩码");

    reset_mask(&task, original);
    CHECK(sys_ppoll(0, 0, TIMEOUT_ADDRESS,
                    MASK_ADDRESS, sizeof(sigset_t_)) == 0 &&
            task.blocked == original && !task.has_saved_mask,
            "ppoll 超时返回后恢复原掩码");

    const struct pselect_mask_argument pselect_mask = {
        .address = MASK_ADDRESS,
        .size = sizeof(sigset_t_),
    };
    CHECK(user_put(PSELECT_MASK_ADDRESS, pselect_mask) == 0,
            "写入 pselect 掩码参数");
    reset_mask(&task, original);
    CHECK(sys_pselect(0, 0, 0, 0, TIMEOUT_ADDRESS,
                    PSELECT_MASK_ADDRESS) == 0 &&
            task.blocked == original && !task.has_saved_mask,
            "pselect 超时返回后恢复原掩码");
    CHECK(sys_pselect(0, 0, 0, 0,
                    TIMEOUT_ADDRESS, 0) == 0,
            "pselect 接受空的可选掩码参数包");

    fd_t epoll_fd = sys_epoll_create0();
    CHECK(epoll_fd >= 0, "创建 epoll 等待实例");
    reset_mask(&task, original);
    CHECK(sys_epoll_pwait(epoll_fd, EVENTS_ADDRESS, 1, 0,
                    MASK_ADDRESS, sizeof(sigset_t_)) == 0 &&
            task.blocked == original && !task.has_saved_mask,
            "epoll_pwait 超时返回后恢复原掩码");

    reset_mask(&task, original);
    CHECK(sys_epoll_pwait(99, EVENTS_ADDRESS, 1, 0,
                    MASK_ADDRESS, sizeof(sigset_t_)) == _EBADF &&
            task.blocked == original && !task.has_saved_mask,
            "epoll_pwait 早期错误后恢复原掩码");

    const sigset_t_ unblocked = 0;
    CHECK(user_put(MASK_ADDRESS, unblocked) == 0,
            "写入可中断等待的临时掩码");
    reset_mask(&task, original);
    task.pending = sig_mask(SIGUSR1_);
    CHECK(sys_ppoll(0, 0, 0,
                    MASK_ADDRESS, sizeof(sigset_t_)) ==
                    (dword_t) _EINTR &&
            task.blocked == unblocked && task.has_saved_mask &&
            task.saved_mask == original,
            "ppoll 被信号中断时保留原掩码供投递路径恢复");
    task.pending = 0;
    sigmask_restore_temp_task(&task);

    reset_mask(&task, original);
    task.pending = sig_mask(SIGUSR1_);
    CHECK(sys_pselect(0, 0, 0, 0, 0,
                    PSELECT_MASK_ADDRESS) == (dword_t) _EINTR &&
            task.blocked == unblocked && task.has_saved_mask &&
            task.saved_mask == original,
            "pselect 被信号中断时保留原掩码供投递路径恢复");
    task.pending = 0;
    sigmask_restore_temp_task(&task);

    reset_mask(&task, original);
    task.pending = sig_mask(SIGUSR1_);
    CHECK(sys_epoll_pwait(epoll_fd, EVENTS_ADDRESS, 1, -1,
                    MASK_ADDRESS, sizeof(sigset_t_)) == _EINTR &&
            task.blocked == unblocked && task.has_saved_mask &&
            task.saved_mask == original,
            "epoll_pwait 被信号中断时保留原掩码供投递路径恢复");
    task.pending = 0;
    sigmask_restore_temp_task(&task);

    CHECK(f_close_task(&task, epoll_fd) == 0,
            "关闭 epoll 等待实例");
    current = NULL;
    fdtable_release(task.files);
    mm_release(task.mm);
    pthread_mutex_destroy(&task.waiting_cond_lock.m);
    return 0;
}

#ifndef GUEST_LINUX_SIGNAL_SERVICE_H
#define GUEST_LINUX_SIGNAL_SERVICE_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

#include "misc.h"

enum guest_linux_signal_payload_kind {
    GUEST_LINUX_SIGNAL_PAYLOAD_NONE,
    GUEST_LINUX_SIGNAL_PAYLOAD_KILL,
    GUEST_LINUX_SIGNAL_PAYLOAD_TIMER,
    GUEST_LINUX_SIGNAL_PAYLOAD_CHILD,
    GUEST_LINUX_SIGNAL_PAYLOAD_FAULT,
    GUEST_LINUX_SIGNAL_PAYLOAD_SIGSYS,
};

struct guest_linux_signal_info {
    sdword_t signal;
    sdword_t error;
    sdword_t code;
    dword_t payload_kind;
    union {
        struct {
            sdword_t pid;
            dword_t uid;
        } kill;
        struct {
            sdword_t timer;
            sdword_t overrun;
            qword_t value;
            sdword_t private_value;
        } timer;
        struct {
            sdword_t pid;
            dword_t uid;
            sdword_t status;
            sqword_t utime;
            sqword_t stime;
        } child;
        struct {
            qword_t address;
        } fault;
        struct {
            qword_t address;
            sdword_t syscall;
            dword_t architecture;
        } sigsys;
    };
};

struct guest_linux_signal_action {
    qword_t handler;
    qword_t flags;
    qword_t restorer;
    qword_t mask;
};

struct guest_linux_signal_stack {
    qword_t base;
    qword_t size;
    dword_t flags;
    dword_t reserved;
};

struct guest_linux_signal_delivery {
    struct guest_linux_signal_info info;
    struct guest_linux_signal_action action;
    qword_t blocked_mask;
    struct guest_linux_signal_stack altstack;
};

struct guest_linux_signal_restore_request {
    qword_t stack_pointer;
    qword_t blocked_mask;
    struct guest_linux_signal_stack altstack;
};

enum guest_linux_signal_install_status {
    GUEST_LINUX_SIGNAL_INSTALL_COMPLETE,
    GUEST_LINUX_SIGNAL_INSTALL_FRAME_FAULT,
};

enum guest_linux_signal_poll_status {
    GUEST_LINUX_SIGNAL_POLL_IDLE,
    GUEST_LINUX_SIGNAL_POLL_HANDLER,
    GUEST_LINUX_SIGNAL_POLL_STOP,
    GUEST_LINUX_SIGNAL_POLL_TERMINATE,
};

struct guest_linux_signal_poll_result {
    dword_t status;
    sdword_t signal;
};

struct guest_linux_signal_context {
    void *runtime_opaque;
    void *task_opaque;
};

typedef enum guest_linux_signal_install_status
        (*guest_linux_signal_installer)(void *opaque,
        const struct guest_linux_signal_delivery *delivery);

typedef struct guest_linux_signal_poll_result
        (*guest_linux_signal_poll)(
        const struct guest_linux_signal_context *context,
        guest_linux_signal_installer installer,
        void *installer_opaque);

typedef void (*guest_linux_signal_restore)(
        const struct guest_linux_signal_context *context,
        const struct guest_linux_signal_restore_request *request);

typedef void (*guest_linux_signal_bad_frame)(
        const struct guest_linux_signal_context *context,
        qword_t frame_address);

struct guest_linux_signal_service {
    void *runtime_opaque;
    guest_linux_signal_poll poll;
    guest_linux_signal_restore restore;
    guest_linux_signal_bad_frame bad_frame;
};

// poll 是同步事务：backend 在选择锁内先消费队列节点，再调用 installer。
// installer 只准备 caller-owned 候选状态，不得发布 CPU、保存 delivery 指针、
// 阻塞或重入 service。COMPLETE 后 backend 才提交 mask 与 SA_RESETHAND；
// FRAME_FAULT 不提交原信号状态，并在同一次 poll 内改派强制故障。
// 只有最终返回 HANDLER 时，调用方才能发布最后一次成功回调产生的候选 CPU。
// STOP 已提交停止状态和父任务通知；TERMINATE 返回时已释放锁但尚未执行退出。
// restore 同步提交当前 task 的阻塞掩码与替代栈；bad_frame 同步强制排队
// SIGSEGV。两种回调都不得保存 context、request 或其内部数据的指针。

_Static_assert(sizeof(enum guest_linux_signal_install_status) ==
                sizeof(dword_t) &&
        sizeof(enum guest_linux_signal_poll_status) == sizeof(dword_t),
        "Linux signal service 状态枚举必须保持 32 位 ABI");
_Static_assert(sizeof(struct guest_linux_signal_info) == 48 &&
        _Alignof(struct guest_linux_signal_info) == 8 &&
        offsetof(struct guest_linux_signal_info, signal) == 0 &&
        offsetof(struct guest_linux_signal_info, error) == 4 &&
        offsetof(struct guest_linux_signal_info, code) == 8 &&
        offsetof(struct guest_linux_signal_info, payload_kind) == 12 &&
        offsetof(struct guest_linux_signal_info, kill) == 16 &&
        offsetof(struct guest_linux_signal_info, child.utime) == 32,
        "Linux signal service 信息布局必须与 guest 选择无关");
_Static_assert(sizeof(struct guest_linux_signal_action) == 32 &&
        _Alignof(struct guest_linux_signal_action) == 8 &&
        offsetof(struct guest_linux_signal_action, handler) == 0 &&
        offsetof(struct guest_linux_signal_action, flags) == 8 &&
        offsetof(struct guest_linux_signal_action, restorer) == 16 &&
        offsetof(struct guest_linux_signal_action, mask) == 24,
        "Linux signal service 动作布局必须固定");
_Static_assert(sizeof(struct guest_linux_signal_stack) == 24 &&
        _Alignof(struct guest_linux_signal_stack) == 8 &&
        offsetof(struct guest_linux_signal_stack, base) == 0 &&
        offsetof(struct guest_linux_signal_stack, size) == 8 &&
        offsetof(struct guest_linux_signal_stack, flags) == 16 &&
        offsetof(struct guest_linux_signal_stack, reserved) == 20,
        "Linux signal service 替代栈布局必须固定");
_Static_assert(sizeof(struct guest_linux_signal_delivery) == 112 &&
        _Alignof(struct guest_linux_signal_delivery) == 8 &&
        offsetof(struct guest_linux_signal_delivery, info) == 0 &&
        offsetof(struct guest_linux_signal_delivery, action) == 48 &&
        offsetof(struct guest_linux_signal_delivery, blocked_mask) == 80 &&
        offsetof(struct guest_linux_signal_delivery, altstack) == 88,
        "Linux signal service 派送 DTO 布局必须固定");
_Static_assert(sizeof(struct guest_linux_signal_restore_request) == 40 &&
        _Alignof(struct guest_linux_signal_restore_request) == 8 &&
        offsetof(struct guest_linux_signal_restore_request,
                stack_pointer) == 0 &&
        offsetof(struct guest_linux_signal_restore_request,
                blocked_mask) == 8 &&
        offsetof(struct guest_linux_signal_restore_request, altstack) == 16,
        "Linux signal service 恢复 DTO 布局必须固定");
_Static_assert(sizeof(struct guest_linux_signal_poll_result) == 8 &&
        offsetof(struct guest_linux_signal_poll_result, status) == 0 &&
        offsetof(struct guest_linux_signal_poll_result, signal) == 4,
        "Linux signal service poll 结果布局必须固定");

#endif

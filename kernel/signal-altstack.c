#include "debug.h"
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "kernel/task.h"

void task_altstack_reset(struct task *task) {
    task->altstack = (struct signal_altstack) {
        .flags = SS_DISABLE_,
    };
}

void task_altstack_on_clone(
        struct task *task, bool shares_vm, bool is_vfork) {
    if (shares_vm && !is_vfork)
        task_altstack_reset(task);
}

bool task_on_altstack(const struct task *task, qword_t stack_pointer) {
    const struct signal_altstack *altstack = &task->altstack;
    return altstack->size != 0 && stack_pointer > altstack->stack &&
            stack_pointer - altstack->stack <= altstack->size;
}

bool i386_signal_frame_pointer(const struct task *task,
        const struct signal_action *action, dword_t original_sp,
        size_t frame_size, dword_t *frame_pointer) {
    bool nested_altstack = task_on_altstack(task, original_sp);
    bool entering_altstack = !nested_altstack &&
            (action->flags & SA_ONSTACK_) && task->altstack.size != 0;
    qword_t sp = original_sp;
    if (entering_altstack) {
        if (task->altstack.stack > UINT64_MAX - task->altstack.size)
            return false;
        sp = task->altstack.stack + task->altstack.size;
        if (sp > UINT32_MAX)
            return false;
    }

    if (xsave_extra != 0) {
        if (xsave_extra < 0 || fxsave_extra < 0 ||
                (qword_t) xsave_extra > sp)
            return false;
        sp -= (qword_t) xsave_extra;
        sp &= ~UINT64_C(0x3f);
        if ((qword_t) fxsave_extra > sp)
            return false;
        sp -= (qword_t) fxsave_extra;
    }
    if ((qword_t) frame_size > sp)
        return false;
    sp -= (qword_t) frame_size;
    sp = (sp + 4) & ~UINT64_C(0xf);
    if (sp < 4)
        return false;
    sp -= 4;
    if (sp > UINT32_MAX ||
            (qword_t) frame_size >
                    UINT64_C(1) + UINT32_MAX - sp)
        return false;

    if (nested_altstack || entering_altstack) {
        if (task->altstack.stack > UINT64_MAX - task->altstack.size)
            return false;
        qword_t altstack_top =
                task->altstack.stack + task->altstack.size;
        if (!task_on_altstack(task, sp) ||
                (qword_t) frame_size > altstack_top - sp)
            return false;
    }

    *frame_pointer = (dword_t) sp;
    return true;
}

static dword_t task_altstack_status(
        const struct task *task, qword_t stack_pointer) {
    if (task->altstack.size == 0)
        return SS_DISABLE_;
    return task_on_altstack(task, stack_pointer) ? SS_ONSTACK_ : 0;
}

int task_sigaltstack(struct task *task, qword_t stack_pointer,
        const struct signal_altstack *new_stack,
        struct signal_altstack *old_stack,
        qword_t minimum_size, qword_t address_limit) {
    if (old_stack != NULL) {
        *old_stack = task->altstack;
        old_stack->flags = task_altstack_status(task, stack_pointer);
    }
    if (new_stack == NULL)
        return 0;

    if (task_on_altstack(task, stack_pointer))
        return _EPERM;
    if (new_stack->flags != 0 && new_stack->flags != SS_DISABLE_ &&
            new_stack->flags != SS_ONSTACK_)
        return _EINVAL;
    if (new_stack->stack == task->altstack.stack &&
            new_stack->size == task->altstack.size &&
            new_stack->flags == task->altstack.flags)
        return 0;
    if (new_stack->flags == SS_DISABLE_) {
        task_altstack_reset(task);
        return 0;
    }
    // 栈顶会直接写入 guest SP，因此它本身也必须位于地址上界内。
    if (new_stack->size < minimum_size ||
            new_stack->size > address_limit ||
            new_stack->stack > address_limit - new_stack->size)
        return _ENOMEM;

    task->altstack = *new_stack;
    return 0;
}

static struct stack_t_ pack_i386_altstack(
        const struct signal_altstack *altstack) {
    return (struct stack_t_) {
        .stack = (addr_t) altstack->stack,
        .flags = altstack->flags,
        .size = (dword_t) altstack->size,
    };
}

dword_t sys_sigaltstack(addr_t ss_addr, addr_t old_ss_addr) {
    STRACE("sigaltstack(0x%x, 0x%x)", ss_addr, old_ss_addr);
    struct stack_t_ new_wire;
    struct signal_altstack new_stack;
    if (ss_addr != 0) {
        if (user_get(ss_addr, new_wire))
            return _EFAULT;
        new_stack = (struct signal_altstack) {
            .stack = new_wire.stack,
            .size = new_wire.size,
            .flags = new_wire.flags,
        };
    }

    struct signal_altstack old_stack;
    int error = task_sigaltstack(current, current->cpu.esp,
            ss_addr != 0 ? &new_stack : NULL,
            old_ss_addr != 0 ? &old_stack : NULL,
            MINSIGSTKSZ_, UINT32_MAX);
    if (error < 0)
        return (dword_t) error;
    if (old_ss_addr != 0) {
        struct stack_t_ old_wire = pack_i386_altstack(&old_stack);
        if (user_put(old_ss_addr, old_wire))
            return _EFAULT;
    }
    return 0;
}

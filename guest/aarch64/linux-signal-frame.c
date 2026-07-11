#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-signal-frame.h"
#include "guest/linux/user-memory.h"

struct aarch64_linux_frame_record {
    qword_t fp;
    qword_t lr;
};

struct aarch64_linux_signal_stack_image {
    struct aarch64_linux_rt_sigframe frame;
    struct aarch64_linux_frame_record record;
};

_Static_assert(sizeof(struct aarch64_linux_frame_record) == 16,
        "AArch64 信号展开记录必须固定为 16 字节");
_Static_assert(__builtin_offsetof(struct aarch64_linux_signal_stack_image,
                record) == sizeof(struct aarch64_linux_rt_sigframe) &&
        sizeof(struct aarch64_linux_signal_stack_image) == 4704,
        "AArch64 信号帧与展开记录必须连续且保持 16 字节对齐");
_Static_assert(sizeof(((struct cpu_state *) 0)->v) ==
                sizeof(((struct aarch64_linux_fpsimd_context *) 0)->vregs),
        "AArch64 CPU 与 signal FPSIMD 向量区大小必须一致");

static void reset_fault(struct guest_memory_fault *fault,
        guest_addr_t address, enum guest_memory_access access) {
    *fault = (struct guest_memory_fault) {
        .address = address,
        .access = access,
        .kind = GUEST_MEMORY_FAULT_NONE,
    };
}

static bool signal_frame_addresses(guest_addr_t stack_top,
        guest_addr_t *frame_address, guest_addr_t *record_address) {
    if (stack_top < sizeof(struct aarch64_linux_frame_record))
        return false;
    guest_addr_t record = (stack_top -
            sizeof(struct aarch64_linux_frame_record)) & ~UINT64_C(0xf);
    if (record < sizeof(struct aarch64_linux_rt_sigframe))
        return false;
    *record_address = record;
    *frame_address = record - sizeof(struct aarch64_linux_rt_sigframe);
    return true;
}

static struct aarch64_linux_fpsimd_context pack_fpsimd(
        const struct cpu_state *cpu) {
    struct aarch64_linux_fpsimd_context fpsimd = {
        .head = {
            .magic = AARCH64_LINUX_FPSIMD_MAGIC,
            .size = sizeof(struct aarch64_linux_fpsimd_context),
        },
        .fpsr = cpu->fpsr,
        .fpcr = cpu->fpcr,
    };
    memcpy(fpsimd.vregs, cpu->v, sizeof(fpsimd.vregs));
    return fpsimd;
}

static void pack_interrupted_context(
        struct aarch64_linux_rt_sigframe *frame,
        const struct cpu_state *cpu,
        const struct aarch64_linux_signal_delivery *delivery) {
    frame->uc.stack.sp = delivery->altstack.sp;
    frame->uc.stack.flags = delivery->altstack.flags;
    frame->uc.stack.size = delivery->altstack.size;
    frame->uc.sigmask = delivery->blocked_mask;
    frame->uc.mcontext.fault_address = delivery->fault_address;
    memcpy(frame->uc.mcontext.regs, cpu->x, sizeof(cpu->x));
    frame->uc.mcontext.sp = cpu->sp;
    frame->uc.mcontext.pc = cpu->pc;
    frame->uc.mcontext.pstate = cpu->nzcv & AARCH64_NZCV_MASK;

    struct aarch64_linux_fpsimd_context fpsimd = pack_fpsimd(cpu);
    memcpy(frame->uc.mcontext.reserved, &fpsimd, sizeof(fpsimd));
    // 整个 image 已清零，FPSIMD 后紧跟的 16 字节自然形成终止记录。
}

enum aarch64_linux_signal_frame_status aarch64_linux_build_rt_sigframe(
        const struct cpu_state *interrupted, struct guest_tlb *tlb,
        const struct aarch64_linux_signal_delivery *delivery,
        struct cpu_state *handler_cpu, guest_addr_t *frame_address,
        struct guest_memory_fault *fault) {
    assert(interrupted != NULL && tlb != NULL && delivery != NULL &&
            handler_cpu != NULL && frame_address != NULL && fault != NULL);
    reset_fault(fault, delivery->stack_top, GUEST_MEMORY_WRITE);

    guest_addr_t candidate_frame;
    guest_addr_t record_address;
    if (!signal_frame_addresses(delivery->stack_top,
            &candidate_frame, &record_address))
        return AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME;

    struct aarch64_linux_signal_stack_image image = {0};
    pack_interrupted_context(&image.frame, interrupted, delivery);
    if (delivery->action_flags & AARCH64_LINUX_SA_SIGINFO) {
        image.frame.info = delivery->info;
        image.frame.info.signo = delivery->signal;
    }
    image.record = (struct aarch64_linux_frame_record) {
        .fp = interrupted->x[29],
        .lr = interrupted->x[30],
    };

    // 写失败可能留下不可达的帧前缀，但绝不能暴露部分 CPU 状态。
    if (!guest_linux_copy_to_user(tlb, candidate_frame,
            &image, sizeof(image), fault))
        return AARCH64_LINUX_SIGNAL_FRAME_MEMORY_FAULT;

    struct cpu_state candidate_cpu = *interrupted;
    candidate_cpu.x[0] = (qword_t) (dword_t) delivery->signal;
    if (delivery->action_flags & AARCH64_LINUX_SA_SIGINFO) {
        candidate_cpu.x[1] = candidate_frame +
                __builtin_offsetof(struct aarch64_linux_rt_sigframe, info);
        candidate_cpu.x[2] = candidate_frame +
                __builtin_offsetof(struct aarch64_linux_rt_sigframe, uc);
    }
    candidate_cpu.sp = candidate_frame;
    candidate_cpu.x[29] = record_address;
    candidate_cpu.x[30] = delivery->restorer;
    candidate_cpu.pc = delivery->handler;
    aarch64_clear_exclusive(&candidate_cpu);

    *handler_cpu = candidate_cpu;
    *frame_address = candidate_frame;
    return AARCH64_LINUX_SIGNAL_FRAME_OK;
}

static bool decode_fpsimd(
        const byte_t reserved[4096],
        struct aarch64_linux_fpsimd_context *fpsimd) {
    size_t offset = 0;
    bool found_fpsimd = false;
    while (offset <= 4096 - sizeof(struct aarch64_linux_ctx)) {
        struct aarch64_linux_ctx head;
        memcpy(&head, reserved + offset, sizeof(head));
        if (head.magic == 0) {
            if (head.size != 0)
                return false;
            return found_fpsimd;
        }
        if (head.size < sizeof(head) || (head.size & 0xf) != 0 ||
                head.size > 4096 - offset)
            return false;
        if (head.magic != AARCH64_LINUX_FPSIMD_MAGIC || found_fpsimd ||
                head.size != sizeof(*fpsimd))
            return false;
        memcpy(fpsimd, reserved + offset, sizeof(*fpsimd));
        found_fpsimd = true;
        offset += head.size;
    }
    return false;
}

enum aarch64_linux_signal_frame_status aarch64_linux_decode_rt_sigreturn(
        const struct cpu_state *handler_cpu, struct guest_tlb *tlb,
        struct aarch64_linux_signal_resume *resume,
        struct guest_memory_fault *fault) {
    assert(handler_cpu != NULL && tlb != NULL && resume != NULL &&
            fault != NULL);
    reset_fault(fault, handler_cpu->sp, GUEST_MEMORY_READ);
    if ((handler_cpu->sp & UINT64_C(0xf)) != 0)
        return AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME;

    struct aarch64_linux_rt_sigframe frame;
    if (!guest_linux_copy_from_user(tlb, handler_cpu->sp,
            &frame, sizeof(frame), fault))
        return AARCH64_LINUX_SIGNAL_FRAME_MEMORY_FAULT;
    if (frame.uc.mcontext.pstate &
            ~((qword_t) AARCH64_NZCV_MASK))
        return AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME;

    struct aarch64_linux_fpsimd_context fpsimd;
    if (!decode_fpsimd(frame.uc.mcontext.reserved, &fpsimd))
        return AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME;

    struct cpu_state candidate_cpu = *handler_cpu;
    memcpy(candidate_cpu.x, frame.uc.mcontext.regs,
            sizeof(candidate_cpu.x));
    candidate_cpu.sp = frame.uc.mcontext.sp;
    candidate_cpu.pc = frame.uc.mcontext.pc;
    aarch64_set_nzcv(&candidate_cpu,
            (dword_t) frame.uc.mcontext.pstate);
    memcpy(candidate_cpu.v, fpsimd.vregs, sizeof(candidate_cpu.v));
    candidate_cpu.fpsr = fpsimd.fpsr;
    candidate_cpu.fpcr = fpsimd.fpcr;
    aarch64_clear_exclusive(&candidate_cpu);

    *resume = (struct aarch64_linux_signal_resume) {
        .cpu = candidate_cpu,
        .blocked_mask = frame.uc.sigmask,
        .altstack = frame.uc.stack,
    };
    return AARCH64_LINUX_SIGNAL_FRAME_OK;
}

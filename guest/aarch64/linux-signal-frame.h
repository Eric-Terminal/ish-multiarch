#ifndef GUEST_AARCH64_LINUX_SIGNAL_FRAME_H
#define GUEST_AARCH64_LINUX_SIGNAL_FRAME_H

#include "emu/cpu.h"
#include "guest/aarch64/linux-signal-abi.h"
#include "guest/memory/tlb.h"

enum aarch64_linux_signal_frame_status {
    AARCH64_LINUX_SIGNAL_FRAME_OK,
    AARCH64_LINUX_SIGNAL_FRAME_BAD_FRAME,
    AARCH64_LINUX_SIGNAL_FRAME_MEMORY_FAULT,
};

struct aarch64_linux_signal_delivery {
    sdword_t signal;
    struct aarch64_linux_siginfo info;
    qword_t handler;
    qword_t restorer;
    qword_t action_flags;
    qword_t blocked_mask;
    struct aarch64_linux_stack altstack;
    guest_addr_t stack_top;
    qword_t fault_address;
};

struct aarch64_linux_signal_resume {
    struct cpu_state cpu;
    qword_t blocked_mask;
    struct aarch64_linux_stack altstack;
};

// 两阶段接口只产生候选 CPU；调用方可与任务信号状态一起提交。
enum aarch64_linux_signal_frame_status aarch64_linux_build_rt_sigframe(
        const struct cpu_state *interrupted, struct guest_tlb *tlb,
        const struct aarch64_linux_signal_delivery *delivery,
        struct cpu_state *handler_cpu, guest_addr_t *frame_address,
        struct guest_memory_fault *fault);
// 当前只接受本模块生成的基础 FPSIMD 链；其他扩展记录按坏帧拒绝。
// 返回的 mask 与 altstack 仍是 wire 值，由任务层负责校验和归一化。
enum aarch64_linux_signal_frame_status aarch64_linux_decode_rt_sigreturn(
        const struct cpu_state *handler_cpu, struct guest_tlb *tlb,
        struct aarch64_linux_signal_resume *resume,
        struct guest_memory_fault *fault);

#endif

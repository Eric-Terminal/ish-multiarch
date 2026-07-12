#ifndef GUEST_AARCH64_CPU_H
#define GUEST_AARCH64_CPU_H

#include "misc.h"
#include "emu/mmu.h"

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

union aarch64_vector_reg {
    __uint128_t q;
    qword_t d[2];
    dword_t s[4];
    word_t h[8];
    byte_t b[16];
    float f32[4];
    double f64[2];
};

_Static_assert(sizeof(union aarch64_vector_reg) == 16,
        "AArch64 向量寄存器必须为 128 位");

struct aarch64_exclusive_monitor {
    guest_addr_t address;
    qword_t value_low;
    qword_t value_high;
    qword_t mapping_epoch;
    byte_t size;
    bool valid;
};

struct cpu_state {
    struct mmu *mmu;
    uint64_t cycle;

    qword_t x[31];
    qword_t sp;
    qword_t pc;
    dword_t nzcv;

    _Alignas(16) union aarch64_vector_reg v[32];
    dword_t fpcr;
    dword_t fpsr;
    qword_t tpidr_el0;

    struct aarch64_exclusive_monitor exclusive;

    guest_addr_t segfault_addr;
    bool segfault_was_write;
    dword_t trapno;
    bool single_step;

    bool *poked_ptr;
    bool _poked;
};

#define CPU_OFFSET(field) offsetof(struct cpu_state, field)

_Static_assert(CPU_OFFSET(x[30]) == CPU_OFFSET(x[0]) + 30 * sizeof(qword_t),
        "AArch64 通用寄存器布局不连续");
_Static_assert(CPU_OFFSET(v) % 16 == 0,
        "AArch64 向量寄存器组必须按 16 字节对齐");

#define AARCH64_NZCV_MASK UINT32_C(0xf0000000)
#define AARCH64_FPCR_RMODE_SHIFT 22
#define AARCH64_FPCR_RMODE_MASK UINT32_C(0x00c00000)
#define AARCH64_FPSR_IXC UINT32_C(0x00000010)

static inline addr_t cpu_get_pc(const struct cpu_state *cpu) {
    return cpu->pc;
}

static inline void cpu_set_pc(struct cpu_state *cpu, addr_t pc) {
    cpu->pc = pc;
}

static inline bool cpu_is_single_step(const struct cpu_state *cpu) {
    return cpu->single_step;
}

static inline void cpu_set_trap(struct cpu_state *cpu, dword_t trap) {
    cpu->trapno = trap;
}

static inline dword_t aarch64_get_nzcv(const struct cpu_state *cpu) {
    return cpu->nzcv;
}

static inline void aarch64_set_nzcv(struct cpu_state *cpu, dword_t nzcv) {
    cpu->nzcv = nzcv & AARCH64_NZCV_MASK;
}

static inline void aarch64_set_exclusive(struct cpu_state *cpu, guest_addr_t address,
        byte_t size, qword_t value_low, qword_t value_high, qword_t mapping_epoch) {
    cpu->exclusive.address = address;
    cpu->exclusive.size = size;
    cpu->exclusive.value_low = value_low;
    cpu->exclusive.value_high = value_high;
    cpu->exclusive.mapping_epoch = mapping_epoch;
    cpu->exclusive.valid = true;
}

static inline bool aarch64_exclusive_matches(const struct cpu_state *cpu,
        guest_addr_t address, byte_t size, qword_t mapping_epoch) {
    return cpu->exclusive.valid && cpu->exclusive.address == address &&
            cpu->exclusive.size == size && cpu->exclusive.mapping_epoch == mapping_epoch;
}

static inline void aarch64_clear_exclusive(struct cpu_state *cpu) {
    cpu->exclusive.valid = false;
}

#endif

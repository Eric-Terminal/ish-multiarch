#include <assert.h>

#include "guest/aarch64/condition.h"
#include "guest/aarch64/threaded.h"

static qword_t register_mask(byte_t width) {
    assert(width == 32 || width == 64);
    return width == 32 ? UINT32_MAX : UINT64_MAX;
}

static qword_t read_general_register(const struct cpu_state *cpu,
        byte_t reg, byte_t width, bool allow_sp) {
    assert(reg <= 31);
    qword_t value = reg == 31 ? (allow_sp ? cpu->sp : 0) : cpu->x[reg];
    return value & register_mask(width);
}

static void write_general_register(struct cpu_state *cpu,
        byte_t reg, byte_t width, bool allow_sp, qword_t value) {
    assert(reg <= 31);
    value &= register_mask(width);
    if (reg == 31) {
        if (allow_sp)
            cpu->sp = value;
        return;
    }
    cpu->x[reg] = value;
}

static dword_t addition_flags(qword_t left, qword_t right,
        qword_t value, byte_t width) {
    qword_t mask = register_mask(width);
    qword_t sign = UINT64_C(1) << (width - 1);
    left &= mask;
    right &= mask;
    value &= mask;
    bool carry = value < left;
    bool overflow = (~(left ^ right) & (left ^ value) & sign) != 0;
    return (value & sign ? UINT32_C(1) << 31 : 0) |
            (value == 0 ? UINT32_C(1) << 30 : 0) |
            (carry ? UINT32_C(1) << 29 : 0) |
            (overflow ? UINT32_C(1) << 28 : 0);
}

static dword_t subtraction_flags(qword_t left, qword_t right,
        qword_t value, byte_t width) {
    qword_t mask = register_mask(width);
    qword_t sign = UINT64_C(1) << (width - 1);
    left &= mask;
    right &= mask;
    value &= mask;
    bool carry = left >= right;
    bool overflow = ((left ^ right) & (left ^ value) & sign) != 0;
    return (value & sign ? UINT32_C(1) << 31 : 0) |
            (value == 0 ? UINT32_C(1) << 30 : 0) |
            (carry ? UINT32_C(1) << 29 : 0) |
            (overflow ? UINT32_C(1) << 28 : 0);
}

// 快速 handler 只执行一条 guest 指令，cycle 由 runner 统一提交。
static void execute_nop_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) instruction;
    (void) result;
    cpu->pc += 4;
}

static void execute_move_wide_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    byte_t rd = instruction->operands.move_wide.rd;
    byte_t width = instruction->width;
    byte_t shift = instruction->operands.move_wide.shift;
    qword_t field = (qword_t)
            instruction->operands.move_wide.immediate << shift;
    qword_t value = field;

    if (instruction->opcode == AARCH64_OP_MOVN) {
        value = ~field;
    } else if (instruction->opcode == AARCH64_OP_MOVK) {
        qword_t preserved = read_general_register(cpu, rd, width, false) &
                ~(UINT64_C(0xffff) << shift);
        value = preserved | field;
    }
    write_general_register(cpu, rd, width, false, value);
    cpu->pc += 4;
}

static void execute_add_sub_immediate_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    byte_t rd = instruction->operands.add_sub_immediate.rd;
    byte_t rn = instruction->operands.add_sub_immediate.rn;
    byte_t width = instruction->width;
    qword_t immediate = instruction->operands.add_sub_immediate.immediate;
    qword_t left = read_general_register(cpu, rn, width, true);
    bool subtract = instruction->opcode == AARCH64_OP_SUB_IMMEDIATE ||
            instruction->opcode == AARCH64_OP_SUBS_IMMEDIATE;
    bool set_flags = instruction->opcode == AARCH64_OP_ADDS_IMMEDIATE ||
            instruction->opcode == AARCH64_OP_SUBS_IMMEDIATE;
    qword_t value = subtract ? left - immediate : left + immediate;
    value &= register_mask(width);

    if (set_flags) {
        aarch64_set_nzcv(cpu, subtract ?
                subtraction_flags(left, immediate, value, width) :
                addition_flags(left, immediate, value, width));
    }
    write_general_register(cpu, rd, width, !set_flags, value);
    cpu->pc += 4;
}

static void execute_branch_immediate_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    if (instruction->opcode == AARCH64_OP_BL)
        cpu->x[30] = cpu->pc + 4;
    cpu->pc += (qword_t)
            instruction->operands.branch_immediate.displacement;
}

static void execute_branch_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    // BLR x30 必须在写入返回地址前捕获跳转目标。
    qword_t target = cpu->x[instruction->operands.branch_register.rn];
    if (instruction->opcode == AARCH64_OP_BLR)
        cpu->x[30] = cpu->pc + 4;
    cpu->pc = target;
}

static void execute_conditional_branch_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    bool branch = aarch64_condition_holds(cpu->nzcv,
            instruction->operands.conditional_branch.condition);
    cpu->pc += branch ? (qword_t)
            instruction->operands.conditional_branch.displacement : 4;
}

static void execute_compare_branch_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    qword_t value = read_general_register(cpu,
            instruction->operands.compare_branch.rt,
            instruction->width, false);
    bool zero = value == 0;
    bool branch = instruction->opcode == AARCH64_OP_CBZ ? zero : !zero;
    cpu->pc += branch ?
            (qword_t) instruction->operands.compare_branch.displacement : 4;
}

static void execute_test_branch_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    qword_t value = read_general_register(cpu,
            instruction->operands.test_branch.rt, 64, false);
    bool set = ((value >> instruction->operands.test_branch.bit) & 1) != 0;
    bool branch = instruction->opcode == AARCH64_OP_TBZ ? !set : set;
    cpu->pc += branch ?
            (qword_t) instruction->operands.test_branch.displacement : 4;
}

static void execute_svc_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) instruction;
    aarch64_clear_exclusive(cpu);
    cpu->pc += 4;
    result->stop = AARCH64_EXECUTE_SYSCALL;
}

static void execute_c_fallback(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    *result = aarch64_execute(cpu, tlb, instruction);
}

static aarch64_threaded_handler select_handler(
        enum aarch64_opcode opcode, bool *c_fallback) {
    assert(c_fallback != NULL);
    *c_fallback = false;
    switch (opcode) {
        case AARCH64_OP_NOP:
            return execute_nop_fast;
        case AARCH64_OP_MOVN:
        case AARCH64_OP_MOVZ:
        case AARCH64_OP_MOVK:
            return execute_move_wide_fast;
        case AARCH64_OP_ADD_IMMEDIATE:
        case AARCH64_OP_ADDS_IMMEDIATE:
        case AARCH64_OP_SUB_IMMEDIATE:
        case AARCH64_OP_SUBS_IMMEDIATE:
            return execute_add_sub_immediate_fast;
        case AARCH64_OP_B:
        case AARCH64_OP_BL:
            return execute_branch_immediate_fast;
        case AARCH64_OP_BR:
        case AARCH64_OP_BLR:
        case AARCH64_OP_RET:
            return execute_branch_register_fast;
        case AARCH64_OP_B_CONDITIONAL:
            return execute_conditional_branch_fast;
        case AARCH64_OP_CBZ:
        case AARCH64_OP_CBNZ:
            return execute_compare_branch_fast;
        case AARCH64_OP_TBZ:
        case AARCH64_OP_TBNZ:
            return execute_test_branch_fast;
        case AARCH64_OP_SVC:
            return execute_svc_fast;
        default:
            // 未提速的指令继续经过 C oracle，保持两套执行语义独立。
            *c_fallback = true;
            return execute_c_fallback;
    }
}

static unsigned cache_index(guest_addr_t pc) {
    return (unsigned) ((pc >> 2) &
            (AARCH64_THREADED_CACHE_SIZE - 1));
}

bool aarch64_threaded_execute(struct aarch64_threaded_cache *cache,
        struct cpu_state *cpu, struct guest_tlb *tlb,
        guest_addr_t pc, dword_t word,
        struct aarch64_execute_result *result) {
    assert(cache != NULL);
    assert(cpu != NULL);
    assert(tlb != NULL);
    assert(result != NULL);
    assert((pc & 3) == 0);

    struct aarch64_threaded_cache_entry *entry =
            &cache->entries[cache_index(pc)];
    if (entry->valid && entry->pc == pc && entry->word == word) {
        cache->stats.cache_hits++;
    } else {
        cache->stats.cache_misses++;
        entry->valid = false;
        entry->pc = pc;
        entry->word = word;
        entry->handler = NULL;
        entry->c_fallback = false;

        struct aarch64_decoded decoded;
        if (aarch64_decode(word, &decoded)) {
            entry->decoded = decoded;
            entry->handler = select_handler(decoded.opcode,
                    &entry->c_fallback);
        }
        // valid 最后置位，保证命中的 token 已经完整初始化。
        entry->valid = true;
    }

    if (entry->handler == NULL)
        return false;
    *result = (struct aarch64_execute_result) {
        .stop = AARCH64_EXECUTE_RETIRED,
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };
    if (entry->c_fallback) {
        cache->stats.c_fallbacks++;
    } else {
        cache->stats.fast_dispatches++;
    }
    entry->handler(cpu, tlb, &entry->decoded, result);
    return true;
}

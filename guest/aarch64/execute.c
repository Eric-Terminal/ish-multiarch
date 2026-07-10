#include "guest/aarch64/execute.h"

static qword_t width_mask(byte_t width) {
    return width == 32 ? UINT32_MAX : UINT64_MAX;
}

static qword_t read_register(const struct cpu_state *cpu, byte_t reg,
        byte_t width, bool allow_sp) {
    qword_t value;
    if (reg == 31)
        value = allow_sp ? cpu->sp : 0;
    else
        value = cpu->x[reg];
    return value & width_mask(width);
}

static void write_register(struct cpu_state *cpu, byte_t reg,
        byte_t width, bool allow_sp, qword_t value) {
    value &= width_mask(width);
    if (reg == 31) {
        if (allow_sp)
            cpu->sp = value;
        return;
    }
    cpu->x[reg] = value;
}

static void set_add_flags(struct cpu_state *cpu, qword_t left,
        qword_t right, qword_t result, byte_t width) {
    qword_t sign = UINT64_C(1) << (width - 1);
    qword_t mask = width_mask(width);
    bool carry;
    if (width == 32)
        carry = (left + right) > UINT32_MAX;
    else
        carry = ((__uint128_t) left + right) > UINT64_MAX;
    bool overflow = (~(left ^ right) & (left ^ result) & sign) != 0;
    dword_t nzcv = (result & sign ? UINT32_C(1) << 31 : 0) |
            ((result & mask) == 0 ? UINT32_C(1) << 30 : 0) |
            (carry ? UINT32_C(1) << 29 : 0) |
            (overflow ? UINT32_C(1) << 28 : 0);
    aarch64_set_nzcv(cpu, nzcv);
}

static void set_sub_flags(struct cpu_state *cpu, qword_t left,
        qword_t right, qword_t result, byte_t width) {
    qword_t sign = UINT64_C(1) << (width - 1);
    qword_t mask = width_mask(width);
    bool carry = (left & mask) >= (right & mask);
    bool overflow = ((left ^ right) & (left ^ result) & sign) != 0;
    dword_t nzcv = (result & sign ? UINT32_C(1) << 31 : 0) |
            ((result & mask) == 0 ? UINT32_C(1) << 30 : 0) |
            (carry ? UINT32_C(1) << 29 : 0) |
            (overflow ? UINT32_C(1) << 28 : 0);
    aarch64_set_nzcv(cpu, nzcv);
}

static void execute_add_sub_immediate(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.add_sub_immediate.rd;
    byte_t rn = instruction->operands.add_sub_immediate.rn;
    qword_t immediate = instruction->operands.add_sub_immediate.immediate;
    byte_t width = instruction->width;
    qword_t left = read_register(cpu, rn, width, true);
    qword_t result;
    bool subtract = instruction->opcode == AARCH64_OP_SUB_IMMEDIATE ||
            instruction->opcode == AARCH64_OP_SUBS_IMMEDIATE;
    bool set_flags = instruction->opcode == AARCH64_OP_ADDS_IMMEDIATE ||
            instruction->opcode == AARCH64_OP_SUBS_IMMEDIATE;

    if (subtract)
        result = (left - immediate) & width_mask(width);
    else
        result = (left + immediate) & width_mask(width);

    if (set_flags) {
        if (subtract)
            set_sub_flags(cpu, left, immediate, result, width);
        else
            set_add_flags(cpu, left, immediate, result, width);
    }
    write_register(cpu, rd, width, !set_flags, result);
    cpu->pc += 4;
}

static void execute_move_wide(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.move_wide.rd;
    byte_t shift = instruction->operands.move_wide.shift;
    qword_t immediate = instruction->operands.move_wide.immediate;
    qword_t value = immediate << shift;
    qword_t mask = width_mask(instruction->width);

    if (instruction->opcode == AARCH64_OP_MOVN)
        value = ~value;
    else if (instruction->opcode == AARCH64_OP_MOVK) {
        qword_t keep_mask = ~(UINT64_C(0xffff) << shift);
        value = (read_register(cpu, rd, instruction->width, false) & keep_mask) | value;
    }

    write_register(cpu, rd, instruction->width, false, value & mask);
    cpu->pc += 4;
}

void aarch64_execute(struct cpu_state *cpu, const struct aarch64_decoded *instruction) {
    switch (instruction->opcode) {
        case AARCH64_OP_NOP:
            cpu->pc += 4;
            break;
        case AARCH64_OP_ADD_IMMEDIATE:
        case AARCH64_OP_ADDS_IMMEDIATE:
        case AARCH64_OP_SUB_IMMEDIATE:
        case AARCH64_OP_SUBS_IMMEDIATE:
            execute_add_sub_immediate(cpu, instruction);
            break;
        case AARCH64_OP_MOVN:
        case AARCH64_OP_MOVZ:
        case AARCH64_OP_MOVK:
            execute_move_wide(cpu, instruction);
            break;
        case AARCH64_OP_B:
            cpu->pc += instruction->operands.branch_immediate.displacement;
            break;
        case AARCH64_OP_BL:
            cpu->x[30] = cpu->pc + 4;
            cpu->pc += instruction->operands.branch_immediate.displacement;
            break;
        case AARCH64_OP_BR:
        case AARCH64_OP_BLR:
        case AARCH64_OP_RET: {
            qword_t target = cpu->x[instruction->operands.branch_register.rn];
            if (instruction->opcode == AARCH64_OP_BLR)
                cpu->x[30] = cpu->pc + 4;
            cpu->pc = target;
            break;
        }
    }
}

#include <string.h>

#include "guest/aarch64/execute.h"
#include "guest/aarch64/condition.h"
#include "guest/aarch64/scalar-fp.h"

_Static_assert(GUEST_TLB_MAX_ACCESS_SIZE >=
        2 * sizeof(union aarch64_vector_reg),
        "AArch64 SIMD 寄存器对访存要求一次覆盖 32 字节");

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

static qword_t shift_register(qword_t value, byte_t width,
        enum aarch64_shift_type type, byte_t amount) {
    qword_t mask = width_mask(width);
    value &= mask;
    if (amount == 0)
        return value;
    if (type == AARCH64_SHIFT_LSL)
        return (value << amount) & mask;
    if (type == AARCH64_SHIFT_LSR)
        return value >> amount;
    if (type == AARCH64_SHIFT_ASR) {
        qword_t shifted = value >> amount;
        if (value & (UINT64_C(1) << (width - 1)))
            shifted |= mask << (width - amount);
        return shifted & mask;
    }
    return ((value >> amount) | (value << (width - amount))) & mask;
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

static void execute_add_sub_shifted(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.add_sub_shifted.rd;
    byte_t rn = instruction->operands.add_sub_shifted.rn;
    byte_t rm = instruction->operands.add_sub_shifted.rm;
    byte_t width = instruction->width;
    qword_t left = read_register(cpu, rn, width, false);
    qword_t right = shift_register(read_register(cpu, rm, width, false),
            width, instruction->operands.add_sub_shifted.shift_type,
            instruction->operands.add_sub_shifted.shift);
    bool subtract = instruction->opcode == AARCH64_OP_SUB_SHIFTED_REGISTER ||
            instruction->opcode == AARCH64_OP_SUBS_SHIFTED_REGISTER;
    bool set_flags = instruction->opcode == AARCH64_OP_ADDS_SHIFTED_REGISTER ||
            instruction->opcode == AARCH64_OP_SUBS_SHIFTED_REGISTER;
    qword_t result = subtract ? left - right : left + right;
    result &= width_mask(width);

    if (set_flags) {
        if (subtract)
            set_sub_flags(cpu, left, right, result, width);
        else
            set_add_flags(cpu, left, right, result, width);
    }
    write_register(cpu, rd, width, false, result);
    cpu->pc += 4;
}

static qword_t extend_register(qword_t value, byte_t width,
        enum aarch64_extend_type extend_type, byte_t shift) {
    unsigned source_width = 8U << (extend_type & 3);
    if (source_width > width)
        source_width = width;
    qword_t source_mask = source_width == 64 ? UINT64_MAX :
            (UINT64_C(1) << source_width) - 1;
    value &= source_mask;
    if (extend_type >= AARCH64_EXTEND_SXTB &&
            (value & (UINT64_C(1) << (source_width - 1))))
        value |= width_mask(width) & ~source_mask;
    return (value << shift) & width_mask(width);
}

static void execute_add_sub_extended(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    byte_t rd = instruction->operands.add_sub_extended.rd;
    qword_t left = read_register(cpu,
            instruction->operands.add_sub_extended.rn, width, true);
    qword_t right = read_register(cpu,
            instruction->operands.add_sub_extended.rm, width, false);
    right = extend_register(right, width,
            instruction->operands.add_sub_extended.extend_type,
            instruction->operands.add_sub_extended.shift);
    bool subtract = instruction->opcode == AARCH64_OP_SUB_EXTENDED_REGISTER ||
            instruction->opcode == AARCH64_OP_SUBS_EXTENDED_REGISTER;
    bool set_flags = instruction->opcode == AARCH64_OP_ADDS_EXTENDED_REGISTER ||
            instruction->opcode == AARCH64_OP_SUBS_EXTENDED_REGISTER;
    qword_t result = subtract ? left - right : left + right;
    result &= width_mask(width);

    if (set_flags) {
        if (subtract)
            set_sub_flags(cpu, left, right, result, width);
        else
            set_add_flags(cpu, left, right, result, width);
    }
    write_register(cpu, rd, width, !set_flags, result);
    cpu->pc += 4;
}

static void execute_add_sub_carry(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    qword_t mask = width_mask(width);
    byte_t rd = instruction->operands.data_processing_2source.rd;
    qword_t left = read_register(cpu,
            instruction->operands.data_processing_2source.rn, width, false);
    qword_t right = read_register(cpu,
            instruction->operands.data_processing_2source.rm, width, false);
    bool subtract = instruction->opcode == AARCH64_OP_SBC ||
            instruction->opcode == AARCH64_OP_SBCS;
    bool set_flags = instruction->opcode == AARCH64_OP_ADCS ||
            instruction->opcode == AARCH64_OP_SBCS;
    // SBC 复用补码加法，旧 C 为一表示本次减法不引入借位。
    if (subtract)
        right = ~right & mask;

    qword_t carry_in = (cpu->nzcv >> 29) & 1;
    __uint128_t sum = (__uint128_t) left + right + carry_in;
    qword_t result = (qword_t) sum & mask;
    if (set_flags) {
        qword_t sign = UINT64_C(1) << (width - 1);
        bool carry_out = (sum >> width) != 0;
        bool overflow = (~(left ^ right) & (left ^ result) & sign) != 0;
        aarch64_set_nzcv(cpu,
                (result & sign ? UINT32_C(1) << 31 : 0) |
                (result == 0 ? UINT32_C(1) << 30 : 0) |
                (carry_out ? UINT32_C(1) << 29 : 0) |
                (overflow ? UINT32_C(1) << 28 : 0));
    }
    write_register(cpu, rd, width, false, result);
    cpu->pc += 4;
}

static void execute_logical_shifted(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.logical_shifted.rd;
    byte_t rn = instruction->operands.logical_shifted.rn;
    byte_t rm = instruction->operands.logical_shifted.rm;
    byte_t width = instruction->width;
    qword_t mask = width_mask(width);
    qword_t left = read_register(cpu, rn, width, false);
    qword_t right = shift_register(read_register(cpu, rm, width, false),
            width, instruction->operands.logical_shifted.shift_type,
            instruction->operands.logical_shifted.shift);
    if (instruction->operands.logical_shifted.invert)
        right = ~right & mask;

    qword_t result;
    if (instruction->opcode == AARCH64_OP_AND_SHIFTED_REGISTER ||
            instruction->opcode == AARCH64_OP_ANDS_SHIFTED_REGISTER)
        result = left & right;
    else if (instruction->opcode == AARCH64_OP_ORR_SHIFTED_REGISTER)
        result = left | right;
    else
        result = left ^ right;
    result &= mask;

    if (instruction->opcode == AARCH64_OP_ANDS_SHIFTED_REGISTER) {
        qword_t sign = UINT64_C(1) << (width - 1);
        aarch64_set_nzcv(cpu,
                (result & sign ? UINT32_C(1) << 31 : 0) |
                (result == 0 ? UINT32_C(1) << 30 : 0));
    }
    write_register(cpu, rd, width, false, result);
    cpu->pc += 4;
}

static void execute_logical_immediate(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.logical_immediate.rd;
    byte_t rn = instruction->operands.logical_immediate.rn;
    byte_t width = instruction->width;
    qword_t left = read_register(cpu, rn, width, false);
    qword_t right = instruction->operands.logical_immediate.immediate;
    bool set_flags = instruction->opcode == AARCH64_OP_ANDS_IMMEDIATE;

    qword_t result;
    if (instruction->opcode == AARCH64_OP_AND_IMMEDIATE || set_flags)
        result = left & right;
    else if (instruction->opcode == AARCH64_OP_ORR_IMMEDIATE)
        result = left | right;
    else
        result = left ^ right;

    if (set_flags) {
        qword_t sign = UINT64_C(1) << (width - 1);
        aarch64_set_nzcv(cpu,
                (result & sign ? UINT32_C(1) << 31 : 0) |
                (result == 0 ? UINT32_C(1) << 30 : 0));
    }
    write_register(cpu, rd, width, !set_flags, result);
    cpu->pc += 4;
}

static void execute_bitfield(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    byte_t rd = instruction->operands.bitfield_move.rd;
    qword_t source = read_register(cpu, instruction->operands.bitfield_move.rn,
            width, false);
    qword_t rotated = shift_register(source, width, AARCH64_SHIFT_ROR,
            instruction->operands.bitfield_move.immr);
    qword_t write_mask = instruction->operands.bitfield_move.write_mask;
    qword_t top_mask = instruction->operands.bitfield_move.top_mask;
    qword_t bottom = rotated & write_mask;
    qword_t result;

    if (instruction->opcode == AARCH64_OP_SBFM) {
        bool sign = (source >> instruction->operands.bitfield_move.imms) & 1;
        qword_t top = sign ? width_mask(width) : 0;
        result = (top & ~top_mask) | (bottom & top_mask);
    } else if (instruction->opcode == AARCH64_OP_BFM) {
        qword_t destination = read_register(cpu, rd, width, false);
        bottom |= destination & ~write_mask;
        result = (destination & ~top_mask) | (bottom & top_mask);
    } else {
        result = bottom & top_mask;
    }

    write_register(cpu, rd, width, false, result);
    cpu->pc += 4;
}

static void execute_extract(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    qword_t high = read_register(cpu, instruction->operands.extract.rn,
            width, false);
    qword_t low = read_register(cpu, instruction->operands.extract.rm,
            width, false);
    byte_t lsb = instruction->operands.extract.lsb;
    qword_t result = low;
    if (lsb != 0)
        result = (low >> lsb) | (high << (width - lsb));
    write_register(cpu, instruction->operands.extract.rd,
            width, false, result);
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

static void execute_pc_relative(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    qword_t displacement =
            (qword_t) instruction->operands.pc_relative.displacement;
    qword_t base = instruction->opcode == AARCH64_OP_ADRP ?
            cpu->pc & ~UINT64_C(0xfff) : cpu->pc;
    qword_t value = base + displacement;
    write_register(cpu, instruction->operands.pc_relative.rd,
            64, false, value);
    cpu->pc += 4;
}

static void execute_conditional_select(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    qword_t result;
    if (aarch64_condition_holds(cpu->nzcv,
            instruction->operands.conditional_select.condition)) {
        result = read_register(cpu,
                instruction->operands.conditional_select.rn, width, false);
    } else {
        result = read_register(cpu,
                instruction->operands.conditional_select.rm, width, false);
        if (instruction->opcode == AARCH64_OP_CSINC)
            result++;
        else if (instruction->opcode == AARCH64_OP_CSINV)
            result = ~result;
        else if (instruction->opcode == AARCH64_OP_CSNEG)
            result = UINT64_C(0) - result;
        result &= width_mask(width);
    }
    write_register(cpu, instruction->operands.conditional_select.rd,
            width, false, result);
    cpu->pc += 4;
}

static void execute_conditional_compare(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    const byte_t width = instruction->width;
    const byte_t condition =
            instruction->operands.conditional_compare.condition;
    if (aarch64_condition_holds(cpu->nzcv, condition)) {
        qword_t left = read_register(cpu,
                instruction->operands.conditional_compare.rn,
                width, false);
        qword_t right = instruction->operands.conditional_compare.immediate ?
                instruction->operands.conditional_compare.operand :
                read_register(cpu,
                        instruction->operands.conditional_compare.operand,
                        width, false);
        qword_t result;
        if (instruction->opcode == AARCH64_OP_CCMP) {
            result = (left - right) & width_mask(width);
            set_sub_flags(cpu, left, right, result, width);
        } else {
            result = (left + right) & width_mask(width);
            set_add_flags(cpu, left, right, result, width);
        }
    } else {
        aarch64_set_nzcv(cpu,
                (dword_t) instruction->operands.conditional_compare.nzcv << 28);
    }
    cpu->pc += 4;
}

static qword_t signed_divide(qword_t dividend, qword_t divisor,
        byte_t width) {
    qword_t mask = width_mask(width);
    qword_t sign = UINT64_C(1) << (width - 1);
    dividend &= mask;
    divisor &= mask;
    bool dividend_negative = (dividend & sign) != 0;
    bool divisor_negative = (divisor & sign) != 0;

    // 无符号幅值运算避免宿主执行 INT_MIN / -1 时触发未定义行为。
    qword_t dividend_magnitude = dividend_negative ?
            (UINT64_C(0) - dividend) & mask : dividend;
    qword_t divisor_magnitude = divisor_negative ?
            (UINT64_C(0) - divisor) & mask : divisor;
    if (divisor_magnitude == 0)
        return 0;
    qword_t quotient = dividend_magnitude / divisor_magnitude;
    if (dividend_negative != divisor_negative)
        quotient = (UINT64_C(0) - quotient) & mask;
    return quotient;
}

static qword_t reverse_bits(qword_t value, byte_t width) {
    qword_t result = 0;
    for (byte_t bit = 0; bit < width; bit++)
        result |= ((value >> bit) & 1) << (width - 1 - bit);
    return result;
}

static qword_t reverse_bytes_in_elements(qword_t value, byte_t width,
        byte_t element_width) {
    qword_t result = 0;
    byte_t bytes_per_element = (byte_t) (element_width / 8);
    byte_t byte_count = (byte_t) (width / 8);
    for (byte_t byte = 0; byte < byte_count; byte++) {
        byte_t element_start = (byte_t)
                (byte / bytes_per_element * bytes_per_element);
        byte_t destination = (byte_t) (element_start +
                bytes_per_element - 1 - byte % bytes_per_element);
        result |= ((value >> (byte * 8)) & UINT64_C(0xff)) <<
                (destination * 8);
    }
    return result;
}

static byte_t count_leading_zeros(qword_t value, byte_t width) {
    byte_t count = 0;
    while (count < width &&
            ((value >> (width - 1 - count)) & 1) == 0)
        count++;
    return count;
}

static void execute_data_processing_1source(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    qword_t value = read_register(cpu,
            instruction->operands.data_processing_1source.rn,
            width, false);
    qword_t result;

    if (instruction->opcode == AARCH64_OP_RBIT) {
        result = reverse_bits(value, width);
    } else if (instruction->opcode == AARCH64_OP_REV16) {
        result = reverse_bytes_in_elements(value, width, 16);
    } else if (instruction->opcode == AARCH64_OP_REV32) {
        result = reverse_bytes_in_elements(value, width, 32);
    } else if (instruction->opcode == AARCH64_OP_REV64) {
        result = reverse_bytes_in_elements(value, width, 64);
    } else if (instruction->opcode == AARCH64_OP_CLZ) {
        result = count_leading_zeros(value, width);
    } else {
        qword_t sign = UINT64_C(1) << (width - 1);
        qword_t normalized = value & sign ?
                (~value & width_mask(width)) : value;
        // 归一化后最高位恒为零，减一排除原始符号位。
        result = count_leading_zeros(normalized, width) - 1;
    }

    write_register(cpu, instruction->operands.data_processing_1source.rd,
            width, false, result);
    cpu->pc += 4;
}

static void execute_advsimd_immediate(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_immediate.rd;
    qword_t immediate = instruction->operands.advsimd_immediate.immediate;
    qword_t low;
    qword_t high;

    if (instruction->opcode == AARCH64_OP_ADVSIMD_MOVI) {
        low = immediate;
        high = immediate;
    } else if (instruction->opcode == AARCH64_OP_ADVSIMD_MVNI) {
        low = ~immediate;
        high = ~immediate;
    } else if (instruction->opcode == AARCH64_OP_ADVSIMD_ORR_IMMEDIATE) {
        low = cpu->v[rd].d[0] | immediate;
        high = cpu->v[rd].d[1] | immediate;
    } else {
        low = cpu->v[rd].d[0] & ~immediate;
        high = cpu->v[rd].d[1] & ~immediate;
    }

    cpu->v[rd].d[0] = low;
    cpu->v[rd].d[1] = instruction->width == 128 ? high : 0;
    cpu->pc += 4;
}

static void execute_advsimd_scalar_shift(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_shift_immediate.rd;
    byte_t rn = instruction->operands.advsimd_shift_immediate.rn;
    byte_t shift = instruction->operands.advsimd_shift_immediate.shift;
    qword_t source = cpu->v[rn].d[0];
    cpu->v[rd].d[0] = source << shift;
    cpu->v[rd].d[1] = 0;
    cpu->pc += 4;
}

static qword_t vector_element_mask(byte_t element_size) {
    return element_size == 8 ? UINT64_MAX :
            (UINT64_C(1) << (element_size * 8)) - 1;
}

static qword_t read_vector_element(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index) {
    unsigned bit = (unsigned) index * element_size * 8;
    qword_t value = reg->d[bit / 64] >> (bit % 64);
    return value & vector_element_mask(element_size);
}

static void write_vector_element(union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index, qword_t value) {
    unsigned bit = (unsigned) index * element_size * 8;
    unsigned half = bit / 64;
    unsigned shift = bit % 64;
    qword_t mask = vector_element_mask(element_size);
    reg->d[half] = (reg->d[half] & ~(mask << shift)) |
            ((value & mask) << shift);
}

static void execute_advsimd_shift_long(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_shift_long.rd;
    byte_t rn = instruction->operands.advsimd_shift_long.rn;
    byte_t source_size =
            instruction->operands.advsimd_shift_long.element_size;
    byte_t shift = instruction->operands.advsimd_shift_long.shift;
    byte_t lanes = 8 / source_size;
    byte_t source_offset =
            instruction->opcode == AARCH64_OP_ADVSIMD_SSHLL2 ? lanes : 0;
    qword_t source_sign = UINT64_C(1) << (source_size * 8 - 1);
    qword_t source_mask = vector_element_mask(source_size);
    union aarch64_vector_reg source = cpu->v[rn];
    union aarch64_vector_reg result = {0};

    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t value = read_vector_element(&source, source_size,
                (byte_t) (source_offset + lane));
        if (value & source_sign)
            value |= ~source_mask;
        write_vector_element(&result, source_size * 2,
                lane, value << shift);
    }
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static void execute_advsimd_add(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_three_same.rd;
    byte_t rn = instruction->operands.advsimd_three_same.rn;
    byte_t rm = instruction->operands.advsimd_three_same.rm;
    byte_t element_size =
            instruction->operands.advsimd_three_same.element_size;
    byte_t lanes = (byte_t) (instruction->width / (element_size * 8));
    // Q=0 写回也必须清零目标寄存器的高 64 位。
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t left = read_vector_element(&cpu->v[rn], element_size, lane);
        qword_t right = read_vector_element(&cpu->v[rm], element_size, lane);
        write_vector_element(&result, element_size, lane, left + right);
    }
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static void execute_advsimd_table(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_table.rd;
    byte_t rn = instruction->operands.advsimd_table.rn;
    byte_t rm = instruction->operands.advsimd_table.rm;
    byte_t table_count = instruction->operands.advsimd_table.table_count;
    byte_t table[4 * sizeof(union aarch64_vector_reg)];
    for (byte_t reg = 0; reg < table_count; reg++) {
        byte_t source = (byte_t) ((rn + reg) & 0x1f);
        memcpy(&table[reg * sizeof(union aarch64_vector_reg)],
                cpu->v[source].b, sizeof(union aarch64_vector_reg));
    }

    union aarch64_vector_reg result = {0};
    byte_t lanes = instruction->width / 8;
    byte_t table_size =
            (byte_t) (table_count * sizeof(union aarch64_vector_reg));
    for (byte_t lane = 0; lane < lanes; lane++) {
        byte_t index = cpu->v[rm].b[lane];
        if (index < table_size)
            result.b[lane] = table[index];
        else if (instruction->opcode == AARCH64_OP_ADVSIMD_TBX)
            result.b[lane] = cpu->v[rd].b[lane];
    }
    // 即使 TBX 保留越界 lane，Q=0 也必须清零目标高 64 位。
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static void execute_advsimd_permute(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_three_same.rd;
    byte_t rn = instruction->operands.advsimd_three_same.rn;
    byte_t rm = instruction->operands.advsimd_three_same.rm;
    byte_t element_size =
            instruction->operands.advsimd_three_same.element_size;
    byte_t lanes = (byte_t) (instruction->width / (element_size * 8));
    byte_t half = lanes / 2;
    bool second = instruction->opcode == AARCH64_OP_ADVSIMD_UZP2 ||
            instruction->opcode == AARCH64_OP_ADVSIMD_TRN2 ||
            instruction->opcode == AARCH64_OP_ADVSIMD_ZIP2;
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        byte_t source;
        byte_t index;
        if (instruction->opcode == AARCH64_OP_ADVSIMD_UZP1 ||
                instruction->opcode == AARCH64_OP_ADVSIMD_UZP2) {
            source = lane < half ? rn : rm;
            index = (byte_t) (2 * (lane % half) + second);
        } else if (instruction->opcode == AARCH64_OP_ADVSIMD_TRN1 ||
                instruction->opcode == AARCH64_OP_ADVSIMD_TRN2) {
            source = (lane & 1) == 0 ? rn : rm;
            index = (byte_t) (2 * (lane / 2) + second);
        } else {
            source = (lane & 1) == 0 ? rn : rm;
            index = (byte_t) ((second ? half : 0) + lane / 2);
        }
        qword_t value = read_vector_element(
                &cpu->v[source], element_size, index);
        write_vector_element(&result, element_size, lane, value);
    }
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static void execute_advsimd_compare(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_three_same.rd;
    byte_t rn = instruction->operands.advsimd_three_same.rn;
    byte_t rm = instruction->operands.advsimd_three_same.rm;
    byte_t element_size =
            instruction->operands.advsimd_three_same.element_size;
    byte_t lanes = (byte_t) (instruction->width / (element_size * 8));
    qword_t true_value = vector_element_mask(element_size);
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < lanes; lane++) {
        qword_t left = read_vector_element(&cpu->v[rn], element_size, lane);
        qword_t right = read_vector_element(&cpu->v[rm], element_size, lane);
        bool matches;
        if (instruction->opcode == AARCH64_OP_ADVSIMD_CMTST) {
            matches = (left & right) != 0;
        } else if (instruction->opcode == AARCH64_OP_ADVSIMD_CMEQ) {
            matches = left == right;
        } else {
            bool unsigned_comparison =
                    instruction->opcode == AARCH64_OP_ADVSIMD_CMHI ||
                    instruction->opcode == AARCH64_OP_ADVSIMD_CMHS;
            bool equal_matches =
                    instruction->opcode == AARCH64_OP_ADVSIMD_CMGE ||
                    instruction->opcode == AARCH64_OP_ADVSIMD_CMHS;
            qword_t ordered_left = unsigned_comparison ? left : left ^ sign;
            qword_t ordered_right = unsigned_comparison ? right : right ^ sign;
            matches = equal_matches ? ordered_left >= ordered_right :
                    ordered_left > ordered_right;
        }
        write_vector_element(&result, element_size, lane,
                matches ? true_value : 0);
    }
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static qword_t advsimd_logical_word(enum aarch64_opcode opcode,
        qword_t old, qword_t left, qword_t right) {
    if (opcode == AARCH64_OP_ADVSIMD_AND)
        return left & right;
    if (opcode == AARCH64_OP_ADVSIMD_BIC)
        return left & ~right;
    if (opcode == AARCH64_OP_ADVSIMD_ORR)
        return left | right;
    if (opcode == AARCH64_OP_ADVSIMD_ORN)
        return left | ~right;
    if (opcode == AARCH64_OP_ADVSIMD_EOR)
        return left ^ right;
    if (opcode == AARCH64_OP_ADVSIMD_BSL)
        return (old & left) | (~old & right);
    if (opcode == AARCH64_OP_ADVSIMD_BIT)
        return (right & left) | (~right & old);
    return (~right & left) | (right & old);
}

static void execute_advsimd_logical(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_three_same.rd;
    byte_t rn = instruction->operands.advsimd_three_same.rn;
    byte_t rm = instruction->operands.advsimd_three_same.rm;
    union aarch64_vector_reg result = {0};
    result.d[0] = advsimd_logical_word(instruction->opcode,
            cpu->v[rd].d[0], cpu->v[rn].d[0], cpu->v[rm].d[0]);
    if (instruction->width == 128) {
        result.d[1] = advsimd_logical_word(instruction->opcode,
                cpu->v[rd].d[1], cpu->v[rn].d[1], cpu->v[rm].d[1]);
    }
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static void execute_advsimd_pairwise_extrema(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_three_same.rd;
    byte_t rn = instruction->operands.advsimd_three_same.rn;
    byte_t rm = instruction->operands.advsimd_three_same.rm;
    byte_t element_size =
            instruction->operands.advsimd_three_same.element_size;
    byte_t lanes = (byte_t) (instruction->width / (element_size * 8));
    byte_t half = lanes / 2;
    bool minimum = instruction->opcode == AARCH64_OP_ADVSIMD_SMINP ||
            instruction->opcode == AARCH64_OP_ADVSIMD_UMINP;
    bool unsigned_comparison =
            instruction->opcode == AARCH64_OP_ADVSIMD_UMAXP ||
            instruction->opcode == AARCH64_OP_ADVSIMD_UMINP;
    qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
    union aarch64_vector_reg result = {0};
    // 结果前半归约 Rn、后半归约 Rm；延迟写回保护全寄存器别名。
    for (byte_t lane = 0; lane < lanes; lane++) {
        byte_t source = lane < half ? rn : rm;
        byte_t pair = (byte_t) (2 * (lane % half));
        qword_t left = read_vector_element(
                &cpu->v[source], element_size, pair);
        qword_t right = read_vector_element(
                &cpu->v[source], element_size, (byte_t) (pair + 1));
        qword_t ordered_left = unsigned_comparison ? left : left ^ sign;
        qword_t ordered_right = unsigned_comparison ? right : right ^ sign;
        bool select_left = minimum ? ordered_left <= ordered_right :
                ordered_left >= ordered_right;
        qword_t value = select_left ? left : right;
        write_vector_element(&result, element_size, lane, value);
    }
    cpu->v[rd] = result;
    cpu->pc += 4;
}

static void execute_fmov_transfer(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.data_processing_1source.rd;
    byte_t rn = instruction->operands.data_processing_1source.rn;
    byte_t width = instruction->width;
    if (instruction->opcode == AARCH64_OP_FMOV_GENERAL_FROM_SIMD) {
        qword_t value = width == 16 ? cpu->v[rn].h[0] :
                width == 32 ? cpu->v[rn].s[0] : cpu->v[rn].d[0];
        write_register(cpu, rd, width == 64 ? 64 : 32, false, value);
    } else if (instruction->opcode ==
            AARCH64_OP_FMOV_GENERAL_FROM_SIMD_HIGH) {
        write_register(cpu, rd, 64, false, cpu->v[rn].d[1]);
    } else if (instruction->opcode == AARCH64_OP_FMOV_SIMD_FROM_GENERAL) {
        qword_t value = read_register(
                cpu, rn, width == 64 ? 64 : 32, false);
        union aarch64_vector_reg result = {0};
        if (width == 16)
            result.h[0] = (word_t) value;
        else if (width == 32)
            result.s[0] = (dword_t) value;
        else
            result.d[0] = value;
        cpu->v[rd] = result;
    } else {
        // 写入 D[1] 只替换向量高半，低 64 位必须保留。
        cpu->v[rd].d[1] = read_register(cpu, rn, 64, false);
    }
    cpu->pc += 4;
}

static bool integer_to_fp_rounds_up(qword_t significand,
        qword_t remainder, unsigned discarded_bits,
        bool negative, dword_t fpcr) {
    if (remainder == 0)
        return false;
    dword_t mode = (fpcr & AARCH64_FPCR_RMODE_MASK) >>
            AARCH64_FPCR_RMODE_SHIFT;
    if (mode == 1)
        return !negative;
    if (mode == 2)
        return negative;
    if (mode == 3)
        return false;

    qword_t halfway = UINT64_C(1) << (discarded_bits - 1);
    return remainder > halfway ||
            (remainder == halfway && (significand & 1) != 0);
}

static qword_t integer_to_fp_bits(qword_t magnitude, bool negative,
        byte_t destination_width, dword_t fpcr, bool *inexact) {
    *inexact = false;
    if (magnitude == 0)
        return 0;

    unsigned fraction_bits = destination_width == 32 ? 23 : 52;
    unsigned precision = fraction_bits + 1;
    unsigned exponent_bias = destination_width == 32 ? 127 : 1023;
    unsigned exponent = 63U - (unsigned) __builtin_clzll(magnitude);
    qword_t significand;
    if (exponent <= fraction_bits) {
        significand = magnitude << (fraction_bits - exponent);
    } else {
        unsigned discarded_bits = exponent - fraction_bits;
        qword_t discarded_mask =
                (UINT64_C(1) << discarded_bits) - 1;
        qword_t remainder = magnitude & discarded_mask;
        significand = magnitude >> discarded_bits;
        *inexact = remainder != 0;
        if (integer_to_fp_rounds_up(significand, remainder,
                discarded_bits, negative, fpcr)) {
            significand++;
            if (significand == (UINT64_C(1) << precision)) {
                significand >>= 1;
                exponent++;
            }
        }
    }

    qword_t fraction_mask =
            (UINT64_C(1) << fraction_bits) - 1;
    qword_t sign = negative ?
            UINT64_C(1) << (destination_width - 1) : 0;
    return sign |
            (qword_t) (exponent + exponent_bias) << fraction_bits |
            (significand & fraction_mask);
}

static void convert_integer_to_fp(struct cpu_state *cpu, byte_t rd,
        qword_t source, byte_t source_width, byte_t destination_width,
        bool signed_conversion) {
    bool negative = signed_conversion &&
            (source & (UINT64_C(1) << (source_width - 1))) != 0;
    qword_t magnitude = source;
    if (negative) {
        magnitude = source_width == 32 ?
                (dword_t) (0 - (dword_t) source) : 0 - source;
    }

    bool inexact;
    qword_t bits = integer_to_fp_bits(magnitude, negative,
            destination_width, cpu->fpcr, &inexact);
    union aarch64_vector_reg result = {0};
    if (destination_width == 32)
        result.s[0] = (dword_t) bits;
    else
        result.d[0] = bits;
    cpu->v[rd] = result;
    // 尚无浮点异常陷阱事件通道；Linux 默认 FPCR 只累积 IXC 粘滞位。
    if (inexact)
        cpu->fpsr |= AARCH64_FPSR_IXC;
}

static void execute_integer_to_fp(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rn = instruction->operands.integer_to_fp.rn;
    qword_t source = read_register(
            cpu, rn, instruction->width, false);
    convert_integer_to_fp(cpu, instruction->operands.integer_to_fp.rd,
            source, instruction->width,
            instruction->operands.integer_to_fp.destination_width,
            instruction->opcode == AARCH64_OP_SCVTF_GENERAL);
    cpu->pc += 4;
}

static qword_t read_scalar_fp(const struct cpu_state *cpu,
        byte_t reg, byte_t width) {
    return width == 32 ? cpu->v[reg].s[0] : cpu->v[reg].d[0];
}

static void write_scalar_fp(struct cpu_state *cpu, byte_t reg,
        byte_t width, qword_t bits) {
    union aarch64_vector_reg result = {0};
    if (width == 32)
        result.s[0] = (dword_t) bits;
    else
        result.d[0] = bits;
    cpu->v[reg] = result;
}

static qword_t scalar_fp_sign_mask(byte_t width) {
    return UINT64_C(1) << (width - 1);
}

static qword_t scalar_fp_fraction_mask(byte_t width) {
    return width == 32 ? UINT32_C(0x007fffff) :
            UINT64_C(0x000fffffffffffff);
}

static qword_t scalar_fp_exponent_mask(byte_t width) {
    return width == 32 ? UINT32_C(0x7f800000) :
            UINT64_C(0x7ff0000000000000);
}

static qword_t flush_scalar_fp_input(qword_t bits, byte_t width,
        dword_t fpcr, dword_t *exceptions) {
    qword_t exponent_mask = scalar_fp_exponent_mask(width);
    qword_t fraction_mask = scalar_fp_fraction_mask(width);
    if ((fpcr & AARCH64_FPCR_FZ) != 0 &&
            (bits & exponent_mask) == 0 &&
            (bits & fraction_mask) != 0) {
        *exceptions |= AARCH64_FPSR_IDC;
        return bits & scalar_fp_sign_mask(width);
    }
    return bits;
}

static void execute_scalar_fp_binary(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    byte_t rd = instruction->operands.data_processing_2source.rd;
    byte_t rn = instruction->operands.data_processing_2source.rn;
    byte_t rm = instruction->operands.data_processing_2source.rm;
    qword_t left = read_scalar_fp(cpu, rn, width);
    qword_t right = read_scalar_fp(cpu, rm, width);
    struct aarch64_scalar_fp_result result;
    if (instruction->opcode == AARCH64_OP_FADD_SCALAR) {
        result = aarch64_scalar_fp_add(
                left, right, width, cpu->fpcr);
    } else if (instruction->opcode == AARCH64_OP_FSUB_SCALAR) {
        result = aarch64_scalar_fp_subtract(
                left, right, width, cpu->fpcr);
    } else {
        result = aarch64_scalar_fp_multiply(
                left, right, width, cpu->fpcr);
    }
    write_scalar_fp(cpu, rd, width, result.bits);
    // 异常使能位尚无执行事件通道；当前只累积 Linux 默认使用的 FPSR。
    cpu->fpsr |= result.exceptions;
    cpu->pc += 4;
}

static void execute_scalar_fp_move(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.data_processing_1source.rd;
    byte_t rn = instruction->operands.data_processing_1source.rn;
    write_scalar_fp(cpu, rd, instruction->width,
            read_scalar_fp(cpu, rn, instruction->width));
    cpu->pc += 4;
}

static void execute_scalar_fp_immediate(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    write_scalar_fp(cpu, instruction->operands.scalar_fp_immediate.rd,
            instruction->width,
            instruction->operands.scalar_fp_immediate.immediate);
    cpu->pc += 4;
}

static void execute_scalar_fp_compare(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    byte_t rn = instruction->operands.scalar_fp_compare.rn;
    qword_t left = read_scalar_fp(cpu, rn, width);
    qword_t right = instruction->operands.scalar_fp_compare.zero ? 0 :
            read_scalar_fp(cpu,
                    instruction->operands.scalar_fp_compare.rm, width);
    struct aarch64_scalar_fp_compare_result result =
            aarch64_scalar_fp_compare(left, right, width, cpu->fpcr,
                    instruction->opcode == AARCH64_OP_FCMPE_SCALAR);
    aarch64_set_nzcv(cpu, result.nzcv);
    cpu->fpsr |= result.exceptions;
    cpu->pc += 4;
}

static qword_t scalar_fp_signed_limit(byte_t width, bool negative) {
    qword_t sign = UINT64_C(1) << (width - 1);
    return negative ? sign : sign - 1;
}

static void execute_scalar_fp_to_integer(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    byte_t rd = instruction->operands.data_processing_1source.rd;
    byte_t rn = instruction->operands.data_processing_1source.rn;
    dword_t exceptions = 0;
    qword_t bits = flush_scalar_fp_input(
            read_scalar_fp(cpu, rn, width), width, cpu->fpcr, &exceptions);
    qword_t sign_mask = scalar_fp_sign_mask(width);
    qword_t exponent_mask = scalar_fp_exponent_mask(width);
    qword_t fraction_mask = scalar_fp_fraction_mask(width);
    qword_t fraction = bits & fraction_mask;
    bool negative = (bits & sign_mask) != 0;
    unsigned fraction_bits = width == 32 ? 23 : 52;
    unsigned exponent_bias = width == 32 ? 127 : 1023;
    unsigned raw_exponent = (unsigned) ((bits & exponent_mask) >>
            fraction_bits);
    unsigned maximum_exponent = width == 32 ? 255 : 2047;
    qword_t converted = 0;

    if (raw_exponent == maximum_exponent) {
        if (fraction != 0)
            converted = 0;
        else
            converted = scalar_fp_signed_limit(width, negative);
        exceptions |= AARCH64_FPSR_IOC;
    } else if (raw_exponent == 0) {
        if (fraction != 0)
            exceptions |= AARCH64_FPSR_IXC;
    } else {
        int exponent = (int) raw_exponent - (int) exponent_bias;
        if (exponent < 0) {
            exceptions |= AARCH64_FPSR_IXC;
        } else {
            qword_t significand = (UINT64_C(1) << fraction_bits) |
                    fraction;
            bool invalid = exponent > width - 1;
            if (exponent == width - 1) {
                invalid = !negative ||
                        significand != (UINT64_C(1) << fraction_bits);
            }
            if (invalid) {
                converted = scalar_fp_signed_limit(width, negative);
                exceptions |= AARCH64_FPSR_IOC;
            } else {
                qword_t magnitude;
                if ((unsigned) exponent >= fraction_bits) {
                    magnitude = significand <<
                            ((unsigned) exponent - fraction_bits);
                } else {
                    unsigned discarded_bits =
                            fraction_bits - (unsigned) exponent;
                    qword_t discarded_mask =
                            (UINT64_C(1) << discarded_bits) - 1;
                    if ((significand & discarded_mask) != 0)
                        exceptions |= AARCH64_FPSR_IXC;
                    magnitude = significand >> discarded_bits;
                }
                converted = negative ? 0 - magnitude : magnitude;
                if (width == 32)
                    converted = (dword_t) converted;
            }
        }
    }
    write_scalar_fp(cpu, rd, width, converted);
    cpu->fpsr |= exceptions;
    cpu->pc += 4;
}

static void execute_scalar_integer_to_fp(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.data_processing_1source.rd;
    byte_t rn = instruction->operands.data_processing_1source.rn;
    qword_t source = read_scalar_fp(cpu, rn, instruction->width);
    convert_integer_to_fp(cpu, rd, source, instruction->width,
            instruction->width, true);
    cpu->pc += 4;
}

static void execute_advsimd_copy(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.advsimd_copy.rd;
    byte_t rn = instruction->operands.advsimd_copy.rn;
    byte_t element_size = instruction->operands.advsimd_copy.element_size;
    qword_t value;

    if (instruction->opcode == AARCH64_OP_ADVSIMD_DUP_GENERAL ||
            instruction->opcode == AARCH64_OP_ADVSIMD_INS_GENERAL) {
        value = read_register(cpu, rn,
                element_size == 8 ? 64 : 32, false);
    } else {
        value = read_vector_element(&cpu->v[rn], element_size,
                instruction->operands.advsimd_copy.source_index);
    }

    if (instruction->opcode == AARCH64_OP_ADVSIMD_DUP_ELEMENT ||
            instruction->opcode == AARCH64_OP_ADVSIMD_DUP_GENERAL) {
        union aarch64_vector_reg result = {0};
        byte_t lanes = (byte_t) (instruction->width /
                (element_size * 8));
        for (byte_t lane = 0; lane < lanes; lane++)
            write_vector_element(&result, element_size, lane, value);
        cpu->v[rd] = result;
    } else if (instruction->opcode == AARCH64_OP_ADVSIMD_INS_ELEMENT ||
            instruction->opcode == AARCH64_OP_ADVSIMD_INS_GENERAL) {
        write_vector_element(&cpu->v[rd], element_size,
                instruction->operands.advsimd_copy.destination_index, value);
    } else {
        if (instruction->opcode == AARCH64_OP_ADVSIMD_SMOV &&
                element_size != 8) {
            qword_t sign = UINT64_C(1) << (element_size * 8 - 1);
            qword_t mask = vector_element_mask(element_size);
            if (value & sign)
                value |= ~mask;
        }
        write_register(cpu, rd, instruction->width, false, value);
    }
    cpu->pc += 4;
}

static void execute_data_processing_2source(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t width = instruction->width;
    byte_t rd = instruction->operands.data_processing_2source.rd;
    qword_t left = read_register(cpu,
            instruction->operands.data_processing_2source.rn,
            width, false);
    qword_t right = read_register(cpu,
            instruction->operands.data_processing_2source.rm,
            width, false);
    qword_t result;

    if (instruction->opcode == AARCH64_OP_UDIV)
        result = right == 0 ? 0 : left / right;
    else if (instruction->opcode == AARCH64_OP_SDIV)
        result = signed_divide(left, right, width);
    else {
        enum aarch64_shift_type shift_type;
        if (instruction->opcode == AARCH64_OP_LSLV)
            shift_type = AARCH64_SHIFT_LSL;
        else if (instruction->opcode == AARCH64_OP_LSRV)
            shift_type = AARCH64_SHIFT_LSR;
        else if (instruction->opcode == AARCH64_OP_ASRV)
            shift_type = AARCH64_SHIFT_ASR;
        else
            shift_type = AARCH64_SHIFT_ROR;
        result = shift_register(left, width, shift_type,
                (byte_t) (right & (width - 1)));
    }

    write_register(cpu, rd, width, false, result);
    cpu->pc += 4;
}

static void execute_multiply_add(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    byte_t rd = instruction->operands.data_processing_3source.rd;
    byte_t rn = instruction->operands.data_processing_3source.rn;
    byte_t rm = instruction->operands.data_processing_3source.rm;
    byte_t ra = instruction->operands.data_processing_3source.ra;
    bool basic = instruction->opcode == AARCH64_OP_MADD ||
            instruction->opcode == AARCH64_OP_MSUB;
    byte_t source_width = basic ? instruction->width : 32;
    qword_t left = read_register(cpu, rn, source_width, false);
    qword_t right = read_register(cpu, rm, source_width, false);
    qword_t accumulator = read_register(cpu, ra,
            instruction->width, false);

    bool signed_long = instruction->opcode == AARCH64_OP_SMADDL ||
            instruction->opcode == AARCH64_OP_SMSUBL;
    if (signed_long) {
        left = (left ^ UINT32_C(0x80000000)) - UINT32_C(0x80000000);
        right = (right ^ UINT32_C(0x80000000)) - UINT32_C(0x80000000);
    }
    qword_t product = left * right;
    bool subtract = instruction->opcode == AARCH64_OP_MSUB ||
            instruction->opcode == AARCH64_OP_SMSUBL ||
            instruction->opcode == AARCH64_OP_UMSUBL;
    qword_t result = subtract ? accumulator - product : accumulator + product;
    write_register(cpu, rd, instruction->width, false, result);
    cpu->pc += 4;
}

static void execute_multiply_high(struct cpu_state *cpu,
        const struct aarch64_decoded *instruction) {
    qword_t left = read_register(cpu,
            instruction->operands.data_processing_3source.rn, 64, false);
    qword_t right = read_register(cpu,
            instruction->operands.data_processing_3source.rm, 64, false);
    qword_t high = (qword_t) (((__uint128_t) left * right) >> 64);
    if (instruction->opcode == AARCH64_OP_SMULH) {
        // 从无符号高半修正符号项，避免依赖宿主有符号转换与算术语义。
        if (left >> 63)
            high -= right;
        if (right >> 63)
            high -= left;
    }
    write_register(cpu, instruction->operands.data_processing_3source.rd,
            64, false, high);
    cpu->pc += 4;
}

static qword_t load_little_endian(const byte_t *bytes, byte_t size) {
    qword_t value = 0;
    for (byte_t i = 0; i < size; i++)
        value |= (qword_t) bytes[i] << (i * 8);
    return value;
}

static void store_little_endian(byte_t *bytes, byte_t size, qword_t value) {
    for (byte_t i = 0; i < size; i++)
        bytes[i] = (byte_t) (value >> (i * 8));
}

static qword_t sign_extend_load(qword_t value, byte_t size) {
    byte_t bits = (byte_t) (size * 8);
    qword_t sign = UINT64_C(1) << (bits - 1);
    return (value ^ sign) - sign;
}

static bool execute_load_store(struct cpu_state *cpu, struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rt = instruction->operands.load_store.rt;
    byte_t rn = instruction->operands.load_store.rn;
    byte_t size = instruction->operands.load_store.size;
    guest_addr_t base = rn == 31 ? cpu->sp : cpu->x[rn];
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store.address_mode;
    qword_t offset = (qword_t) instruction->operands.load_store.offset;
    if (instruction->opcode == AARCH64_OP_LOAD_REGISTER_OFFSET ||
            instruction->opcode == AARCH64_OP_STORE_REGISTER_OFFSET) {
        qword_t index = read_register(cpu,
                instruction->operands.load_store.rm, 64, false);
        offset = extend_register(index, 64,
                instruction->operands.load_store.extend_type,
                instruction->operands.load_store.shift);
    }
    guest_addr_t adjusted = base + offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[8];

    // guest 固定运行在 EL0，未特权形式使用同一权限检查路径，但保留独立 opcode。
    bool load = instruction->opcode == AARCH64_OP_LOAD_IMM12 ||
            instruction->opcode == AARCH64_OP_LOAD_IMM9 ||
            instruction->opcode == AARCH64_OP_LOAD_UNPRIVILEGED ||
            instruction->opcode == AARCH64_OP_LOAD_REGISTER_OFFSET;
    if (load) {
        if (!guest_tlb_read(tlb, address, bytes, size,
                GUEST_MEMORY_READ, fault))
            return false;
        qword_t value = load_little_endian(bytes, size);
        if (instruction->operands.load_store.signed_load)
            value = sign_extend_load(value, size);
        write_register(cpu, rt, instruction->width, false, value);
    } else {
        qword_t value = read_register(cpu, rt, instruction->width, false);
        store_little_endian(bytes, size, value);
        if (!guest_tlb_write(tlb, address, bytes, size, fault))
            return false;
    }
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
    return true;
}

static bool execute_simd_load_store(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rt = instruction->operands.load_store.rt;
    byte_t rn = instruction->operands.load_store.rn;
    byte_t size = instruction->operands.load_store.size;
    guest_addr_t base = rn == 31 ? cpu->sp : cpu->x[rn];
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store.address_mode;
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store.offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;

    bool load = instruction->opcode == AARCH64_OP_LOAD_SIMD_IMM12 ||
            instruction->opcode == AARCH64_OP_LOAD_SIMD_IMM9;
    if (load) {
        union aarch64_vector_reg value = {0};
        if (!guest_tlb_read(tlb, address, value.b, size,
                GUEST_MEMORY_READ, fault))
            return false;
        cpu->v[rt] = value;
    } else if (!guest_tlb_write(
            tlb, address, cpu->v[rt].b, size, fault)) {
        return false;
    }

    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
    return true;
}

static bool execute_load_store_pair(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rt = instruction->operands.load_store_pair.rt;
    byte_t rt2 = instruction->operands.load_store_pair.rt2;
    byte_t rn = instruction->operands.load_store_pair.rn;
    bool signed_load = instruction->operands.load_store_pair.signed_load;
    byte_t size = signed_load ? 4 : (byte_t) (instruction->width / 8);
    guest_addr_t base = rn == 31 ? cpu->sp : cpu->x[rn];
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store_pair.address_mode;
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store_pair.offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[16];
    size_t access_size = (size_t) size * 2;

    if (instruction->opcode == AARCH64_OP_LOAD_PAIR) {
        if (!guest_tlb_read(tlb, address, bytes, access_size,
                GUEST_MEMORY_READ, fault))
            return false;
        qword_t first = load_little_endian(bytes, size);
        qword_t second = load_little_endian(bytes + size, size);
        if (signed_load) {
            first = sign_extend_load(first, size);
            second = sign_extend_load(second, size);
        }
        write_register(cpu, rt, instruction->width, false, first);
        write_register(cpu, rt2, instruction->width, false, second);
    } else {
        qword_t first = read_register(cpu, rt, instruction->width, false);
        qword_t second = read_register(cpu, rt2, instruction->width, false);
        store_little_endian(bytes, size, first);
        store_little_endian(bytes + size, size, second);
        if (!guest_tlb_write(tlb, address, bytes, access_size, fault))
            return false;
    }
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
    return true;
}

static bool execute_simd_load_store_pair(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rt = instruction->operands.load_store_pair.rt;
    byte_t rt2 = instruction->operands.load_store_pair.rt2;
    byte_t rn = instruction->operands.load_store_pair.rn;
    byte_t size = (byte_t) (instruction->width / 8);
    guest_addr_t base = rn == 31 ? cpu->sp : cpu->x[rn];
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store_pair.address_mode;
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store_pair.offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[2 * sizeof(union aarch64_vector_reg)];
    size_t access_size = (size_t) size * 2;

    if (instruction->opcode == AARCH64_OP_LOAD_SIMD_PAIR) {
        if (!guest_tlb_read(tlb, address, bytes, access_size,
                GUEST_MEMORY_READ, fault))
            return false;
        // 临时寄存器保证访存失败不改目标，并自然清零 S/D 的高位。
        union aarch64_vector_reg first = {0};
        union aarch64_vector_reg second = {0};
        memcpy(first.b, bytes, size);
        memcpy(second.b, bytes + size, size);
        cpu->v[rt] = first;
        cpu->v[rt2] = second;
    } else {
        memcpy(bytes, cpu->v[rt].b, size);
        memcpy(bytes + size, cpu->v[rt2].b, size);
        if (!guest_tlb_write(tlb, address, bytes, access_size, fault))
            return false;
    }
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
    return true;
}

static bool check_exclusive_alignment(guest_addr_t address, byte_t size,
        enum guest_memory_access access, struct guest_memory_fault *fault) {
    if ((address & (size - 1)) == 0)
        return true;
    *fault = (struct guest_memory_fault) {
        .address = address,
        .access = access,
        .kind = GUEST_MEMORY_FAULT_ALIGNMENT,
    };
    return false;
}

static bool execute_compare_swap_pair(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rs = instruction->operands.compare_swap_pair.rs;
    byte_t rs2 = instruction->operands.compare_swap_pair.rs2;
    byte_t rt = instruction->operands.compare_swap_pair.rt;
    byte_t rt2 = instruction->operands.compare_swap_pair.rt2;
    byte_t rn = instruction->operands.compare_swap_pair.rn;
    byte_t element_size = instruction->operands.compare_swap_pair.size;
    byte_t pair_size = element_size * 2;

    // 地址与四个数据输入允许重叠，任何写回前必须完成全部快照。
    guest_addr_t address = rn == 31 ? cpu->sp : cpu->x[rn];
    qword_t compare_low = read_register(cpu, rs,
            instruction->width, false);
    qword_t compare_high = read_register(cpu, rs2,
            instruction->width, false);
    qword_t replacement_low = read_register(cpu, rt,
            instruction->width, false);
    qword_t replacement_high = read_register(cpu, rt2,
            instruction->width, false);
    byte_t alignment = rn == 31 ? 16 : pair_size;
    if (!check_exclusive_alignment(
            address, alignment, GUEST_MEMORY_WRITE, fault))
        return false;

    byte_t expected[16];
    byte_t replacement[16];
    byte_t observed[16];
    store_little_endian(expected, element_size, compare_low);
    store_little_endian(expected + element_size,
            element_size, compare_high);
    store_little_endian(replacement, element_size, replacement_low);
    store_little_endian(replacement + element_size,
            element_size, replacement_high);
    enum guest_tlb_compare_exchange_result result =
            guest_tlb_compare_exchange(tlb, address,
                    expected, replacement, observed, pair_size, fault);
    if (result == GUEST_TLB_COMPARE_EXCHANGE_FAULT)
        return false;

    write_register(cpu, rs, instruction->width, false,
            load_little_endian(observed, element_size));
    write_register(cpu, rs2, instruction->width, false,
            load_little_endian(observed + element_size, element_size));
    cpu->pc += 4;
    return true;
}

static bool execute_load_exclusive(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rn = instruction->operands.exclusive.rn;
    byte_t size = instruction->operands.exclusive.size;
    guest_addr_t address = rn == 31 ? cpu->sp : cpu->x[rn];
    if (!check_exclusive_alignment(
            address, size, GUEST_MEMORY_READ, fault))
        return false;

    byte_t bytes[8];
    struct guest_tlb_exclusive_token token;
    if (!guest_tlb_load_exclusive(
            tlb, address, bytes, size, &token, fault))
        return false;
    qword_t value = load_little_endian(bytes, size);
    aarch64_set_exclusive(cpu, address, size, false, value, 0,
            token.address_space, token.mapping_generation,
            token.write_generation, token.sync_identity);
    write_register(cpu, instruction->operands.exclusive.rt,
            instruction->width, false, value);
    cpu->pc += 4;
    return true;
}

static bool execute_store_exclusive(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rn = instruction->operands.exclusive.rn;
    byte_t size = instruction->operands.exclusive.size;
    byte_t status = instruction->operands.exclusive.rs;
    guest_addr_t address = rn == 31 ? cpu->sp : cpu->x[rn];
    qword_t value = read_register(cpu, instruction->operands.exclusive.rt,
            instruction->width, false);
    if (!check_exclusive_alignment(
            address, size, GUEST_MEMORY_WRITE, fault))
        return false;

    if (!aarch64_exclusive_matches(cpu, address, size, false)) {
        aarch64_clear_exclusive(cpu);
        write_register(cpu, status, 32, false, 1);
        cpu->pc += 4;
        return true;
    }

    byte_t expected[8];
    store_little_endian(expected, size, cpu->exclusive.value_low);
    byte_t bytes[8];
    store_little_endian(bytes, size, value);
    struct guest_tlb_exclusive_token token = {
        .address_space = cpu->exclusive.address_space,
        .mapping_generation = cpu->exclusive.mapping_epoch,
        .write_generation = cpu->exclusive.write_epoch,
        .sync_identity = cpu->exclusive.sync_identity,
    };
    aarch64_clear_exclusive(cpu);
    enum guest_tlb_store_exclusive_result result = guest_tlb_store_exclusive(
            tlb, address, expected, bytes, size, token, fault);
    if (result == GUEST_TLB_EXCLUSIVE_FAULT)
        return false;
    write_register(cpu, status, 32, false,
            result == GUEST_TLB_EXCLUSIVE_STORED ? 0 : 1);
    cpu->pc += 4;
    return true;
}

static bool execute_load_exclusive_pair(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rn = instruction->operands.exclusive.rn;
    byte_t element_size = instruction->operands.exclusive.size;
    byte_t pair_size = element_size * 2;
    guest_addr_t address = rn == 31 ? cpu->sp : cpu->x[rn];
    if (!check_exclusive_alignment(
            address, pair_size, GUEST_MEMORY_READ, fault))
        return false;

    byte_t bytes[16];
    struct guest_tlb_exclusive_token token;
    if (!guest_tlb_load_exclusive(
            tlb, address, bytes, pair_size, &token, fault))
        return false;
    qword_t value_low = load_little_endian(bytes, element_size);
    qword_t value_high = load_little_endian(
            bytes + element_size, element_size);
    aarch64_set_exclusive(cpu, address, pair_size, true,
            value_low, value_high,
            token.address_space, token.mapping_generation,
            token.write_generation, token.sync_identity);
    write_register(cpu, instruction->operands.exclusive.rt,
            instruction->width, false, value_low);
    write_register(cpu, instruction->operands.exclusive.rt2,
            instruction->width, false, value_high);
    cpu->pc += 4;
    return true;
}

static bool execute_store_exclusive_pair(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction,
        struct guest_memory_fault *fault) {
    assert(tlb != NULL);
    byte_t rn = instruction->operands.exclusive.rn;
    byte_t element_size = instruction->operands.exclusive.size;
    byte_t pair_size = element_size * 2;
    byte_t status = instruction->operands.exclusive.rs;
    guest_addr_t address = rn == 31 ? cpu->sp : cpu->x[rn];
    qword_t value_low = read_register(cpu,
            instruction->operands.exclusive.rt,
            instruction->width, false);
    qword_t value_high = read_register(cpu,
            instruction->operands.exclusive.rt2,
            instruction->width, false);
    if (!check_exclusive_alignment(
            address, pair_size, GUEST_MEMORY_WRITE, fault))
        return false;

    if (!aarch64_exclusive_matches(cpu, address, pair_size, true)) {
        aarch64_clear_exclusive(cpu);
        write_register(cpu, status, 32, false, 1);
        cpu->pc += 4;
        return true;
    }

    byte_t expected[16];
    store_little_endian(expected, element_size,
            cpu->exclusive.value_low);
    store_little_endian(expected + element_size, element_size,
            cpu->exclusive.value_high);
    byte_t replacement[16];
    store_little_endian(replacement, element_size, value_low);
    store_little_endian(
            replacement + element_size, element_size, value_high);
    struct guest_tlb_exclusive_token token = {
        .address_space = cpu->exclusive.address_space,
        .mapping_generation = cpu->exclusive.mapping_epoch,
        .write_generation = cpu->exclusive.write_epoch,
        .sync_identity = cpu->exclusive.sync_identity,
    };
    aarch64_clear_exclusive(cpu);
    enum guest_tlb_store_exclusive_result result = guest_tlb_store_exclusive(
            tlb, address, expected, replacement, pair_size, token, fault);
    if (result == GUEST_TLB_EXCLUSIVE_FAULT)
        return false;
    write_register(cpu, status, 32, false,
            result == GUEST_TLB_EXCLUSIVE_STORED ? 0 : 1);
    cpu->pc += 4;
    return true;
}

struct aarch64_execute_result aarch64_execute(struct cpu_state *cpu,
        struct guest_tlb *tlb, const struct aarch64_decoded *instruction) {
    struct aarch64_execute_result result = {
        .stop = AARCH64_EXECUTE_RETIRED,
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };
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
        case AARCH64_OP_ADD_SHIFTED_REGISTER:
        case AARCH64_OP_ADDS_SHIFTED_REGISTER:
        case AARCH64_OP_SUB_SHIFTED_REGISTER:
        case AARCH64_OP_SUBS_SHIFTED_REGISTER:
            execute_add_sub_shifted(cpu, instruction);
            break;
        case AARCH64_OP_ADD_EXTENDED_REGISTER:
        case AARCH64_OP_ADDS_EXTENDED_REGISTER:
        case AARCH64_OP_SUB_EXTENDED_REGISTER:
        case AARCH64_OP_SUBS_EXTENDED_REGISTER:
            execute_add_sub_extended(cpu, instruction);
            break;
        case AARCH64_OP_ADC:
        case AARCH64_OP_ADCS:
        case AARCH64_OP_SBC:
        case AARCH64_OP_SBCS:
            execute_add_sub_carry(cpu, instruction);
            break;
        case AARCH64_OP_AND_SHIFTED_REGISTER:
        case AARCH64_OP_ORR_SHIFTED_REGISTER:
        case AARCH64_OP_EOR_SHIFTED_REGISTER:
        case AARCH64_OP_ANDS_SHIFTED_REGISTER:
            execute_logical_shifted(cpu, instruction);
            break;
        case AARCH64_OP_AND_IMMEDIATE:
        case AARCH64_OP_ORR_IMMEDIATE:
        case AARCH64_OP_EOR_IMMEDIATE:
        case AARCH64_OP_ANDS_IMMEDIATE:
            execute_logical_immediate(cpu, instruction);
            break;
        case AARCH64_OP_SBFM:
        case AARCH64_OP_BFM:
        case AARCH64_OP_UBFM:
            execute_bitfield(cpu, instruction);
            break;
        case AARCH64_OP_EXTR:
            execute_extract(cpu, instruction);
            break;
        case AARCH64_OP_MOVN:
        case AARCH64_OP_MOVZ:
        case AARCH64_OP_MOVK:
            execute_move_wide(cpu, instruction);
            break;
        case AARCH64_OP_ADR:
        case AARCH64_OP_ADRP:
            execute_pc_relative(cpu, instruction);
            break;
        case AARCH64_OP_CSEL:
        case AARCH64_OP_CSINC:
        case AARCH64_OP_CSINV:
        case AARCH64_OP_CSNEG:
            execute_conditional_select(cpu, instruction);
            break;
        case AARCH64_OP_CCMP:
        case AARCH64_OP_CCMN:
            execute_conditional_compare(cpu, instruction);
            break;
        case AARCH64_OP_RBIT:
        case AARCH64_OP_REV16:
        case AARCH64_OP_REV32:
        case AARCH64_OP_REV64:
        case AARCH64_OP_CLZ:
        case AARCH64_OP_CLS:
            execute_data_processing_1source(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_MOVI:
        case AARCH64_OP_ADVSIMD_MVNI:
        case AARCH64_OP_ADVSIMD_ORR_IMMEDIATE:
        case AARCH64_OP_ADVSIMD_BIC_IMMEDIATE:
            execute_advsimd_immediate(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_SHL_SCALAR:
            execute_advsimd_scalar_shift(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_SSHLL:
        case AARCH64_OP_ADVSIMD_SSHLL2:
            execute_advsimd_shift_long(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_DUP_ELEMENT:
        case AARCH64_OP_ADVSIMD_DUP_GENERAL:
        case AARCH64_OP_ADVSIMD_INS_ELEMENT:
        case AARCH64_OP_ADVSIMD_INS_GENERAL:
        case AARCH64_OP_ADVSIMD_SMOV:
        case AARCH64_OP_ADVSIMD_UMOV:
            execute_advsimd_copy(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_ADD:
            execute_advsimd_add(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_TBL:
        case AARCH64_OP_ADVSIMD_TBX:
            execute_advsimd_table(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_UZP1:
        case AARCH64_OP_ADVSIMD_UZP2:
        case AARCH64_OP_ADVSIMD_TRN1:
        case AARCH64_OP_ADVSIMD_TRN2:
        case AARCH64_OP_ADVSIMD_ZIP1:
        case AARCH64_OP_ADVSIMD_ZIP2:
            execute_advsimd_permute(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_CMGT:
        case AARCH64_OP_ADVSIMD_CMGE:
        case AARCH64_OP_ADVSIMD_CMHI:
        case AARCH64_OP_ADVSIMD_CMHS:
        case AARCH64_OP_ADVSIMD_CMTST:
        case AARCH64_OP_ADVSIMD_CMEQ:
            execute_advsimd_compare(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_AND:
        case AARCH64_OP_ADVSIMD_BIC:
        case AARCH64_OP_ADVSIMD_ORR:
        case AARCH64_OP_ADVSIMD_ORN:
        case AARCH64_OP_ADVSIMD_EOR:
        case AARCH64_OP_ADVSIMD_BSL:
        case AARCH64_OP_ADVSIMD_BIT:
        case AARCH64_OP_ADVSIMD_BIF:
            execute_advsimd_logical(cpu, instruction);
            break;
        case AARCH64_OP_ADVSIMD_SMAXP:
        case AARCH64_OP_ADVSIMD_SMINP:
        case AARCH64_OP_ADVSIMD_UMAXP:
        case AARCH64_OP_ADVSIMD_UMINP:
            execute_advsimd_pairwise_extrema(cpu, instruction);
            break;
        case AARCH64_OP_FMOV_GENERAL_FROM_SIMD:
        case AARCH64_OP_FMOV_SIMD_FROM_GENERAL:
        case AARCH64_OP_FMOV_GENERAL_FROM_SIMD_HIGH:
        case AARCH64_OP_FMOV_SIMD_HIGH_FROM_GENERAL:
            execute_fmov_transfer(cpu, instruction);
            break;
        case AARCH64_OP_SCVTF_GENERAL:
        case AARCH64_OP_UCVTF_GENERAL:
            execute_integer_to_fp(cpu, instruction);
            break;
        case AARCH64_OP_FADD_SCALAR:
        case AARCH64_OP_FSUB_SCALAR:
        case AARCH64_OP_FMUL_SCALAR:
            execute_scalar_fp_binary(cpu, instruction);
            break;
        case AARCH64_OP_FMOV_SCALAR:
            execute_scalar_fp_move(cpu, instruction);
            break;
        case AARCH64_OP_FMOV_IMMEDIATE:
            execute_scalar_fp_immediate(cpu, instruction);
            break;
        case AARCH64_OP_FCMP_SCALAR:
        case AARCH64_OP_FCMPE_SCALAR:
            execute_scalar_fp_compare(cpu, instruction);
            break;
        case AARCH64_OP_FCVTZS_SCALAR:
            execute_scalar_fp_to_integer(cpu, instruction);
            break;
        case AARCH64_OP_SCVTF_SCALAR:
            execute_scalar_integer_to_fp(cpu, instruction);
            break;
        case AARCH64_OP_UDIV:
        case AARCH64_OP_SDIV:
        case AARCH64_OP_LSLV:
        case AARCH64_OP_LSRV:
        case AARCH64_OP_ASRV:
        case AARCH64_OP_RORV:
            execute_data_processing_2source(cpu, instruction);
            break;
        case AARCH64_OP_MRS_FPCR:
            write_register(cpu, instruction->operands.system_register.rt,
                    64, false, aarch64_get_fpcr(cpu));
            cpu->pc += 4;
            break;
        case AARCH64_OP_MSR_FPCR:
            aarch64_set_fpcr(cpu, (dword_t) read_register(cpu,
                    instruction->operands.system_register.rt,
                    64, false));
            cpu->pc += 4;
            break;
        case AARCH64_OP_MRS_FPSR:
            write_register(cpu, instruction->operands.system_register.rt,
                    64, false, aarch64_get_fpsr(cpu));
            cpu->pc += 4;
            break;
        case AARCH64_OP_MSR_FPSR:
            aarch64_set_fpsr(cpu, (dword_t) read_register(cpu,
                    instruction->operands.system_register.rt,
                    64, false));
            cpu->pc += 4;
            break;
        case AARCH64_OP_MRS_TPIDR_EL0:
            write_register(cpu, instruction->operands.system_register.rt,
                    64, false, cpu->tpidr_el0);
            cpu->pc += 4;
            break;
        case AARCH64_OP_MSR_TPIDR_EL0:
            cpu->tpidr_el0 = read_register(cpu,
                    instruction->operands.system_register.rt,
                    64, false);
            cpu->pc += 4;
            break;
        case AARCH64_OP_MRS_DCZID_EL0:
            // 尚未实现 DC ZVA，因此通过 DZP 明确要求 guest 使用普通清零路径。
            write_register(cpu, instruction->operands.system_register.rt,
                    64, false, UINT64_C(0x10));
            cpu->pc += 4;
            break;
        case AARCH64_OP_MADD:
        case AARCH64_OP_MSUB:
        case AARCH64_OP_SMADDL:
        case AARCH64_OP_SMSUBL:
        case AARCH64_OP_UMADDL:
        case AARCH64_OP_UMSUBL:
            execute_multiply_add(cpu, instruction);
            break;
        case AARCH64_OP_SMULH:
        case AARCH64_OP_UMULH:
            execute_multiply_high(cpu, instruction);
            break;
        case AARCH64_OP_B:
            cpu->pc += (qword_t) instruction->operands.branch_immediate.displacement;
            break;
        case AARCH64_OP_BL:
            cpu->x[30] = cpu->pc + 4;
            cpu->pc += (qword_t) instruction->operands.branch_immediate.displacement;
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
        case AARCH64_OP_B_CONDITIONAL:
            if (aarch64_condition_holds(cpu->nzcv,
                    instruction->operands.conditional_branch.condition))
                cpu->pc += (qword_t)
                        instruction->operands.conditional_branch.displacement;
            else
                cpu->pc += 4;
            break;
        case AARCH64_OP_CBZ:
        case AARCH64_OP_CBNZ: {
            qword_t value = read_register(cpu,
                    instruction->operands.compare_branch.rt,
                    instruction->width, false);
            bool branch = instruction->opcode == AARCH64_OP_CBZ ?
                    value == 0 : value != 0;
            cpu->pc += branch ?
                    (qword_t) instruction->operands.compare_branch.displacement : 4;
            break;
        }
        case AARCH64_OP_TBZ:
        case AARCH64_OP_TBNZ: {
            qword_t value = read_register(cpu,
                    instruction->operands.test_branch.rt, 64, false);
            bool set = ((value >> instruction->operands.test_branch.bit) & 1) != 0;
            bool branch = instruction->opcode == AARCH64_OP_TBZ ? !set : set;
            cpu->pc += branch ?
                    (qword_t) instruction->operands.test_branch.displacement : 4;
            break;
        }
        case AARCH64_OP_LOAD_IMM12:
        case AARCH64_OP_STORE_IMM12:
        case AARCH64_OP_LOAD_IMM9:
        case AARCH64_OP_STORE_IMM9:
        case AARCH64_OP_LOAD_UNPRIVILEGED:
        case AARCH64_OP_STORE_UNPRIVILEGED:
        case AARCH64_OP_LOAD_REGISTER_OFFSET:
        case AARCH64_OP_STORE_REGISTER_OFFSET:
            if (!execute_load_store(cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_LOAD_SIMD_IMM12:
        case AARCH64_OP_STORE_SIMD_IMM12:
        case AARCH64_OP_LOAD_SIMD_IMM9:
        case AARCH64_OP_STORE_SIMD_IMM9:
            if (!execute_simd_load_store(
                    cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_LOAD_PAIR:
        case AARCH64_OP_STORE_PAIR:
            if (!execute_load_store_pair(cpu, tlb, instruction,
                    &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_LOAD_SIMD_PAIR:
        case AARCH64_OP_STORE_SIMD_PAIR:
            if (!execute_simd_load_store_pair(cpu, tlb, instruction,
                    &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_CASP:
        case AARCH64_OP_CASPA:
        case AARCH64_OP_CASPL:
        case AARCH64_OP_CASPAL:
            if (!execute_compare_swap_pair(
                    cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_LDXR:
        case AARCH64_OP_LDAXR:
            if (!execute_load_exclusive(
                    cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_STXR:
        case AARCH64_OP_STLXR:
            if (!execute_store_exclusive(
                    cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_LDXP:
        case AARCH64_OP_LDAXP:
            if (!execute_load_exclusive_pair(
                    cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_STXP:
        case AARCH64_OP_STLXP:
            if (!execute_store_exclusive_pair(
                    cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_CLREX:
            aarch64_clear_exclusive(cpu);
            cpu->pc += 4;
            break;
        case AARCH64_OP_DMB:
        case AARCH64_OP_DSB:
        case AARCH64_OP_ISB:
            cpu->pc += 4;
            break;
        case AARCH64_OP_SVC:
            aarch64_clear_exclusive(cpu);
            cpu->pc += 4;
            result.stop = AARCH64_EXECUTE_SYSCALL;
            break;
    }
    if (result.stop == AARCH64_EXECUTE_DATA_FAULT)
        aarch64_clear_exclusive(cpu);
    return result;
}

#include <assert.h>
#include <string.h>

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

static qword_t shift_register(qword_t value, byte_t width,
        enum aarch64_shift_type type, byte_t amount) {
    qword_t mask = register_mask(width);
    assert(amount < width);
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
    assert(type == AARCH64_SHIFT_ROR);
    return ((value >> amount) | (value << (width - amount))) & mask;
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

static void execute_add_shifted_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    byte_t rd = instruction->operands.add_sub_shifted.rd;
    byte_t rn = instruction->operands.add_sub_shifted.rn;
    byte_t rm = instruction->operands.add_sub_shifted.rm;
    byte_t width = instruction->width;
    qword_t left = read_general_register(cpu, rn, width, false);
    qword_t right = shift_register(
            read_general_register(cpu, rm, width, false),
            width, instruction->operands.add_sub_shifted.shift_type,
            instruction->operands.add_sub_shifted.shift);
    qword_t value = (left + right) & register_mask(width);

    write_general_register(cpu, rd, width, false, value);
    cpu->pc += 4;
}

static qword_t extend_general_register(const struct cpu_state *cpu,
        byte_t rm, byte_t width, enum aarch64_extend_type extend_type,
        byte_t shift) {
    assert(extend_type <= AARCH64_EXTEND_SXTX);
    assert(shift <= 4);
    unsigned source_width =
            8U << ((unsigned) extend_type & 3U);
    // UXTX/SXTX 在 W 形式中仍是合法编码，但源宽度按 32 位截断。
    if (source_width > width)
        source_width = width;
    qword_t source_mask = source_width == 64 ? UINT64_MAX :
            (UINT64_C(1) << source_width) - UINT64_C(1);
    qword_t value =
            read_general_register(cpu, rm, width, false) & source_mask;
    if (extend_type >= AARCH64_EXTEND_SXTB) {
        qword_t sign = UINT64_C(1) << (source_width - 1);
        value = (value ^ sign) - sign;
    }
    return (value << shift) & register_mask(width);
}

static void execute_add_sub_extended_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_ADD_EXTENDED_REGISTER ||
            instruction->opcode == AARCH64_OP_SUB_EXTENDED_REGISTER);
    byte_t width = instruction->width;
    enum aarch64_extend_type extend_type =
            instruction->operands.add_sub_extended.extend_type;
    qword_t right = extend_general_register(cpu,
            instruction->operands.add_sub_extended.rm,
            width, extend_type,
            instruction->operands.add_sub_extended.shift);
    qword_t left = read_general_register(cpu,
            instruction->operands.add_sub_extended.rn,
            width, true);
    bool subtract =
            instruction->opcode == AARCH64_OP_SUB_EXTENDED_REGISTER;
    qword_t value =
            (subtract ? left - right : left + right) & register_mask(width);

    write_general_register(cpu,
            instruction->operands.add_sub_extended.rd,
            width, true, value);
    cpu->pc += 4;
}

static void execute_subs_extended_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_SUBS_EXTENDED_REGISTER);
    byte_t rd = instruction->operands.add_sub_extended.rd;
    byte_t rn = instruction->operands.add_sub_extended.rn;
    byte_t rm = instruction->operands.add_sub_extended.rm;
    byte_t width = instruction->width;
    qword_t left = read_general_register(cpu, rn, width, true);
    qword_t right = extend_general_register(cpu, rm, width,
            instruction->operands.add_sub_extended.extend_type,
            instruction->operands.add_sub_extended.shift);
    qword_t value = (left - right) & register_mask(width);

    aarch64_set_nzcv(
            cpu, subtraction_flags(left, right, value, width));
    write_general_register(cpu, rd, width, false, value);
    cpu->pc += 4;
}

static void execute_sub_shifted_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_SUB_SHIFTED_REGISTER);
    byte_t rd = instruction->operands.add_sub_shifted.rd;
    byte_t rn = instruction->operands.add_sub_shifted.rn;
    byte_t rm = instruction->operands.add_sub_shifted.rm;
    byte_t width = instruction->width;
    qword_t left = read_general_register(cpu, rn, width, false);
    qword_t right = shift_register(
            read_general_register(cpu, rm, width, false),
            width, instruction->operands.add_sub_shifted.shift_type,
            instruction->operands.add_sub_shifted.shift);
    qword_t value = (left - right) & register_mask(width);

    write_general_register(cpu, rd, width, false, value);
    cpu->pc += 4;
}

static void execute_conditional_select_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_CSEL ||
            instruction->opcode == AARCH64_OP_CSINC);
    byte_t width = instruction->width;
    bool choose_rn = aarch64_condition_holds(cpu->nzcv,
            instruction->operands.conditional_select.condition);
    byte_t source = choose_rn ?
            instruction->operands.conditional_select.rn :
            instruction->operands.conditional_select.rm;
    qword_t value = read_general_register(
            cpu, source, width, false);
    if (!choose_rn && instruction->opcode == AARCH64_OP_CSINC)
        value = (value + 1) & register_mask(width);
    write_general_register(cpu,
            instruction->operands.conditional_select.rd,
            width, false, value);
    cpu->pc += 4;
}

static void execute_rev32_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_REV32);
    byte_t width = instruction->width;
    qword_t source = read_general_register(cpu,
            instruction->operands.data_processing_1source.rn,
            width, false);
    qword_t value =
            ((source & UINT64_C(0x000000ff000000ff)) << 24) |
            ((source & UINT64_C(0x0000ff000000ff00)) << 8) |
            ((source & UINT64_C(0x00ff000000ff0000)) >> 8) |
            ((source & UINT64_C(0xff000000ff000000)) >> 24);

    write_general_register(cpu,
            instruction->operands.data_processing_1source.rd,
            width, false, value);
    cpu->pc += 4;
}

static void execute_ccmp_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_CCMP);
    byte_t width = instruction->width;
    byte_t condition =
            instruction->operands.conditional_compare.condition;

    if (aarch64_condition_holds(cpu->nzcv, condition)) {
        qword_t left = read_general_register(cpu,
                instruction->operands.conditional_compare.rn,
                width, false);
        qword_t right = instruction->operands.conditional_compare.immediate ?
                instruction->operands.conditional_compare.operand :
                read_general_register(cpu,
                        instruction->operands.conditional_compare.operand,
                        width, false);
        qword_t value = (left - right) & register_mask(width);
        aarch64_set_nzcv(cpu,
                subtraction_flags(left, right, value, width));
    } else {
        aarch64_set_nzcv(cpu,
                (dword_t) instruction->operands.conditional_compare.nzcv << 28);
    }
    cpu->pc += 4;
}

static void execute_subs_shifted_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_SUBS_SHIFTED_REGISTER);
    byte_t rd = instruction->operands.add_sub_shifted.rd;
    byte_t rn = instruction->operands.add_sub_shifted.rn;
    byte_t rm = instruction->operands.add_sub_shifted.rm;
    byte_t width = instruction->width;
    qword_t left = read_general_register(cpu, rn, width, false);
    qword_t right = shift_register(
            read_general_register(cpu, rm, width, false),
            width, instruction->operands.add_sub_shifted.shift_type,
            instruction->operands.add_sub_shifted.shift);
    qword_t value = (left - right) & register_mask(width);

    aarch64_set_nzcv(
            cpu, subtraction_flags(left, right, value, width));
    write_general_register(cpu, rd, width, false, value);
    cpu->pc += 4;
}

static void execute_logical_shifted_register_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    byte_t rd = instruction->operands.logical_shifted.rd;
    byte_t rn = instruction->operands.logical_shifted.rn;
    byte_t rm = instruction->operands.logical_shifted.rm;
    byte_t width = instruction->width;
    qword_t mask = register_mask(width);
    qword_t left = read_general_register(cpu, rn, width, false);
    qword_t right = shift_register(
            read_general_register(cpu, rm, width, false),
            width, instruction->operands.logical_shifted.shift_type,
            instruction->operands.logical_shifted.shift);
    if (instruction->operands.logical_shifted.invert)
        right = ~right & mask;
    bool set_flags =
            instruction->opcode == AARCH64_OP_ANDS_SHIFTED_REGISTER;
    qword_t value;
    if (instruction->opcode == AARCH64_OP_AND_SHIFTED_REGISTER ||
            set_flags) {
        value = left & right;
    } else if (instruction->opcode == AARCH64_OP_ORR_SHIFTED_REGISTER) {
        value = left | right;
    } else {
        assert(instruction->opcode == AARCH64_OP_EOR_SHIFTED_REGISTER);
        value = left ^ right;
    }
    if (set_flags) {
        qword_t sign = UINT64_C(1) << (width - 1);
        aarch64_set_nzcv(cpu,
                (value & sign ? UINT32_C(1) << 31 : 0) |
                (value == 0 ? UINT32_C(1) << 30 : 0));
    }
    write_general_register(cpu, rd, width, false, value);
    cpu->pc += 4;
}

static void execute_logical_immediate_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_AND_IMMEDIATE ||
            instruction->opcode == AARCH64_OP_ORR_IMMEDIATE ||
            instruction->opcode == AARCH64_OP_ANDS_IMMEDIATE);
    byte_t rd = instruction->operands.logical_immediate.rd;
    byte_t rn = instruction->operands.logical_immediate.rn;
    byte_t width = instruction->width;
    bool set_flags = instruction->opcode == AARCH64_OP_ANDS_IMMEDIATE;
    qword_t left = read_general_register(cpu, rn, width, false);
    qword_t right = instruction->operands.logical_immediate.immediate;
    qword_t value = instruction->opcode == AARCH64_OP_ORR_IMMEDIATE ?
            left | right : left & right;

    if (set_flags) {
        qword_t sign = UINT64_C(1) << (width - 1);
        aarch64_set_nzcv(cpu,
                (value & sign ? UINT32_C(1) << 31 : 0) |
                (value == 0 ? UINT32_C(1) << 30 : 0));
    }
    write_general_register(cpu, rd, width, !set_flags, value);
    cpu->pc += 4;
}

static void execute_ubfm_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_UBFM);
    byte_t width = instruction->width;
    qword_t source = read_general_register(cpu,
            instruction->operands.bitfield_move.rn, width, false);
    qword_t rotated = shift_register(source, width, AARCH64_SHIFT_ROR,
            instruction->operands.bitfield_move.immr);
    qword_t value = rotated &
            instruction->operands.bitfield_move.write_mask &
            instruction->operands.bitfield_move.top_mask;
    write_general_register(cpu,
            instruction->operands.bitfield_move.rd, width, false, value);
    cpu->pc += 4;
}

static void execute_sbfm_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_SBFM);
    byte_t width = instruction->width;
    qword_t source = read_general_register(cpu,
            instruction->operands.bitfield_move.rn, width, false);
    qword_t rotated = shift_register(source, width, AARCH64_SHIFT_ROR,
            instruction->operands.bitfield_move.immr);
    bool sign =
            (source >> instruction->operands.bitfield_move.imms) & 1;
    qword_t top = sign ? register_mask(width) : 0;
    qword_t top_mask = instruction->operands.bitfield_move.top_mask;
    qword_t value = (top & ~top_mask) |
            (rotated & instruction->operands.bitfield_move.write_mask &
                    top_mask);
    write_general_register(cpu,
            instruction->operands.bitfield_move.rd, width, false, value);
    cpu->pc += 4;
}

static void execute_lsrv_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_LSRV);
    byte_t width = instruction->width;
    qword_t left = read_general_register(cpu,
            instruction->operands.data_processing_2source.rn,
            width, false);
    qword_t right = read_general_register(cpu,
            instruction->operands.data_processing_2source.rm,
            width, false);
    byte_t amount = (byte_t) (right & (width - 1));
    qword_t value = shift_register(
            left, width, AARCH64_SHIFT_LSR, amount);

    write_general_register(cpu,
            instruction->operands.data_processing_2source.rd,
            width, false, value);
    cpu->pc += 4;
}

static void execute_lslv_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_LSLV);
    byte_t width = instruction->width;
    qword_t left = read_general_register(cpu,
            instruction->operands.data_processing_2source.rn,
            width, false);
    qword_t right = read_general_register(cpu,
            instruction->operands.data_processing_2source.rm,
            width, false);
    byte_t amount = (byte_t) (right & (width - 1));
    qword_t value = shift_register(
            left, width, AARCH64_SHIFT_LSL, amount);

    write_general_register(cpu,
            instruction->operands.data_processing_2source.rd,
            width, false, value);
    cpu->pc += 4;
}

static void execute_multiply_add_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_MADD ||
            instruction->opcode == AARCH64_OP_MSUB ||
            instruction->opcode == AARCH64_OP_UMADDL);
    byte_t width = instruction->width;
    byte_t source_width =
            instruction->opcode == AARCH64_OP_UMADDL ? 32 : width;
    qword_t left = read_general_register(cpu,
            instruction->operands.data_processing_3source.rn,
            source_width, false);
    qword_t right = read_general_register(cpu,
            instruction->operands.data_processing_3source.rm,
            source_width, false);
    qword_t accumulator = read_general_register(cpu,
            instruction->operands.data_processing_3source.ra,
            width, false);
    qword_t product = left * right;
    qword_t value = instruction->opcode == AARCH64_OP_MSUB ?
            accumulator - product : accumulator + product;

    write_general_register(cpu,
            instruction->operands.data_processing_3source.rd,
            width, false, value);
    cpu->pc += 4;
}

static void execute_extract_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_EXTR);
    byte_t width = instruction->width;
    qword_t high = read_general_register(cpu,
            instruction->operands.extract.rn, width, false);
    qword_t low = read_general_register(cpu,
            instruction->operands.extract.rm, width, false);
    byte_t lsb = instruction->operands.extract.lsb;
    assert(lsb < width);
    qword_t value = low;
    if (lsb != 0)
        value = (low >> lsb) | (high << (width - lsb));
    write_general_register(cpu,
            instruction->operands.extract.rd, width, false, value);
    cpu->pc += 4;
}

static void execute_scalar_load_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_LOAD_IMM12 ||
            instruction->opcode == AARCH64_OP_LOAD_IMM9 ||
            instruction->opcode == AARCH64_OP_LOAD_REGISTER_OFFSET);
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store.address_mode;
    assert(instruction->opcode == AARCH64_OP_LOAD_IMM9 ||
            address_mode == AARCH64_ADDRESS_OFFSET);
    byte_t rt = instruction->operands.load_store.rt;
    byte_t rn = instruction->operands.load_store.rn;
    byte_t size = instruction->operands.load_store.size;
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    qword_t offset;
    if (instruction->opcode == AARCH64_OP_LOAD_IMM12 ||
            instruction->opcode == AARCH64_OP_LOAD_IMM9) {
        offset = (qword_t) instruction->operands.load_store.offset;
    } else {
        enum aarch64_extend_type extend_type =
                instruction->operands.load_store.extend_type;
        offset = read_general_register(cpu,
                instruction->operands.load_store.rm, 64, false);
        if (extend_type == AARCH64_EXTEND_UXTW ||
                extend_type == AARCH64_EXTEND_SXTW) {
            offset &= UINT32_MAX;
            if (extend_type == AARCH64_EXTEND_SXTW &&
                    (offset & (UINT64_C(1) << 31)))
                offset |= UINT64_C(0xffffffff00000000);
        } else {
            assert(extend_type == AARCH64_EXTEND_UXTX ||
                    extend_type == AARCH64_EXTEND_SXTX);
        }
        offset <<= instruction->operands.load_store.shift;
    }
    guest_addr_t adjusted = base + offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[8];
    if (!guest_tlb_read(tlb, address, bytes, size,
            GUEST_MEMORY_READ, &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }

    qword_t value = 0;
    for (byte_t index = 0; index < size; index++)
        value |= (qword_t) bytes[index] << (index * 8);
    if (instruction->operands.load_store.signed_load) {
        byte_t bits = (byte_t) (size * 8);
        qword_t sign = UINT64_C(1) << (bits - 1);
        value = (value ^ sign) - sign;
    }
    write_general_register(cpu, rt, instruction->width, false, value);
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_general_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
}

static void execute_load_pair_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_LOAD_PAIR);
    byte_t rt = instruction->operands.load_store_pair.rt;
    byte_t rt2 = instruction->operands.load_store_pair.rt2;
    byte_t rn = instruction->operands.load_store_pair.rn;
    bool signed_load = instruction->operands.load_store_pair.signed_load;
    byte_t size = signed_load ? 4 : (byte_t) (instruction->width / 8);
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store_pair.offset;
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store_pair.address_mode;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[16];
    if (!guest_tlb_read(tlb, address, bytes, (size_t) size * 2,
            GUEST_MEMORY_READ, &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }

    qword_t values[2] = {0};
    for (byte_t index = 0; index < size; index++) {
        values[0] |= (qword_t) bytes[index] << (index * 8);
        values[1] |= (qword_t) bytes[size + index] << (index * 8);
    }
    if (signed_load) {
        qword_t sign = UINT64_C(1) << 31;
        values[0] = (values[0] ^ sign) - sign;
        values[1] = (values[1] ^ sign) - sign;
    }
    write_general_register(cpu, rt, instruction->width, false, values[0]);
    write_general_register(cpu, rt2, instruction->width, false, values[1]);
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_general_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
}

static void execute_store_pair_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_STORE_PAIR);
    assert(!instruction->operands.load_store_pair.signed_load);
    byte_t rt = instruction->operands.load_store_pair.rt;
    byte_t rt2 = instruction->operands.load_store_pair.rt2;
    byte_t rn = instruction->operands.load_store_pair.rn;
    byte_t size = (byte_t) (instruction->width / 8);
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store_pair.offset;
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store_pair.address_mode;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    qword_t first = read_general_register(
            cpu, rt, instruction->width, false);
    qword_t second = read_general_register(
            cpu, rt2, instruction->width, false);
    byte_t bytes[16];
    for (byte_t index = 0; index < size; index++) {
        bytes[index] = (byte_t) (first >> (index * 8));
        bytes[size + index] = (byte_t) (second >> (index * 8));
    }
    if (!guest_tlb_write(tlb, address, bytes, (size_t) size * 2,
            &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }

    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_general_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
}

static void execute_store_simd_pair_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_STORE_SIMD_PAIR);
    byte_t rt = instruction->operands.load_store_pair.rt;
    byte_t rt2 = instruction->operands.load_store_pair.rt2;
    byte_t rn = instruction->operands.load_store_pair.rn;
    byte_t size = (byte_t) (instruction->width / 8);
    assert(size == 4 || size == 8 ||
            size == sizeof(union aarch64_vector_reg));
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store_pair.offset;
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store_pair.address_mode;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[2 * sizeof(union aarch64_vector_reg)];
    memcpy(bytes, cpu->v[rt].b, size);
    memcpy(bytes + size, cpu->v[rt2].b, size);
    if (!guest_tlb_write(tlb, address, bytes, (size_t) size * 2,
            &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }

    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_general_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
}

static void execute_store_simd_imm12_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_STORE_SIMD_IMM12);
    assert(instruction->operands.load_store.address_mode ==
            AARCH64_ADDRESS_OFFSET);
    byte_t rt = instruction->operands.load_store.rt;
    byte_t rn = instruction->operands.load_store.rn;
    byte_t size = instruction->operands.load_store.size;
    assert(size == 1 || size == 2 || size == 4 || size == 8 ||
            size == sizeof(union aarch64_vector_reg));
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    guest_addr_t address = base +
            (qword_t) instruction->operands.load_store.offset;
    if (!guest_tlb_write(tlb, address, cpu->v[rt].b, size,
            &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }
    cpu->pc += 4;
}

static void execute_scalar_store_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_STORE_IMM12 ||
            instruction->opcode == AARCH64_OP_STORE_IMM9);
    enum aarch64_address_mode address_mode =
            instruction->operands.load_store.address_mode;
    assert(instruction->opcode == AARCH64_OP_STORE_IMM9 ||
            address_mode == AARCH64_ADDRESS_OFFSET);
    byte_t rt = instruction->operands.load_store.rt;
    byte_t rn = instruction->operands.load_store.rn;
    byte_t size = instruction->operands.load_store.size;
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store.offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    qword_t value = read_general_register(
            cpu, rt, instruction->width, false);
    byte_t bytes[8];
    for (byte_t index = 0; index < size; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
    if (!guest_tlb_write(tlb, address, bytes, size, &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }
    if (address_mode != AARCH64_ADDRESS_OFFSET)
        write_general_register(cpu, rn, 64, true, adjusted);
    cpu->pc += 4;
}

static void execute_store_register_offset_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    assert(instruction->opcode == AARCH64_OP_STORE_REGISTER_OFFSET);
    assert(instruction->operands.load_store.address_mode ==
            AARCH64_ADDRESS_OFFSET);
    byte_t rt = instruction->operands.load_store.rt;
    byte_t rn = instruction->operands.load_store.rn;
    byte_t rm = instruction->operands.load_store.rm;
    byte_t size = instruction->operands.load_store.size;
    guest_addr_t base = read_general_register(cpu, rn, 64, true);
    enum aarch64_extend_type extend_type =
            instruction->operands.load_store.extend_type;
    qword_t offset = read_general_register(cpu, rm, 64, false);
    if (extend_type == AARCH64_EXTEND_UXTW ||
            extend_type == AARCH64_EXTEND_SXTW) {
        offset &= UINT32_MAX;
        if (extend_type == AARCH64_EXTEND_SXTW &&
                (offset & (UINT64_C(1) << 31)))
            offset |= UINT64_C(0xffffffff00000000);
    } else {
        assert(extend_type == AARCH64_EXTEND_UXTX ||
                extend_type == AARCH64_EXTEND_SXTX);
    }
    offset <<= instruction->operands.load_store.shift;

    guest_addr_t address = base + offset;
    qword_t value = read_general_register(
            cpu, rt, instruction->width, false);
    byte_t bytes[8];
    for (byte_t index = 0; index < size; index++)
        bytes[index] = (byte_t) (value >> (index * 8));
    if (!guest_tlb_write(tlb, address, bytes, size, &result->fault)) {
        aarch64_clear_exclusive(cpu);
        result->stop = AARCH64_EXECUTE_DATA_FAULT;
        return;
    }
    cpu->pc += 4;
}

static void execute_adrp_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_ADRP);
    assert(instruction->width == 64);
    qword_t page = cpu->pc & ~UINT64_C(0xfff);
    qword_t value = page +
            (qword_t) instruction->operands.pc_relative.displacement;

    write_general_register(cpu,
            instruction->operands.pc_relative.rd, 64, false, value);
    cpu->pc += 4;
}

static void execute_adr_fast(struct cpu_state *cpu,
        struct guest_tlb *tlb,
        const struct aarch64_decoded *instruction,
        struct aarch64_execute_result *result) {
    (void) tlb;
    (void) result;
    assert(instruction->opcode == AARCH64_OP_ADR);
    assert(instruction->width == 64);
    qword_t value = cpu->pc +
            (qword_t) instruction->operands.pc_relative.displacement;

    write_general_register(cpu,
            instruction->operands.pc_relative.rd, 64, false, value);
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
        case AARCH64_OP_ADD_SHIFTED_REGISTER:
            return execute_add_shifted_register_fast;
        case AARCH64_OP_ADD_EXTENDED_REGISTER:
        case AARCH64_OP_SUB_EXTENDED_REGISTER:
            return execute_add_sub_extended_register_fast;
        case AARCH64_OP_SUBS_EXTENDED_REGISTER:
            return execute_subs_extended_register_fast;
        case AARCH64_OP_SUB_SHIFTED_REGISTER:
            return execute_sub_shifted_register_fast;
        case AARCH64_OP_CSEL:
        case AARCH64_OP_CSINC:
            return execute_conditional_select_fast;
        case AARCH64_OP_REV32:
            return execute_rev32_fast;
        case AARCH64_OP_CCMP:
            return execute_ccmp_fast;
        case AARCH64_OP_SUBS_SHIFTED_REGISTER:
            return execute_subs_shifted_register_fast;
        case AARCH64_OP_AND_SHIFTED_REGISTER:
        case AARCH64_OP_ORR_SHIFTED_REGISTER:
        case AARCH64_OP_EOR_SHIFTED_REGISTER:
        case AARCH64_OP_ANDS_SHIFTED_REGISTER:
            return execute_logical_shifted_register_fast;
        case AARCH64_OP_AND_IMMEDIATE:
        case AARCH64_OP_ORR_IMMEDIATE:
        case AARCH64_OP_ANDS_IMMEDIATE:
            return execute_logical_immediate_fast;
        case AARCH64_OP_SBFM:
            return execute_sbfm_fast;
        case AARCH64_OP_UBFM:
            return execute_ubfm_fast;
        case AARCH64_OP_LSLV:
            return execute_lslv_fast;
        case AARCH64_OP_LSRV:
            return execute_lsrv_fast;
        case AARCH64_OP_MADD:
        case AARCH64_OP_MSUB:
        case AARCH64_OP_UMADDL:
            return execute_multiply_add_fast;
        case AARCH64_OP_EXTR:
            return execute_extract_fast;
        case AARCH64_OP_LOAD_IMM12:
        case AARCH64_OP_LOAD_IMM9:
        case AARCH64_OP_LOAD_REGISTER_OFFSET:
            return execute_scalar_load_fast;
        case AARCH64_OP_LOAD_PAIR:
            return execute_load_pair_fast;
        case AARCH64_OP_STORE_PAIR:
            return execute_store_pair_fast;
        case AARCH64_OP_STORE_SIMD_PAIR:
            return execute_store_simd_pair_fast;
        case AARCH64_OP_STORE_SIMD_IMM12:
            return execute_store_simd_imm12_fast;
        case AARCH64_OP_STORE_IMM12:
        case AARCH64_OP_STORE_IMM9:
            return execute_scalar_store_fast;
        case AARCH64_OP_STORE_REGISTER_OFFSET:
            return execute_store_register_offset_fast;
        case AARCH64_OP_ADR:
            return execute_adr_fast;
        case AARCH64_OP_ADRP:
            return execute_adrp_fast;
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

    if (entry->handler == NULL) {
#if ISH_AARCH64_THREADED_PROFILE
        cache->profile.undefined_dispatches++;
#endif
        return false;
    }
    *result = (struct aarch64_execute_result) {
        .stop = AARCH64_EXECUTE_RETIRED,
        .fault = {.kind = GUEST_MEMORY_FAULT_NONE},
    };
    if (entry->c_fallback) {
        cache->stats.c_fallbacks++;
#if ISH_AARCH64_THREADED_PROFILE
        assert(entry->decoded.opcode < AARCH64_OP_COUNT);
        qword_t *count =
                &cache->profile.fallback_by_opcode[entry->decoded.opcode];
        if ((*count)++ == 0) {
            cache->profile.representative_word_by_opcode[
                    entry->decoded.opcode] = word;
        }
#endif
    } else {
        cache->stats.fast_dispatches++;
    }
    entry->handler(cpu, tlb, &entry->decoded, result);
    return true;
}

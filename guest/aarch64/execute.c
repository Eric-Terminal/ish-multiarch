#include "guest/aarch64/execute.h"
#include "guest/aarch64/condition.h"

_Static_assert(GUEST_TLB_MAX_ACCESS_SIZE >= 2 * sizeof(qword_t),
        "AArch64 寄存器对访存要求一次覆盖 16 字节");

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
    guest_addr_t adjusted = base +
            (qword_t) instruction->operands.load_store.offset;
    guest_addr_t address = address_mode == AARCH64_ADDRESS_POST_INDEX ?
            base : adjusted;
    byte_t bytes[8];

    bool load = instruction->opcode == AARCH64_OP_LOAD_IMM12 ||
            instruction->opcode == AARCH64_OP_LOAD_IMM9;
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

static bool execute_load_store_pair(struct cpu_state *cpu,
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
    byte_t bytes[16];
    size_t access_size = (size_t) size * 2;

    if (instruction->opcode == AARCH64_OP_LOAD_PAIR) {
        if (!guest_tlb_read(tlb, address, bytes, access_size,
                GUEST_MEMORY_READ, fault))
            return false;
        qword_t first = load_little_endian(bytes, size);
        qword_t second = load_little_endian(bytes + size, size);
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
        case AARCH64_OP_UDIV:
        case AARCH64_OP_SDIV:
        case AARCH64_OP_LSLV:
        case AARCH64_OP_LSRV:
        case AARCH64_OP_ASRV:
        case AARCH64_OP_RORV:
            execute_data_processing_2source(cpu, instruction);
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
            if (!execute_load_store(cpu, tlb, instruction, &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_LOAD_PAIR:
        case AARCH64_OP_STORE_PAIR:
            if (!execute_load_store_pair(cpu, tlb, instruction,
                    &result.fault))
                result.stop = AARCH64_EXECUTE_DATA_FAULT;
            break;
        case AARCH64_OP_SVC:
            cpu->pc += 4;
            result.stop = AARCH64_EXECUTE_SYSCALL;
            break;
    }
    return result;
}

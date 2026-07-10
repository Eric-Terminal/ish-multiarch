#include "guest/aarch64/decode.h"

static int64_t sign_extend(dword_t immediate, byte_t bits) {
    int64_t value = immediate;
    if (immediate & (UINT32_C(1) << (bits - 1)))
        value -= INT64_C(1) << bits;
    return value;
}

static int64_t sign_extend_branch(dword_t immediate, byte_t bits) {
    return sign_extend(immediate, bits) * 4;
}

bool aarch64_decode(dword_t word, struct aarch64_decoded *decoded) {
    if (word == UINT32_C(0xd503201f)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_NOP,
            .width = 64,
        };
        return true;
    }

    if ((word & UINT32_C(0xffe0001f)) == UINT32_C(0xd4000001)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_SVC,
            .width = 64,
            .operands.exception.immediate =
                    (word >> 5) & UINT32_C(0xffff),
        };
        return true;
    }

    if ((word & UINT32_C(0x1f000000)) == UINT32_C(0x11000000)) {
        bool is_64 = word >> 31;
        bool subtract = (word >> 30) & 1;
        bool set_flags = (word >> 29) & 1;
        qword_t immediate = (word >> 10) & UINT32_C(0xfff);
        if ((word >> 22) & 1)
            immediate <<= 12;

        enum aarch64_opcode opcode;
        if (subtract)
            opcode = set_flags ? AARCH64_OP_SUBS_IMMEDIATE : AARCH64_OP_SUB_IMMEDIATE;
        else
            opcode = set_flags ? AARCH64_OP_ADDS_IMMEDIATE : AARCH64_OP_ADD_IMMEDIATE;

        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = is_64 ? 64 : 32,
            .operands.add_sub_immediate = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .immediate = immediate,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x1f200000)) == UINT32_C(0x0b000000)) {
        bool is_64 = word >> 31;
        bool subtract = (word >> 30) & 1;
        bool set_flags = (word >> 29) & 1;
        byte_t shift_type = (word >> 22) & 3;
        byte_t shift = (word >> 10) & UINT32_C(0x3f);
        if (shift_type == AARCH64_SHIFT_ROR || (!is_64 && shift >= 32))
            return false;

        enum aarch64_opcode opcode;
        if (subtract)
            opcode = set_flags ? AARCH64_OP_SUBS_SHIFTED_REGISTER :
                    AARCH64_OP_SUB_SHIFTED_REGISTER;
        else
            opcode = set_flags ? AARCH64_OP_ADDS_SHIFTED_REGISTER :
                    AARCH64_OP_ADD_SHIFTED_REGISTER;
        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = is_64 ? 64 : 32,
            .operands.add_sub_shifted = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .shift_type = (enum aarch64_shift_type) shift_type,
                .shift = shift,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x1f000000)) == UINT32_C(0x0a000000)) {
        bool is_64 = word >> 31;
        byte_t operation = (word >> 29) & 3;
        byte_t shift_type = (word >> 22) & 3;
        byte_t shift = (word >> 10) & UINT32_C(0x3f);
        if (!is_64 && shift >= 32)
            return false;
        static const enum aarch64_opcode opcodes[] = {
            AARCH64_OP_AND_SHIFTED_REGISTER,
            AARCH64_OP_ORR_SHIFTED_REGISTER,
            AARCH64_OP_EOR_SHIFTED_REGISTER,
            AARCH64_OP_ANDS_SHIFTED_REGISTER,
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation],
            .width = is_64 ? 64 : 32,
            .operands.logical_shifted = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .shift_type = (enum aarch64_shift_type) shift_type,
                .shift = shift,
                .invert = (word >> 21) & 1,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x1f800000)) == UINT32_C(0x12800000)) {
        bool is_64 = word >> 31;
        byte_t operation = (word >> 29) & 3;
        byte_t halfword = (word >> 21) & 3;
        if (operation == 1 || (!is_64 && halfword >= 2))
            return false;

        enum aarch64_opcode opcode = AARCH64_OP_MOVN;
        if (operation == 2)
            opcode = AARCH64_OP_MOVZ;
        else if (operation == 3)
            opcode = AARCH64_OP_MOVK;

        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = is_64 ? 64 : 32,
            .operands.move_wide = {
                .rd = word & 0x1f,
                .shift = halfword * 16,
                .immediate = (word >> 5) & UINT32_C(0xffff),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x1f000000)) == UINT32_C(0x10000000)) {
        dword_t immediate = (((word >> 5) & UINT32_C(0x7ffff)) << 2) |
                ((word >> 29) & 3);
        bool page_relative = word >> 31;
        int64_t displacement = sign_extend(immediate, 21);
        if (page_relative)
            displacement *= INT64_C(4096);
        *decoded = (struct aarch64_decoded) {
            .opcode = page_relative ? AARCH64_OP_ADRP : AARCH64_OP_ADR,
            .width = 64,
            .operands.pc_relative = {
                .rd = word & 0x1f,
                .displacement = displacement,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x7c000000)) == UINT32_C(0x14000000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 31) ? AARCH64_OP_BL : AARCH64_OP_B,
            .width = 64,
            .operands.branch_immediate.displacement =
                sign_extend_branch(word & UINT32_C(0x03ffffff), 26),
        };
        return true;
    }

    if ((word & UINT32_C(0xff000010)) == UINT32_C(0x54000000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_B_CONDITIONAL,
            .width = 64,
            .operands.conditional_branch = {
                .displacement = sign_extend_branch(
                        (word >> 5) & UINT32_C(0x7ffff), 19),
                .condition = word & 0xf,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x7e000000)) == UINT32_C(0x34000000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 24) & 1 ? AARCH64_OP_CBNZ : AARCH64_OP_CBZ,
            .width = (word >> 31) ? 64 : 32,
            .operands.compare_branch = {
                .rt = word & 0x1f,
                .displacement = sign_extend_branch(
                        (word >> 5) & UINT32_C(0x7ffff), 19),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x7e000000)) == UINT32_C(0x36000000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 24) & 1 ? AARCH64_OP_TBNZ : AARCH64_OP_TBZ,
            .width = (word >> 31) ? 64 : 32,
            .operands.test_branch = {
                .rt = word & 0x1f,
                .bit = (byte_t) (((word >> 31) << 5) |
                        ((word >> 19) & 0x1f)),
                .displacement = sign_extend_branch(
                        (word >> 5) & UINT32_C(0x3fff), 14),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3a000000)) == UINT32_C(0x28000000)) {
        byte_t operation = word >> 30;
        bool vector = (word >> 26) & 1;
        byte_t mode = (word >> 23) & 3;
        bool load = (word >> 22) & 1;
        if (vector || (operation != 0 && operation != 2) || mode == 0)
            return false;

        byte_t rt = word & 0x1f;
        byte_t rt2 = (word >> 10) & 0x1f;
        byte_t rn = (word >> 5) & 0x1f;
        bool writeback = mode != 2;
        // 这些重叠形式由架构留给实现选择，本解释器统一拒绝。
        if ((load && rt == rt2) || (writeback && rn != 31 &&
                (rn == rt || rn == rt2)))
            return false;

        byte_t size = operation == 0 ? 4 : 8;
        enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
        if (mode == 1)
            address_mode = AARCH64_ADDRESS_POST_INDEX;
        else if (mode == 3)
            address_mode = AARCH64_ADDRESS_PRE_INDEX;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_PAIR : AARCH64_OP_STORE_PAIR,
            .width = (byte_t) (size * 8),
            .operands.load_store_pair = {
                .rt = rt,
                .rt2 = rt2,
                .rn = rn,
                .offset = sign_extend((word >> 15) &
                        UINT32_C(0x7f), 7) * (int64_t) size,
                .address_mode = address_mode,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3b200000)) == UINT32_C(0x38000000)) {
        byte_t operation = (word >> 22) & 3;
        bool vector = (word >> 26) & 1;
        byte_t mode = (word >> 10) & 3;
        if (vector || operation > 1 || mode == 2)
            return false;
        byte_t rn = (word >> 5) & 0x1f;
        byte_t rt = word & 0x1f;
        // 写回与数据寄存器重叠时，架构不保证可移植的执行结果。
        if (mode != 0 && rn == rt && rn != 31)
            return false;

        byte_t size_shift = word >> 30;
        byte_t size = (byte_t) (1 << size_shift);
        enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
        if (mode == 1)
            address_mode = AARCH64_ADDRESS_POST_INDEX;
        else if (mode == 3)
            address_mode = AARCH64_ADDRESS_PRE_INDEX;
        *decoded = (struct aarch64_decoded) {
            .opcode = operation == 0 ?
                    AARCH64_OP_STORE_IMM9 : AARCH64_OP_LOAD_IMM9,
            .width = (byte_t) (size * 8),
            .operands.load_store = {
                .rt = rt,
                .rn = rn,
                .size = size,
                .offset = sign_extend((word >> 12) & UINT32_C(0x1ff), 9),
                .address_mode = address_mode,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3b000000)) == UINT32_C(0x39000000)) {
        byte_t operation = (word >> 22) & 3;
        bool vector = (word >> 26) & 1;
        if (vector || operation > 1)
            return false;

        byte_t size_shift = word >> 30;
        byte_t size = (byte_t) (1 << size_shift);
        *decoded = (struct aarch64_decoded) {
            .opcode = operation == 0 ?
                    AARCH64_OP_STORE_UNSIGNED_IMMEDIATE :
                    AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE,
            .width = (byte_t) (size * 8),
            .operands.load_store = {
                .rt = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .size = size,
                .offset = (int64_t) ((qword_t) ((word >> 10) &
                        UINT32_C(0xfff)) << size_shift),
                .address_mode = AARCH64_ADDRESS_OFFSET,
            },
        };
        return true;
    }

    dword_t branch_register = word & UINT32_C(0xfffffc1f);
    enum aarch64_opcode opcode;
    if (branch_register == UINT32_C(0xd61f0000))
        opcode = AARCH64_OP_BR;
    else if (branch_register == UINT32_C(0xd63f0000))
        opcode = AARCH64_OP_BLR;
    else if (branch_register == UINT32_C(0xd65f0000))
        opcode = AARCH64_OP_RET;
    else
        return false;

    byte_t rn = (word >> 5) & 0x1f;
    if (rn == 31)
        return false;
    *decoded = (struct aarch64_decoded) {
        .opcode = opcode,
        .width = 64,
        .operands.branch_register.rn = rn,
    };
    return true;
}

#ifndef GUEST_AARCH64_DECODE_H
#define GUEST_AARCH64_DECODE_H

#include "misc.h"

enum aarch64_opcode {
    AARCH64_OP_NOP,
    AARCH64_OP_ADD_IMMEDIATE,
    AARCH64_OP_ADDS_IMMEDIATE,
    AARCH64_OP_SUB_IMMEDIATE,
    AARCH64_OP_SUBS_IMMEDIATE,
    AARCH64_OP_MOVN,
    AARCH64_OP_MOVZ,
    AARCH64_OP_MOVK,
    AARCH64_OP_B,
    AARCH64_OP_BL,
    AARCH64_OP_BR,
    AARCH64_OP_BLR,
    AARCH64_OP_RET,
    AARCH64_OP_LOAD_UNSIGNED_IMMEDIATE,
    AARCH64_OP_STORE_UNSIGNED_IMMEDIATE,
    AARCH64_OP_SVC,
    AARCH64_OP_ADD_SHIFTED_REGISTER,
    AARCH64_OP_ADDS_SHIFTED_REGISTER,
    AARCH64_OP_SUB_SHIFTED_REGISTER,
    AARCH64_OP_SUBS_SHIFTED_REGISTER,
    AARCH64_OP_AND_SHIFTED_REGISTER,
    AARCH64_OP_ORR_SHIFTED_REGISTER,
    AARCH64_OP_EOR_SHIFTED_REGISTER,
    AARCH64_OP_ANDS_SHIFTED_REGISTER,
    AARCH64_OP_B_CONDITIONAL,
    AARCH64_OP_CBZ,
    AARCH64_OP_CBNZ,
    AARCH64_OP_TBZ,
    AARCH64_OP_TBNZ,
    AARCH64_OP_LOAD_IMM9,
    AARCH64_OP_STORE_IMM9,
};

enum aarch64_shift_type {
    AARCH64_SHIFT_LSL,
    AARCH64_SHIFT_LSR,
    AARCH64_SHIFT_ASR,
    AARCH64_SHIFT_ROR,
};

enum aarch64_address_mode {
    AARCH64_ADDRESS_OFFSET,
    AARCH64_ADDRESS_POST_INDEX,
    AARCH64_ADDRESS_PRE_INDEX,
};

struct aarch64_decoded {
    enum aarch64_opcode opcode;
    byte_t width;
    union {
        struct {
            byte_t rd;
            byte_t rn;
            qword_t immediate;
        } add_sub_immediate;
        struct {
            byte_t rd;
            byte_t shift;
            word_t immediate;
        } move_wide;
        struct {
            int64_t displacement;
        } branch_immediate;
        struct {
            byte_t rn;
        } branch_register;
        struct {
            byte_t rt;
            byte_t rn;
            byte_t size;
            int64_t offset;
            enum aarch64_address_mode address_mode;
        } load_store;
        struct {
            word_t immediate;
        } exception;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            enum aarch64_shift_type shift_type;
            byte_t shift;
        } add_sub_shifted;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            enum aarch64_shift_type shift_type;
            byte_t shift;
            bool invert;
        } logical_shifted;
        struct {
            int64_t displacement;
            byte_t condition;
        } conditional_branch;
        struct {
            byte_t rt;
            int64_t displacement;
        } compare_branch;
        struct {
            byte_t rt;
            byte_t bit;
            int64_t displacement;
        } test_branch;
    } operands;
};

bool aarch64_decode(dword_t word, struct aarch64_decoded *decoded);

#endif

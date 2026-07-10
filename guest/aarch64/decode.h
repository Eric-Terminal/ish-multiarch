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
};

enum aarch64_shift_type {
    AARCH64_SHIFT_LSL,
    AARCH64_SHIFT_LSR,
    AARCH64_SHIFT_ASR,
    AARCH64_SHIFT_ROR,
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
            qword_t offset;
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
    } operands;
};

bool aarch64_decode(dword_t word, struct aarch64_decoded *decoded);

#endif

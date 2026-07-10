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
    } operands;
};

bool aarch64_decode(dword_t word, struct aarch64_decoded *decoded);

#endif

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
    AARCH64_OP_LOAD_IMM12,
    AARCH64_OP_STORE_IMM12,
    AARCH64_OP_SVC,
    AARCH64_OP_ADD_SHIFTED_REGISTER,
    AARCH64_OP_ADDS_SHIFTED_REGISTER,
    AARCH64_OP_SUB_SHIFTED_REGISTER,
    AARCH64_OP_SUBS_SHIFTED_REGISTER,
    AARCH64_OP_ADD_EXTENDED_REGISTER,
    AARCH64_OP_ADDS_EXTENDED_REGISTER,
    AARCH64_OP_SUB_EXTENDED_REGISTER,
    AARCH64_OP_SUBS_EXTENDED_REGISTER,
    AARCH64_OP_AND_SHIFTED_REGISTER,
    AARCH64_OP_ORR_SHIFTED_REGISTER,
    AARCH64_OP_EOR_SHIFTED_REGISTER,
    AARCH64_OP_ANDS_SHIFTED_REGISTER,
    AARCH64_OP_AND_IMMEDIATE,
    AARCH64_OP_ORR_IMMEDIATE,
    AARCH64_OP_EOR_IMMEDIATE,
    AARCH64_OP_ANDS_IMMEDIATE,
    AARCH64_OP_SBFM,
    AARCH64_OP_BFM,
    AARCH64_OP_UBFM,
    AARCH64_OP_B_CONDITIONAL,
    AARCH64_OP_CBZ,
    AARCH64_OP_CBNZ,
    AARCH64_OP_TBZ,
    AARCH64_OP_TBNZ,
    AARCH64_OP_LOAD_IMM9,
    AARCH64_OP_STORE_IMM9,
    AARCH64_OP_LOAD_REGISTER_OFFSET,
    AARCH64_OP_STORE_REGISTER_OFFSET,
    AARCH64_OP_LOAD_PAIR,
    AARCH64_OP_STORE_PAIR,
    AARCH64_OP_ADR,
    AARCH64_OP_ADRP,
    AARCH64_OP_CSEL,
    AARCH64_OP_CSINC,
    AARCH64_OP_CSINV,
    AARCH64_OP_CSNEG,
    AARCH64_OP_UDIV,
    AARCH64_OP_SDIV,
    AARCH64_OP_LSLV,
    AARCH64_OP_LSRV,
    AARCH64_OP_ASRV,
    AARCH64_OP_RORV,
    AARCH64_OP_MRS_TPIDR_EL0,
    AARCH64_OP_MSR_TPIDR_EL0,
    AARCH64_OP_MADD,
    AARCH64_OP_MSUB,
    AARCH64_OP_SMADDL,
    AARCH64_OP_SMSUBL,
    AARCH64_OP_UMADDL,
    AARCH64_OP_UMSUBL,
    AARCH64_OP_SMULH,
    AARCH64_OP_UMULH,
    AARCH64_OP_EXTR,
    AARCH64_OP_RBIT,
    AARCH64_OP_REV16,
    AARCH64_OP_REV32,
    AARCH64_OP_REV64,
    AARCH64_OP_CLZ,
    AARCH64_OP_CLS,
    AARCH64_OP_ADVSIMD_MOVI,
    AARCH64_OP_ADVSIMD_MVNI,
    AARCH64_OP_ADVSIMD_ORR_IMMEDIATE,
    AARCH64_OP_ADVSIMD_BIC_IMMEDIATE,
};

enum aarch64_shift_type {
    AARCH64_SHIFT_LSL,
    AARCH64_SHIFT_LSR,
    AARCH64_SHIFT_ASR,
    AARCH64_SHIFT_ROR,
};

enum aarch64_extend_type {
    AARCH64_EXTEND_UXTB,
    AARCH64_EXTEND_UXTH,
    AARCH64_EXTEND_UXTW,
    AARCH64_EXTEND_UXTX,
    AARCH64_EXTEND_SXTB,
    AARCH64_EXTEND_SXTH,
    AARCH64_EXTEND_SXTW,
    AARCH64_EXTEND_SXTX,
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
            byte_t rm;
            byte_t size;
            enum aarch64_extend_type extend_type;
            byte_t shift;
            int64_t offset;
            enum aarch64_address_mode address_mode;
            bool signed_load;
        } load_store;
        struct {
            byte_t rt;
            byte_t rt2;
            byte_t rn;
            int64_t offset;
            enum aarch64_address_mode address_mode;
        } load_store_pair;
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
            enum aarch64_extend_type extend_type;
            byte_t shift;
        } add_sub_extended;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            enum aarch64_shift_type shift_type;
            byte_t shift;
            bool invert;
        } logical_shifted;
        struct {
            byte_t rd;
            byte_t rn;
            qword_t immediate;
        } logical_immediate;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t immr;
            byte_t imms;
            qword_t write_mask;
            qword_t top_mask;
        } bitfield_move;
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
        struct {
            byte_t rd;
            int64_t displacement;
        } pc_relative;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            byte_t condition;
        } conditional_select;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
        } data_processing_2source;
        struct {
            byte_t rd;
            byte_t rn;
        } data_processing_1source;
        struct {
            byte_t rd;
            qword_t immediate;
        } advsimd_immediate;
        struct {
            byte_t rt;
        } system_register;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            byte_t ra;
        } data_processing_3source;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            byte_t lsb;
        } extract;
    } operands;
};

bool aarch64_decode(dword_t word, struct aarch64_decoded *decoded);

#endif

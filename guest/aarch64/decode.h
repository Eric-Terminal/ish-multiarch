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
    AARCH64_OP_LOAD_SIMD_PAIR,
    AARCH64_OP_STORE_SIMD_PAIR,
    AARCH64_OP_LOAD_SIMD_IMM12,
    AARCH64_OP_STORE_SIMD_IMM12,
    AARCH64_OP_LOAD_SIMD_IMM9,
    AARCH64_OP_STORE_SIMD_IMM9,
    AARCH64_OP_LDXR,
    AARCH64_OP_LDAXR,
    AARCH64_OP_STXR,
    AARCH64_OP_STLXR,
    AARCH64_OP_CLREX,
    AARCH64_OP_DMB,
    AARCH64_OP_DSB,
    AARCH64_OP_ISB,
    AARCH64_OP_ADR,
    AARCH64_OP_ADRP,
    AARCH64_OP_CSEL,
    AARCH64_OP_CSINC,
    AARCH64_OP_CSINV,
    AARCH64_OP_CSNEG,
    AARCH64_OP_CCMP,
    AARCH64_OP_CCMN,
    AARCH64_OP_UDIV,
    AARCH64_OP_SDIV,
    AARCH64_OP_LSLV,
    AARCH64_OP_LSRV,
    AARCH64_OP_ASRV,
    AARCH64_OP_RORV,
    AARCH64_OP_MRS_TPIDR_EL0,
    AARCH64_OP_MSR_TPIDR_EL0,
    AARCH64_OP_MRS_DCZID_EL0,
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
    AARCH64_OP_ADVSIMD_DUP_ELEMENT,
    AARCH64_OP_ADVSIMD_DUP_GENERAL,
    AARCH64_OP_ADVSIMD_INS_ELEMENT,
    AARCH64_OP_ADVSIMD_INS_GENERAL,
    AARCH64_OP_ADVSIMD_SMOV,
    AARCH64_OP_ADVSIMD_UMOV,
    AARCH64_OP_ADVSIMD_ADD,
    AARCH64_OP_ADVSIMD_TBL,
    AARCH64_OP_ADVSIMD_TBX,
    AARCH64_OP_ADVSIMD_UZP1,
    AARCH64_OP_ADVSIMD_UZP2,
    AARCH64_OP_ADVSIMD_TRN1,
    AARCH64_OP_ADVSIMD_TRN2,
    AARCH64_OP_ADVSIMD_ZIP1,
    AARCH64_OP_ADVSIMD_ZIP2,
    AARCH64_OP_ADVSIMD_CMGT,
    AARCH64_OP_ADVSIMD_CMGE,
    AARCH64_OP_ADVSIMD_CMHI,
    AARCH64_OP_ADVSIMD_CMHS,
    AARCH64_OP_ADVSIMD_CMTST,
    AARCH64_OP_ADVSIMD_CMEQ,
    AARCH64_OP_ADVSIMD_AND,
    AARCH64_OP_ADVSIMD_BIC,
    AARCH64_OP_ADVSIMD_ORR,
    AARCH64_OP_ADVSIMD_ORN,
    AARCH64_OP_ADVSIMD_EOR,
    AARCH64_OP_ADVSIMD_BSL,
    AARCH64_OP_ADVSIMD_BIT,
    AARCH64_OP_ADVSIMD_BIF,
    AARCH64_OP_ADVSIMD_SMAXP,
    AARCH64_OP_ADVSIMD_SMINP,
    AARCH64_OP_ADVSIMD_UMAXP,
    AARCH64_OP_ADVSIMD_UMINP,
    AARCH64_OP_FMOV_GENERAL_FROM_SIMD,
    AARCH64_OP_FMOV_SIMD_FROM_GENERAL,
    AARCH64_OP_FMOV_GENERAL_FROM_SIMD_HIGH,
    AARCH64_OP_FMOV_SIMD_HIGH_FROM_GENERAL,
    AARCH64_OP_SCVTF_GENERAL,
    AARCH64_OP_UCVTF_GENERAL,
    AARCH64_OP_FADD_SCALAR,
    AARCH64_OP_FSUB_SCALAR,
    AARCH64_OP_FMUL_SCALAR,
    AARCH64_OP_FMOV_SCALAR,
    AARCH64_OP_FCMP_SCALAR,
    AARCH64_OP_FCMPE_SCALAR,
    AARCH64_OP_FCVTZS_SCALAR,
    AARCH64_OP_SCVTF_SCALAR,
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
            bool signed_load;
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
            byte_t rn;
            byte_t operand;
            byte_t condition;
            byte_t nzcv;
            bool immediate;
        } conditional_compare;
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
            byte_t rn;
            byte_t destination_width;
        } integer_to_fp;
        struct {
            byte_t rn;
            byte_t rm;
            bool zero;
        } scalar_fp_compare;
        struct {
            byte_t rd;
            qword_t immediate;
        } advsimd_immediate;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t element_size;
            byte_t destination_index;
            byte_t source_index;
        } advsimd_copy;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            byte_t element_size;
        } advsimd_three_same;
        struct {
            byte_t rd;
            byte_t rn;
            byte_t rm;
            byte_t table_count;
        } advsimd_table;
        struct {
            byte_t rs;
            byte_t rt;
            byte_t rn;
            byte_t size;
        } exclusive;
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

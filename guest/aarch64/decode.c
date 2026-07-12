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

struct aarch64_bit_masks {
    qword_t write;
    qword_t top;
};

static qword_t low_ones(unsigned count) {
    return count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
}

static qword_t rotate_right(qword_t value, unsigned amount,
        unsigned width) {
    qword_t mask = low_ones(width);
    value &= mask;
    if (amount == 0)
        return value;
    return ((value >> amount) | (value << (width - amount))) & mask;
}

static qword_t replicate_element(qword_t element, unsigned element_size,
        byte_t width) {
    qword_t result = 0;
    for (unsigned offset = 0; offset < width; offset += element_size)
        result |= element << offset;
    return result;
}

static bool decode_bit_masks(bool n, byte_t imms, byte_t immr,
        byte_t width, bool immediate, struct aarch64_bit_masks *masks) {

    unsigned pattern = ((unsigned) n << 6) |
            ((~(unsigned) imms) & UINT32_C(0x3f));
    // Arm 只为 2 到 64 位的元素定义逻辑立即数。
    if (pattern < 2)
        return false;
    unsigned length = 0;
    for (unsigned value = pattern; value > 1; value >>= 1)
        length++;

    unsigned element_size = 1U << length;
    if (element_size > width)
        return false;
    unsigned levels = element_size - 1;
    unsigned s = (unsigned) imms & levels;
    if (immediate && s == levels)
        return false;
    unsigned r = (unsigned) immr & levels;
    unsigned d = (s - r) & levels;

    qword_t write_element = rotate_right(low_ones(s + 1), r,
            element_size);
    qword_t top_element = low_ones(d + 1);
    masks->write = replicate_element(write_element, element_size, width);
    masks->top = replicate_element(top_element, element_size, width);
    return true;
}

static bool decode_scalar_transfer(byte_t size, byte_t operation,
        bool *load, bool *signed_load, byte_t *register_width) {
    if (operation >= 2 && (size == 8 || (size == 4 && operation == 3)))
        return false;
    *load = operation != 0;
    *signed_load = operation >= 2;
    *register_width = operation == 2 || size == 8 ? 64 : 32;
    return true;
}

static bool decode_simd_transfer(byte_t size_field, byte_t operation,
        bool *load, byte_t *size) {
    bool quadword = (operation & 2) != 0;
    if (quadword && size_field != 0)
        return false;
    byte_t scale = quadword ? 4 : size_field;
    *load = (operation & 1) != 0;
    *size = (byte_t) (1U << scale);
    return true;
}

bool aarch64_decode(dword_t word, struct aarch64_decoded *decoded) {
    if (word == UINT32_C(0xd503201f)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_NOP,
            .width = 64,
        };
        return true;
    }

    dword_t barrier = word & UINT32_C(0xfffff0ff);
    if (barrier == UINT32_C(0xd503305f) ||
            barrier == UINT32_C(0xd503309f) ||
            barrier == UINT32_C(0xd50330bf) ||
            barrier == UINT32_C(0xd50330df)) {
        enum aarch64_opcode opcode;
        if (barrier == UINT32_C(0xd503305f))
            opcode = AARCH64_OP_CLREX;
        else if (barrier == UINT32_C(0xd503309f))
            opcode = AARCH64_OP_DSB;
        else if (barrier == UINT32_C(0xd50330bf))
            opcode = AARCH64_OP_DMB;
        else
            opcode = AARCH64_OP_ISB;
        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
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

    dword_t system_register = word & UINT32_C(0xffffffe0);
    if (system_register == UINT32_C(0xd53b00e0)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_MRS_DCZID_EL0,
            .width = 64,
            .operands.system_register.rt = word & 0x1f,
        };
        return true;
    }
    if (system_register == UINT32_C(0xd53bd040) ||
            system_register == UINT32_C(0xd51bd040)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = system_register == UINT32_C(0xd53bd040) ?
                    AARCH64_OP_MRS_TPIDR_EL0 : AARCH64_OP_MSR_TPIDR_EL0,
            .width = 64,
            .operands.system_register.rt = word & 0x1f,
        };
        return true;
    }
    if (system_register == UINT32_C(0xd53b4400) ||
            system_register == UINT32_C(0xd51b4400)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = system_register == UINT32_C(0xd53b4400) ?
                    AARCH64_OP_MRS_FPCR : AARCH64_OP_MSR_FPCR,
            .width = 64,
            .operands.system_register.rt = word & 0x1f,
        };
        return true;
    }
    if (system_register == UINT32_C(0xd53b4420) ||
            system_register == UINT32_C(0xd51b4420)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = system_register == UINT32_C(0xd53b4420) ?
                    AARCH64_OP_MRS_FPSR : AARCH64_OP_MSR_FPSR,
            .width = 64,
            .operands.system_register.rt = word & 0x1f,
        };
        return true;
    }

    static const struct {
        dword_t bits;
        enum aarch64_opcode opcode;
        byte_t width;
    } fmov_transfers[] = {
        {UINT32_C(0x1ee60000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 16},
        {UINT32_C(0x1e260000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 32},
        {UINT32_C(0x9e660000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 64},
        {UINT32_C(0x9eae0000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD_HIGH, 64},
        {UINT32_C(0x1ee70000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 16},
        {UINT32_C(0x1e270000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 32},
        {UINT32_C(0x9e670000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 64},
        {UINT32_C(0x9eaf0000), AARCH64_OP_FMOV_SIMD_HIGH_FROM_GENERAL, 64},
        {UINT32_C(0x9ee60000), AARCH64_OP_FMOV_GENERAL_FROM_SIMD, 16},
        {UINT32_C(0x9ee70000), AARCH64_OP_FMOV_SIMD_FROM_GENERAL, 16},
    };
    dword_t fmov_transfer = word & UINT32_C(0xfffffc00);
    for (unsigned i = 0;
            i < sizeof(fmov_transfers) / sizeof(fmov_transfers[0]); i++) {
        if (fmov_transfer != fmov_transfers[i].bits)
            continue;
        *decoded = (struct aarch64_decoded) {
            .opcode = fmov_transfers[i].opcode,
            .width = fmov_transfers[i].width,
            .operands.data_processing_1source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
            },
        };
        return true;
    }

    static const struct {
        dword_t bits;
        enum aarch64_opcode opcode;
        byte_t source_width;
        byte_t destination_width;
    } integer_to_fp_conversions[] = {
        {UINT32_C(0x1e220000), AARCH64_OP_SCVTF_GENERAL, 32, 32},
        {UINT32_C(0x1e620000), AARCH64_OP_SCVTF_GENERAL, 32, 64},
        {UINT32_C(0x9e220000), AARCH64_OP_SCVTF_GENERAL, 64, 32},
        {UINT32_C(0x9e620000), AARCH64_OP_SCVTF_GENERAL, 64, 64},
        {UINT32_C(0x1e230000), AARCH64_OP_UCVTF_GENERAL, 32, 32},
        {UINT32_C(0x1e630000), AARCH64_OP_UCVTF_GENERAL, 32, 64},
        {UINT32_C(0x9e230000), AARCH64_OP_UCVTF_GENERAL, 64, 32},
        {UINT32_C(0x9e630000), AARCH64_OP_UCVTF_GENERAL, 64, 64},
    };
    dword_t integer_to_fp = word & UINT32_C(0xfffffc00);
    for (unsigned i = 0; i < sizeof(integer_to_fp_conversions) /
            sizeof(integer_to_fp_conversions[0]); i++) {
        if (integer_to_fp != integer_to_fp_conversions[i].bits)
            continue;
        *decoded = (struct aarch64_decoded) {
            .opcode = integer_to_fp_conversions[i].opcode,
            .width = integer_to_fp_conversions[i].source_width,
            .operands.integer_to_fp = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .destination_width =
                        integer_to_fp_conversions[i].destination_width,
            },
        };
        return true;
    }

    static const struct {
        dword_t bits;
        enum aarch64_opcode opcode;
        byte_t width;
    } scalar_fp_binary_operations[] = {
        {UINT32_C(0x1e202800), AARCH64_OP_FADD_SCALAR, 32},
        {UINT32_C(0x1e602800), AARCH64_OP_FADD_SCALAR, 64},
        {UINT32_C(0x1e203800), AARCH64_OP_FSUB_SCALAR, 32},
        {UINT32_C(0x1e603800), AARCH64_OP_FSUB_SCALAR, 64},
        {UINT32_C(0x1e200800), AARCH64_OP_FMUL_SCALAR, 32},
        {UINT32_C(0x1e600800), AARCH64_OP_FMUL_SCALAR, 64},
    };
    dword_t scalar_fp_binary = word & UINT32_C(0xffe0fc00);
    for (unsigned i = 0; i < sizeof(scalar_fp_binary_operations) /
            sizeof(scalar_fp_binary_operations[0]); i++) {
        if (scalar_fp_binary != scalar_fp_binary_operations[i].bits)
            continue;
        *decoded = (struct aarch64_decoded) {
            .opcode = scalar_fp_binary_operations[i].opcode,
            .width = scalar_fp_binary_operations[i].width,
            .operands.data_processing_2source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
            },
        };
        return true;
    }

    static const struct {
        dword_t bits;
        enum aarch64_opcode opcode;
        byte_t width;
    } scalar_fp_unary_operations[] = {
        {UINT32_C(0x1e204000), AARCH64_OP_FMOV_SCALAR, 32},
        {UINT32_C(0x1e604000), AARCH64_OP_FMOV_SCALAR, 64},
        {UINT32_C(0x5ea1b800), AARCH64_OP_FCVTZS_SCALAR, 32},
        {UINT32_C(0x5ee1b800), AARCH64_OP_FCVTZS_SCALAR, 64},
        {UINT32_C(0x5e21d800), AARCH64_OP_SCVTF_SCALAR, 32},
        {UINT32_C(0x5e61d800), AARCH64_OP_SCVTF_SCALAR, 64},
    };
    dword_t scalar_fp_unary = word & UINT32_C(0xfffffc00);
    for (unsigned i = 0; i < sizeof(scalar_fp_unary_operations) /
            sizeof(scalar_fp_unary_operations[0]); i++) {
        if (scalar_fp_unary != scalar_fp_unary_operations[i].bits)
            continue;
        *decoded = (struct aarch64_decoded) {
            .opcode = scalar_fp_unary_operations[i].opcode,
            .width = scalar_fp_unary_operations[i].width,
            .operands.data_processing_1source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
            },
        };
        return true;
    }

    static const struct {
        dword_t bits;
        dword_t mask;
        enum aarch64_opcode opcode;
        byte_t width;
        bool zero;
    } scalar_fp_comparisons[] = {
        {UINT32_C(0x1e202000), UINT32_C(0xffe0fc1f),
                AARCH64_OP_FCMP_SCALAR, 32, false},
        {UINT32_C(0x1e602000), UINT32_C(0xffe0fc1f),
                AARCH64_OP_FCMP_SCALAR, 64, false},
        {UINT32_C(0x1e202010), UINT32_C(0xffe0fc1f),
                AARCH64_OP_FCMPE_SCALAR, 32, false},
        {UINT32_C(0x1e602010), UINT32_C(0xffe0fc1f),
                AARCH64_OP_FCMPE_SCALAR, 64, false},
        {UINT32_C(0x1e202008), UINT32_C(0xfffffc1f),
                AARCH64_OP_FCMP_SCALAR, 32, true},
        {UINT32_C(0x1e602008), UINT32_C(0xfffffc1f),
                AARCH64_OP_FCMP_SCALAR, 64, true},
        {UINT32_C(0x1e202018), UINT32_C(0xfffffc1f),
                AARCH64_OP_FCMPE_SCALAR, 32, true},
        {UINT32_C(0x1e602018), UINT32_C(0xfffffc1f),
                AARCH64_OP_FCMPE_SCALAR, 64, true},
    };
    for (unsigned i = 0; i < sizeof(scalar_fp_comparisons) /
            sizeof(scalar_fp_comparisons[0]); i++) {
        if ((word & scalar_fp_comparisons[i].mask) !=
                scalar_fp_comparisons[i].bits)
            continue;
        *decoded = (struct aarch64_decoded) {
            .opcode = scalar_fp_comparisons[i].opcode,
            .width = scalar_fp_comparisons[i].width,
            .operands.scalar_fp_compare = {
                .rn = (word >> 5) & 0x1f,
                .rm = scalar_fp_comparisons[i].zero ? 0 :
                        (word >> 16) & 0x1f,
                .zero = scalar_fp_comparisons[i].zero,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0xbf20fc00)) == UINT32_C(0x0e208400)) {
        bool q = ((word >> 30) & 1) != 0;
        byte_t size = (word >> 22) & 3;
        // 64 位向量不存在单个 64 位 lane 的 ADD arrangement。
        if (!q && size == 3)
            return false;
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_ADVSIMD_ADD,
            .width = q ? 128 : 64,
            .operands.advsimd_three_same = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .element_size = (byte_t) (1U << size),
            },
        };
        return true;
    }

    dword_t compare = word & UINT32_C(0x9f20fc00);
    if (compare == UINT32_C(0x0e203400) ||
            compare == UINT32_C(0x0e203c00) ||
            compare == UINT32_C(0x0e208c00)) {
        bool q = ((word >> 30) & 1) != 0;
        bool u = ((word >> 29) & 1) != 0;
        byte_t size = (word >> 22) & 3;
        if (!q && size == 3)
            return false;
        byte_t family = compare == UINT32_C(0x0e203400) ? 0 :
                compare == UINT32_C(0x0e203c00) ? 1 : 2;
        static const enum aarch64_opcode opcodes[3][2] = {
            {AARCH64_OP_ADVSIMD_CMGT, AARCH64_OP_ADVSIMD_CMHI},
            {AARCH64_OP_ADVSIMD_CMGE, AARCH64_OP_ADVSIMD_CMHS},
            {AARCH64_OP_ADVSIMD_CMTST, AARCH64_OP_ADVSIMD_CMEQ},
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[family][u],
            .width = q ? 128 : 64,
            .operands.advsimd_three_same = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .element_size = (byte_t) (1U << size),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x9f20fc00)) == UINT32_C(0x0e201c00)) {
        byte_t operation = (byte_t) ((((word >> 29) & 1) << 2) |
                ((word >> 22) & 3));
        static const enum aarch64_opcode opcodes[] = {
            AARCH64_OP_ADVSIMD_AND,
            AARCH64_OP_ADVSIMD_BIC,
            AARCH64_OP_ADVSIMD_ORR,
            AARCH64_OP_ADVSIMD_ORN,
            AARCH64_OP_ADVSIMD_EOR,
            AARCH64_OP_ADVSIMD_BSL,
            AARCH64_OP_ADVSIMD_BIT,
            AARCH64_OP_ADVSIMD_BIF,
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation],
            .width = (word >> 30) & 1 ? 128 : 64,
            .operands.advsimd_three_same = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .element_size = 1,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x9f20f400)) == UINT32_C(0x0e20a400)) {
        bool u = ((word >> 29) & 1) != 0;
        bool minimum = ((word >> 11) & 1) != 0;
        byte_t size = (word >> 22) & 3;
        if (size == 3)
            return false;
        static const enum aarch64_opcode opcodes[2][2] = {
            {AARCH64_OP_ADVSIMD_SMAXP, AARCH64_OP_ADVSIMD_UMAXP},
            {AARCH64_OP_ADVSIMD_SMINP, AARCH64_OP_ADVSIMD_UMINP},
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[minimum][u],
            .width = (word >> 30) & 1 ? 128 : 64,
            .operands.advsimd_three_same = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .element_size = (byte_t) (1U << size),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0xbf208c00)) == UINT32_C(0x0e000800)) {
        bool q = ((word >> 30) & 1) != 0;
        byte_t size = (word >> 22) & 3;
        byte_t operation = (word >> 12) & 7;
        if ((!q && size == 3) || operation == 0 || operation == 4)
            return false;
        static const enum aarch64_opcode opcodes[2][3] = {
            {AARCH64_OP_ADVSIMD_UZP1, AARCH64_OP_ADVSIMD_TRN1,
                    AARCH64_OP_ADVSIMD_ZIP1},
            {AARCH64_OP_ADVSIMD_UZP2, AARCH64_OP_ADVSIMD_TRN2,
                    AARCH64_OP_ADVSIMD_ZIP2},
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation >> 2][(operation & 3) - 1],
            .width = q ? 128 : 64,
            .operands.advsimd_three_same = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .element_size = (byte_t) (1U << size),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0xbfe08c00)) == UINT32_C(0x0e000000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 12) & 1 ?
                    AARCH64_OP_ADVSIMD_TBX : AARCH64_OP_ADVSIMD_TBL,
            .width = (word >> 30) & 1 ? 128 : 64,
            .operands.advsimd_table = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .table_count = (byte_t) (((word >> 13) & 3) + 1),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x9fe08400)) == UINT32_C(0x0e000400)) {
        bool q = ((word >> 30) & 1) != 0;
        bool op = ((word >> 29) & 1) != 0;
        byte_t imm5 = (word >> 16) & 0x1f;
        byte_t imm4 = (word >> 11) & 0xf;
        if ((imm5 & 0xf) == 0)
            return false;

        byte_t size_shift = 0;
        while ((imm5 & (1U << size_shift)) == 0)
            size_shift++;
        byte_t element_size = (byte_t) (1U << size_shift);
        byte_t index = imm5 >> (size_shift + 1);
        enum aarch64_opcode opcode;
        byte_t width;

        if (op) {
            if (!q)
                return false;
            opcode = AARCH64_OP_ADVSIMD_INS_ELEMENT;
            width = 128;
        } else if (imm4 == 0 || imm4 == 1) {
            if (!q && element_size == 8)
                return false;
            opcode = imm4 == 0 ? AARCH64_OP_ADVSIMD_DUP_ELEMENT :
                    AARCH64_OP_ADVSIMD_DUP_GENERAL;
            width = q ? 128 : 64;
        } else if (imm4 == 3) {
            if (!q)
                return false;
            opcode = AARCH64_OP_ADVSIMD_INS_GENERAL;
            width = 128;
        } else if (imm4 == 5) {
            if ((!q && element_size > 2) || (q && element_size > 4))
                return false;
            opcode = AARCH64_OP_ADVSIMD_SMOV;
            width = q ? 64 : 32;
        } else if (imm4 == 7) {
            if ((!q && element_size > 4) || (q && element_size != 8))
                return false;
            opcode = AARCH64_OP_ADVSIMD_UMOV;
            width = q ? 64 : 32;
        } else {
            return false;
        }

        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = width,
            .operands.advsimd_copy = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .element_size = element_size,
                .destination_index = index,
                .source_index = op ? imm4 >> size_shift : index,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x9ff80c00)) == UINT32_C(0x0f000400)) {
        bool op = (word >> 29) & 1;
        byte_t cmode = (word >> 12) & UINT32_C(0xf);
        if (cmode == 15)
            return false;
        byte_t imm8 = (byte_t) ((((word >> 16) & 7) << 5) |
                ((word >> 5) & UINT32_C(0x1f)));
        qword_t immediate;

        if (cmode < 8) {
            dword_t element = (dword_t) imm8 << ((cmode >> 1) * 8);
            immediate = replicate_element(element, 32, 64);
        } else if (cmode < 12) {
            word_t element = (word_t) ((dword_t) imm8 <<
                    (((cmode >> 1) & 1) * 8));
            immediate = replicate_element(element, 16, 64);
        } else if (cmode == 12) {
            dword_t element = ((dword_t) imm8 << 8) | UINT32_C(0xff);
            immediate = replicate_element(element, 32, 64);
        } else if (cmode == 13) {
            dword_t element = ((dword_t) imm8 << 16) | UINT32_C(0xffff);
            immediate = replicate_element(element, 32, 64);
        } else if (!op) {
            immediate = replicate_element(imm8, 8, 64);
        } else {
            immediate = 0;
            for (byte_t byte = 0; byte < 8; byte++) {
                if (imm8 & (UINT32_C(1) << byte))
                    immediate |= UINT64_C(0xff) << (byte * 8);
            }
        }

        enum aarch64_opcode opcode;
        // 1110/op=1 是字节掩码 MOVI，1101 则仍属于 MSL MOVI/MVNI。
        if (cmode == 14) {
            opcode = AARCH64_OP_ADVSIMD_MOVI;
        } else if ((cmode & 1) == 0 || cmode == 13) {
            opcode = op ? AARCH64_OP_ADVSIMD_MVNI :
                    AARCH64_OP_ADVSIMD_MOVI;
        } else {
            opcode = op ? AARCH64_OP_ADVSIMD_BIC_IMMEDIATE :
                    AARCH64_OP_ADVSIMD_ORR_IMMEDIATE;
        }
        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = (word >> 30) & 1 ? 128 : 64,
            .operands.advsimd_immediate = {
                .rd = word & 0x1f,
                .immediate = immediate,
            },
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

    if ((word & UINT32_C(0x1fe00000)) == UINT32_C(0x0b200000)) {
        byte_t operation = (word >> 29) & 3;
        byte_t shift = (word >> 10) & 7;
        if (shift > 4)
            return false;
        static const enum aarch64_opcode opcodes[] = {
            AARCH64_OP_ADD_EXTENDED_REGISTER,
            AARCH64_OP_ADDS_EXTENDED_REGISTER,
            AARCH64_OP_SUB_EXTENDED_REGISTER,
            AARCH64_OP_SUBS_EXTENDED_REGISTER,
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation],
            .width = (word >> 31) ? 64 : 32,
            .operands.add_sub_extended = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .extend_type = (enum aarch64_extend_type)
                        ((word >> 13) & 7),
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

    if ((word & UINT32_C(0x1f800000)) == UINT32_C(0x12000000)) {
        bool is_64 = word >> 31;
        byte_t operation = (word >> 29) & 3;
        struct aarch64_bit_masks masks;
        if (!decode_bit_masks((word >> 22) & 1,
                (word >> 10) & UINT32_C(0x3f),
                (word >> 16) & UINT32_C(0x3f),
                is_64 ? 64 : 32, true, &masks))
            return false;
        static const enum aarch64_opcode opcodes[] = {
            AARCH64_OP_AND_IMMEDIATE,
            AARCH64_OP_ORR_IMMEDIATE,
            AARCH64_OP_EOR_IMMEDIATE,
            AARCH64_OP_ANDS_IMMEDIATE,
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation],
            .width = is_64 ? 64 : 32,
            .operands.logical_immediate = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .immediate = masks.write,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x1f800000)) == UINT32_C(0x13000000)) {
        bool is_64 = word >> 31;
        bool n = (word >> 22) & 1;
        byte_t operation = (word >> 29) & 3;
        byte_t immr = (word >> 16) & UINT32_C(0x3f);
        byte_t imms = (word >> 10) & UINT32_C(0x3f);
        if (operation == 3 || n != is_64 ||
                (!is_64 && ((immr | imms) & UINT32_C(0x20))))
            return false;

        struct aarch64_bit_masks masks;
        if (!decode_bit_masks(n, imms, immr, is_64 ? 64 : 32,
                false, &masks))
            return false;
        static const enum aarch64_opcode opcodes[] = {
            AARCH64_OP_SBFM,
            AARCH64_OP_BFM,
            AARCH64_OP_UBFM,
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation],
            .width = is_64 ? 64 : 32,
            .operands.bitfield_move = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .immr = immr,
                .imms = imms,
                .write_mask = masks.write,
                .top_mask = masks.top,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x7fa00000)) == UINT32_C(0x13800000)) {
        bool is_64 = word >> 31;
        bool n = (word >> 22) & 1;
        byte_t lsb = (word >> 10) & UINT32_C(0x3f);
        if (n != is_64 || (!is_64 && lsb >= 32))
            return false;
        *decoded = (struct aarch64_decoded) {
            .opcode = AARCH64_OP_EXTR,
            .width = is_64 ? 64 : 32,
            .operands.extract = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .lsb = lsb,
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

    if ((word & UINT32_C(0x3fe00410)) == UINT32_C(0x3a400000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 30) & 1 ?
                    AARCH64_OP_CCMP : AARCH64_OP_CCMN,
            .width = (word >> 31) ? 64 : 32,
            .operands.conditional_compare = {
                .rn = (word >> 5) & 0x1f,
                .operand = (word >> 16) & 0x1f,
                .condition = (word >> 12) & 0xf,
                .nzcv = word & 0xf,
                .immediate = ((word >> 11) & 1) != 0,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3fe00800)) == UINT32_C(0x1a800000)) {
        byte_t operation = (byte_t) ((((word >> 30) & 1) << 1) |
                ((word >> 10) & 1));
        static const enum aarch64_opcode opcodes[] = {
            AARCH64_OP_CSEL,
            AARCH64_OP_CSINC,
            AARCH64_OP_CSINV,
            AARCH64_OP_CSNEG,
        };
        *decoded = (struct aarch64_decoded) {
            .opcode = opcodes[operation],
            .width = (word >> 31) ? 64 : 32,
            .operands.conditional_select = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .condition = (word >> 12) & 0xf,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x7fff0000)) == UINT32_C(0x5ac00000)) {
        bool is_64 = word >> 31;
        byte_t operation = (word >> 10) & UINT32_C(0x3f);
        enum aarch64_opcode opcode;
        switch (operation) {
            case 0:
                opcode = AARCH64_OP_RBIT;
                break;
            case 1:
                opcode = AARCH64_OP_REV16;
                break;
            case 2:
                opcode = AARCH64_OP_REV32;
                break;
            case 3:
                if (!is_64)
                    return false;
                opcode = AARCH64_OP_REV64;
                break;
            case 4:
                opcode = AARCH64_OP_CLZ;
                break;
            case 5:
                opcode = AARCH64_OP_CLS;
                break;
            default:
                return false;
        }
        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = is_64 ? 64 : 32,
            .operands.data_processing_1source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
            },
        };
        return true;
    }

    dword_t data_processing_2source = word & UINT32_C(0x7fe0fc00);
    if (data_processing_2source == UINT32_C(0x1ac00800) ||
            data_processing_2source == UINT32_C(0x1ac00c00) ||
            data_processing_2source == UINT32_C(0x1ac02000) ||
            data_processing_2source == UINT32_C(0x1ac02400) ||
            data_processing_2source == UINT32_C(0x1ac02800) ||
            data_processing_2source == UINT32_C(0x1ac02c00)) {
        enum aarch64_opcode opcode;
        switch ((word >> 10) & UINT32_C(0x3f)) {
            case 2:
                opcode = AARCH64_OP_UDIV;
                break;
            case 3:
                opcode = AARCH64_OP_SDIV;
                break;
            case 8:
                opcode = AARCH64_OP_LSLV;
                break;
            case 9:
                opcode = AARCH64_OP_LSRV;
                break;
            case 10:
                opcode = AARCH64_OP_ASRV;
                break;
            default:
                opcode = AARCH64_OP_RORV;
                break;
        }
        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = (word >> 31) ? 64 : 32,
            .operands.data_processing_2source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x7fe00000)) == UINT32_C(0x1b000000)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 15) & 1 ? AARCH64_OP_MSUB : AARCH64_OP_MADD,
            .width = (word >> 31) ? 64 : 32,
            .operands.data_processing_3source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .ra = (word >> 10) & 0x1f,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0xff600000)) == UINT32_C(0x9b200000)) {
        bool unsigned_multiply = (word >> 23) & 1;
        bool subtract = (word >> 15) & 1;
        enum aarch64_opcode opcode;
        if (unsigned_multiply)
            opcode = subtract ? AARCH64_OP_UMSUBL : AARCH64_OP_UMADDL;
        else
            opcode = subtract ? AARCH64_OP_SMSUBL : AARCH64_OP_SMADDL;
        *decoded = (struct aarch64_decoded) {
            .opcode = opcode,
            .width = 64,
            .operands.data_processing_3source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .ra = (word >> 10) & 0x1f,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0xff60fc00)) == UINT32_C(0x9b407c00)) {
        *decoded = (struct aarch64_decoded) {
            .opcode = (word >> 23) & 1 ? AARCH64_OP_UMULH :
                    AARCH64_OP_SMULH,
            .width = 64,
            .operands.data_processing_3source = {
                .rd = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .ra = 31,
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

    dword_t exclusive_load = word & UINT32_C(0x3ffffc00);
    if (exclusive_load == UINT32_C(0x085f7c00) ||
            exclusive_load == UINT32_C(0x085ffc00)) {
        byte_t size_shift = word >> 30;
        *decoded = (struct aarch64_decoded) {
            .opcode = exclusive_load == UINT32_C(0x085ffc00) ?
                    AARCH64_OP_LDAXR : AARCH64_OP_LDXR,
            .width = size_shift == 3 ? 64 : 32,
            .operands.exclusive = {
                .rs = 31,
                .rt = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .size = (byte_t) (1U << size_shift),
            },
        };
        return true;
    }

    dword_t exclusive_store = word & UINT32_C(0x3fe0fc00);
    if (exclusive_store == UINT32_C(0x08007c00) ||
            exclusive_store == UINT32_C(0x0800fc00)) {
        byte_t rs = (word >> 16) & 0x1f;
        byte_t rt = word & 0x1f;
        byte_t rn = (word >> 5) & 0x1f;
        if (rs == rt || (rn != 31 && rs == rn))
            return false;
        byte_t size_shift = word >> 30;
        *decoded = (struct aarch64_decoded) {
            .opcode = exclusive_store == UINT32_C(0x0800fc00) ?
                    AARCH64_OP_STLXR : AARCH64_OP_STXR,
            .width = size_shift == 3 ? 64 : 32,
            .operands.exclusive = {
                .rs = rs,
                .rt = rt,
                .rn = rn,
                .size = (byte_t) (1U << size_shift),
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3e000000)) == UINT32_C(0x2c000000)) {
        byte_t operation = word >> 30;
        if (operation == 3)
            return false;
        byte_t mode = (word >> 23) & 3;
        bool load = (word >> 22) & 1;
        byte_t rt = word & 0x1f;
        byte_t rt2 = (word >> 10) & 0x1f;
        if (load && rt == rt2)
            return false;

        byte_t size = (byte_t) (1U << (operation + 2));
        enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
        if (mode == 1)
            address_mode = AARCH64_ADDRESS_POST_INDEX;
        else if (mode == 3)
            address_mode = AARCH64_ADDRESS_PRE_INDEX;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_SIMD_PAIR :
                    AARCH64_OP_STORE_SIMD_PAIR,
            .width = (byte_t) (size * 8),
            .operands.load_store_pair = {
                .rt = rt,
                .rt2 = rt2,
                .rn = (word >> 5) & 0x1f,
                .offset = sign_extend((word >> 15) &
                        UINT32_C(0x7f), 7) * (int64_t) size,
                .address_mode = address_mode,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3a000000)) == UINT32_C(0x28000000)) {
        byte_t operation = word >> 30;
        bool vector = (word >> 26) & 1;
        byte_t mode = (word >> 23) & 3;
        bool load = (word >> 22) & 1;
        bool signed_load = operation == 1;
        if (vector || operation == 3 || mode == 0 ||
                (signed_load && !load))
            return false;

        byte_t rt = word & 0x1f;
        byte_t rt2 = (word >> 10) & 0x1f;
        byte_t rn = (word >> 5) & 0x1f;
        bool writeback = mode != 2;
        // 这些重叠形式由架构留给实现选择，本解释器统一拒绝。
        if ((load && rt == rt2) || (writeback && rn != 31 &&
                (rn == rt || rn == rt2)))
            return false;

        byte_t size = operation == 2 ? 8 : 4;
        enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
        if (mode == 1)
            address_mode = AARCH64_ADDRESS_POST_INDEX;
        else if (mode == 3)
            address_mode = AARCH64_ADDRESS_PRE_INDEX;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_PAIR : AARCH64_OP_STORE_PAIR,
            .width = signed_load ? 64 : (byte_t) (size * 8),
            .operands.load_store_pair = {
                .rt = rt,
                .rt2 = rt2,
                .rn = rn,
                .offset = sign_extend((word >> 15) &
                        UINT32_C(0x7f), 7) * (int64_t) size,
                .address_mode = address_mode,
                .signed_load = signed_load,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3f200000)) == UINT32_C(0x3c000000)) {
        byte_t size_field = word >> 30;
        byte_t operation = (word >> 22) & 3;
        byte_t mode = (word >> 10) & 3;
        bool load;
        byte_t size;
        if (mode == 2 || !decode_simd_transfer(
                size_field, operation, &load, &size))
            return false;

        enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
        if (mode == 1)
            address_mode = AARCH64_ADDRESS_POST_INDEX;
        else if (mode == 3)
            address_mode = AARCH64_ADDRESS_PRE_INDEX;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_SIMD_IMM9 :
                    AARCH64_OP_STORE_SIMD_IMM9,
            .width = (byte_t) (size * 8),
            .operands.load_store = {
                .rt = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .size = size,
                .offset = sign_extend((word >> 12) &
                        UINT32_C(0x1ff), 9),
                .address_mode = address_mode,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3f000000)) == UINT32_C(0x3d000000)) {
        byte_t size_field = word >> 30;
        byte_t operation = (word >> 22) & 3;
        bool load;
        byte_t size;
        if (!decode_simd_transfer(size_field, operation, &load, &size))
            return false;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_SIMD_IMM12 :
                    AARCH64_OP_STORE_SIMD_IMM12,
            .width = (byte_t) (size * 8),
            .operands.load_store = {
                .rt = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .size = size,
                .offset = (int64_t) (((word >> 10) &
                        UINT32_C(0xfff)) * (dword_t) size),
                .address_mode = AARCH64_ADDRESS_OFFSET,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3f200c00)) == UINT32_C(0x38200800)) {
        byte_t operation = (word >> 22) & 3;
        byte_t size_shift = word >> 30;
        byte_t size = (byte_t) (1 << size_shift);
        byte_t extend_type = (word >> 13) & 7;
        bool load;
        bool signed_load;
        byte_t register_width;
        if ((extend_type & 2) == 0 || !decode_scalar_transfer(
                size, operation, &load, &signed_load, &register_width))
            return false;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_REGISTER_OFFSET :
                    AARCH64_OP_STORE_REGISTER_OFFSET,
            .width = register_width,
            .operands.load_store = {
                .rt = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .rm = (word >> 16) & 0x1f,
                .size = size,
                .extend_type = (enum aarch64_extend_type) extend_type,
                .shift = (word >> 12) & 1 ? size_shift : 0,
                .address_mode = AARCH64_ADDRESS_OFFSET,
                .signed_load = signed_load,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3b200000)) == UINT32_C(0x38000000)) {
        byte_t operation = (word >> 22) & 3;
        bool vector = (word >> 26) & 1;
        byte_t mode = (word >> 10) & 3;
        byte_t size_shift = word >> 30;
        byte_t size = (byte_t) (1 << size_shift);
        bool load;
        bool signed_load;
        byte_t register_width;
        if (vector || mode == 2 || !decode_scalar_transfer(size, operation,
                &load, &signed_load, &register_width))
            return false;
        byte_t rn = (word >> 5) & 0x1f;
        byte_t rt = word & 0x1f;
        // 写回与数据寄存器重叠时，架构不保证可移植的执行结果。
        if (mode != 0 && rn == rt && rn != 31)
            return false;

        enum aarch64_address_mode address_mode = AARCH64_ADDRESS_OFFSET;
        if (mode == 1)
            address_mode = AARCH64_ADDRESS_POST_INDEX;
        else if (mode == 3)
            address_mode = AARCH64_ADDRESS_PRE_INDEX;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_IMM9 : AARCH64_OP_STORE_IMM9,
            .width = register_width,
            .operands.load_store = {
                .rt = rt,
                .rn = rn,
                .size = size,
                .offset = sign_extend((word >> 12) & UINT32_C(0x1ff), 9),
                .address_mode = address_mode,
                .signed_load = signed_load,
            },
        };
        return true;
    }

    if ((word & UINT32_C(0x3b000000)) == UINT32_C(0x39000000)) {
        byte_t operation = (word >> 22) & 3;
        bool vector = (word >> 26) & 1;
        byte_t size_shift = word >> 30;
        byte_t size = (byte_t) (1 << size_shift);
        bool load;
        bool signed_load;
        byte_t register_width;
        if (vector || !decode_scalar_transfer(size, operation,
                &load, &signed_load, &register_width))
            return false;
        *decoded = (struct aarch64_decoded) {
            .opcode = load ? AARCH64_OP_LOAD_IMM12 : AARCH64_OP_STORE_IMM12,
            .width = register_width,
            .operands.load_store = {
                .rt = word & 0x1f,
                .rn = (word >> 5) & 0x1f,
                .size = size,
                .offset = (int64_t) ((qword_t) ((word >> 10) &
                        UINT32_C(0xfff)) << size_shift),
                .address_mode = AARCH64_ADDRESS_OFFSET,
                .signed_load = signed_load,
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

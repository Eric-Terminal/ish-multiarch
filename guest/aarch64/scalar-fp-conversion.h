#ifndef GUEST_AARCH64_SCALAR_FP_CONVERSION_H
#define GUEST_AARCH64_SCALAR_FP_CONVERSION_H

#include "guest/aarch64/scalar-fp.h"

// 前四项与 FPCR.RMode 一致；ties-away 只由显式指令选择。
enum aarch64_fp_rounding {
    AARCH64_FP_NEAREST_EVEN,
    AARCH64_FP_PLUS_INFINITY,
    AARCH64_FP_MINUS_INFINITY,
    AARCH64_FP_ZERO,
    AARCH64_FP_TIES_AWAY,
};

static inline bool aarch64_fp_round_increment(enum aarch64_fp_rounding mode,
        bool negative, bool odd, int compare_half) {
    switch (mode) {
        case AARCH64_FP_NEAREST_EVEN:
            return compare_half > 0 || (compare_half == 0 && odd);
        case AARCH64_FP_PLUS_INFINITY: return !negative;
        case AARCH64_FP_MINUS_INFINITY: return negative;
        case AARCH64_FP_TIES_AWAY: return compare_half >= 0;
        case AARCH64_FP_ZERO: return false;
    }
    return false;
}

static inline struct aarch64_scalar_fp_result
aarch64_scalar_fp_round_to_integral(qword_t bits, byte_t width,
        dword_t fpcr, enum aarch64_fp_rounding mode, bool exact) {
    const struct aarch64_scalar_fp_format format = aarch64_scalar_fp_format(width);
    struct aarch64_scalar_fp_number number = aarch64_scalar_fp_unpack(bits, &format);
    dword_t exceptions = 0;
    aarch64_scalar_fp_flush_input(&number, &format, fpcr, &exceptions);
    struct aarch64_scalar_fp_result result = {
        .bits = number.bits, .exceptions = exceptions,
    };
    if (aarch64_scalar_fp_is_nan(&number, &format)) {
        result = aarch64_scalar_fp_propagate_nan(&number, &number, &format, fpcr);
        result.exceptions |= exceptions;
        return result;
    }
    int exponent = (int) number.exponent - format.exponent_bias;
    if (aarch64_scalar_fp_is_zero(&number) ||
            exponent >= (int) format.fraction_bits)
        return result;

    qword_t magnitude = number.bits & ~format.sign_mask;
    qword_t rounded;
    if (exponent < 0) {
        qword_t half = (qword_t) (format.exponent_bias - 1) << format.fraction_bits;
        int compare_half = magnitude < half ? -1 : magnitude > half ? 1 : 0;
        rounded = aarch64_fp_round_increment(mode, number.sign, false, compare_half) ?
                (qword_t) format.exponent_bias << format.fraction_bits : 0;
    } else {
        unsigned discarded = format.fraction_bits - (unsigned) exponent;
        qword_t unit = UINT64_C(1) << discarded;
        qword_t remainder = magnitude & (unit - 1);
        if (remainder == 0)
            return result;
        qword_t half = unit >> 1;
        int compare_half = remainder < half ? -1 : remainder > half ? 1 : 0;
        // exponent=0 时整数位来自隐含的最高位，不能从指数编码判断奇偶。
        bool odd = exponent == 0 || (magnitude & unit) != 0;
        rounded = magnitude & ~(unit - 1);
        if (aarch64_fp_round_increment(mode, number.sign, odd, compare_half))
            rounded += unit;
    }
    result.bits = rounded | (number.sign ? format.sign_mask : 0);
    // FRINTX 与整数转换累积 IXC；其他 FRINT 变体只报告输入异常。
    if (exact)
        result.exceptions |= AARCH64_FPSR_IXC;
    return result;
}

static inline struct aarch64_scalar_fp_result
aarch64_scalar_fp_to_integer(qword_t source, byte_t source_width,
        byte_t destination_width, dword_t fpcr, bool is_signed,
        enum aarch64_fp_rounding mode) {
    // 有小数部分的有限浮点数，其舍入后的整数仍可由同一浮点格式精确表示。
    // 先统一舍入再检查目标范围，避免各变体在负数和饱和边界上分叉。
    struct aarch64_scalar_fp_result rounded = aarch64_scalar_fp_round_to_integral(
            source, source_width, fpcr, mode, true);
    const struct aarch64_scalar_fp_format format = aarch64_scalar_fp_format(source_width);
    struct aarch64_scalar_fp_number number = aarch64_scalar_fp_unpack(rounded.bits, &format);
    qword_t sign = UINT64_C(1) << (destination_width - 1);
    qword_t limit = is_signed ? (number.sign ? sign : sign - 1) :
            (number.sign ? 0 : (destination_width == 32 ? UINT32_MAX : UINT64_MAX));
    qword_t value = 0;
    bool invalid = false;
    if (aarch64_scalar_fp_is_nan(&number, &format)) {
        invalid = true;
    } else if (!aarch64_scalar_fp_is_zero(&number)) {
        int exponent = (int) number.exponent - format.exponent_bias;
        if (exponent >= destination_width) {
            value = limit;
            invalid = true;
        } else {
            qword_t significand = (UINT64_C(1) << format.fraction_bits) | number.fraction;
            qword_t magnitude = exponent >= (int) format.fraction_bits ?
                    significand << (exponent - (int) format.fraction_bits) :
                    significand >> ((int) format.fraction_bits - exponent);
            invalid = magnitude > limit;
            value = invalid ? limit : number.sign ? 0 - magnitude : magnitude;
        }
    }
    // 无效转换优先于此次舍入的 IXC；调用者仍会保留已有的累积异常。
    if (invalid)
        rounded.exceptions = (rounded.exceptions & ~AARCH64_FPSR_IXC) | AARCH64_FPSR_IOC;
    return (struct aarch64_scalar_fp_result) {
        .bits = destination_width == 32 ? (dword_t) value : value,
        .exceptions = rounded.exceptions,
    };
}

#endif

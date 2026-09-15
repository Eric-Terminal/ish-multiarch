#ifndef GUEST_AARCH64_SCALAR_FP_MINMAX_H
#define GUEST_AARCH64_SCALAR_FP_MINMAX_H

#include "guest/aarch64/scalar-fp.h"

static inline struct aarch64_scalar_fp_result
aarch64_scalar_fp_minmax(qword_t left_bits, qword_t right_bits, byte_t width,
        dword_t fpcr, bool maximum, bool numeric) {
    const struct aarch64_scalar_fp_format format = aarch64_scalar_fp_format(width);
    struct aarch64_scalar_fp_number left = aarch64_scalar_fp_unpack(left_bits, &format);
    struct aarch64_scalar_fp_number right = aarch64_scalar_fp_unpack(right_bits, &format);
    dword_t exceptions = 0;
    aarch64_scalar_fp_flush_input(&left, &format, fpcr, &exceptions);
    aarch64_scalar_fp_flush_input(&right, &format, fpcr, &exceptions);
    bool left_nan = aarch64_scalar_fp_is_nan(&left, &format);
    bool right_nan = aarch64_scalar_fp_is_nan(&right, &format);
    if (left_nan || right_nan) {
        // NM 只忽略与数值配对的 quiet NaN；signaling NaN 必须继续传播。
        if (numeric && left_nan != right_nan &&
                !aarch64_scalar_fp_is_signaling_nan(&left, &format) &&
                !aarch64_scalar_fp_is_signaling_nan(&right, &format)) {
            return (struct aarch64_scalar_fp_result) {
                .bits = left_nan ? right.bits : left.bits,
                .exceptions = exceptions,
            };
        }
        struct aarch64_scalar_fp_result result =
                aarch64_scalar_fp_propagate_nan(&left, &right, &format, fpcr);
        result.exceptions |= exceptions;
        return result;
    }
    qword_t bits;
    if (aarch64_scalar_fp_is_zero(&left) && aarch64_scalar_fp_is_zero(&right)) {
        bits = maximum ? left.bits & right.bits : left.bits | right.bits;
    } else {
        bool less = left.sign != right.sign ? left.sign :
                left.sign ? left.bits > right.bits : left.bits < right.bits;
        bits = less != maximum ? left.bits : right.bits;
    }
    return (struct aarch64_scalar_fp_result) {.bits = bits, .exceptions = exceptions};
}

#endif

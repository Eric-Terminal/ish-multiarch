#ifndef GUEST_AARCH64_SCALAR_FP_H
#define GUEST_AARCH64_SCALAR_FP_H

#include "guest/aarch64/cpu.h"

struct aarch64_scalar_fp_result {
    qword_t bits;
    dword_t exceptions;
};

struct aarch64_scalar_fp_compare_result {
    dword_t nzcv;
    dword_t exceptions;
};

struct aarch64_scalar_fp_format {
    qword_t value_mask;
    qword_t sign_mask;
    qword_t fraction_mask;
    qword_t exponent_mask;
    qword_t quiet_bit;
    qword_t default_nan;
    unsigned fraction_bits;
    unsigned exponent_max;
    int exponent_bias;
};

struct aarch64_scalar_fp_number {
    qword_t bits;
    qword_t fraction;
    unsigned exponent;
    bool sign;
};

static inline struct aarch64_scalar_fp_format
aarch64_scalar_fp_format(byte_t width) {
    if (width == 32) {
        return (struct aarch64_scalar_fp_format) {
            .value_mask = UINT64_C(0xffffffff),
            .sign_mask = UINT64_C(0x80000000),
            .fraction_mask = UINT64_C(0x007fffff),
            .exponent_mask = UINT64_C(0x7f800000),
            .quiet_bit = UINT64_C(0x00400000),
            .default_nan = UINT64_C(0x7fc00000),
            .fraction_bits = 23,
            .exponent_max = 0xff,
            .exponent_bias = 127,
        };
    }
    return (struct aarch64_scalar_fp_format) {
        .value_mask = UINT64_MAX,
        .sign_mask = UINT64_C(0x8000000000000000),
        .fraction_mask = UINT64_C(0x000fffffffffffff),
        .exponent_mask = UINT64_C(0x7ff0000000000000),
        .quiet_bit = UINT64_C(0x0008000000000000),
        .default_nan = UINT64_C(0x7ff8000000000000),
        .fraction_bits = 52,
        .exponent_max = 0x7ff,
        .exponent_bias = 1023,
    };
}

static inline struct aarch64_scalar_fp_number
aarch64_scalar_fp_unpack(qword_t bits,
        const struct aarch64_scalar_fp_format *format) {
    bits &= format->value_mask;
    return (struct aarch64_scalar_fp_number) {
        .bits = bits,
        .fraction = bits & format->fraction_mask,
        .exponent = (unsigned) ((bits & format->exponent_mask) >>
                format->fraction_bits),
        .sign = (bits & format->sign_mask) != 0,
    };
}

static inline bool aarch64_scalar_fp_is_nan(
        const struct aarch64_scalar_fp_number *number,
        const struct aarch64_scalar_fp_format *format) {
    return number->exponent == format->exponent_max &&
            number->fraction != 0;
}

static inline bool aarch64_scalar_fp_is_signaling_nan(
        const struct aarch64_scalar_fp_number *number,
        const struct aarch64_scalar_fp_format *format) {
    return aarch64_scalar_fp_is_nan(number, format) &&
            (number->fraction & format->quiet_bit) == 0;
}

static inline bool aarch64_scalar_fp_is_infinity(
        const struct aarch64_scalar_fp_number *number,
        const struct aarch64_scalar_fp_format *format) {
    return number->exponent == format->exponent_max &&
            number->fraction == 0;
}

static inline bool aarch64_scalar_fp_is_zero(
        const struct aarch64_scalar_fp_number *number) {
    return number->exponent == 0 && number->fraction == 0;
}

static inline struct aarch64_scalar_fp_result
aarch64_scalar_fp_default_nan(
        const struct aarch64_scalar_fp_format *format) {
    return (struct aarch64_scalar_fp_result) {
        .bits = format->default_nan,
        .exceptions = AARCH64_FPSR_IOC,
    };
}

static inline struct aarch64_scalar_fp_result
aarch64_scalar_fp_propagate_nan(
        const struct aarch64_scalar_fp_number *left,
        const struct aarch64_scalar_fp_number *right,
        const struct aarch64_scalar_fp_format *format, dword_t fpcr) {
    const struct aarch64_scalar_fp_number *selected;
    bool invalid = false;
    if (aarch64_scalar_fp_is_signaling_nan(left, format)) {
        selected = left;
        invalid = true;
    } else if (aarch64_scalar_fp_is_signaling_nan(right, format)) {
        selected = right;
        invalid = true;
    } else if (aarch64_scalar_fp_is_nan(left, format)) {
        selected = left;
    } else {
        selected = right;
    }

    qword_t bits = (fpcr & AARCH64_FPCR_DN) != 0 ?
            format->default_nan : selected->bits | format->quiet_bit;
    return (struct aarch64_scalar_fp_result) {
        .bits = bits & format->value_mask,
        .exceptions = invalid ? AARCH64_FPSR_IOC : 0,
    };
}

static inline void aarch64_scalar_fp_flush_input(
        struct aarch64_scalar_fp_number *number,
        const struct aarch64_scalar_fp_format *format,
        dword_t fpcr, dword_t *exceptions) {
    if ((fpcr & AARCH64_FPCR_FZ) != 0 && number->exponent == 0 &&
            number->fraction != 0) {
        number->fraction = 0;
        number->bits = number->sign ? format->sign_mask : 0;
        *exceptions |= AARCH64_FPSR_IDC;
    }
}

static inline unsigned aarch64_scalar_fp_top_bit(__uint128_t value) {
    qword_t high = (qword_t) (value >> 64);
    if (high != 0)
        return 127U - (unsigned) __builtin_clzll(high);
    return 63U - (unsigned) __builtin_clzll((qword_t) value);
}

static inline bool aarch64_scalar_fp_any_low_bits(
        __uint128_t value, unsigned count) {
    if (count == 0)
        return false;
    if (count >= 128)
        return value != 0;
    return (value & ((((__uint128_t) 1) << count) - 1)) != 0;
}

static inline bool aarch64_scalar_fp_rounds_up(__uint128_t magnitude,
        qword_t retained, unsigned discarded, bool tail, bool negative,
        dword_t fpcr) {
    bool inexact = tail || aarch64_scalar_fp_any_low_bits(
            magnitude, discarded);
    if (!inexact)
        return false;

    dword_t mode = (fpcr & AARCH64_FPCR_RMODE_MASK) >>
            AARCH64_FPCR_RMODE_SHIFT;
    if (mode == 1)
        return !negative;
    if (mode == 2)
        return negative;
    if (mode == 3 || discarded == 0 || discarded > 128)
        return false;

    __uint128_t halfway = ((__uint128_t) 1) << (discarded - 1);
    __uint128_t remainder = discarded == 128 ? magnitude :
            magnitude & ((((__uint128_t) 1) << discarded) - 1);
    return remainder > halfway ||
            (remainder == halfway && (tail || (retained & 1) != 0));
}

static inline qword_t aarch64_scalar_fp_overflow_bits(bool negative,
        const struct aarch64_scalar_fp_format *format, dword_t fpcr) {
    dword_t mode = (fpcr & AARCH64_FPCR_RMODE_MASK) >>
            AARCH64_FPCR_RMODE_SHIFT;
    bool infinity = mode == 0 || (mode == 1 && !negative) ||
            (mode == 2 && negative);
    qword_t sign = negative ? format->sign_mask : 0;
    if (infinity)
        return sign | format->exponent_mask;
    return sign | format->exponent_mask -
            (UINT64_C(1) << format->fraction_bits) |
            format->fraction_mask;
}

/*
 * magnitude * 2^scale 加上不足一个最低位单位的正尾数，统一在这里舍入。
 * 调用方保留至少三个额外位；存在尾数标记时必然还会丢弃有效位。
 */
static inline struct aarch64_scalar_fp_result aarch64_scalar_fp_round(
        bool negative, __uint128_t magnitude, int scale, bool tail,
        const struct aarch64_scalar_fp_format *format, dword_t fpcr) {
    unsigned top = aarch64_scalar_fp_top_bit(magnitude);
    int exponent = (int) top + scale;
    int minimum_exponent = 1 - format->exponent_bias;
    int maximum_exponent = (int) format->exponent_max - 1 -
            format->exponent_bias;
    if (exponent < minimum_exponent &&
            (fpcr & AARCH64_FPCR_FZ) != 0) {
        return (struct aarch64_scalar_fp_result) {
            .bits = negative ? format->sign_mask : 0,
            .exceptions = AARCH64_FPSR_UFC,
        };
    }
    if (exponent > maximum_exponent) {
        return (struct aarch64_scalar_fp_result) {
            .bits = aarch64_scalar_fp_overflow_bits(
                    negative, format, fpcr),
            .exceptions = AARCH64_FPSR_OFC | AARCH64_FPSR_IXC,
        };
    }

    int target_scale = exponent >= minimum_exponent ?
            exponent - (int) format->fraction_bits :
            minimum_exponent - (int) format->fraction_bits;
    int shift = target_scale - scale;
    qword_t retained;
    if (shift <= 0) {
        retained = (qword_t) (magnitude << (unsigned) -shift);
    } else if (shift < 128) {
        retained = (qword_t) (magnitude >> (unsigned) shift);
    } else {
        retained = 0;
    }

    unsigned discarded = shift > 0 ? (unsigned) shift : 0;
    bool inexact = tail || aarch64_scalar_fp_any_low_bits(
            magnitude, discarded);
    if (aarch64_scalar_fp_rounds_up(magnitude, retained,
            discarded, tail, negative, fpcr))
        retained++;

    qword_t precision_limit = UINT64_C(1) <<
            (format->fraction_bits + 1);
    if (exponent >= minimum_exponent && retained == precision_limit) {
        retained >>= 1;
        exponent++;
        if (exponent > maximum_exponent) {
            return (struct aarch64_scalar_fp_result) {
                .bits = aarch64_scalar_fp_overflow_bits(
                        negative, format, fpcr),
                .exceptions = AARCH64_FPSR_OFC | AARCH64_FPSR_IXC,
            };
        }
    }

    qword_t sign = negative ? format->sign_mask : 0;
    qword_t minimum_normal = UINT64_C(1) << format->fraction_bits;
    dword_t exceptions = inexact ? AARCH64_FPSR_IXC : 0;
    qword_t bits;
    if (exponent < minimum_exponent) {
        // Arm 在舍入前判定微小结果，进位到最小正规数仍属于下溢。
        if (inexact)
            exceptions |= AARCH64_FPSR_UFC;
        bits = retained == minimum_normal ?
                sign | minimum_normal : sign | retained;
    } else {
        qword_t encoded_exponent =
                (qword_t) (exponent + format->exponent_bias);
        bits = sign | encoded_exponent << format->fraction_bits |
                (retained & format->fraction_mask);
    }
    return (struct aarch64_scalar_fp_result) {
        .bits = bits & format->value_mask,
        .exceptions = exceptions,
    };
}

static inline qword_t aarch64_scalar_fp_shift_right(
        qword_t value, unsigned shift, bool *tail) {
    if (shift == 0)
        return value;
    if (shift >= 64) {
        *tail |= value != 0;
        return 0;
    }
    qword_t mask = (UINT64_C(1) << shift) - 1;
    *tail |= (value & mask) != 0;
    return value >> shift;
}

static inline qword_t aarch64_scalar_fp_significand(
        const struct aarch64_scalar_fp_number *number,
        const struct aarch64_scalar_fp_format *format) {
    return number->fraction | (number->exponent != 0 ?
            UINT64_C(1) << format->fraction_bits : 0);
}

static inline int aarch64_scalar_fp_exponent(
        const struct aarch64_scalar_fp_number *number,
        const struct aarch64_scalar_fp_format *format) {
    return (number->exponent != 0 ? (int) number->exponent : 1) -
            format->exponent_bias;
}

static inline struct aarch64_scalar_fp_result aarch64_scalar_fp_convert(
        qword_t source_bits, byte_t source_width,
        byte_t destination_width, dword_t fpcr) {
    struct aarch64_scalar_fp_format source_format =
            aarch64_scalar_fp_format(source_width);
    struct aarch64_scalar_fp_format destination_format =
            aarch64_scalar_fp_format(destination_width);
    struct aarch64_scalar_fp_number source =
            aarch64_scalar_fp_unpack(source_bits, &source_format);
    dword_t exceptions = 0;
    aarch64_scalar_fp_flush_input(
            &source, &source_format, fpcr, &exceptions);

    if (aarch64_scalar_fp_is_nan(&source, &source_format)) {
        bool invalid = aarch64_scalar_fp_is_signaling_nan(
                &source, &source_format);
        qword_t bits;
        if ((fpcr & AARCH64_FPCR_DN) != 0) {
            bits = destination_format.default_nan;
        } else {
            qword_t payload = source.fraction |
                    source_format.quiet_bit;
            if (destination_format.fraction_bits >
                    source_format.fraction_bits) {
                payload <<= destination_format.fraction_bits -
                        source_format.fraction_bits;
            } else {
                payload >>= source_format.fraction_bits -
                        destination_format.fraction_bits;
            }
            bits = (source.sign ? destination_format.sign_mask : 0) |
                    destination_format.exponent_mask |
                    (payload & destination_format.fraction_mask);
        }
        return (struct aarch64_scalar_fp_result) {
            .bits = bits,
            .exceptions = exceptions |
                    (invalid ? AARCH64_FPSR_IOC : 0),
        };
    }
    if (aarch64_scalar_fp_is_infinity(&source, &source_format)) {
        return (struct aarch64_scalar_fp_result) {
            .bits = (source.sign ? destination_format.sign_mask : 0) |
                    destination_format.exponent_mask,
            .exceptions = exceptions,
        };
    }
    if (aarch64_scalar_fp_is_zero(&source)) {
        return (struct aarch64_scalar_fp_result) {
            .bits = source.sign ? destination_format.sign_mask : 0,
            .exceptions = exceptions,
        };
    }

    struct aarch64_scalar_fp_result result = aarch64_scalar_fp_round(
            source.sign,
            aarch64_scalar_fp_significand(&source, &source_format),
            aarch64_scalar_fp_exponent(&source, &source_format) -
                    (int) source_format.fraction_bits,
            false, &destination_format, fpcr);
    result.exceptions |= exceptions;
    return result;
}

static inline struct aarch64_scalar_fp_result aarch64_scalar_fp_add_core(
        qword_t left_bits, qword_t right_bits, byte_t width, dword_t fpcr,
        bool subtract) {
    struct aarch64_scalar_fp_format format =
            aarch64_scalar_fp_format(width);
    struct aarch64_scalar_fp_number left =
            aarch64_scalar_fp_unpack(left_bits, &format);
    struct aarch64_scalar_fp_number right =
            aarch64_scalar_fp_unpack(right_bits, &format);
    dword_t exceptions = 0;
    aarch64_scalar_fp_flush_input(&left, &format, fpcr, &exceptions);
    aarch64_scalar_fp_flush_input(&right, &format, fpcr, &exceptions);
    if (aarch64_scalar_fp_is_nan(&left, &format) ||
            aarch64_scalar_fp_is_nan(&right, &format)) {
        struct aarch64_scalar_fp_result result =
                aarch64_scalar_fp_propagate_nan(
                        &left, &right, &format, fpcr);
        result.exceptions |= exceptions;
        return result;
    }

    right.sign ^= subtract;
    if (aarch64_scalar_fp_is_infinity(&left, &format) ||
            aarch64_scalar_fp_is_infinity(&right, &format)) {
        if (aarch64_scalar_fp_is_infinity(&left, &format) &&
                aarch64_scalar_fp_is_infinity(&right, &format) &&
                left.sign != right.sign) {
            struct aarch64_scalar_fp_result result =
                    aarch64_scalar_fp_default_nan(&format);
            result.exceptions |= exceptions;
            return result;
        }
        const struct aarch64_scalar_fp_number *infinity =
                aarch64_scalar_fp_is_infinity(&left, &format) ?
                &left : &right;
        return (struct aarch64_scalar_fp_result) {
            .bits = (infinity->sign ? format.sign_mask : 0) |
                    format.exponent_mask,
            .exceptions = exceptions,
        };
    }

    bool left_zero = aarch64_scalar_fp_is_zero(&left);
    bool right_zero = aarch64_scalar_fp_is_zero(&right);
    if (left_zero && right_zero) {
        dword_t mode = (fpcr & AARCH64_FPCR_RMODE_MASK) >>
                AARCH64_FPCR_RMODE_SHIFT;
        bool negative = left.sign == right.sign ? left.sign : mode == 2;
        return (struct aarch64_scalar_fp_result) {
            .bits = negative ? format.sign_mask : 0,
            .exceptions = exceptions,
        };
    }
    if (left_zero || right_zero) {
        const struct aarch64_scalar_fp_number *number =
                left_zero ? &right : &left;
        return (struct aarch64_scalar_fp_result) {
            .bits = (number->sign ? format.sign_mask : 0) |
                    (qword_t) number->exponent << format.fraction_bits |
                    number->fraction,
            .exceptions = exceptions,
        };
    }

    qword_t left_significand =
            aarch64_scalar_fp_significand(&left, &format);
    qword_t right_significand =
            aarch64_scalar_fp_significand(&right, &format);
    int left_exponent = aarch64_scalar_fp_exponent(&left, &format);
    int right_exponent = aarch64_scalar_fp_exponent(&right, &format);
    bool left_is_larger = left_exponent > right_exponent ||
            (left_exponent == right_exponent &&
            left_significand >= right_significand);
    qword_t large = left_is_larger ? left_significand : right_significand;
    qword_t small = left_is_larger ? right_significand : left_significand;
    int large_exponent = left_is_larger ? left_exponent : right_exponent;
    int small_exponent = left_is_larger ? right_exponent : left_exponent;
    bool negative = left_is_larger ? left.sign : right.sign;

    large <<= 3;
    small <<= 3;
    bool tail = false;
    small = aarch64_scalar_fp_shift_right(
            small, (unsigned) (large_exponent - small_exponent), &tail);
    __uint128_t magnitude;
    if (left.sign == right.sign) {
        magnitude = (__uint128_t) large + small;
    } else {
        magnitude = (__uint128_t) large - small;
        if (tail) {
            magnitude--;
            /* 减法尾数位于下一整数格内，仍可表示为正尾数。 */
        }
        if (magnitude == 0 && !tail) {
            dword_t mode = (fpcr & AARCH64_FPCR_RMODE_MASK) >>
                    AARCH64_FPCR_RMODE_SHIFT;
            return (struct aarch64_scalar_fp_result) {
                .bits = mode == 2 ? format.sign_mask : 0,
                .exceptions = exceptions,
            };
        }
    }

    struct aarch64_scalar_fp_result result = aarch64_scalar_fp_round(
            negative, magnitude,
            large_exponent - (int) format.fraction_bits - 3,
            tail, &format, fpcr);
    result.exceptions |= exceptions;
    return result;
}

static inline struct aarch64_scalar_fp_result aarch64_scalar_fp_add(
        qword_t left, qword_t right, byte_t width, dword_t fpcr) {
    return aarch64_scalar_fp_add_core(
            left, right, width, fpcr, false);
}

static inline struct aarch64_scalar_fp_result aarch64_scalar_fp_subtract(
        qword_t left, qword_t right, byte_t width, dword_t fpcr) {
    return aarch64_scalar_fp_add_core(
            left, right, width, fpcr, true);
}

static inline struct aarch64_scalar_fp_result aarch64_scalar_fp_multiply(
        qword_t left_bits, qword_t right_bits, byte_t width, dword_t fpcr) {
    struct aarch64_scalar_fp_format format =
            aarch64_scalar_fp_format(width);
    struct aarch64_scalar_fp_number left =
            aarch64_scalar_fp_unpack(left_bits, &format);
    struct aarch64_scalar_fp_number right =
            aarch64_scalar_fp_unpack(right_bits, &format);
    dword_t exceptions = 0;
    aarch64_scalar_fp_flush_input(&left, &format, fpcr, &exceptions);
    aarch64_scalar_fp_flush_input(&right, &format, fpcr, &exceptions);
    if (aarch64_scalar_fp_is_nan(&left, &format) ||
            aarch64_scalar_fp_is_nan(&right, &format)) {
        struct aarch64_scalar_fp_result result =
                aarch64_scalar_fp_propagate_nan(
                        &left, &right, &format, fpcr);
        result.exceptions |= exceptions;
        return result;
    }

    bool negative = left.sign != right.sign;
    bool left_infinity = aarch64_scalar_fp_is_infinity(&left, &format);
    bool right_infinity = aarch64_scalar_fp_is_infinity(&right, &format);
    bool left_zero = aarch64_scalar_fp_is_zero(&left);
    bool right_zero = aarch64_scalar_fp_is_zero(&right);
    if ((left_infinity && right_zero) ||
            (right_infinity && left_zero)) {
        struct aarch64_scalar_fp_result result =
                aarch64_scalar_fp_default_nan(&format);
        result.exceptions |= exceptions;
        return result;
    }
    if (left_infinity || right_infinity) {
        return (struct aarch64_scalar_fp_result) {
            .bits = (negative ? format.sign_mask : 0) |
                    format.exponent_mask,
            .exceptions = exceptions,
        };
    }
    if (left_zero || right_zero) {
        return (struct aarch64_scalar_fp_result) {
            .bits = negative ? format.sign_mask : 0,
            .exceptions = exceptions,
        };
    }

    qword_t left_significand =
            aarch64_scalar_fp_significand(&left, &format);
    qword_t right_significand =
            aarch64_scalar_fp_significand(&right, &format);
    int scale = aarch64_scalar_fp_exponent(&left, &format) +
            aarch64_scalar_fp_exponent(&right, &format) -
            2 * (int) format.fraction_bits;
    __uint128_t magnitude =
            (__uint128_t) left_significand * right_significand;
    struct aarch64_scalar_fp_result result = aarch64_scalar_fp_round(
            negative, magnitude, scale, false, &format, fpcr);
    result.exceptions |= exceptions;
    return result;
}

static inline struct aarch64_scalar_fp_compare_result
aarch64_scalar_fp_compare(qword_t left_bits, qword_t right_bits,
        byte_t width, dword_t fpcr, bool signaling) {
    struct aarch64_scalar_fp_format format =
            aarch64_scalar_fp_format(width);
    struct aarch64_scalar_fp_number left =
            aarch64_scalar_fp_unpack(left_bits, &format);
    struct aarch64_scalar_fp_number right =
            aarch64_scalar_fp_unpack(right_bits, &format);
    dword_t exceptions = 0;
    aarch64_scalar_fp_flush_input(&left, &format, fpcr, &exceptions);
    aarch64_scalar_fp_flush_input(&right, &format, fpcr, &exceptions);
    bool left_nan = aarch64_scalar_fp_is_nan(&left, &format);
    bool right_nan = aarch64_scalar_fp_is_nan(&right, &format);
    if (left_nan || right_nan) {
        bool invalid = signaling ||
                aarch64_scalar_fp_is_signaling_nan(&left, &format) ||
                aarch64_scalar_fp_is_signaling_nan(&right, &format);
        return (struct aarch64_scalar_fp_compare_result) {
            .nzcv = UINT32_C(0x30000000),
            .exceptions = exceptions |
                    (invalid ? AARCH64_FPSR_IOC : 0),
        };
    }

    qword_t left_magnitude = left.bits & ~format.sign_mask;
    qword_t right_magnitude = right.bits & ~format.sign_mask;
    if (aarch64_scalar_fp_is_zero(&left))
        left_magnitude = 0;
    if (aarch64_scalar_fp_is_zero(&right))
        right_magnitude = 0;
    dword_t nzcv;
    if (left_magnitude == 0 && right_magnitude == 0) {
        nzcv = UINT32_C(0x60000000);
    } else if (left.sign != right.sign) {
        nzcv = left.sign ? UINT32_C(0x80000000) : UINT32_C(0x20000000);
    } else if (left_magnitude == right_magnitude) {
        nzcv = UINT32_C(0x60000000);
    } else {
        bool less = left_magnitude < right_magnitude;
        if (left.sign)
            less = !less;
        nzcv = less ? UINT32_C(0x80000000) : UINT32_C(0x20000000);
    }
    return (struct aarch64_scalar_fp_compare_result) {
        .nzcv = nzcv,
        .exceptions = exceptions,
    };
}

#endif

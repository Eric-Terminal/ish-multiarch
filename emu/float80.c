#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "float80.h"
#include "misc.h"

typedef unsigned __int128 uint128_t;

// If you don't understand why something is the way it is, change it and run
// the test suite and all will become clear.

// exponent is stored with a constant added to it, because that's apparently
// easier than just saying the exponent is two's complement
#define BIAS80 0x3fff
#define EXP_MAX 0x7ffe
#define EXP_MIN 0x0001
#define EXP_SPECIAL 0x7fff
#define EXP_DENORMAL 0
static unsigned bias(int exp) {
    return (unsigned) (exp + BIAS80);
}
static int unbias(unsigned exp) {
    return (int) exp - BIAS80;
}
// returns the correct answer for denormal numbers
static int unbias_denormal(unsigned exp) {
    if (exp == EXP_DENORMAL)
        return unbias(EXP_MIN);
    return unbias(exp);
}

#define CURSED_BIT (UINT64_C(1) << 63)

__thread enum f80_rounding_mode f80_rounding_mode;

static bool round_away_from_zero(int sign) {
    return (f80_rounding_mode == round_up && !sign) ||
        (f80_rounding_mode == round_down && sign);
}

static float80 f80_overflow(int sign) {
    bool to_max_finite = f80_rounding_mode == round_chop ||
        (f80_rounding_mode == round_down && !sign) ||
        (f80_rounding_mode == round_up && sign);
    float80 f = to_max_finite
        ? (float80) {.exp = EXP_MAX, .signif = UINT64_MAX}
        : F80_INF;
    f.sign = (unsigned) sign;
    return f;
}

// shift a 128 bit integer right but using the floating point rounding mode
// used by f80_shift_right and to round the 128-bit result of multiplying significands
// sign is necessary to decide which way to round when mode is round_up or round_down
static uint128_t u128_shift_right_round(uint128_t i, int shift, int sign) {
    // we're going to be shifting stuff by shift - 1, so stay safe
    if (shift == 0)
        return i;
    if (shift > 127) {
        // If we should be rounding away from zero, and there are any nonzero
        // bits involved, an infinite amount of right shift should give 1
        if (round_away_from_zero(sign) && i != 0)
            return 1;
        return 0;
    }

    // stuff necessary for rounding to nearest or even. reference: https://stackoverflow.com/a/8984135
    // grab the guard bit, the last bit shifted out
    int guard = (int) ((i >> (shift - 1)) & 1);
    // 粘滞位必须覆盖所有移出位，不能在 64 位边界提前截断。
    uint128_t rest = i & ~((uint128_t) -1 << (shift - 1));

    i >>= shift;
    // if all the bits shifted out were zeroes, we're done
    if (guard == 0 && rest == 0)
        return i;

    if (round_away_from_zero(sign)) {
        i++;
    } else if (f80_rounding_mode == round_to_nearest && guard) {
        if (rest != 0)
            i++; // round up
        else if (i & 1)
            i++; // round to nearest even
    }
    return i;
}

// may overflow
static float80 f80_shift_left(float80 f, int shift) {
    f.signif <<= shift;
    f.exp -= shift;
    return f;
}

// may lose precision
static float80 f80_shift_right(float80 f, int shift) {
    f.signif = (uint64_t) u128_shift_right_round(f.signif, shift, (int) f.sign);
    f.exp += shift;
    return f;
}

// a number is unsupported if the cursed bit (first bit of the significand,
// also known as the integer bit) is incorrect. it must be 0 for denormals and
// 1 for any other type of number.
bool f80_is_supported(float80 f) {
    if (f.exp == EXP_DENORMAL)
        return f.signif >> 63 == 0;
    return f.signif >> 63 == 1;
}

bool f80_isnan(float80 f) {
    return f.exp == EXP_SPECIAL && (f.signif & (UINT64_MAX >> 1)) != 0;
}
bool f80_isinf(float80 f) {
    return f.exp == EXP_SPECIAL && (f.signif & (UINT64_MAX >> 1)) == 0;
}
bool f80_iszero(float80 f) {
    return f.exp == EXP_DENORMAL && f.signif == 0;
}
bool f80_isdenormal(float80 f) {
    return f.exp == EXP_DENORMAL && f.signif != 0;
}

static float80 f80_normalize(float80 f) {
    // this function probably can't handle unsupported numbers
    // except cursed normals which are just unnormals, and working with them is the point of this function
    if (f.exp == EXP_DENORMAL || f.exp == EXP_SPECIAL)
        assert(f80_is_supported(f));

    // denormals (and zero) are already normalized (unlike the name suggests)
    if (f.exp == EXP_DENORMAL)
        return f;
    // shift left as many times as possible without overflow
    // number of leading zeroes = how many times we can shift out a leading digit before overflow
    int shift;
    if (f.signif != 0)
        shift = __builtin_clzll((unsigned long long) f.signif);
    else
        shift = 64; // 计数前单独处理零，避免调用内建函数的未定义分支。
    if (f.exp - shift < EXP_MIN) {
        // if we shifted this much, exponent would go below its minimum
        // so shift as much as possible and create a denormal
        f = f80_shift_left(f, f.exp - EXP_MIN);
        f.exp = EXP_DENORMAL;
        return f;
    }
    return f80_shift_left(f, shift);
}

static int u128_clz(uint128_t x) {
    // correctly counting leading zeros on a 128-bit int is interesting
    int zeros;
    if (x >> 64 != 0)
        zeros = __builtin_clzll((unsigned long long) (x >> 64));
    else if (x != 0)
        zeros = 64 + __builtin_clzll((unsigned long long) x);
    else
        zeros = 128;
    return zeros;
}

static float80 u128_normalize_round(uint128_t signif, int exp, int sign) {
    if (signif == 0)
        return (float80) {.sign = (unsigned) sign};

    int shift = u128_clz(signif);
    // now shift left
    if (exp - shift < unbias(EXP_MIN)) {
        if (exp > unbias(EXP_MIN))
            signif <<= exp - unbias(EXP_MIN);
        else
            signif = u128_shift_right_round(signif, unbias(EXP_MIN) - exp, sign);
        exp = unbias(EXP_DENORMAL);
    } else if (exp - shift > unbias(EXP_MAX)) {
        //printf("0x%.16llx%.16llx ", (unsigned long long) (signif >> 64), (unsigned long long) signif);
        // 超出范围时，精确的二次幂也必须服从当前定向舍入模式。
        return f80_overflow(sign);
    } else {
        signif <<= shift;
        exp -= shift;
    }
    // and round
    float80 f;
    f.exp = bias(exp);
    // hack around cases where u128_shift_right_round returns 0x10000000000000000
    // such as signif = 0xffffffffffffffff0000000000000000
    signif = u128_shift_right_round(signif, 64, sign);
    if (signif >> 64 != 0) {
        signif >>= 1;
        f.exp++;
    }
    f.signif = (uint64_t) signif;
    f.sign = (unsigned) sign;
    return f;
}

float80 f80_from_int(int64_t i) {
    // stick i in the significand, give it an exponent of 2^63 to offset the
    // implicit binary point after the first bit, and then normalize
    uint64_t magnitude = i < 0 ? (uint64_t) (-(i + 1)) + 1 : (uint64_t) i;
    float80 f = {
        .signif = magnitude,
        .exp = bias(63),
        .sign = i < 0,
    };
    if (i == 0)
        f.exp = 0;
    return f80_normalize(f);
}

int64_t f80_to_int(float80 f) {
    if (!f80_is_supported(f))
        return INT64_MIN; // indefinite
    // if you need an exponent greater than 2^63 to represent this number, it
    // can't be represented as a 64-bit integer
    if (f.exp > bias(63))
        return INT64_MIN; // also indefinite
    // shift right (reduce precision) until the exponent is 2^63
    f = f80_shift_right(f, (int) (bias(63) - f.exp));
    // and the answer should be the significand!
    if (!f.sign)
        return f.signif <= INT64_MAX ? (int64_t) f.signif : INT64_MIN;
    if (f.signif >= (UINT64_C(1) << 63))
        return INT64_MIN;
    return -(int64_t) f.signif;
}
#define SIGNIF64_BITS 52
#define SIGNIF64_MASK ((UINT64_C(1) << SIGNIF64_BITS) - 1)
#define EXP64_SHIFT SIGNIF64_BITS
#define SIGN64_SHIFT 63
#define EXP64_MAX 0x7fe
#define EXP64_MIN 0x001
#define EXP64_SPECIAL 0x7ff
#define EXP64_DENORMAL 0x000

// unsupported?
float80 f80_from_double(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    uint64_t signif = bits & SIGNIF64_MASK;
    unsigned exp = (unsigned) ((bits >> EXP64_SHIFT) & EXP64_SPECIAL);
    unsigned sign = (unsigned) (bits >> SIGN64_SHIFT);
    float80 f;

    if (exp == EXP64_SPECIAL)
        f.exp = EXP_SPECIAL;
    else if (exp == EXP64_DENORMAL)
        // denormals actually have an exponent of EXP_MIN, the special exponent
        // is needed to indicate the integer bit is 0
        // zeroes have the same exponent as denormals but need to be handled
        // differently
        f.exp = signif == 0 ? 0 : bias(1 - 0x3ff);
    else
        f.exp = bias((int) exp - 0x3ff);

    f.signif = signif << 11;
    if (exp != EXP64_DENORMAL)
        f.signif |= CURSED_BIT;
    f.sign = sign;
    return f80_normalize(f);
}

double f80_to_double(float80 f) {
    if (!f80_is_supported(f))
        return NAN;
    uint64_t fraction;
    int new_exp;
    if (f.exp == EXP_SPECIAL) {
        new_exp = EXP64_SPECIAL;
        fraction = (f.signif & (UINT64_MAX >> 1)) >> 11;
        if (f80_isnan(f) && fraction == 0)
            fraction = 1;
    } else if (f80_iszero(f)) {
        new_exp = EXP64_DENORMAL;
        fraction = 0;
    } else {
        int exp = unbias_denormal(f.exp);
        if (exp > 1023) {
            bool to_infinity = f80_rounding_mode == round_to_nearest ||
                (f80_rounding_mode == round_up && !f.sign) ||
                (f80_rounding_mode == round_down && f.sign);
            new_exp = to_infinity ? EXP64_SPECIAL : EXP64_MAX;
            fraction = to_infinity ? 0 : SIGNIF64_MASK;
        } else if (exp < -1022) {
            // binary64 子正常数以 2^-1074 为一个整数单位完成一次舍入。
            int shift = -exp - 1011;
            uint64_t rounded = (uint64_t) u128_shift_right_round(
                f.signif, shift, (int) f.sign);
            if (rounded & (UINT64_C(1) << SIGNIF64_BITS)) {
                new_exp = EXP64_MIN;
                fraction = 0;
            } else {
                new_exp = EXP64_DENORMAL;
                fraction = rounded;
            }
        } else {
            new_exp = exp + 0x3ff;
            uint64_t rounded = (uint64_t) u128_shift_right_round(
                f.signif, 11, (int) f.sign);
            if (rounded & (UINT64_C(1) << 53)) {
                rounded >>= 1;
                new_exp++;
            }
            fraction = rounded & SIGNIF64_MASK;
        }
    }
    uint64_t bits = ((uint64_t) f.sign << SIGN64_SHIFT) |
        ((uint64_t) new_exp << EXP64_SHIFT) | fraction;
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

float80 f80_round(float80 f) {
    if (!f80_is_supported(f))
        return F80_NAN;
    // Shift out all the bits to the right of the point (early exit if there are none)
    int bits_to_clear = 63 - unbias(f.exp);
    if (bits_to_clear <= 0)
        return f;

    f = f80_shift_right(f, bits_to_clear);
    if (f.signif == 0) {
        // If that just totally eradicated the significand, guess the answer is 0
        f.exp = EXP_DENORMAL;
    } else {
        f = f80_normalize(f);
    }
    return f;
}

float80 f80_neg(float80 f) {
    f.sign = !f.sign;
    return f;
}
float80 f80_abs(float80 f) {
    f.sign = 0;
    return f;
}

#define handle_nans(a, b) do { \
    if (!f80_is_supported(a) || !f80_is_supported(b)) \
        return F80_NAN; \
    /* this case is bizarre but hey I don't make the chips. though the amd spec
     * says it's undefined which nan is returned if both have the same
     * significant and different sign, so why am I doing this */\
    if (f80_isnan(a) && f80_isnan(b) && a.sign && !b.sign) \
        return b; \
    if (f80_isnan(a)) \
        return a; \
    if (f80_isnan(b)) \
        return b; \
} while(0)

float80 f80_add(float80 a, float80 b) {
    handle_nans(a, b);

    // a has larger exponent, b has smaller exponent
    if (a.exp < b.exp) {
        float80 tmp = a;
        a = b;
        b = tmp;
    }

    // reduce the number of cases to deal with
    bool flipped = false;
    if (a.sign) {
        a.sign = !a.sign;
        b.sign = !b.sign;
        flipped = true;
    }
    // now either both are positive (addition) or a is positive and b is
    // negative (subtraction)

    // do the addition in insane precision to fix that bug with adding 2^64 and 1.5
    uint128_t a_signif = (uint128_t) a.signif << 64;
    uint128_t b_signif = (uint128_t) b.signif << 64;
    // shift b (smaller exponent) right until the exponents are equal
    b_signif = u128_shift_right_round(b_signif, a.exp - b.exp, b.sign ^ flipped);

    int sign = a.sign;
    int exp = unbias_denormal(a.exp);
    uint128_t signif = a_signif;
    if (!b.sign) {
        // b is positive, so add
        if (!f80_isinf(a)) {
            if (__builtin_add_overflow(a_signif, b_signif, &signif)) {
                // in case of overflow, lose 1 bit of precision
                signif = u128_shift_right_round(signif, 1, sign);
                signif |= (uint128_t) 1 << 127; // recover the bit lost by the overflow
                exp++;
            }
        }
    } else {
        // b is negative, so subtract
        // but first, special case time!

        // infinity - infinity is indefinite, not zero
        if (f80_isinf(a) && f80_isinf(b))
            return F80_NAN;

        // When subtracting a (relatively) very small number in chop mode, all
        // the bits will get shifted out and nothing will happen, but this
        // should give a smaller result.
        if (f80_rounding_mode == round_chop && b_signif == 0 && b.signif != 0)
            b_signif = 1;

        // Depending on the rounding mode, it's possible that shifting out all
        // the bits produced 1 instead of 0. Subtracting 1 from infinity would
        // give 2^16384-1 which is wrong.
        if (f80_isinf(a))
            b_signif = 0;

        if (a_signif >= b_signif) {
            // we can subtract without underflow
            signif = a_signif - b_signif;
        } else {
            // the answer will be negative
            signif = b_signif - a_signif;
            sign = 1;
        }

        // a bizarre special case. from https://twitter.com/tblodt/status/1262145524620234752:
        // > why does 1 - 1 = -0 when the x86 rounding mode is set to "round down"? it's just 0 for any other rounding mode
        if (signif == 0 && a_signif != 0 && f80_rounding_mode == round_down)
            return (float80) {.sign = 1};

        // a - a = 0
        if (signif == 0)
            return (float80) {0};
    }

    if (flipped)
        sign = !sign;
    float80 f = u128_normalize_round(signif, exp, sign);
    assert(f80_is_supported(f));
    return f;
}
float80 f80_sub(float80 a, float80 b) {
    return f80_add(a, f80_neg(b));
}

float80 f80_mul(float80 a, float80 b) {
    handle_nans(a, b);

    if (f80_isinf(a) || f80_isinf(b)) {
        // infinity times zero is undefined
        if (f80_iszero(a) || f80_iszero(b))
            return F80_NAN;
        // infinity times anything else is infinity
        float80 f = F80_INF;
        f.sign = a.sign ^ b.sign;
        return f;
    }

    // add exponents (the +1 is necessary to be correct in 128-bit precision)
    int f_exp = unbias_denormal(a.exp) + unbias_denormal(b.exp) + 1;
    // multiply significands
    uint128_t f_signif = (uint128_t) a.signif * b.signif;
    // normalize and round the 128-bit result
    float80 f = u128_normalize_round(f_signif, f_exp, a.sign ^ b.sign);
    // xor signs
    f.sign = a.sign ^ b.sign;
    return f;
}

float80 f80_div(float80 a, float80 b) {
    handle_nans(a, b);

    float80 f;
    if (f80_isinf(a)) {
        // dividing into infinity gives infinity
        f = F80_INF;
        // except infinity / infinity is nan
        if (f80_isinf(b))
            return F80_NAN;
    } else if (f80_isinf(b)) {
        // dividing by infinity gives zero
        f = (float80) {0};
    } else if (f80_iszero(b)) {
        // division by zero gives infinity
        f = F80_INF;
        // except 0 / 0 is nan
        if (f80_iszero(a))
            f = F80_NAN;
    } else {
        int b_trailing = __builtin_ctzll((unsigned long long) b.signif);
        b.signif >>= b_trailing;
        uint128_t signif = ((uint128_t) a.signif << 64) / b.signif;
        uint128_t remainder = ((uint128_t) a.signif << 64) % b.signif;
        // extend this to 128 bit precision because hell yeah
        int extra_bits = 0;
        if (signif != 0) {
            extra_bits = u128_clz(signif);
            for (int bit = 0; bit < extra_bits; bit++) {
                signif <<= 1;
                remainder <<= 1;
                if (remainder >= b.signif) {
                    remainder -= b.signif;
                    signif |= 1;
                }
            }
            // 未生成的商位仍非零时，最低位记录粘滞信息供最终舍入使用。
            if (remainder != 0)
                signif |= 1;
        }
        int exp = unbias_denormal(a.exp) - unbias_denormal(b.exp) + 63 - b_trailing - extra_bits;
        f = u128_normalize_round(signif, exp, a.sign ^ b.sign);
    }

    f.sign = a.sign ^ b.sign;
    return f;
}

float80 f80_mod(float80 x, float80 y) {
    float80 quotient = f80_div(x, y);
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    quotient = f80_round(quotient);
    f80_rounding_mode = old_mode;
    return f80_sub(x, f80_mul(quotient, y));
}

bool f80_uncomparable(float80 a, float80 b) {
    if (!f80_is_supported(a) || !f80_is_supported(b))
        return true;
    if (f80_isnan(a) || f80_isnan(b))
        return true;
    return false;
}

bool f80_lt(float80 a, float80 b) {
    if (f80_uncomparable(a, b))
        return false;
    // same signed infinities are equal, not less (though subtraction would produce nan)
    if (f80_isinf(a) && f80_isinf(b) && a.sign == b.sign)
        return false;
    // zeroes are always equal
    if (f80_iszero(a) && f80_iszero(b))
        return false;
    // if a < b then a - b < 0
    float80 diff = f80_sub(a, b);
    return diff.sign == 1 && !f80_iszero(diff);
}
bool f80_eq(float80 a, float80 b) {
    if (f80_uncomparable(a, b))
        return false;
    if (f80_iszero(a)) a.sign = 0;
    if (f80_iszero(a)) b.sign = 0;
    return a.sign == b.sign && a.exp == b.exp && a.signif == b.signif;
}

bool f80_lte(float80 a, float80 b) {
    return f80_lt(a, b) || f80_eq(a, b);
}
bool f80_gt(float80 a, float80 b) {
    return !f80_lte(a, b);
}

float80 f80_log2(float80 x) {
    float80 zero = f80_from_int(0);
    float80 one = f80_from_int(1);
    float80 two = f80_from_int(2);
    if (f80_isnan(x) || f80_lte(x, zero))
        return F80_NAN;

    int ipart = 0;
    while (f80_lt(x, one)) {
        ipart--;
        x = f80_mul(x, two);
    }
    while (f80_gt(x, two)) {
        ipart++;
        x = f80_div(x, two);
    }
    float80 res = f80_from_int(ipart);

    float80 bit = one;
    while (f80_gt(bit, zero)) {
        while (f80_lte(x, two) && f80_gt(bit, zero)) {
            x = f80_mul(x, x);
            bit = f80_div(bit, two);
        }
        float80 oldres = res;
        res = f80_add(res, bit);
        if (oldres.signif == res.signif && oldres.exp == res.exp && oldres.sign == res.sign)
            break;
        x = f80_div(x, two);
    }
    return res;
}

float80 f80_sqrt(float80 x) {
    if (f80_iszero(x))
        return x;
    if (f80_isnan(x) || x.sign)
        return F80_NAN;
    // for a rough guess, just cut the exponent by 2
    float80 guess = x;
    guess.exp = bias(unbias(guess.exp) / 2);
    // now converge on the answer, using newton's method
    float80 old_guess;
    float80 two = f80_from_int(2);
    int i = 0;
    do {
        old_guess = guess;
        guess = f80_div(f80_add(guess, f80_div(x, guess)), two);
    } while (!f80_eq(guess, old_guess) && i++ < 100);
    return guess;
}

float80 f80_scale(float80 x, int scale) {
    if (!f80_is_supported(x) || f80_isnan(x))
        return F80_NAN;
    if (f80_iszero(x) || f80_isinf(x))
        return x;

    int shift = __builtin_clzll((unsigned long long) x.signif);
    int64_t scaled_exp = (int64_t) unbias_denormal(x.exp) + scale;
    int64_t normalized_exp = scaled_exp - shift;
    if (normalized_exp > unbias(EXP_MAX))
        return f80_overflow((int) x.sign);
    // 低于最小子正常数的一半后，仅定向远离零的模式还能得到非零结果。
    int min_half_exp = unbias(EXP_MIN) - 64;
    if (normalized_exp < min_half_exp) {
        float80 result = {.sign = x.sign};
        if (round_away_from_zero((int) x.sign))
            result.signif = 1;
        return result;
    }
    return u128_normalize_round(
        (uint128_t) x.signif << 64, (int) scaled_exp, (int) x.sign);
}

float80 f80_scale_by_float(float80 x, float80 scale) {
    if (!f80_is_supported(x) || !f80_is_supported(scale))
        return F80_NAN;
    if (f80_isnan(x))
        return x;
    if (f80_isnan(scale))
        return scale;

    if (f80_isinf(scale)) {
        if (scale.sign) {
            if (f80_isinf(x))
                return F80_NAN;
            if (f80_iszero(x))
                return x;
            return (float80) {.sign = x.sign};
        }
        if (f80_iszero(x))
            return F80_NAN;
        if (f80_isinf(x))
            return x;
        float80 result = F80_INF;
        result.sign = x.sign;
        return result;
    }

    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    int64_t integral_scale = f80_to_int(scale);
    f80_rounding_mode = old_mode;

    int clamped_scale;
    if (integral_scale == INT64_MIN)
        clamped_scale = scale.sign ? INT_MIN : INT_MAX;
    else if (integral_scale > INT_MAX)
        clamped_scale = INT_MAX;
    else if (integral_scale < INT_MIN)
        clamped_scale = INT_MIN;
    else
        clamped_scale = (int) integral_scale;
    return f80_scale(x, clamped_scale);
}

void f80_xtract(float80 f, int *exp, float80 *signif) {
    *exp = unbias(f.exp);
    *signif = f;
    signif->exp = bias(0);
}

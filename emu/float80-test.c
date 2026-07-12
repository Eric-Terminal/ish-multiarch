#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "float80.h"

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

#if (defined(__i386__) || defined(__x86_64__)) && \
        LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384 && \
        defined(__SIZEOF_LONG_DOUBLE__) && __SIZEOF_LONG_DOUBLE__ >= 10 && \
        defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
        __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define HAVE_NATIVE_X87 1
#else
#define HAVE_NATIVE_X87 0
#endif

_Static_assert(sizeof(double) == sizeof(uint64_t), "测试要求宿主 double 为 64 位");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
    "测试要求宿主 double 使用 IEEE binary64");

static int tests_passed;
static int tests_total;
static int suite_passed;
static int suite_total;

static void suite_start(const char *name) {
    printf("==== %s ====\n", name);
    suite_passed = 0;
    suite_total = 0;
}

static void suite_end(const char *name) {
    printf("%s：%d/%d 通过\n", name, suite_passed, suite_total);
}

static void check(bool condition, const char *format, ...) {
    tests_total++;
    suite_total++;
    if (condition) {
        tests_passed++;
        suite_passed++;
        return;
    }

    fputs("失败：", stdout);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

static float80 f80_bits(uint64_t signif, uint16_t sign_exp) {
    return (float80) {.signif = signif, .signExp = sign_exp};
}

static bool f80_same(float80 actual, float80 expected) {
    return actual.signif == expected.signif && actual.signExp == expected.signExp;
}

static void check_f80(const char *name, float80 actual, float80 expected) {
    check(f80_same(actual, expected),
        "%s：实际 %04" PRIx16 ":%016" PRIx64 "，期望 %04" PRIx16 ":%016" PRIx64,
        name, actual.signExp, actual.signif, expected.signExp, expected.signif);
}

static double double_from_bits(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t double_to_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void check_double(const char *name, double actual, uint64_t expected) {
    uint64_t bits = double_to_bits(actual);
    check(bits == expected,
        "%s：实际 %016" PRIx64 "，期望 %016" PRIx64,
        name, bits, expected);
}

static const char *rounding_name(enum f80_rounding_mode mode) {
    switch (mode) {
        case round_to_nearest: return "最近偶数";
        case round_down: return "向下";
        case round_up: return "向上";
        case round_chop: return "向零";
    }
    abort();
}

static void test_fixed_conversions(void) {
    static const struct {
        const char *name;
        uint64_t binary64;
        uint64_t signif;
        uint16_t sign_exp;
    } cases[] = {
        {"正零", UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), 0x0000},
        {"负零", UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000000), 0x8000},
        {"一", UINT64_C(0x3ff0000000000000), UINT64_C(0x8000000000000000), 0x3fff},
        {"负一", UINT64_C(0xbff0000000000000), UINT64_C(0x8000000000000000), 0xbfff},
        {"一百二十三", UINT64_C(0x405ec00000000000), UINT64_C(0xf600000000000000), 0x4005},
        {"最小 binary64 子正常数", UINT64_C(0x0000000000000001), UINT64_C(0x8000000000000000), 0x3bcd},
        {"最大 binary64 子正常数", UINT64_C(0x000fffffffffffff), UINT64_C(0xfffffffffffff000), 0x3c00},
        {"最小 binary64 正常数", UINT64_C(0x0010000000000000), UINT64_C(0x8000000000000000), 0x3c01},
        {"最大 binary64 有限数", UINT64_C(0x7fefffffffffffff), UINT64_C(0xfffffffffffff800), 0x43fe},
        {"正无穷", UINT64_C(0x7ff0000000000000), UINT64_C(0x8000000000000000), 0x7fff},
        {"负无穷", UINT64_C(0xfff0000000000000), UINT64_C(0x8000000000000000), 0xffff},
        {"安静 NaN", UINT64_C(0x7ff8000000000000), UINT64_C(0xc000000000000000), 0x7fff},
    };

    suite_start("固定编码转换");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float80 expected = f80_bits(cases[i].signif, cases[i].sign_exp);
        check_f80(cases[i].name,
            f80_from_double(double_from_bits(cases[i].binary64)), expected);
        for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
            f80_rounding_mode = mode;
            check_double(cases[i].name, f80_to_double(expected), cases[i].binary64);
        }
    }
    suite_end("固定编码转换");
}

static void test_double_rounding(void) {
    static const struct {
        const char *name;
        uint64_t signif;
        uint16_t sign_exp;
        uint64_t expected[4];
    } cases[] = {
        {"最小 x87 子正常正数", UINT64_C(1), 0x0000,
            {UINT64_C(0), UINT64_C(0), UINT64_C(1), UINT64_C(0)}},
        {"最小 x87 子正常负数", UINT64_C(1), 0x8000,
            {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
             UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000)}},
        {"最小 binary64 子正常数的一半", UINT64_C(0x8000000000000000), 0x3bcc,
            {UINT64_C(0), UINT64_C(0), UINT64_C(1), UINT64_C(0)}},
        {"负的最小 binary64 子正常数的一半", UINT64_C(0x8000000000000000), 0xbbcc,
            {UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
             UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000)}},
        {"略大于最小子正常数的一半", UINT64_C(0x8000000000000001), 0x3bcc,
            {UINT64_C(1), UINT64_C(0), UINT64_C(1), UINT64_C(0)}},
        {"一与下一数的中点", UINT64_C(0x8000000000000400), 0x3fff,
            {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000),
             UINT64_C(0x3ff0000000000001), UINT64_C(0x3ff0000000000000)}},
        {"负一与下一数的中点", UINT64_C(0x8000000000000400), 0xbfff,
            {UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000001),
             UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000000)}},
        {"正溢出中点", UINT64_C(0xfffffffffffffc00), 0x43fe,
            {UINT64_C(0x7ff0000000000000), UINT64_C(0x7fefffffffffffff),
             UINT64_C(0x7ff0000000000000), UINT64_C(0x7fefffffffffffff)}},
        {"负溢出中点", UINT64_C(0xfffffffffffffc00), 0xc3fe,
            {UINT64_C(0xfff0000000000000), UINT64_C(0xfff0000000000000),
             UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff)}},
        {"超出 binary64 的正数", UINT64_C(0x8000000000000000), 0x43ff,
            {UINT64_C(0x7ff0000000000000), UINT64_C(0x7fefffffffffffff),
             UINT64_C(0x7ff0000000000000), UINT64_C(0x7fefffffffffffff)}},
        {"超出 binary64 的负数", UINT64_C(0x8000000000000000), 0xc3ff,
            {UINT64_C(0xfff0000000000000), UINT64_C(0xfff0000000000000),
             UINT64_C(0xffefffffffffffff), UINT64_C(0xffefffffffffffff)}},
    };

    suite_start("binary64 定向舍入");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float80 input = f80_bits(cases[i].signif, cases[i].sign_exp);
        for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
            f80_rounding_mode = mode;
            char name[160];
            snprintf(name, sizeof(name), "%s（%s）", cases[i].name, rounding_name(mode));
            check_double(name, f80_to_double(input), cases[i].expected[mode]);
        }
    }
    suite_end("binary64 定向舍入");
}

static void test_integer_conversions(void) {
    static const struct {
        const char *name;
        int64_t input;
        uint64_t signif;
        uint16_t sign_exp;
    } fixed[] = {
        {"零", 0, UINT64_C(0), 0x0000},
        {"一", 1, UINT64_C(0x8000000000000000), 0x3fff},
        {"负一", -1, UINT64_C(0x8000000000000000), 0xbfff},
        {"一百二十三", 123, UINT64_C(0xf600000000000000), 0x4005},
        {"最大整数", INT64_MAX, UINT64_C(0xfffffffffffffffe), 0x403d},
        {"最小整数", INT64_MIN, UINT64_C(0x8000000000000000), 0xc03e},
    };
    static const struct {
        const char *name;
        uint64_t signif;
        uint16_t sign_exp;
        int64_t expected[4];
    } rounded[] = {
        {"正零点七五", UINT64_C(0xc000000000000000), 0x3ffe, {1, 0, 1, 0}},
        {"负零点七五", UINT64_C(0xc000000000000000), 0xbffe, {-1, -1, 0, 0}},
        {"正一点五", UINT64_C(0xc000000000000000), 0x3fff, {2, 1, 2, 1}},
        {"负一点五", UINT64_C(0xc000000000000000), 0xbfff, {-2, -2, -1, -1}},
    };

    suite_start("整数转换");
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        float80 expected = f80_bits(fixed[i].signif, fixed[i].sign_exp);
        check_f80(fixed[i].name, f80_from_int(fixed[i].input), expected);
        for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
            f80_rounding_mode = mode;
            check(f80_to_int(expected) == fixed[i].input, "%s回转失败", fixed[i].name);
        }
    }
    for (size_t i = 0; i < sizeof(rounded) / sizeof(rounded[0]); i++) {
        float80 input = f80_bits(rounded[i].signif, rounded[i].sign_exp);
        for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
            f80_rounding_mode = mode;
            check(f80_to_int(input) == rounded[i].expected[mode],
                "%s（%s）", rounded[i].name, rounding_name(mode));
        }
    }
    check(f80_to_int(f80_bits(UINT64_C(0x8000000000000000), 0x403e)) == INT64_MIN,
        "正向越过 INT64_MAX 未返回 indefinite");
    check(f80_to_int(f80_bits(UINT64_C(0x8000000000000001), 0xc03e)) == INT64_MIN,
        "负向越过 INT64_MIN 未返回 indefinite");
    suite_end("整数转换");
}

static void test_round_to_integer(void) {
    static const struct {
        const char *name;
        float80 input;
        float80 expected[4];
    } cases[] = {
        {"正零点七五", {.signif = UINT64_C(0xc000000000000000), .signExp = 0x3ffe},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
             {.signif = 0, .signExp = 0},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
             {.signif = 0, .signExp = 0}}},
        {"负零点七五", {.signif = UINT64_C(0xc000000000000000), .signExp = 0xbffe},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0xbfff},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0xbfff},
             {.signif = 0, .signExp = 0x8000},
             {.signif = 0, .signExp = 0x8000}}},
        {"正一点五", {.signif = UINT64_C(0xc000000000000000), .signExp = 0x3fff},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff}}},
        {"负一点五", {.signif = UINT64_C(0xc000000000000000), .signExp = 0xbfff},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0xc000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0xc000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0xbfff},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0xbfff}}},
    };

    suite_start("舍入到整数");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
            f80_rounding_mode = mode;
            check_f80(cases[i].name, f80_round(cases[i].input), cases[i].expected[mode]);
        }
    }
    suite_end("舍入到整数");
}

static void test_arithmetic_golden(void) {
    float80 zero = f80_bits(0, 0x0000);
    float80 one = f80_bits(UINT64_C(0x8000000000000000), 0x3fff);
    float80 neg_one = f80_bits(UINT64_C(0x8000000000000000), 0xbfff);
    float80 two = f80_bits(UINT64_C(0x8000000000000000), 0x4000);
    float80 half = f80_bits(UINT64_C(0x8000000000000000), 0x3ffe);
    float80 neg_zero = f80_bits(0, 0x8000);
    float80 max = f80_bits(UINT64_MAX, 0x7ffe);
    float80 min_subnormal = f80_bits(1, 0x0000);
    float80 third_near = f80_bits(UINT64_C(0xaaaaaaaaaaaaaaab), 0x3ffd);
    float80 third_low = f80_bits(UINT64_C(0xaaaaaaaaaaaaaaaa), 0x3ffd);

    suite_start("固定算术向量");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        check_f80("一加一", f80_add(one, one), two);
        check_f80("一减一", f80_sub(one, one),
            f80_bits(0, mode == round_down ? 0x8000 : 0x0000));

        float80 tiny = f80_bits(UINT64_C(0x8000000000000000), 0x3fbf);
        float80 one_next = f80_bits(UINT64_C(0x8000000000000001), 0x3fff);
        check_f80("一加半个末位",
            f80_add(one, tiny), mode == round_up ? one_next : one);
        float80 sticky = f80_bits(UINT64_C(0x8000000000000001), 0x3fbd);
        check_f80("超长对齐保留粘滞位",
            f80_add(one, sticky), mode == round_up ? one_next : one);

        float80 expected_third =
            mode == round_up || mode == round_to_nearest ? third_near : third_low;
        check_f80("一除以三", f80_div(one, f80_from_int(3)), expected_third);
        float80 min_normal_plus_ulp =
            f80_bits(UINT64_C(0x8000000000000001), 0x0001);
        float80 tiny_quotient = f80_bits(
            mode == round_up ? UINT64_MAX : UINT64_C(0xfffffffffffffffe),
            0x3fbf);
        check_f80("最小子正常数除以最小正常数加一末位",
            f80_div(f80_bits(1, 0x0000), min_normal_plus_ulp), tiny_quotient);
        check_f80("一点五乘二", f80_mul(
            f80_bits(UINT64_C(0xc000000000000000), 0x3fff), two),
            f80_bits(UINT64_C(0xc000000000000000), 0x4000));

        float80 overflow = mode == round_down || mode == round_chop ? max : F80_INF;
        check_f80("最大数乘二", f80_mul(max, two), overflow);

        float80 underflow = mode == round_up ? min_subnormal : zero;
        check_f80("最小子正常数乘一半", f80_mul(min_subnormal, half), underflow);
        check_f80("正零乘最大有限数", f80_mul(zero, max), zero);
        check_f80("负零乘最大有限数", f80_mul(neg_zero, max), neg_zero);
        check_f80("正零除最小子正常数", f80_div(zero, min_subnormal), zero);
        check_f80("负零除最小子正常数", f80_div(neg_zero, min_subnormal), neg_zero);
    }

    check(f80_isnan(f80_mul(F80_INF, zero)), "无穷乘零应为 NaN");
    check(f80_isnan(f80_div(zero, zero)), "零除以零应为 NaN");
    check_f80("一除以零", f80_div(one, zero), F80_INF);
    check(!f80_lt(one, neg_one), "一不应小于负一");
    check(f80_lt(neg_one, one), "负一应小于一");
    check(f80_eq(zero, f80_bits(0, 0x8000)), "正负零应相等");
    check(f80_uncomparable(F80_NAN, one), "NaN 应不可比较");
    suite_end("固定算术向量");
}

static void test_scale_golden(void) {
    float80 zero = f80_bits(0, 0x0000);
    float80 neg_zero = f80_bits(0, 0x8000);
    float80 one = f80_bits(UINT64_C(0x8000000000000000), 0x3fff);
    float80 neg_one = f80_bits(UINT64_C(0x8000000000000000), 0xbfff);
    float80 two = f80_bits(UINT64_C(0x8000000000000000), 0x4000);
    float80 half = f80_bits(UINT64_C(0x8000000000000000), 0x3ffe);
    float80 max = f80_bits(UINT64_MAX, 0x7ffe);
    float80 neg_max = f80_bits(UINT64_MAX, 0xfffe);
    float80 min_subnormal = f80_bits(1, 0x0000);
    float80 neg_min_subnormal = f80_bits(1, 0x8000);
    float80 max_subnormal = f80_bits(UINT64_C(0x7fffffffffffffff), 0x0000);
    float80 infinity = F80_INF;
    float80 neg_infinity = f80_bits(UINT64_C(0x8000000000000000), 0xffff);

    suite_start("缩放边界");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        float80 positive_overflow =
            mode == round_down || mode == round_chop ? max : infinity;
        float80 negative_overflow =
            mode == round_up || mode == round_chop ? neg_max : neg_infinity;
        float80 positive_underflow = mode == round_up ? min_subnormal : zero;
        float80 negative_underflow = mode == round_down ? neg_min_subnormal : neg_zero;

        check_f80("最大有限数按 INT_MAX 缩放",
            f80_scale(max, INT_MAX), positive_overflow);
        check_f80("负最大有限数按 INT_MAX 缩放",
            f80_scale(neg_max, INT_MAX), negative_overflow);
        check_f80("精确二次幂按 INT_MAX 缩放",
            f80_scale(one, INT_MAX), positive_overflow);
        check_f80("最小子正常数按 INT_MIN 缩放",
            f80_scale(min_subnormal, INT_MIN), positive_underflow);
        check_f80("负最小子正常数按 INT_MIN 缩放",
            f80_scale(neg_min_subnormal, INT_MIN), negative_underflow);

        check_f80("正零按 INT_MIN 缩放", f80_scale(zero, INT_MIN), zero);
        check_f80("正零按 INT_MAX 缩放", f80_scale(zero, INT_MAX), zero);
        check_f80("负零按 INT_MIN 缩放", f80_scale(neg_zero, INT_MIN), neg_zero);
        check_f80("负零按 INT_MAX 缩放", f80_scale(neg_zero, INT_MAX), neg_zero);
        check_f80("正无穷按 INT_MIN 缩放", f80_scale(infinity, INT_MIN), infinity);
        check_f80("正无穷按 INT_MAX 缩放", f80_scale(infinity, INT_MAX), infinity);
        check_f80("负无穷按 INT_MIN 缩放", f80_scale(neg_infinity, INT_MIN), neg_infinity);
        check_f80("负无穷按 INT_MAX 缩放", f80_scale(neg_infinity, INT_MAX), neg_infinity);
        check(f80_isnan(f80_scale(F80_NAN, INT_MAX)), "NaN 缩放后应保持 NaN 类别");
        check(f80_isnan(f80_scale(f80_bits(0, 0x3fff), INT_MIN)),
            "不受支持编码缩放后应为 NaN");

        check_f80("最小子正常数缩放零", f80_scale(min_subnormal, 0), min_subnormal);
        check_f80("最大子正常数缩放零", f80_scale(max_subnormal, 0), max_subnormal);
        check_f80("最小子正常数放大一位", f80_scale(min_subnormal, 1),
            f80_bits(2, 0x0000));
        check_f80("最小子正常数缩小一位",
            f80_scale(min_subnormal, -1), positive_underflow);
        check_f80("负最小子正常数缩小一位",
            f80_scale(neg_min_subnormal, -1), negative_underflow);

        check(f80_isnan(f80_scale_by_float(one, F80_NAN)),
            "FSCALE 的 ST1 为 NaN 时结果应为 NaN");
        check(f80_isnan(f80_scale_by_float(F80_NAN, one)),
            "FSCALE 的 ST0 为 NaN 时结果应为 NaN");
        check(f80_isnan(f80_scale_by_float(one, f80_bits(0, 0x3fff))),
            "FSCALE 的 ST1 为不受支持编码时结果应为 NaN");
        check(f80_isnan(f80_scale_by_float(f80_bits(0, 0x3fff), one)),
            "FSCALE 的 ST0 为不受支持编码时结果应为 NaN");

        check_f80("一按正无穷缩放", f80_scale_by_float(one, infinity), infinity);
        check_f80("负一按正无穷缩放", f80_scale_by_float(neg_one, infinity), neg_infinity);
        check_f80("一按负无穷缩放", f80_scale_by_float(one, neg_infinity), zero);
        check_f80("负一按负无穷缩放", f80_scale_by_float(neg_one, neg_infinity), neg_zero);
        check(f80_isnan(f80_scale_by_float(zero, infinity)),
            "正零按正无穷缩放应为 NaN");
        check(f80_isnan(f80_scale_by_float(neg_zero, infinity)),
            "负零按正无穷缩放应为 NaN");
        check_f80("正零按负无穷缩放", f80_scale_by_float(zero, neg_infinity), zero);
        check_f80("负零按负无穷缩放", f80_scale_by_float(neg_zero, neg_infinity), neg_zero);
        check_f80("正无穷按正无穷缩放",
            f80_scale_by_float(infinity, infinity), infinity);
        check_f80("负无穷按正无穷缩放",
            f80_scale_by_float(neg_infinity, infinity), neg_infinity);
        check(f80_isnan(f80_scale_by_float(infinity, neg_infinity)),
            "正无穷按负无穷缩放应为 NaN");
        check(f80_isnan(f80_scale_by_float(neg_infinity, neg_infinity)),
            "负无穷按负无穷缩放应为 NaN");

        check_f80("一按最大有限数缩放",
            f80_scale_by_float(one, max), positive_overflow);
        check_f80("一按负最大有限数缩放",
            f80_scale_by_float(one, neg_max), positive_underflow);
        check_f80("正零按最大有限数缩放", f80_scale_by_float(zero, max), zero);
        check_f80("正无穷按负最大有限数缩放",
            f80_scale_by_float(infinity, neg_max), infinity);
        check_f80("一按正一点五缩放", f80_scale_by_float(
            one, f80_bits(UINT64_C(0xc000000000000000), 0x3fff)), two);
        check_f80("一按负一点五缩放", f80_scale_by_float(
            one, f80_bits(UINT64_C(0xc000000000000000), 0xbfff)), half);
        check_f80("一按正 2^63 缩放", f80_scale_by_float(
            one, f80_bits(UINT64_C(0x8000000000000000), 0x403e)), positive_overflow);
        check_f80("一按负 2^63 缩放", f80_scale_by_float(
            one, f80_bits(UINT64_C(0x8000000000000000), 0xc03e)), positive_underflow);
        check(f80_rounding_mode == mode, "FSCALE 转换 ST1 后未恢复舍入模式");
    }
    suite_end("缩放边界");
}

static void test_sqrt_golden(void) {
    static const struct {
        const char *name;
        float80 input;
        float80 expected[4];
    } cases[] = {
        {"最小子正常数", {.signif = 1, .signExp = 0x0000},
            {{.signif = UINT64_C(0xb504f333f9de6484), .signExp = 0x1fe0},
             {.signif = UINT64_C(0xb504f333f9de6484), .signExp = 0x1fe0},
             {.signif = UINT64_C(0xb504f333f9de6485), .signExp = 0x1fe0},
             {.signif = UINT64_C(0xb504f333f9de6484), .signExp = 0x1fe0}}},
        {"最大子正常数", {.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x0000},
            {{.signif = UINT64_MAX, .signExp = 0x1fff},
             {.signif = UINT64_C(0xfffffffffffffffe), .signExp = 0x1fff},
             {.signif = UINT64_MAX, .signExp = 0x1fff},
             {.signif = UINT64_C(0xfffffffffffffffe), .signExp = 0x1fff}}},
        {"最小正常数", {.signif = UINT64_C(0x8000000000000000), .signExp = 0x0001},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000}}},
        {"最小正常数加一末位",
            {.signif = UINT64_C(0x8000000000000001), .signExp = 0x0001},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000},
             {.signif = UINT64_C(0x8000000000000001), .signExp = 0x2000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x2000}}},
        {"二", {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
            {{.signif = UINT64_C(0xb504f333f9de6484), .signExp = 0x3fff},
             {.signif = UINT64_C(0xb504f333f9de6484), .signExp = 0x3fff},
             {.signif = UINT64_C(0xb504f333f9de6485), .signExp = 0x3fff},
             {.signif = UINT64_C(0xb504f333f9de6484), .signExp = 0x3fff}}},
        {"三", {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000},
            {{.signif = UINT64_C(0xddb3d742c265539e), .signExp = 0x3fff},
             {.signif = UINT64_C(0xddb3d742c265539d), .signExp = 0x3fff},
             {.signif = UINT64_C(0xddb3d742c265539e), .signExp = 0x3fff},
             {.signif = UINT64_C(0xddb3d742c265539d), .signExp = 0x3fff}}},
        {"最大有限数", {.signif = UINT64_MAX, .signExp = 0x7ffe},
            {{.signif = UINT64_MAX, .signExp = 0x5ffe},
             {.signif = UINT64_MAX, .signExp = 0x5ffe},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x5fff},
             {.signif = UINT64_MAX, .signExp = 0x5ffe}}},
        {"一的前一数", {.signif = UINT64_MAX, .signExp = 0x3ffe},
            {{.signif = UINT64_MAX, .signExp = 0x3ffe},
             {.signif = UINT64_MAX, .signExp = 0x3ffe},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
             {.signif = UINT64_MAX, .signExp = 0x3ffe}}},
        {"一的后一数", {.signif = UINT64_C(0x8000000000000001), .signExp = 0x3fff},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
             {.signif = UINT64_C(0x8000000000000001), .signExp = 0x3fff},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff}}},
        {"四的前一数", {.signif = UINT64_MAX, .signExp = 0x4000},
            {{.signif = UINT64_MAX, .signExp = 0x3fff},
             {.signif = UINT64_MAX, .signExp = 0x3fff},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_MAX, .signExp = 0x3fff}}},
        {"四", {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4001},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000}}},
        {"四的后一数", {.signif = UINT64_C(0x8000000000000001), .signExp = 0x4001},
            {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000001), .signExp = 0x4000},
             {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000}}},
    };

    suite_start("平方根边界");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            check_f80(cases[i].name, f80_sqrt(cases[i].input), cases[i].expected[mode]);

        float80 zero = f80_bits(0, 0x0000);
        float80 neg_zero = f80_bits(0, 0x8000);
        float80 infinity = F80_INF;
        check_f80("正零平方根", f80_sqrt(zero), zero);
        check_f80("负零平方根", f80_sqrt(neg_zero), neg_zero);
        check_f80("正无穷平方根", f80_sqrt(infinity), infinity);
        check(f80_isnan(f80_sqrt(f80_bits(
            UINT64_C(0x8000000000000000), 0xbfff))), "负有限数平方根应为 NaN");
        check(f80_isnan(f80_sqrt(f80_bits(
            UINT64_C(0x8000000000000000), 0xffff))), "负无穷平方根应为 NaN");
        check(f80_isnan(f80_sqrt(F80_NAN)), "NaN 平方根应为 NaN");
        check(f80_isnan(f80_sqrt(f80_bits(0, 0x3fff))),
            "不受支持编码平方根应为 NaN");
    }
    suite_end("平方根边界");
}

static uint64_t random_state = UINT64_C(0x243f6a8885a308d3);

static uint64_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static void test_portable_properties(void) {
    suite_start("固定种子可移植性质");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        random_state = UINT64_C(0x243f6a8885a308d3) ^ (uint64_t) mode;
        for (int i = 0; i < 20000; i++) {
            uint64_t bits = next_random();
            double input = double_from_bits(bits);
            uint64_t output = double_to_bits(f80_to_double(f80_from_double(input)));
            check(output == bits, "binary64 往返：%016" PRIx64 " -> %016" PRIx64, bits, output);

            int64_t integer = (int64_t) next_random();
            check(f80_to_int(f80_from_int(integer)) == integer,
                "整数往返：%" PRId64, integer);

            uint64_t signif = next_random() | (UINT64_C(1) << 63);
            uint16_t exp = (uint16_t) (1 + next_random() % 0x7ffe);
            uint16_t sign = (uint16_t) ((next_random() & 1) << 15);
            float80 value = f80_bits(signif, (uint16_t) (exp | sign));
            float80 zero = f80_bits(0, 0);
            float80 one = f80_bits(UINT64_C(0x8000000000000000), 0x3fff);
            check_f80("加零恒等", f80_add(value, zero), value);
            check_f80("乘一恒等", f80_mul(value, one), value);
            check_f80("双重取反", f80_neg(f80_neg(value)), value);
        }
    }
    suite_end("固定种子可移植性质");
}

#if HAVE_NATIVE_X87
static long double native_from_f80(float80 value) {
    long double native = 0;
    memcpy(&native, &value, 10);
    return native;
}

static float80 f80_from_native(long double value) {
    float80 result = {0};
    memcpy(&result, &value, 10);
    return result;
}

static float80 native_x87_fscale(float80 x, float80 scale) {
    long double native_x = native_from_f80(x);
    long double native_scale = native_from_f80(scale);
    long double native_result;
    __asm__ volatile(
        "fldt %[scale]\n\t"
        "fldt %[x]\n\t"
        "fscale\n\t"
        "fstpt %[result]\n\t"
        "fstp %%st(0)"
        : [result] "=m" (native_result)
        : [x] "m" (native_x), [scale] "m" (native_scale)
        : "st");
    return f80_from_native(native_result);
}

static float80 native_x87_sqrt(float80 x) {
    long double native_x = native_from_f80(x);
    long double native_result;
    __asm__ volatile(
        "fldt %[x]\n\t"
        "fsqrt\n\t"
        "fstpt %[result]"
        : [result] "=m" (native_result)
        : [x] "m" (native_x)
        : "st");
    return f80_from_native(native_result);
}

static bool native_result_same(float80 actual, float80 expected) {
    return (f80_isnan(actual) && f80_isnan(expected)) || f80_same(actual, expected);
}

static int native_rounding_mode(enum f80_rounding_mode mode) {
    switch (mode) {
        case round_to_nearest: return FE_TONEAREST;
        case round_down: return FE_DOWNWARD;
        case round_up: return FE_UPWARD;
        case round_chop: return FE_TOWARDZERO;
    }
    abort();
}

static void test_native_x87_oracle(void) {
    static const struct {
        float80 input;
        int scale;
    } scale_cases[] = {
        {{.signif = UINT64_MAX, .signExp = 0x7ffe}, INT_MAX},
        {{.signif = UINT64_MAX, .signExp = 0xfffe}, INT_MAX},
        {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff}, INT_MAX},
        {{.signif = 1, .signExp = 0x0000}, INT_MIN},
        {{.signif = 1, .signExp = 0x8000}, INT_MIN},
        {{.signif = 0, .signExp = 0x0000}, INT_MIN},
        {{.signif = 0, .signExp = 0x8000}, INT_MAX},
        {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x7fff}, INT_MIN},
        {{.signif = UINT64_C(0x8000000000000000), .signExp = 0xffff}, INT_MAX},
        {{.signif = 1, .signExp = 0x0000}, 0},
        {{.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x0000}, 0},
        {{.signif = 1, .signExp = 0x0000}, 1},
        {{.signif = 1, .signExp = 0x0000}, -1},
        {{.signif = 1, .signExp = 0x8000}, -1},
    };
    static const float80 fscale_values[] = {
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
        {.signif = 0, .signExp = 0x0000},
        {.signif = 0, .signExp = 0x8000},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x7fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0xffff},
        {.signif = UINT64_C(0xc000000000000000), .signExp = 0x7fff},
        {.signif = 0, .signExp = 0x3fff},
    };
    static const float80 fscale_scales[] = {
        {.signif = UINT64_C(0xc000000000000000), .signExp = 0x7fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x7fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0xffff},
        {.signif = UINT64_MAX, .signExp = 0x7ffe},
        {.signif = UINT64_MAX, .signExp = 0xfffe},
        {.signif = UINT64_C(0xc000000000000000), .signExp = 0x3fff},
        {.signif = UINT64_C(0xc000000000000000), .signExp = 0xbfff},
        {.signif = 0, .signExp = 0x3fff},
    };
    static const float80 division_values[] = {
        {.signif = 1, .signExp = 0x0000},
        {.signif = UINT64_C(0x0123456789abcdef), .signExp = 0x0000},
        {.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x0000},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x0001},
        {.signif = UINT64_C(0x8000000000000001), .signExp = 0x0001},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
        {.signif = UINT64_MAX, .signExp = 0x7ffe},
    };
    static const float80 sqrt_values[] = {
        {.signif = 1, .signExp = 0x0000},
        {.signif = UINT64_C(0x0123456789abcdef), .signExp = 0x0000},
        {.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x0000},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x0001},
        {.signif = UINT64_C(0x8000000000000001), .signExp = 0x0001},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
        {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000},
        {.signif = UINT64_MAX, .signExp = 0x7ffe},
        {.signif = 0, .signExp = 0x0000},
        {.signif = 0, .signExp = 0x8000},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x7fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0xffff},
        {.signif = UINT64_C(0xc000000000000000), .signExp = 0x7fff},
        {.signif = 0, .signExp = 0x3fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0xbfff},
    };

    suite_start("原生 x87 参考");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        int native_mode = native_rounding_mode(mode);
        check(fesetround(native_mode) == 0 && fegetround() == native_mode,
            "无法设置宿主舍入模式：%s", rounding_name(mode));
        for (size_t i = 0; i < sizeof(scale_cases) / sizeof(scale_cases[0]); i++) {
            volatile long double native_input = native_from_f80(scale_cases[i].input);
            long double native_scaled = scalbnl(native_input, scale_cases[i].scale);
            check(native_result_same(
                    f80_scale(scale_cases[i].input, scale_cases[i].scale),
                    f80_from_native(native_scaled)),
                "x87 缩放不一致（%s，scale=%d）",
                rounding_name(mode), scale_cases[i].scale);
        }
        for (size_t x = 0; x < sizeof(fscale_values) / sizeof(fscale_values[0]); x++) {
            for (size_t scale = 0;
                    scale < sizeof(fscale_scales) / sizeof(fscale_scales[0]); scale++) {
                float80 expected = native_x87_fscale(
                    fscale_values[x], fscale_scales[scale]);
                float80 actual = f80_scale_by_float(
                    fscale_values[x], fscale_scales[scale]);
                check(native_result_same(actual, expected),
                    "x87 FSCALE 不一致（%s，x=%zu，scale=%zu）",
                    rounding_name(mode), x, scale);
            }
        }
        for (size_t left = 0;
                left < sizeof(division_values) / sizeof(division_values[0]); left++) {
            for (size_t right = 0;
                    right < sizeof(division_values) / sizeof(division_values[0]); right++) {
                volatile long double native_left = native_from_f80(division_values[left]);
                volatile long double native_right = native_from_f80(division_values[right]);
                float80 expected = f80_from_native(native_left / native_right);
                float80 actual = f80_div(division_values[left], division_values[right]);
                check(native_result_same(actual, expected),
                    "x87 固定除法不一致（%s，left=%zu，right=%zu）",
                    rounding_name(mode), left, right);
            }
        }
        for (size_t i = 0; i < sizeof(sqrt_values) / sizeof(sqrt_values[0]); i++) {
            float80 expected = native_x87_sqrt(sqrt_values[i]);
            float80 actual = f80_sqrt(sqrt_values[i]);
            check(native_result_same(actual, expected),
                "x87 固定平方根不一致（%s，序号=%zu）", rounding_name(mode), i);
        }
        random_state = UINT64_C(0x082efa98ec4e6c89) ^ (uint64_t) mode;
        for (int i = 0; i < 50000; i++) {
            uint64_t signif = next_random();
            float80 value;
            if (i & 1) {
                value = f80_bits(signif | (UINT64_C(1) << 63),
                    (uint16_t) (1 + next_random() % 0x7ffe));
            } else {
                signif &= UINT64_MAX >> 1;
                value = f80_bits(signif == 0 ? 1 : signif, 0x0000);
            }
            float80 expected = native_x87_sqrt(value);
            float80 actual = f80_sqrt(value);
            check(native_result_same(actual, expected),
                "x87 随机平方根不一致（%s，序号=%d）", rounding_name(mode), i);
        }
        random_state = UINT64_C(0xa4093822299f31d0) ^ (uint64_t) mode;
        for (int i = 0; i < 50000; i++) {
            uint64_t left_signif = next_random();
            uint64_t right_signif = next_random();
            uint16_t left_sign = (uint16_t) ((next_random() & 1) << 15);
            uint16_t right_sign = (uint16_t) ((next_random() & 1) << 15);
            float80 left;
            float80 right;
            if (i & 1) {
                left = f80_bits(left_signif | (UINT64_C(1) << 63),
                    (uint16_t) (1 + next_random() % 0x7ffe) | left_sign);
            } else {
                left_signif &= UINT64_MAX >> 1;
                left = f80_bits(left_signif == 0 ? 1 : left_signif, left_sign);
            }
            if (i & 2) {
                right = f80_bits(right_signif | (UINT64_C(1) << 63),
                    (uint16_t) (1 + next_random() % 0x7ffe) | right_sign);
            } else {
                right_signif &= UINT64_MAX >> 1;
                right = f80_bits(right_signif == 0 ? 1 : right_signif, right_sign);
            }
            volatile long double native_left = native_from_f80(left);
            volatile long double native_right = native_from_f80(right);
            float80 expected = f80_from_native(native_left / native_right);
            float80 actual = f80_div(left, right);
            check(native_result_same(actual, expected),
                "x87 随机除法不一致（%s，序号=%d）", rounding_name(mode), i);
        }
        random_state = UINT64_C(0x13198a2e03707344) ^ (uint64_t) mode;
        for (int i = 0; i < 20000; i++) {
            float80 a = f80_bits(next_random() | (UINT64_C(1) << 63),
                (uint16_t) (1 + next_random() % 0x7ffe) |
                (uint16_t) ((next_random() & 1) << 15));
            float80 b = f80_bits(next_random() | (UINT64_C(1) << 63),
                (uint16_t) (1 + next_random() % 0x7ffe) |
                (uint16_t) ((next_random() & 1) << 15));
            volatile long double native_a = native_from_f80(a);
            volatile long double native_b = native_from_f80(b);
            long double native_result = native_a + native_b;
            check(native_result_same(f80_add(a, b), f80_from_native(native_result)),
                "x87 加法不一致（%s）", rounding_name(mode));
            native_result = native_a * native_b;
            check(native_result_same(f80_mul(a, b), f80_from_native(native_result)),
                "x87 乘法不一致（%s）", rounding_name(mode));
            native_result = native_a / native_b;
            check(native_result_same(f80_div(a, b), f80_from_native(native_result)),
                "x87 除法不一致（%s）", rounding_name(mode));
            volatile double narrowed = (double) native_a;
            check(double_to_bits(f80_to_double(a)) == double_to_bits(narrowed),
                "x87 转 binary64 不一致（%s）", rounding_name(mode));
            native_result = rintl(native_a);
            check(native_result_same(f80_round(a), f80_from_native(native_result)),
                "x87 舍入到整数不一致（%s）", rounding_name(mode));
            check(f80_lt(a, b) == (native_a < native_b),
                "x87 小于比较不一致（%s）", rounding_name(mode));
            check(f80_eq(a, b) == (native_a == native_b),
                "x87 相等比较不一致（%s）", rounding_name(mode));
        }
    }
    suite_end("原生 x87 参考");
}
#endif

int main(void) {
    test_fixed_conversions();
    test_double_rounding();
    test_integer_conversions();
    test_round_to_integer();
    test_arithmetic_golden();
    test_scale_golden();
    test_sqrt_golden();
    test_portable_properties();
#if HAVE_NATIVE_X87
    test_native_x87_oracle();
#endif
    printf("总计：%d/%d 通过\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}

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

#include "cpu.h"
#include "float80.h"
#include "fpu.h"

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

static void check_mod_result(const char *name, struct f80_mod_result actual,
        float80 expected, uint8_t quotient_low, bool quotient_valid) {
    check_f80(name, actual.value, expected);
    check(actual.quotient_valid == quotient_valid,
        "%s：商有效性实际为 %d，期望为 %d",
        name, actual.quotient_valid, quotient_valid);
    if (quotient_valid) {
        check(actual.quotient_low == quotient_low,
            "%s：商低三位实际为 %u，期望为 %u",
            name, (unsigned) actual.quotient_low, (unsigned) quotient_low);
    }
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
    check(f80_gt(one, neg_one), "一应大于负一");
    check(f80_eq(zero, f80_bits(0, 0x8000)), "正负零应相等");
    check(f80_uncomparable(F80_NAN, one), "NaN 应不可比较");
    check(!f80_gt(F80_NAN, one) && !f80_gt(one, F80_NAN),
        "NaN 不得参与大于关系");
    float80 unsupported = f80_bits(0, 0x3fff);
    check(!f80_gt(unsupported, one) && !f80_gt(one, unsupported),
        "不受支持编码不得参与大于关系");
    suite_end("固定算术向量");
}

static void test_add_subnormal_golden(void) {
    float80 zero = f80_bits(0, 0x0000);
    float80 neg_zero = f80_bits(0, 0x8000);
    float80 min_subnormal = f80_bits(1, 0x0000);
    float80 neg_min_subnormal = f80_bits(1, 0x8000);
    float80 max_subnormal = f80_bits(UINT64_C(0x7fffffffffffffff), 0x0000);
    float80 min_normal = f80_bits(UINT64_C(0x8000000000000000), 0x0001);
    float80 exp_two = f80_bits(UINT64_C(0x8000000000000000), 0x0002);
    float80 exp_two_odd = f80_bits(UINT64_C(0x8000000000000001), 0x0002);
    float80 neg_exp_two = f80_bits(UINT64_C(0x8000000000000000), 0x8002);

    suite_start("正常数与子正常数加减边界");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        check_f80("最小正常数加最大子正常数",
            f80_add(min_normal, max_subnormal),
            f80_bits(UINT64_MAX, 0x0001));
        check_f80("交换操作数后加法一致",
            f80_add(max_subnormal, min_normal),
            f80_bits(UINT64_MAX, 0x0001));
        check_f80("最小正常数减最大子正常数",
            f80_sub(min_normal, max_subnormal), min_subnormal);
        check_f80("最小正常数加最小子正常数",
            f80_add(min_normal, min_subnormal),
            f80_bits(UINT64_C(0x8000000000000001), 0x0001));
        check_f80("最小正常数减最小子正常数",
            f80_sub(min_normal, min_subnormal), max_subnormal);
        check_f80("最大子正常数加最小子正常数",
            f80_add(max_subnormal, min_subnormal), min_normal);
        check_f80("两个最大子正常数相加",
            f80_add(max_subnormal, max_subnormal),
            f80_bits(UINT64_C(0xfffffffffffffffe), 0x0001));

        check_f80("偶数末位正常数加半末位",
            f80_add(exp_two, min_subnormal),
            f80_bits(mode == round_up
                ? UINT64_C(0x8000000000000001)
                : UINT64_C(0x8000000000000000), 0x0002));
        check_f80("奇数末位正常数加半末位",
            f80_add(exp_two_odd, min_subnormal),
            f80_bits(mode == round_to_nearest || mode == round_up
                ? UINT64_C(0x8000000000000002)
                : UINT64_C(0x8000000000000001), 0x0002));
        check_f80("奇数末位正常数减半末位",
            f80_sub(exp_two_odd, min_subnormal),
            f80_bits(mode == round_up
                ? UINT64_C(0x8000000000000001)
                : UINT64_C(0x8000000000000000), 0x0002));
        check_f80("负正常数远离零的半末位",
            f80_sub(neg_exp_two, min_subnormal),
            f80_bits(mode == round_down
                ? UINT64_C(0x8000000000000001)
                : UINT64_C(0x8000000000000000), 0x8002));
        check_f80("正常数与负子正常数跨边界抵消",
            f80_add(exp_two, neg_min_subnormal),
            f80_bits(UINT64_MAX, 0x0001));

        float80 cancelled_zero = mode == round_down ? neg_zero : zero;
        check_f80("正负零相加", f80_add(zero, neg_zero), cancelled_zero);
        check_f80("负正零相加", f80_add(neg_zero, zero), cancelled_zero);
        check_f80("正零减正零", f80_sub(zero, zero), cancelled_zero);
        check_f80("负零减负零", f80_sub(neg_zero, neg_zero), cancelled_zero);
        check_f80("两个负零相加", f80_add(neg_zero, neg_zero), neg_zero);
    }
    suite_end("正常数与子正常数加减边界");
}

static void check_mod_case(const char *name, float80 x, float80 y,
        float80 expected, uint8_t quotient_low, bool quotient_valid,
        enum f80_rounding_mode mode) {
    char label[192];
    snprintf(label, sizeof(label), "%s（%s）", name, rounding_name(mode));
    check_mod_result(label, f80_mod(x, y), expected, quotient_low, quotient_valid);
}

static void check_fpu_prem_case(const char *name, float80 x, float80 y,
        float80 expected, uint8_t quotient_low, bool quotient_valid,
        enum f80_rounding_mode mode) {
    struct cpu_state cpu = {0};
    cpu.fp[0] = x;
    cpu.fp[1] = y;
    cpu.c0 = 1;
    cpu.c1 = 0;
    cpu.c2 = 1;
    cpu.c3 = 1;

    fpu_prem(&cpu);

    char label[192];
    snprintf(label, sizeof(label), "%s（%s）", name, rounding_name(mode));
    check_f80(label, cpu.fp[0], expected);
    check(cpu.c2 == 0, "%s：完成态 C2 未清零", label);
    if (quotient_valid) {
        check(cpu.c0 == ((quotient_low >> 2) & 1) &&
                cpu.c3 == ((quotient_low >> 1) & 1) &&
                cpu.c1 == (quotient_low & 1),
            "%s：商位实际 C0=%u C3=%u C1=%u，期望商低三位为 %u",
            label, cpu.c0, cpu.c3, cpu.c1, (unsigned) quotient_low);
    } else {
        check(cpu.c0 == 1 && cpu.c1 == 0 && cpu.c3 == 1,
            "%s：无效商改写了 C0/C1/C3", label);
    }
}

static void test_mod_golden(void) {
    float80 zero = f80_bits(0, 0x0000);
    float80 neg_zero = f80_bits(0, 0x8000);
    float80 one = f80_from_int(1);
    float80 neg_one = f80_from_int(-1);
    float80 two = f80_from_int(2);
    float80 neg_two = f80_from_int(-2);
    float80 three = f80_from_int(3);
    float80 neg_three = f80_from_int(-3);
    float80 seven = f80_from_int(7);
    float80 neg_seven = f80_from_int(-7);
    float80 ten = f80_from_int(10);
    float80 min_subnormal = f80_bits(1, 0x0000);
    float80 max_subnormal = f80_bits(UINT64_C(0x7fffffffffffffff), 0x0000);
    float80 min_normal = f80_bits(UINT64_C(0x8000000000000000), 0x0001);
    float80 positive_inf = F80_INF;
    float80 negative_inf = f80_bits(UINT64_C(0x8000000000000000), 0xffff);
    float80 positive_nan = f80_bits(UINT64_C(0xd000000000000001), 0x7fff);
    float80 negative_nan = f80_bits(UINT64_C(0xe000000000000002), 0xffff);
    float80 unsupported = f80_bits(0, 0x3fff);
    float80 blocker = f80_bits(UINT64_C(0x8000000000000003), 0x403f);
    float80 max_finite = f80_bits(UINT64_MAX, 0x7ffe);

    suite_start("精确截断余数与 FPREM 商位");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        check_mod_case("正零除以有限数", zero, three, zero, 0, true, mode);
        check_mod_case("负零除以有限数", neg_zero, neg_three, neg_zero, 0, true, mode);
        check_mod_case("绝对值小于除数", two, neg_three, two, 0, true, mode);
        check_mod_case("负数绝对值小于除数", neg_two, three, neg_two, 0, true, mode);
        check_mod_case("相等正数", three, three, zero, 1, true, mode);
        check_mod_case("相等负被除数", neg_three, three, neg_zero, 1, true, mode);
        check_mod_case("正七模正三", seven, three, one, 2, true, mode);
        check_mod_case("正七模负三", seven, neg_three, one, 2, true, mode);
        check_mod_case("负七模正三", neg_seven, three, neg_one, 2, true, mode);
        check_mod_case("负七模负三", neg_seven, neg_three, neg_one, 2, true, mode);
        check_mod_case("十模三", ten, three, one, 3, true, mode);
        check_mod_case("大商低位保真", blocker, three, one, 7, true, mode);
        check_mod_case("正常数跨入子正常余数", min_normal, max_subnormal,
            min_subnormal, 1, true, mode);
        check_mod_case("子正常数小于最小正常数", max_subnormal, min_normal,
            max_subnormal, 0, true, mode);
        check_mod_case("子正常整数单位整除", max_subnormal, min_subnormal,
            zero, 7, true, mode);
        check_mod_case("偶数公因子", f80_from_int(48), f80_from_int(18),
            f80_from_int(12), 2, true, mode);
        check_mod_case("最大指数差", max_finite, f80_bits(7, 0x0000),
            f80_bits(4, 0x0000), 4, true, mode);

        check_mod_case("不受支持被除数", unsupported, three,
            F80_NAN, 0, false, mode);
        check_mod_case("不受支持除数", three, unsupported,
            F80_NAN, 0, false, mode);
        check_mod_case("被除数 NaN 传播", negative_nan, three,
            negative_nan, 0, false, mode);
        check_mod_case("除数 NaN 传播", three, positive_nan,
            positive_nan, 0, false, mode);
        check_mod_case("双 NaN 选择正号除数", negative_nan, positive_nan,
            positive_nan, 0, false, mode);
        check_mod_case("双 NaN 保留被除数", positive_nan, negative_nan,
            positive_nan, 0, false, mode);
        check_mod_case("无穷被除数", positive_inf, three,
            F80_NAN, 0, false, mode);
        check_mod_case("负无穷被除数", negative_inf, neg_three,
            F80_NAN, 0, false, mode);
        check_mod_case("正零除数", three, zero,
            F80_NAN, 0, false, mode);
        check_mod_case("负零除数", three, neg_zero,
            F80_NAN, 0, false, mode);
        check_mod_case("有限数模正无穷", three, positive_inf,
            three, 0, true, mode);
        check_mod_case("有限数模负无穷", neg_three, negative_inf,
            neg_three, 0, true, mode);

        for (uint8_t quotient = 0; quotient < 8; quotient++) {
            char name[96];
            snprintf(name, sizeof(name), "FPREM 商位映射 %u", (unsigned) quotient);
            check_fpu_prem_case(name, f80_from_int((int64_t) quotient * 3 + 1),
                three, one, quotient, true, mode);
        }
        check_fpu_prem_case("FPREM 无效商保持条件位", positive_inf, three,
            F80_NAN, 0, false, mode);
    }
    suite_end("精确截断余数与 FPREM 商位");
}

static float80 run_fyl2x(float80 x, float80 y) {
    struct cpu_state cpu = {0};
    cpu.fp[0] = x;
    cpu.fp[1] = y;
    fpu_yl2x(&cpu);
    check(cpu.top == 1, "FYL2X 应弹出一个栈元素");
    return cpu.fp[cpu.top];
}

static void test_log2_golden(void) {
    float80 zero = f80_bits(0, 0x0000);
    float80 neg_zero = f80_bits(0, 0x8000);
    float80 one = f80_from_int(1);
    float80 neg_one = f80_from_int(-1);
    float80 two = f80_from_int(2);
    float80 neg_two = f80_from_int(-2);
    float80 three = f80_from_int(3);
    float80 half = f80_bits(UINT64_C(0x8000000000000000), 0x3ffe);
    float80 positive_inf = F80_INF;
    float80 negative_inf = f80_bits(UINT64_C(0x8000000000000000), 0xffff);
    float80 unsupported = f80_bits(0, 0x3fff);
    float80 signaling_nan = f80_bits(UINT64_C(0x8000000000000042), 0x7fff);
    float80 neg_signaling_nan = f80_bits(UINT64_C(0x8000000000001234), 0xffff);
    float80 quiet_nan = f80_bits(UINT64_C(0xd000000000000077), 0x7fff);
    float80 expected_signaling_nan =
        f80_bits(UINT64_C(0xc000000000000042), 0x7fff);
    float80 expected_neg_signaling_nan =
        f80_bits(UINT64_C(0xc000000000001234), 0xffff);

    suite_start("二进制对数与 FYL2X 边界");
    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        check_f80("不受支持编码的对数", f80_log2(unsupported), F80_NAN);
        check_f80("正信号 NaN 静默并保留载荷",
            f80_log2(signaling_nan), expected_signaling_nan);
        check_f80("负信号 NaN 静默并保留符号载荷",
            f80_log2(neg_signaling_nan), expected_neg_signaling_nan);
        check_f80("安静 NaN 保持编码", f80_log2(quiet_nan), quiet_nan);
        check_f80("正零的对数", f80_log2(zero), negative_inf);
        check_f80("负零的对数", f80_log2(neg_zero), negative_inf);
        check_f80("负有限数的对数", f80_log2(neg_one), F80_NAN);
        check_f80("负无穷的对数", f80_log2(negative_inf), F80_NAN);
        check_f80("正无穷的对数", f80_log2(positive_inf), positive_inf);
        check_f80("一的对数", f80_log2(one), zero);
        check_f80("二的对数", f80_log2(two), one);
        check_f80("二分之一的对数", f80_log2(half), neg_one);
        check_f80("最小子正常数的对数", f80_log2(f80_bits(1, 0x0000)),
            f80_from_int(-16445));
        check_f80("最小正常数的对数",
            f80_log2(f80_bits(UINT64_C(0x8000000000000000), 0x0001)),
            f80_from_int(-16382));
        check_f80("最大有限二次幂的对数",
            f80_log2(f80_bits(UINT64_C(0x8000000000000000), 0x7ffe)),
            f80_from_int(16383));

        float80 log_one_and_quarter = f80_log2(
            f80_bits(UINT64_C(0xa000000000000000), 0x3fff));
        float80 log_one_and_half = f80_log2(
            f80_bits(UINT64_C(0xc000000000000000), 0x3fff));
        float80 log_one_and_three_quarters = f80_log2(
            f80_bits(UINT64_C(0xe000000000000000), 0x3fff));
        check(f80_lt(log_one_and_quarter, log_one_and_half) &&
                f80_lt(log_one_and_half, log_one_and_three_quarters),
            "[1,2) 内的对数应严格递增（%s）", rounding_name(mode));
        check(!f80_log2(f80_bits(UINT64_C(0x8000000000000001), 0x3fff)).sign,
            "一的后继数对数应为正（%s）", rounding_name(mode));
        check(f80_log2(f80_bits(UINT64_MAX, 0x3ffe)).sign,
            "一的前驱数对数应为负（%s）", rounding_name(mode));

        check_f80("FYL2X 精确有限值", run_fyl2x(two, three), three);
        check_f80("FYL2X 负精确有限值", run_fyl2x(half, three), f80_from_int(-3));
        check_f80("FYL2X 正零输入", run_fyl2x(zero, two), negative_inf);
        check_f80("FYL2X 负零输入与负乘数", run_fyl2x(neg_zero, neg_two), positive_inf);
        check(f80_isnan(run_fyl2x(zero, zero)), "FYL2X 的零乘负无穷应为 NaN");
        check_f80("FYL2X 正无穷与负乘数",
            run_fyl2x(positive_inf, neg_two), negative_inf);
        check(f80_isnan(run_fyl2x(one, positive_inf)),
            "FYL2X 的无穷乘零应为 NaN");
        check(f80_isnan(run_fyl2x(neg_one, two)),
            "FYL2X 的负对数输入应为 NaN");
        check_f80("FYL2X 传播静默后的 NaN 载荷",
            run_fyl2x(signaling_nan, one), expected_signaling_nan);
    }

    for (enum f80_rounding_mode mode = round_to_nearest; mode <= round_chop; mode++) {
        f80_rounding_mode = mode;
        for (unsigned exp = 1; exp <= 0x7ffe; exp++) {
            float80 power = f80_bits(
                UINT64_C(0x8000000000000000), (uint16_t) exp);
            check(f80_same(f80_log2(power), f80_from_int((int) exp - 0x3fff)),
                "正常二次幂对数不精确（%s，存储指数=%u）",
                rounding_name(mode), exp);
        }
        for (int bit = 0; bit < 63; bit++) {
            float80 power = f80_bits(UINT64_C(1) << bit, 0x0000);
            check(f80_same(f80_log2(power), f80_from_int(-16445 + bit)),
                "子正常二次幂对数不精确（%s，位=%d）",
                rounding_name(mode), bit);
        }
    }
    suite_end("二进制对数与 FYL2X 边界");
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

static void test_log2_properties(void) {
    suite_start("二进制对数固定种子性质");
    random_state = UINT64_C(0x3bd39e10cb0ef593);
    for (int i = 0; i < 5000; i++) {
        uint64_t first_signif = next_random();
        uint64_t second_signif = next_random();
        uint16_t exp;
        if (i & 1) {
            first_signif |= UINT64_C(1) << 63;
            second_signif |= UINT64_C(1) << 63;
            exp = (uint16_t) (1 + next_random() % 0x7ffe);
        } else {
            first_signif &= UINT64_MAX >> 1;
            second_signif &= UINT64_MAX >> 1;
            if (first_signif == 0)
                first_signif = 1;
            if (second_signif == 0)
                second_signif = 1;
            exp = 0;
        }
        if (first_signif > second_signif) {
            uint64_t temporary = first_signif;
            first_signif = second_signif;
            second_signif = temporary;
        }
        float80 lower_input = f80_bits(first_signif, exp);
        float80 upper_input = f80_bits(second_signif, exp);
        float80 rounded[4];
        int shift = __builtin_clzll((unsigned long long) lower_input.signif);
        int integer = (int) (lower_input.exp == 0 ? 1 : lower_input.exp) -
            0x3fff - shift;
        float80 lower_bound = f80_from_int(integer);
        float80 upper_bound = f80_from_int(integer + 1);

        for (enum f80_rounding_mode mode = round_to_nearest;
                mode <= round_chop; mode++) {
            f80_rounding_mode = mode;
            rounded[mode] = f80_log2(lower_input);
            check_f80("相同输入结果应确定",
                f80_log2(lower_input), rounded[mode]);
            check(!f80_lt(rounded[mode], lower_bound) &&
                    !f80_gt(rounded[mode], upper_bound),
                "对数结果超出规格化指数区间（%s，序号=%d）",
                rounding_name(mode), i);
        }
        check(!f80_gt(rounded[round_down], rounded[round_to_nearest]) &&
                !f80_gt(rounded[round_to_nearest], rounded[round_up]),
            "对数定向舍入次序错误（序号=%d）", i);
        enum f80_rounding_mode chop_peer = rounded[round_to_nearest].sign
            ? round_up : round_down;
        check(f80_eq(rounded[round_chop], rounded[chop_peer]),
            "对数向零舍入与同向模式不一致（序号=%d）", i);

        f80_rounding_mode = round_to_nearest;
        check(!f80_gt(f80_log2(lower_input), f80_log2(upper_input)),
            "对数单调性错误（序号=%d）", i);
    }

    f80_rounding_mode = round_to_nearest;
    random_state = UINT64_C(0xc0ac29b7c97c50dd);
    for (int i = 0; i < 10000; i++) {
        uint64_t bits;
        switch (i & 3) {
            case 0:
                bits = ((UINT64_C(1) + next_random() % 0x7fe) << 52) |
                    (next_random() & UINT64_C(0x000fffffffffffff));
                break;
            case 1:
                bits = next_random() & UINT64_C(0x000fffffffffffff);
                if (bits == 0)
                    bits = 1;
                break;
            case 2: {
                uint64_t distance = 1 + next_random() % 0xffff;
                bits = i & 4
                    ? UINT64_C(0x3ff0000000000000) - distance
                    : UINT64_C(0x3ff0000000000000) + distance;
                break;
            }
            default:
                bits = (UINT64_C(1) + next_random() % 0x7fe) << 52;
                break;
        }
        double input = double_from_bits(bits);
        double expected = log2(input);
        double actual = f80_to_double(f80_log2(f80_from_double(input)));
        double scale = fabs(expected);
        if (scale < 1)
            scale = 1;
        check(fabs(actual - expected) <= 8 * DBL_EPSILON * scale,
            "binary64 对数参考误差超限（序号=%d，输入=%016" PRIx64 "）",
            i, bits);
    }
    suite_end("二进制对数固定种子性质");
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

static float80 native_x87_add(float80 left, float80 right) {
    long double native_left = native_from_f80(left);
    long double native_right = native_from_f80(right);
    long double native_result;
    __asm__ volatile(
        "fldt %[left]\n\t"
        "fldt %[right]\n\t"
        "faddp\n\t"
        "fstpt %[result]"
        : [result] "=m" (native_result)
        : [left] "m" (native_left), [right] "m" (native_right)
        : "st");
    return f80_from_native(native_result);
}

static float80 native_x87_sub(float80 left, float80 right) {
    long double native_left = native_from_f80(left);
    long double native_right = native_from_f80(right);
    long double native_result;
    __asm__ volatile(
        "fldt %[right]\n\t"
        "fldt %[left]\n\t"
        "fsubp\n\t"
        "fstpt %[result]"
        : [result] "=m" (native_result)
        : [left] "m" (native_left), [right] "m" (native_right)
        : "st");
    return f80_from_native(native_result);
}

struct native_prem_result {
    float80 value;
    uint8_t quotient_low;
    bool completed;
};

static struct native_prem_result native_x87_prem(float80 x, float80 y) {
    long double native_x = native_from_f80(x);
    long double native_y = native_from_f80(y);
    long double native_result;
    uint16_t status;
    int remaining = 2048;
    __asm__ volatile(
        "fldt %[y]\n\t"
        "fldt %[x]\n\t"
        "1:\n\t"
        "fprem\n\t"
        "fnstsw %[status]\n\t"
        "testw $0x0400, %[status]\n\t"
        "jz 2f\n\t"
        "decl %[remaining]\n\t"
        "jnz 1b\n\t"
        "2:\n\t"
        "fstpt %[result]\n\t"
        "fstp %%st(0)"
        : [result] "=m" (native_result), [status] "=&a" (status),
          [remaining] "+r" (remaining)
        : [x] "m" (native_x), [y] "m" (native_y)
        : "cc", "st");

    unsigned quotient_low = ((unsigned) status >> 8 & 1) << 2 |
        ((unsigned) status >> 14 & 1) << 1 |
        ((unsigned) status >> 9 & 1);
    return (struct native_prem_result) {
        .value = f80_from_native(native_result),
        .quotient_low = (uint8_t) quotient_low,
        .completed = (status & UINT16_C(0x0400)) == 0,
    };
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
    static const float80 add_values[] = {
        {.signif = 0, .signExp = 0x0000},
        {.signif = 0, .signExp = 0x8000},
        {.signif = 1, .signExp = 0x0000},
        {.signif = 1, .signExp = 0x8000},
        {.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x0000},
        {.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x8000},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x0001},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x8001},
        {.signif = UINT64_C(0x8000000000000001), .signExp = 0x0001},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x0002},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x8002},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0x3fff},
        {.signif = UINT64_C(0x8000000000000000), .signExp = 0xbfff},
        {.signif = UINT64_MAX, .signExp = 0x7ffe},
        {.signif = UINT64_MAX, .signExp = 0xfffe},
    };
    static const struct {
        float80 x;
        float80 y;
    } prem_cases[] = {
        {{.signif = 0, .signExp = 0x0000},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = 0, .signExp = 0x8000},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0xc000}},
        {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x4000},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = UINT64_C(0xe000000000000000), .signExp = 0x4001},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = UINT64_C(0xe000000000000000), .signExp = 0x4001},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0xc000}},
        {{.signif = UINT64_C(0xe000000000000000), .signExp = 0xc001},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = UINT64_C(0xe000000000000000), .signExp = 0xc001},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0xc000}},
        {{.signif = UINT64_C(0xa000000000000000), .signExp = 0x4002},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = UINT64_C(0x8000000000000003), .signExp = 0x403f},
         {.signif = UINT64_C(0xc000000000000000), .signExp = 0x4000}},
        {{.signif = UINT64_C(0x8000000000000000), .signExp = 0x0001},
         {.signif = UINT64_C(0x7fffffffffffffff), .signExp = 0x0000}},
        {{.signif = UINT64_MAX, .signExp = 0x7ffe},
         {.signif = 7, .signExp = 0x0000}},
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
        for (size_t left = 0; left < sizeof(add_values) / sizeof(add_values[0]); left++) {
            for (size_t right = 0; right < sizeof(add_values) / sizeof(add_values[0]); right++) {
                float80 expected_add = native_x87_add(add_values[left], add_values[right]);
                float80 expected_sub = native_x87_sub(add_values[left], add_values[right]);
                check(native_result_same(
                        f80_add(add_values[left], add_values[right]), expected_add),
                    "x87 固定加法不一致（%s，left=%zu，right=%zu）",
                    rounding_name(mode), left, right);
                check(native_result_same(
                        f80_sub(add_values[left], add_values[right]), expected_sub),
                    "x87 固定减法不一致（%s，left=%zu，right=%zu）",
                    rounding_name(mode), left, right);
            }
        }
        for (size_t i = 0; i < sizeof(prem_cases) / sizeof(prem_cases[0]); i++) {
            struct native_prem_result expected = native_x87_prem(
                prem_cases[i].x, prem_cases[i].y);
            struct f80_mod_result actual = f80_mod(prem_cases[i].x, prem_cases[i].y);
            check(expected.completed,
                "x87 固定 FPREM 超出循环上限（%s，序号=%zu）",
                rounding_name(mode), i);
            if (!expected.completed)
                continue;
            check(actual.quotient_valid,
                "固定 FPREM 商无效（%s，序号=%zu）", rounding_name(mode), i);
            check(native_result_same(actual.value, expected.value),
                "x87 固定 FPREM 余数不一致（%s，序号=%zu）",
                rounding_name(mode), i);
            check(actual.quotient_low == expected.quotient_low,
                "x87 固定 FPREM 商位不一致（%s，序号=%zu，实际=%u，期望=%u）",
                rounding_name(mode), i, (unsigned) actual.quotient_low,
                (unsigned) expected.quotient_low);
        }
        random_state = UINT64_C(0x452821e638d01377) ^ (uint64_t) mode;
        for (int i = 0; i < 50000; i++) {
            uint64_t left_signif = next_random();
            uint64_t right_signif = next_random();
            uint16_t left_sign = (uint16_t) ((next_random() & 1) << 15);
            uint16_t right_sign = (uint16_t) ((next_random() & 1) << 15);
            float80 left;
            float80 right;
            switch (i & 3) {
                case 0:
                    left_signif &= UINT64_MAX >> 1;
                    right_signif &= UINT64_MAX >> 1;
                    left = f80_bits(left_signif == 0 ? 1 : left_signif, left_sign);
                    right = f80_bits(right_signif == 0 ? 1 : right_signif, right_sign);
                    break;
                case 1:
                    right_signif &= UINT64_MAX >> 1;
                    left = f80_bits(left_signif | (UINT64_C(1) << 63),
                        (uint16_t) (1 + next_random() % 2) | left_sign);
                    right = f80_bits(right_signif == 0 ? 1 : right_signif, right_sign);
                    break;
                case 2:
                    left_signif &= UINT64_MAX >> 1;
                    left = f80_bits(left_signif == 0 ? 1 : left_signif, left_sign);
                    right = f80_bits(right_signif | (UINT64_C(1) << 63),
                        (uint16_t) (1 + next_random() % 2) | right_sign);
                    break;
                default:
                    right_sign = left_sign ^ 0x8000;
                    left = f80_bits(left_signif | (UINT64_C(1) << 63),
                        (uint16_t) (0x0001 | left_sign));
                    right = f80_bits(right_signif | (UINT64_C(1) << 63),
                        (uint16_t) (0x0001 | right_sign));
                    break;
            }
            float80 expected_add = native_x87_add(left, right);
            float80 expected_sub = native_x87_sub(left, right);
            check(native_result_same(f80_add(left, right), expected_add),
                "x87 随机加法不一致（%s，序号=%d）", rounding_name(mode), i);
            check(native_result_same(f80_sub(left, right), expected_sub),
                "x87 随机减法不一致（%s，序号=%d）", rounding_name(mode), i);
        }
        random_state = UINT64_C(0xbe5466cf34e90c6c) ^ (uint64_t) mode;
        for (int i = 0; i < 5000; i++) {
            uint64_t x_signif = next_random();
            uint64_t y_signif = next_random();
            uint16_t x_sign = (uint16_t) ((next_random() & 1) << 15);
            uint16_t y_sign = (uint16_t) ((next_random() & 1) << 15);
            float80 x;
            float80 y;
            if (i % 32 == 0) {
                uint16_t x_exp = (uint16_t) (1 + next_random() % 0x7ffe);
                uint16_t y_exp = (uint16_t) (1 + next_random() % x_exp);
                x = f80_bits(x_signif | (UINT64_C(1) << 63),
                    (uint16_t) (x_exp | x_sign));
                y = f80_bits(y_signif | (UINT64_C(1) << 63),
                    (uint16_t) (y_exp | y_sign));
            } else {
                switch (i & 3) {
                    case 0:
                        x_signif &= UINT64_MAX >> 1;
                        y_signif &= UINT64_MAX >> 1;
                        x = f80_bits(x_signif == 0 ? 1 : x_signif, x_sign);
                        y = f80_bits(y_signif == 0 ? 1 : y_signif, y_sign);
                        break;
                    case 1:
                        x = f80_bits(x_signif | (UINT64_C(1) << 63),
                            (uint16_t) (1 + next_random() % 4) | x_sign);
                        y_signif &= UINT64_MAX >> 1;
                        y = f80_bits(y_signif == 0 ? 1 : y_signif, y_sign);
                        break;
                    case 2: {
                        uint16_t exp = (uint16_t) (1 + next_random() % 0x7ffe);
                        x = f80_bits(x_signif | (UINT64_C(1) << 63),
                            (uint16_t) (exp | x_sign));
                        y = f80_bits(y_signif | (UINT64_C(1) << 63),
                            (uint16_t) (exp | y_sign));
                        break;
                    }
                    default: {
                        uint16_t x_exp = (uint16_t) (1 + next_random() % 0x7ffd);
                        x = f80_bits(x_signif | (UINT64_C(1) << 63),
                            (uint16_t) (x_exp | x_sign));
                        y = f80_bits(y_signif | (UINT64_C(1) << 63),
                            (uint16_t) (x_exp + 1) | y_sign);
                        break;
                    }
                }
            }

            struct native_prem_result expected = native_x87_prem(x, y);
            struct f80_mod_result actual = f80_mod(x, y);
            check(expected.completed,
                "x87 随机 FPREM 超出循环上限（%s，序号=%d）",
                rounding_name(mode), i);
            if (!expected.completed)
                continue;
            check(actual.quotient_valid,
                "随机 FPREM 商无效（%s，序号=%d）", rounding_name(mode), i);
            check(native_result_same(actual.value, expected.value),
                "x87 随机 FPREM 余数不一致（%s，序号=%d）",
                rounding_name(mode), i);
            check(actual.quotient_low == expected.quotient_low,
                "x87 随机 FPREM 商位不一致（%s，序号=%d，实际=%u，期望=%u）",
                rounding_name(mode), i, (unsigned) actual.quotient_low,
                (unsigned) expected.quotient_low);
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
            check(f80_gt(a, b) == (native_a > native_b),
                "x87 大于比较不一致（%s）", rounding_name(mode));
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
    test_add_subnormal_golden();
    test_mod_golden();
    test_log2_golden();
    test_log2_properties();
    test_scale_golden();
    test_sqrt_golden();
    test_portable_properties();
#if HAVE_NATIVE_X87
    test_native_x87_oracle();
#endif
    printf("总计：%d/%d 通过\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}

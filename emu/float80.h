#ifndef FLOAT80_H
#define FLOAT80_H

#include "misc.h"

typedef struct {
    uint64_t signif;
    union {
        uint16_t signExp;
        struct {
            unsigned exp:15;
            unsigned sign:1;
        };
    };
} float80;

float80 f80_from_int(int64_t i);
int64_t f80_to_int(float80 f);
float80 f80_from_double(double d);
double f80_to_double(float80 f);
float80 f80_round(float80 f);

bool f80_isnan(float80 f);
bool f80_isinf(float80 f);
bool f80_iszero(float80 f);
bool f80_isdenormal(float80 f);
bool f80_is_supported(float80 f);

float80 f80_add(float80 a, float80 b);
float80 f80_sub(float80 a, float80 b);
float80 f80_mul(float80 a, float80 b);
float80 f80_div(float80 a, float80 b);
struct f80_mod_result {
    float80 value;
    uint8_t quotient_low;
    bool quotient_valid;
};
struct f80_mod_result f80_mod(float80 a, float80 b);

bool f80_lt(float80 a, float80 b);
bool f80_gt(float80 a, float80 b);
bool f80_eq(float80 a, float80 b);
bool f80_uncomparable(float80 a, float80 b);

float80 f80_neg(float80 f);
float80 f80_abs(float80 f);

float80 f80_log2(float80 x);
float80 f80_sqrt(float80 x);

float80 f80_scale(float80 x, int scale);
float80 f80_scale_by_float(float80 x, float80 scale);

// 拆出的两个结果可直接作为浮点栈值交给 FYL2X 与 FSCALE 等后续指令。
void f80_xtract(float80 f, float80 *exponent, float80 *significand);

enum f80_rounding_mode {
    round_to_nearest = 0,
    round_down = 1,
    round_up = 2,
    round_chop = 3,
};
extern __thread enum f80_rounding_mode f80_rounding_mode;

#define F80_NAN ((float80) {.signif = 0xc000000000000000, .exp = 0x7fff, .sign = 0})
#define F80_INDEFINITE ((float80) {.signif = 0xc000000000000000, .exp = 0x7fff, .sign = 1})
#define F80_INF ((float80) {.signif = 0x8000000000000000, .exp = 0x7fff, .sign = 0})

#endif

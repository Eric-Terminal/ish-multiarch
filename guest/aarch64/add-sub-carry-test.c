#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

struct arithmetic_result {
    qword_t value;
    dword_t nzcv;
};

static dword_t encode(bool is_64, bool subtract, bool set_flags,
        byte_t rd, byte_t rn, byte_t rm) {
    return UINT32_C(0x1a000000) |
            (dword_t) is_64 << 31 |
            (dword_t) subtract << 30 |
            (dword_t) set_flags << 29 |
            (dword_t) rm << 16 |
            (dword_t) rn << 5 | rd;
}

static enum aarch64_opcode expected_opcode(
        bool subtract, bool set_flags) {
    static const enum aarch64_opcode opcodes[] = {
        AARCH64_OP_ADC,
        AARCH64_OP_ADCS,
        AARCH64_OP_SBC,
        AARCH64_OP_SBCS,
    };
    return opcodes[((unsigned) subtract << 1) | (unsigned) set_flags];
}

static bool is_carry_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_ADC || opcode == AARCH64_OP_ADCS ||
            opcode == AARCH64_OP_SBC || opcode == AARCH64_OP_SBCS;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t rd, byte_t rn, byte_t rm) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.data_processing_2source.rd == rd);
    assert(instruction.operands.data_processing_2source.rn == rn);
    assert(instruction.operands.data_processing_2source.rm == rm);
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static qword_t width_mask(byte_t width) {
    return width == 32 ? UINT32_MAX : UINT64_MAX;
}

static __int128 signed_value(qword_t value, byte_t width) {
    qword_t sign = UINT64_C(1) << (width - 1);
    __int128 result = (__int128) (value & width_mask(width));
    if (value & sign)
        result -= (__int128) 1 << width;
    return result;
}

// 预言机使用数学加减与有符号范围，避免复刻执行器的补码路径。
static struct arithmetic_result reference_result(byte_t width,
        bool subtract, qword_t left, qword_t right, bool carry_in) {
    qword_t mask = width_mask(width);
    qword_t sign = UINT64_C(1) << (width - 1);
    left &= mask;
    right &= mask;

    qword_t value;
    bool carry_out;
    __int128 mathematical;
    if (subtract) {
        unsigned borrow = carry_in ? 0U : 1U;
        __int128 difference = (__int128) left - right - borrow;
        if (difference < 0)
            difference += (__int128) 1 << width;
        value = (qword_t) difference & mask;
        carry_out = (__uint128_t) left >=
                (__uint128_t) right + borrow;
        mathematical = signed_value(left, width) -
                signed_value(right, width) - borrow;
    } else {
        __uint128_t sum = (__uint128_t) left + right +
                (carry_in ? 1U : 0U);
        value = (qword_t) sum & mask;
        carry_out = sum > mask;
        mathematical = signed_value(left, width) +
                signed_value(right, width) + (carry_in ? 1 : 0);
    }

    __int128 minimum = -((__int128) 1 << (width - 1));
    __int128 maximum = ((__int128) 1 << (width - 1)) - 1;
    bool overflow = mathematical < minimum || mathematical > maximum;
    dword_t nzcv = (value & sign ? UINT32_C(1) << 31 : 0) |
            (value == 0 ? UINT32_C(1) << 30 : 0) |
            (carry_out ? UINT32_C(1) << 29 : 0) |
            (overflow ? UINT32_C(1) << 28 : 0);
    return (struct arithmetic_result) {.value = value, .nzcv = nzcv};
}

static void test_decode_and_neighbors(void) {
    assert_decode(UINT32_C(0x1a020020), AARCH64_OP_ADC,
            32, 0, 1, 2);
    assert_decode(UINT32_C(0x3a050083), AARCH64_OP_ADCS,
            32, 3, 4, 5);
    assert_decode(UINT32_C(0x5a0800e6), AARCH64_OP_SBC,
            32, 6, 7, 8);
    assert_decode(UINT32_C(0x7a0b0149), AARCH64_OP_SBCS,
            32, 9, 10, 11);
    assert_decode(UINT32_C(0x9a0e01ac), AARCH64_OP_ADC,
            64, 12, 13, 14);
    assert_decode(UINT32_C(0xba11020f), AARCH64_OP_ADCS,
            64, 15, 16, 17);
    assert_decode(UINT32_C(0xda060061), AARCH64_OP_SBC,
            64, 1, 3, 6);
    assert_decode(UINT32_C(0xfa140272), AARCH64_OP_SBCS,
            64, 18, 19, 20);

    unsigned count = 0;
    for (unsigned is_64 = 0; is_64 < 2; is_64++) {
        for (unsigned subtract = 0; subtract < 2; subtract++) {
            for (unsigned set_flags = 0; set_flags < 2; set_flags++) {
                for (unsigned rm = 0; rm < 32; rm++) {
                    for (unsigned rn = 0; rn < 32; rn++) {
                        for (unsigned rd = 0; rd < 32; rd++) {
                            dword_t word = encode(is_64 != 0, subtract != 0,
                                    set_flags != 0, (byte_t) rd,
                                    (byte_t) rn, (byte_t) rm);
                            assert_decode(word, expected_opcode(
                                    subtract != 0, set_flags != 0),
                                    is_64 ? 64 : 32, (byte_t) rd,
                                    (byte_t) rn, (byte_t) rm);
                            count++;
                        }
                    }
                }
            }
        }
    }
    assert(count == 262144);

    const dword_t fixed_mask = UINT32_C(0x1fe0fc00);
    const dword_t base = UINT32_C(0x9a020020);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                base ^ (UINT32_C(1) << bit), &instruction);
        assert(!decoded || !is_carry_opcode(instruction.opcode));
    }
}

static void run_case(byte_t width, bool subtract, bool set_flags,
        bool carry_in, qword_t left, qword_t right) {
    dword_t initial_nzcv = UINT32_C(0x90000000) |
            (carry_in ? UINT32_C(1) << 29 : 0);
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x8877665544332211),
        .nzcv = initial_nzcv,
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000000),
    };
    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = width == 32 ?
            UINT64_C(0xfeedface00000000) | (left & UINT32_MAX) : left;
    cpu.x[2] = width == 32 ?
            UINT64_C(0xcafebabe00000000) | (right & UINT32_MAX) : right;
    cpu.x[3] = UINT64_C(0x0123456789abcdef);
    struct arithmetic_result expected = reference_result(
            width, subtract, left, right, carry_in);

    execute_instruction(&cpu, encode(width == 64, subtract, set_flags,
            0, 1, 2));
    assert(cpu.x[0] == expected.value);
    assert(cpu.nzcv == (set_flags ? expected.nzcv : initial_nzcv));
    assert(cpu.pc == UINT64_C(0x1004));
    assert(cpu.sp == UINT64_C(0x8877665544332211));
    assert(cpu.x[3] == UINT64_C(0x0123456789abcdef));
    assert(cpu.fpcr == UINT32_C(0x01000000));
    assert(cpu.fpsr == UINT32_C(0x08000000));
}

static void test_operation_matrix(void) {
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        byte_t width = width_index == 0 ? 32 : 64;
        qword_t mask = width_mask(width);
        qword_t sign = UINT64_C(1) << (width - 1);
        qword_t values[] = {
            0,
            1,
            2,
            sign - 1,
            sign,
            sign + 1,
            mask - 1,
            mask,
            UINT64_C(0x0123456789abcdef) & mask,
        };
        for (unsigned subtract = 0; subtract < 2; subtract++) {
            for (unsigned set_flags = 0; set_flags < 2; set_flags++) {
                for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                    for (unsigned left = 0;
                            left < sizeof(values) / sizeof(values[0]);
                            left++) {
                        for (unsigned right = 0;
                                right < sizeof(values) / sizeof(values[0]);
                                right++) {
                            run_case(width, subtract != 0, set_flags != 0,
                                    carry_in != 0, values[left], values[right]);
                        }
                    }
                }
            }
        }
    }
}

static void test_flag_boundaries(void) {
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        byte_t width = width_index == 0 ? 32 : 64;
        qword_t mask = width_mask(width);
        qword_t sign = UINT64_C(1) << (width - 1);
        struct {
            bool subtract;
            bool carry_in;
            qword_t left;
            qword_t right;
            qword_t value;
            dword_t nzcv;
        } cases[] = {
            {false, true, mask, 0, 0, UINT32_C(0x60000000)},
            {false, true, sign - 1, 0, sign, UINT32_C(0x90000000)},
            {false, false, sign, sign, 0, UINT32_C(0x70000000)},
            {false, true, mask, mask, mask, UINT32_C(0xa0000000)},
            {true, true, 5, 5, 0, UINT32_C(0x60000000)},
            {true, false, 0, 0, mask, UINT32_C(0x80000000)},
            {true, true, sign - 1, mask, sign, UINT32_C(0x90000000)},
            {true, true, sign, 1, sign - 1, UINT32_C(0x30000000)},
            {true, false, 0, mask, 0, UINT32_C(0x40000000)},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            struct cpu_state cpu = {
                .pc = UINT64_C(0x1800),
                .nzcv = cases[i].carry_in ?
                        UINT32_C(1) << 29 : 0,
            };
            cpu.x[4] = cases[i].left;
            cpu.x[5] = cases[i].right;
            execute_instruction(&cpu, encode(width == 64,
                    cases[i].subtract, true, 6, 4, 5));
            assert(cpu.x[6] == cases[i].value);
            assert(cpu.nzcv == cases[i].nzcv);
            assert(cpu.pc == UINT64_C(0x1804));
        }
    }
}

static void test_zero_register(void) {
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        byte_t width = width_index == 0 ? 32 : 64;
        for (unsigned subtract = 0; subtract < 2; subtract++) {
            for (unsigned set_flags = 0; set_flags < 2; set_flags++) {
                for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                    dword_t initial_nzcv = UINT32_C(0x90000000) |
                            (carry_in ? UINT32_C(1) << 29 : 0);
                    struct cpu_state cpu = {
                        .pc = UINT64_C(0x2000),
                        .sp = UINT64_C(0xfedcba9876543210),
                        .nzcv = initial_nzcv,
                    };
                    cpu.x[0] = UINT64_MAX;
                    cpu.x[1] = UINT64_C(0x123456789abcdef0);
                    cpu.x[2] = UINT64_C(0x1111111122222222);

                    struct arithmetic_result expected = reference_result(
                            width, subtract != 0, 0, cpu.x[2],
                            carry_in != 0);
                    execute_instruction(&cpu, encode(width == 64,
                            subtract != 0, set_flags != 0, 0, 31, 2));
                    assert(cpu.x[0] == expected.value);
                    assert(cpu.nzcv == (set_flags ?
                            expected.nzcv : initial_nzcv));
                    assert(cpu.sp == UINT64_C(0xfedcba9876543210));

                    cpu.pc = UINT64_C(0x2100);
                    cpu.nzcv = initial_nzcv;
                    expected = reference_result(width, subtract != 0,
                            cpu.x[1], 0, carry_in != 0);
                    execute_instruction(&cpu, encode(width == 64,
                            subtract != 0, set_flags != 0, 0, 1, 31));
                    assert(cpu.x[0] == expected.value);
                    assert(cpu.nzcv == (set_flags ?
                            expected.nzcv : initial_nzcv));
                    assert(cpu.sp == UINT64_C(0xfedcba9876543210));

                    cpu.pc = UINT64_C(0x2200);
                    cpu.nzcv = initial_nzcv;
                    cpu.x[0] = UINT64_C(0xaaaaaaaa55555555);
                    expected = reference_result(width, subtract != 0,
                            cpu.x[1], cpu.x[2], carry_in != 0);
                    execute_instruction(&cpu, encode(width == 64,
                            subtract != 0, set_flags != 0, 31, 1, 2));
                    assert(cpu.x[0] == UINT64_C(0xaaaaaaaa55555555));
                    assert(cpu.nzcv == (set_flags ?
                            expected.nzcv : initial_nzcv));
                    assert(cpu.sp == UINT64_C(0xfedcba9876543210));
                    assert(cpu.pc == UINT64_C(0x2204));
                }
            }
        }
    }
}

static void test_aliases_and_real_path(void) {
    for (unsigned width_index = 0; width_index < 2; width_index++) {
        byte_t width = width_index == 0 ? 32 : 64;
        for (unsigned subtract = 0; subtract < 2; subtract++) {
            for (unsigned set_flags = 0; set_flags < 2; set_flags++) {
                for (unsigned carry_in = 0; carry_in < 2; carry_in++) {
                    qword_t left = UINT64_C(0x89abcdef01234567);
                    qword_t right = UINT64_C(0x76543210fedcba98);
                    dword_t nzcv = carry_in ?
                            UINT32_C(0xb0000000) : UINT32_C(0x90000000);
                    struct arithmetic_result expected = reference_result(
                            width, subtract != 0, left, right,
                            carry_in != 0);
                    struct cpu_state cpu = {.pc = 0, .nzcv = nzcv};
                    cpu.x[1] = left;
                    cpu.x[2] = right;
                    execute_instruction(&cpu, encode(width == 64,
                            subtract != 0, set_flags != 0, 1, 1, 2));
                    assert(cpu.x[1] == expected.value);

                    cpu = (struct cpu_state) {.pc = 0, .nzcv = nzcv};
                    cpu.x[1] = left;
                    cpu.x[2] = right;
                    execute_instruction(&cpu, encode(width == 64,
                            subtract != 0, set_flags != 0, 2, 1, 2));
                    assert(cpu.x[2] == expected.value);

                    expected = reference_result(width, subtract != 0,
                            left, left, carry_in != 0);
                    cpu = (struct cpu_state) {.pc = 0, .nzcv = nzcv};
                    cpu.x[1] = left;
                    execute_instruction(&cpu, encode(width == 64,
                            subtract != 0, set_flags != 0, 1, 1, 1));
                    assert(cpu.x[1] == expected.value);
                    assert(cpu.pc == 4);
                }
            }
        }
    }

    struct cpu_state cpu = {
        .pc = UINT64_C(0x4000),
        .nzcv = UINT32_C(0xa0000000),
    };
    cpu.x[3] = UINT64_C(0x147ae147ae147);
    cpu.x[6] = UINT64_C(0x1000000000000);
    execute_instruction(&cpu, UINT32_C(0xda060061));
    assert(cpu.x[1] == UINT64_C(0x47ae147ae147));
    assert(cpu.nzcv == UINT32_C(0xa0000000));
    assert(cpu.pc == UINT64_C(0x4004));
}

int main(void) {
    test_decode_and_neighbors();
    test_operation_matrix();
    test_flag_boundaries();
    test_zero_register();
    test_aliases_and_real_path();
    return 0;
}

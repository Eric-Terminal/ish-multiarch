#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define ADVSIMD_PAIRWISE_LONG_FIXED_MASK UINT32_C(0x9f3ffc00)
#define ADVSIMD_PAIRWISE_LONG_FIXED_BITS UINT32_C(0x0e202800)
#define ADVSIMD_PAIRWISE_LONG_VARIABLE_MASK UINT32_C(0x60c003ff)

static dword_t encode(bool q, bool u, byte_t size, byte_t rn, byte_t rd) {
    return ADVSIMD_PAIRWISE_LONG_FIXED_BITS |
            (dword_t) q << 30 |
            (dword_t) u << 29 |
            (dword_t) size << 22 |
            (dword_t) rn << 5 |
            rd;
}

static enum aarch64_opcode opcode_for(bool u) {
    return u ? AARCH64_OP_ADVSIMD_UADDLP : AARCH64_OP_ADVSIMD_SADDLP;
}

static bool is_pairwise_long_opcode(enum aarch64_opcode opcode) {
    return opcode == AARCH64_OP_ADVSIMD_SADDLP ||
            opcode == AARCH64_OP_ADVSIMD_UADDLP;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(decoded);
    use(decoded);
    return instruction;
}

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t width, byte_t source_element_size, byte_t rd, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.advsimd_pairwise_long.rd == rd);
    assert(instruction.operands.advsimd_pairwise_long.rn == rn);
    assert(instruction.operands.advsimd_pairwise_long.source_element_size ==
            source_element_size);
}

static void execute_instruction(struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    struct aarch64_execute_result result =
            aarch64_execute(cpu, NULL, &instruction);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
}

static void test_llvm_vectors(void) {
    // 由 Apple LLVM 的 AArch64 汇编器生成，覆盖全部合法 arrangement。
    assert_decode(UINT32_C(0x0e202820), AARCH64_OP_ADVSIMD_SADDLP,
            64, 1, 0, 1);
    assert_decode(UINT32_C(0x4e202862), AARCH64_OP_ADVSIMD_SADDLP,
            128, 1, 2, 3);
    assert_decode(UINT32_C(0x0e6028a4), AARCH64_OP_ADVSIMD_SADDLP,
            64, 2, 4, 5);
    assert_decode(UINT32_C(0x4e6028e6), AARCH64_OP_ADVSIMD_SADDLP,
            128, 2, 6, 7);
    assert_decode(UINT32_C(0x0ea02928), AARCH64_OP_ADVSIMD_SADDLP,
            64, 4, 8, 9);
    assert_decode(UINT32_C(0x4ea0296a), AARCH64_OP_ADVSIMD_SADDLP,
            128, 4, 10, 11);
    assert_decode(UINT32_C(0x2e2029ac), AARCH64_OP_ADVSIMD_UADDLP,
            64, 1, 12, 13);
    assert_decode(UINT32_C(0x6e2029ee), AARCH64_OP_ADVSIMD_UADDLP,
            128, 1, 14, 15);
    assert_decode(UINT32_C(0x2e602a30), AARCH64_OP_ADVSIMD_UADDLP,
            64, 2, 16, 17);
    assert_decode(UINT32_C(0x6e602a72), AARCH64_OP_ADVSIMD_UADDLP,
            128, 2, 18, 19);
    assert_decode(UINT32_C(0x2ea02ab4), AARCH64_OP_ADVSIMD_UADDLP,
            64, 4, 20, 21);
    assert_decode(UINT32_C(0x6ea02bff), AARCH64_OP_ADVSIMD_UADDLP,
            128, 4, 31, 31);

    // Watch 产品门禁中真实触发的 V31 寄存器别名形式。
    assert_decode(UINT32_C(0x6e202bff), AARCH64_OP_ADVSIMD_UADDLP,
            128, 1, 31, 31);
}

static void test_encoding_space(void) {
    assert((ADVSIMD_PAIRWISE_LONG_FIXED_MASK &
            ADVSIMD_PAIRWISE_LONG_VARIABLE_MASK) == 0);
    assert((ADVSIMD_PAIRWISE_LONG_FIXED_MASK |
            ADVSIMD_PAIRWISE_LONG_VARIABLE_MASK) == UINT32_MAX);

    unsigned decoded_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 3; size++) {
                for (unsigned rn = 0; rn < 32; rn++) {
                    for (unsigned rd = 0; rd < 32; rd++) {
                        struct aarch64_decoded instruction = {0};
                        bool decoded = aarch64_decode(encode(q, u,
                                (byte_t) size, (byte_t) rn, (byte_t) rd),
                                &instruction);
                        assert(decoded);
                        decoded_count++;
                        assert(instruction.opcode == opcode_for(u));
                        assert(instruction.width == (q ? 128 : 64));
                        assert(instruction.operands.advsimd_pairwise_long.rd ==
                                rd);
                        assert(instruction.operands.advsimd_pairwise_long.rn ==
                                rn);
                        assert(instruction.operands.advsimd_pairwise_long.
                                source_element_size == (1U << size));
                    }
                }
            }
        }
    }
    assert(decoded_count == 12288);
}

static void test_reserved_size(void) {
    unsigned rejected_count = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rd = 0; rd < 32; rd++) {
                    struct aarch64_decoded instruction = {0};
                    bool decoded = aarch64_decode(encode(q, u, 3,
                            (byte_t) rn, (byte_t) rd), &instruction);
                    assert(!decoded);
                    rejected_count++;
                }
            }
        }
    }
    assert(rejected_count == 4096);
}

static void test_fixed_bits(void) {
    dword_t base = encode(true, true, 0, 31, 31);
    assert((base & ADVSIMD_PAIRWISE_LONG_FIXED_MASK) ==
            ADVSIMD_PAIRWISE_LONG_FIXED_BITS);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((ADVSIMD_PAIRWISE_LONG_FIXED_MASK &
                (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction = {0};
        bool decoded = aarch64_decode(base ^ (UINT32_C(1) << bit),
                &instruction);
        assert(!decoded || !is_pairwise_long_opcode(instruction.opcode));
    }
}

static void assert_not_pairwise_long(dword_t word) {
    struct aarch64_decoded instruction = {0};
    bool decoded = aarch64_decode(word, &instruction);
    assert(!decoded || !is_pairwise_long_opcode(instruction.opcode));
}

static void test_rejected_neighbors(void) {
    static const dword_t neighbors[] = {
        // SADALP/UADALP 是累加族，不能被无累加的 ADDLP 误收。
        UINT32_C(0x0e206820),
        UINT32_C(0x6e206862),
        // 相邻的窄化、三同源、标量归约及不同主前缀编码。
        UINT32_C(0x0e2128a4),
        UINT32_C(0x0e2804e6),
        UINT32_C(0x5ef1b800),
        UINT32_C(0x0f202800),
        UINT32_C(0xc610a400),
    };
    for (unsigned i = 0; i < sizeof(neighbors) / sizeof(neighbors[0]); i++)
        assert_not_pairwise_long(neighbors[i]);
}

static qword_t read_lane(const union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index) {
    if (element_size == 1)
        return reg->b[index];
    if (element_size == 2)
        return reg->h[index];
    if (element_size == 4)
        return reg->s[index];
    return reg->d[index];
}

static void write_lane(union aarch64_vector_reg *reg,
        byte_t element_size, byte_t index, qword_t value) {
    if (element_size == 1)
        reg->b[index] = (byte_t) value;
    else if (element_size == 2)
        reg->h[index] = (word_t) value;
    else if (element_size == 4)
        reg->s[index] = (dword_t) value;
    else
        reg->d[index] = value;
}

static union aarch64_vector_reg reference_result(
        const union aarch64_vector_reg *source, bool q, bool u, byte_t size) {
    byte_t source_element_size = (byte_t) (1U << size);
    byte_t source_lanes =
            (byte_t) ((q ? 128 : 64) / (source_element_size * 8));
    qword_t sign = UINT64_C(1) <<
            (source_element_size * 8 - 1);
    qword_t mask = source_element_size == 4 ? UINT32_MAX :
            (UINT64_C(1) << (source_element_size * 8)) - 1;
    union aarch64_vector_reg result = {0};
    for (byte_t lane = 0; lane < source_lanes / 2; lane++) {
        qword_t left = read_lane(source, source_element_size,
                (byte_t) (2 * lane));
        qword_t right = read_lane(source, source_element_size,
                (byte_t) (2 * lane + 1));
        if (!u && (left & sign))
            left |= ~mask;
        if (!u && (right & sign))
            right |= ~mask;
        write_lane(&result, source_element_size * 2,
                lane, left + right);
    }
    return result;
}

static void load_edge_source(union aarch64_vector_reg *reg, byte_t size) {
    static const byte_t bytes[] = {
        0x7f, 0x01, 0x80, 0xff, 0x7f, 0x7f, 0x80, 0x80,
        0xff, 0x01, 0x00, 0x00, 0x55, 0xaa, 0xfe, 0x02,
    };
    static const word_t words[] = {
        0x7fff, 0x0001, 0x8000, 0xffff,
        0x7fff, 0x7fff, 0x8000, 0x8000,
    };
    static const dword_t dwords[] = {
        UINT32_C(0x7fffffff), 1,
        UINT32_C(0x80000000), UINT32_MAX,
    };
    if (size == 0)
        memcpy(reg->b, bytes, sizeof(bytes));
    else if (size == 1)
        memcpy(reg->h, words, sizeof(words));
    else
        memcpy(reg->s, dwords, sizeof(dwords));
}

static union aarch64_vector_reg edge_golden(bool q, bool u, byte_t size) {
    static const word_t byte_results[2][8] = {
        {
            0x0080, 0xff7f, 0x00fe, 0xff00,
            0x0000, 0x0000, 0xffff, 0x0000,
        },
        {
            0x0080, 0x017f, 0x00fe, 0x0100,
            0x0100, 0x0000, 0x00ff, 0x0100,
        },
    };
    static const dword_t word_results[2][4] = {
        {
            UINT32_C(0x00008000), UINT32_C(0xffff7fff),
            UINT32_C(0x0000fffe), UINT32_C(0xffff0000),
        },
        {
            UINT32_C(0x00008000), UINT32_C(0x00017fff),
            UINT32_C(0x0000fffe), UINT32_C(0x00010000),
        },
    };
    static const qword_t dword_results[2][2] = {
        {
            UINT64_C(0x0000000080000000),
            UINT64_C(0xffffffff7fffffff),
        },
        {
            UINT64_C(0x0000000080000000),
            UINT64_C(0x000000017fffffff),
        },
    };
    union aarch64_vector_reg result = {0};
    size_t bytes = q ? sizeof(result) : sizeof(result) / 2;
    if (size == 0)
        memcpy(result.h, byte_results[u], bytes);
    else if (size == 1)
        memcpy(result.s, word_results[u], bytes);
    else
        memcpy(result.d, dword_results[u], bytes);
    return result;
}

static void initialize_cpu(struct cpu_state *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->cycle = UINT64_C(0x1020304050607080);
    for (unsigned reg = 0; reg < 31; reg++)
        cpu->x[reg] = UINT64_C(0x1111111111111111) * (reg + 1);
    cpu->sp = UINT64_C(0x8877665544332210);
    cpu->pc = UINT64_C(0x1000);
    cpu->nzcv = UINT32_C(0xa0000000);
    for (unsigned reg = 0; reg < 32; reg++) {
        for (unsigned byte = 0; byte < 16; byte++)
            cpu->v[reg].b[byte] = (byte_t) (reg * 19 + byte * 7);
    }
    cpu->fpcr = UINT32_C(0x01c00000);
    cpu->fpsr = UINT32_C(0x08000011);
    cpu->tpidr_el0 = UINT64_C(0x123456789abcdef0);
    cpu->exclusive.address = UINT64_C(0x2000);
    cpu->exclusive.value_low = UINT64_C(0x0102030405060708);
    cpu->exclusive.value_high = UINT64_C(0x1112131415161718);
    cpu->exclusive.mapping_epoch = 17;
    cpu->exclusive.write_epoch = 23;
    cpu->exclusive.sync_identity = 29;
    cpu->exclusive.size = 16;
    cpu->exclusive.pair = true;
    cpu->exclusive.valid = true;
    cpu->segfault_addr = UINT64_C(0x3000);
    cpu->segfault_was_write = true;
    cpu->trapno = UINT32_C(0x55aa);
    cpu->single_step = true;
    cpu->_poked = true;
}

static void test_execution_space(void) {
    static const struct {
        byte_t rd;
        byte_t rn;
    } registers[] = {
        {0, 1},
        {1, 1},
        {31, 30},
        {31, 31},
    };
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned u = 0; u < 2; u++) {
            for (unsigned size = 0; size < 3; size++) {
                for (unsigned form = 0; form <
                        sizeof(registers) / sizeof(registers[0]); form++) {
                    byte_t rd = registers[form].rd;
                    byte_t rn = registers[form].rn;
                    struct cpu_state cpu;
                    initialize_cpu(&cpu);
                    load_edge_source(&cpu.v[rn], (byte_t) size);
                    struct cpu_state before = cpu;
                    union aarch64_vector_reg expected_vector =
                            reference_result(&before.v[rn], q, u,
                                    (byte_t) size);
                    union aarch64_vector_reg golden =
                            edge_golden(q, u, (byte_t) size);
                    assert(memcmp(&expected_vector, &golden,
                            sizeof(golden)) == 0);

                    dword_t word = encode(q, u, (byte_t) size, rn, rd);
                    if (q && u && size == 0 && rd == 31 && rn == 31)
                        assert(word == UINT32_C(0x6e202bff));
                    execute_instruction(&cpu, word);

                    struct cpu_state expected = before;
                    expected.pc += 4;
                    expected.v[rd] = expected_vector;
                    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
                    if (!q) {
                        for (unsigned byte = 8; byte < 16; byte++)
                            assert(cpu.v[rd].b[byte] == 0);
                    }
                }
            }
        }
    }
}

int main(void) {
    test_llvm_vectors();
    test_encoding_space();
    test_reserved_size();
    test_fixed_bits();
    test_rejected_neighbors();
    test_execution_space();
    return 0;
}

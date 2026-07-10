#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define DATA_PAGE UINT64_C(0x0000456789abc000)
#define DATA_NEXT (DATA_PAGE + GUEST_MEMORY_PAGE_SIZE)

struct test_page {
    guest_addr_t address;
    byte_t *host_page;
    unsigned permissions;
};

struct test_memory {
    byte_t first[GUEST_MEMORY_PAGE_SIZE];
    byte_t next[GUEST_MEMORY_PAGE_SIZE];
    struct test_page pages[2];
    struct guest_address_space space;
    struct guest_tlb tlb;
};

static enum guest_memory_fault_kind resolve_test_page(void *opaque,
        guest_addr_t page_base, enum guest_memory_access access,
        struct guest_page_view *view) {
    struct test_memory *memory = opaque;
    use(access);
    for (size_t i = 0; i < array_size(memory->pages); i++) {
        if (memory->pages[i].address == page_base) {
            *view = (struct guest_page_view) {
                .host_page = memory->pages[i].host_page,
                .permissions = memory->pages[i].permissions,
            };
            return GUEST_MEMORY_FAULT_NONE;
        }
    }
    return GUEST_MEMORY_FAULT_UNMAPPED;
}

static const struct guest_address_space_ops test_ops = {
    .resolve_page = resolve_test_page,
};

static void init_test_memory(struct test_memory *memory) {
    *memory = (struct test_memory) {0};
    memory->pages[0] = (struct test_page) {
        .address = DATA_PAGE,
        .host_page = memory->first,
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
    };
    memory->pages[1] = (struct test_page) {
        .address = DATA_NEXT,
        .host_page = memory->next,
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
    };
    guest_address_space_init(&memory->space, &test_ops, memory, 48);
    guest_tlb_init(&memory->tlb, &memory->space);
}

static dword_t encode_unsigned(unsigned size_field, unsigned operation,
        unsigned immediate, byte_t rt, byte_t rn) {
    return UINT32_C(0x3d000000) |
            (dword_t) size_field << 30 |
            (dword_t) operation << 22 |
            (dword_t) immediate << 10 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_unscaled(unsigned size_field, unsigned operation,
        int immediate, unsigned mode, byte_t rt, byte_t rn) {
    return UINT32_C(0x3c000000) |
            (dword_t) size_field << 30 |
            (dword_t) operation << 22 |
            ((dword_t) immediate & UINT32_C(0x1ff)) << 12 |
            (dword_t) mode << 10 |
            (dword_t) rn << 5 |
            rt;
}

static struct aarch64_execute_result execute_word(struct test_memory *memory,
        struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return aarch64_execute(cpu, &memory->tlb, &instruction);
}

static void assert_retired(struct test_memory *memory,
        struct cpu_state *cpu, dword_t word) {
    qword_t old_pc = cpu->pc;
    struct aarch64_execute_result result =
            execute_word(memory, cpu, word);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(cpu->pc == old_pc + 4);
}

static void fill_vector(union aarch64_vector_reg *reg, byte_t seed) {
    for (byte_t byte = 0; byte < sizeof(reg->b); byte++)
        reg->b[byte] = (byte_t) (seed + byte);
}

static void assert_vector(const union aarch64_vector_reg *reg,
        const byte_t *low, size_t size) {
    assert(memcmp(reg->b, low, size) == 0);
    for (size_t byte = size; byte < sizeof(reg->b); byte++)
        assert(reg->b[byte] == 0);
}

struct decode_case {
    dword_t word;
    enum aarch64_opcode opcode;
    byte_t size;
    byte_t rt;
    byte_t rn;
    int64_t offset;
    enum aarch64_address_mode address_mode;
};

static void test_apple_vectors(void) {
    static const struct decode_case cases[] = {
        {UINT32_C(0x3d800fe0), AARCH64_OP_STORE_SIMD_IMM12,
                16, 0, 31, 48, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3dc09120), AARCH64_OP_LOAD_SIMD_IMM12,
                16, 0, 9, 576, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3d3ffc20), AARCH64_OP_STORE_SIMD_IMM12,
                1, 0, 1, 4095, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3d4007e2), AARCH64_OP_LOAD_SIMD_IMM12,
                1, 2, 31, 1, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x7d3ffc83), AARCH64_OP_STORE_SIMD_IMM12,
                2, 3, 4, 8190, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x7d4007e5), AARCH64_OP_LOAD_SIMD_IMM12,
                2, 5, 31, 2, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xbd3ffce6), AARCH64_OP_STORE_SIMD_IMM12,
                4, 6, 7, 16380, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xbd400528), AARCH64_OP_LOAD_SIMD_IMM12,
                4, 8, 9, 4, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xfd3ffd6a), AARCH64_OP_STORE_SIMD_IMM12,
                8, 10, 11, 32760, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xfd4007ec), AARCH64_OP_LOAD_SIMD_IMM12,
                8, 12, 31, 8, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3dbffdee), AARCH64_OP_STORE_SIMD_IMM12,
                16, 14, 15, 65520, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3c100020), AARCH64_OP_STORE_SIMD_IMM9,
                1, 0, 1, -256, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x7c4ff3e2), AARCH64_OP_LOAD_SIMD_IMM9,
                2, 2, 31, 255, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xbc1ff083), AARCH64_OP_STORE_SIMD_IMM9,
                4, 3, 4, -1, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xfc4010c5), AARCH64_OP_LOAD_SIMD_IMM9,
                8, 5, 6, 1, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3c9f03e7), AARCH64_OP_STORE_SIMD_IMM9,
                16, 7, 31, -16, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3cc0f128), AARCH64_OP_LOAD_SIMD_IMM9,
                16, 8, 9, 15, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x3c100d6a), AARCH64_OP_STORE_SIMD_IMM9,
                1, 10, 11, -256, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x7c4fffec), AARCH64_OP_LOAD_SIMD_IMM9,
                2, 12, 31, 255, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0xbc1ffdcd), AARCH64_OP_STORE_SIMD_IMM9,
                4, 13, 14, -1, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0xfc401e0f), AARCH64_OP_LOAD_SIMD_IMM9,
                8, 15, 16, 1, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x3c9f0ff1), AARCH64_OP_STORE_SIMD_IMM9,
                16, 17, 31, -16, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x3cc0fe72), AARCH64_OP_LOAD_SIMD_IMM9,
                16, 18, 19, 15, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x3c1006b4), AARCH64_OP_STORE_SIMD_IMM9,
                1, 20, 21, -256, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x7c4ff7f6), AARCH64_OP_LOAD_SIMD_IMM9,
                2, 22, 31, 255, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0xbc1ff717), AARCH64_OP_STORE_SIMD_IMM9,
                4, 23, 24, -1, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0xfc401759), AARCH64_OP_LOAD_SIMD_IMM9,
                8, 25, 26, 1, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x3c9f07fb), AARCH64_OP_STORE_SIMD_IMM9,
                16, 27, 31, -16, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x3cc0f7bc), AARCH64_OP_LOAD_SIMD_IMM9,
                16, 28, 29, 15, AARCH64_ADDRESS_POST_INDEX},
    };

    for (size_t i = 0; i < array_size(cases); i++) {
        struct aarch64_decoded instruction;
        assert(aarch64_decode(cases[i].word, &instruction));
        assert(instruction.opcode == cases[i].opcode);
        assert(instruction.width == cases[i].size * 8);
        assert(instruction.operands.load_store.size == cases[i].size);
        assert(instruction.operands.load_store.rt == cases[i].rt);
        assert(instruction.operands.load_store.rn == cases[i].rn);
        assert(instruction.operands.load_store.offset == cases[i].offset);
        assert(instruction.operands.load_store.address_mode ==
                cases[i].address_mode);
    }
}

static bool valid_form(unsigned size_field, unsigned operation) {
    return (operation & 2) == 0 || size_field == 0;
}

static size_t form_size(unsigned size_field, unsigned operation) {
    return (size_t) 1 << ((operation & 2) ? 4 : size_field);
}

static enum aarch64_address_mode expected_address_mode(unsigned mode) {
    if (mode == 1)
        return AARCH64_ADDRESS_POST_INDEX;
    if (mode == 3)
        return AARCH64_ADDRESS_PRE_INDEX;
    return AARCH64_ADDRESS_OFFSET;
}

static void test_encoding_space(void) {
    unsigned unsigned_count = 0;
    for (unsigned size_field = 0; size_field < 4; size_field++) {
        for (unsigned operation = 0; operation < 4; operation++) {
            for (unsigned immediate = 0; immediate < 4096; immediate++) {
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(encode_unsigned(size_field,
                        operation, immediate, 0, 1), &instruction);
                assert(decoded == valid_form(size_field, operation));
                if (!decoded)
                    continue;
                unsigned_count++;
                size_t size = form_size(size_field, operation);
                assert(instruction.opcode == ((operation & 1) ?
                        AARCH64_OP_LOAD_SIMD_IMM12 :
                        AARCH64_OP_STORE_SIMD_IMM12));
                assert(instruction.operands.load_store.size == size);
                assert(instruction.operands.load_store.offset ==
                        (int64_t) (immediate * size));
            }
        }
    }
    assert(unsigned_count == 40960);

    unsigned unscaled_count = 0;
    for (unsigned size_field = 0; size_field < 4; size_field++) {
        for (unsigned operation = 0; operation < 4; operation++) {
            for (unsigned mode = 0; mode < 4; mode++) {
                for (unsigned encoded = 0; encoded < 512; encoded++) {
                    int immediate = encoded < 256 ?
                            (int) encoded : (int) encoded - 512;
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(encode_unscaled(
                            size_field, operation, immediate, mode, 0, 1),
                            &instruction);
                    bool expected = mode != 2 &&
                            valid_form(size_field, operation);
                    assert(decoded == expected);
                    if (!decoded)
                        continue;
                    unscaled_count++;
                    assert(instruction.opcode == ((operation & 1) ?
                            AARCH64_OP_LOAD_SIMD_IMM9 :
                            AARCH64_OP_STORE_SIMD_IMM9));
                    assert(instruction.operands.load_store.offset ==
                            immediate);
                    assert(instruction.operands.load_store.address_mode ==
                            expected_address_mode(mode));
                }
            }
        }
    }
    assert(unscaled_count == 15360);

    for (unsigned rn = 0; rn < 32; rn++) {
        for (unsigned rt = 0; rt < 32; rt++) {
            struct aarch64_decoded instruction;
            assert(aarch64_decode(encode_unscaled(0, 3, 1, 3,
                    (byte_t) rt, (byte_t) rn), &instruction));
            assert(instruction.operands.load_store.rn == rn);
            assert(instruction.operands.load_store.rt == rt);
        }
    }
}

struct transfer_form {
    byte_t size_field;
    byte_t store_operation;
    byte_t load_operation;
    byte_t size;
};

static void test_widths_and_unsigned_offsets(struct test_memory *memory) {
    static const struct transfer_form forms[] = {
        {0, 0, 1, 1},
        {1, 0, 1, 2},
        {2, 0, 1, 4},
        {3, 0, 1, 8},
        {0, 2, 3, 16},
    };
    for (size_t i = 0; i < array_size(forms); i++) {
        size_t offset = 128 + i * 64;
        struct cpu_state cpu = {
            .pc = UINT64_C(0x1000),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
            .fpcr = UINT32_C(0x01000000),
            .fpsr = UINT32_C(0x08000000),
        };
        fill_vector(&cpu.v[0], (byte_t) (0x10 + i * 0x20));
        union aarch64_vector_reg source = cpu.v[0];
        memset(memory->first + offset - 1, 0xcc, forms[i].size + 2);
        cpu.x[1] = DATA_PAGE + offset;
        assert_retired(memory, &cpu, encode_unsigned(forms[i].size_field,
                forms[i].store_operation, 0, 0, 1));
        assert(memory->first[offset - 1] == 0xcc);
        assert(memcmp(memory->first + offset, source.b,
                forms[i].size) == 0);
        assert(memory->first[offset + forms[i].size] == 0xcc);
        assert(memcmp(cpu.v[0].b, source.b, sizeof(source.b)) == 0);

        memset(cpu.v[2].b, 0xff, sizeof(cpu.v[2].b));
        cpu.x[3] = DATA_PAGE + offset;
        assert_retired(memory, &cpu, encode_unsigned(forms[i].size_field,
                forms[i].load_operation, 0, 2, 3));
        assert_vector(&cpu.v[2], source.b, forms[i].size);
        assert(cpu.sp == UINT64_C(0x1122334455667788));
        assert(cpu.nzcv == UINT32_C(0xa0000000));
        assert(cpu.fpcr == UINT32_C(0x01000000));
        assert(cpu.fpsr == UINT32_C(0x08000000));

        size_t maximum_offset = (size_t) 4095 * forms[i].size;
        cpu.x[4] = DATA_PAGE + 1024 - maximum_offset;
        assert_retired(memory, &cpu, encode_unsigned(forms[i].size_field,
                forms[i].store_operation, 4095, 0, 4));
        assert(memcmp(memory->first + 1024, source.b,
                forms[i].size) == 0);
    }
}

static void test_unscaled_and_writeback(struct test_memory *memory) {
    static const int offsets[] = {-256, -1, 0, 1, 255};
    struct cpu_state cpu = {.pc = UINT64_C(0x1800)};
    fill_vector(&cpu.v[0], 0x40);
    for (size_t i = 0; i < array_size(offsets); i++) {
        size_t target = 1536 + i * 32;
        cpu.x[1] = DATA_PAGE + target - (qword_t) offsets[i];
        assert_retired(memory, &cpu,
                encode_unscaled(0, 2, offsets[i], 0, 0, 1));
        assert(memcmp(memory->first + target, cpu.v[0].b, 16) == 0);
    }

    cpu.x[2] = DATA_PAGE + 1792;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 2, 15, 3, 0, 2));
    assert(cpu.x[2] == DATA_PAGE + 1807);
    assert(memcmp(memory->first + 1807, cpu.v[0].b, 16) == 0);

    memset(cpu.v[3].b, 0xff, sizeof(cpu.v[3].b));
    cpu.x[4] = DATA_PAGE + 1856;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 3, -16, 1, 3, 4));
    assert(cpu.x[4] == DATA_PAGE + 1840);
    assert_vector(&cpu.v[3], memory->first + 1856, 16);

    fill_vector(&cpu.v[1], 0x90);
    cpu.x[1] = DATA_PAGE + 1919;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 2, 1, 3, 1, 1));
    assert(cpu.x[1] == DATA_PAGE + 1920);
    assert(memcmp(memory->first + 1920, cpu.v[1].b, 16) == 0);
}

static void test_sp_and_v31(struct test_memory *memory) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x2000),
        .sp = DATA_PAGE + 2048,
    };
    fill_vector(&cpu.v[31], 0xa0);
    union aarch64_vector_reg expected = cpu.v[31];
    assert_retired(memory, &cpu, encode_unsigned(0, 2, 3, 31, 31));
    assert(cpu.sp == DATA_PAGE + 2048);
    assert(memcmp(memory->first + 2096, expected.b, 16) == 0);

    memset(cpu.v[31].b, 0, sizeof(cpu.v[31].b));
    assert_retired(memory, &cpu, encode_unsigned(0, 3, 3, 31, 31));
    assert(memcmp(cpu.v[31].b, expected.b, 16) == 0);

    cpu.sp = DATA_PAGE + 2112;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 2, -16, 3, 31, 31));
    assert(cpu.sp == DATA_PAGE + 2096);
}

static void test_cross_page(struct test_memory *memory) {
    struct cpu_state cpu = {.pc = UINT64_C(0x2800)};
    fill_vector(&cpu.v[0], 0x30);
    cpu.x[1] = DATA_NEXT - 8;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 2, 0, 0, 0, 1));
    assert(memcmp(memory->first + GUEST_MEMORY_PAGE_SIZE - 8,
            cpu.v[0].b, 8) == 0);
    assert(memcmp(memory->next, cpu.v[0].b + 8, 8) == 0);

    memset(cpu.v[2].b, 0, sizeof(cpu.v[2].b));
    cpu.x[3] = DATA_NEXT - 8;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 3, 0, 0, 2, 3));
    assert(memcmp(cpu.v[2].b, cpu.v[0].b, 16) == 0);

    cpu.x[1] = DATA_PAGE + 2179;
    assert_retired(memory, &cpu,
            encode_unscaled(0, 2, 0, 0, 0, 1));
    assert(memcmp(memory->first + 2179, cpu.v[0].b, 16) == 0);
}

static void test_fault_transactions(struct test_memory *memory) {
    byte_t first_before[8];
    byte_t next_before[8];
    memset(memory->first + GUEST_MEMORY_PAGE_SIZE - 8, 0x55, 8);
    memset(memory->next, 0xaa, 8);
    memcpy(first_before,
            memory->first + GUEST_MEMORY_PAGE_SIZE - 8, 8);
    memcpy(next_before, memory->next, 8);
    memory->pages[1].permissions = GUEST_MEMORY_READ;
    guest_address_space_changed(&memory->space);

    struct cpu_state cpu = {
        .cycle = 23,
        .pc = UINT64_C(0x3000),
        .nzcv = UINT32_C(0x60000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000000),
    };
    fill_vector(&cpu.v[0], 0x10);
    union aarch64_vector_reg source = cpu.v[0];
    cpu.x[1] = DATA_NEXT - 23;
    struct aarch64_execute_result result = execute_word(memory, &cpu,
            encode_unscaled(0, 2, 15, 3, 0, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(memory->first + GUEST_MEMORY_PAGE_SIZE - 8,
            first_before, 8) == 0);
    assert(memcmp(memory->next, next_before, 8) == 0);
    assert(memcmp(cpu.v[0].b, source.b, 16) == 0);
    assert(cpu.x[1] == DATA_NEXT - 23);
    assert(cpu.pc == UINT64_C(0x3000) && cpu.cycle == 23);
    assert(cpu.nzcv == UINT32_C(0x60000000));

    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    union aarch64_vector_reg old_value;
    fill_vector(&old_value, 0xc0);
    cpu.v[2] = old_value;
    cpu.x[3] = DATA_NEXT - 8;
    result = execute_word(memory, &cpu,
            encode_unscaled(0, 3, 15, 1, 2, 3));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(cpu.v[2].b, old_value.b, 16) == 0);
    assert(cpu.x[3] == DATA_NEXT - 8);
    assert(cpu.pc == UINT64_C(0x3000) && cpu.cycle == 23);
    assert(cpu.fpcr == UINT32_C(0x01000000));
    assert(cpu.fpsr == UINT32_C(0x08000000));

    cpu.x[3] = (UINT64_C(1) << 48) - 8;
    result = execute_word(memory, &cpu,
            encode_unscaled(0, 3, 0, 0, 2, 3));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.fault.address == (UINT64_C(1) << 48));
    assert(memcmp(cpu.v[2].b, old_value.b, 16) == 0);
}

int main(void) {
    test_apple_vectors();
    test_encoding_space();

    struct test_memory memory;
    init_test_memory(&memory);
    test_widths_and_unsigned_offsets(&memory);
    test_unscaled_and_writeback(&memory);
    test_sp_and_v31(&memory);
    test_cross_page(&memory);
    test_fault_transactions(&memory);
    return 0;
}

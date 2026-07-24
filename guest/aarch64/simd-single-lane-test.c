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

static dword_t encode(bool load, byte_t element_size,
        byte_t element_index, byte_t rt, byte_t rn) {
    assert(element_size == 1 || element_size == 2 ||
            element_size == 4 || element_size == 8);
    assert(element_index < 16 / element_size);
    byte_t q;
    byte_t opcode;
    byte_t s;
    byte_t size;
    if (element_size == 1) {
        q = element_index >> 3;
        opcode = 0;
        s = (element_index >> 2) & 1;
        size = element_index & 3;
    } else if (element_size == 2) {
        q = element_index >> 2;
        opcode = 2;
        s = (element_index >> 1) & 1;
        size = (element_index & 1) << 1;
    } else if (element_size == 4) {
        q = element_index >> 1;
        opcode = 4;
        s = element_index & 1;
        size = 0;
    } else {
        q = element_index;
        opcode = 4;
        s = 0;
        size = 1;
    }
    return (load ? UINT32_C(0x0d400000) : UINT32_C(0x0d000000)) |
            (dword_t) q << 30 |
            (dword_t) opcode << 13 |
            (dword_t) s << 12 |
            (dword_t) size << 10 |
            (dword_t) rn << 5 |
            rt;
}

static bool expected_form(byte_t q, byte_t opcode, byte_t s, byte_t size,
        byte_t *element_size, byte_t *element_index) {
    if (opcode == 0) {
        *element_size = 1;
        *element_index = (byte_t) ((q << 3) | (s << 2) | size);
        return true;
    }
    if (opcode == 2 && (size & 1) == 0) {
        *element_size = 2;
        *element_index =
                (byte_t) ((q << 2) | (s << 1) | (size >> 1));
        return true;
    }
    if (opcode == 4 && size == 0) {
        *element_size = 4;
        *element_index = (byte_t) ((q << 1) | s);
        return true;
    }
    if (opcode == 4 && s == 0 && size == 1) {
        *element_size = 8;
        *element_index = q;
        return true;
    }
    return false;
}

static bool decodes_as_single_lane(dword_t word,
        enum aarch64_opcode opcode,
        struct aarch64_decoded *instruction) {
    return aarch64_decode(word, instruction) &&
            instruction->opcode == opcode;
}

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t element_size, byte_t element_index, byte_t rt, byte_t rn) {
    struct aarch64_decoded instruction;
    assert(decodes_as_single_lane(word, opcode, &instruction));
    assert(instruction.width == 128);
    assert(instruction.operands.advsimd_single_lane.element_size ==
            element_size);
    assert(instruction.operands.advsimd_single_lane.element_index ==
            element_index);
    assert(instruction.operands.advsimd_single_lane.rt == rt);
    assert(instruction.operands.advsimd_single_lane.rn == rn);
}

static void assert_not_single_lane(
        dword_t word, enum aarch64_opcode opcode) {
    struct aarch64_decoded instruction;
    assert(!decodes_as_single_lane(word, opcode, &instruction));
}

static void assert_not_any_single_lane(dword_t word) {
    assert_not_single_lane(word, AARCH64_OP_LOAD_SIMD_SINGLE_LANE);
    assert_not_single_lane(word, AARCH64_OP_STORE_SIMD_SINGLE_LANE);
}

static void assert_undefined(dword_t word) {
    struct aarch64_decoded instruction;
    assert(!aarch64_decode(word, &instruction));
}

static void test_decode_space(void) {
    static const struct {
        dword_t word;
        byte_t element_size;
        byte_t element_index;
    } load_goldens[] = {
        {UINT32_C(0x0d40003f), 1, 0},
        {UINT32_C(0x0d40043f), 1, 1},
        {UINT32_C(0x0d40083f), 1, 2},
        {UINT32_C(0x0d400c3f), 1, 3},
        {UINT32_C(0x0d40103f), 1, 4},
        {UINT32_C(0x0d40143f), 1, 5},
        {UINT32_C(0x0d40183f), 1, 6},
        {UINT32_C(0x0d401c3f), 1, 7},
        {UINT32_C(0x4d40003f), 1, 8},
        {UINT32_C(0x4d40043f), 1, 9},
        {UINT32_C(0x4d40083f), 1, 10},
        {UINT32_C(0x4d400c3f), 1, 11},
        {UINT32_C(0x4d40103f), 1, 12},
        {UINT32_C(0x4d40143f), 1, 13},
        {UINT32_C(0x4d40183f), 1, 14},
        {UINT32_C(0x4d401c3f), 1, 15},
        {UINT32_C(0x0d40403f), 2, 0},
        {UINT32_C(0x0d40483f), 2, 1},
        {UINT32_C(0x0d40503f), 2, 2},
        {UINT32_C(0x0d40583f), 2, 3},
        {UINT32_C(0x4d40403f), 2, 4},
        {UINT32_C(0x4d40483f), 2, 5},
        {UINT32_C(0x4d40503f), 2, 6},
        {UINT32_C(0x4d40583f), 2, 7},
        {UINT32_C(0x0d40803f), 4, 0},
        {UINT32_C(0x0d40903f), 4, 1},
        {UINT32_C(0x4d40803f), 4, 2},
        {UINT32_C(0x4d40903f), 4, 3},
        {UINT32_C(0x0d40843f), 8, 0},
        {UINT32_C(0x4d40843f), 8, 1},
    };
    for (size_t i = 0; i < array_size(load_goldens); i++) {
        assert_decode(load_goldens[i].word,
                AARCH64_OP_LOAD_SIMD_SINGLE_LANE,
                load_goldens[i].element_size,
                load_goldens[i].element_index, 31, 1);
    }

    static const struct {
        dword_t word;
        byte_t element_size;
        byte_t element_index;
        byte_t rt;
        byte_t rn;
    } store_goldens[] = {
        {UINT32_C(0x0d000020), 1, 0, 0, 1},
        {UINT32_C(0x4d001c62), 1, 15, 2, 3},
        {UINT32_C(0x0d0040a4), 2, 0, 4, 5},
        {UINT32_C(0x4d0058e6), 2, 7, 6, 7},
        {UINT32_C(0x0d008128), 4, 0, 8, 9},
        {UINT32_C(0x4d00916a), 4, 3, 10, 11},
        {UINT32_C(0x0d0085ac), 8, 0, 12, 13},
        {UINT32_C(0x4d00841f), 8, 1, 31, 0},
    };
    for (size_t i = 0; i < array_size(store_goldens); i++) {
        assert_decode(store_goldens[i].word,
                AARCH64_OP_STORE_SIMD_SINGLE_LANE,
                store_goldens[i].element_size,
                store_goldens[i].element_index,
                store_goldens[i].rt, store_goldens[i].rn);
    }

    unsigned arrangements = 0;
    unsigned legal = 0;
    for (byte_t direction = 0; direction < 2; direction++) {
        bool load = direction != 0;
        enum aarch64_opcode expected_opcode = load ?
                AARCH64_OP_LOAD_SIMD_SINGLE_LANE :
                AARCH64_OP_STORE_SIMD_SINGLE_LANE;
        dword_t base = load ?
                UINT32_C(0x0d400000) : UINT32_C(0x0d000000);
        for (byte_t q = 0; q < 2; q++) {
            for (byte_t encoding_opcode = 0;
                    encoding_opcode < 8; encoding_opcode++) {
                for (byte_t s = 0; s < 2; s++) {
                    for (byte_t size = 0; size < 4; size++) {
                        byte_t expected_size = 0;
                        byte_t expected_index = 0;
                        bool expected = expected_form(q, encoding_opcode,
                                s, size, &expected_size, &expected_index);
                        if (expected)
                            arrangements++;
                        for (byte_t rn = 0; rn < 32; rn++) {
                            for (byte_t rt = 0; rt < 32; rt++) {
                                dword_t word = base |
                                        (dword_t) q << 30 |
                                        (dword_t) encoding_opcode << 13 |
                                        (dword_t) s << 12 |
                                        (dword_t) size << 10 |
                                        (dword_t) rn << 5 |
                                        rt;
                                struct aarch64_decoded instruction;
                                bool decoded = decodes_as_single_lane(
                                        word, expected_opcode,
                                        &instruction);
                                assert(decoded == expected);
                                if (!expected)
                                    continue;
                                assert(instruction.width == 128);
                                assert(instruction.operands.
                                        advsimd_single_lane.element_size ==
                                        expected_size);
                                assert(instruction.operands.
                                        advsimd_single_lane.element_index ==
                                        expected_index);
                                assert(instruction.operands.
                                        advsimd_single_lane.rn == rn);
                                assert(instruction.operands.
                                        advsimd_single_lane.rt == rt);
                                legal++;
                            }
                        }
                    }
                }
            }
        }
    }
    assert(arrangements == 60);
    assert(legal == 61440);

    const dword_t common_fixed_mask = UINT32_C(0xbfff2000);
    static const struct {
        dword_t word;
        enum aarch64_opcode opcode;
    } fixed_examples[] = {
        {UINT32_C(0x0d40103f), AARCH64_OP_LOAD_SIMD_SINGLE_LANE},
        {UINT32_C(0x0d00103f), AARCH64_OP_STORE_SIMD_SINGLE_LANE},
    };
    for (size_t example = 0;
            example < array_size(fixed_examples); example++) {
        for (byte_t bit = 0; bit < 32; bit++) {
            if ((common_fixed_mask & (UINT32_C(1) << bit)) != 0) {
                assert_not_single_lane(
                        fixed_examples[example].word ^
                                (UINT32_C(1) << bit),
                        fixed_examples[example].opcode);
            }
        }
    }

    assert_not_single_lane(
            UINT32_C(0x0d00103f), AARCH64_OP_LOAD_SIMD_SINGLE_LANE);
    assert_not_single_lane(
            UINT32_C(0x0d40103f), AARCH64_OP_STORE_SIMD_SINGLE_LANE);
    static const dword_t neighboring_forms[] = {
        UINT32_C(0x0d60103e), // LD2
        UINT32_C(0x0d40303d), // LD3
        UINT32_C(0x0d60303c), // LD4
        UINT32_C(0x4d20841f), // ST2
        UINT32_C(0x4d00a41f), // ST3
        UINT32_C(0x4d20a41f), // ST4
        UINT32_C(0x4d40cc1f), // LD1R
        UINT32_C(0x0ddf103f), // 立即数 post-index
        UINT32_C(0x0dc2103f), // 寄存器 post-index
        UINT32_C(0x4d9f841f), // ST1 立即数 post-index
        UINT32_C(0x4d82841f), // ST1 寄存器 post-index
        UINT32_C(0x0c407020), // 完整向量 LD1
        UINT32_C(0x4c007c1f), // 完整向量 ST1
        UINT32_C(0xfd00001f), // 普通 SIMD STR D
    };
    for (size_t i = 0; i < array_size(neighboring_forms); i++)
        assert_not_any_single_lane(neighboring_forms[i]);

    static const dword_t reserved_forms[] = {
        UINT32_C(0x0d41103f), // 无 post-index 时 Rm 非零
        UINT32_C(0x0d404420), // H 的保留 size
        UINT32_C(0x0d404c20),
        UINT32_C(0x0d408820), // S/D 的保留 size
        UINT32_C(0x0d408c20),
        UINT32_C(0x0d409420), // D 的保留 S
    };
    for (size_t i = 0; i < array_size(reserved_forms); i++) {
        assert_undefined(reserved_forms[i]);
        assert_undefined(reserved_forms[i] ^ UINT32_C(0x00400000));
    }
}

static struct aarch64_execute_result execute_word(struct test_memory *memory,
        struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return aarch64_execute(cpu, &memory->tlb, &instruction);
}

static void fill_vector(union aarch64_vector_reg *value, byte_t seed) {
    for (size_t i = 0; i < sizeof(value->b); i++)
        value->b[i] = (byte_t) (seed + i);
}

static void assert_store_retires_without_cpu_change(
        struct test_memory *memory, struct cpu_state *cpu, dword_t word) {
    struct cpu_state expected = *cpu;
    expected.pc += 4;
    struct aarch64_execute_result result =
            execute_word(memory, cpu, word);
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(cpu, &expected, sizeof(expected)) == 0);
}

static void test_load_all_arrangements(struct test_memory *memory) {
    static const byte_t element_sizes[] = {1, 2, 4, 8};
    unsigned arrangement = 0;
    for (size_t size = 0; size < array_size(element_sizes); size++) {
        byte_t element_size = element_sizes[size];
        for (byte_t index = 0; index < 16 / element_size; index++) {
            size_t offset = 257 + arrangement * 16;
            for (byte_t byte = 0; byte < element_size; byte++)
                memory->first[offset + byte] =
                        (byte_t) (0x80 + arrangement + byte);
            struct cpu_state cpu = {
                .cycle = 41,
                .pc = UINT64_C(0x1000),
                .sp = DATA_PAGE + 0x800,
                .nzcv = UINT32_C(0xa0000000),
                .fpcr = UINT32_C(0x01000000),
                .fpsr = UINT32_C(0x08000000),
            };
            cpu.x[1] = DATA_PAGE + offset;
            fill_vector(&cpu.v[2], (byte_t) (0x10 + arrangement));
            fill_vector(&cpu.v[3], 0xe0);
            union aarch64_vector_reg expected = cpu.v[2];
            union aarch64_vector_reg stable = cpu.v[3];
            memcpy(expected.b + index * element_size,
                    memory->first + offset, element_size);

            struct aarch64_execute_result result = execute_word(
                    memory, &cpu,
                    encode(true, element_size, index, 2, 1));
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
            assert(memcmp(cpu.v[2].b, expected.b, sizeof(expected.b)) == 0);
            assert(memcmp(cpu.v[3].b, stable.b, sizeof(stable.b)) == 0);
            assert(cpu.x[1] == DATA_PAGE + offset);
            assert(cpu.sp == DATA_PAGE + 0x800);
            assert(cpu.pc == UINT64_C(0x1004) && cpu.cycle == 41);
            assert(cpu.nzcv == UINT32_C(0xa0000000));
            assert(cpu.fpcr == UINT32_C(0x01000000));
            assert(cpu.fpsr == UINT32_C(0x08000000));
            arrangement++;
        }
    }
    assert(arrangement == 30);
}

static void test_store_all_registers(struct test_memory *memory) {
    static const byte_t element_sizes[] = {1, 2, 4, 8};
    unsigned arrangement = 0;
    unsigned executions = 0;
    for (size_t size = 0; size < array_size(element_sizes); size++) {
        byte_t element_size = element_sizes[size];
        for (byte_t index = 0; index < 16 / element_size; index++) {
            for (byte_t rn = 0; rn < 32; rn++) {
                for (byte_t rt = 0; rt < 32; rt++) {
                    size_t offset = 257 +
                            ((arrangement * 1024 + rn * 32 + rt) * 13) %
                                    3000;
                    memset(memory->first + offset - 1, 0xa5, 10);
                    struct cpu_state cpu = {
                        .cycle = 47,
                        .pc = UINT64_C(0x1400),
                        .sp = DATA_PAGE + 0xb00,
                        .nzcv = UINT32_C(0x90000000),
                        .fpcr = UINT32_C(0x01000000),
                        .fpsr = UINT32_C(0x08000010),
                        .tpidr_el0 = UINT64_C(0x1122334455667788),
                        .segfault_addr = UINT64_C(0xaabbccddeeff0011),
                        .segfault_was_write = true,
                        .trapno = UINT32_C(0x12345678),
                        .single_step = true,
                        ._poked = true,
                    };
                    for (byte_t reg = 0; reg < 31; reg++)
                        cpu.x[reg] = UINT64_C(0x1020304050607000) + reg;
                    for (byte_t reg = 0; reg < 32; reg++)
                        fill_vector(&cpu.v[reg],
                                (byte_t) (0x20 + arrangement + reg));
                    guest_addr_t address = DATA_PAGE + offset;
                    if (rn == 31)
                        cpu.sp = address;
                    else
                        cpu.x[rn] = address;
                    byte_t expected_lane[8];
                    memcpy(expected_lane,
                            cpu.v[rt].b + index * element_size,
                            element_size);

                    assert_store_retires_without_cpu_change(
                            memory, &cpu,
                            encode(false, element_size, index, rt, rn));
                    for (byte_t byte = 0; byte < 10; byte++) {
                        byte_t expected = 0xa5;
                        if (byte >= 1 && byte < 1 + element_size)
                            expected = expected_lane[byte - 1];
                        assert(memory->first[offset - 1 + byte] ==
                                expected);
                    }
                    executions++;
                }
            }
            arrangement++;
        }
    }
    assert(arrangement == 30);
    assert(executions == 30720);
}

static void test_store_little_endian(struct test_memory *memory) {
    static const byte_t h_expected[] = {0xb2, 0xa1};
    static const byte_t s_expected[] = {0xd4, 0xc3, 0xb2, 0xa1};
    static const byte_t d_expected[] =
            {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
    struct cpu_state cpu = {
        .cycle = 59,
        .pc = UINT64_C(0x1600),
        .x[0] = DATA_PAGE + 1536,
        .x[1] = DATA_PAGE + 1568,
        .x[2] = DATA_PAGE + 1600,
        .x[3] = DATA_PAGE + 1632,
        .nzcv = UINT32_C(0x60000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000010),
    };
    memset(memory->first + 1535, 0xa5, 10);
    cpu.v[31].b[15] = 0x5a;
    assert_store_retires_without_cpu_change(
            memory, &cpu, encode(false, 1, 15, 31, 0));
    assert(memory->first[1535] == 0xa5);
    assert(memory->first[1536] == 0x5a);
    assert(memory->first[1537] == 0xa5);

    memset(memory->first + 1567, 0xa5, 10);
    cpu.v[30].h[7] = UINT16_C(0xa1b2);
    assert_store_retires_without_cpu_change(
            memory, &cpu, encode(false, 2, 7, 30, 1));
    assert(memcmp(memory->first + 1568,
            h_expected, sizeof(h_expected)) == 0);
    assert(memory->first[1567] == 0xa5);
    assert(memory->first[1570] == 0xa5);

    memset(memory->first + 1599, 0xa5, 10);
    cpu.v[29].s[3] = UINT32_C(0xa1b2c3d4);
    assert_store_retires_without_cpu_change(
            memory, &cpu, encode(false, 4, 3, 29, 2));
    assert(memcmp(memory->first + 1600,
            s_expected, sizeof(s_expected)) == 0);
    assert(memory->first[1599] == 0xa5);
    assert(memory->first[1604] == 0xa5);

    memset(memory->first + 1631, 0xa5, 10);
    cpu.x[0] = DATA_PAGE + 1632;
    cpu.v[31].d[1] = UINT64_C(0x0123456789abcdef);
    assert_store_retires_without_cpu_change(
            memory, &cpu, UINT32_C(0x4d00841f));
    assert(memcmp(memory->first + 1632,
            d_expected, sizeof(d_expected)) == 0);
    assert(memory->first[1631] == 0xa5);
    assert(memory->first[1640] == 0xa5);
}

static void test_product_sp_and_v31(struct test_memory *memory) {
    memory->first[1027] = 0x6b;
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .x[1] = DATA_PAGE + 1027,
    };
    fill_vector(&cpu.v[31], 0x30);
    union aarch64_vector_reg expected = cpu.v[31];
    expected.b[4] = 0x6b;
    struct aarch64_execute_result result = execute_word(
            memory, &cpu, UINT32_C(0x0d40103f));
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(cpu.v[31].b, expected.b, sizeof(expected.b)) == 0);
    assert(cpu.x[1] == DATA_PAGE + 1027);

    for (byte_t byte = 0; byte < 8; byte++)
        memory->first[1280 + byte] = (byte_t) (0xa0 + byte);
    cpu.pc = UINT64_C(0x1c00);
    cpu.sp = DATA_PAGE + 1280;
    fill_vector(&cpu.v[31], 0x50);
    expected = cpu.v[31];
    memcpy(expected.b + 8, memory->first + 1280, 8);
    result = execute_word(memory, &cpu, encode(true, 8, 1, 31, 31));
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(cpu.v[31].b, expected.b, sizeof(expected.b)) == 0);
    assert(cpu.sp == DATA_PAGE + 1280);
    assert(cpu.pc == UINT64_C(0x1c04));
}

static void test_load_cross_page_and_faults(struct test_memory *memory) {
    static const byte_t crossing[8] =
            {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    memcpy(memory->first + GUEST_MEMORY_PAGE_SIZE - 4, crossing, 4);
    memcpy(memory->next, crossing + 4, 4);
    struct cpu_state cpu = {
        .cycle = 53,
        .pc = UINT64_C(0x2000),
        .x[3] = DATA_NEXT - 4,
        .sp = DATA_PAGE + 0x900,
        .nzcv = UINT32_C(0x60000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000000),
    };
    fill_vector(&cpu.v[4], 0x90);
    union aarch64_vector_reg expected = cpu.v[4];
    memcpy(expected.b, crossing, sizeof(crossing));
    struct aarch64_execute_result result =
            execute_word(memory, &cpu, encode(true, 8, 0, 4, 3));
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(cpu.v[4].b, expected.b, sizeof(expected.b)) == 0);
    assert(cpu.x[3] == DATA_NEXT - 4);

    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    cpu.pc = UINT64_C(0x2400);
    fill_vector(&cpu.v[5], 0xc0);
    union aarch64_vector_reg before = cpu.v[5];
    result = execute_word(memory, &cpu, encode(true, 8, 1, 5, 3));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(cpu.v[5].b, before.b, sizeof(before.b)) == 0);
    assert(cpu.x[3] == DATA_NEXT - 4);
    assert(cpu.sp == DATA_PAGE + 0x900);
    assert(cpu.pc == UINT64_C(0x2400) && cpu.cycle == 53);
    assert(cpu.nzcv == UINT32_C(0x60000000));
    assert(cpu.fpcr == UINT32_C(0x01000000));
    assert(cpu.fpsr == UINT32_C(0x08000000));

    memory->pages[1].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    cpu.x[3] = (UINT64_C(1) << 48) - 4;
    result = execute_word(memory, &cpu, encode(true, 8, 0, 5, 3));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.fault.address == (UINT64_C(1) << 48));
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(cpu.v[5].b, before.b, sizeof(before.b)) == 0);
    assert(cpu.pc == UINT64_C(0x2400));
}

static void test_store_cross_page_and_faults(struct test_memory *memory) {
    static const byte_t crossing[8] =
            {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    memory->pages[0].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    memory->pages[1].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    memset(memory->first + GUEST_MEMORY_PAGE_SIZE - 8, 0xa5, 8);
    memset(memory->next, 0xa5, 8);
    struct cpu_state cpu = {
        .cycle = 67,
        .pc = UINT64_C(0x2800),
        .x[3] = DATA_NEXT - 4,
        .sp = DATA_PAGE + 0xa00,
        .nzcv = UINT32_C(0x30000000),
        .fpcr = UINT32_C(0x01000000),
        .fpsr = UINT32_C(0x08000010),
        .tpidr_el0 = UINT64_C(0x0123456789abcdef),
        .segfault_addr = UINT64_C(0xfedcba9876543210),
        .segfault_was_write = true,
        .trapno = UINT32_C(0x89abcdef),
        .single_step = true,
        ._poked = true,
    };
    cpu.v[6].d[1] = UINT64_C(0x8877665544332211);
    assert_store_retires_without_cpu_change(
            memory, &cpu, encode(false, 8, 1, 6, 3));
    assert(memcmp(memory->first + GUEST_MEMORY_PAGE_SIZE - 4,
            crossing, 4) == 0);
    assert(memcmp(memory->next, crossing + 4, 4) == 0);
    assert(memory->first[GUEST_MEMORY_PAGE_SIZE - 5] == 0xa5);
    assert(memory->next[4] == 0xa5);

    cpu.v[6].d[1] = UINT64_C(0x0102030405060708);
    byte_t first_before[GUEST_MEMORY_PAGE_SIZE];
    byte_t next_before[GUEST_MEMORY_PAGE_SIZE];
    memcpy(first_before, memory->first, sizeof(first_before));
    memcpy(next_before, memory->next, sizeof(next_before));
    memory->pages[1].permissions = GUEST_MEMORY_READ;
    guest_address_space_changed(&memory->space);
    cpu.pc = UINT64_C(0x2c00);
    struct cpu_state cpu_before = cpu;
    struct aarch64_execute_result result = execute_word(
            memory, &cpu, encode(false, 8, 1, 6, 3));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);
    assert(memcmp(memory->first,
            first_before, sizeof(first_before)) == 0);
    assert(memcmp(memory->next,
            next_before, sizeof(next_before)) == 0);

    memory->pages[1].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    cpu.x[3] = (UINT64_C(1) << 48) - 4;
    cpu_before = cpu;
    result = execute_word(memory, &cpu, encode(false, 8, 1, 6, 3));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.fault.address == (UINT64_C(1) << 48));
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(&cpu, &cpu_before, sizeof(cpu)) == 0);
    assert(memcmp(memory->first,
            first_before, sizeof(first_before)) == 0);
    assert(memcmp(memory->next,
            next_before, sizeof(next_before)) == 0);
}

int main(void) {
    test_decode_space();

    struct test_memory memory;
    init_test_memory(&memory);
    test_load_all_arrangements(&memory);
    test_store_all_registers(&memory);
    test_store_little_endian(&memory);
    test_product_sp_and_v31(&memory);
    test_load_cross_page_and_faults(&memory);
    test_store_cross_page_and_faults(&memory);
    return 0;
}

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

static dword_t encode_register_offset(unsigned size_field,
        unsigned operation, unsigned option, bool scaled,
        byte_t rm, byte_t rt, byte_t rn) {
    return UINT32_C(0x3c200800) |
            (dword_t) size_field << 30 |
            (dword_t) operation << 22 |
            (dword_t) rm << 16 |
            (dword_t) option << 13 |
            (dword_t) scaled << 12 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_ld4(bool quadword, unsigned size_field,
        byte_t rt, byte_t rn) {
    return UINT32_C(0x0c400000) |
            (dword_t) quadword << 30 |
            (dword_t) size_field << 10 |
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

struct register_decode_case {
    dword_t word;
    enum aarch64_opcode opcode;
    byte_t size;
    byte_t rt;
    byte_t rn;
    byte_t rm;
    enum aarch64_extend_type extend_type;
    byte_t shift;
};

static void test_register_offset_apple_vectors(void) {
    // 机器码由 LLVM AArch64 汇编器生成。
    static const struct register_decode_case cases[] = {
        {UINT32_C(0x3c224820), AARCH64_OP_STORE_SIMD_REGISTER_OFFSET,
                1, 0, 1, 2, AARCH64_EXTEND_UXTW, 0},
        {UINT32_C(0x7c64dbe3), AARCH64_OP_LOAD_SIMD_REGISTER_OFFSET,
                2, 3, 31, 4, AARCH64_EXTEND_SXTW, 1},
        {UINT32_C(0xbc2768c5), AARCH64_OP_STORE_SIMD_REGISTER_OFFSET,
                4, 5, 6, 7, AARCH64_EXTEND_UXTX, 0},
        {UINT32_C(0xfc6a7928), AARCH64_OP_LOAD_SIMD_REGISTER_OFFSET,
                8, 8, 9, 10, AARCH64_EXTEND_UXTX, 3},
        // Alpine TLS 子进程实际执行的指令。
        {UINT32_C(0x3ca06abe), AARCH64_OP_STORE_SIMD_REGISTER_OFFSET,
                16, 30, 21, 0, AARCH64_EXTEND_UXTX, 0},
        {UINT32_C(0x3cedf98b), AARCH64_OP_LOAD_SIMD_REGISTER_OFFSET,
                16, 11, 12, 13, AARCH64_EXTEND_SXTX, 4},
    };
    for (size_t i = 0; i < array_size(cases); i++) {
        struct aarch64_decoded instruction;
        assert(aarch64_decode(cases[i].word, &instruction));
        assert(instruction.opcode == cases[i].opcode);
        assert(instruction.width == cases[i].size * 8);
        assert(instruction.operands.load_store.size == cases[i].size);
        assert(instruction.operands.load_store.rt == cases[i].rt);
        assert(instruction.operands.load_store.rn == cases[i].rn);
        assert(instruction.operands.load_store.rm == cases[i].rm);
        assert(instruction.operands.load_store.extend_type ==
                cases[i].extend_type);
        assert(instruction.operands.load_store.shift == cases[i].shift);
        assert(instruction.operands.load_store.address_mode ==
                AARCH64_ADDRESS_OFFSET);
    }
}

static void test_ld4_decode(void) {
    static const struct {
        dword_t word;
        byte_t width;
        byte_t element_size;
        byte_t rt;
        byte_t rn;
    } cases[] = {
        {UINT32_C(0x4c400020), 128, 1, 0, 1},
        {UINT32_C(0x4c4007e4), 128, 2, 4, 31},
        // SQLite WAL 实际执行的结构化读取。
        {UINT32_C(0x4c40081c), 128, 4, 28, 0},
        {UINT32_C(0x4c400c20), 128, 8, 0, 1},
        {UINT32_C(0x0c400048), 64, 1, 8, 2},
        {UINT32_C(0x0c40046c), 64, 2, 12, 3},
        {UINT32_C(0x0c400890), 64, 4, 16, 4},
    };
    for (size_t i = 0; i < array_size(cases); i++) {
        struct aarch64_decoded instruction;
        assert(aarch64_decode(cases[i].word, &instruction));
        assert(instruction.opcode == AARCH64_OP_LOAD_SIMD_MULTIPLE_4);
        assert(instruction.width == cases[i].width);
        assert(instruction.operands.advsimd_multiple.element_size ==
                cases[i].element_size);
        assert(instruction.operands.advsimd_multiple.rt == cases[i].rt);
        assert(instruction.operands.advsimd_multiple.rn == cases[i].rn);
    }

    struct aarch64_decoded instruction;
    assert(!aarch64_decode(encode_ld4(false, 3, 0, 1), &instruction));
    static const dword_t neighboring_forms[] = {
        UINT32_C(0x4c000020),
        UINT32_C(0x4cdf0020),
        UINT32_C(0x4cc20020),
        UINT32_C(0x4c404020),
        UINT32_C(0x4c408020),
        UINT32_C(0x4c407020),
    };
    for (size_t i = 0; i < array_size(neighboring_forms); i++) {
        bool decoded = aarch64_decode(neighboring_forms[i], &instruction);
        assert(!decoded ||
                instruction.opcode != AARCH64_OP_LOAD_SIMD_MULTIPLE_4);
    }
    static const dword_t reserved_forms[] = {
        UINT32_C(0x4c40181c),
        UINT32_C(0x4c40381c),
        UINT32_C(0x4c60081c),
        UINT32_C(0x4c41081c),
    };
    for (size_t i = 0; i < array_size(reserved_forms); i++)
        assert(!aarch64_decode(reserved_forms[i], &instruction));
}

static void prepare_ld4(byte_t *bytes,
        union aarch64_vector_reg expected[4],
        byte_t vector_size, byte_t element_size, byte_t seed) {
    memset(expected, 0, 4 * sizeof(*expected));
    for (byte_t index = 0; index < 4 * vector_size; index++)
        bytes[index] = (byte_t) (seed + index);
    for (byte_t offset = 0; offset < vector_size;
            offset += element_size) {
        for (byte_t structure = 0; structure < 4; structure++) {
            memcpy(expected[structure].b + offset,
                    bytes + 4 * offset + structure * element_size,
                    element_size);
        }
    }
}

static void test_ld4_execution(struct test_memory *memory) {
    union aarch64_vector_reg expected[4];
    prepare_ld4(memory->first + 256, expected, 16, 4, 0x10);
    struct cpu_state cpu = {
        .cycle = 37,
        .pc = UINT64_C(0x3600),
        .x[0] = DATA_PAGE + 256,
        .nzcv = UINT32_C(0xa0000000),
    };
    for (byte_t structure = 0; structure < 4; structure++)
        fill_vector(&cpu.v[28 + structure], (byte_t) (0xc0 + structure));
    assert_retired(memory, &cpu, UINT32_C(0x4c40081c));
    for (byte_t structure = 0; structure < 4; structure++)
        assert(memcmp(cpu.v[28 + structure].b,
                expected[structure].b, 16) == 0);
    assert(cpu.x[0] == DATA_PAGE + 256 && cpu.cycle == 37);
    assert(cpu.nzcv == UINT32_C(0xa0000000));

    prepare_ld4(memory->first + 512, expected, 8, 2, 0x60);
    cpu.sp = DATA_PAGE + 512;
    for (byte_t structure = 0; structure < 4; structure++)
        fill_vector(&cpu.v[(31 + structure) & 31], 0xe0);
    assert_retired(memory, &cpu, encode_ld4(false, 1, 31, 31));
    for (byte_t structure = 0; structure < 4; structure++) {
        assert(memcmp(cpu.v[(31 + structure) & 31].b,
                expected[structure].b, 16) == 0);
    }
    assert(cpu.sp == DATA_PAGE + 512);

    byte_t crossing[64];
    prepare_ld4(crossing, expected, 16, 4, 0x90);
    memcpy(memory->first + GUEST_MEMORY_PAGE_SIZE - 32, crossing, 32);
    memcpy(memory->next, crossing + 32, 32);
    cpu.x[1] = DATA_NEXT - 32;
    assert_retired(memory, &cpu, encode_ld4(true, 2, 4, 1));
    for (byte_t structure = 0; structure < 4; structure++)
        assert(memcmp(cpu.v[4 + structure].b,
                expected[structure].b, 16) == 0);

    union aarch64_vector_reg before[4];
    for (byte_t structure = 0; structure < 4; structure++) {
        fill_vector(&cpu.v[8 + structure], (byte_t) (0x20 + structure));
        before[structure] = cpu.v[8 + structure];
    }
    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    cpu.pc = UINT64_C(0x3800);
    cpu.x[2] = DATA_NEXT - 32;
    struct aarch64_execute_result result = execute_word(
            memory, &cpu, encode_ld4(true, 2, 8, 2));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    for (byte_t structure = 0; structure < 4; structure++)
        assert(memcmp(cpu.v[8 + structure].b,
                before[structure].b, 16) == 0);
    assert(cpu.x[2] == DATA_NEXT - 32 && cpu.pc == UINT64_C(0x3800));
    memory->pages[1].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
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

    unsigned register_count = 0;
    for (unsigned size_field = 0; size_field < 4; size_field++) {
        for (unsigned operation = 0; operation < 4; operation++) {
            for (unsigned option = 0; option < 8; option++) {
                for (unsigned scaled = 0; scaled < 2; scaled++) {
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(encode_register_offset(
                            size_field, operation, option, scaled != 0,
                            2, 3, 4), &instruction);
                    bool expected = valid_form(size_field, operation) &&
                            (option & 2) != 0;
                    assert(decoded == expected);
                    if (!decoded)
                        continue;
                    register_count++;
                    byte_t size = (byte_t)
                            form_size(size_field, operation);
                    assert(instruction.opcode == ((operation & 1) ?
                            AARCH64_OP_LOAD_SIMD_REGISTER_OFFSET :
                            AARCH64_OP_STORE_SIMD_REGISTER_OFFSET));
                    assert(instruction.operands.load_store.size == size);
                    assert(instruction.operands.load_store.rm == 2);
                    assert(instruction.operands.load_store.rt == 3);
                    assert(instruction.operands.load_store.rn == 4);
                    assert(instruction.operands.load_store.extend_type ==
                            (enum aarch64_extend_type) option);
                    byte_t scale = size == 16 ? 4 : (byte_t) size_field;
                    assert(instruction.operands.load_store.shift ==
                            (scaled ? scale : 0));
                }
            }
        }
    }
    assert(register_count == 80);

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

static void test_register_offsets(struct test_memory *memory) {
    static const struct transfer_form forms[] = {
        {0, 0, 1, 1},
        {1, 0, 1, 2},
        {2, 0, 1, 4},
        {3, 0, 1, 8},
        {0, 2, 3, 16},
    };
    for (size_t i = 0; i < array_size(forms); i++) {
        size_t target = 2304 + i * 128;
        struct cpu_state cpu = {
            .pc = UINT64_C(0x1400),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
            .fpcr = UINT32_C(0x01000000),
            .fpsr = UINT32_C(0x08000000),
        };
        fill_vector(&cpu.v[0], (byte_t) (0x20 + i * 0x20));
        union aarch64_vector_reg source = cpu.v[0];
        cpu.x[1] = DATA_PAGE + target - 3 * forms[i].size;
        cpu.x[2] = 3;
        assert_retired(memory, &cpu, encode_register_offset(
                forms[i].size_field, forms[i].store_operation,
                AARCH64_EXTEND_UXTX, true, 2, 0, 1));
        assert(memcmp(memory->first + target,
                source.b, forms[i].size) == 0);

        memset(cpu.v[3].b, 0xff, sizeof(cpu.v[3].b));
        assert_retired(memory, &cpu, encode_register_offset(
                forms[i].size_field, forms[i].load_operation,
                AARCH64_EXTEND_UXTX, true, 2, 3, 1));
        assert_vector(&cpu.v[3], source.b, forms[i].size);
        assert(cpu.sp == UINT64_C(0x1122334455667788));
        assert(cpu.nzcv == UINT32_C(0xa0000000));
        assert(cpu.fpcr == UINT32_C(0x01000000));
        assert(cpu.fpsr == UINT32_C(0x08000000));
    }

    struct cpu_state cpu = {.pc = UINT64_C(0x1600)};
    fill_vector(&cpu.v[30], 0xa0);
    union aarch64_vector_reg tls_source = cpu.v[30];
    cpu.x[21] = DATA_PAGE + 3200;
    cpu.x[0] = 32;
    assert_retired(memory, &cpu, UINT32_C(0x3ca06abe));
    assert(memcmp(memory->first + 3232,
            tls_source.b, sizeof(tls_source.b)) == 0);

    fill_vector(&cpu.v[31], 0xc0);
    union aarch64_vector_reg v31_source = cpu.v[31];
    cpu.sp = DATA_PAGE + 3264;
    cpu.x[4] = 16;
    assert_retired(memory, &cpu, encode_register_offset(
            0, 2, AARCH64_EXTEND_UXTX, false, 4, 31, 31));
    memset(cpu.v[31].b, 0, sizeof(cpu.v[31].b));
    assert_retired(memory, &cpu, encode_register_offset(
            0, 3, AARCH64_EXTEND_UXTX, false, 4, 31, 31));
    assert(memcmp(cpu.v[31].b,
            v31_source.b, sizeof(v31_source.b)) == 0);
    assert(cpu.sp == DATA_PAGE + 3264);
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

static void test_register_offset_fault_transactions(
        struct test_memory *memory) {
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
        .cycle = 29,
        .pc = UINT64_C(0x3400),
        .x[1] = DATA_NEXT - 16,
        .x[2] = 8,
    };
    fill_vector(&cpu.v[0], 0x20);
    union aarch64_vector_reg source = cpu.v[0];
    struct aarch64_execute_result result = execute_word(memory, &cpu,
            encode_register_offset(0, 2, AARCH64_EXTEND_UXTX,
                    false, 2, 0, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(memory->first + GUEST_MEMORY_PAGE_SIZE - 8,
            first_before, 8) == 0);
    assert(memcmp(memory->next, next_before, 8) == 0);
    assert(memcmp(cpu.v[0].b, source.b, sizeof(source.b)) == 0);
    assert(cpu.pc == UINT64_C(0x3400) && cpu.cycle == 29);

    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    union aarch64_vector_reg destination;
    fill_vector(&destination, 0xd0);
    cpu.v[3] = destination;
    result = execute_word(memory, &cpu,
            encode_register_offset(0, 3, AARCH64_EXTEND_UXTX,
                    false, 2, 3, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(cpu.v[3].b,
            destination.b, sizeof(destination.b)) == 0);
    assert(cpu.pc == UINT64_C(0x3400) && cpu.cycle == 29);

    memory->pages[1].permissions =
            GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
}

int main(void) {
    test_apple_vectors();
    test_register_offset_apple_vectors();
    test_ld4_decode();
    test_encoding_space();

    struct test_memory memory;
    init_test_memory(&memory);
    test_widths_and_unsigned_offsets(&memory);
    test_register_offsets(&memory);
    test_ld4_execution(&memory);
    test_unscaled_and_writeback(&memory);
    test_sp_and_v31(&memory);
    test_cross_page(&memory);
    test_register_offset_fault_transactions(&memory);
    test_fault_transactions(&memory);
    return 0;
}

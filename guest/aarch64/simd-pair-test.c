#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define DATA_PAGE UINT64_C(0x00003456789ab000)
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

static dword_t encode(unsigned operation, unsigned mode, bool load,
        byte_t imm7, byte_t rt, byte_t rt2, byte_t rn) {
    return UINT32_C(0x2c000000) |
            (dword_t) operation << 30 |
            (dword_t) mode << 23 |
            (dword_t) load << 22 |
            (dword_t) imm7 << 15 |
            (dword_t) rt2 << 10 |
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
    byte_t width;
    byte_t rt;
    byte_t rt2;
    byte_t rn;
    int64_t offset;
    enum aarch64_address_mode address_mode;
};

static void test_apple_vectors(void) {
    static const struct decode_case cases[] = {
        {UINT32_C(0x2c018440), AARCH64_OP_STORE_SIMD_PAIR,
                32, 0, 1, 2, 12, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x2c7e10a3), AARCH64_OP_LOAD_SIMD_PAIR,
                32, 3, 4, 5, -16, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x6c019d06), AARCH64_OP_STORE_SIMD_PAIR,
                64, 6, 7, 8, 24, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x6c7e2969), AARCH64_OP_LOAD_SIMD_PAIR,
                64, 9, 10, 11, -32, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xac01b5cc), AARCH64_OP_STORE_SIMD_PAIR,
                128, 12, 13, 14, 48, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xac7e43ef), AARCH64_OP_LOAD_SIMD_PAIR,
                128, 15, 16, 31, -64, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x2c818440), AARCH64_OP_STORE_SIMD_PAIR,
                32, 0, 1, 2, 12, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x2cfe10a3), AARCH64_OP_LOAD_SIMD_PAIR,
                32, 3, 4, 5, -16, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x6c819d06), AARCH64_OP_STORE_SIMD_PAIR,
                64, 6, 7, 8, 24, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x6cfe2969), AARCH64_OP_LOAD_SIMD_PAIR,
                64, 9, 10, 11, -32, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0xac81b5cc), AARCH64_OP_STORE_SIMD_PAIR,
                128, 12, 13, 14, 48, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0xacfe43ef), AARCH64_OP_LOAD_SIMD_PAIR,
                128, 15, 16, 31, -64, AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x2d018440), AARCH64_OP_STORE_SIMD_PAIR,
                32, 0, 1, 2, 12, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x2d7e10a3), AARCH64_OP_LOAD_SIMD_PAIR,
                32, 3, 4, 5, -16, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x6d019d06), AARCH64_OP_STORE_SIMD_PAIR,
                64, 6, 7, 8, 24, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x6d7e2969), AARCH64_OP_LOAD_SIMD_PAIR,
                64, 9, 10, 11, -32, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xad01b5cc), AARCH64_OP_STORE_SIMD_PAIR,
                128, 12, 13, 14, 48, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0xad7e43ef), AARCH64_OP_LOAD_SIMD_PAIR,
                128, 15, 16, 31, -64, AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x2d818440), AARCH64_OP_STORE_SIMD_PAIR,
                32, 0, 1, 2, 12, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x2dfe10a3), AARCH64_OP_LOAD_SIMD_PAIR,
                32, 3, 4, 5, -16, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x6d819d06), AARCH64_OP_STORE_SIMD_PAIR,
                64, 6, 7, 8, 24, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0x6dfe2969), AARCH64_OP_LOAD_SIMD_PAIR,
                64, 9, 10, 11, -32, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0xad81b5cc), AARCH64_OP_STORE_SIMD_PAIR,
                128, 12, 13, 14, 48, AARCH64_ADDRESS_PRE_INDEX},
        {UINT32_C(0xadfe43ef), AARCH64_OP_LOAD_SIMD_PAIR,
                128, 15, 16, 31, -64, AARCH64_ADDRESS_PRE_INDEX},
    };

    for (size_t i = 0; i < array_size(cases); i++) {
        struct aarch64_decoded instruction;
        assert(aarch64_decode(cases[i].word, &instruction));
        assert(instruction.opcode == cases[i].opcode);
        assert(instruction.width == cases[i].width);
        assert(instruction.operands.load_store_pair.rt == cases[i].rt);
        assert(instruction.operands.load_store_pair.rt2 == cases[i].rt2);
        assert(instruction.operands.load_store_pair.rn == cases[i].rn);
        assert(instruction.operands.load_store_pair.offset == cases[i].offset);
        assert(instruction.operands.load_store_pair.address_mode ==
                cases[i].address_mode);
    }
}

static enum aarch64_address_mode expected_address_mode(unsigned mode) {
    if (mode == 1)
        return AARCH64_ADDRESS_POST_INDEX;
    if (mode == 3)
        return AARCH64_ADDRESS_PRE_INDEX;
    return AARCH64_ADDRESS_OFFSET;
}

static void test_encoding_space(void) {
    unsigned valid = 0;
    for (unsigned operation = 0; operation < 4; operation++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            for (unsigned load = 0; load < 2; load++) {
                for (unsigned imm7 = 0; imm7 < 128; imm7++) {
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(encode(operation, mode,
                            load, (byte_t) imm7, 0, 1, 2), &instruction);
                    assert(decoded == (operation < 3));
                    if (!decoded)
                        continue;
                    valid++;
                    unsigned size = 1U << (operation + 2);
                    int immediate = imm7 < 64 ?
                            (int) imm7 : (int) imm7 - 128;
                    assert(instruction.opcode == (load ?
                            AARCH64_OP_LOAD_SIMD_PAIR :
                            AARCH64_OP_STORE_SIMD_PAIR));
                    assert(instruction.width == size * 8);
                    assert(instruction.operands.load_store_pair.offset ==
                            (int64_t) immediate * size);
                    assert(instruction.operands.load_store_pair.address_mode ==
                            expected_address_mode(mode));
                }
            }
        }
    }
    assert(valid == 3072);

    unsigned stores = 0;
    unsigned loads = 0;
    for (unsigned rt = 0; rt < 32; rt++) {
        for (unsigned rt2 = 0; rt2 < 32; rt2++) {
            struct aarch64_decoded instruction;
            assert(aarch64_decode(encode(2, 2, false, 0,
                    (byte_t) rt, (byte_t) rt2, 2), &instruction));
            stores++;
            bool decoded = aarch64_decode(encode(2, 2, true, 0,
                    (byte_t) rt, (byte_t) rt2, 2), &instruction);
            assert(decoded == (rt != rt2));
            if (decoded)
                loads++;
        }
    }
    assert(stores == 1024 && loads == 992);

    dword_t representative = UINT32_C(0xad010440);
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((UINT32_C(0x3e000000) & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction;
        bool decoded = aarch64_decode(
                representative ^ (UINT32_C(1) << bit), &instruction);
        if (decoded) {
            assert(instruction.opcode != AARCH64_OP_LOAD_SIMD_PAIR);
            assert(instruction.opcode != AARCH64_OP_STORE_SIMD_PAIR);
        }
    }
}

static void test_widths(struct test_memory *memory) {
    for (unsigned operation = 0; operation < 3; operation++) {
        size_t size = (size_t) 1 << (operation + 2);
        size_t offset = 128 + operation * 64;
        memset(memory->first + offset, 0xcc, 32);
        struct cpu_state cpu = {
            .pc = UINT64_C(0x1000),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
        };
        fill_vector(&cpu.v[0], 0x10);
        fill_vector(&cpu.v[1], 0x80);
        cpu.x[2] = DATA_PAGE + offset;
        assert_retired(memory, &cpu,
                encode(operation, 2, false, 0, 0, 1, 2));
        assert(memcmp(memory->first + offset, cpu.v[0].b, size) == 0);
        assert(memcmp(memory->first + offset + size,
                cpu.v[1].b, size) == 0);

        memset(cpu.v[2].b, 0xff, sizeof(cpu.v[2].b));
        memset(cpu.v[3].b, 0xff, sizeof(cpu.v[3].b));
        cpu.x[4] = DATA_PAGE + offset;
        assert_retired(memory, &cpu,
                encode(operation, 2, true, 0, 2, 3, 4));
        assert_vector(&cpu.v[2], memory->first + offset, size);
        assert_vector(&cpu.v[3], memory->first + offset + size, size);
        assert(cpu.sp == UINT64_C(0x1122334455667788));
        assert(cpu.nzcv == UINT32_C(0xa0000000));
    }
}

static void test_address_modes(struct test_memory *memory) {
    struct cpu_state cpu = {.pc = UINT64_C(0x1800)};
    fill_vector(&cpu.v[0], 0x20);
    fill_vector(&cpu.v[1], 0x60);

    cpu.x[2] = DATA_PAGE + 384;
    assert_retired(memory, &cpu, encode(0, 0, false, 3, 0, 1, 2));
    assert(cpu.x[2] == DATA_PAGE + 384);
    assert(memcmp(memory->first + 396, cpu.v[0].b, 4) == 0);

    cpu.x[2] = DATA_PAGE + 448;
    assert_retired(memory, &cpu, encode(2, 1, false, 2, 0, 1, 2));
    assert(cpu.x[2] == DATA_PAGE + 480);
    assert(memcmp(memory->first + 448, cpu.v[0].b, 16) == 0);

    cpu.x[2] = DATA_PAGE + 512;
    assert_retired(memory, &cpu, encode(2, 3, false, 2, 0, 1, 2));
    assert(cpu.x[2] == DATA_PAGE + 544);
    assert(memcmp(memory->first + 544, cpu.v[0].b, 16) == 0);

    cpu.x[2] = DATA_PAGE + 640;
    assert_retired(memory, &cpu,
            encode(2, 3, true, UINT8_C(0x7e), 2, 3, 2));
    assert(cpu.x[2] == DATA_PAGE + 608);
    assert_vector(&cpu.v[2], memory->first + 608, 16);
    assert_vector(&cpu.v[3], memory->first + 624, 16);
}

static void test_sp_v31_and_same_source(struct test_memory *memory) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x2000),
        .sp = DATA_PAGE + 704,
    };
    fill_vector(&cpu.v[0], 0x30);
    assert_retired(memory, &cpu, UINT32_C(0xad0203e0));
    assert(cpu.sp == DATA_PAGE + 704);
    assert(memcmp(memory->first + 768, cpu.v[0].b, 16) == 0);
    assert(memcmp(memory->first + 784, cpu.v[0].b, 16) == 0);

    fill_vector(&cpu.v[31], 0xa0);
    fill_vector(&cpu.v[30], 0xc0);
    cpu.sp = DATA_PAGE + 896;
    assert_retired(memory, &cpu,
            encode(2, 2, false, 0, 31, 30, 31));
    assert(memcmp(memory->first + 896, cpu.v[31].b, 16) == 0);
    assert(memcmp(memory->first + 912, cpu.v[30].b, 16) == 0);

    union aarch64_vector_reg expected_v31 = cpu.v[31];
    union aarch64_vector_reg expected_v30 = cpu.v[30];
    memset(cpu.v[31].b, 0, sizeof(cpu.v[31].b));
    memset(cpu.v[30].b, 0, sizeof(cpu.v[30].b));
    cpu.x[2] = DATA_PAGE + 896;
    assert_retired(memory, &cpu,
            encode(2, 2, true, 0, 31, 30, 2));
    assert(memcmp(cpu.v[31].b, expected_v31.b, 16) == 0);
    assert(memcmp(cpu.v[30].b, expected_v30.b, 16) == 0);

    cpu.x[2] = DATA_PAGE + 963;
    assert_retired(memory, &cpu,
            encode(2, 2, false, 0, 31, 30, 2));
    assert(memcmp(memory->first + 963, cpu.v[31].b, 16) == 0);
}

static void test_cross_page(struct test_memory *memory) {
    for (unsigned operation = 0; operation < 3; operation++) {
        size_t size = (size_t) 1 << (operation + 2);
        guest_addr_t address = DATA_NEXT - size;
        struct cpu_state cpu = {.pc = UINT64_C(0x2800)};
        fill_vector(&cpu.v[0], (byte_t) (0x10 + operation * 0x20));
        fill_vector(&cpu.v[1], (byte_t) (0x80 + operation * 0x20));
        cpu.x[2] = address;
        assert_retired(memory, &cpu,
                encode(operation, 2, false, 0, 0, 1, 2));
        assert(memcmp(memory->first + GUEST_MEMORY_PAGE_SIZE - size,
                cpu.v[0].b, size) == 0);
        assert(memcmp(memory->next, cpu.v[1].b, size) == 0);

        memset(cpu.v[2].b, 0, sizeof(cpu.v[2].b));
        memset(cpu.v[3].b, 0, sizeof(cpu.v[3].b));
        cpu.x[4] = address;
        assert_retired(memory, &cpu,
                encode(operation, 2, true, 0, 2, 3, 4));
        assert_vector(&cpu.v[2], cpu.v[0].b, size);
        assert_vector(&cpu.v[3], cpu.v[1].b, size);
    }
}

static void test_fault_transactions(struct test_memory *memory) {
    byte_t first_before[16];
    byte_t next_before[16];
    memset(memory->first + GUEST_MEMORY_PAGE_SIZE - 16, 0x55, 16);
    memset(memory->next, 0xaa, 16);
    memcpy(first_before,
            memory->first + GUEST_MEMORY_PAGE_SIZE - 16, 16);
    memcpy(next_before, memory->next, 16);
    memory->pages[1].permissions = GUEST_MEMORY_READ;
    guest_address_space_changed(&memory->space);

    struct cpu_state cpu = {
        .cycle = 17,
        .pc = UINT64_C(0x3000),
        .nzcv = UINT32_C(0x90000000),
    };
    fill_vector(&cpu.v[0], 0x10);
    fill_vector(&cpu.v[1], 0x90);
    cpu.x[2] = DATA_NEXT - 48;
    struct aarch64_execute_result result = execute_word(memory, &cpu,
            encode(2, 3, false, 2, 0, 1, 2));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(memory->first + GUEST_MEMORY_PAGE_SIZE - 16,
            first_before, 16) == 0);
    assert(memcmp(memory->next, next_before, 16) == 0);
    assert(cpu.x[2] == DATA_NEXT - 48);
    assert(cpu.pc == UINT64_C(0x3000) && cpu.cycle == 17);
    assert(cpu.nzcv == UINT32_C(0x90000000));

    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    union aarch64_vector_reg old_first;
    union aarch64_vector_reg old_second;
    fill_vector(&old_first, 0x33);
    fill_vector(&old_second, 0x77);
    cpu.v[2] = old_first;
    cpu.v[3] = old_second;
    cpu.x[4] = DATA_NEXT - 16;
    result = execute_word(memory, &cpu,
            encode(2, 1, true, 2, 2, 3, 4));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(cpu.v[2].b, old_first.b, 16) == 0);
    assert(memcmp(cpu.v[3].b, old_second.b, 16) == 0);
    assert(cpu.x[4] == DATA_NEXT - 16);
    assert(cpu.pc == UINT64_C(0x3000) && cpu.cycle == 17);

    cpu.x[4] = (UINT64_C(1) << 48) - 16;
    result = execute_word(memory, &cpu,
            encode(2, 2, true, 0, 2, 3, 4));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.fault.address == (UINT64_C(1) << 48));
    assert(memcmp(cpu.v[2].b, old_first.b, 16) == 0);
    assert(memcmp(cpu.v[3].b, old_second.b, 16) == 0);
}

int main(void) {
    test_apple_vectors();
    test_encoding_space();

    struct test_memory memory;
    init_test_memory(&memory);
    test_widths(&memory);
    test_address_modes(&memory);
    test_sp_v31_and_same_source(&memory);
    test_cross_page(&memory);
    test_fault_transactions(&memory);
    return 0;
}

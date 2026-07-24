#include <assert.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define DATA_PAGE UINT64_C(0x0000456789abc000)
#define DATA_NEXT (DATA_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define LD1R_FIXED_MASK UINT32_C(0xbffff000)
#define LD1R_FIXED_BITS UINT32_C(0x0d40c000)
#define LD1R_VARIABLE_MASK UINT32_C(0x40000fff)

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
    for (size_t index = 0; index < array_size(memory->pages); index++) {
        if (memory->pages[index].address == page_base) {
            *view = (struct guest_page_view) {
                .host_page = memory->pages[index].host_page,
                .permissions = memory->pages[index].permissions,
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

static dword_t encode_ld1r(bool quadword, byte_t size_field,
        byte_t rt, byte_t rn) {
    return LD1R_FIXED_BITS |
            (dword_t) quadword << 30 |
            (dword_t) size_field << 10 |
            (dword_t) rn << 5 |
            rt;
}

static bool decode_is_ld1r(dword_t word,
        struct aarch64_decoded *instruction) {
    return aarch64_decode(word, instruction) &&
            instruction->opcode == AARCH64_OP_LOAD_SIMD_REPLICATE_1;
}

static void assert_decode(dword_t word, bool quadword,
        byte_t size_field, byte_t rt, byte_t rn) {
    struct aarch64_decoded instruction = {0};
    assert(decode_is_ld1r(word, &instruction));
    assert(instruction.width == (quadword ? 128 : 64));
    assert(instruction.operands.advsimd_multiple.rt == rt);
    assert(instruction.operands.advsimd_multiple.rn == rn);
    assert(instruction.operands.advsimd_multiple.element_size ==
            (byte_t) (1U << size_field));
}

static void test_decode(void) {
    static const struct {
        dword_t word;
        bool quadword;
        byte_t size_field;
        byte_t rt;
        byte_t rn;
    } goldens[] = {
        {UINT32_C(0x0d40c020), false, 0, 0, 1},
        {UINT32_C(0x4d40c062), true, 0, 2, 3},
        {UINT32_C(0x0d40c4a4), false, 1, 4, 5},
        {UINT32_C(0x4d40c4e6), true, 1, 6, 7},
        {UINT32_C(0x0d40c928), false, 2, 8, 9},
        {UINT32_C(0x4d40c96a), true, 2, 10, 11},
        {UINT32_C(0x0d40cdac), false, 3, 12, 13},
        {UINT32_C(0x4d40cdee), true, 3, 14, 15},
        // GNU assembler 实际触发的 V31.16B/X27 形式。
        {UINT32_C(0x4d40c37f), true, 0, 31, 27},
    };
    for (size_t index = 0; index < array_size(goldens); index++) {
        assert_decode(goldens[index].word, goldens[index].quadword,
                goldens[index].size_field,
                goldens[index].rt, goldens[index].rn);
    }

    assert((LD1R_FIXED_MASK & LD1R_VARIABLE_MASK) == 0);
    assert((LD1R_FIXED_MASK | LD1R_VARIABLE_MASK) == UINT32_MAX);
    unsigned legal = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rt = 0; rt < 32; rt++) {
                    assert_decode(encode_ld1r(q != 0, (byte_t) size,
                            (byte_t) rt, (byte_t) rn), q != 0,
                            (byte_t) size, (byte_t) rt, (byte_t) rn);
                    legal++;
                }
            }
        }
    }
    assert(legal == 8192);

    const dword_t product = UINT32_C(0x4d40c37f);
    unsigned fixed_bits = 0;
    for (unsigned bit = 0; bit < 32; bit++) {
        if ((LD1R_FIXED_MASK & (UINT32_C(1) << bit)) == 0)
            continue;
        struct aarch64_decoded instruction = {0};
        assert(!decode_is_ld1r(
                product ^ (UINT32_C(1) << bit), &instruction));
        fixed_bits++;
    }
    assert(fixed_bits == 19);

    static const dword_t neighbors[] = {
        UINT32_C(0x4d60c37f), // LD2R。
        UINT32_C(0x4d40e37f), // LD3R。
        UINT32_C(0x4d60e37f), // LD4R。
        UINT32_C(0x4ddfc37f), // LD1R 立即数 post-index。
        UINT32_C(0x4dc2c37f), // LD1R 寄存器 post-index。
        UINT32_C(0x4d401f7f), // LD1 single-lane。
        UINT32_C(0x4d001f7f), // ST1 single-lane。
        UINT32_C(0x4c40737f), // 完整向量 LD1。
        UINT32_C(0x3d40037f), // 普通 SIMD LDR B。
    };
    for (size_t index = 0; index < array_size(neighbors); index++) {
        struct aarch64_decoded instruction = {0};
        assert(!decode_is_ld1r(neighbors[index], &instruction));
    }

    static const dword_t reserved[] = {
        UINT32_C(0x4d00c37f), // replicate 类不存在 store 形式。
        UINT32_C(0x4d40d37f), // LD1R 无 S 位形式。
        UINT32_C(0x4d41c37f), // 无写回形式要求 Rm 为零。
    };
    for (size_t index = 0; index < array_size(reserved); index++) {
        struct aarch64_decoded instruction = {0};
        assert(!aarch64_decode(reserved[index], &instruction));
    }
}

static struct aarch64_execute_result execute_word(
        struct test_memory *memory, struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = {0};
    assert(decode_is_ld1r(word, &instruction));
    return aarch64_execute(cpu, &memory->tlb, &instruction);
}

static void fill_cpu(struct cpu_state *cpu, byte_t poison) {
    *cpu = (struct cpu_state) {
        .cycle = UINT64_C(0x1122334455667788),
        .sp = UINT64_C(0x8877665544332211),
        .pc = UINT64_C(0x1020304050607000),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_DN | AARCH64_FPCR_FZ,
        .fpsr = AARCH64_FPSR_QC | AARCH64_FPSR_IXC,
        .tpidr_el0 = UINT64_C(0x0102030405060708),
        .segfault_addr = UINT64_C(0x9988776655443322),
        .segfault_was_write = true,
        .trapno = UINT32_C(0xa5a55a5a),
        .single_step = true,
        ._poked = true,
    };
    for (byte_t reg = 0; reg < 31; reg++)
        cpu->x[reg] = UINT64_C(0x2000000000000000) + reg;
    for (byte_t reg = 0; reg < 32; reg++)
        memset(cpu->v[reg].b, poison + reg, sizeof(cpu->v[reg].b));
}

static union aarch64_vector_reg reference_replicate(
        const byte_t element[8], byte_t element_size, bool quadword) {
    union aarch64_vector_reg result = {0};
    byte_t vector_size = quadword ? 16 : 8;
    for (byte_t offset = 0; offset < vector_size; offset += element_size)
        memcpy(result.b + offset, element, element_size);
    return result;
}

static void test_all_registers_and_arrangements(
        struct test_memory *memory) {
    const guest_addr_t address = DATA_PAGE + 259;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            byte_t element_size = (byte_t) (1U << size);
            byte_t element[8] = {0};
            for (byte_t byte = 0; byte < element_size; byte++)
                element[byte] = (byte_t) (0x31 + q * 37 + size * 19 +
                        byte * 11);
            memcpy(memory->first + 259, element, element_size);
            byte_t first_before[GUEST_MEMORY_PAGE_SIZE];
            byte_t next_before[GUEST_MEMORY_PAGE_SIZE];
            memcpy(first_before, memory->first, sizeof(first_before));
            memcpy(next_before, memory->next, sizeof(next_before));
            union aarch64_vector_reg expected_value =
                    reference_replicate(element, element_size, q != 0);

            for (unsigned rn = 0; rn < 32; rn++) {
                for (unsigned rt = 0; rt < 32; rt++) {
                    struct cpu_state cpu;
                    fill_cpu(&cpu, 0x40);
                    if (rn == 31)
                        cpu.sp = address;
                    else
                        cpu.x[rn] = address;
                    struct cpu_state expected = cpu;
                    expected.v[rt] = expected_value;
                    expected.pc += 4;

                    struct aarch64_execute_result result = execute_word(
                            memory, &cpu, encode_ld1r(q != 0,
                                    (byte_t) size,
                                    (byte_t) rt, (byte_t) rn));
                    assert(result.stop == AARCH64_EXECUTE_RETIRED);
                    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
                    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
                }
            }
            assert(memcmp(memory->first,
                    first_before, sizeof(first_before)) == 0);
            assert(memcmp(memory->next,
                    next_before, sizeof(next_before)) == 0);
        }
    }
}

static void test_cross_page_transaction(struct test_memory *memory) {
    static const byte_t element[8] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
    };
    memcpy(memory->first + GUEST_MEMORY_PAGE_SIZE - 4, element, 4);
    memcpy(memory->next, element + 4, 4);
    byte_t first_before[GUEST_MEMORY_PAGE_SIZE];
    byte_t next_before[GUEST_MEMORY_PAGE_SIZE];
    memcpy(first_before, memory->first, sizeof(first_before));
    memcpy(next_before, memory->next, sizeof(next_before));

    struct cpu_state cpu;
    fill_cpu(&cpu, 0x70);
    cpu.x[2] = DATA_NEXT - 4;
    struct cpu_state expected = cpu;
    expected.v[31] = reference_replicate(element, 8, true);
    expected.pc += 4;
    struct aarch64_execute_result result = execute_word(
            memory, &cpu, encode_ld1r(true, 3, 31, 2));
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
    assert(memcmp(memory->first,
            first_before, sizeof(first_before)) == 0);
    assert(memcmp(memory->next,
            next_before, sizeof(next_before)) == 0);

    fill_cpu(&cpu, 0x25);
    cpu.x[2] = DATA_NEXT - 4;
    struct cpu_state before = cpu;
    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    result = execute_word(memory, &cpu, encode_ld1r(true, 3, 31, 2));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(&cpu, &before, sizeof(cpu)) == 0);
    assert(memcmp(memory->first,
            first_before, sizeof(first_before)) == 0);
    assert(memcmp(memory->next,
            next_before, sizeof(next_before)) == 0);
}

int main(void) {
    test_decode();
    struct test_memory memory;
    init_test_memory(&memory);
    test_all_registers_and_arrangements(&memory);
    test_cross_page_transaction(&memory);
    return 0;
}

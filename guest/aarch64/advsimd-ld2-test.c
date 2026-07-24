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

static dword_t encode_ld2(bool quadword, unsigned size_field,
        byte_t rt, byte_t rn) {
    return UINT32_C(0x0c408000) |
            (dword_t) quadword << 30 |
            (dword_t) size_field << 10 |
            (dword_t) rn << 5 |
            rt;
}

static bool decode_is_ld2(dword_t word) {
    struct aarch64_decoded instruction;
    return aarch64_decode(word, &instruction) &&
            instruction.opcode == AARCH64_OP_LOAD_SIMD_MULTIPLE_2;
}

static void test_decode(void) {
    static const dword_t goldens[] = {
        UINT32_C(0x0c408040), UINT32_C(0x4c408062),
        UINT32_C(0x0c4084a4), UINT32_C(0x4c4084e6),
        UINT32_C(0x0c408928), UINT32_C(0x4c40896a),
        UINT32_C(0x4c408dac), UINT32_C(0x4c40883e),
        UINT32_C(0x4c408bff),
    };
    for (size_t index = 0; index < array_size(goldens); index++)
        assert(decode_is_ld2(goldens[index]));

    unsigned legal = 0;
    unsigned reserved = 0;
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned size = 0; size < 4; size++) {
            for (byte_t rn = 0; rn < 32; rn++) {
                for (byte_t rt = 0; rt < 32; rt++) {
                    dword_t word = encode_ld2(q != 0, size, rt, rn);
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(word, &instruction);
                    bool valid = q != 0 || size != 3;
                    assert(decoded == valid);
                    if (!valid) {
                        reserved++;
                        continue;
                    }
                    legal++;
                    assert(instruction.opcode ==
                            AARCH64_OP_LOAD_SIMD_MULTIPLE_2);
                    assert(instruction.width == (q ? 128 : 64));
                    assert(instruction.operands.advsimd_multiple.rt == rt);
                    assert(instruction.operands.advsimd_multiple.rn == rn);
                    assert(instruction.operands.advsimd_multiple.element_size ==
                            (byte_t) (1U << size));
                }
            }
        }
    }
    assert(legal == 7168 && reserved == 1024);

    const dword_t fixed_mask = UINT32_C(0xbffff000);
    for (byte_t bit = 0; bit < 32; bit++) {
        if ((fixed_mask & (UINT32_C(1) << bit)) != 0)
            assert(!decode_is_ld2(
                    UINT32_C(0x4c40883e) ^ (UINT32_C(1) << bit)));
    }
    static const dword_t neighbors[] = {
        UINT32_C(0x4c00883e), UINT32_C(0x4c40a83e),
        UINT32_C(0x4c40483e), UINT32_C(0x4c40083e),
        UINT32_C(0x4cdf883e), UINT32_C(0x4cc2883e),
        UINT32_C(0x4d60c83e), UINT32_C(0x4d60803e),
        UINT32_C(0x4c40783e), UINT32_C(0x4c40683e),
        UINT32_C(0x4c40283e), UINT32_C(0x3dc0003e),
        UINT32_C(0xad407c3e),
        UINT32_C(0x4c41883e), UINT32_C(0x4c60883e),
    };
    for (size_t index = 0; index < array_size(neighbors); index++)
        assert(!decode_is_ld2(neighbors[index]));
}

static void prepare_interleaved(byte_t *memory,
        union aarch64_vector_reg expected[2], byte_t vector_size,
        byte_t element_size, byte_t seed) {
    memset(expected, 0, 2 * sizeof(*expected));
    for (byte_t index = 0; index < 2 * vector_size; index++)
        memory[index] = (byte_t) (seed + index * 13);
    for (byte_t offset = 0; offset < vector_size;
            offset += element_size) {
        for (byte_t structure = 0; structure < 2; structure++) {
            memcpy(expected[structure].b + offset,
                    memory + 2 * offset + structure * element_size,
                    element_size);
        }
    }
}

static struct aarch64_execute_result execute_word(
        struct test_memory *memory, struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return aarch64_execute(cpu, &memory->tlb, &instruction);
}

static void fill_cpu(struct cpu_state *cpu, byte_t poison) {
    *cpu = (struct cpu_state) {
        .cycle = UINT64_C(0x1122334455667788),
        .pc = UINT64_C(0x8877665544332200),
        .nzcv = UINT32_C(0xa0000000),
        .fpcr = AARCH64_FPCR_DN | AARCH64_FPCR_FZ,
        .fpsr = AARCH64_FPSR_QC | AARCH64_FPSR_IXC,
        .tpidr_el0 = UINT64_C(0x1020304050607080),
        .segfault_addr = UINT64_C(0x9988776655443322),
        .segfault_was_write = true,
        .trapno = UINT32_C(0xa5a55a5a),
        .single_step = true,
        ._poked = true,
    };
    for (byte_t reg = 0; reg < 31; reg++)
        cpu->x[reg] = UINT64_C(0x1000000000000000) + reg;
    for (byte_t reg = 0; reg < 32; reg++)
        memset(cpu->v[reg].b, poison + reg, sizeof(cpu->v[reg].b));
}

static void test_all_registers_and_arrangements(
        struct test_memory *memory) {
    static const struct {
        bool quadword;
        byte_t size_field;
    } forms[] = {
        {false, 0}, {true, 0}, {false, 1}, {true, 1},
        {false, 2}, {true, 2}, {true, 3},
    };
    byte_t first_before[GUEST_MEMORY_PAGE_SIZE];
    byte_t next_before[GUEST_MEMORY_PAGE_SIZE];
    for (size_t form = 0; form < array_size(forms); form++) {
        byte_t vector_size = forms[form].quadword ? 16 : 8;
        byte_t element_size = (byte_t) (1U << forms[form].size_field);
        union aarch64_vector_reg values[2];
        prepare_interleaved(memory->first + 256, values,
                vector_size, element_size, (byte_t) (0x20 + form * 17));
        memcpy(first_before, memory->first, sizeof(first_before));
        memcpy(next_before, memory->next, sizeof(next_before));
        for (byte_t rn = 0; rn < 32; rn++) {
            for (byte_t rt = 0; rt < 32; rt++) {
                struct cpu_state cpu;
                fill_cpu(&cpu, 0x40);
                if (rn == 31)
                    cpu.sp = DATA_PAGE + 256;
                else
                    cpu.x[rn] = DATA_PAGE + 256;
                struct cpu_state expected = cpu;
                expected.v[rt] = values[0];
                expected.v[(rt + 1) & 31] = values[1];
                expected.pc += 4;
                struct aarch64_execute_result result = execute_word(
                        memory, &cpu, encode_ld2(forms[form].quadword,
                                forms[form].size_field, rt, rn));
                assert(result.stop == AARCH64_EXECUTE_RETIRED);
                assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);
            }
        }
        assert(memcmp(memory->first,
                first_before, sizeof(first_before)) == 0);
        assert(memcmp(memory->next,
                next_before, sizeof(next_before)) == 0);
    }
}

static void test_cross_page_transaction(struct test_memory *memory) {
    byte_t crossing[32];
    union aarch64_vector_reg values[2];
    prepare_interleaved(crossing, values, 16, 4, 0x91);
    memcpy(memory->first + GUEST_MEMORY_PAGE_SIZE - 16, crossing, 16);
    memcpy(memory->next, crossing + 16, 16);

    struct cpu_state cpu;
    fill_cpu(&cpu, 0x70);
    cpu.x[2] = DATA_NEXT - 16;
    struct cpu_state expected = cpu;
    expected.v[31] = values[0];
    expected.v[0] = values[1];
    expected.pc += 4;
    struct aarch64_execute_result result = execute_word(
            memory, &cpu, encode_ld2(true, 2, 31, 2));
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(memcmp(&cpu, &expected, sizeof(cpu)) == 0);

    fill_cpu(&cpu, 0x35);
    cpu.x[2] = DATA_NEXT - 16;
    struct cpu_state before = cpu;
    memory->pages[1].permissions = GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    result = execute_word(memory, &cpu, encode_ld2(true, 2, 31, 2));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(memcmp(&cpu, &before, sizeof(cpu)) == 0);
}

int main(void) {
    test_decode();
    struct test_memory memory;
    init_test_memory(&memory);
    test_all_registers_and_arrangements(&memory);
    test_cross_page_transaction(&memory);
    return 0;
}

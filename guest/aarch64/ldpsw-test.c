#include <assert.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define DATA_PAGE UINT64_C(0x00003456789ab000)
#define DATA_NEXT (DATA_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define UNMAPPED_PAGE (DATA_NEXT + GUEST_MEMORY_PAGE_SIZE)

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
    return UINT32_C(0x28000000) |
            (dword_t) operation << 30 |
            (dword_t) mode << 23 |
            (dword_t) load << 22 |
            (dword_t) imm7 << 15 |
            (dword_t) rt2 << 10 |
            (dword_t) rn << 5 |
            rt;
}

static void put_dword(byte_t *destination, dword_t value) {
    for (byte_t i = 0; i < 4; i++)
        destination[i] = (byte_t) (value >> (i * 8));
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

static enum aarch64_address_mode expected_address_mode(unsigned mode) {
    if (mode == 1)
        return AARCH64_ADDRESS_POST_INDEX;
    if (mode == 3)
        return AARCH64_ADDRESS_PRE_INDEX;
    return AARCH64_ADDRESS_OFFSET;
}

static void test_known_encodings(void) {
    static const struct {
        dword_t word;
        byte_t rt;
        byte_t rt2;
        byte_t rn;
        int64_t offset;
        enum aarch64_address_mode address_mode;
    } cases[] = {
        {UINT32_C(0x69400440), 0, 1, 2, 0,
                AARCH64_ADDRESS_OFFSET},
        {UINT32_C(0x68dfa548), 8, 9, 10, 252,
                AARCH64_ADDRESS_POST_INDEX},
        {UINT32_C(0x69e047f0), 16, 17, 31, -256,
                AARCH64_ADDRESS_PRE_INDEX},
    };

    for (size_t i = 0; i < array_size(cases); i++) {
        struct aarch64_decoded instruction;
        assert(aarch64_decode(cases[i].word, &instruction));
        assert(instruction.opcode == AARCH64_OP_LOAD_PAIR);
        assert(instruction.width == 64);
        assert(instruction.operands.load_store_pair.rt == cases[i].rt);
        assert(instruction.operands.load_store_pair.rt2 == cases[i].rt2);
        assert(instruction.operands.load_store_pair.rn == cases[i].rn);
        assert(instruction.operands.load_store_pair.offset == cases[i].offset);
        assert(instruction.operands.load_store_pair.address_mode ==
                cases[i].address_mode);
        assert(instruction.operands.load_store_pair.signed_load);
    }
}

static void test_encoding_space(void) {
    unsigned valid = 0;
    for (unsigned operation = 0; operation < 4; operation++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            for (unsigned load = 0; load < 2; load++) {
                for (unsigned imm7 = 0; imm7 < 128; imm7++) {
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(encode(operation, mode,
                            load != 0, (byte_t) imm7, 0, 1, 2),
                            &instruction);
                    bool expected = mode != 0 && operation != 3 &&
                            (operation != 1 || load != 0);
                    assert(decoded == expected);
                    if (!decoded)
                        continue;
                    valid++;
                    bool signed_load = operation == 1;
                    unsigned size = operation == 2 ? 8 : 4;
                    int immediate = imm7 < 64 ?
                            (int) imm7 : (int) imm7 - 128;
                    assert(instruction.opcode == (load != 0 ?
                            AARCH64_OP_LOAD_PAIR : AARCH64_OP_STORE_PAIR));
                    assert(instruction.width ==
                            (signed_load ? 64 : size * 8));
                    assert(instruction.operands.load_store_pair.offset ==
                            (int64_t) immediate * (int64_t) size);
                    assert(instruction.operands.load_store_pair.address_mode ==
                            expected_address_mode(mode));
                    assert(instruction.operands.load_store_pair.signed_load ==
                            signed_load);
                }
            }
        }
    }
    assert(valid == 1920);
}

static void test_reserved_and_overlaps(void) {
    static const dword_t invalid[] = {
        UINT32_C(0x68400440),
        UINT32_C(0x69000440),
        UINT32_C(0x69400040),
        UINT32_C(0x68c10442),
        UINT32_C(0x69ff0840),
    };
    for (size_t i = 0; i < array_size(invalid); i++) {
        struct aarch64_decoded instruction;
        assert(!aarch64_decode(invalid[i], &instruction));
    }

    struct aarch64_decoded instruction;
    assert(aarch64_decode(encode(1, 2, true, 0, 10, 11, 10),
            &instruction));
    assert(aarch64_decode(encode(1, 3, true, 0, 0, 1, 31),
            &instruction));
}

static void test_sign_extension_and_modes(struct test_memory *memory) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1000),
        .sp = UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0xa0000000),
    };
    put_dword(memory->first + 64, UINT32_C(0x80000001));
    put_dword(memory->first + 68, UINT32_C(0x7ffffffe));
    cpu.x[2] = DATA_PAGE + 64;
    assert_retired(memory, &cpu, UINT32_C(0x69400440));
    assert(cpu.x[0] == UINT64_C(0xffffffff80000001));
    assert(cpu.x[1] == UINT64_C(0x000000007ffffffe));
    assert(cpu.x[2] == DATA_PAGE + 64);
    assert(cpu.sp == UINT64_C(0x1122334455667788));
    assert(cpu.nzcv == UINT32_C(0xa0000000));

    put_dword(memory->first + 128, UINT32_MAX);
    put_dword(memory->first + 132, UINT32_C(0x12345678));
    cpu.x[5] = DATA_PAGE + 128;
    assert_retired(memory, &cpu, encode(1, 1, true, 2, 3, 4, 5));
    assert(cpu.x[3] == UINT64_MAX);
    assert(cpu.x[4] == UINT32_C(0x12345678));
    assert(cpu.x[5] == DATA_PAGE + 136);

    put_dword(memory->first + 200, UINT32_C(0x87654321));
    put_dword(memory->first + 204, 0);
    cpu.sp = DATA_PAGE + 208;
    assert_retired(memory, &cpu,
            encode(1, 3, true, UINT8_C(0x7e), 6, 7, 31));
    assert(cpu.x[6] == UINT64_C(0xffffffff87654321));
    assert(cpu.x[7] == 0);
    assert(cpu.sp == DATA_PAGE + 200);

    put_dword(memory->first + 256, UINT32_C(0x80000000));
    put_dword(memory->first + 260, 42);
    cpu.x[10] = DATA_PAGE + 256;
    assert_retired(memory, &cpu, encode(1, 2, true, 0, 10, 11, 10));
    assert(cpu.x[10] == UINT64_C(0xffffffff80000000));
    assert(cpu.x[11] == 42);

    put_dword(memory->first + 300, UINT32_C(0x80000002));
    put_dword(memory->first + 304, UINT32_C(0xfffffffe));
    cpu.x[9] = DATA_PAGE + 300;
    assert_retired(memory, &cpu, encode(1, 2, true, 0, 31, 8, 9));
    assert(cpu.x[8] == UINT64_C(0xfffffffffffffffe));
}

static void test_cross_page_and_fault_transaction(struct test_memory *memory) {
    put_dword(memory->first + GUEST_MEMORY_PAGE_SIZE - 4,
            UINT32_C(0x80000003));
    put_dword(memory->next, UINT32_C(0x70000004));
    struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
    cpu.x[12] = DATA_NEXT - 4;
    assert_retired(memory, &cpu, encode(1, 2, true, 0, 13, 14, 12));
    assert(cpu.x[13] == UINT64_C(0xffffffff80000003));
    assert(cpu.x[14] == UINT64_C(0x0000000070000004));

    put_dword(memory->next + GUEST_MEMORY_PAGE_SIZE - 4,
            UINT32_C(0x80000005));
    cpu = (struct cpu_state) {
        .cycle = 17,
        .pc = UINT64_C(0x3000),
        .sp = UNMAPPED_PAGE + 4,
        .x[3] = UINT64_C(0x1111111111111111),
        .x[4] = UINT64_C(0x2222222222222222),
        .nzcv = UINT32_C(0x90000000),
    };
    struct aarch64_execute_result result = execute_word(memory, &cpu,
            encode(1, 3, true, UINT8_C(0x7e), 3, 4, 31));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[3] == UINT64_C(0x1111111111111111));
    assert(cpu.x[4] == UINT64_C(0x2222222222222222));
    assert(cpu.sp == UNMAPPED_PAGE + 4);
    assert(cpu.pc == UINT64_C(0x3000));
    assert(cpu.cycle == 17);
    assert(cpu.nzcv == UINT32_C(0x90000000));
}

int main(void) {
    test_known_encodings();
    test_encoding_space();
    test_reserved_and_overlaps();

    struct test_memory memory;
    init_test_memory(&memory);
    test_sign_extension_and_modes(&memory);
    test_cross_page_and_fault_transaction(&memory);
    return 0;
}

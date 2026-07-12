#include <assert.h>
#include <string.h>

#include "guest/aarch64/runner.h"

#define CODE_PAGE UINT64_C(0x000023456789a000)
#define DATA_PAGE UINT64_C(0x00003456789ab000)
#define DATA_NEXT (DATA_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define UNMAPPED_PAGE (DATA_NEXT + GUEST_MEMORY_PAGE_SIZE)

struct test_page {
    guest_addr_t address;
    byte_t *host_page;
    unsigned permissions;
};

struct test_memory {
    byte_t code[GUEST_MEMORY_PAGE_SIZE];
    byte_t data[GUEST_MEMORY_PAGE_SIZE];
    byte_t next[GUEST_MEMORY_PAGE_SIZE];
    struct test_page pages[3];
    struct guest_address_space space;
    struct guest_tlb tlb;
    struct aarch64_runner runner;
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
        .address = CODE_PAGE,
        .host_page = memory->code,
        .permissions = GUEST_MEMORY_EXECUTE,
    };
    memory->pages[1] = (struct test_page) {
        .address = DATA_PAGE,
        .host_page = memory->data,
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
    };
    memory->pages[2] = (struct test_page) {
        .address = DATA_NEXT,
        .host_page = memory->next,
        .permissions = GUEST_MEMORY_READ,
    };
    guest_address_space_init(&memory->space, &test_ops, memory, 48);
    guest_tlb_init(&memory->tlb, &memory->space);
    aarch64_runner_init(&memory->runner, &memory->tlb);
}

static void put_instruction(byte_t *destination, dword_t instruction) {
    destination[0] = (byte_t) instruction;
    destination[1] = (byte_t) (instruction >> 8);
    destination[2] = (byte_t) (instruction >> 16);
    destination[3] = (byte_t) (instruction >> 24);
}

static void put_qword(byte_t *destination, qword_t value) {
    for (byte_t i = 0; i < 8; i++)
        destination[i] = (byte_t) (value >> (i * 8));
}

static struct aarch64_step_result run_instruction(struct test_memory *memory,
        struct cpu_state *cpu, dword_t instruction) {
    put_instruction(memory->code, instruction);
    cpu->pc = CODE_PAGE;
    return aarch64_run_one(&memory->runner, cpu);
}

static void assert_retired(struct aarch64_step_result result,
        const struct cpu_state *cpu) {
    assert(result.stop == AARCH64_STEP_RETIRED);
    assert(cpu->pc == CODE_PAGE + 4);
}

static void test_loads(struct test_memory *memory) {
    struct cpu_state cpu = {0};

    qword_t value = UINT64_C(0x8877665544332211);
    put_qword(&memory->data[1], value);
    cpu.x[1] = DATA_PAGE + 1;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf9400020)), &cpu);
    assert(cpu.x[0] == value);

    memory->data[12] = 0x78;
    memory->data[13] = 0x56;
    memory->data[14] = 0x34;
    memory->data[15] = 0x12;
    cpu.x[2] = UINT64_MAX;
    cpu.x[3] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb9400c62)), &cpu);
    assert(cpu.x[2] == UINT32_C(0x12345678));

    memory->data[7] = 0xab;
    cpu.x[4] = UINT64_MAX;
    cpu.x[5] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x39401ca4)), &cpu);
    assert(cpu.x[4] == 0xab);

    memory->data[6] = 0xcd;
    memory->data[7] = 0xab;
    cpu.x[6] = UINT64_MAX;
    cpu.x[7] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x79400ce6)), &cpu);
    assert(cpu.x[6] == UINT16_C(0xabcd));

    put_qword(&memory->data[16], UINT64_C(0x1020304050607080));
    cpu.sp = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf9400be9)), &cpu);
    assert(cpu.x[9] == UINT64_C(0x1020304050607080));

    put_qword(&memory->data[64], UINT64_C(0xabcdef0123456789));
    cpu.x[1] = DATA_PAGE + 64;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf9400021)), &cpu);
    assert(cpu.x[1] == UINT64_C(0xabcdef0123456789));

    value = UINT64_C(0x0102030405060708);
    byte_t encoded[8];
    put_qword(encoded, value);
    memcpy(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4], encoded, 4);
    memcpy(memory->next, encoded + 4, 4);
    cpu.x[1] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf9400020)), &cpu);
    assert(cpu.x[0] == value);

    put_qword(&memory->data[80], UINT64_C(0xfedcba9876543210));
    cpu.x[10] = DATA_PAGE + 80;
    qword_t old_x0 = cpu.x[0];
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf940015f)), &cpu);
    assert(cpu.x[0] == old_x0);
}

static void test_stores(struct test_memory *memory) {
    struct cpu_state cpu = {0};

    cpu.x[0] = UINT64_C(0x8877665544332211);
    cpu.x[1] = DATA_PAGE + 1;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf9000020)), &cpu);
    const byte_t expected_qword[] = {0x11, 0x22, 0x33, 0x44,
            0x55, 0x66, 0x77, 0x88};
    assert(memcmp(&memory->data[1], expected_qword,
            sizeof(expected_qword)) == 0);

    cpu.x[2] = UINT64_C(0xffffffff12345678);
    cpu.x[3] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb9000c62)), &cpu);
    const byte_t expected_word[] = {0x78, 0x56, 0x34, 0x12};
    assert(memcmp(&memory->data[12], expected_word,
            sizeof(expected_word)) == 0);

    cpu.x[4] = UINT64_C(0x1234ab);
    cpu.x[5] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x39001ca4)), &cpu);
    assert(memory->data[7] == 0xab);

    cpu.x[6] = UINT64_C(0x1234abcd);
    cpu.x[7] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x79000ce6)), &cpu);
    assert(memory->data[6] == 0xcd);
    assert(memory->data[7] == 0xab);

    memset(&memory->data[96], 0xff, 8);
    cpu.x[11] = DATA_PAGE + 96;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf900017f)), &cpu);
    const byte_t zeros[8] = {0};
    assert(memcmp(&memory->data[96], zeros, sizeof(zeros)) == 0);
}

static void test_unscaled_and_writeback(struct test_memory *memory) {
    struct cpu_state cpu = {0};
    put_qword(&memory->data[8], UINT64_C(0x1020304050607080));
    cpu.x[1] = DATA_PAGE + 16;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf85f8020)), &cpu);
    assert(cpu.x[0] == UINT64_C(0x1020304050607080));
    assert(cpu.x[1] == DATA_PAGE + 16);

    cpu.x[2] = UINT64_C(0xffffffff12345678);
    cpu.x[3] = DATA_PAGE + 32;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb8007062)), &cpu);
    const byte_t expected_word[] = {0x78, 0x56, 0x34, 0x12};
    assert(memcmp(&memory->data[39], expected_word,
            sizeof(expected_word)) == 0);

    put_qword(&memory->data[48], UINT64_C(0x8877665544332211));
    cpu.x[5] = DATA_PAGE + 48;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf84084a4)), &cpu);
    assert(cpu.x[4] == UINT64_C(0x8877665544332211));
    assert(cpu.x[5] == DATA_PAGE + 56);

    cpu.x[6] = UINT64_C(0xffffffff89abcdef);
    cpu.x[7] = DATA_PAGE + 68;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb81fcce6)), &cpu);
    const byte_t expected_pre[] = {0xef, 0xcd, 0xab, 0x89};
    assert(memcmp(&memory->data[64], expected_pre,
            sizeof(expected_pre)) == 0);
    assert(cpu.x[7] == DATA_PAGE + 64);
}

struct unprivileged_decode_case {
    dword_t word;
    enum aarch64_opcode opcode;
    byte_t size;
    byte_t width;
    byte_t rn;
    byte_t rt;
    int64_t offset;
    bool signed_load;
};

static void test_unprivileged_decode(void) {
    static const struct unprivileged_decode_case cases[] = {
        {UINT32_C(0x38100820), AARCH64_OP_STORE_UNPRIVILEGED,
                1, 32, 1, 0, -256, false},
        {UINT32_C(0x384ff862), AARCH64_OP_LOAD_UNPRIVILEGED,
                1, 32, 3, 2, 255, false},
        {UINT32_C(0x389ff8a4), AARCH64_OP_LOAD_UNPRIVILEGED,
                1, 64, 5, 4, -1, true},
        {UINT32_C(0x38c018e6), AARCH64_OP_LOAD_UNPRIVILEGED,
                1, 32, 7, 6, 1, true},
        {UINT32_C(0x781fe928), AARCH64_OP_STORE_UNPRIVILEGED,
                2, 32, 9, 8, -2, false},
        {UINT32_C(0x7840296a), AARCH64_OP_LOAD_UNPRIVILEGED,
                2, 32, 11, 10, 2, false},
        {UINT32_C(0x789fd9ac), AARCH64_OP_LOAD_UNPRIVILEGED,
                2, 64, 13, 12, -3, true},
        {UINT32_C(0x78c039ee), AARCH64_OP_LOAD_UNPRIVILEGED,
                2, 32, 15, 14, 3, true},
        {UINT32_C(0xb81fca30), AARCH64_OP_STORE_UNPRIVILEGED,
                4, 32, 17, 16, -4, false},
        {UINT32_C(0xb8404a72), AARCH64_OP_LOAD_UNPRIVILEGED,
                4, 32, 19, 18, 4, false},
        {UINT32_C(0xb89fbab4), AARCH64_OP_LOAD_UNPRIVILEGED,
                4, 64, 21, 20, -5, true},
        {UINT32_C(0xf8005af6), AARCH64_OP_STORE_UNPRIVILEGED,
                8, 64, 23, 22, 5, false},
        {UINT32_C(0xf85f8b38), AARCH64_OP_LOAD_UNPRIVILEGED,
                8, 64, 25, 24, -8, false},
    };
    for (size_t index = 0; index < array_size(cases); index++) {
        const struct unprivileged_decode_case *test = &cases[index];
        struct aarch64_decoded instruction;
        assert(aarch64_decode(test->word, &instruction));
        assert(instruction.opcode == test->opcode);
        assert(instruction.width == test->width);
        assert(instruction.operands.load_store.size == test->size);
        assert(instruction.operands.load_store.rn == test->rn);
        assert(instruction.operands.load_store.rt == test->rt);
        assert(instruction.operands.load_store.offset == test->offset);
        assert(instruction.operands.load_store.address_mode ==
                AARCH64_ADDRESS_OFFSET);
        assert(instruction.operands.load_store.signed_load ==
                test->signed_load);
    }

    struct aarch64_decoded overlapping;
    assert(aarch64_decode(UINT32_C(0xf8400821), &overlapping));
    assert(overlapping.opcode == AARCH64_OP_LOAD_UNPRIVILEGED);
    assert(overlapping.operands.load_store.rn == 1 &&
            overlapping.operands.load_store.rt == 1);
}

static void test_unprivileged_transfers(struct test_memory *memory) {
    struct cpu_state cpu = {.nzcv = UINT32_C(0xa0000000)};

    cpu.x[0] = 0xab;
    cpu.x[1] = DATA_PAGE + 256;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38100820)), &cpu);
    assert(memory->data[0] == 0xab && cpu.x[1] == DATA_PAGE + 256);

    memory->data[255] = 0xcd;
    cpu.x[2] = UINT64_MAX;
    cpu.x[3] = DATA_PAGE;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x384ff862)), &cpu);
    assert(cpu.x[2] == 0xcd && cpu.x[3] == DATA_PAGE);

    memory->data[300] = 0x80;
    cpu.x[5] = DATA_PAGE + 301;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x389ff8a4)), &cpu);
    assert(cpu.x[4] == UINT64_C(0xffffffffffffff80));

    memory->data[302] = 0x81;
    cpu.x[7] = DATA_PAGE + 301;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38c018e6)), &cpu);
    assert(cpu.x[6] == UINT32_C(0xffffff81));

    cpu.x[8] = UINT32_C(0x1234abcd);
    cpu.x[9] = DATA_PAGE + 322;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x781fe928)), &cpu);
    assert(memory->data[320] == 0xcd && memory->data[321] == 0xab);

    memory->data[332] = 0x78;
    memory->data[333] = 0x56;
    cpu.x[11] = DATA_PAGE + 330;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x7840296a)), &cpu);
    assert(cpu.x[10] == UINT16_C(0x5678));

    memory->data[340] = 0x01;
    memory->data[341] = 0x80;
    cpu.x[13] = DATA_PAGE + 343;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x789fd9ac)), &cpu);
    assert(cpu.x[12] == UINT64_C(0xffffffffffff8001));

    memory->data[350] = 0x02;
    memory->data[351] = 0x80;
    cpu.x[15] = DATA_PAGE + 347;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x78c039ee)), &cpu);
    assert(cpu.x[14] == UINT32_C(0xffff8002));

    cpu.x[16] = UINT64_C(0xffffffff89abcdef);
    cpu.x[17] = DATA_PAGE + 364;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb81fca30)), &cpu);
    const byte_t expected_word[] = {0xef, 0xcd, 0xab, 0x89};
    assert(memcmp(&memory->data[360], expected_word,
            sizeof(expected_word)) == 0);

    memory->data[374] = 0x78;
    memory->data[375] = 0x56;
    memory->data[376] = 0x34;
    memory->data[377] = 0x12;
    cpu.x[19] = DATA_PAGE + 370;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb8404a72)), &cpu);
    assert(cpu.x[18] == UINT32_C(0x12345678));

    memory->data[380] = 0x03;
    memory->data[381] = 0x00;
    memory->data[382] = 0x00;
    memory->data[383] = 0x80;
    cpu.x[21] = DATA_PAGE + 385;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb89fbab4)), &cpu);
    assert(cpu.x[20] == UINT64_C(0xffffffff80000003));

    cpu.x[22] = UINT64_C(0x8877665544332211);
    cpu.x[23] = DATA_PAGE + 395;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf8005af6)), &cpu);
    const byte_t expected_qword[] = {0x11, 0x22, 0x33, 0x44,
            0x55, 0x66, 0x77, 0x88};
    assert(memcmp(&memory->data[400], expected_qword,
            sizeof(expected_qword)) == 0);

    memcpy(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4], expected_qword, 4);
    memcpy(memory->next, expected_qword + 4, 4);
    cpu.x[25] = DATA_NEXT + 4;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf85f8b38)), &cpu);
    assert(cpu.x[24] == UINT64_C(0x8877665544332211));

    put_qword(&memory->data[420], UINT64_C(0x1020304050607080));
    cpu.x[1] = DATA_PAGE + 420;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf8400821)), &cpu);
    assert(cpu.x[1] == UINT64_C(0x1020304050607080));

    memset(&memory->data[432], 0xff, 8);
    cpu.sp = DATA_PAGE + 432;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf8000bff)), &cpu);
    const byte_t zeros[8] = {0};
    assert(memcmp(&memory->data[432], zeros, sizeof(zeros)) == 0);
    assert(cpu.sp == DATA_PAGE + 432 && cpu.nzcv == UINT32_C(0xa0000000));
}

static void test_unprivileged_faults(struct test_memory *memory) {
    struct cpu_state cpu = {
        .cycle = 23,
        .x[24] = UINT64_C(0x1122334455667788),
        .x[25] = UNMAPPED_PAGE + 8,
    };
    struct aarch64_step_result result = run_instruction(
            memory, &cpu, UINT32_C(0xf85f8b38));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[24] == UINT64_C(0x1122334455667788));
    assert(cpu.x[25] == UNMAPPED_PAGE + 8);
    assert(cpu.pc == CODE_PAGE && cpu.cycle == 23);

    const byte_t first_before[] = {0xaa, 0xbb, 0xcc, 0xdd};
    const byte_t second_before[] = {0x11, 0x22, 0x33, 0x44};
    memcpy(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4], first_before, 4);
    memcpy(memory->next, second_before, 4);
    cpu.x[22] = UINT64_C(0x8877665544332211);
    cpu.x[23] = DATA_NEXT - 4;
    result = run_instruction(memory, &cpu, UINT32_C(0xf8000af6));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4],
            first_before, 4) == 0);
    assert(memcmp(memory->next, second_before, 4) == 0);
    assert(cpu.x[23] == DATA_NEXT - 4);
    assert(cpu.pc == CODE_PAGE && cpu.cycle == 23);
}

static void test_pairs(struct test_memory *memory) {
    struct cpu_state cpu = {
        .x[29] = UINT64_C(0x0123456789abcdef),
        .x[30] = UINT64_C(0xfedcba9876543210),
        .sp = DATA_PAGE + 160,
    };
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xa9bf7bfd)), &cpu);
    assert(cpu.sp == DATA_PAGE + 144);
    byte_t expected[16];
    put_qword(expected, UINT64_C(0x0123456789abcdef));
    put_qword(expected + 8, UINT64_C(0xfedcba9876543210));
    assert(memcmp(&memory->data[144], expected, sizeof(expected)) == 0);

    cpu.x[29] = 0;
    cpu.x[30] = 0;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xa8c17bfd)), &cpu);
    assert(cpu.x[29] == UINT64_C(0x0123456789abcdef));
    assert(cpu.x[30] == UINT64_C(0xfedcba9876543210));
    assert(cpu.sp == DATA_PAGE + 160);

    cpu.x[0] = UINT64_C(0xffffffff89abcdef);
    cpu.x[1] = UINT64_C(0xffffffff01234567);
    cpu.x[2] = DATA_PAGE + 184;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x29010440)), &cpu);
    const byte_t expected_words[] = {0xef, 0xcd, 0xab, 0x89,
            0x67, 0x45, 0x23, 0x01};
    assert(memcmp(&memory->data[192], expected_words,
            sizeof(expected_words)) == 0);
    assert(cpu.x[2] == DATA_PAGE + 184);

    cpu.x[6] = UINT64_MAX;
    cpu.x[7] = UINT64_MAX;
    cpu.x[8] = DATA_PAGE + 184;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x29411d06)), &cpu);
    assert(cpu.x[6] == UINT32_C(0x89abcdef));
    assert(cpu.x[7] == UINT32_C(0x01234567));

    memset(&memory->data[208], 0xff, 16);
    cpu.x[9] = DATA_PAGE + 208;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xa9007d3f)), &cpu);
    const byte_t zeros[16] = {0};
    assert(memcmp(&memory->data[208], zeros, sizeof(zeros)) == 0);

    put_qword(&memory->data[224], UINT64_C(0x1122334455667788));
    put_qword(&memory->data[232], UINT64_C(0x99aabbccddeeff00));
    cpu.x[5] = DATA_PAGE + 200;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xa94190a3)), &cpu);
    assert(cpu.x[3] == UINT64_C(0x1122334455667788));
    assert(cpu.x[4] == UINT64_C(0x99aabbccddeeff00));
    assert(cpu.x[5] == DATA_PAGE + 200);
}

static void test_signed_loads(struct test_memory *memory) {
    struct cpu_state cpu = {0};

    memory->data[287] = 0x80;
    cpu.x[1] = DATA_PAGE + 288;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x389ff020)), &cpu);
    assert(cpu.x[0] == UINT64_C(0xffffffffffffff80));
    assert(cpu.x[1] == DATA_PAGE + 288);

    memory->data[293] = 0x81;
    cpu.x[11] = DATA_PAGE + 288;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x39c0156a)), &cpu);
    assert(cpu.x[10] == UINT32_C(0xffffff81));

    memory->data[308] = 0x01;
    memory->data[309] = 0x80;
    cpu.x[12] = DATA_PAGE + 304;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x79c0098b)), &cpu);
    assert(cpu.x[11] == UINT32_C(0xffff8001));

    memory->data[310] = 0x02;
    memory->data[311] = 0x80;
    cpu.x[13] = DATA_PAGE + 304;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x79800dac)), &cpu);
    assert(cpu.x[12] == UINT64_C(0xffffffffffff8002));

    memory->data[320] = 0x03;
    memory->data[321] = 0x00;
    memory->data[322] = 0x00;
    memory->data[323] = 0x80;
    cpu.x[14] = DATA_PAGE + 312;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb98009cd)), &cpu);
    assert(cpu.x[13] == UINT64_C(0xffffffff80000003));

    memory->data[336] = 0x04;
    memory->data[337] = 0x00;
    memory->data[338] = 0x00;
    memory->data[339] = 0x80;
    cpu.x[5] = DATA_PAGE + 336;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb88044a4)), &cpu);
    assert(cpu.x[4] == UINT64_C(0xffffffff80000004));
    assert(cpu.x[5] == DATA_PAGE + 340);

    memory->data[350] = 0xff;
    cpu.x[7] = DATA_PAGE + 352;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38dfece6)), &cpu);
    assert(cpu.x[6] == UINT32_MAX);
    assert(cpu.x[7] == DATA_PAGE + 350);

    memory->data[363] = 0x7f;
    cpu.x[10] = DATA_PAGE + 360;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x39800d49)), &cpu);
    assert(cpu.x[9] == 0x7f);

    memory->data[374] = 0xff;
    memory->data[375] = 0x7f;
    cpu.x[13] = DATA_PAGE + 368;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x79800dac)), &cpu);
    assert(cpu.x[12] == UINT16_C(0x7fff));

    memory->data[392] = 0xff;
    memory->data[393] = 0xff;
    memory->data[394] = 0xff;
    memory->data[395] = 0x7f;
    cpu.x[14] = DATA_PAGE + 384;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb98009cd)), &cpu);
    assert(cpu.x[13] == UINT32_C(0x7fffffff));

    memory->data[400] = 0x80;
    cpu.x[0] = UINT64_C(0x0123456789abcdef);
    cpu.sp = DATA_PAGE + 401;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x389fffff)), &cpu);
    assert(cpu.x[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu.sp == DATA_PAGE + 400);
}

static void assert_register_offset_decode(dword_t word,
        enum aarch64_opcode opcode, byte_t size, byte_t width,
        byte_t rt, byte_t rn, byte_t rm,
        enum aarch64_extend_type extend_type, byte_t shift,
        bool signed_load) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.load_store.size == size);
    assert(instruction.operands.load_store.rt == rt);
    assert(instruction.operands.load_store.rn == rn);
    assert(instruction.operands.load_store.rm == rm);
    assert(instruction.operands.load_store.extend_type == extend_type);
    assert(instruction.operands.load_store.shift == shift);
    assert(instruction.operands.load_store.address_mode ==
            AARCH64_ADDRESS_OFFSET);
    assert(instruction.operands.load_store.signed_load == signed_load);
}

static bool valid_scalar_transfer(unsigned size_shift,
        unsigned operation) {
    return (size_shift < 2 && operation < 4) ||
            (size_shift == 2 && operation < 3) ||
            (size_shift == 3 && operation < 2);
}

static void test_register_offset_decode(void) {
    assert_register_offset_decode(UINT32_C(0x38226820),
            AARCH64_OP_STORE_REGISTER_OFFSET, 1, 32,
            0, 1, 2, AARCH64_EXTEND_UXTX, 0, false);
    assert_register_offset_decode(UINT32_C(0x38644be3),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 1, 32,
            3, 31, 4, AARCH64_EXTEND_UXTW, 0, false);
    assert_register_offset_decode(UINT32_C(0x7827d8c5),
            AARCH64_OP_STORE_REGISTER_OFFSET, 2, 32,
            5, 6, 7, AARCH64_EXTEND_SXTW, 1, false);
    assert_register_offset_decode(UINT32_C(0x786a7928),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 2, 32,
            8, 9, 10, AARCH64_EXTEND_UXTX, 1, false);
    assert_register_offset_decode(UINT32_C(0xb82d598b),
            AARCH64_OP_STORE_REGISTER_OFFSET, 4, 32,
            11, 12, 13, AARCH64_EXTEND_UXTW, 2, false);
    assert_register_offset_decode(UINT32_C(0xb870f9ee),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 4, 32,
            14, 15, 16, AARCH64_EXTEND_SXTX, 2, false);
    assert_register_offset_decode(UINT32_C(0xf8337a51),
            AARCH64_OP_STORE_REGISTER_OFFSET, 8, 64,
            17, 18, 19, AARCH64_EXTEND_UXTX, 3, false);
    assert_register_offset_decode(UINT32_C(0xf876dab4),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 8, 64,
            20, 21, 22, AARCH64_EXTEND_SXTW, 3, false);
    assert_register_offset_decode(UINT32_C(0x38f94b17),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 1, 32,
            23, 24, 25, AARCH64_EXTEND_UXTW, 0, true);
    assert_register_offset_decode(UINT32_C(0x38bceb7a),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 1, 64,
            26, 27, 28, AARCH64_EXTEND_SXTX, 0, true);
    assert_register_offset_decode(UINT32_C(0x78e27820),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 2, 32,
            0, 1, 2, AARCH64_EXTEND_UXTX, 1, true);
    assert_register_offset_decode(UINT32_C(0x78a5d883),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 2, 64,
            3, 4, 5, AARCH64_EXTEND_SXTW, 1, true);
    assert_register_offset_decode(UINT32_C(0xb8a858e6),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 4, 64,
            6, 7, 8, AARCH64_EXTEND_UXTW, 2, true);
    assert_register_offset_decode(UINT32_C(0xb8aafbe9),
            AARCH64_OP_LOAD_REGISTER_OFFSET, 4, 64,
            9, 31, 10, AARCH64_EXTEND_SXTX, 2, true);

    unsigned valid = 0;
    for (unsigned size_shift = 0; size_shift < 4; size_shift++) {
        for (unsigned operation = 0; operation < 4; operation++) {
            for (unsigned option = 0; option < 8; option++) {
                for (unsigned scaled = 0; scaled < 2; scaled++) {
                    dword_t word = UINT32_C(0x38200820) |
                            (dword_t) size_shift << 30 |
                            (dword_t) operation << 22 |
                            UINT32_C(2) << 16 |
                            (dword_t) option << 13 |
                            (dword_t) scaled << 12;
                    bool expected = (option & 2) != 0 &&
                            valid_scalar_transfer(size_shift, operation);
                    struct aarch64_decoded instruction;
                    bool decoded = aarch64_decode(word, &instruction);
                    assert(decoded == expected);
                    if (!decoded)
                        continue;
                    valid++;
                    byte_t size = (byte_t) (1U << size_shift);
                    byte_t width = operation == 2 || size == 8 ? 64 : 32;
                    assert(instruction.width == width);
                    assert(instruction.operands.load_store.size == size);
                    assert(instruction.operands.load_store.extend_type ==
                            (enum aarch64_extend_type) option);
                    assert(instruction.operands.load_store.shift ==
                            (scaled ? size_shift : 0));
                }
            }
        }
    }
    assert(valid == 104);

    static const dword_t unsupported[] = {
        UINT32_C(0x38222820), UINT32_C(0xf8a26820),
        UINT32_C(0xb8e858e6), UINT32_C(0xf8e26820),
        UINT32_C(0x3ce26820),
    };
    for (size_t i = 0; i < array_size(unsupported); i++) {
        struct aarch64_decoded instruction;
        assert(!aarch64_decode(unsupported[i], &instruction));
    }
}

static void test_register_offsets(struct test_memory *memory) {
    struct cpu_state cpu = {
        .nzcv = UINT32_C(0xa0000000),
        .sp = DATA_PAGE + 510,
    };

    cpu.x[0] = 0xab;
    cpu.x[1] = DATA_PAGE + 500;
    cpu.x[2] = 3;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38226820)), &cpu);
    assert(memory->data[503] == 0xab);

    memory->data[512] = 0xcd;
    cpu.x[3] = UINT64_MAX;
    cpu.x[4] = UINT64_C(0xffffffff00000002);
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38644be3)), &cpu);
    assert(cpu.x[3] == 0xcd);

    cpu.x[5] = UINT64_C(0x1234abcd);
    cpu.x[6] = DATA_PAGE + 530;
    cpu.x[7] = UINT32_C(0xfffffffe);
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x7827d8c5)), &cpu);
    assert(memory->data[526] == 0xcd && memory->data[527] == 0xab);

    memory->data[546] = 0x34;
    memory->data[547] = 0x12;
    cpu.x[9] = DATA_PAGE + 540;
    cpu.x[10] = 3;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x786a7928)), &cpu);
    assert(cpu.x[8] == UINT16_C(0x1234));

    cpu.x[11] = UINT64_C(0xffffffff89abcdef);
    cpu.x[12] = DATA_PAGE + 560;
    cpu.x[13] = 2;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb82d598b)), &cpu);
    const byte_t expected_word[] = {0xef, 0xcd, 0xab, 0x89};
    assert(memcmp(&memory->data[568], expected_word,
            sizeof(expected_word)) == 0);

    memory->data[576] = 0x78;
    memory->data[577] = 0x56;
    memory->data[578] = 0x34;
    memory->data[579] = 0x12;
    cpu.x[15] = DATA_PAGE + 580;
    cpu.x[16] = UINT64_MAX;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb870f9ee)), &cpu);
    assert(cpu.x[14] == UINT32_C(0x12345678));

    cpu.x[17] = UINT64_C(0x8877665544332211);
    cpu.x[18] = DATA_PAGE + 600;
    cpu.x[19] = 2;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf8337a51)), &cpu);
    const byte_t expected_qword[] = {0x11, 0x22, 0x33, 0x44,
            0x55, 0x66, 0x77, 0x88};
    assert(memcmp(&memory->data[616], expected_qword,
            sizeof(expected_qword)) == 0);

    put_qword(&memory->data[632], UINT64_C(0x1020304050607080));
    cpu.x[21] = DATA_PAGE + 640;
    cpu.x[22] = UINT32_MAX;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf876dab4)), &cpu);
    assert(cpu.x[20] == UINT64_C(0x1020304050607080));

    memory->data[650] = 0x80;
    cpu.x[24] = DATA_PAGE + 648;
    cpu.x[25] = 2;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38f94b17)), &cpu);
    assert(cpu.x[23] == UINT32_C(0xffffff80));

    memory->data[659] = 0x81;
    cpu.x[27] = DATA_PAGE + 660;
    cpu.x[28] = UINT64_MAX;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x38bceb7a)), &cpu);
    assert(cpu.x[26] == UINT64_C(0xffffffffffffff81));

    memory->data[670] = 0x01;
    memory->data[671] = 0x80;
    cpu.x[1] = DATA_PAGE + 666;
    cpu.x[2] = 2;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x78e27820)), &cpu);
    assert(cpu.x[0] == UINT32_C(0xffff8001));

    memory->data[678] = 0x02;
    memory->data[679] = 0x80;
    cpu.x[4] = DATA_PAGE + 680;
    cpu.x[5] = UINT32_MAX;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0x78a5d883)), &cpu);
    assert(cpu.x[3] == UINT64_C(0xffffffffffff8002));

    memory->data[692] = 0x03;
    memory->data[693] = 0x00;
    memory->data[694] = 0x00;
    memory->data[695] = 0x80;
    cpu.x[7] = DATA_PAGE + 684;
    cpu.x[8] = 2;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb8a858e6)), &cpu);
    assert(cpu.x[6] == UINT64_C(0xffffffff80000003));

    memory->data[704] = 0x04;
    memory->data[705] = 0x00;
    memory->data[706] = 0x00;
    memory->data[707] = 0x80;
    cpu.sp = DATA_PAGE + 708;
    cpu.x[10] = UINT64_MAX;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xb8aafbe9)), &cpu);
    assert(cpu.x[9] == UINT64_C(0xffffffff80000004));

    put_qword(&memory->data[720], UINT64_C(0xa1a2a3a4a5a6a7a8));
    cpu.x[1] = DATA_PAGE + 712;
    cpu.x[2] = 8;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf8626821)), &cpu);
    assert(cpu.x[1] == UINT64_C(0xa1a2a3a4a5a6a7a8));

    put_qword(&memory->data[736], UINT64_C(0xb1b2b3b4b5b6b7b8));
    cpu.x[1] = DATA_PAGE + 728;
    cpu.x[2] = 8;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf8626822)), &cpu);
    assert(cpu.x[2] == UINT64_C(0xb1b2b3b4b5b6b7b8));

    qword_t old_x0 = cpu.x[0];
    cpu.sp = DATA_PAGE + 720;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf87f6bff)), &cpu);
    assert(cpu.x[0] == old_x0);

    memset(&memory->data[752], 0xff, 8);
    cpu.x[1] = DATA_PAGE + 752;
    assert_retired(run_instruction(memory, &cpu, UINT32_C(0xf83f683f)), &cpu);
    const byte_t zeros[8] = {0};
    assert(memcmp(&memory->data[752], zeros, sizeof(zeros)) == 0);
    assert(cpu.nzcv == UINT32_C(0xa0000000));
}

static void test_register_offset_faults(struct test_memory *memory) {
    struct cpu_state cpu = {
        .cycle = 11,
        .x[0] = UINT64_C(0x1122334455667788),
        .x[1] = UNMAPPED_PAGE,
        .x[2] = 0,
        .sp = UINT64_C(0x123456789abcdef0),
        .nzcv = UINT32_C(0x90000000),
    };
    struct aarch64_step_result result = run_instruction(
            memory, &cpu, UINT32_C(0xf8627820));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[0] == UINT64_C(0x1122334455667788));
    assert(cpu.x[1] == UNMAPPED_PAGE && cpu.x[2] == 0);
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x90000000));
    assert(cpu.pc == CODE_PAGE && cpu.cycle == 11);

    const byte_t first_before[] = {0xaa, 0xbb, 0xcc, 0xdd};
    const byte_t second_before[] = {0x11, 0x22, 0x33, 0x44};
    memcpy(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4],
            first_before, sizeof(first_before));
    memcpy(memory->next, second_before, sizeof(second_before));
    cpu.x[0] = UINT64_C(0x8877665544332211);
    cpu.x[1] = DATA_NEXT - 4;
    cpu.x[2] = 0;
    result = run_instruction(memory, &cpu, UINT32_C(0xf8227820));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4],
            first_before, sizeof(first_before)) == 0);
    assert(memcmp(memory->next, second_before, sizeof(second_before)) == 0);
    assert(cpu.x[0] == UINT64_C(0x8877665544332211));
    assert(cpu.x[1] == DATA_NEXT - 4 && cpu.x[2] == 0);
    assert(cpu.sp == UINT64_C(0x123456789abcdef0));
    assert(cpu.nzcv == UINT32_C(0x90000000));
    assert(cpu.pc == CODE_PAGE && cpu.cycle == 11);
}

static void test_faults(struct test_memory *memory) {
    struct cpu_state cpu = {
        .cycle = 7,
        .x[0] = UINT64_C(0x1122334455667788),
        .x[1] = UNMAPPED_PAGE,
    };
    struct aarch64_step_result result = run_instruction(
            memory, &cpu, UINT32_C(0xf9400020));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(cpu.x[0] == UINT64_C(0x1122334455667788));
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    cpu.x[4] = UINT64_C(0xabcdef);
    cpu.x[5] = UNMAPPED_PAGE;
    result = run_instruction(memory, &cpu, UINT32_C(0xf84084a4));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(cpu.x[4] == UINT64_C(0xabcdef));
    assert(cpu.x[5] == UNMAPPED_PAGE);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    cpu.x[6] = UINT64_C(0x12345678);
    cpu.x[7] = UNMAPPED_PAGE + 2;
    result = run_instruction(memory, &cpu, UINT32_C(0x38dfece6));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(cpu.x[6] == UINT64_C(0x12345678));
    assert(cpu.x[7] == UNMAPPED_PAGE + 2);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    cpu.x[4] = UINT64_C(0x87654321);
    cpu.x[5] = UNMAPPED_PAGE - 2;
    result = run_instruction(memory, &cpu, UINT32_C(0xb88044a4));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[4] == UINT64_C(0x87654321));
    assert(cpu.x[5] == UNMAPPED_PAGE - 2);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    const byte_t pre_store_before[] = {0x31, 0x41, 0x59, 0x26};
    memcpy(memory->next, pre_store_before, sizeof(pre_store_before));
    cpu.x[6] = UINT64_C(0x0123456789abcdef);
    cpu.x[7] = DATA_NEXT + 4;
    result = run_instruction(memory, &cpu, UINT32_C(0xb81fcce6));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(memcmp(memory->next, pre_store_before,
            sizeof(pre_store_before)) == 0);
    assert(cpu.x[7] == DATA_NEXT + 4);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    cpu.x[10] = UNMAPPED_PAGE;
    result = run_instruction(memory, &cpu, UINT32_C(0xf940015f));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);

    const byte_t first_before[] = {0xaa, 0xbb, 0xcc, 0xdd};
    const byte_t next_before[] = {0x11, 0x22, 0x33, 0x44};
    memcpy(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4],
            first_before, sizeof(first_before));
    memcpy(memory->next, next_before, sizeof(next_before));
    cpu.x[0] = UINT64_C(0x8877665544332211);
    cpu.x[1] = DATA_PAGE + GUEST_MEMORY_PAGE_SIZE - 4;
    result = run_instruction(memory, &cpu, UINT32_C(0xf9000020));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(memcmp(&memory->data[GUEST_MEMORY_PAGE_SIZE - 4],
            first_before, sizeof(first_before)) == 0);
    assert(memcmp(memory->next, next_before, sizeof(next_before)) == 0);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    const byte_t pair_first_before[] = {0x10, 0x20, 0x30, 0x40,
            0x50, 0x60, 0x70, 0x80};
    const byte_t pair_second_before[] = {0x91, 0x82, 0x73, 0x64,
            0x55, 0x46, 0x37, 0x28};
    memcpy(&memory->data[GUEST_MEMORY_PAGE_SIZE - 8], pair_first_before,
            sizeof(pair_first_before));
    memcpy(memory->next, pair_second_before, sizeof(pair_second_before));
    cpu.x[0] = UINT64_C(0x0123456789abcdef);
    cpu.x[1] = UINT64_C(0xfedcba9876543210);
    cpu.x[2] = DATA_NEXT + 8;
    result = run_instruction(memory, &cpu, UINT32_C(0xa9bf0440));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == DATA_NEXT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(memcmp(&memory->data[GUEST_MEMORY_PAGE_SIZE - 8],
            pair_first_before, sizeof(pair_first_before)) == 0);
    assert(memcmp(memory->next, pair_second_before,
            sizeof(pair_second_before)) == 0);
    assert(cpu.x[0] == UINT64_C(0x0123456789abcdef));
    assert(cpu.x[1] == UINT64_C(0xfedcba9876543210));
    assert(cpu.x[2] == DATA_NEXT + 8);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);

    put_qword(&memory->next[GUEST_MEMORY_PAGE_SIZE - 8],
            UINT64_C(0x8877665544332211));
    cpu.x[3] = UINT64_C(0x1111111111111111);
    cpu.x[4] = UINT64_C(0x2222222222222222);
    cpu.x[5] = UNMAPPED_PAGE - 8;
    result = run_instruction(memory, &cpu, UINT32_C(0xa8c110a3));
    assert(result.stop == AARCH64_STEP_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[3] == UINT64_C(0x1111111111111111));
    assert(cpu.x[4] == UINT64_C(0x2222222222222222));
    assert(cpu.x[5] == UNMAPPED_PAGE - 8);
    assert(cpu.pc == CODE_PAGE);
    assert(cpu.cycle == 7);
}

int main(void) {
    struct test_memory memory;
    init_test_memory(&memory);
    test_loads(&memory);
    test_stores(&memory);
    test_unscaled_and_writeback(&memory);
    test_unprivileged_decode();
    test_unprivileged_transfers(&memory);
    test_unprivileged_faults(&memory);
    test_pairs(&memory);
    test_signed_loads(&memory);
    test_register_offset_decode();
    test_register_offsets(&memory);
    test_register_offset_faults(&memory);
    test_faults(&memory);
    return 0;
}

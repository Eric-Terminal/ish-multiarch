#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/aarch64/threaded.h"

#define DATA_PAGE UINT64_C(0x000056789abcd000)
#define READONLY_PAGE (DATA_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define UNMAPPED_PAGE (READONLY_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define ADDRESS_LIMIT (UINT64_C(1) << 48)

struct casp_vector {
    dword_t word;
    enum aarch64_opcode opcode;
    byte_t width;
    byte_t rs;
    byte_t rt;
    byte_t rn;
};

struct test_page {
    guest_addr_t address;
    byte_t *host_page;
    unsigned permissions;
};

struct test_memory {
    byte_t data[GUEST_MEMORY_PAGE_SIZE];
    byte_t readonly[GUEST_MEMORY_PAGE_SIZE];
    struct test_page pages[2];
    pthread_rwlock_t lock;
    struct guest_address_space space;
    struct guest_tlb tlb;
};

struct pair_value {
    qword_t low;
    qword_t high;
};

static bool read_lock(void *opaque) {
    struct test_memory *memory = opaque;
    assert(pthread_rwlock_rdlock(&memory->lock) == 0);
    return true;
}

static void read_unlock(void *opaque, bool locked) {
    struct test_memory *memory = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&memory->lock) == 0);
}

static bool write_lock(void *opaque) {
    struct test_memory *memory = opaque;
    assert(pthread_rwlock_wrlock(&memory->lock) == 0);
    return true;
}

static void write_unlock(void *opaque, bool locked) {
    struct test_memory *memory = opaque;
    assert(locked);
    assert(pthread_rwlock_unlock(&memory->lock) == 0);
}

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
    .read_lock = read_lock,
    .read_unlock = read_unlock,
    .write_lock = write_lock,
    .write_unlock = write_unlock,
    .resolve_page = resolve_test_page,
};

static void init_test_memory(struct test_memory *memory) {
    *memory = (struct test_memory) {0};
    assert(pthread_rwlock_init(&memory->lock, NULL) == 0);
    memory->pages[0] = (struct test_page) {
        .address = DATA_PAGE,
        .host_page = memory->data,
        .permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
    };
    memory->pages[1] = (struct test_page) {
        .address = READONLY_PAGE,
        .host_page = memory->readonly,
        .permissions = GUEST_MEMORY_READ,
    };
    guest_address_space_init(&memory->space, &test_ops, memory, 48);
    guest_tlb_init(&memory->tlb, &memory->space);
}

static void destroy_test_memory(struct test_memory *memory) {
    assert(pthread_rwlock_destroy(&memory->lock) == 0);
}

static dword_t encode(bool wide, bool acquire, bool release,
        byte_t rs, byte_t rt, byte_t rn) {
    return UINT32_C(0x08207c00) |
            (dword_t) wide << 30 |
            (dword_t) acquire << 22 |
            (dword_t) rs << 16 |
            (dword_t) release << 15 |
            (dword_t) rn << 5 |
            rt;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static struct aarch64_execute_result execute_word(
        struct test_memory *memory, struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
    return aarch64_execute(cpu, &memory->tlb, &instruction);
}

static qword_t element_mask(byte_t element_size) {
    return element_size == 4 ? UINT32_MAX : UINT64_MAX;
}

static void put_value(byte_t *destination, qword_t value, byte_t size) {
    for (byte_t i = 0; i < size; i++)
        destination[i] = (byte_t) (value >> (i * 8));
}

static qword_t get_value(const byte_t *source, byte_t size) {
    qword_t value = 0;
    for (byte_t i = 0; i < size; i++)
        value |= (qword_t) source[i] << (i * 8);
    return value;
}

static void put_pair(byte_t *destination, byte_t element_size,
        qword_t low, qword_t high) {
    put_value(destination, low, element_size);
    put_value(destination + element_size, high, element_size);
}

static struct pair_value get_pair(
        const byte_t *source, byte_t element_size) {
    return (struct pair_value) {
        .low = get_value(source, element_size),
        .high = get_value(source + element_size, element_size),
    };
}

static void assert_pair(struct pair_value actual,
        qword_t low, qword_t high) {
    assert(actual.low == low);
    assert(actual.high == high);
}

static void set_monitor_sentinel(struct cpu_state *cpu,
        const struct guest_address_space *space) {
    aarch64_set_exclusive(cpu, DATA_PAGE + 0x700, 8, true,
            UINT64_C(0x1122334455667788),
            UINT64_C(0x8877665544332211), space, 7, 11, 13);
}

static void assert_monitor_equal(
        const struct aarch64_exclusive_monitor *actual,
        const struct aarch64_exclusive_monitor *expected) {
    assert(actual->address == expected->address);
    assert(actual->value_low == expected->value_low);
    assert(actual->value_high == expected->value_high);
    assert(actual->address_space == expected->address_space);
    assert(actual->mapping_epoch == expected->mapping_epoch);
    assert(actual->write_epoch == expected->write_epoch);
    assert(actual->sync_identity == expected->sync_identity);
    assert(actual->size == expected->size);
    assert(actual->pair == expected->pair);
    assert(actual->valid == expected->valid);
}

static void assert_retired(struct aarch64_execute_result result) {
    assert(result.stop == AARCH64_EXECUTE_RETIRED);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_NONE);
}

static void assert_vector(const struct casp_vector *vector) {
    struct aarch64_decoded instruction = decode(vector->word);
    assert(instruction.opcode == vector->opcode);
    assert(instruction.width == vector->width);
    assert(instruction.operands.compare_swap_pair.rs == vector->rs);
    assert(instruction.operands.compare_swap_pair.rs2 == vector->rs + 1);
    assert(instruction.operands.compare_swap_pair.rt == vector->rt);
    assert(instruction.operands.compare_swap_pair.rt2 == vector->rt + 1);
    assert(instruction.operands.compare_swap_pair.rn == vector->rn);
    assert(instruction.operands.compare_swap_pair.size ==
            vector->width / 8);
}

static void test_clang_vectors(void) {
    // 机器码由 Apple clang 21 的 armv8.1-a+lse 汇编器生成。
    static const struct casp_vector vectors[] = {
        {UINT32_C(0x08207c82), AARCH64_OP_CASP, 32, 0, 2, 4},
        {UINT32_C(0x08667d48), AARCH64_OP_CASPA, 32, 6, 8, 10},
        {UINT32_C(0x082cffee), AARCH64_OP_CASPL, 32, 12, 14, 31},
        {UINT32_C(0x087effbe), AARCH64_OP_CASPAL, 32, 30, 30, 29},
        {UINT32_C(0x48227fe4), AARCH64_OP_CASP, 64, 2, 4, 31},
        {UINT32_C(0x48687d8a), AARCH64_OP_CASPA, 64, 8, 10, 12},
        {UINT32_C(0x482efe50), AARCH64_OP_CASPL, 64, 14, 16, 18},
        {UINT32_C(0x487efffe), AARCH64_OP_CASPAL, 64, 30, 30, 31},
    };

    for (size_t i = 0; i < array_size(vectors); i++)
        assert_vector(&vectors[i]);
}

static void test_ordering_and_width_fields(void) {
    static const enum aarch64_opcode opcodes[2][2] = {
        {AARCH64_OP_CASP, AARCH64_OP_CASPL},
        {AARCH64_OP_CASPA, AARCH64_OP_CASPAL},
    };

    for (unsigned wide = 0; wide < 2; wide++) {
        for (unsigned acquire = 0; acquire < 2; acquire++) {
            for (unsigned release = 0; release < 2; release++) {
                struct casp_vector vector = {
                    .word = encode(wide != 0, acquire != 0,
                            release != 0, 28, 30, 31),
                    .opcode = opcodes[acquire][release],
                    .width = wide ? 64 : 32,
                    .rs = 28,
                    .rt = 30,
                    .rn = 31,
                };
                assert_vector(&vector);
            }
        }
    }
}

static void test_odd_pair_heads_are_undefined(void) {
    for (unsigned wide = 0; wide < 2; wide++) {
        for (unsigned acquire = 0; acquire < 2; acquire++) {
            for (unsigned release = 0; release < 2; release++) {
                struct aarch64_decoded instruction;
                assert(!aarch64_decode(encode(wide != 0, acquire != 0,
                        release != 0, 1, 2, 3), &instruction));
                assert(!aarch64_decode(encode(wide != 0, acquire != 0,
                        release != 0, 2, 3, 3), &instruction));
            }
        }
    }
}

static void test_legal_register_overlap(void) {
    struct casp_vector vector = {
        .word = encode(true, true, true, 2, 2, 2),
        .opcode = AARCH64_OP_CASPAL,
        .width = 64,
        .rs = 2,
        .rt = 2,
        .rn = 2,
    };
    assert_vector(&vector);
}

static void test_all_orders_success_and_mismatch(
        struct test_memory *memory) {
    const size_t offset = 0x100;
    for (unsigned wide = 0; wide < 2; wide++) {
        byte_t element_size = wide ? 8 : 4;
        qword_t mask = element_mask(element_size);
        qword_t original_low = UINT64_C(0x8877665544332211) & mask;
        qword_t original_high = UINT64_C(0x1020304050607080) & mask;
        qword_t replacement_low = UINT64_C(0xfedcba9876543210) & mask;
        qword_t replacement_high = UINT64_C(0x0123456789abcdef) & mask;
        for (unsigned acquire = 0; acquire < 2; acquire++) {
            for (unsigned release = 0; release < 2; release++) {
                for (unsigned success = 0; success < 2; success++) {
                    put_pair(memory->data + offset, element_size,
                            original_low, original_high);
                    struct cpu_state cpu = {
                        .pc = UINT64_C(0x1000),
                        .nzcv = UINT32_C(0xa0000000),
                    };
                    cpu.x[0] = original_low |
                            (wide ? 0 : UINT64_C(0xa5a5a5a500000000));
                    cpu.x[1] = original_high |
                            (wide ? 0 : UINT64_C(0x5a5a5a5a00000000));
                    if (!success)
                        cpu.x[1] ^= 1;
                    cpu.x[2] = replacement_low;
                    cpu.x[3] = replacement_high;
                    cpu.x[4] = DATA_PAGE + offset;
                    set_monitor_sentinel(&cpu, &memory->space);
                    struct aarch64_exclusive_monitor monitor = cpu.exclusive;

                    assert_retired(execute_word(memory, &cpu,
                            encode(wide != 0, acquire != 0,
                                    release != 0, 0, 2, 4)));
                    assert(cpu.pc == UINT64_C(0x1004));
                    assert(cpu.nzcv == UINT32_C(0xa0000000));
                    assert(cpu.x[0] == original_low);
                    assert(cpu.x[1] == original_high);
                    assert_monitor_equal(&cpu.exclusive, &monitor);
                    struct pair_value memory_pair = get_pair(
                            memory->data + offset, element_size);
                    if (success) {
                        assert_pair(memory_pair,
                                replacement_low, replacement_high);
                    } else {
                        assert_pair(memory_pair,
                                original_low, original_high);
                    }
                }
            }
        }
    }
}

static void test_zero_register_and_zero_extension(
        struct test_memory *memory) {
    const size_t offset = 0x180;
    put_pair(memory->data + offset, 4,
            UINT32_C(0x89abcdef), UINT32_C(0x10203040));
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .nzcv = UINT32_C(0x50000000),
    };
    cpu.x[30] = UINT64_C(0xffffeeee89abcdef);
    cpu.x[2] = UINT32_C(0xaabbccdd);
    cpu.x[3] = UINT32_C(0xeeff0011);
    cpu.x[4] = DATA_PAGE + offset;
    assert_retired(execute_word(memory, &cpu,
            encode(false, false, false, 30, 2, 4)));
    // Rs2 编码为 31，比较使用零且写回丢弃；W30 写回必须清高位。
    assert(cpu.x[30] == UINT32_C(0x89abcdef));
    assert_pair(get_pair(memory->data + offset, 4),
            UINT32_C(0x89abcdef), UINT32_C(0x10203040));

    put_pair(memory->data + offset, 4,
            UINT32_C(0x11112222), UINT32_C(0x33334444));
    cpu.pc = UINT64_C(0x1810);
    cpu.x[0] = UINT32_C(0x11112222);
    cpu.x[1] = UINT32_C(0x33334444);
    cpu.x[30] = UINT64_C(0xdeadbeef55667788);
    assert_retired(execute_word(memory, &cpu,
            encode(false, true, true, 0, 30, 4)));
    assert(cpu.x[0] == UINT32_C(0x11112222));
    assert(cpu.x[1] == UINT32_C(0x33334444));
    assert(cpu.x[30] == UINT64_C(0xdeadbeef55667788));
    assert_pair(get_pair(memory->data + offset, 4),
            UINT32_C(0x55667788), 0);
    assert(cpu.nzcv == UINT32_C(0x50000000));
}

static void test_register_overlap_snapshots(struct test_memory *memory) {
    const size_t offset = 0x200;
    put_pair(memory->data + offset, 8,
            UINT64_C(0xaaaaaaaaaaaaaaaa),
            UINT64_C(0xbbbbbbbbbbbbbbbb));
    struct cpu_state same = {.pc = UINT64_C(0x2000)};
    same.x[2] = UINT64_C(0x1111111111111111);
    same.x[3] = UINT64_C(0x2222222222222222);
    same.x[4] = DATA_PAGE + offset;
    assert_retired(execute_word(memory, &same,
            encode(true, false, false, 2, 2, 4)));
    assert(same.x[2] == UINT64_C(0xaaaaaaaaaaaaaaaa));
    assert(same.x[3] == UINT64_C(0xbbbbbbbbbbbbbbbb));
    assert_pair(get_pair(memory->data + offset, 8),
            UINT64_C(0xaaaaaaaaaaaaaaaa),
            UINT64_C(0xbbbbbbbbbbbbbbbb));

    put_pair(memory->data + offset, 8,
            UINT64_C(0x1111111111111111),
            UINT64_C(0x2222222222222222));
    same.pc = UINT64_C(0x2008);
    same.x[2] = UINT64_C(0x1111111111111111);
    same.x[3] = UINT64_C(0x2222222222222222);
    assert_retired(execute_word(memory, &same,
            encode(true, true, true, 2, 2, 4)));
    assert_pair(get_pair(memory->data + offset, 8),
            UINT64_C(0x1111111111111111),
            UINT64_C(0x2222222222222222));

    put_pair(memory->data + offset, 8,
            UINT64_C(0x123456789abcdef0),
            UINT64_C(0x0fedcba987654321));
    struct cpu_state rn_replacement = {.pc = UINT64_C(0x2010)};
    rn_replacement.x[0] = UINT64_C(0x123456789abcdef0);
    rn_replacement.x[1] = UINT64_C(0x0fedcba987654321);
    rn_replacement.x[4] = DATA_PAGE + offset;
    rn_replacement.x[5] = UINT64_C(0x55aa55aa55aa55aa);
    assert_retired(execute_word(memory, &rn_replacement,
            encode(true, false, false, 0, 4, 4)));
    assert_pair(get_pair(memory->data + offset, 8),
            DATA_PAGE + offset, UINT64_C(0x55aa55aa55aa55aa));

    put_pair(memory->data + offset, 8,
            UINT64_C(0x9999999999999999),
            UINT64_C(0x7777777777777777));
    struct cpu_state rn_compare = {.pc = UINT64_C(0x2020)};
    rn_compare.x[2] = DATA_PAGE + offset;
    rn_compare.x[3] = UINT64_C(0x7777777777777777);
    rn_compare.x[4] = UINT64_C(0x3333333333333333);
    rn_compare.x[5] = UINT64_C(0x4444444444444444);
    assert_retired(execute_word(memory, &rn_compare,
            encode(true, true, false, 2, 4, 2)));
    assert(rn_compare.x[2] == UINT64_C(0x9999999999999999));
    assert(rn_compare.x[3] == UINT64_C(0x7777777777777777));
    assert_pair(get_pair(memory->data + offset, 8),
            UINT64_C(0x9999999999999999),
            UINT64_C(0x7777777777777777));
}

static void assert_fault_without_commit(struct test_memory *memory,
        struct cpu_state *cpu, dword_t word,
        enum guest_memory_fault_kind kind, guest_addr_t fault_address) {
    struct cpu_state before = *cpu;
    byte_t data_before[GUEST_MEMORY_PAGE_SIZE];
    byte_t readonly_before[GUEST_MEMORY_PAGE_SIZE];
    memcpy(data_before, memory->data, sizeof(data_before));
    memcpy(readonly_before, memory->readonly, sizeof(readonly_before));

    struct aarch64_execute_result result = execute_word(memory, cpu, word);
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == kind);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(result.fault.address == fault_address);
    for (size_t i = 0; i < array_size(cpu->x); i++)
        assert(cpu->x[i] == before.x[i]);
    assert(cpu->sp == before.sp);
    assert(cpu->pc == before.pc);
    assert(cpu->nzcv == before.nzcv);
    assert(!cpu->exclusive.valid);
    before.exclusive.valid = false;
    assert_monitor_equal(&cpu->exclusive, &before.exclusive);
    assert(memcmp(memory->data, data_before, sizeof(data_before)) == 0);
    assert(memcmp(memory->readonly,
            readonly_before, sizeof(readonly_before)) == 0);
}

static void init_fault_cpu(struct cpu_state *cpu,
        struct test_memory *memory, guest_addr_t address, bool sp_base) {
    *cpu = (struct cpu_state) {
        .pc = UINT64_C(0x3000),
        .sp = sp_base ? address : UINT64_C(0x1122334455667788),
        .nzcv = UINT32_C(0x90000000),
    };
    cpu->x[0] = UINT64_C(0x0123456789abcdef);
    cpu->x[1] = UINT64_C(0xfedcba9876543210);
    cpu->x[2] = UINT64_C(0x1111222233334444);
    cpu->x[3] = UINT64_C(0x5555666677778888);
    if (!sp_base)
        cpu->x[4] = address;
    set_monitor_sentinel(cpu, &memory->space);
}

static void test_alignment_and_faults(struct test_memory *memory) {
    const size_t offset = 8;
    put_pair(memory->data + offset, 4,
            UINT32_C(0x11223344), UINT32_C(0x55667788));
    struct cpu_state ordinary = {.pc = UINT64_C(0x2800)};
    ordinary.x[0] = UINT32_C(0x11223344);
    ordinary.x[1] = UINT32_C(0x55667788);
    ordinary.x[2] = UINT32_C(0xaabbccdd);
    ordinary.x[3] = UINT32_C(0xeeff0011);
    ordinary.x[4] = DATA_PAGE + offset;
    set_monitor_sentinel(&ordinary, &memory->space);
    struct aarch64_exclusive_monitor ordinary_monitor = ordinary.exclusive;
    assert_retired(execute_word(memory, &ordinary,
            encode(false, false, false, 0, 2, 4)));
    assert_pair(get_pair(memory->data + offset, 4),
            UINT32_C(0xaabbccdd), UINT32_C(0xeeff0011));
    assert_monitor_equal(&ordinary.exclusive, &ordinary_monitor);

    put_pair(memory->data + offset, 4,
            UINT32_C(0x11223344), UINT32_C(0x55667788));
    struct cpu_state sp;
    init_fault_cpu(&sp, memory, DATA_PAGE + offset, true);
    sp.x[0] = UINT32_C(0x11223344);
    sp.x[1] = UINT32_C(0x55667788);
    assert_fault_without_commit(memory, &sp,
            encode(false, false, false, 0, 2, 31),
            GUEST_MEMORY_FAULT_ALIGNMENT, DATA_PAGE + offset);

    const size_t aligned_offset = 0x20;
    put_pair(memory->data + aligned_offset, 4,
            UINT32_C(0x01020304), UINT32_C(0x05060708));
    struct cpu_state aligned_sp = {
        .pc = UINT64_C(0x2810),
        .sp = DATA_PAGE + aligned_offset,
    };
    aligned_sp.x[0] = UINT32_C(0x01020304);
    aligned_sp.x[1] = UINT32_C(0x05060708);
    aligned_sp.x[2] = UINT32_C(0x11121314);
    aligned_sp.x[3] = UINT32_C(0x15161718);
    set_monitor_sentinel(&aligned_sp, &memory->space);
    struct aarch64_exclusive_monitor aligned_monitor =
            aligned_sp.exclusive;
    assert_retired(execute_word(memory, &aligned_sp,
            encode(false, true, false, 0, 2, 31)));
    assert(aligned_sp.sp == DATA_PAGE + aligned_offset);
    assert(aligned_sp.pc == UINT64_C(0x2814));
    assert_pair(get_pair(memory->data + aligned_offset, 4),
            UINT32_C(0x11121314), UINT32_C(0x15161718));
    assert_monitor_equal(&aligned_sp.exclusive, &aligned_monitor);

    struct cpu_state ordinary_misaligned;
    init_fault_cpu(&ordinary_misaligned, memory, DATA_PAGE + 4, false);
    assert_fault_without_commit(memory, &ordinary_misaligned,
            encode(false, false, false, 0, 2, 4),
            GUEST_MEMORY_FAULT_ALIGNMENT, DATA_PAGE + 4);

    struct cpu_state unmapped;
    init_fault_cpu(&unmapped, memory, UNMAPPED_PAGE, false);
    assert_fault_without_commit(memory, &unmapped,
            encode(true, false, false, 0, 2, 4),
            GUEST_MEMORY_FAULT_UNMAPPED, UNMAPPED_PAGE);

    put_pair(memory->readonly + 0x100, 8,
            UINT64_C(0xaaaaaaaaaaaaaaaa),
            UINT64_C(0xbbbbbbbbbbbbbbbb));
    struct cpu_state readonly;
    init_fault_cpu(&readonly, memory, READONLY_PAGE + 0x100, false);
    // 即使比较必然失败，CASP 仍以写访问预检权限。
    assert_fault_without_commit(memory, &readonly,
            encode(true, true, true, 0, 2, 4),
            GUEST_MEMORY_FAULT_PERMISSION, READONLY_PAGE + 0x100);

    struct cpu_state address_size;
    init_fault_cpu(&address_size, memory, ADDRESS_LIMIT, false);
    assert_fault_without_commit(memory, &address_size,
            encode(true, false, true, 0, 2, 4),
            GUEST_MEMORY_FAULT_ADDRESS_SIZE, ADDRESS_LIMIT);
}

static struct guest_tlb_exclusive_token reserve_pair(
        struct test_memory *memory, guest_addr_t address) {
    byte_t observed[16];
    struct guest_tlb_exclusive_token token;
    struct guest_memory_fault fault;
    assert(guest_tlb_load_exclusive(&memory->tlb, address,
            observed, sizeof(observed), &token, &fault));
    return token;
}

static void test_reservation_effects(struct test_memory *memory) {
    const size_t offset = 0x300;
    guest_addr_t address = DATA_PAGE + offset;
    put_pair(memory->data + offset, 8,
            UINT64_C(0x123456789abcdef0),
            UINT64_C(0x0fedcba987654321));
    struct guest_tlb_exclusive_token success_token =
            reserve_pair(memory, address);
    assert(guest_address_space_exclusive_matches(&memory->space,
            address, success_token.write_generation));

    struct cpu_state success = {.pc = UINT64_C(0x3800)};
    success.x[0] = UINT64_C(0x123456789abcdef0);
    success.x[1] = UINT64_C(0x0fedcba987654321);
    success.x[2] = success.x[0];
    success.x[3] = success.x[1];
    success.x[4] = address;
    aarch64_set_exclusive(&success, address, 16, true,
            success.x[0], success.x[1], &memory->space,
            success_token.mapping_generation,
            success_token.write_generation, success_token.sync_identity);
    struct aarch64_exclusive_monitor success_monitor = success.exclusive;
    assert_retired(execute_word(memory, &success,
            encode(true, false, false, 0, 2, 4)));
    assert_monitor_equal(&success.exclusive, &success_monitor);
    assert(!guest_address_space_exclusive_matches(&memory->space,
            address, success_token.write_generation));

    struct guest_tlb_exclusive_token mismatch_token =
            reserve_pair(memory, address);
    struct cpu_state mismatch = {.pc = UINT64_C(0x3810)};
    mismatch.x[0] = 0;
    mismatch.x[1] = 0;
    mismatch.x[2] = UINT64_MAX;
    mismatch.x[3] = UINT64_MAX;
    mismatch.x[4] = address;
    aarch64_set_exclusive(&mismatch, address, 16, true,
            UINT64_C(0x123456789abcdef0),
            UINT64_C(0x0fedcba987654321), &memory->space,
            mismatch_token.mapping_generation,
            mismatch_token.write_generation, mismatch_token.sync_identity);
    struct aarch64_exclusive_monitor mismatch_monitor = mismatch.exclusive;
    assert_retired(execute_word(memory, &mismatch,
            encode(true, true, true, 0, 2, 4)));
    assert_monitor_equal(&mismatch.exclusive, &mismatch_monitor);
    assert(guest_address_space_exclusive_matches(&memory->space,
            address, mismatch_token.write_generation));
    assert_pair(get_pair(memory->data + offset, 8),
            UINT64_C(0x123456789abcdef0),
            UINT64_C(0x0fedcba987654321));
}

struct concurrent_context {
    struct test_memory *memory;
    struct aarch64_decoded instruction;
    guest_addr_t address;
    unsigned iterations;
    atomic_uint *ready;
    atomic_uint *mismatches;
};

static void *increment_pair(void *opaque) {
    struct concurrent_context *context = opaque;
    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &context->memory->space);
    struct cpu_state cpu = {0};
    cpu.x[4] = context->address;
    set_monitor_sentinel(&cpu, &context->memory->space);
    struct aarch64_exclusive_monitor monitor = cpu.exclusive;
    qword_t expected = 0;
    atomic_fetch_add_explicit(context->ready, 1, memory_order_release);
    while (atomic_load_explicit(context->ready, memory_order_acquire) != 2)
        sched_yield();

    for (unsigned successes = 0; successes < context->iterations;) {
        qword_t replacement = expected + 1;
        cpu.x[0] = expected;
        cpu.x[1] = ~expected;
        cpu.x[2] = replacement;
        cpu.x[3] = ~replacement;
        struct aarch64_execute_result result = aarch64_execute(
                &cpu, &tlb, &context->instruction);
        assert_retired(result);
        assert(cpu.x[1] == ~cpu.x[0]);
        if (cpu.x[0] == expected) {
            expected = replacement;
            successes++;
        } else {
            expected = cpu.x[0];
            atomic_fetch_add_explicit(
                    context->mismatches, 1, memory_order_relaxed);
        }
    }
    assert_monitor_equal(&cpu.exclusive, &monitor);
    return NULL;
}

static void test_two_thread_increment(struct test_memory *memory) {
    const size_t offset = 0x400;
    const unsigned iterations = 2000;
    put_pair(memory->data + offset, 8, 0, UINT64_MAX);
    atomic_uint ready = 0;
    atomic_uint mismatches = 0;
    struct concurrent_context contexts[2];
    pthread_t threads[2];
    for (size_t i = 0; i < array_size(contexts); i++) {
        contexts[i] = (struct concurrent_context) {
            .memory = memory,
            .instruction = decode(encode(true, true, true, 0, 2, 4)),
            .address = DATA_PAGE + offset,
            .iterations = iterations,
            .ready = &ready,
            .mismatches = &mismatches,
        };
        assert(pthread_create(
                &threads[i], NULL, increment_pair, &contexts[i]) == 0);
    }
    for (size_t i = 0; i < array_size(threads); i++)
        assert(pthread_join(threads[i], NULL) == 0);

    qword_t expected = (qword_t) iterations * array_size(threads);
    assert_pair(get_pair(memory->data + offset, 8), expected, ~expected);
    assert(atomic_load_explicit(&mismatches, memory_order_relaxed) > 0);
}

static void assert_execution_equal(
        const struct aarch64_execute_result *left,
        const struct aarch64_execute_result *right) {
    assert(left->stop == right->stop);
    assert(left->fault.address == right->fault.address);
    assert(left->fault.access == right->fault.access);
    assert(left->fault.kind == right->fault.kind);
}

static void assert_cpu_equivalent(const struct cpu_state *left,
        const struct cpu_state *right) {
    for (size_t i = 0; i < array_size(left->x); i++)
        assert(left->x[i] == right->x[i]);
    assert(left->sp == right->sp);
    assert(left->pc == right->pc);
    assert(left->nzcv == right->nzcv);
    assert(left->exclusive.address == right->exclusive.address);
    assert(left->exclusive.value_low == right->exclusive.value_low);
    assert(left->exclusive.value_high == right->exclusive.value_high);
    assert(left->exclusive.mapping_epoch == right->exclusive.mapping_epoch);
    assert(left->exclusive.write_epoch == right->exclusive.write_epoch);
    assert(left->exclusive.sync_identity == right->exclusive.sync_identity);
    assert(left->exclusive.size == right->exclusive.size);
    assert(left->exclusive.pair == right->exclusive.pair);
    assert(left->exclusive.valid == right->exclusive.valid);
}

static void test_threaded_fallback_differential(void) {
    struct test_memory c_memory;
    struct test_memory threaded_memory;
    init_test_memory(&c_memory);
    init_test_memory(&threaded_memory);
    const size_t offset = 0x500;
    const dword_t word = encode(true, true, true, 0, 2, 4);
    struct aarch64_decoded instruction = decode(word);
    put_pair(c_memory.data + offset, 8,
            UINT64_C(0x1111222233334444),
            UINT64_C(0x5555666677778888));
    put_pair(threaded_memory.data + offset, 8,
            UINT64_C(0x1111222233334444),
            UINT64_C(0x5555666677778888));
    struct cpu_state c_cpu = {
        .pc = UINT64_C(0x4000),
        .nzcv = UINT32_C(0x60000000),
    };
    struct cpu_state threaded_cpu = c_cpu;
    c_cpu.x[0] = threaded_cpu.x[0] = UINT64_C(0x1111222233334444);
    c_cpu.x[1] = threaded_cpu.x[1] = UINT64_C(0x5555666677778888);
    c_cpu.x[2] = threaded_cpu.x[2] = UINT64_C(0x9999aaaabbbbcccc);
    c_cpu.x[3] = threaded_cpu.x[3] = UINT64_C(0xddddeeeeffff0000);
    c_cpu.x[4] = threaded_cpu.x[4] = DATA_PAGE + offset;
    set_monitor_sentinel(&c_cpu, &c_memory.space);
    set_monitor_sentinel(&threaded_cpu, &threaded_memory.space);
    struct aarch64_threaded_cache cache = {0};

    struct aarch64_execute_result c_result = aarch64_execute(
            &c_cpu, &c_memory.tlb, &instruction);
    struct aarch64_execute_result threaded_result;
    assert(aarch64_threaded_execute(&cache, &threaded_cpu,
            &threaded_memory.tlb, UINT64_C(0x4000), word,
            &threaded_result));
    assert_execution_equal(&c_result, &threaded_result);
    assert_cpu_equivalent(&c_cpu, &threaded_cpu);
    assert(memcmp(c_memory.data,
            threaded_memory.data, sizeof(c_memory.data)) == 0);
    assert(cache.stats.cache_hits == 0);
    assert(cache.stats.cache_misses == 1);
    assert(cache.stats.fast_dispatches == 0);
    assert(cache.stats.c_fallbacks == 1);

    c_cpu.pc = threaded_cpu.pc = UINT64_C(0x4000);
    c_cpu.x[0] = threaded_cpu.x[0] = 0;
    c_cpu.x[1] = threaded_cpu.x[1] = 0;
    c_cpu.x[2] = threaded_cpu.x[2] = 1;
    c_cpu.x[3] = threaded_cpu.x[3] = ~UINT64_C(1);
    c_result = aarch64_execute(&c_cpu, &c_memory.tlb, &instruction);
    assert(aarch64_threaded_execute(&cache, &threaded_cpu,
            &threaded_memory.tlb, UINT64_C(0x4000), word,
            &threaded_result));
    assert_execution_equal(&c_result, &threaded_result);
    assert_cpu_equivalent(&c_cpu, &threaded_cpu);
    assert(memcmp(c_memory.data,
            threaded_memory.data, sizeof(c_memory.data)) == 0);
    assert(cache.stats.cache_hits == 1);
    assert(cache.stats.cache_misses == 1);
    assert(cache.stats.fast_dispatches == 0);
    assert(cache.stats.c_fallbacks == 2);

    destroy_test_memory(&c_memory);
    destroy_test_memory(&threaded_memory);
}

int main(void) {
    test_clang_vectors();
    test_ordering_and_width_fields();
    test_odd_pair_heads_are_undefined();
    test_legal_register_overlap();

    struct test_memory memory;
    init_test_memory(&memory);
    test_all_orders_success_and_mismatch(&memory);
    test_zero_register_and_zero_extension(&memory);
    test_register_overlap_snapshots(&memory);
    test_alignment_and_faults(&memory);
    test_reservation_effects(&memory);
    test_two_thread_increment(&memory);
    destroy_test_memory(&memory);

    test_threaded_fallback_differential();
    return 0;
}

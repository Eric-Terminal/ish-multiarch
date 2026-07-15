#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "guest/aarch64/decode.h"
#include "guest/aarch64/execute.h"

#define DATA_PAGE UINT64_C(0x000056789abcd000)
#define READONLY_PAGE (DATA_PAGE + GUEST_MEMORY_PAGE_SIZE)
#define UNMAPPED_PAGE (READONLY_PAGE + GUEST_MEMORY_PAGE_SIZE)

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

static dword_t encode_load(unsigned size_shift, bool acquire,
        byte_t rt, byte_t rn) {
    return UINT32_C(0x085f7c00) |
            (dword_t) size_shift << 30 |
            (dword_t) acquire << 15 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_store(unsigned size_shift, bool release,
        byte_t rs, byte_t rt, byte_t rn) {
    return UINT32_C(0x08007c00) |
            (dword_t) size_shift << 30 |
            (dword_t) release << 15 |
            (dword_t) rs << 16 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_pair_load(bool wide, bool acquire,
        byte_t rt, byte_t rt2, byte_t rn) {
    return UINT32_C(0x887f0000) |
            (dword_t) wide << 30 |
            (dword_t) acquire << 15 |
            (dword_t) rt2 << 10 |
            (dword_t) rn << 5 |
            rt;
}

static dword_t encode_pair_store(bool wide, bool release,
        byte_t rs, byte_t rt, byte_t rt2, byte_t rn) {
    return UINT32_C(0x88200000) |
            (dword_t) wide << 30 |
            (dword_t) rs << 16 |
            (dword_t) release << 15 |
            (dword_t) rt2 << 10 |
            (dword_t) rn << 5 |
            rt;
}

static struct aarch64_decoded decode(dword_t word) {
    struct aarch64_decoded instruction;
    assert(aarch64_decode(word, &instruction));
    return instruction;
}

static struct aarch64_execute_result execute_word(struct test_memory *memory,
        struct cpu_state *cpu, dword_t word) {
    struct aarch64_decoded instruction = decode(word);
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

static void put_value(byte_t *destination, qword_t value, byte_t size) {
    for (byte_t byte = 0; byte < size; byte++)
        destination[byte] = (byte_t) (value >> (byte * 8));
}

static qword_t get_value(const byte_t *source, byte_t size) {
    qword_t value = 0;
    for (byte_t byte = 0; byte < size; byte++)
        value |= (qword_t) source[byte] << (byte * 8);
    return value;
}

static qword_t size_mask(byte_t size) {
    return size == 8 ? UINT64_MAX :
            (UINT64_C(1) << (size * 8)) - 1;
}

static void assert_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t size, byte_t width, byte_t rs, byte_t rt, byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.exclusive.size == size);
    assert(instruction.operands.exclusive.rs == rs);
    assert(instruction.operands.exclusive.rt == rt);
    assert(instruction.operands.exclusive.rt2 == 31);
    assert(instruction.operands.exclusive.rn == rn);
}

static void assert_pair_decode(dword_t word, enum aarch64_opcode opcode,
        byte_t size, byte_t width, byte_t rs, byte_t rt, byte_t rt2,
        byte_t rn) {
    struct aarch64_decoded instruction = decode(word);
    assert(instruction.opcode == opcode);
    assert(instruction.width == width);
    assert(instruction.operands.exclusive.size == size);
    assert(instruction.operands.exclusive.rs == rs);
    assert(instruction.operands.exclusive.rt == rt);
    assert(instruction.operands.exclusive.rt2 == rt2);
    assert(instruction.operands.exclusive.rn == rn);
}

static void test_apple_vectors(void) {
    assert_decode(UINT32_C(0x085f7c20), AARCH64_OP_LDXR,
            1, 32, 31, 0, 1);
    assert_decode(UINT32_C(0x485f7c62), AARCH64_OP_LDXR,
            2, 32, 31, 2, 3);
    assert_decode(UINT32_C(0x885f7ca4), AARCH64_OP_LDXR,
            4, 32, 31, 4, 5);
    assert_decode(UINT32_C(0xc85f7ce6), AARCH64_OP_LDXR,
            8, 64, 31, 6, 7);
    assert_decode(UINT32_C(0x085ffd28), AARCH64_OP_LDAXR,
            1, 32, 31, 8, 9);
    assert_decode(UINT32_C(0x485ffd6a), AARCH64_OP_LDAXR,
            2, 32, 31, 10, 11);
    assert_decode(UINT32_C(0x885ffdac), AARCH64_OP_LDAXR,
            4, 32, 31, 12, 13);
    assert_decode(UINT32_C(0xc85ffdee), AARCH64_OP_LDAXR,
            8, 64, 31, 14, 15);
    assert_decode(UINT32_C(0x08107e51), AARCH64_OP_STXR,
            1, 32, 16, 17, 18);
    assert_decode(UINT32_C(0x48137eb4), AARCH64_OP_STXR,
            2, 32, 19, 20, 21);
    assert_decode(UINT32_C(0x88167f17), AARCH64_OP_STXR,
            4, 32, 22, 23, 24);
    assert_decode(UINT32_C(0xc8197f7a), AARCH64_OP_STXR,
            8, 64, 25, 26, 27);
    assert_decode(UINT32_C(0x0800fc41), AARCH64_OP_STLXR,
            1, 32, 0, 1, 2);
    assert_decode(UINT32_C(0x4803fca4), AARCH64_OP_STLXR,
            2, 32, 3, 4, 5);
    assert_decode(UINT32_C(0x8806fd07), AARCH64_OP_STLXR,
            4, 32, 6, 7, 8);
    assert_decode(UINT32_C(0xc809fd6a), AARCH64_OP_STLXR,
            8, 64, 9, 10, 11);
    assert_decode(UINT32_C(0xc85fffff), AARCH64_OP_LDAXR,
            8, 64, 31, 31, 31);
    assert_decode(UINT32_C(0xc800ffff), AARCH64_OP_STLXR,
            8, 64, 0, 31, 31);
}

static void test_pair_apple_vectors(void) {
    assert_pair_decode(UINT32_C(0x887f0440), AARCH64_OP_LDXP,
            4, 32, 31, 0, 1, 2);
    assert_pair_decode(UINT32_C(0x887f90a3), AARCH64_OP_LDAXP,
            4, 32, 31, 3, 4, 5);
    assert_pair_decode(UINT32_C(0xc87f1d06), AARCH64_OP_LDXP,
            8, 64, 31, 6, 7, 8);
    assert_pair_decode(UINT32_C(0xc87fabe9), AARCH64_OP_LDAXP,
            8, 64, 31, 9, 10, 31);
    assert_pair_decode(UINT32_C(0x882b35cc), AARCH64_OP_STXP,
            4, 32, 11, 12, 13, 14);
    assert_pair_decode(UINT32_C(0x882fc650), AARCH64_OP_STLXP,
            4, 32, 15, 16, 17, 18);
    assert_pair_decode(UINT32_C(0xc83356d4), AARCH64_OP_STXP,
            8, 64, 19, 20, 21, 22);
    assert_pair_decode(UINT32_C(0xc837e7f8), AARCH64_OP_STLXP,
            8, 64, 23, 24, 25, 31);
}

static void test_pair_encoding_space(void) {
    for (unsigned wide = 0; wide < 2; wide++) {
        byte_t width = wide ? 64 : 32;
        byte_t size = wide ? 8 : 4;
        for (unsigned acquire = 0; acquire < 2; acquire++) {
            struct aarch64_decoded instruction = decode(encode_pair_load(
                    wide != 0, acquire != 0, 1, 2, 3));
            assert(instruction.opcode == (acquire ?
                    AARCH64_OP_LDAXP : AARCH64_OP_LDXP));
            assert(instruction.width == width);
            assert(instruction.operands.exclusive.size == size);
        }
        for (unsigned release = 0; release < 2; release++) {
            struct aarch64_decoded instruction = decode(encode_pair_store(
                    wide != 0, release != 0, 4, 5, 6, 7));
            assert(instruction.opcode == (release ?
                    AARCH64_OP_STLXP : AARCH64_OP_STXP));
            assert(instruction.width == width);
            assert(instruction.operands.exclusive.size == size);
        }
    }

    struct aarch64_decoded instruction;
    assert(!aarch64_decode(encode_pair_load(false, false, 2, 2, 3),
            &instruction));
    assert(!aarch64_decode(encode_pair_store(
            false, false, 4, 4, 5, 6), &instruction));
    assert(!aarch64_decode(encode_pair_store(
            false, false, 5, 4, 5, 6), &instruction));
    assert(!aarch64_decode(encode_pair_store(
            false, false, 6, 4, 5, 6), &instruction));
    assert(aarch64_decode(encode_pair_store(
            false, false, 31, 4, 5, 31), &instruction));
}

static void test_encoding_space(void) {
    for (unsigned size_shift = 0; size_shift < 4; size_shift++) {
        for (unsigned acquire = 0; acquire < 2; acquire++) {
            struct aarch64_decoded instruction = decode(encode_load(
                    size_shift, acquire, 0, 1));
            assert(instruction.opcode == (acquire ?
                    AARCH64_OP_LDAXR : AARCH64_OP_LDXR));
            assert(instruction.operands.exclusive.size ==
                    (1U << size_shift));
            assert(instruction.width == (size_shift == 3 ? 64 : 32));
        }
    }

    unsigned valid = 0;
    for (unsigned rs = 0; rs < 32; rs++) {
        for (unsigned rt = 0; rt < 32; rt++) {
            for (unsigned rn = 0; rn < 32; rn++) {
                struct aarch64_decoded instruction;
                bool decoded = aarch64_decode(encode_store(2, true,
                        (byte_t) rs, (byte_t) rt, (byte_t) rn),
                        &instruction);
                bool expected = rs != rt && (rn == 31 || rs != rn);
                assert(decoded == expected);
                if (decoded)
                    valid++;
            }
        }
    }
    assert(valid == 30783);

    for (unsigned size_shift = 0; size_shift < 4; size_shift++) {
        for (unsigned release = 0; release < 2; release++) {
            struct aarch64_decoded instruction = decode(encode_store(
                    size_shift, release, 0, 1, 2));
            assert(instruction.opcode == (release ?
                    AARCH64_OP_STLXR : AARCH64_OP_STXR));
            assert(instruction.operands.exclusive.size ==
                    (1U << size_shift));
        }
    }
}

static void test_all_sizes(struct test_memory *memory) {
    for (unsigned size_shift = 0; size_shift < 4; size_shift++) {
        byte_t size = (byte_t) (1U << size_shift);
        size_t offset = 128 + size_shift * 32;
        qword_t original = UINT64_C(0xfedcba9876543210) & size_mask(size);
        qword_t replacement = UINT64_C(0x0123456789abcdef) & size_mask(size);
        put_value(memory->data + offset, original, size);

        struct cpu_state cpu = {
            .pc = UINT64_C(0x1000),
            .sp = UINT64_C(0x1122334455667788),
            .nzcv = UINT32_C(0xa0000000),
        };
        cpu.x[1] = DATA_PAGE + offset;
        cpu.x[2] = UINT64_MAX;
        assert_retired(memory, &cpu,
                encode_load(size_shift, size_shift & 1, 2, 1));
        assert(cpu.x[2] == original);
        assert(cpu.exclusive.valid);
        assert(cpu.exclusive.address == DATA_PAGE + offset);
        assert(cpu.exclusive.size == size);
        assert(!cpu.exclusive.pair);
        assert(cpu.exclusive.value_low == original);
        assert(cpu.exclusive.address_space == &memory->space);
        assert(cpu.exclusive.mapping_epoch == memory->space.generation);
        assert(guest_address_space_exclusive_matches(&memory->space,
                DATA_PAGE + offset, cpu.exclusive.write_epoch));

        cpu.x[3] = replacement;
        cpu.x[4] = UINT64_MAX;
        assert_retired(memory, &cpu,
                encode_store(size_shift, size_shift & 1, 4, 3, 1));
        assert(cpu.x[4] == 0);
        assert(get_value(memory->data + offset, size) == replacement);
        assert(!cpu.exclusive.valid);

        cpu.x[3] = original;
        cpu.x[4] = UINT64_MAX;
        assert_retired(memory, &cpu,
                encode_store(size_shift, false, 4, 3, 1));
        assert(cpu.x[4] == 1);
        assert(get_value(memory->data + offset, size) == replacement);
        assert(cpu.sp == UINT64_C(0x1122334455667788));
        assert(cpu.nzcv == UINT32_C(0xa0000000));
    }
}

static void test_zero_register_and_sp(struct test_memory *memory) {
    size_t offset = 384;
    put_value(memory->data + offset,
            UINT64_C(0x8877665544332211), 8);
    struct cpu_state cpu = {
        .pc = UINT64_C(0x1800),
        .sp = DATA_PAGE + offset,
    };
    assert_retired(memory, &cpu, UINT32_C(0xc85fffff));
    assert(cpu.exclusive.valid);
    assert_retired(memory, &cpu, UINT32_C(0xc800ffff));
    assert(cpu.x[0] == 0);
    assert(get_value(memory->data + offset, 8) == 0);

    put_value(memory->data + offset, UINT32_C(0x12345678), 4);
    assert_retired(memory, &cpu, encode_load(2, false, 31, 31));
    cpu.x[1] = UINT32_C(0xaabbccdd);
    assert_retired(memory, &cpu, encode_store(2, false, 31, 1, 31));
    assert(get_value(memory->data + offset, 4) == UINT32_C(0xaabbccdd));
}

static void test_monitor_failures(struct test_memory *memory) {
    size_t offset = 512;
    put_value(memory->data + offset, UINT32_C(0x12345678), 4);
    struct cpu_state cpu = {.pc = UINT64_C(0x2000)};
    cpu.x[1] = DATA_PAGE + offset;
    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));

    cpu.x[0] = UINT32_C(0x12345678);
    assert_retired(memory, &cpu, UINT32_C(0xb9000020));
    assert(cpu.exclusive.valid);
    cpu.x[3] = UINT32_C(0x87654321);
    cpu.x[4] = UINT64_MAX;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);

    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));
    byte_t external[4];
    put_value(external, UINT32_C(0xabcdef01), sizeof(external));
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset,
            external, sizeof(external), &fault));
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);
    assert(get_value(memory->data + offset, 4) == UINT32_C(0xabcdef01));

    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));
    guest_address_space_changed(&memory->space);
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);

    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));
    cpu.x[1] += 4;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);
    cpu.x[1] -= 4;

    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));
    assert_retired(memory, &cpu, encode_store(1, false, 4, 3, 1));
    assert(cpu.x[4] == 1);

    aarch64_clear_exclusive(&cpu);
    cpu.x[1] = UNMAPPED_PAGE;
    cpu.x[4] = UINT64_MAX;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);
}

static void test_aba_invalidates_reservation(struct test_memory *memory) {
    size_t offset = 576;
    put_value(memory->data + offset, 0, 4);
    struct cpu_state cpu = {.pc = UINT64_C(0x2400)};
    cpu.x[1] = DATA_PAGE + offset;
    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));
    qword_t reservation_generation = cpu.exclusive.write_epoch;

    byte_t intermediate[4];
    byte_t original[4];
    put_value(intermediate, 1, sizeof(intermediate));
    put_value(original, 0, sizeof(original));
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset,
            intermediate, sizeof(intermediate), &fault));
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset,
            original, sizeof(original), &fault));
    assert(!guest_address_space_exclusive_matches(&memory->space,
            DATA_PAGE + offset, reservation_generation));

    cpu.x[3] = 2;
    cpu.x[4] = UINT64_MAX;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);
    assert(get_value(memory->data + offset, 4) == 0);
}

static void test_same_granule_write_invalidates_reservation(
        struct test_memory *memory) {
    size_t offset = 608;
    put_value(memory->data + offset, 3, 4);
    struct cpu_state cpu = {.pc = UINT64_C(0x2600)};
    cpu.x[1] = DATA_PAGE + offset;
    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));

    byte_t unrelated[4];
    put_value(unrelated, 9, sizeof(unrelated));
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset + 8,
            unrelated, sizeof(unrelated), &fault));
    cpu.x[3] = 4;
    cpu.x[4] = UINT64_MAX;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);
    assert(get_value(memory->data + offset, 4) == 3);

    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));
    byte_t same_value[4];
    put_value(same_value, 3, sizeof(same_value));
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset,
            same_value, sizeof(same_value), &fault));
    cpu.x[4] = UINT64_MAX;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 1);
    assert(get_value(memory->data + offset, 4) == 3);
}

static void test_unrelated_store_preserves_reservation(
        struct test_memory *memory) {
    size_t reserved_offset = 768;
    size_t unrelated_offset = 800;
    put_value(memory->data + reserved_offset, 5, 4);
    put_value(memory->data + unrelated_offset, 0, 4);
    struct cpu_state cpu = {.pc = UINT64_C(0x2700)};
    cpu.x[1] = DATA_PAGE + reserved_offset;
    assert_retired(memory, &cpu, encode_load(2, false, 2, 1));

    cpu.x[0] = 11;
    cpu.x[1] = DATA_PAGE + unrelated_offset;
    assert_retired(memory, &cpu, UINT32_C(0xb9000020));
    assert(cpu.exclusive.valid);
    assert(get_value(memory->data + unrelated_offset, 4) == 11);

    cpu.x[1] = DATA_PAGE + reserved_offset;
    cpu.x[3] = 6;
    cpu.x[4] = UINT64_MAX;
    assert_retired(memory, &cpu, encode_store(2, false, 4, 3, 1));
    assert(cpu.x[4] == 0);
    assert(get_value(memory->data + reserved_offset, 4) == 6);
}

static void test_exact_granule_tracking(struct test_memory *memory) {
    enum { reservation_count = GUEST_MEMORY_EXCLUSIVE_BUCKET_COUNT + 1 };
    memory->pages[1].permissions |= GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
    memset(memory->data, 0, sizeof(memory->data));
    memory->readonly[0] = 0;

    struct guest_tlb_exclusive_token tokens[reservation_count];
    byte_t values[reservation_count];
    struct guest_memory_fault fault;
    for (unsigned index = 0; index < reservation_count; index++) {
        guest_addr_t address = DATA_PAGE +
                index * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE;
        assert(guest_tlb_load_exclusive(&memory->tlb, address,
                &values[index], 1, &tokens[index], &fault));
        assert(values[index] == 0);
    }

    byte_t replacement = 1;
    for (unsigned index = 0; index < reservation_count; index++) {
        guest_addr_t address = DATA_PAGE +
                index * GUEST_MEMORY_EXCLUSIVE_GRANULE_SIZE;
        assert(guest_tlb_store_exclusive(&memory->tlb, address,
                &values[index], &replacement, 1, tokens[index], &fault) ==
                GUEST_TLB_EXCLUSIVE_STORED);
    }

    memory->pages[1].permissions &= ~GUEST_MEMORY_WRITE;
    guest_address_space_changed(&memory->space);
}

static void test_two_reservations_have_one_winner(
        struct test_memory *memory) {
    size_t offset = 832;
    put_value(memory->data + offset, 0, 4);
    struct cpu_state cpus[2] = {
        {.pc = UINT64_C(0x2750)},
        {.pc = UINT64_C(0x2750)},
    };
    for (unsigned index = 0; index < array_size(cpus); index++) {
        cpus[index].x[1] = DATA_PAGE + offset;
        assert_retired(memory, &cpus[index],
                encode_load(2, true, 2, 1));
        cpus[index].x[3] = index + 1;
        cpus[index].x[4] = UINT64_MAX;
    }

    assert_retired(memory, &cpus[0], encode_store(2, true, 4, 3, 1));
    assert_retired(memory, &cpus[1], encode_store(2, true, 4, 3, 1));
    assert(cpus[0].x[4] == 0 && cpus[1].x[4] == 1);
    assert(get_value(memory->data + offset, 4) == 1);
}

struct increment_context {
    struct guest_tlb tlb;
    struct aarch64_decoded load;
    struct aarch64_decoded store;
    atomic_bool *start;
    atomic_uint *first_loads;
    atomic_uint *failures;
    guest_addr_t address;
    unsigned participants;
    unsigned iterations;
};

static void *increment_exclusively(void *opaque) {
    struct increment_context *context = opaque;
    struct cpu_state cpu = {0};
    cpu.x[1] = context->address;
    while (!atomic_load_explicit(context->start, memory_order_acquire)) {
    }

    for (unsigned iteration = 0; iteration < context->iterations;
            iteration++) {
        unsigned attempts = 0;
        do {
            struct aarch64_execute_result result = aarch64_execute(
                    &cpu, &context->tlb, &context->load);
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
            if (iteration == 0 && attempts == 0) {
                atomic_fetch_add_explicit(context->first_loads, 1,
                        memory_order_release);
                while (atomic_load_explicit(context->first_loads,
                        memory_order_acquire) != context->participants)
                    sched_yield();
            }
            cpu.x[3] = (dword_t) cpu.x[2] + 1;
            result = aarch64_execute(
                    &cpu, &context->tlb, &context->store);
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
            attempts++;
            if (cpu.x[4] != 0) {
                atomic_fetch_add_explicit(context->failures, 1,
                        memory_order_relaxed);
                sched_yield();
            }
            assert(attempts < 100000);
        } while (cpu.x[4] != 0);
    }
    return NULL;
}

static void *increment_pair_exclusively(void *opaque) {
    struct increment_context *context = opaque;
    struct cpu_state cpu = {0};
    cpu.x[1] = context->address;
    while (!atomic_load_explicit(context->start, memory_order_acquire)) {
    }

    for (unsigned iteration = 0; iteration < context->iterations;
            iteration++) {
        unsigned attempts = 0;
        do {
            struct aarch64_execute_result result = aarch64_execute(
                    &cpu, &context->tlb, &context->load);
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
            assert(cpu.x[3] == ~cpu.x[2]);
            if (iteration == 0 && attempts == 0) {
                atomic_fetch_add_explicit(context->first_loads, 1,
                        memory_order_release);
                while (atomic_load_explicit(context->first_loads,
                        memory_order_acquire) != context->participants)
                    sched_yield();
            }
            cpu.x[4] = cpu.x[2] + 1;
            cpu.x[5] = ~cpu.x[4];
            result = aarch64_execute(
                    &cpu, &context->tlb, &context->store);
            assert(result.stop == AARCH64_EXECUTE_RETIRED);
            attempts++;
            if (cpu.x[6] != 0) {
                atomic_fetch_add_explicit(context->failures, 1,
                        memory_order_relaxed);
                sched_yield();
            }
            assert(attempts < 100000);
        } while (cpu.x[6] != 0);
    }
    return NULL;
}

static void test_concurrent_increment(struct test_memory *memory) {
    enum { thread_count = 2, iterations = 10000 };
    size_t offset = 704;
    byte_t zero[4] = {0};
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset,
            zero, sizeof(zero), &fault));

    atomic_bool start;
    atomic_uint first_loads;
    atomic_uint failures;
    atomic_init(&start, false);
    atomic_init(&first_loads, 0);
    atomic_init(&failures, 0);
    struct increment_context contexts[thread_count];
    pthread_t threads[thread_count];
    for (unsigned index = 0; index < thread_count; index++) {
        contexts[index] = (struct increment_context) {
            .load = decode(encode_load(2, true, 2, 1)),
            .store = decode(encode_store(2, true, 4, 3, 1)),
            .start = &start,
            .first_loads = &first_loads,
            .failures = &failures,
            .address = DATA_PAGE + offset,
            .participants = thread_count,
            .iterations = iterations,
        };
        guest_tlb_init(&contexts[index].tlb, &memory->space);
        assert(pthread_create(&threads[index], NULL,
                increment_exclusively, &contexts[index]) == 0);
    }

    atomic_store_explicit(&start, true, memory_order_release);
    for (unsigned index = 0; index < thread_count; index++)
        assert(pthread_join(threads[index], NULL) == 0);
    assert(get_value(memory->data + offset, 4) ==
            thread_count * iterations);
    assert(atomic_load_explicit(&failures, memory_order_relaxed) != 0);
}

static void test_concurrent_pair_increment(struct test_memory *memory) {
    enum { thread_count = 2, iterations = 5000 };
    size_t offset = 736;
    byte_t initial[16];
    put_value(initial, 0, 8);
    put_value(initial + 8, UINT64_MAX, 8);
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset,
            initial, sizeof(initial), &fault));

    atomic_bool start;
    atomic_uint first_loads;
    atomic_uint failures;
    atomic_init(&start, false);
    atomic_init(&first_loads, 0);
    atomic_init(&failures, 0);
    struct increment_context contexts[thread_count];
    pthread_t threads[thread_count];
    for (unsigned index = 0; index < thread_count; index++) {
        contexts[index] = (struct increment_context) {
            .load = decode(encode_pair_load(true, true, 2, 3, 1)),
            .store = decode(encode_pair_store(
                    true, true, 6, 4, 5, 1)),
            .start = &start,
            .first_loads = &first_loads,
            .failures = &failures,
            .address = DATA_PAGE + offset,
            .participants = thread_count,
            .iterations = iterations,
        };
        guest_tlb_init(&contexts[index].tlb, &memory->space);
        assert(pthread_create(&threads[index], NULL,
                increment_pair_exclusively, &contexts[index]) == 0);
    }

    atomic_store_explicit(&start, true, memory_order_release);
    for (unsigned index = 0; index < thread_count; index++)
        assert(pthread_join(threads[index], NULL) == 0);
    byte_t final[16];
    assert(guest_tlb_read(&memory->tlb, DATA_PAGE + offset,
            final, sizeof(final), GUEST_MEMORY_READ, &fault));
    qword_t total = thread_count * iterations;
    assert(get_value(final, 8) == total);
    assert(get_value(final + 8, 8) == ~total);
    assert(atomic_load_explicit(&failures, memory_order_relaxed) != 0);
}

static void test_barrier_and_clrex(struct test_memory *memory) {
    size_t offset = 640;
    put_value(memory->data + offset, 0, 4);
    struct cpu_state cpu = {.pc = UINT64_C(0x2800)};
    cpu.x[1] = DATA_PAGE + offset;
    cpu.x[2] = UINT32_C(0x11223344);
    assert_retired(memory, &cpu, encode_load(2, true, 3, 1));
    assert_retired(memory, &cpu, UINT32_C(0xd5033bbf));
    assert(cpu.exclusive.valid);
    assert_retired(memory, &cpu, encode_store(2, true, 4, 2, 1));
    assert(cpu.x[4] == 0);

    assert_retired(memory, &cpu, encode_load(2, false, 3, 1));
    assert_retired(memory, &cpu, UINT32_C(0xd5033f5f));
    assert(!cpu.exclusive.valid);
    assert_retired(memory, &cpu, encode_store(2, false, 4, 2, 1));
    assert(cpu.x[4] == 1);
}

static void test_faults(struct test_memory *memory) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x3000),
        .x[1] = UNMAPPED_PAGE,
        .x[2] = UINT64_C(0x1122334455667788),
    };
    struct aarch64_execute_result result = execute_word(
            memory, &cpu, encode_load(2, false, 2, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[2] == UINT64_C(0x1122334455667788));
    assert(cpu.pc == UINT64_C(0x3000));

    cpu.x[1] = DATA_PAGE + 3;
    result = execute_word(memory, &cpu, encode_load(2, false, 2, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ALIGNMENT);
    assert(result.fault.address == DATA_PAGE + 3);
    assert(result.fault.access == GUEST_MEMORY_READ);

    cpu.x[1] = DATA_PAGE + 3;
    cpu.x[3] = UINT32_C(0xaabbccdd);
    cpu.x[4] = UINT64_C(0x8877665544332211);
    result = execute_word(memory, &cpu,
            encode_store(2, false, 4, 3, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ALIGNMENT);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(cpu.x[4] == UINT64_C(0x8877665544332211));

    size_t offset = 128;
    put_value(memory->readonly + offset, UINT32_C(0x12345678), 4);
    cpu.x[1] = READONLY_PAGE + offset;
    assert_retired(memory, &cpu, encode_load(2, true, 2, 1));
    cpu.x[3] = UINT32_C(0x87654321);
    cpu.x[4] = UINT64_C(0xaaaaaaaa55555555);
    qword_t store_pc = cpu.pc;
    result = execute_word(memory, &cpu,
            encode_store(2, true, 4, 3, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == READONLY_PAGE + offset);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(cpu.x[4] == UINT64_C(0xaaaaaaaa55555555));
    assert(cpu.pc == store_pc);
    assert(!cpu.exclusive.valid);
    assert(get_value(memory->readonly + offset, 4) == UINT32_C(0x12345678));

    cpu.x[1] = UINT64_C(1) << 48;
    result = execute_word(memory, &cpu, encode_load(3, false, 2, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
}

static void test_musl_lock_sequence(struct test_memory *memory) {
    size_t offset = 896;
    put_value(memory->data + offset, 0, 4);
    struct cpu_state cpu = {
        .pc = UINT64_C(0x3800),
        .x[8] = 1,
        .x[9] = DATA_PAGE + offset,
    };
    assert_retired(memory, &cpu, UINT32_C(0x885ffd2a));
    assert(cpu.x[10] == 0);
    assert_retired(memory, &cpu, UINT32_C(0x880afd28));
    assert(cpu.x[10] == 0);
    assert(get_value(memory->data + offset, 4) == 1);
    assert_retired(memory, &cpu, UINT32_C(0xd5033bbf));
}

static void test_pair_round_trip(struct test_memory *memory) {
    for (unsigned wide = 0; wide < 2; wide++) {
        byte_t element_size = wide ? 8 : 4;
        byte_t pair_size = element_size * 2;
        size_t offset = 1024 + wide * 32;
        qword_t original_low = wide ?
                UINT64_C(0x0123456789abcdef) : UINT32_C(0x89abcdef);
        qword_t original_high = wide ?
                UINT64_C(0xfedcba9876543210) : UINT32_C(0x76543210);
        qword_t replacement_low = wide ?
                UINT64_C(0x1122334455667788) : UINT32_C(0x55667788);
        qword_t replacement_high = wide ?
                UINT64_C(0x8877665544332211) : UINT32_C(0x44332211);
        put_value(memory->data + offset, original_low, element_size);
        put_value(memory->data + offset + element_size,
                original_high, element_size);

        struct cpu_state cpu = {
            .pc = UINT64_C(0x4000),
            .nzcv = UINT32_C(0xa0000000),
        };
        cpu.x[1] = DATA_PAGE + offset;
        cpu.x[2] = UINT64_MAX;
        cpu.x[3] = UINT64_MAX;
        assert_retired(memory, &cpu, encode_pair_load(
                wide != 0, wide != 0, 2, 3, 1));
        assert(cpu.x[2] == original_low);
        assert(cpu.x[3] == original_high);
        assert(cpu.exclusive.valid);
        assert(cpu.exclusive.address == DATA_PAGE + offset);
        assert(cpu.exclusive.size == pair_size);
        assert(cpu.exclusive.pair);
        assert(cpu.exclusive.value_low == original_low);
        assert(cpu.exclusive.value_high == original_high);

        cpu.x[4] = replacement_low;
        cpu.x[5] = replacement_high;
        cpu.x[6] = UINT64_MAX;
        assert_retired(memory, &cpu, encode_pair_store(
                wide != 0, true, 6, 4, 5, 1));
        assert(cpu.x[6] == 0);
        assert(get_value(memory->data + offset, element_size) ==
                replacement_low);
        assert(get_value(memory->data + offset + element_size,
                element_size) == replacement_high);
        assert(!cpu.exclusive.valid);
        assert(cpu.nzcv == UINT32_C(0xa0000000));

        cpu.x[6] = UINT64_MAX;
        assert_retired(memory, &cpu, encode_pair_store(
                wide != 0, false, 6, 4, 5, 1));
        assert(cpu.x[6] == 1);
    }
}

static void test_pair_sp_and_monitor(struct test_memory *memory) {
    size_t offset = 1088;
    put_value(memory->data + offset,
            UINT64_C(0x1020304050607080), 8);
    put_value(memory->data + offset + 8,
            UINT64_C(0x90a0b0c0d0e0f000), 8);
    struct cpu_state cpu = {
        .pc = UINT64_C(0x4800),
        .sp = DATA_PAGE + offset,
    };
    assert_retired(memory, &cpu, UINT32_C(0xc87fabe9));
    assert(cpu.x[9] == UINT64_C(0x1020304050607080));
    assert(cpu.x[10] == UINT64_C(0x90a0b0c0d0e0f000));

    byte_t external[8];
    put_value(external, UINT64_C(0xaabbccddeeff0011), sizeof(external));
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset + 8,
            external, sizeof(external), &fault));
    cpu.x[24] = UINT64_C(0x1111111111111111);
    cpu.x[25] = UINT64_C(0x2222222222222222);
    cpu.x[23] = UINT64_MAX;
    assert_retired(memory, &cpu, UINT32_C(0xc837e7f8));
    assert(cpu.x[23] == 1);
    assert(get_value(memory->data + offset, 8) ==
            UINT64_C(0x1020304050607080));
    assert(get_value(memory->data + offset + 8, 8) ==
            UINT64_C(0xaabbccddeeff0011));
}

static void test_pair_zero_register(struct test_memory *memory) {
    size_t offset = 1120;
    put_value(memory->data + offset,
            UINT64_C(0x0102030405060708), 8);
    put_value(memory->data + offset + 8,
            UINT64_C(0x1112131415161718), 8);
    struct cpu_state cpu = {
        .pc = UINT64_C(0x4c00),
        .sp = DATA_PAGE + offset,
    };

    assert_retired(memory, &cpu,
            encode_pair_load(true, false, 31, 0, 31));
    assert(cpu.x[0] == UINT64_C(0x1112131415161718));
    cpu.x[1] = UINT64_C(0x2122232425262728);
    cpu.x[2] = UINT64_MAX;
    assert_retired(memory, &cpu,
            encode_pair_store(true, false, 2, 31, 1, 31));
    assert(cpu.x[2] == 0);
    assert(get_value(memory->data + offset, 8) == 0);
    assert(get_value(memory->data + offset + 8, 8) ==
            UINT64_C(0x2122232425262728));

    assert_retired(memory, &cpu,
            encode_pair_load(true, false, 3, 4, 31));
    cpu.x[5] = UINT64_C(0x3132333435363738);
    cpu.x[6] = UINT64_C(0x4142434445464748);
    assert_retired(memory, &cpu,
            encode_pair_store(true, false, 31, 5, 6, 31));
    assert(get_value(memory->data + offset, 8) ==
            UINT64_C(0x3132333435363738));
    assert(get_value(memory->data + offset + 8, 8) ==
            UINT64_C(0x4142434445464748));
}

static void test_pair_reservation_has_one_winner(
        struct test_memory *memory) {
    size_t offset = 1152;
    put_value(memory->data + offset, 0, 8);
    put_value(memory->data + offset + 8, 0, 8);
    struct cpu_state cpus[2] = {
        {.pc = UINT64_C(0x5000)},
        {.pc = UINT64_C(0x5000)},
    };
    for (unsigned index = 0; index < array_size(cpus); index++) {
        cpus[index].x[1] = DATA_PAGE + offset;
        assert_retired(memory, &cpus[index],
                encode_pair_load(true, true, 2, 3, 1));
        cpus[index].x[4] = index + 1;
        cpus[index].x[5] = index + 11;
        cpus[index].x[6] = UINT64_MAX;
    }
    assert_retired(memory, &cpus[0],
            encode_pair_store(true, true, 6, 4, 5, 1));
    assert_retired(memory, &cpus[1],
            encode_pair_store(true, true, 6, 4, 5, 1));
    assert(cpus[0].x[6] == 0 && cpus[1].x[6] == 1);
    assert(get_value(memory->data + offset, 8) == 1);
    assert(get_value(memory->data + offset + 8, 8) == 11);
}

static void test_mixed_reservation_shape_fails(struct test_memory *memory) {
    size_t offset = 1184;
    qword_t original = UINT64_C(0x0000000076543210);
    put_value(memory->data + offset, original, 8);
    struct cpu_state cpu = {.pc = UINT64_C(0x5400)};
    cpu.x[1] = DATA_PAGE + offset;

    assert_retired(memory, &cpu,
            encode_pair_load(false, false, 2, 3, 1));
    cpu.x[4] = UINT64_C(0x1111111122222222);
    assert_retired(memory, &cpu, encode_store(3, false, 5, 4, 1));
    assert(cpu.x[5] == 1);
    assert(get_value(memory->data + offset, 8) == original);

    put_value(memory->data + offset, original, 8);
    assert_retired(memory, &cpu, encode_load(3, false, 2, 1));
    cpu.x[3] = UINT32_C(0x33333333);
    cpu.x[4] = UINT32_C(0x44444444);
    assert_retired(memory, &cpu,
            encode_pair_store(false, false, 5, 3, 4, 1));
    assert(cpu.x[5] == 1);
    assert(get_value(memory->data + offset, 8) == original);
}

static void test_pair_reservation_granule(struct test_memory *memory) {
    size_t offset = 1216;
    put_value(memory->data + offset, UINT32_C(0x11111111), 4);
    put_value(memory->data + offset + 4, UINT32_C(0x22222222), 4);
    struct cpu_state cpu = {.pc = UINT64_C(0x5600)};
    cpu.x[1] = DATA_PAGE + offset;
    assert_retired(memory, &cpu,
            encode_pair_load(false, false, 2, 3, 1));

    byte_t external[4];
    put_value(external, UINT32_C(0x33333333), sizeof(external));
    struct guest_memory_fault fault;
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset + 8,
            external, sizeof(external), &fault));
    cpu.x[4] = UINT32_C(0x44444444);
    cpu.x[5] = UINT32_C(0x55555555);
    assert_retired(memory, &cpu,
            encode_pair_store(false, false, 6, 4, 5, 1));
    assert(cpu.x[6] == 1);
    assert(get_value(memory->data + offset, 4) == UINT32_C(0x11111111));
    assert(get_value(memory->data + offset + 4, 4) ==
            UINT32_C(0x22222222));

    assert_retired(memory, &cpu,
            encode_pair_load(false, false, 2, 3, 1));
    assert(guest_tlb_write(&memory->tlb, DATA_PAGE + offset + 16,
            external, sizeof(external), &fault));
    assert_retired(memory, &cpu,
            encode_pair_store(false, false, 6, 4, 5, 1));
    assert(cpu.x[6] == 0);
    assert(get_value(memory->data + offset, 4) == UINT32_C(0x44444444));
    assert(get_value(memory->data + offset + 4, 4) ==
            UINT32_C(0x55555555));
}

static void test_pair_faults(struct test_memory *memory) {
    struct cpu_state cpu = {
        .pc = UINT64_C(0x5800),
        .x[1] = DATA_PAGE + 8,
        .x[2] = UINT64_C(0x1111111111111111),
        .x[3] = UINT64_C(0x2222222222222222),
    };
    struct aarch64_execute_result result = execute_word(memory, &cpu,
            encode_pair_load(true, false, 2, 3, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ALIGNMENT);
    assert(result.fault.address == DATA_PAGE + 8);
    assert(result.fault.access == GUEST_MEMORY_READ);
    assert(cpu.x[2] == UINT64_C(0x1111111111111111));
    assert(cpu.x[3] == UINT64_C(0x2222222222222222));
    assert(cpu.pc == UINT64_C(0x5800));

    cpu.x[1] = UNMAPPED_PAGE;
    result = execute_word(memory, &cpu,
            encode_pair_load(true, false, 2, 3, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_UNMAPPED);
    assert(result.fault.address == UNMAPPED_PAGE);
    assert(result.fault.access == GUEST_MEMORY_READ);

    cpu.x[1] = UINT64_C(1) << 48;
    result = execute_word(memory, &cpu,
            encode_pair_load(true, false, 2, 3, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_ADDRESS_SIZE);
    assert(result.fault.address == (UINT64_C(1) << 48));
    assert(result.fault.access == GUEST_MEMORY_READ);

    size_t offset = 128;
    put_value(memory->readonly + offset, UINT64_C(0x1234), 8);
    put_value(memory->readonly + offset + 8, UINT64_C(0x5678), 8);
    cpu.x[1] = READONLY_PAGE + offset;
    assert_retired(memory, &cpu,
            encode_pair_load(true, true, 2, 3, 1));
    cpu.x[4] = UINT64_C(0xaabbccdd);
    cpu.x[5] = UINT64_C(0xeeff0011);
    cpu.x[6] = UINT64_MAX;
    qword_t store_pc = cpu.pc;
    result = execute_word(memory, &cpu,
            encode_pair_store(true, true, 6, 4, 5, 1));
    assert(result.stop == AARCH64_EXECUTE_DATA_FAULT);
    assert(result.fault.kind == GUEST_MEMORY_FAULT_PERMISSION);
    assert(result.fault.address == READONLY_PAGE + offset);
    assert(result.fault.access == GUEST_MEMORY_WRITE);
    assert(cpu.x[6] == UINT64_MAX);
    assert(cpu.pc == store_pc);
    assert(!cpu.exclusive.valid);
    assert(get_value(memory->readonly + offset, 8) == UINT64_C(0x1234));
    assert(get_value(memory->readonly + offset + 8, 8) == UINT64_C(0x5678));
}

int main(void) {
    test_apple_vectors();
    test_pair_apple_vectors();
    test_encoding_space();
    test_pair_encoding_space();

    struct test_memory memory;
    init_test_memory(&memory);
    test_all_sizes(&memory);
    test_zero_register_and_sp(&memory);
    test_monitor_failures(&memory);
    test_aba_invalidates_reservation(&memory);
    test_same_granule_write_invalidates_reservation(&memory);
    test_unrelated_store_preserves_reservation(&memory);
    test_exact_granule_tracking(&memory);
    test_two_reservations_have_one_winner(&memory);
    test_concurrent_increment(&memory);
    test_concurrent_pair_increment(&memory);
    test_barrier_and_clrex(&memory);
    test_faults(&memory);
    test_musl_lock_sequence(&memory);
    test_pair_round_trip(&memory);
    test_pair_sp_and_monitor(&memory);
    test_pair_zero_register(&memory);
    test_pair_reservation_has_one_winner(&memory);
    test_mixed_reservation_shape_fails(&memory);
    test_pair_reservation_granule(&memory);
    test_pair_faults(&memory);
    destroy_test_memory(&memory);
    return 0;
}

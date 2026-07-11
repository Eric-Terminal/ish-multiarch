#include <assert.h>
#include <string.h>

#include "guest/aarch64/linux-stack.h"
#include "guest/linux/auxv.h"
#include "guest/memory/tlb.h"

#define STACK_TOP UINT64_C(0x00007fff00000000)
#define STACK_SIZE (2 * GUEST_MEMORY_PAGE_SIZE)
#define LOAD_BIAS UINT64_C(0x0000400000000000)
#define INTERPRETER_BASE UINT64_C(0x0000500000000000)

static qword_t read_qword(struct guest_tlb *tlb, guest_addr_t address) {
    byte_t bytes[8];
    struct guest_memory_fault fault;
    assert(guest_tlb_read(tlb, address, bytes, sizeof(bytes),
            GUEST_MEMORY_READ, &fault));
    qword_t value = 0;
    for (byte_t i = 0; i < 8; i++)
        value |= (qword_t) bytes[i] << (i * 8);
    return value;
}

static void assert_string(struct guest_tlb *tlb, guest_addr_t address,
        const char *expected) {
    struct guest_memory_fault fault;
    for (size_t i = 0;; i++) {
        char value;
        assert(guest_tlb_read(tlb, address + i, &value, 1,
                GUEST_MEMORY_READ, &fault));
        assert(value == expected[i]);
        if (value == '\0')
            break;
    }
}

int main(void) {
    struct guest_page_table table;
    assert(guest_page_table_init(&table, 48));
    const char *arguments[] = {"demo", "one"};
    const char *environment[] = {"A=B", "C=D"};
    byte_t random[AARCH64_LINUX_RANDOM_SIZE];
    for (byte_t i = 0; i < sizeof(random); i++)
        random[i] = i;
    struct aarch64_elf64_load_result loaded = {
        .load_bias = LOAD_BIAS,
        .entry = LOAD_BIAS + UINT64_C(0x100),
        .program_headers = LOAD_BIAS + UINT64_C(0x40),
        .program_header_count = 3,
    };
    struct aarch64_linux_stack_config config = {
        .stack_top = STACK_TOP,
        .stack_size = STACK_SIZE,
        .executable = "/bin/demo",
        .arguments = arguments,
        .argument_count = array_size(arguments),
        .environment = environment,
        .environment_count = array_size(environment),
        .random = random,
        .uid = 1000,
        .euid = 1001,
        .gid = 1002,
        .egid = 1003,
        .interpreter_base = INTERPRETER_BASE,
    };
    struct aarch64_linux_stack_result stack;
    assert(aarch64_linux_build_initial_stack(&table, &loaded,
            &config, &stack) == AARCH64_LINUX_STACK_OK);
    assert((stack.stack_pointer & 0xf) == 0);
    assert(stack.stack_pointer >= STACK_TOP - STACK_SIZE);

    struct guest_tlb tlb;
    guest_tlb_init(&tlb, &table.address_space);
    guest_addr_t cursor = stack.stack_pointer;
    assert(read_qword(&tlb, cursor) == 2);
    cursor += 8;
    assert(cursor == stack.argv);
    guest_addr_t argv0 = read_qword(&tlb, cursor);
    guest_addr_t argv1 = read_qword(&tlb, cursor + 8);
    assert_string(&tlb, argv0, "demo");
    assert_string(&tlb, argv1, "one");
    assert(read_qword(&tlb, cursor + 16) == 0);
    cursor += 24;
    assert(cursor == stack.environment);
    guest_addr_t env0 = read_qword(&tlb, cursor);
    guest_addr_t env1 = read_qword(&tlb, cursor + 8);
    assert_string(&tlb, env0, "A=B");
    assert_string(&tlb, env1, "C=D");
    assert(read_qword(&tlb, cursor + 16) == 0);
    cursor += 24;
    assert(cursor == stack.auxv_start);

    guest_addr_t platform = 0;
    guest_addr_t random_address = 0;
    guest_addr_t executable = 0;
    bool saw_phdr = false;
    bool saw_entry = false;
    const qword_t expected_auxv_types[] = {
        GUEST_AT_PHDR, GUEST_AT_PHENT, GUEST_AT_PHNUM, GUEST_AT_PAGESZ,
        GUEST_AT_BASE, GUEST_AT_FLAGS, GUEST_AT_ENTRY, GUEST_AT_UID,
        GUEST_AT_EUID, GUEST_AT_GID, GUEST_AT_EGID, GUEST_AT_PLATFORM,
        GUEST_AT_HWCAP, GUEST_AT_CLKTCK, GUEST_AT_SECURE, GUEST_AT_RANDOM,
        GUEST_AT_HWCAP2, GUEST_AT_EXECFN, GUEST_AT_NULL,
    };
    size_t auxv_index = 0;
    while (true) {
        qword_t type = read_qword(&tlb, cursor);
        qword_t value = read_qword(&tlb, cursor + 8);
        cursor += 16;
        assert(auxv_index < array_size(expected_auxv_types));
        assert(type == expected_auxv_types[auxv_index++]);
        if (type == GUEST_AT_NULL) {
            assert(value == 0);
            break;
        }
        if (type == GUEST_AT_PHDR) {
            assert(value == loaded.program_headers);
            saw_phdr = true;
        } else if (type == GUEST_AT_PHENT) {
            assert(value == AARCH64_ELF64_PROGRAM_HEADER_SIZE);
        } else if (type == GUEST_AT_PHNUM) {
            assert(value == loaded.program_header_count);
        } else if (type == GUEST_AT_PAGESZ) {
            assert(value == GUEST_MEMORY_PAGE_SIZE);
        } else if (type == GUEST_AT_BASE) {
            assert(value == INTERPRETER_BASE);
            assert(value != loaded.load_bias);
        } else if (type == GUEST_AT_ENTRY) {
            assert(value == loaded.entry);
            saw_entry = true;
        } else if (type == GUEST_AT_UID) {
            assert(value == config.uid);
        } else if (type == GUEST_AT_EUID) {
            assert(value == config.euid);
        } else if (type == GUEST_AT_GID) {
            assert(value == config.gid);
        } else if (type == GUEST_AT_EGID) {
            assert(value == config.egid);
        } else if (type == GUEST_AT_PLATFORM) {
            platform = value;
        } else if (type == GUEST_AT_RANDOM) {
            random_address = value;
        } else if (type == GUEST_AT_EXECFN) {
            executable = value;
        } else if (type == GUEST_AT_CLKTCK) {
            assert(value == 100);
        } else {
            assert(value == 0);
        }
    }
    assert(cursor == stack.auxv_end);
    assert(auxv_index == array_size(expected_auxv_types));
    assert(saw_phdr && saw_entry);
    assert_string(&tlb, platform, "aarch64");
    assert_string(&tlb, executable, "/bin/demo");
    byte_t random_copy[AARCH64_LINUX_RANDOM_SIZE];
    struct guest_memory_fault fault;
    assert(guest_tlb_read(&tlb, random_address, random_copy,
            sizeof(random_copy), GUEST_MEMORY_READ, &fault));
    assert(memcmp(random_copy, random, sizeof(random)) == 0);

    bool poked = true;
    struct cpu_state cpu = {
        .mmu = (struct mmu *) (uintptr_t) 0x1234,
        .cycle = 99,
        .x = {[0] = UINT64_MAX, [30] = UINT64_MAX},
        .sp = UINT64_MAX,
        .pc = UINT64_MAX,
        .nzcv = AARCH64_NZCV_MASK,
        .fpcr = UINT32_MAX,
        .fpsr = UINT32_MAX,
        .tpidr_el0 = UINT64_MAX,
        .exclusive = {.valid = true},
        .poked_ptr = &poked,
    };
    aarch64_linux_prepare_cpu(&cpu, &loaded, &stack);
    assert(cpu.mmu == (struct mmu *) (uintptr_t) 0x1234);
    assert(cpu.poked_ptr == &poked);
    assert(cpu.pc == loaded.entry);
    assert(cpu.sp == stack.stack_pointer);
    assert(cpu.cycle == 0 && cpu.nzcv == 0 && cpu.tpidr_el0 == 0);
    assert(cpu.x[0] == 0 && cpu.x[30] == 0);
    assert(!cpu.exclusive.valid);

    aarch64_linux_prepare_cpu_at(&cpu,
            INTERPRETER_BASE + 0x100, &stack);
    assert(cpu.mmu == (struct mmu *) (uintptr_t) 0x1234);
    assert(cpu.poked_ptr == &poked);
    assert(cpu.pc == INTERPRETER_BASE + 0x100);
    assert(cpu.sp == stack.stack_pointer && cpu.cycle == 0);

    guest_page_table_destroy(&table);

    struct guest_page_table invalid;
    assert(guest_page_table_init(&invalid, 48));
    config.stack_top++;
    assert(aarch64_linux_build_initial_stack(&invalid, &loaded,
            &config, &stack) == AARCH64_LINUX_STACK_INVALID_ARGUMENT);
    guest_page_table_destroy(&invalid);

    struct guest_page_table overflow;
    assert(guest_page_table_init(&overflow, 48));
    char huge_executable[STACK_SIZE + 1];
    memset(huge_executable, 'x', sizeof(huge_executable) - 1);
    huge_executable[sizeof(huge_executable) - 1] = '\0';
    config.stack_top = STACK_TOP;
    config.executable = huge_executable;
    assert(aarch64_linux_build_initial_stack(&overflow, &loaded,
            &config, &stack) == AARCH64_LINUX_STACK_OVERFLOW);
    guest_page_table_destroy(&overflow);

    struct guest_page_table conflict;
    assert(guest_page_table_init(&conflict, 48));
    byte_t *host_page;
    assert(guest_page_table_map(&conflict, STACK_TOP - STACK_SIZE,
            GUEST_MEMORY_READ, &host_page) == GUEST_PAGE_TABLE_OK);
    config.executable = "/bin/demo";
    assert(aarch64_linux_build_initial_stack(&conflict, &loaded,
            &config, &stack) == AARCH64_LINUX_STACK_MAPPING_CONFLICT);
    guest_page_table_destroy(&conflict);
    return 0;
}

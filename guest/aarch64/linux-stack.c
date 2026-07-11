#include <stdlib.h>
#include <string.h>

#include "guest/aarch64/linux-stack.h"
#include "guest/linux/auxv.h"

#define AARCH64_LINUX_PLATFORM "aarch64"

struct auxv_entry {
    qword_t type;
    qword_t value;
};
_Static_assert(sizeof(struct auxv_entry) == 16,
        "AArch64 auxv 条目必须为两个 64 位 guest word");

static bool reserve_down(guest_addr_t *cursor, guest_addr_t bottom,
        size_t size, guest_addr_t *address) {
    if ((qword_t) size > *cursor - bottom)
        return false;
    *cursor -= (guest_addr_t) size;
    *address = *cursor;
    return true;
}

static enum aarch64_linux_stack_error map_stack(
        struct guest_page_table *table, guest_addr_t bottom,
        guest_addr_t top) {
    for (guest_addr_t page = bottom; page < top;
            page += GUEST_MEMORY_PAGE_SIZE) {
        byte_t *host_page;
        enum guest_page_table_result map_result = guest_page_table_map(
                table, page, GUEST_MEMORY_READ | GUEST_MEMORY_WRITE,
                &host_page);
        if (map_result == GUEST_PAGE_TABLE_OUT_OF_MEMORY)
            return AARCH64_LINUX_STACK_OUT_OF_MEMORY;
        if (map_result == GUEST_PAGE_TABLE_ALREADY_MAPPED)
            return AARCH64_LINUX_STACK_MAPPING_CONFLICT;
        if (map_result != GUEST_PAGE_TABLE_OK)
            return AARCH64_LINUX_STACK_INVALID_ARGUMENT;
    }
    return AARCH64_LINUX_STACK_OK;
}

static bool write_bytes(struct guest_page_table *table,
        guest_addr_t address, const void *source, size_t size) {
    const byte_t *input = source;
    size_t remaining = size;
    while (remaining != 0) {
        guest_addr_t page_base = address & ~GUEST_MEMORY_PAGE_MASK;
        size_t page_offset = (size_t) (address & GUEST_MEMORY_PAGE_MASK);
        size_t available = (size_t) GUEST_MEMORY_PAGE_SIZE - page_offset;
        size_t chunk = remaining < available ? remaining : available;
        byte_t *host_page;
        unsigned permissions;
        if (guest_page_table_lookup(table, page_base,
                &host_page, &permissions) != GUEST_PAGE_TABLE_OK)
            return false;
        memcpy(host_page + page_offset, input, chunk);
        address += (guest_addr_t) chunk;
        input += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool write_qword(struct guest_page_table *table,
        guest_addr_t address, qword_t value) {
    byte_t bytes[8];
    for (byte_t i = 0; i < 8; i++)
        bytes[i] = (byte_t) (value >> (i * 8));
    return write_bytes(table, address, bytes, sizeof(bytes));
}

static bool write_pointer_table(struct guest_page_table *table,
        guest_addr_t *cursor, const guest_addr_t *addresses, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!write_qword(table, *cursor, addresses[i]))
            return false;
        *cursor += 8;
    }
    if (!write_qword(table, *cursor, 0))
        return false;
    *cursor += 8;
    return true;
}

static bool write_auxv(struct guest_page_table *table, guest_addr_t *cursor,
        const struct auxv_entry *auxv, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!write_qword(table, *cursor, auxv[i].type) ||
                !write_qword(table, *cursor + 8, auxv[i].value))
            return false;
        *cursor += 16;
    }
    return true;
}

enum aarch64_linux_stack_error aarch64_linux_build_initial_stack(
        struct guest_page_table *table,
        const struct aarch64_elf64_load_result *loaded,
        const struct aarch64_linux_stack_config *config,
        struct aarch64_linux_stack_result *result) {
    if (config->stack_size == 0 ||
            (config->stack_size & GUEST_MEMORY_PAGE_MASK) != 0 ||
            (config->stack_top & GUEST_MEMORY_PAGE_MASK) != 0 ||
            (qword_t) config->stack_size > config->stack_top ||
            config->executable == NULL || config->random == NULL ||
            (config->argument_count != 0 && config->arguments == NULL) ||
            (config->environment_count != 0 && config->environment == NULL))
        return AARCH64_LINUX_STACK_INVALID_ARGUMENT;
    guest_addr_t stack_bottom =
            config->stack_top - (guest_addr_t) config->stack_size;
    if (!guest_address_space_contains(&table->address_space,
            stack_bottom, config->stack_size))
        return AARCH64_LINUX_STACK_INVALID_ARGUMENT;

    guest_addr_t *argument_addresses = config->argument_count == 0 ? NULL :
            calloc(config->argument_count, sizeof(*argument_addresses));
    guest_addr_t *environment_addresses = config->environment_count == 0 ? NULL :
            calloc(config->environment_count, sizeof(*environment_addresses));
    if ((config->argument_count != 0 && argument_addresses == NULL) ||
            (config->environment_count != 0 && environment_addresses == NULL)) {
        free(argument_addresses);
        free(environment_addresses);
        return AARCH64_LINUX_STACK_OUT_OF_MEMORY;
    }

    enum aarch64_linux_stack_error error = AARCH64_LINUX_STACK_OVERFLOW;
    guest_addr_t cursor = config->stack_top;
    guest_addr_t executable_address;
    guest_addr_t platform_address;
    guest_addr_t random_address;
    if (!reserve_down(&cursor, stack_bottom, strlen(config->executable) + 1,
            &executable_address))
        goto out;
    for (size_t i = config->environment_count; i-- > 0;) {
        if (!reserve_down(&cursor, stack_bottom,
                strlen(config->environment[i]) + 1,
                &environment_addresses[i]))
            goto out;
    }
    for (size_t i = config->argument_count; i-- > 0;) {
        if (!reserve_down(&cursor, stack_bottom,
                strlen(config->arguments[i]) + 1,
                &argument_addresses[i]))
            goto out;
    }
    if (!reserve_down(&cursor, stack_bottom, sizeof(AARCH64_LINUX_PLATFORM),
            &platform_address) ||
            !reserve_down(&cursor, stack_bottom, AARCH64_LINUX_RANDOM_SIZE,
            &random_address))
        goto out;

    const struct auxv_entry auxv[] = {
        {GUEST_AT_PHDR, loaded->program_headers},
        {GUEST_AT_PHENT, AARCH64_ELF64_PROGRAM_HEADER_SIZE},
        {GUEST_AT_PHNUM, loaded->program_header_count},
        {GUEST_AT_PAGESZ, GUEST_MEMORY_PAGE_SIZE},
        {GUEST_AT_BASE, config->interpreter_base},
        {GUEST_AT_FLAGS, 0},
        {GUEST_AT_ENTRY, loaded->entry},
        {GUEST_AT_UID, config->uid},
        {GUEST_AT_EUID, config->euid},
        {GUEST_AT_GID, config->gid},
        {GUEST_AT_EGID, config->egid},
        {GUEST_AT_PLATFORM, platform_address},
        {GUEST_AT_HWCAP, 0},
        {GUEST_AT_CLKTCK, 100},
        {GUEST_AT_SECURE, 0},
        {GUEST_AT_RANDOM, random_address},
        {GUEST_AT_HWCAP2, 0},
        {GUEST_AT_EXECFN, executable_address},
        {GUEST_AT_NULL, 0},
    };
    qword_t table_words = 1 + (qword_t) config->argument_count + 1 +
            (qword_t) config->environment_count + 1;
    qword_t table_size = table_words * 8 + sizeof(auxv);
    if (table_size > cursor - stack_bottom)
        goto out;
    guest_addr_t stack_pointer = (cursor - table_size) & ~UINT64_C(0xf);
    if (stack_pointer < stack_bottom)
        goto out;

    error = map_stack(table, stack_bottom, config->stack_top);
    if (error != AARCH64_LINUX_STACK_OK)
        goto out;
    if (!write_bytes(table, executable_address, config->executable,
            strlen(config->executable) + 1))
        goto mapping_error;
    for (size_t i = 0; i < config->environment_count; i++) {
        if (!write_bytes(table, environment_addresses[i], config->environment[i],
                strlen(config->environment[i]) + 1))
            goto mapping_error;
    }
    for (size_t i = 0; i < config->argument_count; i++) {
        if (!write_bytes(table, argument_addresses[i], config->arguments[i],
                strlen(config->arguments[i]) + 1))
            goto mapping_error;
    }
    if (!write_bytes(table, platform_address, AARCH64_LINUX_PLATFORM,
            sizeof(AARCH64_LINUX_PLATFORM)) ||
            !write_bytes(table, random_address, config->random,
            AARCH64_LINUX_RANDOM_SIZE))
        goto mapping_error;

    guest_addr_t output = stack_pointer;
    if (!write_qword(table, output, config->argument_count))
        goto mapping_error;
    output += 8;
    guest_addr_t argv = output;
    if (!write_pointer_table(table, &output, argument_addresses,
            config->argument_count))
        goto mapping_error;
    guest_addr_t environment = output;
    if (!write_pointer_table(table, &output, environment_addresses,
            config->environment_count))
        goto mapping_error;
    guest_addr_t auxv_start = output;
    if (!write_auxv(table, &output, auxv, array_size(auxv)))
        goto mapping_error;

    *result = (struct aarch64_linux_stack_result) {
        .stack_pointer = stack_pointer,
        .argv = argv,
        .environment = environment,
        .auxv_start = auxv_start,
        .auxv_end = output,
    };
    error = AARCH64_LINUX_STACK_OK;
    goto out;

mapping_error:
    error = AARCH64_LINUX_STACK_MAPPING_CONFLICT;
out:
    free(argument_addresses);
    free(environment_addresses);
    return error;
}

void aarch64_linux_prepare_cpu(struct cpu_state *cpu,
        const struct aarch64_elf64_load_result *loaded,
        const struct aarch64_linux_stack_result *stack) {
    aarch64_linux_prepare_cpu_at(cpu, loaded->entry, stack);
}

void aarch64_linux_prepare_cpu_at(struct cpu_state *cpu,
        guest_addr_t initial_pc,
        const struct aarch64_linux_stack_result *stack) {
    struct mmu *mmu = cpu->mmu;
    bool *poked_ptr = cpu->poked_ptr;
    *cpu = (struct cpu_state) {
        .mmu = mmu,
        .sp = stack->stack_pointer,
        .pc = initial_pc,
        .poked_ptr = poked_ptr,
    };
}

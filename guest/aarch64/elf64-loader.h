#ifndef GUEST_AARCH64_ELF64_LOADER_H
#define GUEST_AARCH64_ELF64_LOADER_H

#include "guest/aarch64/elf64.h"
#include "guest/memory/page-table.h"

enum aarch64_elf64_load_error {
    AARCH64_ELF64_LOAD_OK,
    AARCH64_ELF64_LOAD_OUT_OF_MEMORY,
    AARCH64_ELF64_LOAD_MAPPING_CONFLICT,
    AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT,
};

struct aarch64_elf64_load_result {
    guest_addr_t load_bias;
    guest_addr_t entry;
    guest_addr_t program_headers;
    word_t program_header_count;
    guest_addr_t brk_end;
};

enum aarch64_elf64_load_error aarch64_elf64_load(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table, guest_addr_t load_bias,
        struct aarch64_elf64_load_result *result);

#endif

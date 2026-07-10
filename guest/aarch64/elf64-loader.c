#include <string.h>

#include "guest/aarch64/elf64-loader.h"

#define ELF_PT_LOAD 1
#define ELF_PT_PHDR 6

static bool range_contains(qword_t outer_start, qword_t outer_size,
        qword_t inner_start, qword_t inner_size) {
    return inner_start >= outer_start &&
            inner_start - outer_start <= outer_size &&
            inner_size <= outer_size - (inner_start - outer_start);
}

static bool load_contains_memory_range(
        const struct aarch64_elf64_program_header *load,
        guest_addr_t address, qword_t size) {
    return range_contains(load->virtual_address, load->memory_size,
            address, size);
}

static bool find_program_headers(const struct aarch64_elf64_image *image,
        guest_addr_t *address) {
    qword_t table_size = (qword_t) image->program_header_count *
            AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    bool found_phdr = false;
    guest_addr_t phdr_address = 0;

    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header header;
        aarch64_elf64_program_header(image, i, &header);
        if (header.type != ELF_PT_PHDR)
            continue;
        if (found_phdr || header.file_offset != image->program_header_offset ||
                header.file_size < table_size || header.memory_size < table_size)
            return false;
        found_phdr = true;
        phdr_address = header.virtual_address;
    }

    if (!found_phdr) {
        for (word_t i = 0; i < image->program_header_count; i++) {
            struct aarch64_elf64_program_header header;
            aarch64_elf64_program_header(image, i, &header);
            if (header.type != ELF_PT_LOAD ||
                    !range_contains(header.file_offset, header.file_size,
                            image->program_header_offset, table_size))
                continue;
            qword_t delta = image->program_header_offset - header.file_offset;
            if (delta > header.memory_size ||
                    table_size > header.memory_size - delta)
                return false;
            phdr_address = header.virtual_address + delta;
            found_phdr = true;
            break;
        }
    }
    if (!found_phdr)
        return false;

    bool mapped = false;
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header header;
        aarch64_elf64_program_header(image, i, &header);
        if (header.type == ELF_PT_LOAD &&
                load_contains_memory_range(&header, phdr_address, table_size)) {
            mapped = true;
            break;
        }
    }
    if (!mapped)
        return false;
    *address = phdr_address;
    return true;
}

static bool segment_covers_page(
        const struct aarch64_elf64_program_header *segment,
        guest_addr_t page_base) {
    if (segment->type != ELF_PT_LOAD || segment->memory_size == 0)
        return false;
    guest_addr_t first = segment->virtual_address & ~GUEST_MEMORY_PAGE_MASK;
    guest_addr_t last = (segment->virtual_address + segment->memory_size - 1) &
            ~GUEST_MEMORY_PAGE_MASK;
    return page_base >= first && page_base <= last;
}

static bool previous_segment_covers_page(
        const struct aarch64_elf64_image *image, word_t current_index,
        guest_addr_t page_base) {
    for (word_t i = 0; i < current_index; i++) {
        struct aarch64_elf64_program_header previous;
        aarch64_elf64_program_header(image, i, &previous);
        if (segment_covers_page(&previous, page_base))
            return true;
    }
    return false;
}

static enum aarch64_elf64_load_error map_segment_pages(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table, word_t segment_index,
        const struct aarch64_elf64_program_header *segment) {
    if (segment->memory_size == 0)
        return AARCH64_ELF64_LOAD_OK;
    guest_addr_t first = segment->virtual_address & ~GUEST_MEMORY_PAGE_MASK;
    guest_addr_t last = (segment->virtual_address + segment->memory_size - 1) &
            ~GUEST_MEMORY_PAGE_MASK;

    for (guest_addr_t page = first;; page += GUEST_MEMORY_PAGE_SIZE) {
        byte_t *host_page;
        enum guest_page_table_result map_result = guest_page_table_map(
                table, page, segment->permissions, &host_page);
        if (map_result == GUEST_PAGE_TABLE_OUT_OF_MEMORY)
            return AARCH64_ELF64_LOAD_OUT_OF_MEMORY;
        if (map_result == GUEST_PAGE_TABLE_ALREADY_MAPPED) {
            if (!previous_segment_covers_page(image, segment_index, page))
                return AARCH64_ELF64_LOAD_MAPPING_CONFLICT;
            unsigned permissions;
            if (guest_page_table_lookup(table, page, &host_page, &permissions) !=
                    GUEST_PAGE_TABLE_OK)
                return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
            permissions |= segment->permissions;
            if (guest_page_table_protect(table, page, permissions) !=
                    GUEST_PAGE_TABLE_OK)
                return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        } else if (map_result != GUEST_PAGE_TABLE_OK) {
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        }
        if (page == last)
            break;
    }
    return AARCH64_ELF64_LOAD_OK;
}

static enum aarch64_elf64_load_error copy_segment(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table,
        const struct aarch64_elf64_program_header *segment) {
    guest_addr_t address = segment->virtual_address;
    qword_t file_offset = segment->file_offset;
    qword_t remaining = segment->file_size;
    while (remaining != 0) {
        guest_addr_t page_base = address & ~GUEST_MEMORY_PAGE_MASK;
        size_t page_offset = (size_t) (address & GUEST_MEMORY_PAGE_MASK);
        qword_t available = GUEST_MEMORY_PAGE_SIZE - page_offset;
        size_t chunk = (size_t) (remaining < available ? remaining : available);
        byte_t *host_page;
        unsigned permissions;
        if (guest_page_table_lookup(table, page_base,
                &host_page, &permissions) != GUEST_PAGE_TABLE_OK)
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        memcpy(host_page + page_offset,
                image->data + (size_t) file_offset, chunk);
        address += (guest_addr_t) chunk;
        file_offset += chunk;
        remaining -= chunk;
    }
    return AARCH64_ELF64_LOAD_OK;
}

enum aarch64_elf64_load_error aarch64_elf64_load(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table,
        struct aarch64_elf64_load_result *result) {
    guest_addr_t program_headers;
    if (!find_program_headers(image, &program_headers))
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;

    guest_addr_t brk_end = 0;
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (segment.type != ELF_PT_LOAD)
            continue;
        enum aarch64_elf64_load_error error = map_segment_pages(
                image, table, i, &segment);
        if (error != AARCH64_ELF64_LOAD_OK)
            return error;
        guest_addr_t end = segment.virtual_address + segment.memory_size;
        if (end > brk_end)
            brk_end = end;
    }

    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (segment.type != ELF_PT_LOAD)
            continue;
        enum aarch64_elf64_load_error error =
                copy_segment(image, table, &segment);
        if (error != AARCH64_ELF64_LOAD_OK)
            return error;
    }

    *result = (struct aarch64_elf64_load_result) {
        .entry = image->entry,
        .program_headers = program_headers,
        .program_header_count = image->program_header_count,
        .brk_end = (brk_end + GUEST_MEMORY_PAGE_MASK) &
                ~GUEST_MEMORY_PAGE_MASK,
    };
    return AARCH64_ELF64_LOAD_OK;
}

#include <assert.h>
#include <string.h>

#include "guest/aarch64/elf64-loader.h"

#define ELF_PT_LOAD 1
#define ELF_PT_PHDR 6

static bool add_load_bias(guest_addr_t address, guest_addr_t load_bias,
        guest_addr_t *rebased) {
    if (address > UINT64_MAX - load_bias)
        return false;
    *rebased = address + load_bias;
    return true;
}

static bool rebase_load_segment(
        struct aarch64_elf64_program_header *segment,
        guest_addr_t load_bias, const struct guest_address_space *space) {
    if (segment->type != ELF_PT_LOAD)
        return true;
    if (segment->alignment > 1 &&
            (load_bias & (segment->alignment - 1)) != 0)
        return false;
    guest_addr_t address;
    if (segment->memory_size > SIZE_MAX ||
            !add_load_bias(segment->virtual_address, load_bias, &address) ||
            !guest_address_space_contains(
                    space, address, (size_t) segment->memory_size))
        return false;
    segment->virtual_address = address;
    return true;
}

static bool range_contains(qword_t outer_start, qword_t outer_size,
        qword_t inner_start, qword_t inner_size) {
    return inner_start >= outer_start &&
            inner_start - outer_start <= outer_size &&
            inner_size <= outer_size - (inner_start - outer_start);
}

static bool find_program_headers(const struct aarch64_elf64_image *image,
        guest_addr_t load_bias, guest_addr_t *address) {
    qword_t table_size = (qword_t) image->program_header_count *
            AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    bool declared_phdr = false;
    guest_addr_t declared_address = 0;

    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header header;
        aarch64_elf64_program_header(image, i, &header);
        if (header.type != ELF_PT_PHDR)
            continue;
        if (declared_phdr ||
                header.file_offset != image->program_header_offset ||
                header.file_size < table_size || header.memory_size < table_size)
            return false;
        declared_phdr = true;
        declared_address = header.virtual_address;
    }

    bool found_mapping = false;
    guest_addr_t mapped_address = 0;
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header header;
        aarch64_elf64_program_header(image, i, &header);
        if (header.type != ELF_PT_LOAD ||
                !range_contains(header.file_offset, header.file_size,
                        image->program_header_offset, table_size))
            continue;
        qword_t delta = image->program_header_offset - header.file_offset;
        if (delta > header.memory_size ||
                table_size > header.memory_size - delta ||
                header.virtual_address > UINT64_MAX - delta)
            return false;
        guest_addr_t candidate = header.virtual_address + delta;
        if (!declared_phdr || candidate == declared_address) {
            mapped_address = candidate;
            found_mapping = true;
            break;
        }
    }
    if (!found_mapping)
        return false;
    return add_load_bias(mapped_address, load_bias, address);
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
        guest_addr_t load_bias, guest_addr_t page_base) {
    for (word_t i = 0; i < current_index; i++) {
        struct aarch64_elf64_program_header previous;
        aarch64_elf64_program_header(image, i, &previous);
        if (previous.type == ELF_PT_LOAD &&
                !add_load_bias(previous.virtual_address, load_bias,
                        &previous.virtual_address))
            return false;
        if (segment_covers_page(&previous, page_base))
            return true;
    }
    return false;
}

static enum aarch64_elf64_load_error preflight_segment_pages(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table, guest_addr_t load_bias) {
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (!rebase_load_segment(
                &segment, load_bias, &table->address_space))
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        if (segment.type != ELF_PT_LOAD || segment.memory_size == 0)
            continue;

        guest_addr_t first =
                segment.virtual_address & ~GUEST_MEMORY_PAGE_MASK;
        guest_addr_t last = (segment.virtual_address +
                segment.memory_size - 1) & ~GUEST_MEMORY_PAGE_MASK;
        for (guest_addr_t page = first;; page += GUEST_MEMORY_PAGE_SIZE) {
            if (!previous_segment_covers_page(image, i, load_bias, page)) {
                byte_t *host_page;
                unsigned permissions;
                enum guest_page_table_result lookup = guest_page_table_lookup(
                        table, page, &host_page, &permissions);
                if (lookup == GUEST_PAGE_TABLE_OK)
                    return AARCH64_ELF64_LOAD_MAPPING_CONFLICT;
                if (lookup != GUEST_PAGE_TABLE_NOT_MAPPED)
                    return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
            }
            if (page == last)
                break;
        }
    }
    return AARCH64_ELF64_LOAD_OK;
}

static void rollback_segment_pages(const struct aarch64_elf64_image *image,
        struct guest_page_table *table, guest_addr_t load_bias) {
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (!rebase_load_segment(
                &segment, load_bias, &table->address_space)) {
            assert(false);
            return;
        }
        if (segment.type != ELF_PT_LOAD || segment.memory_size == 0)
            continue;

        guest_addr_t first =
                segment.virtual_address & ~GUEST_MEMORY_PAGE_MASK;
        guest_addr_t last = (segment.virtual_address +
                segment.memory_size - 1) & ~GUEST_MEMORY_PAGE_MASK;
        for (guest_addr_t page = first;; page += GUEST_MEMORY_PAGE_SIZE) {
            if (!previous_segment_covers_page(image, i, load_bias, page)) {
                byte_t *host_page;
                unsigned permissions;
                if (guest_page_table_lookup(table, page,
                        &host_page, &permissions) == GUEST_PAGE_TABLE_OK &&
                        guest_page_table_unmap(table, page) !=
                                GUEST_PAGE_TABLE_OK) {
                    assert(false);
                    return;
                }
            }
            if (page == last)
                break;
        }
    }
}

static enum aarch64_elf64_load_error map_segment_pages(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table, word_t segment_index,
        guest_addr_t load_bias,
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
            if (!previous_segment_covers_page(
                    image, segment_index, load_bias, page))
                return AARCH64_ELF64_LOAD_MAPPING_CONFLICT;
            unsigned permissions;
            if (guest_page_table_lookup(table, page, &host_page, &permissions) !=
                    GUEST_PAGE_TABLE_OK)
                return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
            if (guest_page_table_protect(
                    table, page, segment->permissions) !=
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

static enum aarch64_elf64_load_error make_page_anonymous(
        struct guest_page_table *table, guest_addr_t page) {
    enum guest_page_table_result result = guest_page_table_set_origin(
            table, page, GUEST_PAGE_ORIGIN_ANONYMOUS);
    if (result == GUEST_PAGE_TABLE_OUT_OF_MEMORY)
        return AARCH64_ELF64_LOAD_OUT_OF_MEMORY;
    if (result != GUEST_PAGE_TABLE_OK)
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
    return AARCH64_ELF64_LOAD_OK;
}

static enum aarch64_elf64_load_error classify_segment_pages(
        struct guest_page_table *table,
        struct guest_file_source *file_source,
        const struct aarch64_elf64_program_header *segment) {
    // 按 PT_LOAD 顺序重放文件映射与补零；后段重叠映射决定最终页面来源。
    if (segment->file_size != 0) {
        guest_addr_t first = segment->virtual_address &
                ~GUEST_MEMORY_PAGE_MASK;
        guest_addr_t last = (segment->virtual_address +
                segment->file_size - 1) & ~GUEST_MEMORY_PAGE_MASK;
        qword_t file_offset = segment->file_offset &
                ~GUEST_MEMORY_PAGE_MASK;
        for (guest_addr_t page = first;; page += GUEST_MEMORY_PAGE_SIZE) {
            if (guest_page_table_protect(table, page,
                    segment->permissions) != GUEST_PAGE_TABLE_OK ||
                    guest_page_table_set_file_source(table, page,
                            file_source, file_offset) !=
                                    GUEST_PAGE_TABLE_OK)
                return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
            if (page == last)
                break;
            file_offset += GUEST_MEMORY_PAGE_SIZE;
        }
    }
    if (segment->memory_size == segment->file_size)
        return AARCH64_ELF64_LOAD_OK;

    guest_addr_t zero_start = segment->virtual_address +
            segment->file_size;
    guest_addr_t memory_end = segment->virtual_address +
            segment->memory_size;
    guest_addr_t anonymous_start;
    if (segment->file_size == 0) {
        anonymous_start = segment->virtual_address &
                ~GUEST_MEMORY_PAGE_MASK;
    } else {
        if ((zero_start & GUEST_MEMORY_PAGE_MASK) != 0 &&
                (segment->permissions & GUEST_MEMORY_WRITE) != 0) {
            enum aarch64_elf64_load_error error = make_page_anonymous(
                    table, zero_start & ~GUEST_MEMORY_PAGE_MASK);
            if (error != AARCH64_ELF64_LOAD_OK)
                return error;
        }
        anonymous_start = (zero_start + GUEST_MEMORY_PAGE_MASK) &
                ~GUEST_MEMORY_PAGE_MASK;
    }
    guest_addr_t anonymous_end = (memory_end + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    // Linux 的 vm_brk_flags 使用匿名数据页默认 RW 权限，只额外传递执行位。
    unsigned anonymous_permissions = GUEST_MEMORY_READ | GUEST_MEMORY_WRITE |
            (segment->permissions & GUEST_MEMORY_EXECUTE);
    for (guest_addr_t page = anonymous_start;
            page < anonymous_end; page += GUEST_MEMORY_PAGE_SIZE) {
        enum aarch64_elf64_load_error error =
                make_page_anonymous(table, page);
        if (error != AARCH64_ELF64_LOAD_OK)
            return error;
        if (guest_page_table_protect(table, page,
                anonymous_permissions) != GUEST_PAGE_TABLE_OK)
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
    }
    return AARCH64_ELF64_LOAD_OK;
}

static enum aarch64_elf64_load_error copy_segment(
        const struct aarch64_elf64_image *image,
        struct guest_page_table *table,
        const struct aarch64_elf64_program_header *segment) {
    guest_addr_t page = segment->virtual_address &
            ~GUEST_MEMORY_PAGE_MASK;
    if (segment->file_size != 0) {
        qword_t file_offset = segment->file_offset &
                ~GUEST_MEMORY_PAGE_MASK;
        qword_t mapped_size = (segment->file_size +
                (segment->virtual_address & GUEST_MEMORY_PAGE_MASK) +
                GUEST_MEMORY_PAGE_MASK) & ~GUEST_MEMORY_PAGE_MASK;
        for (qword_t copied = 0; copied < mapped_size;
                copied += GUEST_MEMORY_PAGE_SIZE) {
            byte_t *host_page;
            unsigned permissions;
            if (guest_page_table_lookup(table, page,
                    &host_page, &permissions) != GUEST_PAGE_TABLE_OK)
                return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
            qword_t source_offset = file_offset + copied;
            size_t available = source_offset >= image->size ? 0 :
                    image->size - (size_t) source_offset;
            if (available > GUEST_MEMORY_PAGE_SIZE)
                available = GUEST_MEMORY_PAGE_SIZE;
            if (available != 0)
                memcpy(host_page,
                        image->data + (size_t) source_offset, available);
            if (available != GUEST_MEMORY_PAGE_SIZE)
                memset(host_page + available, 0,
                        GUEST_MEMORY_PAGE_SIZE - available);
            page += GUEST_MEMORY_PAGE_SIZE;
        }
    }

    if (segment->memory_size == segment->file_size)
        return AARCH64_ELF64_LOAD_OK;

    guest_addr_t zero_start = segment->virtual_address + segment->file_size;
    if (segment->file_size != 0 &&
            (segment->permissions & GUEST_MEMORY_WRITE) != 0 &&
            (zero_start & GUEST_MEMORY_PAGE_MASK) != 0) {
        guest_addr_t page_base = zero_start & ~GUEST_MEMORY_PAGE_MASK;
        size_t page_offset = (size_t)
                (zero_start & GUEST_MEMORY_PAGE_MASK);
        byte_t *host_page;
        unsigned permissions;
        if (guest_page_table_lookup(table, page_base,
                &host_page, &permissions) != GUEST_PAGE_TABLE_OK)
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        memset(host_page + page_offset, 0,
                GUEST_MEMORY_PAGE_SIZE - page_offset);
    }

    guest_addr_t anonymous_start = segment->file_size == 0 ?
            segment->virtual_address & ~GUEST_MEMORY_PAGE_MASK :
            (zero_start + GUEST_MEMORY_PAGE_MASK) &
                    ~GUEST_MEMORY_PAGE_MASK;
    guest_addr_t anonymous_end = (segment->virtual_address +
            segment->memory_size + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    for (page = anonymous_start; page < anonymous_end;
            page += GUEST_MEMORY_PAGE_SIZE) {
        byte_t *host_page;
        unsigned permissions;
        if (guest_page_table_lookup(table, page,
                &host_page, &permissions) != GUEST_PAGE_TABLE_OK)
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        memset(host_page, 0, GUEST_MEMORY_PAGE_SIZE);
    }
    return AARCH64_ELF64_LOAD_OK;
}

enum aarch64_elf64_load_error aarch64_elf64_load(
        const struct aarch64_elf64_image *image,
        struct guest_file_source *file_source,
        struct guest_page_table *table, guest_addr_t load_bias,
        struct aarch64_elf64_load_result *result) {
    if (file_source == NULL ||
            (image->position_independent ? load_bias == 0 : load_bias != 0) ||
            (load_bias & GUEST_MEMORY_PAGE_MASK) != 0)
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
    guest_addr_t program_headers;
    if (!find_program_headers(image, load_bias, &program_headers))
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
    qword_t program_header_size =
            (qword_t) image->program_header_count *
            AARCH64_ELF64_PROGRAM_HEADER_SIZE;
    if (program_header_size > SIZE_MAX ||
            !guest_address_space_contains(&table->address_space,
                    program_headers, (size_t) program_header_size))
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;

    guest_addr_t entry;
    if (!add_load_bias(image->entry, load_bias, &entry) ||
            !guest_address_space_contains(
                    &table->address_space, entry, 1))
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;

    guest_addr_t brk_end = 0;
    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (segment.type != ELF_PT_LOAD)
            continue;
        if (!rebase_load_segment(
                &segment, load_bias, &table->address_space))
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        guest_addr_t end = segment.virtual_address + segment.memory_size;
        if (end > brk_end)
            brk_end = end;
    }
    if (brk_end > UINT64_MAX - GUEST_MEMORY_PAGE_MASK)
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
    brk_end = (brk_end + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    if (!guest_address_space_contains(&table->address_space, brk_end, 0))
        return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;

    enum aarch64_elf64_load_error error = preflight_segment_pages(
            image, table, load_bias);
    if (error != AARCH64_ELF64_LOAD_OK)
        return error;

    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (segment.type != ELF_PT_LOAD)
            continue;
        if (!rebase_load_segment(
                &segment, load_bias, &table->address_space)) {
            rollback_segment_pages(image, table, load_bias);
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        }
        error = map_segment_pages(
                image, table, i, load_bias, &segment);
        if (error != AARCH64_ELF64_LOAD_OK) {
            rollback_segment_pages(image, table, load_bias);
            return error;
        }
    }

    for (word_t i = 0; i < image->program_header_count; i++) {
        struct aarch64_elf64_program_header segment;
        aarch64_elf64_program_header(image, i, &segment);
        if (segment.type != ELF_PT_LOAD)
            continue;
        if (!rebase_load_segment(
                &segment, load_bias, &table->address_space)) {
            rollback_segment_pages(image, table, load_bias);
            return AARCH64_ELF64_LOAD_UNSUPPORTED_LAYOUT;
        }
        error = copy_segment(image, table, &segment);
        if (error == AARCH64_ELF64_LOAD_OK)
            error = classify_segment_pages(table, file_source, &segment);
        if (error != AARCH64_ELF64_LOAD_OK) {
            rollback_segment_pages(image, table, load_bias);
            return error;
        }
    }

    *result = (struct aarch64_elf64_load_result) {
        .load_bias = load_bias,
        .entry = entry,
        .program_headers = program_headers,
        .program_header_count = image->program_header_count,
        .brk_end = brk_end,
    };
    return AARCH64_ELF64_LOAD_OK;
}

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"

static qword_t linux_error(unsigned error) {
    return (qword_t) -(sqword_t) error;
}

static qword_t address_limit(const struct guest_linux_mm *memory) {
    return UINT64_C(1) <<
            memory->page_table->address_space.address_bits;
}

static bool page_count(qword_t length, qword_t *count) {
    if (length == 0 || length > UINT64_MAX - GUEST_MEMORY_PAGE_MASK)
        return false;
    *count = (length + GUEST_MEMORY_PAGE_MASK) >>
            GUEST_MEMORY_PAGE_BITS;
    return true;
}

static bool valid_range(const struct guest_linux_mm *memory,
        qword_t first, qword_t count) {
    qword_t limit = address_limit(memory);
    if ((first & GUEST_MEMORY_PAGE_MASK) != 0 || first >= limit)
        return false;
    return count <= (limit - first) >> GUEST_MEMORY_PAGE_BITS;
}

static struct guest_page_range make_range(qword_t first, qword_t count) {
    return (struct guest_page_range) {
        .first = (guest_addr_t) first,
        .page_count = count,
    };
}

static bool page_is_mapped(struct guest_linux_mm *memory,
        qword_t page) {
    byte_t *host_page;
    unsigned permissions;
    enum guest_page_table_result result = guest_page_table_lookup(
            memory->page_table, (guest_addr_t) page,
            &host_page, &permissions);
    assert(result == GUEST_PAGE_TABLE_OK ||
            result == GUEST_PAGE_TABLE_NOT_MAPPED);
    return result == GUEST_PAGE_TABLE_OK;
}

static bool page_is_occupied(struct guest_linux_mm *memory,
        qword_t page) {
    return guest_linux_vma_find(
            &memory->vmas, (guest_addr_t) page) != NULL ||
            page_is_mapped(memory, page);
}

static bool range_is_hole(struct guest_linux_mm *memory,
        qword_t first, qword_t count) {
    if (!valid_range(memory, first, count))
        return false;
    for (qword_t i = 0; i < count; i++) {
        if (page_is_occupied(memory,
                first + (i << GUEST_MEMORY_PAGE_BITS)))
            return false;
    }
    return true;
}

static bool range_is_in_mmap_arena(const struct guest_linux_mm *memory,
        qword_t first, qword_t count) {
    if (first < memory->mmap_base || first >= memory->mmap_limit)
        return false;
    qword_t offset_pages = (first - memory->mmap_base) >>
            GUEST_MEMORY_PAGE_BITS;
    qword_t arena_pages = (memory->mmap_limit - memory->mmap_base) >>
            GUEST_MEMORY_PAGE_BITS;
    return count <= arena_pages - offset_pages;
}

static bool find_mmap_hole(struct guest_linux_mm *memory,
        qword_t count, qword_t *first) {
    qword_t arena_pages = (memory->mmap_limit - memory->mmap_base) >>
            GUEST_MEMORY_PAGE_BITS;
    qword_t offset = 0;
    while (offset <= arena_pages && count <= arena_pages - offset) {
        bool retry = false;
        for (qword_t i = 0; i < count; i++) {
            qword_t page = memory->mmap_base +
                    ((offset + i) << GUEST_MEMORY_PAGE_BITS);
            if (page_is_occupied(memory, page)) {
                offset += i + 1;
                retry = true;
                break;
            }
        }
        if (!retry) {
            *first = memory->mmap_base +
                    (offset << GUEST_MEMORY_PAGE_BITS);
            return true;
        }
    }
    return false;
}

struct mmap_selection {
    qword_t first;
    qword_t count;
    bool fixed;
    bool no_replace;
};

static bool select_mmap_range(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t flags,
        struct mmap_selection *selection, qword_t *error) {
    qword_t count;
    if (!page_count(length, &count)) {
        *error = length == 0 ? linux_error(GUEST_LINUX_EINVAL) :
                linux_error(GUEST_LINUX_ENOMEM);
        return false;
    }
    if (count > (GUEST_LINUX_MMAP_ARENA_SIZE >>
            GUEST_MEMORY_PAGE_BITS)) {
        *error = linux_error(GUEST_LINUX_ENOMEM);
        return false;
    }

    bool no_replace =
            (flags & GUEST_LINUX_MAP_FIXED_NOREPLACE) != 0;
    bool fixed = no_replace || (flags & GUEST_LINUX_MAP_FIXED) != 0;
    qword_t first;
    if (fixed) {
        first = address;
        if ((first & GUEST_MEMORY_PAGE_MASK) != 0) {
            *error = linux_error(GUEST_LINUX_EINVAL);
            return false;
        }
        if (!valid_range(memory, first, count)) {
            *error = linux_error(GUEST_LINUX_ENOMEM);
            return false;
        }
    } else {
        qword_t hint = (qword_t) address & ~GUEST_MEMORY_PAGE_MASK;
        if (address != 0 && range_is_in_mmap_arena(
                memory, hint, count) && range_is_hole(
                memory, hint, count)) {
            first = hint;
        } else if (!find_mmap_hole(memory, count, &first)) {
            *error = linux_error(GUEST_LINUX_ENOMEM);
            return false;
        }
    }

    if (no_replace && !range_is_hole(memory, first, count)) {
        *error = linux_error(GUEST_LINUX_EEXIST);
        return false;
    }
    *selection = (struct mmap_selection) {
        .first = first,
        .count = count,
        .fixed = fixed,
        .no_replace = no_replace,
    };
    return true;
}

static bool decode_permissions(qword_t protection,
        unsigned *permissions) {
    if ((protection & ~GUEST_LINUX_PROT_MASK) != 0)
        return false;
    *permissions = 0;
    if ((protection & GUEST_LINUX_PROT_READ) != 0)
        *permissions |= GUEST_MEMORY_READ;
    // AArch64 的普通用户页没有只写形式，写权限同时允许读取。
    if ((protection & GUEST_LINUX_PROT_WRITE) != 0)
        *permissions |= GUEST_MEMORY_READ | GUEST_MEMORY_WRITE;
    if ((protection & GUEST_LINUX_PROT_EXEC) != 0)
        *permissions |= GUEST_MEMORY_EXECUTE;
    return true;
}

static bool page_end(const struct guest_linux_mm *memory,
        guest_addr_t address, qword_t *end) {
    qword_t rounded = ((qword_t) address + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    if (rounded > address_limit(memory))
        return false;
    *end = rounded;
    return true;
}

static bool vma_range_intersects(const struct guest_linux_vma_set *set,
        guest_addr_t first, guest_addr_t last) {
    for (size_t index = 0; index < set->count; index++) {
        const struct guest_linux_vma *mapping = &set->entries[index];
        if (mapping->first >= last)
            break;
        if (mapping->last > first)
            return true;
    }
    return false;
}

static bool vma_protection_changes(const struct guest_linux_vma_set *set,
        guest_addr_t first, guest_addr_t last, qword_t protection) {
    for (size_t index = 0; index < set->count; index++) {
        const struct guest_linux_vma *mapping = &set->entries[index];
        if (mapping->first >= last)
            break;
        if (mapping->last > first && mapping->protection != protection)
            return true;
    }
    return false;
}

enum mprotect_range_status {
    MPROTECT_RANGE_OK,
    MPROTECT_RANGE_HOLE,
    MPROTECT_RANGE_DENIED,
};

static enum mprotect_range_status check_mprotect_range(
        struct guest_linux_mm *memory, guest_addr_t first,
        qword_t count, qword_t protection) {
    for (qword_t index = 0; index < count; index++) {
        guest_addr_t page = first +
                (index << GUEST_MEMORY_PAGE_BITS);
        const struct guest_linux_vma *mapping = guest_linux_vma_find(
                &memory->vmas, page);
        if (mapping != NULL) {
            if ((protection & ~mapping->maximum_protection) != 0)
                return MPROTECT_RANGE_DENIED;
        } else if (!page_is_mapped(memory, page)) {
            return MPROTECT_RANGE_HOLE;
        }
    }
    return MPROTECT_RANGE_OK;
}

static bool vma_allows_access(const struct guest_linux_vma *mapping,
        enum guest_memory_access access, unsigned *permissions) {
    bool decoded = decode_permissions(mapping->protection, permissions);
    assert(decoded);
    return (*permissions & access) != 0;
}

static bool file_vma_source(enum guest_linux_vma_source source) {
    return source == GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE ||
            source == GUEST_LINUX_VMA_SOURCE_FILE_SHARED;
}

static bool same_file_page(const struct guest_linux_vma *mapping,
        guest_addr_t page_base, struct guest_file_pager *pager,
        qword_t file_offset, enum guest_linux_vma_source source) {
    if (mapping == NULL || mapping->source != source ||
            mapping->file_pager != pager || page_base < mapping->first)
        return false;
    qword_t delta = page_base - mapping->first;
    return mapping->file_offset <= UINT64_MAX - delta &&
            mapping->file_offset + delta == file_offset;
}

static enum guest_memory_page_in_result guest_linux_page_in(
        void *opaque, guest_addr_t page_base,
        enum guest_memory_access access,
        struct guest_memory_fault *fault) {
    struct guest_linux_mm *memory = opaque;
    struct guest_page_table *table = memory->page_table;
    bool locked = guest_page_table_write_lock(table);
    const struct guest_linux_vma *mapping = guest_linux_vma_find(
            &memory->vmas, page_base);
    unsigned permissions;
    if (mapping == NULL || !file_vma_source(mapping->source)) {
        guest_page_table_write_unlock(table, locked);
        return GUEST_MEMORY_PAGE_IN_FAILED;
    }
    if (!vma_allows_access(mapping, access, &permissions)) {
        fault->kind = GUEST_MEMORY_FAULT_PERMISSION;
        guest_page_table_write_unlock(table, locked);
        return GUEST_MEMORY_PAGE_IN_FAILED;
    }
    // resize 只把跨地址空间 backing 标成不可访问；在当前 fault 事务中
    // 撤销旧 PTE，随后按 provider 的新 EOF 重新换页或返回 SIGBUS。
    (void) guest_page_table_remove_inaccessible(table, page_base);
    byte_t *resident_page;
    unsigned resident_permissions;
    if (guest_page_table_lookup(table, page_base,
            &resident_page, &resident_permissions) == GUEST_PAGE_TABLE_OK) {
        use(resident_page);
        use(resident_permissions);
        guest_page_table_write_unlock(table, locked);
        return GUEST_MEMORY_PAGE_IN_RETRY;
    }

    qword_t sequence = memory->vma_sequence;
    enum guest_linux_vma_source source = mapping->source;
    struct guest_file_pager *pager = guest_file_pager_retain(
            mapping->file_pager);
    qword_t delta = page_base - mapping->first;
    assert(mapping->file_offset <= UINT64_MAX - delta);
    qword_t file_offset = mapping->file_offset + delta;
    guest_page_table_write_unlock(table, locked);

    struct guest_page_backing *backing = NULL;
    enum guest_file_page_result page_result = guest_file_pager_get_page(
            pager, file_offset, &backing);

    locked = guest_page_table_write_lock(table);
    mapping = guest_linux_vma_find(&memory->vmas, page_base);
    bool current = memory->vma_sequence == sequence &&
            same_file_page(
                    mapping, page_base, pager, file_offset, source);
    enum guest_memory_page_in_result result =
            GUEST_MEMORY_PAGE_IN_RETRY;
    if (current && !vma_allows_access(mapping, access, &permissions)) {
        fault->kind = GUEST_MEMORY_FAULT_PERMISSION;
        result = GUEST_MEMORY_PAGE_IN_FAILED;
    } else if (current && page_result != GUEST_FILE_PAGE_OK) {
        fault->kind = page_result == GUEST_FILE_PAGE_OUT_OF_MEMORY ?
                GUEST_MEMORY_FAULT_OUT_OF_MEMORY :
                GUEST_MEMORY_FAULT_BUS_ADDRESS;
        result = GUEST_MEMORY_PAGE_IN_FAILED;
    } else if (current) {
        assert(backing != NULL);
        enum guest_page_table_result installed = source ==
                GUEST_LINUX_VMA_SOURCE_FILE_SHARED ?
                guest_page_table_map_shared_file_backing(
                        table, page_base, permissions,
                        guest_file_pager_file_source(pager),
                        file_offset, backing) :
                guest_page_table_map_private_file_backing(
                        table, page_base, permissions,
                        guest_file_pager_file_source(pager),
                        file_offset, backing);
        if (installed == GUEST_PAGE_TABLE_OUT_OF_MEMORY) {
            fault->kind = GUEST_MEMORY_FAULT_OUT_OF_MEMORY;
            result = GUEST_MEMORY_PAGE_IN_FAILED;
        } else {
            assert(installed == GUEST_PAGE_TABLE_OK ||
                    installed == GUEST_PAGE_TABLE_ALREADY_MAPPED);
        }
    }
    guest_page_table_write_unlock(table, locked);

    if (backing != NULL)
        guest_page_backing_release(backing);
    guest_file_pager_release(pager);
    return result;
}

static const struct guest_page_table_fault_ops linux_fault_ops = {
    .page_in = guest_linux_page_in,
};

void guest_linux_mm_init(struct guest_linux_mm *memory,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit) {
    assert(memory != NULL && page_table != NULL);
    assert((start_brk & GUEST_MEMORY_PAGE_MASK) == 0);
    assert(brk_limit >= start_brk);
    assert(guest_address_space_contains(&page_table->address_space,
            start_brk, 0) &&
            guest_address_space_contains(&page_table->address_space,
                    brk_limit, 0));
    qword_t limit = UINT64_C(1) << page_table->address_space.address_bits;
    qword_t mmap_base = ((qword_t) brk_limit + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    qword_t mmap_limit = GUEST_LINUX_MMAP_ARENA_SIZE > limit - mmap_base ?
            limit : mmap_base + GUEST_LINUX_MMAP_ARENA_SIZE;
    *memory = (struct guest_linux_mm) {
        .page_table = page_table,
        .start_brk = start_brk,
        .brk = start_brk,
        .brk_limit = brk_limit,
        .mmap_base = mmap_base,
        .mmap_limit = mmap_limit,
        .vma_sequence = 1,
    };
    guest_linux_vma_set_init(&memory->vmas);
    guest_page_table_set_fault_ops(
            page_table, &linux_fault_ops, memory);
}

void guest_linux_mm_destroy(struct guest_linux_mm *memory) {
    assert(memory != NULL);
    if (memory->page_table != NULL)
        guest_page_table_set_fault_ops(memory->page_table, NULL, NULL);
    guest_linux_vma_set_destroy(&memory->vmas);
    *memory = (struct guest_linux_mm) {0};
}

bool guest_linux_mm_clone(struct guest_linux_mm *destination,
        struct guest_page_table *destination_page_table,
        const struct guest_linux_mm *source) {
    assert(destination != NULL && destination_page_table != NULL &&
            source != NULL && source->page_table != NULL);
    if (destination == source)
        return false;
    *destination = (struct guest_linux_mm) {0};
    if (destination_page_table == source->page_table)
        return false;
    *destination_page_table = (struct guest_page_table) {0};
    bool locked = guest_page_table_read_lock(source->page_table);
    struct guest_linux_vma_set vmas;
    bool result = guest_linux_vma_set_clone(&vmas, &source->vmas);
    if (result) {
        result = guest_page_table_clone_locked(
                destination_page_table, source->page_table);
    }
    if (result) {
        *destination = (struct guest_linux_mm) {
            .page_table = destination_page_table,
            .start_brk = source->start_brk,
            .brk = source->brk,
            .brk_limit = source->brk_limit,
            .mmap_base = source->mmap_base,
            .mmap_limit = source->mmap_limit,
            .vma_sequence = source->vma_sequence,
            .membarrier_registrations =
                    source->membarrier_registrations,
            .vmas = vmas,
        };
        guest_page_table_set_fault_ops(destination_page_table,
                &linux_fault_ops, destination);
    } else {
        guest_linux_vma_set_destroy(&vmas);
    }
    guest_page_table_read_unlock(source->page_table, locked);
    return result;
}

static void commit_vma_candidate(struct guest_linux_mm *memory,
        struct guest_linux_vma_set *candidate,
        struct guest_linux_vma_set *retired) {
    assert(retired->entries == NULL && retired->count == 0);
    *retired = memory->vmas;
    memory->vmas = *candidate;
    *candidate = (struct guest_linux_vma_set) {0};
    if (memory->vma_sequence == UINT64_MAX)
        abort();
    memory->vma_sequence++;
}

static bool prepare_vma_insert(const struct guest_linux_mm *memory,
        struct guest_linux_vma mapping,
        struct guest_linux_vma_set *candidate) {
    if (!guest_linux_vma_set_clone(candidate, &memory->vmas))
        return false;
    if (guest_linux_vma_insert(candidate, mapping))
        return true;
    guest_linux_vma_set_destroy(candidate);
    return false;
}

static bool prepare_vma_remove(const struct guest_linux_mm *memory,
        guest_addr_t first, guest_addr_t last,
        struct guest_linux_vma_set *candidate) {
    if (!guest_linux_vma_set_clone(candidate, &memory->vmas))
        return false;
    if (guest_linux_vma_remove(candidate, first, last))
        return true;
    guest_linux_vma_set_destroy(candidate);
    return false;
}

static bool prepare_vma_protect(const struct guest_linux_mm *memory,
        guest_addr_t first, guest_addr_t last, qword_t protection,
        struct guest_linux_vma_set *candidate) {
    if (!guest_linux_vma_set_clone(candidate, &memory->vmas))
        return false;
    if (guest_linux_vma_protect_tracked(
            candidate, first, last, protection))
        return true;
    guest_linux_vma_set_destroy(candidate);
    return false;
}

qword_t guest_linux_membarrier(struct guest_linux_mm *memory,
        sdword_t command, dword_t flags) {
    assert(memory != NULL && memory->page_table != NULL);
    if (flags != 0)
        return linux_error(GUEST_LINUX_EINVAL);
    if (command == GUEST_LINUX_MEMBARRIER_CMD_QUERY)
        return GUEST_LINUX_MEMBARRIER_SUPPORTED_COMMANDS;
    if (command != GUEST_LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED &&
            command !=
                    GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED &&
            command != GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS)
        return linux_error(GUEST_LINUX_EINVAL);

    // 所有并发 guest 访存都经过这把锁，独占事务形成同一 mm 的全序切面。
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result;
    if (command ==
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED) {
        memory->membarrier_registrations |=
                GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
        result = 0;
    } else if (command ==
            GUEST_LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS) {
        result = memory->membarrier_registrations;
    } else if ((memory->membarrier_registrations &
            GUEST_LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED) == 0) {
        result = linux_error(GUEST_LINUX_EPERM);
    } else {
        result = 0;
    }
    guest_page_table_write_unlock(memory->page_table, locked);
    return result;
}

static guest_addr_t guest_linux_brk_unlocked(struct guest_linux_mm *memory,
        guest_addr_t requested, struct guest_linux_vma_set *retired) {
    if (requested == 0)
        return memory->brk;
    if (requested < memory->start_brk || requested > memory->brk_limit ||
            !guest_address_space_contains(
                    &memory->page_table->address_space, requested, 0))
        return memory->brk;

    qword_t old_end;
    qword_t new_end;
    if (!page_end(memory, memory->brk, &old_end) ||
            !page_end(memory, requested, &new_end))
        return memory->brk;

    struct guest_linux_vma_set candidate;
    if (new_end > old_end) {
        struct guest_linux_vma mapping = {
            .first = (guest_addr_t) old_end,
            .last = (guest_addr_t) new_end,
            .protection = GUEST_LINUX_PROT_READ |
                    GUEST_LINUX_PROT_WRITE,
            .source = GUEST_LINUX_VMA_SOURCE_BRK,
            .maximum_protection = GUEST_LINUX_PROT_MASK,
        };
        if (!prepare_vma_insert(memory, mapping, &candidate))
            return memory->brk;
        struct guest_page_range range = make_range(old_end,
                (new_end - old_end) >> GUEST_MEMORY_PAGE_BITS);
        if (guest_page_table_map_zero_range(memory->page_table, range,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) !=
                GUEST_PAGE_TABLE_OK) {
            guest_linux_vma_set_destroy(&candidate);
            return memory->brk;
        }
        commit_vma_candidate(memory, &candidate, retired);
    } else if (new_end < old_end) {
        bool update_vmas = vma_range_intersects(&memory->vmas,
                (guest_addr_t) new_end, (guest_addr_t) old_end);
        if (update_vmas && !prepare_vma_remove(memory,
                (guest_addr_t) new_end,
                (guest_addr_t) old_end, &candidate))
            return memory->brk;
        struct guest_page_range range = make_range(new_end,
                (old_end - new_end) >> GUEST_MEMORY_PAGE_BITS);
        assert(guest_page_table_unmap_range(
                memory->page_table, range, true) == GUEST_PAGE_TABLE_OK);
        if (update_vmas)
            commit_vma_candidate(memory, &candidate, retired);
    }

    memory->brk = requested;
    return memory->brk;
}

guest_addr_t guest_linux_brk(struct guest_linux_mm *memory,
        guest_addr_t requested) {
    struct guest_linux_vma_set retired;
    guest_linux_vma_set_init(&retired);
    bool locked = guest_page_table_write_lock(memory->page_table);
    guest_addr_t result = guest_linux_brk_unlocked(
            memory, requested, &retired);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired);
    return result;
}

static qword_t guest_linux_mmap_unlocked(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t flags, qword_t fd, qword_t offset,
        struct guest_linux_vma_set *retired) {
    use(fd);
    unsigned permissions;
    if (!decode_permissions(protection, &permissions))
        return linux_error(GUEST_LINUX_EINVAL);

    qword_t allowed = GUEST_LINUX_MAP_SHARED | GUEST_LINUX_MAP_PRIVATE |
            GUEST_LINUX_MAP_FIXED | GUEST_LINUX_MAP_ANONYMOUS |
            GUEST_LINUX_MAP_FIXED_NOREPLACE;
    qword_t mapping_type = flags & GUEST_LINUX_MAP_TYPE;
    bool shared = mapping_type == GUEST_LINUX_MAP_SHARED;
    if ((flags & ~allowed) != 0 ||
            (mapping_type != GUEST_LINUX_MAP_PRIVATE &&
            mapping_type != GUEST_LINUX_MAP_SHARED) ||
            (flags & GUEST_LINUX_MAP_ANONYMOUS) == 0 ||
            (offset & GUEST_MEMORY_PAGE_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);

    struct mmap_selection selection;
    qword_t selection_error;
    if (!select_mmap_range(memory, address, length, flags,
            &selection, &selection_error))
        return selection_error;
    qword_t first = selection.first;
    qword_t count = selection.count;
    struct guest_linux_vma mapping = {
        .first = (guest_addr_t) first,
        .last = (guest_addr_t) (first +
                (count << GUEST_MEMORY_PAGE_BITS)),
        .protection = protection,
        .source = shared ? GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED :
                GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE,
        .maximum_protection = GUEST_LINUX_PROT_MASK,
    };
    struct guest_linux_vma_set candidate;
    if (!prepare_vma_insert(memory, mapping, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);

    struct guest_page_range range = make_range(first, count);
    enum guest_page_table_result result = shared ?
            guest_page_table_map_zero_shared_range(
                    memory->page_table, range, permissions,
                    selection.fixed && !selection.no_replace) :
            guest_page_table_map_zero_range(
                    memory->page_table, range, permissions,
                    selection.fixed && !selection.no_replace);
    if (result == GUEST_PAGE_TABLE_OK) {
        commit_vma_candidate(memory, &candidate, retired);
        return first;
    }
    guest_linux_vma_set_destroy(&candidate);
    if (result == GUEST_PAGE_TABLE_ALREADY_MAPPED &&
            selection.no_replace)
        return linux_error(GUEST_LINUX_EEXIST);
    return linux_error(GUEST_LINUX_ENOMEM);
}

qword_t guest_linux_mmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t flags, qword_t fd, qword_t offset) {
    struct guest_linux_vma_set retired;
    guest_linux_vma_set_init(&retired);
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mmap_unlocked(memory,
            address, length, protection, flags, fd, offset, &retired);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired);
    return result;
}

static bool select_file_mmap_range(
        struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t flags, qword_t offset,
        struct mmap_selection *selection, unsigned *permissions,
        qword_t *mapped_size, qword_t *error) {
    if ((offset & GUEST_MEMORY_PAGE_MASK) != 0) {
        *error = linux_error(GUEST_LINUX_EINVAL);
        return false;
    }

    qword_t count;
    if (!page_count(length, &count)) {
        *error = length == 0 ? linux_error(GUEST_LINUX_EINVAL) :
                linux_error(GUEST_LINUX_ENOMEM);
        return false;
    }
    *mapped_size = count << GUEST_MEMORY_PAGE_BITS;
    qword_t page_offset = offset >> GUEST_MEMORY_PAGE_BITS;
    if (page_offset > UINT64_MAX - count) {
        *error = linux_error(GUEST_LINUX_EOVERFLOW);
        return false;
    }
    if (!decode_permissions(protection, permissions)) {
        *error = linux_error(GUEST_LINUX_EINVAL);
        return false;
    }

    if (!select_mmap_range(memory, address, length, flags,
            selection, error))
        return false;
    assert(selection->count == count);
    return true;
}

qword_t guest_linux_mmap_file_preflight(
        struct guest_linux_mm *memory, guest_addr_t address,
        qword_t length, qword_t protection, qword_t flags,
        qword_t offset) {
    assert(memory != NULL && memory->page_table != NULL);
    struct mmap_selection selection;
    unsigned permissions;
    qword_t mapped_size;
    qword_t error;
    bool locked = guest_page_table_write_lock(memory->page_table);
    bool selected = select_file_mmap_range(memory, address,
            length, protection, flags, offset, &selection,
            &permissions, &mapped_size, &error);
    guest_page_table_write_unlock(memory->page_table, locked);
    return selected ? selection.first : error;
}

static qword_t guest_linux_mmap_file_unlocked(
        struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t maximum_protection, qword_t flags,
        struct guest_file_pager *pager, qword_t offset,
        enum guest_linux_vma_source source,
        struct guest_linux_vma_set *retired) {
    assert(pager != NULL && file_vma_source(source));
    struct mmap_selection selection;
    unsigned permissions;
    qword_t mapped_size;
    qword_t error;
    if (!select_file_mmap_range(memory, address, length,
            protection, flags, offset, &selection, &permissions,
            &mapped_size, &error))
        return error;
    if (offset > UINT64_MAX - mapped_size)
        return linux_error(GUEST_LINUX_EOVERFLOW);
    qword_t required_type = source ==
            GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE ?
            GUEST_LINUX_MAP_PRIVATE : GUEST_LINUX_MAP_SHARED;
    qword_t allowed = required_type |
            GUEST_LINUX_MAP_FIXED | GUEST_LINUX_MAP_FIXED_NOREPLACE;
    qword_t mapping_type = flags & GUEST_LINUX_MAP_TYPE;
    if (mapping_type != required_type)
        return linux_error(mapping_type == GUEST_LINUX_MAP_SHARED ||
                mapping_type == GUEST_LINUX_MAP_PRIVATE ?
                GUEST_LINUX_EOPNOTSUPP : GUEST_LINUX_EINVAL);
    if ((flags & ~allowed) != 0)
        return linux_error(GUEST_LINUX_EINVAL);
    if ((maximum_protection & ~GUEST_LINUX_PROT_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);
    if ((protection & ~maximum_protection) != 0)
        return linux_error(GUEST_LINUX_EACCES);
    use(permissions);

    struct guest_linux_vma mapping = {
        .first = (guest_addr_t) selection.first,
        .last = (guest_addr_t) (selection.first + mapped_size),
        .protection = protection,
        .source = source,
        .maximum_protection = maximum_protection,
        .file_pager = pager,
        .file_offset = offset,
    };
    struct guest_linux_vma_set candidate;
    if (!prepare_vma_insert(memory, mapping, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);

    qword_t generation = memory->page_table->address_space.generation;
    if (selection.fixed && !selection.no_replace) {
        enum guest_page_table_result unmapped =
                guest_page_table_unmap_range(memory->page_table,
                        make_range(selection.first, selection.count), true);
        assert(unmapped == GUEST_PAGE_TABLE_OK);
    }
    commit_vma_candidate(memory, &candidate, retired);
    if (memory->page_table->address_space.generation == generation)
        guest_address_space_changed(
                &memory->page_table->address_space);
    return selection.first;
}

qword_t guest_linux_mmap_file_private(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t maximum_protection, qword_t flags,
        struct guest_file_pager *pager, qword_t offset) {
    assert(memory != NULL && memory->page_table != NULL && pager != NULL);
    struct guest_linux_vma_set retired;
    guest_linux_vma_set_init(&retired);
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mmap_file_unlocked(
            memory, address, length, protection,
            maximum_protection, flags, pager, offset,
            GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE, &retired);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired);
    return result;
}

qword_t guest_linux_mmap_file_shared(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t maximum_protection, qword_t flags,
        struct guest_file_pager *pager, qword_t offset) {
    assert(memory != NULL && memory->page_table != NULL && pager != NULL);
    struct guest_linux_vma_set retired;
    guest_linux_vma_set_init(&retired);
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mmap_file_unlocked(
            memory, address, length, protection,
            maximum_protection, flags, pager, offset,
            GUEST_LINUX_VMA_SOURCE_FILE_SHARED, &retired);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired);
    return result;
}

static qword_t guest_linux_munmap_unlocked(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length,
        struct guest_linux_vma_set *retired) {
    if (((qword_t) address & GUEST_MEMORY_PAGE_MASK) != 0 || length == 0)
        return linux_error(GUEST_LINUX_EINVAL);
    qword_t count;
    if (!page_count(length, &count) ||
            !valid_range(memory, address, count))
        return linux_error(GUEST_LINUX_EINVAL);
    guest_addr_t last = address +
            (count << GUEST_MEMORY_PAGE_BITS);
    struct guest_linux_vma_set candidate;
    bool update_vmas = vma_range_intersects(
            &memory->vmas, address, last);
    if (update_vmas &&
            !prepare_vma_remove(memory, address, last, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);
    qword_t generation = memory->page_table->address_space.generation;
    enum guest_page_table_result result = guest_page_table_unmap_range(
            memory->page_table, make_range(address, count), true);
    assert(result == GUEST_PAGE_TABLE_OK);
    if (update_vmas) {
        commit_vma_candidate(memory, &candidate, retired);
        if (memory->page_table->address_space.generation == generation)
            guest_address_space_changed(
                    &memory->page_table->address_space);
    }
    return 0;
}

qword_t guest_linux_munmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length) {
    struct guest_linux_vma_set retired;
    guest_linux_vma_set_init(&retired);
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_munmap_unlocked(
            memory, address, length, &retired);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired);
    return result;
}

static bool mremap_length(qword_t length, qword_t *count,
        qword_t *rounded) {
    if (length == 0) {
        *count = 0;
        *rounded = 0;
        return true;
    }
    if (!page_count(length, count))
        return false;
    *rounded = *count << GUEST_MEMORY_PAGE_BITS;
    return true;
}

static bool ranges_overlap(qword_t first, qword_t length,
        qword_t other_first, qword_t other_length) {
    if (length == 0 || other_length == 0)
        return false;
    qword_t end = length > UINT64_MAX - first ?
            UINT64_MAX : first + length;
    qword_t other_end = other_length > UINT64_MAX - other_first ?
            UINT64_MAX : other_first + other_length;
    return first < other_end && other_first < end;
}

static bool private_anonymous_vma(
        const struct guest_linux_vma *mapping) {
    return mapping->source == GUEST_LINUX_VMA_SOURCE_BRK ||
            mapping->source == GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE;
}

static bool shared_vma(const struct guest_linux_vma *mapping) {
    return mapping->source == GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED ||
            mapping->source == GUEST_LINUX_VMA_SOURCE_FILE_SHARED;
}

static bool mremap_file_offset_valid(
        const struct guest_linux_vma *mapping,
        qword_t old_address, qword_t new_size) {
    if (!file_vma_source(mapping->source))
        return true;
    qword_t delta = old_address - mapping->first;
    if (mapping->file_offset > UINT64_MAX - delta)
        return false;
    qword_t file_offset = mapping->file_offset + delta;
    // VMA 以字节保存文件偏移，整个映射跨度也必须保持可表示。
    return file_offset <= UINT64_MAX - new_size;
}

static struct guest_linux_vma make_mremap_mapping(
        const struct guest_linux_vma *source, qword_t old_address,
        qword_t destination, qword_t new_size, bool moved) {
    struct guest_linux_vma mapping = *source;
    mapping.first = (guest_addr_t) destination;
    mapping.last = (guest_addr_t) (destination + new_size);
    if (file_vma_source(mapping.source)) {
        qword_t delta = old_address - source->first;
        assert(mapping.file_offset <= UINT64_MAX - delta);
        mapping.file_offset += delta;
    } else {
        mapping.file_offset = 0;
        mapping.file_pager = NULL;
    }
    if (moved && mapping.source == GUEST_LINUX_VMA_SOURCE_BRK)
        mapping.source = GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE;
    return mapping;
}

static bool prepare_mremap_candidate(
        const struct guest_linux_mm *memory,
        qword_t old_address, qword_t old_size,
        struct guest_linux_vma destination, bool keep_source,
        struct guest_linux_vma_set *candidate) {
    if (!guest_linux_vma_set_clone(candidate, &memory->vmas))
        return false;
    if (!keep_source && old_size != 0 &&
            !guest_linux_vma_remove(candidate,
                    (guest_addr_t) old_address,
                    (guest_addr_t) (old_address + old_size))) {
        guest_linux_vma_set_destroy(candidate);
        return false;
    }
    if (!guest_linux_vma_insert(candidate, destination)) {
        guest_linux_vma_set_destroy(candidate);
        return false;
    }
    return true;
}

static bool select_mremap_destination(struct guest_linux_mm *memory,
        qword_t hint, qword_t count, bool use_hint,
        qword_t *destination) {
    if (use_hint && hint != 0 && range_is_hole(memory, hint, count)) {
        *destination = hint;
        return true;
    }
    return find_mmap_hole(memory, count, destination);
}

static qword_t shrink_mremap_in_place(struct guest_linux_mm *memory,
        qword_t old_address, qword_t old_size, qword_t new_size,
        struct guest_linux_vma_set *retired) {
    guest_addr_t tail = (guest_addr_t) (old_address + new_size);
    guest_addr_t old_end = (guest_addr_t) (old_address + old_size);
    struct guest_linux_vma_set candidate;
    if (!prepare_vma_remove(memory, tail, old_end, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);
    qword_t generation = memory->page_table->address_space.generation;
    enum guest_page_table_result unmapped = guest_page_table_unmap_range(
            memory->page_table,
            make_range(tail,
                    (old_size - new_size) >> GUEST_MEMORY_PAGE_BITS),
            true);
    assert(unmapped == GUEST_PAGE_TABLE_OK);
    commit_vma_candidate(memory, &candidate, retired);
    if (memory->page_table->address_space.generation == generation)
        guest_address_space_changed(&memory->page_table->address_space);
    return old_address;
}

static qword_t expand_mremap_in_place(struct guest_linux_mm *memory,
        const struct guest_linux_vma *source, qword_t old_address,
        qword_t old_count, qword_t old_size, qword_t new_count,
        qword_t new_size, unsigned permissions,
        struct guest_linux_vma_set *retired) {
    struct guest_linux_vma mapping = make_mremap_mapping(
            source, old_address, old_address, new_size, false);
    struct guest_linux_vma_set candidate;
    if (!prepare_vma_insert(memory, mapping, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);

    qword_t generation = memory->page_table->address_space.generation;
    if (private_anonymous_vma(source)) {
        enum guest_page_table_result mapped =
                guest_page_table_map_zero_range(memory->page_table,
                        make_range(old_address + old_size,
                                new_count - old_count),
                        permissions, false);
        if (mapped != GUEST_PAGE_TABLE_OK) {
            assert(mapped == GUEST_PAGE_TABLE_OUT_OF_MEMORY);
            guest_linux_vma_set_destroy(&candidate);
            return linux_error(GUEST_LINUX_ENOMEM);
        }
    }
    commit_vma_candidate(memory, &candidate, retired);
    if (memory->page_table->address_space.generation == generation)
        guest_address_space_changed(&memory->page_table->address_space);
    return old_address;
}

static qword_t move_mremap(struct guest_linux_mm *memory,
        const struct guest_linux_vma *source, qword_t old_address,
        qword_t old_count, qword_t old_size, qword_t new_count,
        qword_t new_size, qword_t destination, bool keep_source,
        bool zero_length_duplicate, unsigned permissions,
        struct guest_linux_vma_set *retired) {
    struct guest_linux_vma destination_mapping = make_mremap_mapping(
            source, old_address, destination, new_size, true);
    struct guest_linux_vma_set candidate;
    if (!prepare_mremap_candidate(memory, old_address, old_size,
            destination_mapping, keep_source, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);

    qword_t generation = memory->page_table->address_space.generation;
    enum guest_page_table_result moved = GUEST_PAGE_TABLE_OK;
    bool dontunmap = keep_source && !zero_length_duplicate;
    if (zero_length_duplicate) {
        if (source->source == GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED) {
            moved = guest_page_table_alias_shared_range(
                    memory->page_table,
                    make_range(old_address, new_count),
                    (guest_addr_t) destination);
        }
    } else if (dontunmap &&
            source->source == GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED) {
        moved = guest_page_table_alias_shared_range(
                memory->page_table, make_range(old_address, old_count),
                (guest_addr_t) destination);
    } else if (dontunmap && private_anonymous_vma(source)) {
        moved = guest_page_table_move_range_replace_zero(
                memory->page_table, make_range(old_address, old_count),
                (guest_addr_t) destination, permissions);
    } else if (new_count > old_count && private_anonymous_vma(source)) {
        moved = guest_page_table_move_range_expand_zero(
                memory->page_table, make_range(old_address, old_count),
                (guest_addr_t) destination, new_count, permissions);
    } else {
        qword_t moved_count = old_count < new_count ?
                old_count : new_count;
        moved = guest_page_table_move_range(memory->page_table,
                make_range(old_address, moved_count),
                (guest_addr_t) destination);
    }
    if (moved != GUEST_PAGE_TABLE_OK) {
        guest_linux_vma_set_destroy(&candidate);
        assert(moved == GUEST_PAGE_TABLE_OUT_OF_MEMORY);
        return linux_error(GUEST_LINUX_ENOMEM);
    }

    if (!keep_source && new_count < old_count) {
        enum guest_page_table_result unmapped =
                guest_page_table_unmap_range(memory->page_table,
                        make_range(old_address + new_size,
                                old_count - new_count), true);
        assert(unmapped == GUEST_PAGE_TABLE_OK);
    }
    commit_vma_candidate(memory, &candidate, retired);
    if (memory->page_table->address_space.generation == generation)
        guest_address_space_changed(&memory->page_table->address_space);
    return destination;
}

static qword_t guest_linux_mremap_unlocked(struct guest_linux_mm *memory,
        qword_t old_address, qword_t old_length, qword_t new_length,
        qword_t flags, qword_t new_address,
        struct guest_linux_vma_set *retired_target,
        struct guest_linux_vma_set *retired_shrink,
        struct guest_linux_vma_set *retired_result) {
    qword_t old_count;
    qword_t old_size;
    qword_t new_count;
    qword_t new_size;
    if (!mremap_length(old_length, &old_count, &old_size) ||
            !mremap_length(new_length, &new_count, &new_size))
        return linux_error(GUEST_LINUX_EINVAL);

    const qword_t allowed = GUEST_LINUX_MREMAP_MAYMOVE |
            GUEST_LINUX_MREMAP_FIXED |
            GUEST_LINUX_MREMAP_DONTUNMAP;
    if ((flags & ~allowed) != 0 ||
            (old_address & GUEST_MEMORY_PAGE_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);

    qword_t limit = address_limit(memory);
    if (new_size == 0 || new_size > limit)
        return linux_error(GUEST_LINUX_EINVAL);
    bool maymove = (flags & GUEST_LINUX_MREMAP_MAYMOVE) != 0;
    bool fixed = (flags & GUEST_LINUX_MREMAP_FIXED) != 0;
    bool dontunmap = (flags & GUEST_LINUX_MREMAP_DONTUNMAP) != 0;
    bool implied_address = fixed || dontunmap;
    if (implied_address &&
            (new_address > limit - new_size ||
            (new_address & GUEST_MEMORY_PAGE_MASK) != 0 || !maymove ||
            (dontunmap && old_size != new_size) ||
            ranges_overlap(old_address, old_size,
                    new_address, new_size)))
        return linux_error(GUEST_LINUX_EINVAL);
    if (old_count != 0 && !valid_range(memory, old_address, old_count))
        return linux_error(GUEST_LINUX_EFAULT);

    const struct guest_linux_vma *source = guest_linux_vma_find(
            &memory->vmas, (guest_addr_t) old_address);
    if (source == NULL)
        return linux_error(GUEST_LINUX_EFAULT);
    bool zero_length_duplicate = old_count == 0;
    if (zero_length_duplicate && !shared_vma(source))
        return linux_error(GUEST_LINUX_EINVAL);

    bool shrink = old_size > new_size;
    bool expand = old_size < new_size;
    if (!expand && !shrink && !implied_address)
        return old_address;
    qword_t checked_size = shrink ? new_size : old_size;
    if (checked_size > (qword_t) source->last - old_address)
        return linux_error(GUEST_LINUX_EFAULT);
    if (!mremap_file_offset_valid(source, old_address, new_size))
        return linux_error(GUEST_LINUX_EINVAL);
    if (expand &&
            source->source == GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED &&
            (!zero_length_duplicate ||
                    new_size > (qword_t) source->last - old_address))
        return linux_error(GUEST_LINUX_EFAULT);

    unsigned permissions;
    bool decoded = decode_permissions(source->protection, &permissions);
    assert(decoded);
    if (shrink && !implied_address)
        return shrink_mremap_in_place(memory, old_address,
                old_size, new_size, retired_result);

    bool can_expand_in_place = expand && !implied_address &&
            !zero_length_duplicate &&
            old_address + old_size == source->last &&
            range_is_hole(memory, old_address + old_size,
                    new_count - old_count) &&
            source->source != GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED;
    if (can_expand_in_place)
        return expand_mremap_in_place(memory, source, old_address,
                old_count, old_size, new_count, new_size,
                permissions, retired_result);
    if (!maymove)
        return linux_error(GUEST_LINUX_ENOMEM);

    qword_t destination;
    if (fixed) {
        destination = new_address;
        qword_t unmapped = guest_linux_munmap_unlocked(memory,
                (guest_addr_t) destination, new_size, retired_target);
        if ((sqword_t) unmapped < 0)
            return unmapped;
        source = guest_linux_vma_find(
                &memory->vmas, (guest_addr_t) old_address);
        if (source == NULL || checked_size >
                (qword_t) source->last - old_address)
            return linux_error(GUEST_LINUX_EFAULT);
        decoded = decode_permissions(source->protection, &permissions);
        assert(decoded);
        if (shrink) {
            qword_t shrunk = shrink_mremap_in_place(memory,
                    old_address, old_size, new_size, retired_shrink);
            if ((sqword_t) shrunk < 0)
                return shrunk;
            old_count = new_count;
            old_size = new_size;
            source = guest_linux_vma_find(
                    &memory->vmas, (guest_addr_t) old_address);
            assert(source != NULL && old_size <=
                    (qword_t) source->last - old_address);
        }
    } else if (!select_mremap_destination(memory, new_address,
            new_count, dontunmap, &destination)) {
        return linux_error(GUEST_LINUX_ENOMEM);
    }

    return move_mremap(memory, source, old_address, old_count,
            old_size, new_count, new_size, destination,
            dontunmap || zero_length_duplicate,
            zero_length_duplicate, permissions, retired_result);
}

qword_t guest_linux_mremap(struct guest_linux_mm *memory,
        qword_t old_address, qword_t old_length, qword_t new_length,
        qword_t flags, qword_t new_address) {
    assert(memory != NULL && memory->page_table != NULL);
    struct guest_linux_vma_set retired_target;
    struct guest_linux_vma_set retired_shrink;
    struct guest_linux_vma_set retired_result;
    guest_linux_vma_set_init(&retired_target);
    guest_linux_vma_set_init(&retired_shrink);
    guest_linux_vma_set_init(&retired_result);
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mremap_unlocked(memory,
            old_address, old_length, new_length, flags, new_address,
            &retired_target, &retired_shrink, &retired_result);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired_target);
    guest_linux_vma_set_destroy(&retired_shrink);
    guest_linux_vma_set_destroy(&retired_result);
    return result;
}

static qword_t guest_linux_mprotect_unlocked(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        struct guest_linux_vma_set *retired) {
    if (((qword_t) address & GUEST_MEMORY_PAGE_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);
    if (length == 0)
        return 0;
    unsigned permissions;
    if (!decode_permissions(protection, &permissions))
        return linux_error(GUEST_LINUX_EINVAL);
    qword_t count;
    if (!page_count(length, &count) ||
            !valid_range(memory, address, count))
        return linux_error(GUEST_LINUX_ENOMEM);
    enum mprotect_range_status status = check_mprotect_range(
            memory, address, count, protection);
    if (status == MPROTECT_RANGE_HOLE)
        return linux_error(GUEST_LINUX_ENOMEM);
    if (status == MPROTECT_RANGE_DENIED)
        return linux_error(GUEST_LINUX_EACCES);
    guest_addr_t last = address +
            (count << GUEST_MEMORY_PAGE_BITS);
    struct guest_linux_vma_set candidate;
    bool update_vmas = vma_protection_changes(
            &memory->vmas, address, last, protection);
    if (update_vmas && !prepare_vma_protect(
            memory, address, last, protection, &candidate))
        return linux_error(GUEST_LINUX_ENOMEM);
    qword_t generation = memory->page_table->address_space.generation;
    enum guest_page_table_result result =
            guest_page_table_protect_present_range(
            memory->page_table, make_range(address, count), permissions);
    assert(result == GUEST_PAGE_TABLE_OK);
    if (update_vmas) {
        commit_vma_candidate(memory, &candidate, retired);
        if (memory->page_table->address_space.generation == generation)
            guest_address_space_changed(
                    &memory->page_table->address_space);
    }
    return 0;
}

qword_t guest_linux_mprotect(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection) {
    struct guest_linux_vma_set retired;
    guest_linux_vma_set_init(&retired);
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mprotect_unlocked(
            memory, address, length, protection, &retired);
    guest_page_table_write_unlock(memory->page_table, locked);
    guest_linux_vma_set_destroy(&retired);
    return result;
}

static qword_t file_sync_error(enum guest_file_sync_result result) {
    switch (result) {
        case GUEST_FILE_SYNC_OK:
            return 0;
        case GUEST_FILE_SYNC_IO_ERROR:
            return linux_error(GUEST_LINUX_EIO);
        case GUEST_FILE_SYNC_UNSUPPORTED:
            return linux_error(GUEST_LINUX_EINVAL);
    }
    assert(false);
    return linux_error(GUEST_LINUX_EIO);
}

static const struct guest_linux_vma *vma_at_or_after(
        const struct guest_linux_vma_set *set, guest_addr_t address) {
    size_t first = 0;
    size_t count = set->count;
    while (count != 0) {
        size_t step = count / 2;
        size_t index = first + step;
        if (set->entries[index].last <= address) {
            first = index + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    return first < set->count ? &set->entries[first] : NULL;
}

qword_t guest_linux_msync(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, dword_t flags) {
    const dword_t allowed = GUEST_LINUX_MS_ASYNC |
            GUEST_LINUX_MS_INVALIDATE | GUEST_LINUX_MS_SYNC;
    if ((flags & ~allowed) != 0 ||
            ((qword_t) address & GUEST_MEMORY_PAGE_MASK) != 0 ||
            ((flags & GUEST_LINUX_MS_ASYNC) != 0 &&
                    (flags & GUEST_LINUX_MS_SYNC) != 0))
        return linux_error(GUEST_LINUX_EINVAL);

    qword_t rounded_length = (length + GUEST_MEMORY_PAGE_MASK) &
            ~GUEST_MEMORY_PAGE_MASK;
    qword_t end = (qword_t) address + rounded_length;
    if (end < (qword_t) address)
        return linux_error(GUEST_LINUX_ENOMEM);
    if (end == (qword_t) address)
        return 0;

    bool unmapped = false;
    qword_t cursor = address;
    qword_t limit = address_limit(memory);
    while (cursor < end) {
        if (cursor >= limit)
            return linux_error(GUEST_LINUX_ENOMEM);

        struct guest_file_pager *pager = NULL;
        qword_t file_offset = 0;
        qword_t sync_length = 0;
        qword_t next = cursor + GUEST_MEMORY_PAGE_SIZE;
        bool locked = guest_page_table_read_lock(memory->page_table);
        const struct guest_linux_vma *mapping = guest_linux_vma_find(
                &memory->vmas, (guest_addr_t) cursor);
        if (mapping != NULL) {
            next = mapping->last < end ? mapping->last : end;
            /* Linux 会为不可写 fd 清除 VM_SHARED，仅保留 MAYSHARE。 */
            if ((flags & GUEST_LINUX_MS_SYNC) != 0 &&
                    mapping->source ==
                            GUEST_LINUX_VMA_SOURCE_FILE_SHARED &&
                    (mapping->maximum_protection &
                            GUEST_LINUX_PROT_WRITE) != 0) {
                qword_t delta = cursor - mapping->first;
                assert(mapping->file_offset <= UINT64_MAX - delta);
                file_offset = mapping->file_offset + delta;
                sync_length = next - cursor;
                pager = guest_file_pager_retain(
                        mapping->file_pager);
            }
        } else if (!page_is_mapped(memory, cursor)) {
            unmapped = true;
            const struct guest_linux_vma *following =
                    vma_at_or_after(&memory->vmas,
                            (guest_addr_t) cursor);
            next = following == NULL || following->first >= end ?
                    end : following->first;
        }
        guest_page_table_read_unlock(memory->page_table, locked);

        if (pager != NULL) {
            enum guest_file_sync_result sync =
                    guest_file_pager_sync_range(
                            pager, file_offset, sync_length);
            guest_file_pager_release(pager);
            qword_t error = file_sync_error(sync);
            if (error != 0)
                return error;
        }
        if (unmapped && flags == GUEST_LINUX_MS_ASYNC)
            return linux_error(GUEST_LINUX_ENOMEM);
        cursor = next;
    }
    return unmapped ? linux_error(GUEST_LINUX_ENOMEM) : 0;
}

static bool madvise_supported(dword_t advice) {
    return advice == GUEST_LINUX_MADV_NORMAL ||
            advice == GUEST_LINUX_MADV_RANDOM ||
            advice == GUEST_LINUX_MADV_SEQUENTIAL ||
            advice == GUEST_LINUX_MADV_WILLNEED ||
            advice == GUEST_LINUX_MADV_DONTNEED;
}

static void madvise_dontneed_vma(struct guest_linux_mm *memory,
        const struct guest_linux_vma *mapping,
        guest_addr_t first, guest_addr_t last) {
    assert(mapping != NULL && first >= mapping->first &&
            last <= mapping->last && first < last);
    if (mapping->source == GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED) {
        // 当前模型没有独立于共享 backing 的 PTE/RSS 驻留层。
        return;
    }
    if (mapping->source == GUEST_LINUX_VMA_SOURCE_FILE_PRIVATE ||
            mapping->source == GUEST_LINUX_VMA_SOURCE_FILE_SHARED) {
        struct guest_page_range range = {
            .first = first,
            .page_count = (last - first) >> GUEST_MEMORY_PAGE_BITS,
        };
        enum guest_page_table_result result =
                guest_page_table_unmap_range(
                        memory->page_table, range, true);
        assert(result == GUEST_PAGE_TABLE_OK);
        return;
    }

    assert(mapping->source == GUEST_LINUX_VMA_SOURCE_BRK ||
            mapping->source ==
                    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE);
    for (guest_addr_t page = first; page < last;
            page += GUEST_MEMORY_PAGE_SIZE) {
        byte_t *host_page;
        unsigned permissions;
        enum guest_page_table_result lookup =
                guest_page_table_lookup(memory->page_table,
                        page, &host_page, &permissions);
        assert(lookup == GUEST_PAGE_TABLE_OK ||
                lookup == GUEST_PAGE_TABLE_NOT_MAPPED);
        if (lookup == GUEST_PAGE_TABLE_NOT_MAPPED)
            continue;
        use(permissions);
        memset(host_page, 0, GUEST_MEMORY_PAGE_SIZE);
        guest_address_space_written(
                &memory->page_table->address_space,
                page, GUEST_MEMORY_PAGE_SIZE);
    }
}

static qword_t guest_linux_madvise_unlocked(
        struct guest_linux_mm *memory, guest_addr_t address,
        qword_t length, dword_t advice) {
    if (!madvise_supported(advice) ||
            ((qword_t) address & GUEST_MEMORY_PAGE_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);
    if (length == 0)
        return 0;

    qword_t count;
    if (!page_count(length, &count))
        return linux_error(GUEST_LINUX_EINVAL);
    if (!valid_range(memory, address, count))
        return linux_error(GUEST_LINUX_ENOMEM);

    qword_t end = (qword_t) address +
            (count << GUEST_MEMORY_PAGE_BITS);
    bool unmapped = false;
    qword_t cursor = address;
    while (cursor < end) {
        const struct guest_linux_vma *mapping = vma_at_or_after(
                &memory->vmas, (guest_addr_t) cursor);
        if (mapping == NULL || cursor < mapping->first) {
            qword_t gap_end = mapping == NULL || mapping->first >= end ?
                    end : mapping->first;
            while (cursor < gap_end) {
                if (page_is_mapped(memory, cursor)) {
                    if (advice == GUEST_LINUX_MADV_DONTNEED)
                        return linux_error(GUEST_LINUX_EINVAL);
                } else {
                    unmapped = true;
                }
                cursor += GUEST_MEMORY_PAGE_SIZE;
            }
            continue;
        }

        guest_addr_t first = (guest_addr_t) cursor;
        guest_addr_t last = mapping->last < end ?
                mapping->last : (guest_addr_t) end;
        if (advice == GUEST_LINUX_MADV_DONTNEED)
            madvise_dontneed_vma(memory, mapping, first, last);
        cursor = last;
    }
    return unmapped ? linux_error(GUEST_LINUX_ENOMEM) : 0;
}

qword_t guest_linux_madvise(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, dword_t advice) {
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_madvise_unlocked(
            memory, address, length, advice);
    guest_page_table_write_unlock(memory->page_table, locked);
    return result;
}

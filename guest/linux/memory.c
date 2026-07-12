#include <assert.h>

#include "guest/linux/errno.h"
#include "guest/linux/memory.h"
#include "guest/linux/mman.h"

// standalone 页表按页立即分配 backing，有限 arena 同时约束搜索与单次映射成本。
#define GUEST_LINUX_MMAP_ARENA_SIZE (UINT64_C(128) * 1024 * 1024)

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

static bool range_is_hole(struct guest_linux_mm *memory,
        qword_t first, qword_t count) {
    if (!valid_range(memory, first, count))
        return false;
    for (qword_t i = 0; i < count; i++) {
        if (page_is_mapped(memory,
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
            if (page_is_mapped(memory, page)) {
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

static bool decode_permissions(qword_t protection,
        unsigned *permissions) {
    qword_t supported = GUEST_LINUX_PROT_READ |
            GUEST_LINUX_PROT_WRITE | GUEST_LINUX_PROT_EXEC;
    if ((protection & ~supported) != 0)
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
    };
}

static guest_addr_t guest_linux_brk_unlocked(struct guest_linux_mm *memory,
        guest_addr_t requested) {
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

    if (new_end > old_end) {
        struct guest_page_range range = make_range(old_end,
                (new_end - old_end) >> GUEST_MEMORY_PAGE_BITS);
        if (guest_page_table_map_zero_range(memory->page_table, range,
                GUEST_MEMORY_READ | GUEST_MEMORY_WRITE, false) !=
                GUEST_PAGE_TABLE_OK)
            return memory->brk;
    } else if (new_end < old_end) {
        struct guest_page_range range = make_range(new_end,
                (old_end - new_end) >> GUEST_MEMORY_PAGE_BITS);
        assert(guest_page_table_unmap_range(
                memory->page_table, range, true) == GUEST_PAGE_TABLE_OK);
    }

    memory->brk = requested;
    return memory->brk;
}

guest_addr_t guest_linux_brk(struct guest_linux_mm *memory,
        guest_addr_t requested) {
    bool locked = guest_page_table_write_lock(memory->page_table);
    guest_addr_t result = guest_linux_brk_unlocked(memory, requested);
    guest_page_table_write_unlock(memory->page_table, locked);
    return result;
}

static qword_t guest_linux_mmap_unlocked(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t flags, qword_t fd, qword_t offset) {
    use(fd);
    unsigned permissions;
    if (!decode_permissions(protection, &permissions))
        return linux_error(GUEST_LINUX_EINVAL);

    qword_t allowed = GUEST_LINUX_MAP_PRIVATE | GUEST_LINUX_MAP_FIXED |
            GUEST_LINUX_MAP_ANONYMOUS |
            GUEST_LINUX_MAP_FIXED_NOREPLACE;
    if ((flags & ~allowed) != 0 ||
            (flags & GUEST_LINUX_MAP_TYPE) != GUEST_LINUX_MAP_PRIVATE ||
            (flags & GUEST_LINUX_MAP_ANONYMOUS) == 0 ||
            (offset & GUEST_MEMORY_PAGE_MASK) != 0)
        return linux_error(GUEST_LINUX_EINVAL);

    qword_t count;
    if (!page_count(length, &count))
        return length == 0 ? linux_error(GUEST_LINUX_EINVAL) :
                linux_error(GUEST_LINUX_ENOMEM);
    if (count > (GUEST_LINUX_MMAP_ARENA_SIZE >>
            GUEST_MEMORY_PAGE_BITS))
        return linux_error(GUEST_LINUX_ENOMEM);

    bool no_replace =
            (flags & GUEST_LINUX_MAP_FIXED_NOREPLACE) != 0;
    bool fixed = no_replace || (flags & GUEST_LINUX_MAP_FIXED) != 0;
    qword_t first;
    if (fixed) {
        first = address;
        if ((first & GUEST_MEMORY_PAGE_MASK) != 0)
            return linux_error(GUEST_LINUX_EINVAL);
        if (!valid_range(memory, first, count))
            return linux_error(GUEST_LINUX_ENOMEM);
    } else {
        qword_t hint = (qword_t) address & ~GUEST_MEMORY_PAGE_MASK;
        if (address != 0 && range_is_in_mmap_arena(memory, hint, count) &&
                range_is_hole(memory, hint, count)) {
            first = hint;
        } else if (!find_mmap_hole(memory, count, &first)) {
            return linux_error(GUEST_LINUX_ENOMEM);
        }
    }

    enum guest_page_table_result result = guest_page_table_map_zero_range(
            memory->page_table, make_range(first, count), permissions,
            fixed && !no_replace);
    if (result == GUEST_PAGE_TABLE_OK)
        return first;
    if (result == GUEST_PAGE_TABLE_ALREADY_MAPPED && no_replace)
        return linux_error(GUEST_LINUX_EEXIST);
    return linux_error(GUEST_LINUX_ENOMEM);
}

qword_t guest_linux_mmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t flags, qword_t fd, qword_t offset) {
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mmap_unlocked(memory,
            address, length, protection, flags, fd, offset);
    guest_page_table_write_unlock(memory->page_table, locked);
    return result;
}

static qword_t guest_linux_munmap_unlocked(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length) {
    if (((qword_t) address & GUEST_MEMORY_PAGE_MASK) != 0 || length == 0)
        return linux_error(GUEST_LINUX_EINVAL);
    qword_t count;
    if (!page_count(length, &count) ||
            !valid_range(memory, address, count))
        return linux_error(GUEST_LINUX_EINVAL);
    enum guest_page_table_result result = guest_page_table_unmap_range(
            memory->page_table, make_range(address, count), true);
    assert(result == GUEST_PAGE_TABLE_OK);
    return 0;
}

qword_t guest_linux_munmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length) {
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_munmap_unlocked(memory, address, length);
    guest_page_table_write_unlock(memory->page_table, locked);
    return result;
}

static qword_t guest_linux_mprotect_unlocked(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection) {
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
    enum guest_page_table_result result = guest_page_table_protect_range(
            memory->page_table, make_range(address, count), permissions);
    if (result == GUEST_PAGE_TABLE_NOT_MAPPED)
        return linux_error(GUEST_LINUX_ENOMEM);
    assert(result == GUEST_PAGE_TABLE_OK);
    return 0;
}

qword_t guest_linux_mprotect(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection) {
    bool locked = guest_page_table_write_lock(memory->page_table);
    qword_t result = guest_linux_mprotect_unlocked(
            memory, address, length, protection);
    guest_page_table_write_unlock(memory->page_table, locked);
    return result;
}

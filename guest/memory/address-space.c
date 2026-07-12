#include <assert.h>
#include <stdint.h>

#include "guest/memory/address-space.h"

void guest_address_space_init(struct guest_address_space *space,
        const struct guest_address_space_ops *ops, void *opaque,
        byte_t address_bits) {
    assert(ops != NULL && ops->resolve_page != NULL);
    assert((ops->read_lock == NULL) == (ops->read_unlock == NULL));
    assert((ops->write_lock == NULL) == (ops->write_unlock == NULL));
    assert(address_bits > GUEST_MEMORY_PAGE_BITS);
    assert(address_bits <= sizeof(guest_addr_t) * 8);
    *space = (struct guest_address_space) {
        .ops = ops,
        .opaque = opaque,
        .generation = 1,
        .address_bits = address_bits,
    };
}

void guest_address_space_changed(struct guest_address_space *space) {
    space->generation++;
}

bool guest_address_space_read_lock(struct guest_address_space *space) {
    return space->ops->read_lock != NULL &&
            space->ops->read_lock(space->opaque);
}

void guest_address_space_read_unlock(
        struct guest_address_space *space, bool locked) {
    if (space->ops->read_unlock != NULL)
        space->ops->read_unlock(space->opaque, locked);
}

bool guest_address_space_write_lock(struct guest_address_space *space) {
    return space->ops->write_lock != NULL &&
            space->ops->write_lock(space->opaque);
}

void guest_address_space_write_unlock(
        struct guest_address_space *space, bool locked) {
    if (space->ops->write_unlock != NULL)
        space->ops->write_unlock(space->opaque, locked);
}

bool guest_address_space_contains(const struct guest_address_space *space,
        guest_addr_t address, size_t size) {
    qword_t max_address = space->address_bits == 64 ?
            UINT64_MAX : (UINT64_C(1) << space->address_bits) - 1;
    qword_t first = address;

    if (first > max_address)
        return false;
    if (size == 0)
        return true;
    return (qword_t) (size - 1) <= max_address - first;
}

enum guest_memory_fault_kind guest_address_space_resolve_page(
        struct guest_address_space *space, guest_addr_t page_base,
        enum guest_memory_access access, struct guest_page_view *view) {
    assert(access == GUEST_MEMORY_READ || access == GUEST_MEMORY_WRITE ||
            access == GUEST_MEMORY_EXECUTE);
    if ((page_base & GUEST_MEMORY_PAGE_MASK) != 0)
        return GUEST_MEMORY_FAULT_ALIGNMENT;
    if (!guest_address_space_contains(space, page_base, GUEST_MEMORY_PAGE_SIZE))
        return GUEST_MEMORY_FAULT_ADDRESS_SIZE;

    enum guest_memory_fault_kind fault = space->ops->resolve_page(
            space->opaque, page_base, access, view);
    if (fault != GUEST_MEMORY_FAULT_NONE)
        return fault;

    assert(view->host_page != NULL);
    if ((view->permissions & access) == 0)
        return GUEST_MEMORY_FAULT_PERMISSION;
    return GUEST_MEMORY_FAULT_NONE;
}

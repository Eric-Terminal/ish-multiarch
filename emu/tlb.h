#ifndef TLB_H
#define TLB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "emu/mmu.h"
#include "debug.h"

#if !defined(ISH_GUEST_I386)
#error "旧版 TLB 仅支持 i386 guest"
#endif

struct tlb_entry {
    page_t page;
    page_t page_if_writable;
    // AArch64 gadget 始终按 64 位读取该字段，ILP32 宿主也必须保持相同布局。
    uint64_t data_minus_addr;
};
_Static_assert(offsetof(struct tlb_entry, page) == 0,
        "TLB guest 页标签必须位于第 0 字节");
_Static_assert(offsetof(struct tlb_entry, page_if_writable) == 4,
        "TLB 可写页标签必须位于第 4 字节");
_Static_assert(offsetof(struct tlb_entry, data_minus_addr) == 8,
        "TLB 宿主页偏移必须位于第 8 字节");
_Static_assert(sizeof(((struct tlb_entry *) 0)->data_minus_addr) == 8,
        "TLB 宿主页偏移必须为 64 位");
_Static_assert(_Alignof(struct tlb_entry) >= 8,
        "TLB 缓存项必须至少按 8 字节对齐");
_Static_assert(sizeof(struct tlb_entry) == 16,
        "TLB 缓存项必须保持 16 字节");
#define TLB_BITS 10
#define TLB_SIZE (1 << TLB_BITS)
_Static_assert(sizeof(page_t) == 4, "旧版 TLB 页标签必须为 32 位");
_Static_assert(PAGE_BITS == 12, "旧版 TLB 仅支持 4 KiB guest 页");
_Static_assert(TLB_BITS == 10, "宿主 gadget 要求 10 位 TLB 索引");
_Static_assert((TLB_SIZE & (TLB_SIZE - 1)) == 0,
        "TLB 容量必须为二次幂");
struct tlb {
    struct mmu *mmu;
    page_t dirty_page;
    uint64_t mem_changes;
    // this is basically one of the return values of tlb_handle_miss, tlb_{read,write}, and __tlb_{read,write}_cross_page
    // yes, this sucks
    addr_t segfault_addr;
    struct tlb_entry entries[TLB_SIZE];
};
_Static_assert(sizeof(((struct tlb *) 0)->mem_changes) ==
        sizeof(((struct mmu *) 0)->changes),
        "TLB 与 MMU 的映射版本必须等宽");
_Static_assert(offsetof(struct tlb, entries) % _Alignof(struct tlb_entry) == 0,
        "TLB 缓存项数组未正确对齐");
_Static_assert(sizeof(uintptr_t) <= sizeof(uint64_t),
        "宿主指针无法存入 TLB 偏移字段");
_Static_assert(sizeof(addr_t) <= sizeof(uint64_t),
        "guest 地址无法参与 TLB 偏移运算");

#define TLB_INDEX(addr) (((addr >> PAGE_BITS) & (TLB_SIZE - 1)) ^ (addr >> (PAGE_BITS + TLB_BITS)))
#define TLB_PAGE(addr) (addr & 0xfffff000)
#define TLB_PAGE_EMPTY 1
void tlb_refresh(struct tlb *tlb, struct mmu *mmu);
void tlb_free(struct tlb *tlb);
void tlb_flush(struct tlb *tlb);
void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type);

forceinline __no_instrument void *tlb_host_pointer(uint64_t data_minus_addr, addr_t addr) {
    return (void *) (uintptr_t) (data_minus_addr + (uint64_t) addr);
}

forceinline __no_instrument void *__tlb_read_ptr(struct tlb *tlb, addr_t addr) {
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)) {
        void *address = tlb_host_pointer(entry.data_minus_addr, addr);
        posit(address != NULL);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_READ);
}
bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *out, unsigned size);
forceinline __no_instrument bool tlb_read(struct tlb *tlb, addr_t addr, void *out, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_read_cross_page(tlb, addr, out, size);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(out, ptr, size);
    return true;
}

forceinline __no_instrument void *__tlb_write_ptr(struct tlb *tlb, addr_t addr) {
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page_if_writable == TLB_PAGE(addr)) {
        tlb->dirty_page = TLB_PAGE(addr);
        void *address = tlb_host_pointer(entry.data_minus_addr, addr);
        posit(address != NULL);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_WRITE);
}
bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size);
forceinline __no_instrument bool tlb_write(struct tlb *tlb, addr_t addr, const void *value, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_write_cross_page(tlb, addr, value, size);
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(ptr, value, size);
    return true;
}

#endif

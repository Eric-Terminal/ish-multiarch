#ifndef GUEST_LINUX_MEMORY_H
#define GUEST_LINUX_MEMORY_H

#include "guest/linux/membarrier.h"
#include "guest/memory/page-table.h"

// 固定 arena 限制搜索成本，并让每个匿名页可由 4 KiB 位图追踪。
#define GUEST_LINUX_MMAP_ARENA_SIZE (UINT64_C(128) * 1024 * 1024)
#define GUEST_LINUX_MMAP_ANONYMOUS_BITMAP_SIZE \
    ((GUEST_LINUX_MMAP_ARENA_SIZE >> GUEST_MEMORY_PAGE_BITS) / 8)

struct guest_linux_mm {
    struct guest_page_table *page_table;
    guest_addr_t start_brk;
    guest_addr_t brk;
    guest_addr_t brk_limit;
    qword_t mmap_base;
    qword_t mmap_limit;
    dword_t membarrier_registrations;
    // 每一位记录 arena 对应页是否由匿名 mmap 成功创建。
    byte_t mmap_anonymous_bitmap[
            GUEST_LINUX_MMAP_ANONYMOUS_BITMAP_SIZE];
};

void guest_linux_mm_init(struct guest_linux_mm *memory,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit);
bool guest_linux_mm_clone(struct guest_linux_mm *destination,
        struct guest_page_table *destination_page_table,
        const struct guest_linux_mm *source);
qword_t guest_linux_membarrier(struct guest_linux_mm *memory,
        sdword_t command, dword_t flags);
guest_addr_t guest_linux_brk(struct guest_linux_mm *memory,
        guest_addr_t requested);
qword_t guest_linux_mmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t flags, qword_t fd, qword_t offset);
qword_t guest_linux_munmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length);
qword_t guest_linux_mprotect(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection);
qword_t guest_linux_madvise(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, dword_t advice);

#endif

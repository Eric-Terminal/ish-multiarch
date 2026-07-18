#ifndef GUEST_LINUX_MEMORY_H
#define GUEST_LINUX_MEMORY_H

#include "guest/linux/membarrier.h"
#include "guest/linux/vma.h"
#include "guest/memory/page-table.h"

// 固定 arena 只限制非 fixed 映射的搜索成本，不限制 VMA 表达范围。
#define GUEST_LINUX_MMAP_ARENA_SIZE (UINT64_C(128) * 1024 * 1024)

struct guest_linux_mm {
    struct guest_page_table *page_table;
    guest_addr_t start_brk;
    guest_addr_t brk;
    guest_addr_t brk_limit;
    qword_t mmap_base;
    qword_t mmap_limit;
    // VMA 语义每次成功替换后递增，用于拒绝锁外 page-in 的陈旧提交。
    qword_t vma_sequence;
    dword_t membarrier_registrations;
    struct guest_linux_vma_set vmas;
};

void guest_linux_mm_init(struct guest_linux_mm *memory,
        struct guest_page_table *page_table, guest_addr_t start_brk,
        guest_addr_t brk_limit);
// 接受全零状态且可重复调用；page_table 的所有权仍属于调用方。
void guest_linux_mm_destroy(struct guest_linux_mm *memory);
// 两个 destination 均须未初始化，并且不得与 source 的所有权别名。
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
// 仅执行 do_mmap 的通用参数与地址选择检查，不修改 VMA 或页表。
qword_t guest_linux_mmap_file_preflight(
        struct guest_linux_mm *memory, guest_addr_t address,
        qword_t length, qword_t protection, qword_t flags,
        qword_t offset);
qword_t guest_linux_mmap_file_private(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t maximum_protection, qword_t flags,
        struct guest_file_pager *pager, qword_t offset);
qword_t guest_linux_mmap_file_shared(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection,
        qword_t maximum_protection, qword_t flags,
        struct guest_file_pager *pager, qword_t offset);
qword_t guest_linux_munmap(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length);
qword_t guest_linux_mprotect(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, qword_t protection);
qword_t guest_linux_msync(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, dword_t flags);
qword_t guest_linux_madvise(struct guest_linux_mm *memory,
        guest_addr_t address, qword_t length, dword_t advice);

#endif

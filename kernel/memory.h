#ifndef MEMORY_H
#define MEMORY_H

#include <stdatomic.h>
#include <unistd.h>
#include <stdbool.h>
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"
#include "misc.h"

struct mem {
    // 单调且不复用；PRIVATE futex 不把 host 指针当作身份。
    qword_t identity;
    struct pt_entry **pgdir;
    int pgdir_used;

    struct mmu mmu;

    wrlock_t lock;
};
#define MEM_PGDIR_SIZE (1 << 10)

// Initialize the address space
void mem_init(struct mem *mem);
// Uninitialize the address space
void mem_destroy(struct mem *mem);
// Return the pagetable entry for the given page
struct pt_entry *mem_pt(struct mem *mem, page_t page);
// Increment *page, skipping over unallocated page directories. Intended to be
// used as the incremenent in a for loop to traverse mappings.
void mem_next_page(struct mem *mem, page_t *page);

#define BYTES_ROUND_DOWN(bytes) (PAGE(bytes) << PAGE_BITS)
#define BYTES_ROUND_UP(bytes) (PAGE_ROUND_UP(bytes) << PAGE_BITS)

#define LEAK_DEBUG 0

struct data {
    // 匿名共享后备释放后，仍存活的 futex 键不得与新对象发生 ABA。
    qword_t identity;
    void *data; // immutable
    size_t size; // also immutable
    atomic_uint refcount;

    // for display in /proc/pid/maps
    struct fd *fd;
    qword_t file_offset;
    // data[0] 对应的真实文件字节偏移，独立于 Apple host 页大小。
    qword_t file_backing_offset;
    /* realfs MAP_SHARED token；最后一份 data 引用释放时归还。 */
    struct inode_data *host_shared_mapping_inode;
    const char *name;
#if LEAK_DEBUG
    int pid;
    addr_t dest;
#endif
};
struct pt_entry {
    struct data *data;
    size_t offset;
    unsigned flags;
    struct list blocks[2];
};
// page flags
// P_READ and P_EXEC are ignored for now
#define P_READ (1 << 0)
#define P_WRITE (1 << 1)
#undef P_EXEC // defined in sys/proc.h on darwin
#define P_EXEC (1 << 2)
#define P_RWX (P_READ | P_WRITE | P_EXEC)
#define P_GROWSDOWN (1 << 3)
#define P_COW (1 << 4)
#define P_SPECIAL (1 << 5)
#define P_WRITABLE(flags) (flags & P_WRITE && !(flags & P_COW))

// mapping was created with pt_map_nothing
#define P_ANONYMOUS (1 << 6)
// mapping was created with MAP_SHARED, should not CoW
#define P_SHARED (1 << 7)
// 当前页内容仍直接来自文件后备；COW 后清除，但 VMA 展示元数据继续保留。
#define P_FILE_BACKED (1 << 8)

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages);
page_t pt_find_hole(struct mem *mem, pages_t size);

// Map memory + offset into fake memory, unmapping existing mappings. 除
// MAP_FAILED 外始终接管 memory；成功析构或失败回滚均以相同长度 munmap。
int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags);
/* 仅供测试覆盖 pt_map 接管 host VM 后的元数据分配失败。 */
void mem_test_fail_pt_map_at(size_t index);
// Map empty space into fake memory
int pt_map_nothing(struct mem *mem, page_t page, pages_t pages, unsigned flags);
// Unmap fake memory, return -1 if any part of the range isn't mapped and 0 otherwise
int pt_unmap(struct mem *mem, page_t start, pages_t pages);
// like pt_unmap but doesn't care if part of the range isn't mapped
int pt_unmap_always(struct mem *mem, page_t start, pages_t pages);
// Set the flags on memory
int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags);
// Copy pages from src memory to dst memory using copy-on-write
int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages);

// Must call with mem read-locked.
void *mem_ptr(struct mem *mem, addr_t addr, int type);

enum mem_futex_backing_kind {
    MEM_FUTEX_BACKING_PRIVATE,
    MEM_FUTEX_BACKING_SHARED_MEMORY,
    MEM_FUTEX_BACKING_SHARED_FILE,
};

struct mem_futex_word_snapshot {
    enum mem_futex_backing_kind kind;
    qword_t identity;
    qword_t offset;
};

enum mem_futex_waitv_prepare_result {
    MEM_FUTEX_WAITV_READY,
    MEM_FUTEX_WAITV_ALIGNMENT,
    MEM_FUTEX_WAITV_FAULT,
    MEM_FUTEX_WAITV_MISMATCH,
    MEM_FUTEX_WAITV_NO_MEMORY,
};

/*
 * 在一次最终页表事务内解析最多两个 futex 字；故障时不修改输出。
 * shared_key 为真时还会执行共享 futex 键所需的写固定与私有文件页 COW。
 * 返回 0 或负的 guest errno。
 */
int mem_snapshot_futex_words(struct mem *mem,
        const addr_t *addresses, size_t count, bool shared_key,
        struct mem_futex_word_snapshot *snapshots,
        dword_t *first_value);

// 仅供隔离测试精确注入私有页 COW 的 ENOMEM；SIZE_MAX 恢复正常分配。
void mem_test_fail_private_cow_at(size_t index);

/*
 * 在同一最终页表事务中先解析全部稳定键，再按索引读取并比较值。
 * PRIVATE 项只在值阶段要求存在映射，以保留 Linux 的错误先后顺序。
 * 成功前不修改 snapshots；count 由调用方限制在 1～128。
 */
enum mem_futex_waitv_prepare_result mem_prepare_futex_waitv(
        struct mem *mem, const qword_t *addresses,
        const bool *private_mappings, const dword_t *expected,
        size_t count, struct mem_futex_word_snapshot *snapshots);

enum mem_compare_exchange_result {
    MEM_COMPARE_EXCHANGE_SUCCESS,
    MEM_COMPARE_EXCHANGE_MISMATCH,
    MEM_COMPARE_EXCHANGE_FAULT,
};

// 在单次页表写事务内完成 COW、32 位比较交换和映射快照。
// 故障时不修改 observed 与 snapshot。
enum mem_compare_exchange_result mem_compare_exchange_u32(
        struct mem *mem, addr_t address,
        dword_t expected, dword_t replacement,
        dword_t *observed,
        struct mem_futex_word_snapshot *snapshot);
int mem_segv_reason(struct mem *mem, addr_t addr);

extern size_t real_page_size;

#endif

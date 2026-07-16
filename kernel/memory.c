#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/memory.h"
#include "asbestos/asbestos.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "guest/linux/futex-abi.h"

// increment the change count
static void mem_changed(struct mem *mem);
static struct mmu_ops mem_mmu_ops;
static lock_t identity_lock = LOCK_INITIALIZER;
static qword_t last_memory_identity;
static qword_t last_data_identity;

static qword_t allocate_identity(qword_t *last_identity) {
    lock(&identity_lock);
    if (*last_identity == UINT64_MAX)
        die("i386 内存身份已耗尽");
    qword_t identity = ++*last_identity;
    unlock(&identity_lock);
    return identity;
}

void mem_init(struct mem *mem) {
    mem->identity = allocate_identity(&last_memory_identity);
    mem->pgdir = calloc(MEM_PGDIR_SIZE, sizeof(struct pt_entry *));
    mem->pgdir_used = 0;
    mem->mmu.ops = &mem_mmu_ops;
    mem->mmu.asbestos = asbestos_new(&mem->mmu);
    mem->mmu.changes = 0;
    wrlock_init(&mem->lock);
}

void mem_destroy(struct mem *mem) {
    write_wrlock(&mem->lock);
    pt_unmap_always(mem, 0, MEM_PAGES);
    asbestos_free(mem->mmu.asbestos);
    for (int i = 0; i < MEM_PGDIR_SIZE; i++) {
        if (mem->pgdir[i] != NULL)
            free(mem->pgdir[i]);
    }
    free(mem->pgdir);
    write_wrunlock(&mem->lock);
    wrlock_destroy(&mem->lock);
}

#define PGDIR_TOP(page) ((page) >> 10)
#define PGDIR_BOTTOM(page) ((page) & (MEM_PGDIR_SIZE - 1))

static struct pt_entry *mem_pt_new(struct mem *mem, page_t page) {
    struct pt_entry *pgdir = mem->pgdir[PGDIR_TOP(page)];
    if (pgdir == NULL) {
        pgdir = mem->pgdir[PGDIR_TOP(page)] = calloc(MEM_PGDIR_SIZE, sizeof(struct pt_entry));
        mem->pgdir_used++;
    }
    return &pgdir[PGDIR_BOTTOM(page)];
}

struct pt_entry *mem_pt(struct mem *mem, page_t page) {
    struct pt_entry *pgdir = mem->pgdir[PGDIR_TOP(page)];
    if (pgdir == NULL)
        return NULL;
    struct pt_entry *entry = &pgdir[PGDIR_BOTTOM(page)];
    if (entry->data == NULL)
        return NULL;
    return entry;
}

static void mem_pt_del(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt(mem, page);
    if (entry != NULL)
        entry->data = NULL;
}

void mem_next_page(struct mem *mem, page_t *page) {
    (*page)++;
    if (*page >= MEM_PAGES)
        return;
    while (*page < MEM_PAGES && mem->pgdir[PGDIR_TOP(*page)] == NULL)
        *page = (*page - PGDIR_BOTTOM(*page)) + MEM_PGDIR_SIZE;
}

page_t pt_find_hole(struct mem *mem, pages_t size) {
    page_t hole_end = 0; // this can never be used before initializing but gcc doesn't realize
    bool in_hole = false;
    for (page_t page = 0xf7ffd; page > 0x40000; page--) {
        // I don't know how this works but it does
        if (!in_hole && mem_pt(mem, page) == NULL) {
            in_hole = true;
            hole_end = page + 1;
        }
        if (mem_pt(mem, page) != NULL)
            in_hole = false;
        else if (hole_end - page == size)
            return page;
    }
    return BAD_PAGE;
}

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            return false;
    }
    return true;
}

int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags) {
    if (memory == MAP_FAILED)
        return errno_map();

    // If this fails, the munmap in pt_unmap would probably fail.
    assert((uintptr_t) memory % real_page_size == 0 || memory == vdso_data);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .identity = allocate_identity(&last_data_identity),
        .data = memory,
        .size = pages * PAGE_SIZE + offset,

#if LEAK_DEBUG
        .pid = current ? current->pid : 0,
        .dest = start << PAGE_BITS,
#endif
    };

    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
        data->refcount++;
        struct pt_entry *pt = mem_pt_new(mem, page);
        pt->data = data;
        pt->offset = ((page - start) << PAGE_BITS) + offset;
        pt->flags = flags;
    }
    return 0;
}

int pt_unmap(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return -1;
    return pt_unmap_always(mem, start, pages);
}

int pt_unmap_always(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; mem_next_page(mem, &page)) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL)
            continue;
        asbestos_invalidate_page(mem->mmu.asbestos, page);
        struct data *data = pt->data;
        mem_pt_del(mem, page);
        if (--data->refcount == 0) {
            // vdso wasn't allocated with mmap, it's just in our data segment
            if (data->data != vdso_data) {
                int err = munmap(data->data, data->size);
                if (err != 0)
                    die("munmap(%p, %lu) failed: %s", data->data, data->size, strerror(errno));
            }
            if (data->fd != NULL) {
                fd_close(data->fd);
            }
            free(data);
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_map_nothing(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages == 0) return 0;
    void *memory = mmap(NULL, pages * PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    return pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
}

int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags) {
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return _ENOMEM;
    for (page_t page = start; page < start + pages; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        unsigned old_flags = entry->flags;
        unsigned new_flags =
                (old_flags & ~(unsigned) P_RWX) |
                ((unsigned) flags & (unsigned) P_RWX);
        entry->flags = new_flags;
        // check if protection is increasing
        if ((new_flags & ~old_flags) & (P_READ | P_WRITE)) {
            void *data = (char *) entry->data->data + entry->offset;
            // force to be page aligned
            data = (void *) ((uintptr_t) data & ~(real_page_size - 1));
            int prot = PROT_READ;
            if (flags & P_WRITE) prot |= PROT_WRITE;
            if (mprotect(data, real_page_size, prot) < 0)
                return errno_map();
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages) {
    for (page_t page = start; page < start + pages; mem_next_page(src, &page)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always(dst, page, 1) < 0)
            return -1;
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
    }
    mem_changed(src);
    mem_changed(dst);
    return 0;
}

static void mem_changed(struct mem *mem) {
    mem->mmu.changes++;
}

// This version will return NULL instead of making necessary pagetable changes.
// Used by the emulator to avoid deadlocks.
static void *mem_ptr_nofault(struct mem *mem, addr_t addr, int type) {
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    return entry->data->data + entry->offset + PGOFFSET(addr);
}

static size_t private_cow_fail_at = SIZE_MAX;
static size_t private_cow_count;

void mem_test_fail_private_cow_at(size_t index) {
    private_cow_fail_at = index;
    private_cow_count = 0;
}

// 调用者持有页表写锁；物理页私有化不能丢失所属 VMA 的展示元数据。
static int mem_private_cow_locked(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt(mem, page);
    assert(entry != NULL && (entry->flags & P_COW) != 0);
    if (private_cow_fail_at != SIZE_MAX &&
            private_cow_count++ == private_cow_fail_at)
        return _ENOMEM;
    struct data *source_data = entry->data;
    const void *source =
            (const char *) source_data->data + entry->offset;
    void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if (copy == MAP_FAILED)
        return errno_map();
    memcpy(copy, source, PAGE_SIZE);
    struct fd *file = source_data->fd == NULL ? NULL :
            fd_retain(source_data->fd);
    qword_t file_offset = file == NULL ? source_data->file_offset :
            source_data->file_backing_offset + entry->offset;
    qword_t file_backing_offset = file == NULL ?
            source_data->file_backing_offset : file_offset;
    const char *name = source_data->name;
    unsigned flags = entry->flags &
            ~((unsigned) P_COW | (unsigned) P_FILE_BACKED);
    int result = pt_map(mem, page, 1, copy, 0, flags);
    if (result < 0) {
        if (file != NULL)
            fd_close(file);
        (void) munmap(copy, PAGE_SIZE);
        return result;
    }
    struct data *private_data = mem_pt(mem, page)->data;
    private_data->fd = file;
    private_data->file_offset = file_offset;
    private_data->file_backing_offset = file_backing_offset;
    private_data->name = name;
    return 0;
}

void *mem_ptr(struct mem *mem, addr_t addr, int type) {
    void *old_ptr = mem_ptr_nofault(mem, addr, type); // just for an assert
    bool mapping_rechecked = false;

    page_t page = PAGE(addr);
    struct pt_entry *entry = mem_pt(mem, page);

    if (entry == NULL) {
        // page does not exist
        // look to see if the next VM region is willing to grow down
        page_t p = page + 1;
        while (p < MEM_PAGES && mem_pt(mem, p) == NULL)
            p++;
        if (p >= MEM_PAGES)
            return NULL;
        if (!(mem_pt(mem, p)->flags & P_GROWSDOWN))
            return NULL;

        // Changing memory maps must be done with the write lock. But this is
        // called with the read lock.
        // This locking stuff is copy/pasted for all the code in this function
        // which changes memory maps.
        // TODO: factor the lock/unlock code here into a new function. Do this
        // next time you touch this function.
        read_wrunlock(&mem->lock);
        write_wrlock(&mem->lock);
        pt_map_nothing(mem, page, 1, P_WRITE | P_GROWSDOWN);
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);

        entry = mem_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        // if page is unwritable, well tough luck
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        // get rid of any compiled blocks in this page
        asbestos_invalidate_page(mem->mmu.asbestos, page);
        if (type == MEM_WRITE_PTRACE || (entry->flags & P_COW)) {
            read_wrunlock(&mem->lock);
            write_wrlock(&mem->lock);
            mapping_rechecked = true;
            entry = mem_pt(mem, page);
            bool copied = entry != NULL;
            if (copied && type != MEM_WRITE_PTRACE &&
                    !(entry->flags & P_WRITE))
                copied = false;
            if (copied && type == MEM_WRITE_PTRACE) {
                // ptrace 写入只改变当前锁定映射，不沿用升级前的陈旧 entry。
                entry->flags |= P_WRITE | P_COW;
            }
            if (copied) {
                asbestos_invalidate_page(mem->mmu.asbestos, page);
                if (entry->flags & P_COW)
                    copied = mem_private_cow_locked(mem, page) == 0;
            }
            write_wrunlock(&mem->lock);
            read_wrlock(&mem->lock);
            if (!copied)
                return NULL;
        }
    }

    void *ptr = mem_ptr_nofault(mem, addr, type);
    assert(old_ptr == NULL || old_ptr == ptr ||
            type == MEM_WRITE_PTRACE || mapping_rechecked);
    return ptr;
}

static struct mem_futex_word_snapshot
mem_futex_word_snapshot_locked(
        const struct pt_entry *entry, addr_t address) {
    const struct data *data = entry->data;
    qword_t entry_offset =
            (qword_t) entry->offset + PGOFFSET(address);
    if ((entry->flags & P_FILE_BACKED) != 0 &&
            data->fd != NULL && S_ISREG(data->fd->type) &&
            data->fd->inode != NULL &&
            data->fd->inode->futex_sequence != 0) {
        return (struct mem_futex_word_snapshot) {
            .kind = MEM_FUTEX_BACKING_SHARED_FILE,
            .identity = data->fd->inode->futex_sequence,
            .offset = data->file_backing_offset + entry_offset,
        };
    }
    if (!(entry->flags & P_SHARED))
        return (struct mem_futex_word_snapshot) {
            .kind = MEM_FUTEX_BACKING_PRIVATE,
        };
    return (struct mem_futex_word_snapshot) {
        .kind = MEM_FUTEX_BACKING_SHARED_MEMORY,
        .identity = data->identity,
        .offset = entry_offset,
    };
}

static bool mem_has_stable_file_futex_key_locked(
        const struct pt_entry *entry) {
    const struct data *data = entry->data;
    return (entry->flags & P_FILE_BACKED) != 0 &&
            data->fd != NULL && S_ISREG(data->fd->type) &&
            data->fd->inode != NULL &&
            data->fd->inode->futex_sequence != 0;
}

static void mem_futex_transaction_lock(
        struct mem *mem, bool write) {
    if (write)
        write_wrlock(&mem->lock);
    else
        read_wrlock(&mem->lock);
}

static void mem_futex_transaction_unlock(
        struct mem *mem, bool write) {
    if (write)
        write_wrunlock(&mem->lock);
    else
        read_wrunlock(&mem->lock);
}

static int mem_prepare_shared_futex_key_locked(
        struct mem *mem, addr_t address) {
    page_t page = PAGE(address);
    struct pt_entry *entry = mem_pt(mem, page);
    assert(entry != NULL);
    if (entry->flags & P_SPECIAL)
        return _EFAULT;

    // FOLL_WRITE 会让可写私有文件页和 fork COW 页先成为当前 mm 的副本。
    if (!(entry->flags & P_SHARED) && (entry->flags & P_WRITE) &&
            (entry->flags & (P_COW | P_FILE_BACKED))) {
        int result = mem_private_cow_locked(mem, page);
        if (result < 0)
            return result;
        entry = mem_pt(mem, page);
        assert(entry != NULL);
    }

    if (entry->flags & P_FILE_BACKED)
        return mem_has_stable_file_futex_key_locked(entry) ? 0 : _EFAULT;
    // 无文件后备的只读私有页不能通过共享键的写固定阶段。
    return (entry->flags & P_SHARED) != 0 ||
            (entry->flags & P_WRITE) != 0 ? 0 : _EFAULT;
}

int mem_snapshot_futex_words(struct mem *mem,
        const addr_t *addresses, size_t count, bool shared_key,
        struct mem_futex_word_snapshot *snapshots,
        dword_t *first_value) {
    assert(mem != NULL && addresses != NULL &&
            count != 0 && count <= 2 && snapshots != NULL);

    // 先触发可能的栈向下增长，再在不释放的最终事务内固定全部映射。
    for (size_t index = 0; index < count; index++) {
        read_wrlock(&mem->lock);
        void *pointer = mem_ptr(mem, addresses[index], MEM_READ);
        bool mapped = pointer != NULL;
        read_wrunlock(&mem->lock);
        if (!mapped)
            return _EFAULT;
    }

    struct mem_futex_word_snapshot local_snapshots[2];
    dword_t local_first_value = 0;
    mem_futex_transaction_lock(mem, shared_key);
    for (size_t index = 0; index < count; index++) {
        struct pt_entry *entry = mem_pt(
                mem, PAGE(addresses[index]));
        if (entry == NULL) {
            mem_futex_transaction_unlock(mem, shared_key);
            return _EFAULT;
        }
        // i386 页表以任一硬件访问位表示可读；PROT_NONE 不可取值或固定键。
        if ((entry->flags & P_RWX) == 0) {
            mem_futex_transaction_unlock(mem, shared_key);
            return _EFAULT;
        }
        if (shared_key) {
            int result = mem_prepare_shared_futex_key_locked(
                    mem, addresses[index]);
            if (result < 0) {
                mem_futex_transaction_unlock(mem, shared_key);
                return result;
            }
        }
        entry = mem_pt(mem, PAGE(addresses[index]));
        assert(entry != NULL);
        void *raw_pointer = mem_ptr_nofault(
                mem, addresses[index], MEM_READ);
        if (raw_pointer == NULL ||
                (uintptr_t) raw_pointer % _Alignof(dword_t) != 0) {
            mem_futex_transaction_unlock(mem, shared_key);
            return _EFAULT;
        }
        local_snapshots[index] =
                mem_futex_word_snapshot_locked(
                        entry, addresses[index]);
        if (index == 0 && first_value != NULL) {
            dword_t *pointer = raw_pointer;
            local_first_value = __atomic_load_n(
                    pointer, __ATOMIC_SEQ_CST);
        }
    }
    mem_futex_transaction_unlock(mem, shared_key);

    memcpy(snapshots, local_snapshots,
            count * sizeof(*snapshots));
    if (first_value != NULL)
        *first_value = local_first_value;
    return 0;
}

enum mem_futex_waitv_prepare_result mem_prepare_futex_waitv(
        struct mem *mem, const qword_t *addresses,
        const bool *private_mappings, const dword_t *expected,
        size_t count, struct mem_futex_word_snapshot *snapshots) {
    assert(mem != NULL && addresses != NULL &&
            private_mappings != NULL && expected != NULL &&
            count != 0 && count <= GUEST_LINUX_FUTEX_WAITV_MAX &&
            snapshots != NULL);

    bool has_shared_key = false;
    for (size_t index = 0; index < count; index++)
        has_shared_key |= !private_mappings[index];

    for (;;) {
        struct mem_futex_word_snapshot resolved
                [GUEST_LINUX_FUTEX_WAITV_MAX];
        qword_t fault_address = 0;
        bool retry_fault = false;

        mem_futex_transaction_lock(mem, has_shared_key);
        for (size_t index = 0; index < count; index++) {
            qword_t wide_address = addresses[index];
            if ((wide_address & (sizeof(dword_t) - 1)) != 0) {
                mem_futex_transaction_unlock(mem, has_shared_key);
                return MEM_FUTEX_WAITV_ALIGNMENT;
            }
            if (wide_address > UINT32_MAX) {
                mem_futex_transaction_unlock(mem, has_shared_key);
                return MEM_FUTEX_WAITV_FAULT;
            }

            addr_t address = (addr_t) wide_address;
            if (private_mappings[index]) {
                resolved[index] = (struct mem_futex_word_snapshot) {
                    .kind = MEM_FUTEX_BACKING_PRIVATE,
                };
                continue;
            }

            void *pointer = mem_ptr_nofault(mem, address, MEM_READ);
            if (pointer == NULL) {
                fault_address = wide_address;
                retry_fault = true;
                break;
            }
            struct pt_entry *entry = mem_pt(mem, PAGE(address));
            assert(entry != NULL);
            if ((entry->flags & P_RWX) == 0) {
                mem_futex_transaction_unlock(mem, has_shared_key);
                return MEM_FUTEX_WAITV_FAULT;
            }
            int prepare_result =
                    mem_prepare_shared_futex_key_locked(mem, address);
            if (prepare_result < 0) {
                mem_futex_transaction_unlock(mem, has_shared_key);
                return prepare_result == _ENOMEM ?
                        MEM_FUTEX_WAITV_NO_MEMORY :
                        MEM_FUTEX_WAITV_FAULT;
            }
            entry = mem_pt(mem, PAGE(address));
            assert(entry != NULL);
            resolved[index] = mem_futex_word_snapshot_locked(
                    entry, address);
        }

        if (!retry_fault) {
            for (size_t index = 0; index < count; index++) {
                addr_t address = (addr_t) addresses[index];
                dword_t *pointer = mem_ptr_nofault(
                        mem, address, MEM_READ);
                if (pointer == NULL) {
                    fault_address = addresses[index];
                    retry_fault = true;
                    break;
                }
                struct pt_entry *entry = mem_pt(mem, PAGE(address));
                assert(entry != NULL);
                if ((entry->flags & P_RWX) == 0) {
                    mem_futex_transaction_unlock(mem, has_shared_key);
                    return MEM_FUTEX_WAITV_FAULT;
                }
                dword_t observed = __atomic_load_n(
                        pointer, __ATOMIC_SEQ_CST);
                if (observed != expected[index]) {
                    mem_futex_transaction_unlock(mem, has_shared_key);
                    return MEM_FUTEX_WAITV_MISMATCH;
                }
            }
        }

        mem_futex_transaction_unlock(mem, has_shared_key);
        if (!retry_fault) {
            memcpy(snapshots, resolved,
                    count * sizeof(*snapshots));
            return MEM_FUTEX_WAITV_READY;
        }

        assert(fault_address <= UINT32_MAX);
        read_wrlock(&mem->lock);
        bool fault_resolved = mem_ptr(mem,
                (addr_t) fault_address, MEM_READ) != NULL;
        read_wrunlock(&mem->lock);
        if (!fault_resolved)
            return MEM_FUTEX_WAITV_FAULT;
    }
}

static dword_t *mem_prepare_atomic_u32_write_locked(
        struct mem *mem, addr_t address) {
    if ((address & (sizeof(dword_t) - 1)) != 0)
        return NULL;

    page_t page = PAGE(address);
    struct pt_entry *entry = mem_pt(mem, page);
    if (entry == NULL || !(entry->flags & P_WRITE))
        return NULL;

    asbestos_invalidate_page(mem->mmu.asbestos, page);
    if (entry->flags & P_COW) {
        if (mem_private_cow_locked(mem, page) < 0)
            return NULL;
    }

    void *raw_pointer = mem_ptr_nofault(
            mem, address, MEM_WRITE);
    if (raw_pointer == NULL ||
            (uintptr_t) raw_pointer % _Alignof(dword_t) != 0)
        return NULL;
    return raw_pointer;
}

enum mem_compare_exchange_result mem_compare_exchange_u32(
        struct mem *mem, addr_t address,
        dword_t expected, dword_t replacement,
        dword_t *observed,
        struct mem_futex_word_snapshot *snapshot) {
    assert(mem != NULL && observed != NULL && snapshot != NULL);
    write_wrlock(&mem->lock);
    dword_t *pointer =
            mem_prepare_atomic_u32_write_locked(mem, address);
    if (pointer == NULL) {
        write_wrunlock(&mem->lock);
        return MEM_COMPARE_EXCHANGE_FAULT;
    }

    struct pt_entry *entry = mem_pt(mem, PAGE(address));
    assert(entry != NULL);
    struct mem_futex_word_snapshot actual_snapshot =
            mem_futex_word_snapshot_locked(entry, address);
    dword_t actual = expected;
    bool exchanged = __atomic_compare_exchange_n(
            pointer, &actual, replacement, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *observed = actual;
    *snapshot = actual_snapshot;
    write_wrunlock(&mem->lock);
    return exchanged ? MEM_COMPARE_EXCHANGE_SUCCESS :
            MEM_COMPARE_EXCHANGE_MISMATCH;
}

static void *mem_mmu_translate(struct mmu *mmu, addr_t addr, int type) {
    return mem_ptr_nofault(container_of(mmu, struct mem, mmu), addr, type);
}

static struct mmu_ops mem_mmu_ops = {
    .translate = mem_mmu_translate,
};

int mem_segv_reason(struct mem *mem, addr_t addr) {
    struct pt_entry *pt = mem_pt(mem, PAGE(addr));
    if (pt == NULL)
        return SEGV_MAPERR_;
    return SEGV_ACCERR_;
}

size_t real_page_size;
__attribute__((constructor)) static void get_real_page_size() {
    real_page_size = sysconf(_SC_PAGESIZE);
}

void mem_coredump(struct mem *mem, const char *file) {
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return;
    }
    if (ftruncate(fd, 0xffffffff) < 0) {
        perror("ftruncate");
        return;
    }

    int pages = 0;
    for (page_t page = 0; page < MEM_PAGES; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL)
            continue;
        pages++;
        if (lseek(fd, page << PAGE_BITS, SEEK_SET) < 0) {
            perror("lseek");
            return;
        }
        if (write(fd, entry->data->data, PAGE_SIZE) < 0) {
            perror("write");
            return;
        }
    }
    printk("dumped %d pages\n", pages);
    close(fd);
}

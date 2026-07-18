#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/memory.h"
#include "kernel/mm.h"

struct mm *mm_new(void) {
    struct mm *mm = calloc(1, sizeof(struct mm));
    if (mm == NULL)
        return NULL;
    mem_init(&mm->mem);
    mm->start_brk = mm->brk = 0; // should get overwritten by exec
    mm->refcount = 1;
    return mm;
}

struct mm *mm_copy(struct mm *mm) {
    struct mm *new_mm = malloc(sizeof(struct mm));
    if (new_mm == NULL)
        return NULL;
    *new_mm = *mm;
    // Fix wrlock_init failing because it thinks it's reinitializing the same lock
    memset(&new_mm->mem.lock, 0, sizeof(new_mm->mem.lock));
    new_mm->refcount = 1;
    mem_init(&new_mm->mem);
    fd_retain(new_mm->exefile);
    write_wrlock(&mm->mem.lock);
    pt_copy_on_write(&mm->mem, &new_mm->mem, 0, MEM_PAGES);
    write_wrunlock(&mm->mem.lock);
    return new_mm;
}

void mm_retain(struct mm *mm) {
    mm->refcount++;
}

void mm_release(struct mm *mm) {
    if (--mm->refcount == 0) {
        if (mm->exefile != NULL)
            fd_close(mm->exefile);
        mem_destroy(&mm->mem);
        free(mm);
    }
}

static addr_t do_mmap(addr_t addr, dword_t len, dword_t prot,
        dword_t flags, fd_t fd_no, qword_t offset) {
    int err;
    pages_t pages = PAGE_ROUND_UP(len);
    if (!pages) return _EINVAL;
    page_t page;
    if (addr != 0) {
        if (PGOFFSET(addr) != 0)
            return _EINVAL;
        page = PAGE(addr);
        if (!(flags & MMAP_FIXED) && !pt_is_hole(current->mem, page, pages)) {
            page = pt_find_hole(current->mem, pages);
            if (page == BAD_PAGE)
                return _ENOMEM;
        }
    } else {
        page = pt_find_hole(current->mem, pages);
        if (page == BAD_PAGE)
            return _ENOMEM;
    }

    if (flags & MMAP_SHARED)
        prot |= P_SHARED;

    if (flags & MMAP_ANONYMOUS) {
        if ((err = pt_map_nothing(current->mem, page, pages, prot)) < 0)
            return err;
    } else {
        // fd must be valid
        struct fd *fd = f_get(fd_no);
        if (fd == NULL)
            return _EBADF;
        if (fd->ops->mmap == NULL)
            return _ENODEV;
        if ((err = fd->ops->mmap(fd, current->mem,
                page, pages, (off_t) offset, prot, flags)) < 0)
            return err;
        mem_pt(current->mem, page)->data->fd = fd_retain(fd);
        mem_pt(current->mem, page)->data->file_offset = offset;
        mem_pt(current->mem, page)->data->file_backing_offset =
                offset - offset % (qword_t) real_page_size;
    }
    return page << PAGE_BITS;
}

static addr_t mmap_common(addr_t addr, dword_t len, dword_t prot,
        dword_t flags, fd_t fd_no, qword_t offset) {
    STRACE("mmap(0x%x, 0x%x, 0x%x, 0x%x, %d, %llu)",
            addr, len, prot, flags, fd_no,
            (unsigned long long) offset);
    if (len == 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    if ((flags & MMAP_PRIVATE) && (flags & MMAP_SHARED))
        return _EINVAL;

    write_wrlock(&current->mem->lock);
    addr_t res = do_mmap(addr, len, prot, flags, fd_no, offset);
    write_wrunlock(&current->mem->lock);
    return res;
}

addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset) {
    return mmap_common(addr, len, prot, flags, fd_no,
            (qword_t) offset << PAGE_BITS);
}

struct mmap_arg_struct {
    dword_t addr, len, prot, flags, fd, offset;
};

addr_t sys_mmap(addr_t args_addr) {
    struct mmap_arg_struct args;
    if (user_get(args_addr, args))
        return _EFAULT;
    return mmap_common(args.addr, args.len, args.prot, args.flags, args.fd, args.offset);
}

int_t sys_munmap(addr_t addr, uint_t len) {
    STRACE("munmap(0x%x, 0x%x)", addr, len);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (len == 0)
        return _EINVAL;
    write_wrlock(&current->mem->lock);
    int err = pt_unmap_always(current->mem, PAGE(addr), PAGE_ROUND_UP(len));
    write_wrunlock(&current->mem->lock);
    if (err < 0)
        return _EINVAL;
    return 0;
}

#define MREMAP_MAYMOVE_ 1
#define MREMAP_FIXED_ 2

int_t sys_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags) {
    STRACE("mremap(%#x, %#x, %#x, %d)", addr, old_len, new_len, flags);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (flags & ~(MREMAP_MAYMOVE_ | MREMAP_FIXED_))
        return _EINVAL;
    if (flags & MREMAP_FIXED_) {
        FIXME("missing MREMAP_FIXED");
        return _EINVAL;
    }
    if (old_len == 0 || new_len == 0)
        return _EINVAL;
    pages_t old_pages = (pages_t) (
            ((qword_t) old_len + PAGE_SIZE - 1) >> PAGE_BITS);
    pages_t new_pages = (pages_t) (
            ((qword_t) new_len + PAGE_SIZE - 1) >> PAGE_BITS);
    page_t start_page = PAGE(addr);
    if ((qword_t) start_page + old_pages > MEM_PAGES ||
            (qword_t) start_page + new_pages > MEM_PAGES)
        return _EFAULT;

    write_wrlock(&current->mem->lock);
    int_t result;
    struct pt_entry *entry = mem_pt(current->mem, start_page);
    if (entry == NULL) {
        result = _EFAULT;
        goto out;
    }
    const dword_t page_state_flags = P_COW | P_FILE_BACKED;
    dword_t mapping_flags = entry->flags & ~page_state_flags;
    for (pages_t index = 0; index < old_pages; index++) {
        page_t page = start_page + index;
        entry = mem_pt(current->mem, page);
        if (entry == NULL ||
                (entry->flags & ~page_state_flags) != mapping_flags) {
            result = _EFAULT;
            goto out;
        }
    }

    if (new_pages <= old_pages) {
        pages_t removed_pages = old_pages - new_pages;
        int err = removed_pages == 0 ? 0 :
                pt_unmap(current->mem,
                        start_page + new_pages, removed_pages);
        result = err < 0 ? _EFAULT : (int_t) addr;
        goto out;
    }
    if (!(mapping_flags & P_ANONYMOUS) ||
            (mapping_flags & P_SHARED)) {
        // 共享匿名扩展必须由真正的单一 shmem 后备承载，不能伪造同键副本。
        FIXME("尚未实现共享或文件映射的 mremap 扩展");
        result = _EFAULT;
        goto out;
    }
    page_t extra_start = start_page + old_pages;
    pages_t extra_pages = new_pages - old_pages;
    if (!pt_is_hole(current->mem, extra_start, extra_pages)) {
        result = _ENOMEM;
        goto out;
    }
    int err = pt_map_nothing(
            current->mem, extra_start, extra_pages, mapping_flags);
    if (err < 0) {
        result = err;
        goto out;
    }
    result = (int_t) addr;
out:
    write_wrunlock(&current->mem->lock);
    return result;
}

int_t sys_mprotect(addr_t addr, uint_t len, int_t prot) {
    STRACE("mprotect(0x%x, 0x%x, 0x%x)", addr, len, prot);
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if (prot & ~P_RWX)
        return _EINVAL;
    pages_t pages = PAGE_ROUND_UP(len);
    write_wrlock(&current->mem->lock);
    int err = pt_set_flags(current->mem, PAGE(addr), pages, prot);
    write_wrunlock(&current->mem->lock);
    return err;
}

dword_t sys_madvise(addr_t UNUSED(addr), dword_t UNUSED(len), dword_t UNUSED(advice)) {
    // portable applications should not rely on linux's destructive semantics for MADV_DONTNEED.
    return 0;
}

dword_t sys_mbind(addr_t UNUSED(addr), dword_t UNUSED(len), int_t UNUSED(mode),
        addr_t UNUSED(nodemask), dword_t UNUSED(maxnode), uint_t UNUSED(flags)) {
    return 0;
}

int_t sys_mlock(addr_t UNUSED(addr), dword_t UNUSED(len)) {
    return 0;
}

#define MS_ASYNC_ 1
#define MS_INVALIDATE_ 2
#define MS_SYNC_ 4

static bool msync_shared_file_entry(const struct pt_entry *entry) {
    return entry != NULL &&
            (entry->flags & (P_SHARED | P_FILE_BACKED)) ==
                    (P_SHARED | P_FILE_BACKED) &&
            entry->data->fd != NULL;
}

static bool msync_fd_is_writable(struct fd *fd) {
    lock(&fd->lock);
    bool writable = (fd->flags & O_ACCMODE_) == O_RDWR_;
    unlock(&fd->lock);
    return writable;
}

int_t sys_msync(addr_t addr, dword_t len, int_t flags) {
    STRACE("msync(%#x, %#x, %#x)", addr, len, flags);
    dword_t sync_flags = (dword_t) flags;
    const dword_t allowed = MS_ASYNC_ | MS_INVALIDATE_ | MS_SYNC_;
    if ((sync_flags & ~allowed) != 0)
        return _EINVAL;
    if (PGOFFSET(addr) != 0)
        return _EINVAL;
    if ((sync_flags & MS_ASYNC_) != 0 &&
            (sync_flags & MS_SYNC_) != 0)
        return _EINVAL;

    // i386 size_t 的页对齐加法按 32 位无符号规则环绕。
    const dword_t page_mask = (dword_t) PAGE_SIZE - 1;
    dword_t rounded_len = (len + page_mask) & ~page_mask;
    addr_t end = addr + rounded_len;
    if (end < addr)
        return _ENOMEM;
    if (end == addr)
        return 0;

    bool unmapped = false;
    addr_t cursor = addr;
    while (cursor < end) {
        addr_t next = cursor + PAGE_SIZE;
        struct fd *sync_fd = NULL;

        read_wrlock(&current->mem->lock);
        struct pt_entry *entry = mem_pt(current->mem, PAGE(cursor));
        if (entry == NULL) {
            unmapped = true;
        } else if ((sync_flags & MS_SYNC_) != 0 &&
                msync_shared_file_entry(entry)) {
            struct data *data = entry->data;
            page_t next_page = PAGE(cursor) + 1;
            page_t limit = PAGE(end);
            while (next_page < limit) {
                struct pt_entry *following =
                        mem_pt(current->mem, next_page);
                if (!msync_shared_file_entry(following) ||
                        following->data != data)
                    break;
                next_page++;
            }
            next = (addr_t) (next_page << PAGE_BITS);
            sync_fd = fd_retain(data->fd);
        }
        read_wrunlock(&current->mem->lock);

        if (sync_fd != NULL) {
            int error = 0;
            if (msync_fd_is_writable(sync_fd))
                error = file_sync_fd(sync_fd, true);
            fd_close(sync_fd);
            if (error < 0)
                return error;
        }
        if (unmapped && sync_flags == MS_ASYNC_)
            return _ENOMEM;
        cursor = next;
    }
    return unmapped ? _ENOMEM : 0;
}

addr_t sys_brk(addr_t new_brk) {
    STRACE("brk(0x%x)", new_brk);
    struct mm *mm = current->mm;

    write_wrlock(&mm->mem.lock);
    if (new_brk < mm->start_brk)
        goto out;
    addr_t old_brk = mm->brk;

    if (new_brk > old_brk) {
        // expand heap: map region from old_brk to new_brk
        // round up because of the definition of brk: "the first location after the end of the uninitialized data segment." (brk(2))
        // if the brk is 0x2000, page 0x2000 shouldn't be mapped, but it should be if the brk is 0x2001.
        page_t start = PAGE_ROUND_UP(old_brk);
        pages_t size = PAGE_ROUND_UP(new_brk) - PAGE_ROUND_UP(old_brk);
        if (!pt_is_hole(&mm->mem, start, size))
            goto out;
        int err = pt_map_nothing(&mm->mem, start, size, P_WRITE);
        if (err < 0)
            goto out;
    } else if (new_brk < old_brk) {
        // shrink heap: unmap region from new_brk to old_brk
        // first page to unmap is PAGE(new_brk)
        // last page to unmap is PAGE(old_brk)
        pt_unmap_always(&mm->mem, PAGE(new_brk), PAGE(old_brk) - PAGE(new_brk));
    }

    mm->brk = new_brk;
out:;
    addr_t brk = mm->brk;
    write_wrunlock(&mm->mem.lock);
    return brk;
}

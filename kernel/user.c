#include <string.h>
#include "kernel/calls.h"

_Static_assert(sizeof(addr_t) == sizeof(dword_t),
        "旧版用户复制路径只服务 32 位 i386 guest 地址空间");

static bool user_range_fits(addr_t address, size_t count) {
    qword_t available = UINT64_C(1) + UINT32_MAX - (qword_t) address;
    return (qword_t) count <= available;
}

static int __user_read_task(struct task *task, addr_t addr, void *buf, size_t count) {
    if (!user_range_fits(addr, count))
        return 1;

    char *cbuf = (char *) buf;
    size_t copied = 0;
    while (copied < count) {
        addr_t p = addr + (addr_t) copied;
        size_t chunk = PAGE_SIZE - PGOFFSET(p);
        if (chunk > count - copied)
            chunk = count - copied;
        const char *ptr = mem_ptr(task->mem, p, MEM_READ);
        if (ptr == NULL)
            return 1;
        memcpy(&cbuf[copied], ptr, chunk);
        copied += chunk;
    }
    return 0;
}

static int __user_write_task(struct task *task, addr_t addr, const void *buf, size_t count, bool ptrace) {
    if (!user_range_fits(addr, count))
        return 1;

    const char *cbuf = (const char *) buf;
    size_t copied = 0;
    while (copied < count) {
        addr_t p = addr + (addr_t) copied;
        size_t chunk = PAGE_SIZE - PGOFFSET(p);
        if (chunk > count - copied)
            chunk = count - copied;
        char *ptr = mem_ptr(task->mem, p, ptrace ? MEM_WRITE_PTRACE : MEM_WRITE);
        if (ptr == NULL)
            return 1;
        memcpy(ptr, &cbuf[copied], chunk);
        copied += chunk;
    }
    return 0;
}

int user_read_task(struct task *task, addr_t addr, void *buf, size_t count) {
    read_wrlock(&task->mem->lock);
    int res = __user_read_task(task, addr, buf, count);
    read_wrunlock(&task->mem->lock);
    return res;
}

int user_read(addr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

int user_write_task(struct task *task, addr_t addr, const void *buf, size_t count) {
    read_wrlock(&task->mem->lock);
    int res = __user_write_task(task, addr, buf, count, false);
    read_wrunlock(&task->mem->lock);
    return res;
}

int user_write_task_ptrace(struct task *task, addr_t addr, const void *buf, size_t count) {
    read_wrlock(&task->mem->lock);
    int res = __user_write_task(task, addr, buf, count, true);
    read_wrunlock(&task->mem->lock);
    return res;
}

int user_write(addr_t addr, const void *buf, size_t count) {
    return user_write_task(current, addr, buf, count);
}

int user_read_string(addr_t addr, char *buf, size_t max) {
    if (addr == 0)
        return 1;
    read_wrlock(&current->mem->lock);
    size_t i = 0;
    while (i < max) {
        if ((qword_t) i > UINT32_MAX - (qword_t) addr ||
                __user_read_task(current, addr + (addr_t) i,
                        &buf[i], sizeof(buf[i]))) {
            read_wrunlock(&current->mem->lock);
            return 1;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    read_wrunlock(&current->mem->lock);
    return 0;
}

int user_write_string(addr_t addr, const char *buf) {
    if (addr == 0)
        return 1;
    read_wrlock(&current->mem->lock);
    size_t i = 0;
    do {
        if ((qword_t) i > UINT32_MAX - (qword_t) addr ||
                __user_write_task(current, addr + (addr_t) i,
                        &buf[i], sizeof(buf[i]), false)) {
            read_wrunlock(&current->mem->lock);
            return 1;
        }
        i++;
    } while (buf[i - 1] != '\0');
    read_wrunlock(&current->mem->lock);
    return 0;
}

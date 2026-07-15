#ifndef GUEST_MEMORY_PAGE_BACKING_H
#define GUEST_MEMORY_PAGE_BACKING_H

#include "guest/memory/address-space.h"

struct guest_page_backing;

// create/clone 返回一份强引用；retain 新增一份，release 消耗一份。
// 释放最后一份强引用时，必须已无调用者持有或等待该同步域的锁。
struct guest_page_backing *guest_page_backing_create(void);
// clone 内部会持有 source 同步域读锁；并发写入者须遵守同一同步域。
struct guest_page_backing *guest_page_backing_clone(
        const struct guest_page_backing *source);
// retain 只接受调用方已持有强引用的存活对象，不提供弱引用复活。
void guest_page_backing_retain(struct guest_page_backing *backing);
void guest_page_backing_release(struct guest_page_backing *backing);
// 返回值只在调用方持有 backing 强引用期间有效。
// 共享 backing 的并发读写必须分别持有对应同步域的读锁或写锁。
// 共享写入者还必须在解锁前调用 written，以使物理页独占保留失效。
byte_t *guest_page_backing_bytes(struct guest_page_backing *backing);
// 同步域与 backing 同寿命，身份在进程生命周期内不复用。
// track_exclusive、exclusive_matches 与 written 回调只能在持有写锁时调用。
const struct guest_page_sync *guest_page_backing_sync(
        const struct guest_page_backing *backing);
// 常驻生命周期计数仅供集成测试在静止点检查资源回收。
unsigned guest_page_backing_test_live_count(void);

#if defined(GUEST_PAGE_BACKING_TESTING)
void guest_page_backing_test_fail_allocation_at(size_t index);
size_t guest_page_backing_test_allocation_count(void);
unsigned guest_page_backing_test_reference_count(
        const struct guest_page_backing *backing);
#endif

#endif

#ifndef GUEST_MEMORY_PAGE_BACKING_H
#define GUEST_MEMORY_PAGE_BACKING_H

#include "guest/memory/address-space.h"

struct guest_page_backing;

// create/clone 返回一份强引用；retain 新增一份，release 消耗一份。
struct guest_page_backing *guest_page_backing_create(void);
// clone 期间调用方须持有 source 强引用，并保证其字节内容不被并发修改。
struct guest_page_backing *guest_page_backing_clone(
        const struct guest_page_backing *source);
// retain 只接受调用方已持有强引用的存活对象，不提供弱引用复活。
void guest_page_backing_retain(struct guest_page_backing *backing);
void guest_page_backing_release(struct guest_page_backing *backing);
// 返回值只在调用方持有 backing 强引用期间有效。
byte_t *guest_page_backing_bytes(struct guest_page_backing *backing);

#if defined(GUEST_PAGE_BACKING_TESTING)
void guest_page_backing_test_fail_allocation_at(size_t index);
size_t guest_page_backing_test_allocation_count(void);
unsigned guest_page_backing_test_live_count(void);
unsigned guest_page_backing_test_reference_count(
        const struct guest_page_backing *backing);
#endif

#endif

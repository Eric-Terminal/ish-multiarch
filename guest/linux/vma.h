#ifndef GUEST_LINUX_VMA_H
#define GUEST_LINUX_VMA_H

#include "guest/memory/address-space.h"

enum guest_linux_vma_source {
    GUEST_LINUX_VMA_SOURCE_BRK,
    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_PRIVATE,
    GUEST_LINUX_VMA_SOURCE_ANONYMOUS_SHARED,
};

struct guest_linux_vma {
    guest_addr_t first;
    guest_addr_t last;
    qword_t protection;
    enum guest_linux_vma_source source;
};

struct guest_linux_vma_set {
    struct guest_linux_vma *entries;
    size_t count;
};

void guest_linux_vma_set_init(struct guest_linux_vma_set *set);
// destination 须未初始化且不能与 source 别名；成功后拥有独立区间数组。
bool guest_linux_vma_set_clone(struct guest_linux_vma_set *destination,
        const struct guest_linux_vma_set *source);
void guest_linux_vma_set_destroy(struct guest_linux_vma_set *set);

// 返回值只在下一次修改或销毁 set 前有效；last 边界不属于当前区间。
const struct guest_linux_vma *guest_linux_vma_find(
        const struct guest_linux_vma_set *set, guest_addr_t address);

// 变更失败只表示宿主内存不足，原集合保持不变。
bool guest_linux_vma_insert(struct guest_linux_vma_set *set,
        struct guest_linux_vma mapping);
bool guest_linux_vma_remove(struct guest_linux_vma_set *set,
        guest_addr_t first, guest_addr_t last);
// 只更新已登记区间的交集；未登记区间留给页表判断是否映射。
bool guest_linux_vma_protect_tracked(struct guest_linux_vma_set *set,
        guest_addr_t first, guest_addr_t last, qword_t protection);

#if defined(GUEST_LINUX_VMA_TESTING)
void guest_linux_vma_test_fail_allocation_at(size_t index);
size_t guest_linux_vma_test_allocation_count(void);
size_t guest_linux_vma_test_live_allocation_count(void);
#endif

#endif

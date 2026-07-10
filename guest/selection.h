#ifndef GUEST_SELECTION_H
#define GUEST_SELECTION_H

#if defined(ISH_GUEST_I386) && defined(ISH_GUEST_AARCH64)
#error "只能选择一种 guest 架构"
#endif

// 未指定目标时保持官方 iSH 的 i386 行为。
#if !defined(ISH_GUEST_I386) && !defined(ISH_GUEST_AARCH64)
#define ISH_GUEST_I386 1
#endif

#endif

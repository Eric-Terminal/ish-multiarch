#ifndef GUEST_AARCH64_LINUX_SIGNAL_TRAMPOLINE_H
#define GUEST_AARCH64_LINUX_SIGNAL_TRAMPOLINE_H

#include "guest/memory/page-table.h"

// page_base 由装载器选择；成功后 entry 可直接写入 runtime services。
enum guest_page_table_result aarch64_linux_map_signal_trampoline(
        struct guest_page_table *table, guest_addr_t page_base,
        guest_addr_t *entry);

#endif

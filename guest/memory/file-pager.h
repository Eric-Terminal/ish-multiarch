#ifndef GUEST_MEMORY_FILE_PAGER_H
#define GUEST_MEMORY_FILE_PAGER_H

#include "guest/memory/page-backing.h"

struct guest_file_pager;

enum guest_file_page_result {
    GUEST_FILE_PAGE_OK,
    GUEST_FILE_PAGE_END_OF_FILE,
    GUEST_FILE_PAGE_IO_ERROR,
    GUEST_FILE_PAGE_OUT_OF_MEMORY,
};

struct guest_file_pager_provider {
    void *opaque;
    /*
     * page 已由 pager 清零。成功时 valid_bytes 必须为 1..4096；
     * provider 只写有效前缀，尾部继续保持零。
     */
    enum guest_file_page_result (*read_page)(void *opaque,
            qword_t file_offset, byte_t *page, dword_t *valid_bytes);
    /*
     * 最后一份强引用归零后同步调用；pager 在回调返回前仍可用于
     * 条件清理外部弱引用，但不得尝试复活。
     */
    void (*release)(struct guest_file_pager *pager, void *opaque);
};

/* identity 在当前 host 生命周期内必须非零且不复用。 */
struct guest_file_pager *guest_file_pager_create(qword_t identity,
        struct guest_file_pager_provider provider);
/* retain 只接受调用方已持有强引用的对象。 */
struct guest_file_pager *guest_file_pager_retain(
        struct guest_file_pager *pager);
/* 供受外部锁保护的弱槽使用；引用归零后绝不复活。 */
bool guest_file_pager_try_retain(struct guest_file_pager *pager);
void guest_file_pager_release(struct guest_file_pager *pager);

/* 成功时返回一份由调用方拥有的 backing 强引用。 */
enum guest_file_page_result guest_file_pager_get_page(
        struct guest_file_pager *pager, qword_t file_offset,
        struct guest_page_backing **backing);
/* 返回值在调用方持有 pager 强引用期间有效，调用方不拥有额外引用。 */
struct guest_file_source *guest_file_pager_file_source(
        struct guest_file_pager *pager);

#if defined(GUEST_FILE_PAGER_TESTING)
void guest_file_pager_test_fail_allocation_at(size_t index);
size_t guest_file_pager_test_allocation_count(void);
unsigned guest_file_pager_test_reference_count(
        const struct guest_file_pager *pager);
#endif

#endif

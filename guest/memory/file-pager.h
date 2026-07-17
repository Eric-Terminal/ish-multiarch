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

enum guest_file_sync_result {
    GUEST_FILE_SYNC_OK,
    GUEST_FILE_SYNC_IO_ERROR,
    GUEST_FILE_SYNC_UNSUPPORTED,
};

struct guest_file_pager_provider {
    void *opaque;
    /*
     * 可选的同文件 I/O 串行域；两个回调必须同时提供或同时为空。
     * pager 统一按 I/O 域→cache/backing 的顺序取锁，且不会在
     * cache/backing 锁内调用 provider。这些 I/O 回调均不可重入同一个
     * pager；生产可写文件应让同 inode 的 pager 共用此串行域。
     * 调用方也不得在已持有该域时调用 pager 的 get_page/sync_range。
     */
    void (*begin_io)(void *opaque);
    void (*end_io)(void *opaque);
    /*
     * page 已由 pager 清零。成功时 valid_bytes 必须为 1..4096；
     * provider 只写有效前缀，尾部继续保持零。
     */
    enum guest_file_page_result (*read_page)(void *opaque,
            qword_t file_offset, byte_t *page, dword_t *valid_bytes);
    /*
     * 可写 provider 接收仅在回调期间有效的完整稳定页快照，并自行按
     * 当前 EOF 限制提交长度。sync_range 使用半开文件区间；为空时写回
     * 成功后不再执行额外同步。
     */
    enum guest_file_sync_result (*write_page)(void *opaque,
            qword_t file_offset, const byte_t *page);
    enum guest_file_sync_result (*sync_range)(void *opaque,
            qword_t file_offset, qword_t length);
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

/*
 * 成功时返回一份由调用方拥有的 backing 强引用。backing 引用只保护
 * 页内存；只要它仍可能接受文件语义写入，调用方就必须另持 pager 强
 * 引用。释放 pager 最后一份引用前必须令写者静止；可写 provider 的
 * 析构写回仅是无法报告错误的 best-effort drain。
 */
enum guest_file_page_result guest_file_pager_get_page(
        struct guest_file_pager *pager, qword_t file_offset,
        struct guest_page_backing **backing);
/* 同步所有与半开区间相交的驻留脏页；调用方须保证 offset+length 不溢出。 */
enum guest_file_sync_result guest_file_pager_sync_range(
        struct guest_file_pager *pager,
        qword_t file_offset, qword_t length);
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

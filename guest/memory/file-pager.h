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
     * 含脏页或待确认 durability 的最终 drain 失败，且注册表已取得强
     * 引用后调用。外部弱槽若会等待 DRAINING 结束，必须在这里唤醒并
     * 继续指向同一个 pager；不得执行 provider I/O。
     */
    void (*drain_failed)(struct guest_file_pager *pager, void *opaque);
    /*
     * 生命周期进入 DEAD 后同步调用；pager 在回调返回前仍可用于条件
     * 清理外部弱引用，但 try_retain 必须失败。
     */
    void (*release)(struct guest_file_pager *pager, void *opaque);
};

/* identity 在当前 host 生命周期内必须非零且不复用。 */
struct guest_file_pager *guest_file_pager_create(qword_t identity,
        struct guest_file_pager_provider provider);
/* retain 只接受调用方已持有强引用的对象。 */
struct guest_file_pager *guest_file_pager_retain(
        struct guest_file_pager *pager);
/* 供受外部锁保护的弱槽使用；DRAINING/DEAD 状态不可 retain。 */
bool guest_file_pager_try_retain(struct guest_file_pager *pager);
void guest_file_pager_release(struct guest_file_pager *pager);
/*
 * 重试调用开始时登记的 orphan 快照，返回本次尝试数；失败对象进入下一
 * 批，不在同一次调用内忙循环。正常 retain 会自动接管登记中的 pager；
 * 任一 provider 回调均不得重入此函数。
 */
size_t guest_file_pager_retry_orphans(void);
size_t guest_file_pager_orphan_count(void);

/*
 * 成功时返回一份由调用方拥有的 backing 强引用。backing 引用只保护
 * 页内存；只要它仍可能接受文件语义写入，调用方就必须另持 pager 强
 * 引用。释放 pager 最后一份引用前必须令写者静止；可写 provider 的
 * 最终 drain 无法向 guest 报错；只有含脏页或待确认 durability 的失败
 * 才由 orphan registry 保留 pager，纯净 pager 直接结束生命周期。
 */
enum guest_file_page_result guest_file_pager_get_page(
        struct guest_file_pager *pager, qword_t file_offset,
        struct guest_page_backing **backing);
/* 同步所有与半开区间相交的驻留脏页；调用方须保证 offset+length 不溢出。 */
enum guest_file_sync_result guest_file_pager_sync_range(
        struct guest_file_pager *pager,
        qword_t file_offset, qword_t length);
/*
 * 普通文件 I/O 在持有 provider I/O 串行域时调用；两者都只访问已驻留
 * cache，不触发 page-in 或 provider I/O。read_resident 用缓存覆盖底层
 * read 的结果；commit_file_write 合并底层已经成功写入的前缀。
 */
void guest_file_pager_read_resident(struct guest_file_pager *pager,
        qword_t file_offset, byte_t *data, size_t size);
void guest_file_pager_commit_file_write(struct guest_file_pager *pager,
        qword_t file_offset, const byte_t *data, size_t size);
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

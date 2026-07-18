#ifndef GUEST_MEMORY_PAGE_BACKING_H
#define GUEST_MEMORY_PAGE_BACKING_H

#include "guest/memory/address-space.h"

struct guest_page_backing;
struct guest_file_page_domain;

/*
 * 同一 pager 的 cache backing 与私有 COW 副本登记在同一失效域。
 * resize 不分配内存；调用方必须在外层串行文件大小变更和 provider I/O。
 */
struct guest_file_page_domain *guest_file_page_domain_create(void);
void guest_file_page_domain_release(struct guest_file_page_domain *domain);
void guest_file_page_domain_resize(struct guest_file_page_domain *domain,
        qword_t old_size, qword_t new_size);

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
// 仅文件失效域成员返回同步域；普通私有页无需额外逐页加锁。
const struct guest_page_sync *guest_page_backing_file_access_sync(
        const struct guest_page_backing *backing);
bool guest_page_backing_file_accessible(
        const struct guest_page_backing *backing);

/*
 * 文件 pager 在 backing 对外可见前启用脏世代。copy_dirty 在内部读锁下
 * 复制完整稳定页；finish_writeback 只会清理由同一内容世代产生的快照。
 */
void guest_page_backing_track_file_writes(
        struct guest_page_backing *backing,
        struct guest_file_page_domain *domain, qword_t file_offset);
bool guest_page_backing_copy_dirty(
        struct guest_page_backing *backing, byte_t *page,
        qword_t *content_generation);
void guest_page_backing_finish_writeback(
        struct guest_page_backing *backing, qword_t content_generation);
/*
 * 文件系统已经提交 data 后，用同一份结果合并驻留页。若页面原本 clean，
 * 合并后仍 clean；若映射写与文件 I/O 并发发生，则保留 dirty，避免后续
 * writeback 用旧页内容覆盖任一方的修改。
 */
void guest_page_backing_commit_file_write(
        struct guest_page_backing *backing, size_t page_offset,
        const byte_t *data, size_t size);
// 常驻生命周期计数仅供集成测试在静止点检查资源回收。
unsigned guest_page_backing_test_live_count(void);

#if defined(GUEST_PAGE_BACKING_TESTING)
void guest_page_backing_test_fail_allocation_at(size_t index);
size_t guest_page_backing_test_allocation_count(void);
unsigned guest_page_backing_test_reference_count(
        const struct guest_page_backing *backing);
#endif

#endif

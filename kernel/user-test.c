#include <stdio.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/task.h"

#define LOW_PAGE UINT32_C(0x00000000)
#define CROSS_PAGE UINT32_C(0x00100000)
#define PARTIAL_PAGE UINT32_C(0x00200000)
#define LAST_PAGE UINT32_C(0xfffff000)

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "i386 用户内存复制测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

static int map_pages(struct task *task, addr_t address, pages_t pages) {
    write_wrlock(&task->mem->lock);
    int error = pt_map_nothing(task->mem, PAGE(address), pages, P_RWX);
    write_wrunlock(&task->mem->lock);
    return error;
}

int main(void) {
    struct task task = {0};
    task_set_mm(&task, mm_new());
    CHECK(task.mm != NULL, "创建用户地址空间");
    current = &task;

    CHECK(map_pages(&task, LOW_PAGE, 1) == 0 &&
            map_pages(&task, CROSS_PAGE, 2) == 0 &&
            map_pages(&task, PARTIAL_PAGE, 1) == 0 &&
            map_pages(&task, LAST_PAGE, 1) == 0,
            "映射低地址、跨页与末页测试区域");

    const byte_t cross_page_source[] = {1, 2, 3, 4, 5, 6};
    byte_t cross_page_result[sizeof(cross_page_source)] = {0};
    addr_t cross_page_address = CROSS_PAGE + PAGE_SIZE - 3;
    CHECK(user_write(cross_page_address, cross_page_source,
                    sizeof(cross_page_source)) == 0 &&
            user_read(cross_page_address, cross_page_result,
                    sizeof(cross_page_result)) == 0 &&
            memcmp(cross_page_source, cross_page_result,
                    sizeof(cross_page_source)) == 0,
            "普通跨页读写按每页剩余长度分段");

    const byte_t partial_source[] = {0x11, 0x22, 0x33, 0x44};
    addr_t partial_address = PARTIAL_PAGE + PAGE_SIZE - 2;
    CHECK(user_write(partial_address, partial_source,
                    sizeof(partial_source)) == 1,
            "跨入未映射页时报告写入故障");
    byte_t partial_prefix[2] = {0};
    CHECK(user_read(partial_address, partial_prefix,
                    sizeof(partial_prefix)) == 0 &&
            memcmp(partial_prefix, partial_source,
                    sizeof(partial_prefix)) == 0,
            "未映射页故障保留已写入的前一页前缀");

    byte_t last_page_source[268];
    byte_t last_page_result[sizeof(last_page_source)];
    for (size_t index = 0; index < sizeof(last_page_source); index++)
        last_page_source[index] = (byte_t) index;
    addr_t last_page_address = UINT32_MAX -
            (addr_t) sizeof(last_page_source) + 1;
    CHECK(user_write(last_page_address, last_page_source,
                    sizeof(last_page_source)) == 0 &&
            user_read(last_page_address, last_page_result,
                    sizeof(last_page_result)) == 0 &&
            memcmp(last_page_source, last_page_result,
                    sizeof(last_page_source)) == 0,
            "末页内的读写不把页尾回绕为地址零");

    const byte_t top_sentinel[4] = {0xa1, 0xa2, 0xa3, 0xa4};
    const byte_t low_sentinel[4] = {0xb1, 0xb2, 0xb3, 0xb4};
    CHECK(user_write(UINT32_MAX - 3, top_sentinel,
                    sizeof(top_sentinel)) == 0 &&
            user_write(LOW_PAGE, low_sentinel,
                    sizeof(low_sentinel)) == 0,
            "写入地址空间两端哨兵");
    byte_t wrapping_source[8] = {0};
    CHECK(user_write(UINT32_MAX - 3, wrapping_source,
                    sizeof(wrapping_source)) == 1 &&
            user_write_task_ptrace(&task, UINT32_MAX - 3,
                    wrapping_source, sizeof(wrapping_source)) == 1,
            "普通与 ptrace 写入都拒绝跨越 4 GiB 上界");
    byte_t top_after[sizeof(top_sentinel)];
    byte_t low_after[sizeof(low_sentinel)];
    CHECK(user_read(UINT32_MAX - 3, top_after, sizeof(top_after)) == 0 &&
            user_read(LOW_PAGE, low_after, sizeof(low_after)) == 0 &&
            memcmp(top_after, top_sentinel, sizeof(top_after)) == 0 &&
            memcmp(low_after, low_sentinel, sizeof(low_after)) == 0,
            "回绕写入在访问任一 guest 页前失败");

    byte_t read_sentinel[8];
    memset(read_sentinel, 0x5a, sizeof(read_sentinel));
    CHECK(user_read(UINT32_MAX - 3, read_sentinel,
                    sizeof(read_sentinel)) == 1,
            "读取拒绝跨越 4 GiB 上界");
    for (size_t index = 0; index < sizeof(read_sentinel); index++)
        CHECK(read_sentinel[index] == 0x5a,
                "回绕读取在修改 host 输出前失败");

    byte_t one = 0;
    CHECK(user_read(UINT32_MAX, &one, 0) == 0 &&
            user_write(UINT32_MAX, &one, 0) == 0 &&
            user_write(UINT32_C(2), &one, SIZE_MAX) == 1,
            "零长度访问成功且超大长度在读取 host 缓冲前失败");

    const byte_t top_character = 'x';
    CHECK(user_write(UINT32_MAX, &top_character,
                    sizeof(top_character)) == 0,
            "在最高地址写入字符串首字节");
    char string_result[2] = {0};
    CHECK(user_read_string(UINT32_MAX, string_result,
                    sizeof(string_result)) == 1 &&
            string_result[0] == 'x',
            "字符串读取在地址上界停止且保留已读前缀");
    CHECK(user_write_string(UINT32_MAX, "yz") == 1 &&
            user_read(LOW_PAGE, low_after, sizeof(low_after)) == 0 &&
            memcmp(low_after, low_sentinel, sizeof(low_after)) == 0,
            "字符串写入不会回绕到地址零");

    current = NULL;
    mm_release(task.mm);
    return 0;
}

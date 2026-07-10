#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "asbestos/asbestos.h"
#include "asbestos/frame.h"

static_assert(sizeof(((struct fiber_block *) 0)->code[0]) == 8,
        "fiber block 代码流单元必须为 64 位");
static_assert(sizeof(((struct fiber_frame *) 0)->ret_cache[0]) == 8,
        "fiber 返回缓存单元必须为 64 位");

int main(void) {
    struct fiber_frame frame = {0};
    frame.ret_cache[0] = UINT64_C(0xfedcba9876543210);
    assert(frame.ret_cache[0] == UINT64_C(0xfedcba9876543210));
    return 0;
}

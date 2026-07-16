#include <string.h>

#include "debug.h"

int main(void) {
    char oversized[32768];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';

    ish_printk("%s", oversized);
    ish_printk("\n");

    // 同时覆盖多次无换行片段累计越过线程局部缓冲区的路径。
    for (unsigned index = 0; index < 4096; index++)
        ish_printk("partial-%u ", index);
    ish_printk("\n");
    return 0;
}

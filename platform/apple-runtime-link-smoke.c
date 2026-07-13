#include "kernel/init.h"

static int (*volatile runtime_entry)(void) = become_first_process;

int main(void) {
    // 保留真实入口的链接依赖，但验证程序本身不启动 guest。
    return runtime_entry == NULL;
}

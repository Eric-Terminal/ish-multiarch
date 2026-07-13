#include "platform/platform.h"

int main(void) {
    struct cpu_usage cpu = get_cpu_usage();
    struct mem_usage memory = get_mem_usage();
    struct uptime_info uptime = get_uptime();

    return cpu.user_ticks + cpu.system_ticks + cpu.idle_ticks == 0 ||
            memory.total == 0 || uptime.uptime_ticks == 0;
}

#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "platform/platform.h"

struct cpu_usage get_cpu_usage(void) {
    host_cpu_load_info_data_t load = {};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    kern_return_t status = host_statistics(
            mach_host_self(), HOST_CPU_LOAD_INFO,
            (host_info_t) &load, &count);
    assert(status == KERN_SUCCESS);
    struct cpu_usage usage;
    usage.user_ticks = load.cpu_ticks[CPU_STATE_USER];
    usage.system_ticks = load.cpu_ticks[CPU_STATE_SYSTEM];
    usage.idle_ticks = load.cpu_ticks[CPU_STATE_IDLE];
    usage.nice_ticks = load.cpu_ticks[CPU_STATE_NICE];
    return usage;
}

struct mem_usage get_mem_usage(void) {
    uint64_t total = 0;
    size_t total_size = sizeof(total);
    int sysctl_status = sysctlbyname(
            "hw.memsize", &total, &total_size, NULL, 0);
    assert(sysctl_status == 0 && total_size == sizeof(total));

    vm_statistics64_data_t vm = {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t status = host_statistics64(
            mach_host_self(), HOST_VM_INFO64,
            (host_info64_t) &vm, &count);
    assert(status == KERN_SUCCESS);
    uint64_t page_size = (uint64_t) vm_page_size;

    struct mem_usage usage;
    usage.total = total;
    usage.free = (uint64_t) vm.free_count * page_size;
    usage.active = (uint64_t) vm.active_count * page_size;
    usage.inactive = (uint64_t) vm.inactive_count * page_size;
    return usage;
}

struct uptime_info get_uptime(void) {
    struct timeval kern_boottime = {};
    size_t size = sizeof(kern_boottime);
    int status = sysctlbyname(
            "kern.boottime", &kern_boottime, &size, NULL, 0);
    assert(status == 0 && size == sizeof(kern_boottime));
    struct timeval now;
    status = gettimeofday(&now, NULL);
    assert(status == 0 && now.tv_sec >= kern_boottime.tv_sec);

    struct {
        uint32_t ldavg[3];
        long scale;
    } vm_loadavg = {};
    size = sizeof(vm_loadavg);
    status = sysctlbyname("vm.loadavg", &vm_loadavg, &size, NULL, 0);
    assert(status == 0 && size == sizeof(vm_loadavg));

    // linux wants the scale to be 16 bits
#if FSHIFT < 16
    for (int i = 0; i < 3; i++) {
        vm_loadavg.ldavg[i] <<= 16 - FSHIFT;
    }
#elif FSHIFT > 16
    for (int i = 0; i < 3; i++) {
        vm_loadavg.ldavg[i] >>= FSHIFT - 16;
    }
#endif

    struct uptime_info uptime = {
        .uptime_ticks = (uint64_t) (now.tv_sec - kern_boottime.tv_sec),
        .load_1m = vm_loadavg.ldavg[0],
        .load_5m = vm_loadavg.ldavg[1],
        .load_15m = vm_loadavg.ldavg[2],
    };
    return uptime;
}

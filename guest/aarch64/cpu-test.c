#include <assert.h>
#include <stdint.h>

#include "emu/cpu.h"

int main(void) {
    struct cpu_state cpu = {0};

    cpu_set_pc(&cpu, UINT64_C(0x123456789abc));
    assert(cpu_get_pc(&cpu) == UINT64_C(0x123456789abc));

    cpu.x[30] = UINT64_C(0xfedcba9876543210);
    assert(cpu.x[30] == UINT64_C(0xfedcba9876543210));

    aarch64_set_nzcv(&cpu, UINT32_C(0xffffffff));
    assert(aarch64_get_nzcv(&cpu) == AARCH64_NZCV_MASK);

    aarch64_set_exclusive(&cpu, UINT64_C(0x4000), 8, false,
            UINT64_C(0x1020304050607080), 0, NULL, 7, 11);
    assert(aarch64_exclusive_matches(&cpu,
            UINT64_C(0x4000), 8, false));
    assert(!aarch64_exclusive_matches(&cpu,
            UINT64_C(0x4000), 8, true));
    assert(!aarch64_exclusive_matches(&cpu,
            UINT64_C(0x4008), 8, false));
    aarch64_clear_exclusive(&cpu);
    assert(!cpu.exclusive.valid);
    return 0;
}

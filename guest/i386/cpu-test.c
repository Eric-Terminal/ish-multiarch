#include <assert.h>

#include "emu/cpu.h"

int main(void) {
    struct cpu_state cpu = {0};

    cpu_set_pc(&cpu, 0x12345678);
    assert(cpu_get_pc(&cpu) == 0x12345678);

    cpu.tf = 1;
    assert(cpu_is_single_step(&cpu));

    cpu_set_trap(&cpu, 13);
    assert(cpu.trapno == 13);
    return 0;
}

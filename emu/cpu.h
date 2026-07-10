#ifndef EMU_CPU_H
#define EMU_CPU_H

#include "guest/i386/cpu.h"

struct tlb;

int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb);
void cpu_poke(struct cpu_state *cpu);

#endif

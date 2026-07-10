#ifndef EMU_CPU_H
#define EMU_CPU_H

#include "guest/selection.h"

#if defined(ISH_GUEST_I386)
#include "guest/i386/cpu.h"
#elif defined(ISH_GUEST_AARCH64)
#include "guest/aarch64/cpu.h"
#endif

struct tlb;

int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb);
void cpu_poke(struct cpu_state *cpu);

#endif

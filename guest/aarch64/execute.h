#ifndef GUEST_AARCH64_EXECUTE_H
#define GUEST_AARCH64_EXECUTE_H

#include "emu/cpu.h"
#include "guest/aarch64/decode.h"

void aarch64_execute(struct cpu_state *cpu, const struct aarch64_decoded *instruction);

#endif

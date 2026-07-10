#ifndef GUEST_AARCH64_CONDITION_H
#define GUEST_AARCH64_CONDITION_H

#include "misc.h"

bool aarch64_condition_holds(dword_t nzcv, byte_t condition);

#endif

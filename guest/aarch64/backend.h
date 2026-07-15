#ifndef GUEST_AARCH64_BACKEND_H
#define GUEST_AARCH64_BACKEND_H

#include "misc.h"

enum aarch64_backend {
    AARCH64_BACKEND_C,
    AARCH64_BACKEND_THREADED,
};

enum aarch64_backend aarch64_backend_default(void);
bool aarch64_backend_available(enum aarch64_backend backend);

#endif

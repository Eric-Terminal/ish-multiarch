#include "guest/aarch64/backend.h"

enum aarch64_backend aarch64_backend_default(void) {
    return AARCH64_BACKEND_C;
}

bool aarch64_backend_available(enum aarch64_backend backend) {
    switch (backend) {
        case AARCH64_BACKEND_C:
            return true;
        case AARCH64_BACKEND_THREADED:
#if defined(__aarch64__)
            return true;
#else
            return false;
#endif
    }
    return false;
}

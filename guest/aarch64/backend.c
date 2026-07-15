#include "guest/aarch64/backend.h"
#include "aarch64-backend-config.h"

enum aarch64_backend aarch64_backend_default(void) {
#if ISH_AARCH64_BACKEND_THREADED_DEFAULT
    return AARCH64_BACKEND_THREADED;
#else
    return AARCH64_BACKEND_C;
#endif
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

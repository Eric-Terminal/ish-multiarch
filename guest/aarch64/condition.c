#include <assert.h>

#include "guest/aarch64/condition.h"

bool aarch64_condition_holds(dword_t nzcv, byte_t condition) {
    assert(condition < 16);
    bool n = (nzcv & (UINT32_C(1) << 31)) != 0;
    bool z = (nzcv & (UINT32_C(1) << 30)) != 0;
    bool c = (nzcv & (UINT32_C(1) << 29)) != 0;
    bool v = (nzcv & (UINT32_C(1) << 28)) != 0;

    switch (condition) {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        case 13: return z || n != v;
        case 14:
        case 15:
            return true;
    }
    return false;
}

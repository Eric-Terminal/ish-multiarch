#include <assert.h>

#include "guest/aarch64/condition.h"

int main(void) {
    static const word_t expected[] = {
        UINT16_C(0xf0f0), UINT16_C(0x0f0f),
        UINT16_C(0xcccc), UINT16_C(0x3333),
        UINT16_C(0xff00), UINT16_C(0x00ff),
        UINT16_C(0xaaaa), UINT16_C(0x5555),
        UINT16_C(0x0c0c), UINT16_C(0xf3f3),
        UINT16_C(0xaa55), UINT16_C(0x55aa),
        UINT16_C(0x0a05), UINT16_C(0xf5fa),
        UINT16_C(0xffff), UINT16_C(0xffff),
    };
    for (byte_t condition = 0; condition < 16; condition++) {
        for (byte_t flags = 0; flags < 16; flags++) {
            dword_t nzcv = (dword_t) flags << 28;
            bool should_hold = ((expected[condition] >> flags) & 1) != 0;
            assert(aarch64_condition_holds(nzcv, condition) == should_hold);
        }
    }
    return 0;
}

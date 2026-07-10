#include "guest/aarch64/decode.h"

int main(void) {
    struct aarch64_decoded instruction;
    return aarch64_decode(UINT32_C(0xd503201f), &instruction) ? 0 : 1;
}

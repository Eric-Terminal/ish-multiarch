#include "guest/aarch64/decode.h"
#include "guest/aarch64/linux-file-abi.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-signal-abi.h"

int main(void) {
    struct aarch64_decoded instruction;
    const struct aarch64_linux_process_config config = {
        .load_bias = UINT64_C(0x0000400000000000),
        .stack_top = UINT64_C(0x00007fff00000000),
        .stack_size = UINT64_C(0x2000),
        .signal_trampoline_page = UINT64_C(0x00007ffe00000000),
        .brk_limit = UINT64_C(0x0000400010000000),
    };
    const struct aarch64_linux_process_result result = {
        .fault.address = UINT64_C(0x00007fff12345000),
    };
    if (config.load_bias <= UINT32_MAX ||
            config.stack_top <= UINT32_MAX ||
            config.signal_trampoline_page <= UINT32_MAX ||
            config.brk_limit <= UINT32_MAX ||
            result.fault.address <= UINT32_MAX)
        return 1;
    return aarch64_decode(UINT32_C(0xd503201f), &instruction) ? 0 : 1;
}

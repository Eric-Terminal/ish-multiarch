#include "platform/apple-rootfs-sha256.h"

#include <string.h>

// SHA-256 只用于固定 bundle 的流式完整性校验，保持实现无平台加密依赖。

static const uint32_t round_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2),
};

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static uint32_t load_big_endian(const unsigned char *bytes) {
    return ((uint32_t) bytes[0] << 24) |
            ((uint32_t) bytes[1] << 16) |
            ((uint32_t) bytes[2] << 8) |
            (uint32_t) bytes[3];
}

static void transform(
        struct ish_apple_rootfs_sha256 *state,
        const unsigned char block[64]) {
    uint32_t schedule[64];
    for (size_t index = 0; index < 16; index++)
        schedule[index] = load_big_endian(block + index * 4);
    for (size_t index = 16; index < 64; index++) {
        uint32_t left = schedule[index - 15];
        uint32_t right = schedule[index - 2];
        uint32_t sigma_zero = rotate_right(left, 7) ^
                rotate_right(left, 18) ^ (left >> 3);
        uint32_t sigma_one = rotate_right(right, 17) ^
                rotate_right(right, 19) ^ (right >> 10);
        schedule[index] = schedule[index - 16] + sigma_zero +
                schedule[index - 7] + sigma_one;
    }

    uint32_t a = state->words[0];
    uint32_t b = state->words[1];
    uint32_t c = state->words[2];
    uint32_t d = state->words[3];
    uint32_t e = state->words[4];
    uint32_t f = state->words[5];
    uint32_t g = state->words[6];
    uint32_t h = state->words[7];
    for (size_t index = 0; index < 64; index++) {
        uint32_t sum_one = rotate_right(e, 6) ^
                rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t first = h + sum_one + choose +
                round_constants[index] + schedule[index];
        uint32_t sum_zero = rotate_right(a, 2) ^
                rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = sum_zero + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state->words[0] += a;
    state->words[1] += b;
    state->words[2] += c;
    state->words[3] += d;
    state->words[4] += e;
    state->words[5] += f;
    state->words[6] += g;
    state->words[7] += h;
}

void ish_apple_rootfs_sha256_initialize(
        struct ish_apple_rootfs_sha256 *state) {
    *state = (struct ish_apple_rootfs_sha256) {
        .words = {
            UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
            UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
            UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
            UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19),
        },
    };
}

void ish_apple_rootfs_sha256_update(
        struct ish_apple_rootfs_sha256 *state,
        const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    state->byte_count += (uint64_t) length;
    while (length != 0) {
        size_t available = sizeof(state->block) - state->block_length;
        size_t count = length < available ? length : available;
        memcpy(state->block + state->block_length, cursor, count);
        state->block_length += count;
        cursor += count;
        length -= count;
        if (state->block_length == sizeof(state->block)) {
            transform(state, state->block);
            state->block_length = 0;
        }
    }
}

void ish_apple_rootfs_sha256_finish(
        struct ish_apple_rootfs_sha256 *state,
        unsigned char digest[32]) {
    uint64_t bit_count = state->byte_count * UINT64_C(8);
    state->block[state->block_length++] = 0x80;
    if (state->block_length > 56) {
        memset(state->block + state->block_length, 0,
                sizeof(state->block) - state->block_length);
        transform(state, state->block);
        state->block_length = 0;
    }
    memset(state->block + state->block_length, 0,
            56 - state->block_length);
    for (size_t index = 0; index < 8; index++)
        state->block[56 + index] =
                (unsigned char) (bit_count >> (56 - index * 8));
    transform(state, state->block);

    for (size_t index = 0; index < 8; index++) {
        digest[index * 4] = (unsigned char) (state->words[index] >> 24);
        digest[index * 4 + 1] =
                (unsigned char) (state->words[index] >> 16);
        digest[index * 4 + 2] =
                (unsigned char) (state->words[index] >> 8);
        digest[index * 4 + 3] = (unsigned char) state->words[index];
    }
    memset(state, 0, sizeof(*state));
}

void ish_apple_rootfs_sha256_hex(
        const unsigned char digest[32], char hexadecimal[65]) {
    static const char alphabet[] = "0123456789abcdef";
    for (size_t index = 0; index < 32; index++) {
        hexadecimal[index * 2] = alphabet[digest[index] >> 4];
        hexadecimal[index * 2 + 1] = alphabet[digest[index] & 0x0f];
    }
    hexadecimal[64] = '\0';
}

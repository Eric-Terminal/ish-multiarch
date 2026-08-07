#ifndef PLATFORM_APPLE_ROOTFS_SHA256_H
#define PLATFORM_APPLE_ROOTFS_SHA256_H

#include <stddef.h>
#include <stdint.h>

// RootFS bundle 校验专用；状态不跨公共 ABI 暴露。
struct ish_apple_rootfs_sha256 {
    uint32_t words[8];
    uint64_t byte_count;
    unsigned char block[64];
    size_t block_length;
};

#pragma GCC visibility push(hidden)

void ish_apple_rootfs_sha256_initialize(
        struct ish_apple_rootfs_sha256 *state);
void ish_apple_rootfs_sha256_update(
        struct ish_apple_rootfs_sha256 *state,
        const void *bytes, size_t length);
void ish_apple_rootfs_sha256_finish(
        struct ish_apple_rootfs_sha256 *state,
        unsigned char digest[32]);
void ish_apple_rootfs_sha256_hex(
        const unsigned char digest[32], char hexadecimal[65]);

#pragma GCC visibility pop

#endif

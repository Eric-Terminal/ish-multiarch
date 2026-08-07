#include "platform/apple-rootfs-archive.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum probe_mode {
    PROBE_INSTALL,
    PROBE_CANCEL,
    PROBE_BAD_HASH,
    PROBE_INVALID_ARCHIVE,
};

struct probe_progress {
    enum probe_mode mode;
    uint32_t previous_phase;
    uint64_t previous_compressed;
    uint64_t previous_extracted;
    bool invalid;
    bool cancelled;
};

static int32_t progress_callback(
        void *opaque,
        const struct ish_apple_rootfs_archive_progress_v1 *progress) {
    struct probe_progress *state = opaque;
    if (progress->version != ISH_APPLE_ABI_VERSION ||
            progress->structure_size != sizeof(*progress) ||
            progress->compressed_bytes_completed >
                    progress->compressed_bytes_total ||
            progress->extracted_bytes_completed >
                    progress->extracted_bytes_total ||
            progress->entries_completed > progress->entries_total ||
            progress->phase < state->previous_phase) {
        state->invalid = true;
        return ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CANCEL;
    }
    if (progress->phase == state->previous_phase &&
            (progress->compressed_bytes_completed <
                    state->previous_compressed ||
             progress->extracted_bytes_completed <
                    state->previous_extracted)) {
        state->invalid = true;
        return ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CANCEL;
    }
    state->previous_phase = progress->phase;
    state->previous_compressed = progress->compressed_bytes_completed;
    state->previous_extracted = progress->extracted_bytes_completed;
    if (state->mode == PROBE_CANCEL &&
            progress->phase == ISH_APPLE_ROOTFS_ARCHIVE_PHASE_EXTRACT) {
        state->cancelled = true;
        return ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CANCEL;
    }
    return ISH_APPLE_ROOTFS_ARCHIVE_PROGRESS_CONTINUE;
}

static int parse_u64(const char *value, uint64_t *result) {
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0)
        return EINVAL;
    *result = (uint64_t) parsed;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "用法：%s <archive> <sha256> <bytes> <entries> "
                "<parent> <root> <install|cancel|bad-hash|invalid-archive>\n",
                argv[0]);
        return 2;
    }
    enum probe_mode mode;
    if (strcmp(argv[7], "install") == 0)
        mode = PROBE_INSTALL;
    else if (strcmp(argv[7], "cancel") == 0)
        mode = PROBE_CANCEL;
    else if (strcmp(argv[7], "bad-hash") == 0)
        mode = PROBE_BAD_HASH;
    else if (strcmp(argv[7], "invalid-archive") == 0)
        mode = PROBE_INVALID_ARCHIVE;
    else
        return 2;

    uint64_t uncompressed_bytes;
    uint64_t entry_count;
    if (parse_u64(argv[3], &uncompressed_bytes) != 0 ||
            parse_u64(argv[4], &entry_count) != 0)
        return 2;
    const char *digest = mode == PROBE_BAD_HASH ?
            "00000000000000000000000000000000"
            "00000000000000000000000000000000" : argv[2];
    struct ish_apple_rootfs_archive_spec_v1 spec = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = (uint32_t) sizeof(spec),
        .archive_path = argv[1],
        .expected_sha256 = digest,
        .persistent_parent = argv[5],
        .root_name = argv[6],
        .expected_uncompressed_bytes = uncompressed_bytes,
        .expected_entry_count = entry_count,
    };
    struct probe_progress progress = {.mode = mode};
    struct ish_apple_rootfs_archive_callbacks_v1 callbacks = {
        .version = ISH_APPLE_ABI_VERSION,
        .structure_size = (uint32_t) sizeof(callbacks),
        .context = &progress,
        .progress = progress_callback,
    };
    enum ish_apple_rootfs_seed_result result =
            ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
    int error = ish_apple_rootfs_archive_install(
            &spec, &callbacks, &result);
    if (progress.invalid) {
        fprintf(stderr, "归档进度事件不满足单调性或 ABI 合同。\n");
        return 1;
    }
    if (mode == PROBE_CANCEL) {
        if (error != ECANCELED || !progress.cancelled) {
            fprintf(stderr, "归档取消结果错误：%d\n", error);
            return 1;
        }
        puts("cancelled");
        return 0;
    }
    if (mode == PROBE_BAD_HASH || mode == PROBE_INVALID_ARCHIVE) {
        if (error != EBADMSG) {
            fprintf(stderr, "损坏归档返回值错误：%d\n", error);
            return 1;
        }
        puts(mode == PROBE_BAD_HASH ? "bad-hash" : "invalid-archive");
        return 0;
    }
    if (error != 0) {
        fprintf(stderr, "归档安装失败：%d\n", error);
        return 1;
    }
    puts(result == ISH_APPLE_ROOTFS_SEED_INSTALLED ?
            "installed" : "already-present");
    return 0;
}

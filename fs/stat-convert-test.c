#include <stdio.h>

#include "fs/stat.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "stat64 转换测试失败：%s（第 %d 行）\n", \
                message, __LINE__); \
        return 1; \
    } \
} while (0)

struct newstat64 stat_convert_newstat64(struct statbuf stat);

int main(void) {
    const struct statbuf source = {
        .dev = UINT64_C(0x0102030405060708),
        .inode = UINT64_C(0x1112131415161718),
        .mode = 0100644,
        .nlink = 2,
        .uid = UINT32_C(0xa1b2c3d4),
        .gid = UINT32_C(0x10203040),
        .rdev = UINT64_C(0x2122232425262728),
        .size = UINT64_C(0x3132333435363738),
        .blksize = 4096,
        .blocks = UINT64_C(0x4142434445464748),
        .atime = UINT32_C(0x51525354),
        .atime_nsec = UINT32_C(0x61626364),
        .mtime = UINT32_C(0x71727374),
        .mtime_nsec = UINT32_C(0x81828384),
        .ctime = UINT32_C(0x91929394),
        .ctime_nsec = UINT32_C(0xa1a2a3a4),
    };
    struct newstat64 converted = stat_convert_newstat64(source);

    CHECK(converted._pad1 == 0 && converted._pad2 == 0,
            "所有 guest ABI 保留字段必须清零");
    CHECK(converted.dev == source.dev && converted.rdev == source.rdev &&
            converted.ino == source.inode &&
            converted.fucked_ino == (dword_t) source.inode,
            "设备号与 inode 字段保持位值");
    CHECK(converted.mode == source.mode && converted.nlink == source.nlink &&
            converted.uid == source.uid && converted.gid == source.gid,
            "权限与所有者字段保持位值");
    CHECK(converted.size == source.size &&
            converted.blksize == source.blksize &&
            converted.blocks == source.blocks,
            "文件大小字段保持位值");
    CHECK(converted.atime == source.atime &&
            converted.atime_nsec == source.atime_nsec &&
            converted.mtime == source.mtime &&
            converted.mtime_nsec == source.mtime_nsec &&
            converted.ctime == source.ctime &&
            converted.ctime_nsec == source.ctime_nsec,
            "时间字段保持位值");
    return 0;
}

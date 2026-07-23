#ifndef GUEST_AARCH64_LINUX_FILE_ABI_H
#define GUEST_AARCH64_LINUX_FILE_ABI_H

#include "misc.h"

struct aarch64_linux_iovec {
    qword_t base;
    qword_t length;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_iovec) == 16 &&
        _Alignof(struct aarch64_linux_iovec) == 8 &&
        __builtin_offsetof(struct aarch64_linux_iovec, base) == 0 &&
        __builtin_offsetof(struct aarch64_linux_iovec, length) == 8,
        "AArch64 Linux iovec ABI 必须固定为两个连续 qword");

struct aarch64_linux_epoll_event {
    dword_t events;
    dword_t padding;
    qword_t data;
} __attribute__((aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_epoll_event) == 16 &&
        _Alignof(struct aarch64_linux_epoll_event) == 8 &&
        __builtin_offsetof(struct aarch64_linux_epoll_event, events) == 0 &&
        __builtin_offsetof(struct aarch64_linux_epoll_event, data) == 8,
        "AArch64 Linux epoll_event ABI 必须保留 4 字节对齐填充");

#define AARCH64_LINUX_DIRENT64_NAME_OFFSET 19
#define AARCH64_LINUX_DIRENT64_ALIGNMENT 8
#define AARCH64_LINUX_DIRENT64_MAX_SIZE 280

struct aarch64_linux_dirent64 {
    qword_t inode;
    sqword_t next_offset;
    word_t length;
    byte_t type;
    char name[];
} __attribute__((packed));

_Static_assert(sizeof(struct aarch64_linux_dirent64) ==
        AARCH64_LINUX_DIRENT64_NAME_OFFSET &&
        __builtin_offsetof(struct aarch64_linux_dirent64, inode) == 0 &&
        __builtin_offsetof(struct aarch64_linux_dirent64, next_offset) == 8 &&
        __builtin_offsetof(struct aarch64_linux_dirent64, length) == 16 &&
        __builtin_offsetof(struct aarch64_linux_dirent64, type) == 18 &&
        __builtin_offsetof(struct aarch64_linux_dirent64, name) == 19,
        "AArch64 Linux dirent64 字段偏移必须与内核 ABI 一致");

struct aarch64_linux_stat {
    qword_t dev;
    qword_t ino;
    dword_t mode;
    dword_t nlink;
    dword_t uid;
    dword_t gid;
    qword_t rdev;
    qword_t pad1;
    sqword_t size;
    sdword_t blksize;
    sdword_t pad2;
    sqword_t blocks;
    sqword_t atime_sec;
    qword_t atime_nsec;
    sqword_t mtime_sec;
    qword_t mtime_nsec;
    sqword_t ctime_sec;
    qword_t ctime_nsec;
    dword_t unused4;
    dword_t unused5;
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_stat) == 128 &&
        _Alignof(struct aarch64_linux_stat) == 8,
        "AArch64 Linux stat ABI 必须固定为 128 字节且按 8 字节对齐");
_Static_assert(__builtin_offsetof(struct aarch64_linux_stat, dev) == 0 &&
        __builtin_offsetof(struct aarch64_linux_stat, ino) == 8 &&
        __builtin_offsetof(struct aarch64_linux_stat, mode) == 16 &&
        __builtin_offsetof(struct aarch64_linux_stat, nlink) == 20 &&
        __builtin_offsetof(struct aarch64_linux_stat, uid) == 24 &&
        __builtin_offsetof(struct aarch64_linux_stat, gid) == 28 &&
        __builtin_offsetof(struct aarch64_linux_stat, rdev) == 32 &&
        __builtin_offsetof(struct aarch64_linux_stat, pad1) == 40,
        "AArch64 Linux stat 前半字段偏移不正确");
_Static_assert(__builtin_offsetof(struct aarch64_linux_stat, size) == 48 &&
        __builtin_offsetof(struct aarch64_linux_stat, blksize) == 56 &&
        __builtin_offsetof(struct aarch64_linux_stat, pad2) == 60 &&
        __builtin_offsetof(struct aarch64_linux_stat, blocks) == 64,
        "AArch64 Linux stat 文件大小字段偏移不正确");
_Static_assert(__builtin_offsetof(struct aarch64_linux_stat, atime_sec) == 72 &&
        __builtin_offsetof(struct aarch64_linux_stat, atime_nsec) == 80 &&
        __builtin_offsetof(struct aarch64_linux_stat, mtime_sec) == 88 &&
        __builtin_offsetof(struct aarch64_linux_stat, mtime_nsec) == 96 &&
        __builtin_offsetof(struct aarch64_linux_stat, ctime_sec) == 104 &&
        __builtin_offsetof(struct aarch64_linux_stat, ctime_nsec) == 112 &&
        __builtin_offsetof(struct aarch64_linux_stat, unused4) == 120 &&
        __builtin_offsetof(struct aarch64_linux_stat, unused5) == 124,
        "AArch64 Linux stat 时间与保留字段偏移不正确");

struct aarch64_linux_statfs {
    sqword_t type;
    sqword_t bsize;
    sqword_t blocks;
    sqword_t bfree;
    sqword_t bavail;
    sqword_t files;
    sqword_t ffree;
    sdword_t fsid[2];
    sqword_t namelen;
    sqword_t frsize;
    sqword_t flags;
    sqword_t spare[4];
} __attribute__((packed, aligned(8)));

_Static_assert(sizeof(struct aarch64_linux_statfs) == 120 &&
        _Alignof(struct aarch64_linux_statfs) == 8,
        "AArch64 Linux statfs ABI 必须固定为 120 字节且按 8 字节对齐");
_Static_assert(__builtin_offsetof(struct aarch64_linux_statfs, type) == 0 &&
        __builtin_offsetof(struct aarch64_linux_statfs, blocks) == 16 &&
        __builtin_offsetof(struct aarch64_linux_statfs, fsid) == 56 &&
        __builtin_offsetof(struct aarch64_linux_statfs, namelen) == 64 &&
        __builtin_offsetof(struct aarch64_linux_statfs, flags) == 80 &&
        __builtin_offsetof(struct aarch64_linux_statfs, spare) == 88,
        "AArch64 Linux statfs 字段偏移必须与 asm-generic ABI 一致");

#endif

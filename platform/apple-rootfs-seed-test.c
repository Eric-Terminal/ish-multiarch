#include "platform/apple-rootfs-seed.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sqlite3.h>

#ifndef __APPLE__
#error "Apple rootfs seed 测试只能在 Apple 宿主运行"
#endif

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "Apple rootfs seed 测试失败：%s（第 %d 行，errno=%d）\n", \
                message, __LINE__, errno); \
        return 1; \
    } \
} while (0)

static const char digest_a[] =
        "1111111111111111111111111111111111111111111111111111111111111111";
static const char digest_b[] =
        "2222222222222222222222222222222222222222222222222222222222222222";
static const char owner_magic[] = "format=ish-rootfs-install-owner-v2";
static char executable_path[PATH_MAX];

extern int ish_apple_rootfs_seed_test_fail_phase;

enum rootfs_seed_test_phase {
    ROOTFS_SEED_TEST_NONE,
    ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC,
    ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC,
    ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC,
    ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC,
};

struct fixture {
    char base[PATH_MAX];
    char seed[PATH_MAX];
    char persistent[PATH_MAX];
    unsigned hardlink_groups;
};

struct child_report {
    int error;
    int result;
};

static int format_path(char output[PATH_MAX], const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(output, PATH_MAX, format, arguments);
    va_end(arguments);
    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static int write_all(int file, const void *bytes, size_t length) {
    const unsigned char *cursor = bytes;
    while (length != 0) {
        ssize_t count = write(file, cursor, length);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            return -1;
        }
        cursor += (size_t) count;
        length -= (size_t) count;
    }
    return 0;
}

static int read_all(int file, void *bytes, size_t length) {
    unsigned char *cursor = bytes;
    while (length != 0) {
        ssize_t count = read(file, cursor, length);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            return -1;
        }
        cursor += (size_t) count;
        length -= (size_t) count;
    }
    return 0;
}

static int write_file(
        const char *path, const void *bytes, size_t length, mode_t mode) {
    int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (file < 0)
        return -1;
    int result = write_all(file, bytes, length);
    int saved_errno = errno;
    if (close(file) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    errno = saved_errno;
    return result;
}

static int file_equals(const char *path, const void *expected, size_t length) {
    int file = open(path, O_RDONLY);
    if (file < 0)
        return 0;
    unsigned char *actual = malloc(length == 0 ? 1 : length);
    if (actual == NULL) {
        close(file);
        return 0;
    }
    int matches = read_all(file, actual, length) == 0;
    unsigned char extra;
    ssize_t count;
    do {
        count = read(file, &extra, 1);
    } while (count < 0 && errno == EINTR);
    matches = matches && count == 0 && memcmp(actual, expected, length) == 0;
    free(actual);
    close(file);
    return matches;
}

static int path_absent(const char *path) {
    struct stat metadata;
    if (lstat(path, &metadata) == 0)
        return 0;
    return errno == ENOENT;
}

static int same_entry(const struct stat *before, const char *path) {
    struct stat after;
    return lstat(path, &after) == 0 &&
            before->st_dev == after.st_dev &&
            before->st_ino == after.st_ino &&
            before->st_mode == after.st_mode;
}

static int symlink_equals(const char *path, const char *expected) {
    char target[PATH_MAX];
    ssize_t length = readlink(path, target, sizeof(target));
    size_t expected_length = strlen(expected);
    return length >= 0 && (size_t) length == expected_length &&
            memcmp(target, expected, expected_length) == 0;
}

static int remove_tree(const char *path) {
    struct stat metadata;
    if (lstat(path, &metadata) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(metadata.st_mode))
        return unlink(path);

    // 先关闭枚举目录再递归，深度边界夹具不能耗尽每层一个 fd。
    struct dirent **entries = NULL;
    int count = scandir(path, &entries, NULL, NULL);
    if (count < 0)
        return -1;
    int result = 0;
    int saved_errno = 0;
    for (int i = 0; i < count; i++) {
        struct dirent *entry = entries[i];
        if (result != 0 || strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
            free(entry);
            continue;
        }
        char child[PATH_MAX];
        if (format_path(child, "%s/%s", path, entry->d_name) < 0) {
            result = -1;
            saved_errno = ENAMETOOLONG;
        } else if (remove_tree(child) < 0) {
            result = -1;
            saved_errno = errno;
        }
        free(entry);
    }
    free(entries);
    if (result == 0 && rmdir(path) < 0) {
        result = -1;
        saved_errno = errno;
    }
    if (result != 0)
        errno = saved_errno;
    return result;
}

static int sqlite_exec_checked(sqlite3 *database, const char *sql) {
    char *message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (result != SQLITE_OK)
        fprintf(stderr, "SQLite 夹具错误：%s\n", message == NULL ?
                sqlite3_errmsg(database) : message);
    sqlite3_free(message);
    return result == SQLITE_OK ? 0 : -1;
}

static int insert_stat(sqlite3 *database, sqlite3_int64 inode) {
    static const uint32_t fake_stat[4] = {
        UINT32_C(0100755), UINT32_C(0), UINT32_C(0), UINT32_C(0),
    };
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "insert into stats (inode, stat) values (?, ?)", -1,
            &statement, NULL) != SQLITE_OK)
        return -1;
    int result = sqlite3_bind_int64(statement, 1, inode);
    if (result == SQLITE_OK)
        result = sqlite3_bind_blob(statement, 2, fake_stat,
                (int) sizeof(fake_stat), SQLITE_STATIC);
    if (result == SQLITE_OK)
        result = sqlite3_step(statement);
    int finalize_result = sqlite3_finalize(statement);
    return result == SQLITE_DONE && finalize_result == SQLITE_OK ? 0 : -1;
}

static int insert_path(
        sqlite3 *database, const char *path, sqlite3_int64 inode) {
    size_t length = strlen(path);
    if (length > INT_MAX)
        return -1;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "insert into paths (path, inode) values (?, ?)", -1,
            &statement, NULL) != SQLITE_OK)
        return -1;
    int result = sqlite3_bind_blob(statement, 1, path,
            (int) length, SQLITE_STATIC);
    if (result == SQLITE_OK)
        result = sqlite3_bind_int64(statement, 2, inode);
    if (result == SQLITE_OK)
        result = sqlite3_step(statement);
    int finalize_result = sqlite3_finalize(statement);
    return result == SQLITE_DONE && finalize_result == SQLITE_OK ? 0 : -1;
}

static int create_database(const struct fixture *fixture) {
    char database_path[PATH_MAX];
    if (format_path(database_path, "%s/meta.db", fixture->seed) < 0)
        return -1;
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(database_path, &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        sqlite3_close(database);
        return -1;
    }
    static const char schema[] =
            "pragma journal_mode=delete;"
            "create table meta (id integer unique default 0, db_inode integer);"
            "insert into meta (db_inode) values (0);"
            "create table stats (inode integer primary key, stat blob);"
            "create table paths (path blob primary key, "
                    "inode integer references stats(inode));"
            "create index inode_to_path on paths (inode, path);"
            "pragma user_version=3;";
    int result = sqlite_exec_checked(database, schema);
    if (result == 0)
        result = insert_stat(database, 1);
    if (result == 0)
        result = insert_stat(database, 2);
    if (result == 0)
        result = insert_path(database, "", 1);
    if (result == 0)
        result = insert_path(database, "/bin/busybox", 2);
    if (result == 0 && fixture->hardlink_groups >= 1)
        result = insert_stat(database, 3);
    if (result == 0 && fixture->hardlink_groups >= 1)
        result = insert_path(database, "/usr/lib/alpha-alias", 3);
    if (result == 0 && fixture->hardlink_groups >= 1)
        result = insert_path(database, "/usr/lib/alpha-middle", 3);
    if (result == 0 && fixture->hardlink_groups >= 1)
        result = insert_path(database, "/usr/lib/alpha-source", 3);
    if (result == 0 && fixture->hardlink_groups >= 2)
        result = insert_stat(database, 4);
    if (result == 0 && fixture->hardlink_groups >= 2)
        result = insert_path(database, "/usr/lib/beta-alias", 4);
    if (result == 0 && fixture->hardlink_groups >= 2)
        result = insert_path(database, "/usr/lib/beta-source", 4);
    if (sqlite3_close(database) != SQLITE_OK)
        result = -1;
    return result;
}

static int write_manifest(
        const struct fixture *fixture, const char *source_kind,
        const char *digest) {
    char path[PATH_MAX];
    if (format_path(path, "%s/rootfs-manifest.txt", fixture->seed) < 0)
        return -1;
    char manifest[1024];
    int length = snprintf(manifest, sizeof(manifest),
            "format=ish-fakefs-v3\n"
            "packager=apple-aarch64-rootfs-v1\n"
            "guest_arch=aarch64\n"
            "source_kind=%s\n"
            "alpine_version=3.24.1\n"
            "archive_sha256=%s\n"
            "source_url=https://example.invalid/alpine.tar.gz\n"
            "hardlinks=rootfs-hardlinks.tsv\n",
            source_kind, digest);
    if (length < 0 || (size_t) length >= sizeof(manifest))
        return -1;
    return write_file(path, manifest, (size_t) length, 0600);
}

static int write_hardlink_manifest(const struct fixture *fixture) {
    static const char one_group[] =
            "usr/lib/alpha-alias\tusr/lib/alpha-alias\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-middle\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n";
    static const char two_groups[] =
            "usr/lib/alpha-alias\tusr/lib/alpha-alias\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-middle\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n"
            "usr/lib/beta-alias\tusr/lib/beta-alias\n"
            "usr/lib/beta-alias\tusr/lib/beta-source\n";
    const char *contents = "";
    size_t length = 0;
    if (fixture->hardlink_groups == 1) {
        contents = one_group;
        length = sizeof(one_group) - 1;
    } else if (fixture->hardlink_groups == 2) {
        contents = two_groups;
        length = sizeof(two_groups) - 1;
    }
    char path[PATH_MAX];
    if (format_path(path, "%s/rootfs-hardlinks.tsv", fixture->seed) < 0)
        return -1;
    return write_file(path, contents, length, 0600);
}

static int write_busybox(const struct fixture *fixture, unsigned machine) {
    unsigned char elf[64] = {0};
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[16] = 2;
    elf[18] = (unsigned char) (machine & 0xffu);
    elf[19] = (unsigned char) ((machine >> 8u) & 0xffu);
    char path[PATH_MAX];
    if (format_path(path, "%s/data/bin/busybox", fixture->seed) < 0)
        return -1;
    if (write_file(path, elf, sizeof(elf), 0700) < 0)
        return -1;
    return chmod(path, 0700);
}

static int create_fixture(
        const char *workspace, const char *name,
        unsigned hardlink_groups, struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->hardlink_groups = hardlink_groups;
    if (format_path(fixture->base, "%s/%s", workspace, name) < 0 ||
            format_path(fixture->seed, "%s/seed", fixture->base) < 0 ||
            format_path(fixture->persistent,
                    "%s/persistent", fixture->base) < 0)
        return -1;
    if (mkdir(fixture->base, 0700) < 0 ||
            mkdir(fixture->seed, 0700) < 0 ||
            mkdir(fixture->persistent, 0700) < 0)
        return -1;

    char path[PATH_MAX];
    if (format_path(path, "%s/data", fixture->seed) < 0 ||
            mkdir(path, 0700) < 0 ||
            format_path(path, "%s/data/bin", fixture->seed) < 0 ||
            mkdir(path, 0700) < 0 ||
            format_path(path, "%s/data/usr", fixture->seed) < 0 ||
            mkdir(path, 0700) < 0 ||
            format_path(path, "%s/data/usr/lib", fixture->seed) < 0 ||
            mkdir(path, 0700) < 0)
        return -1;
    if (write_busybox(fixture, 183) < 0)
        return -1;

    if (hardlink_groups >= 1) {
        static const char alpha[] = "alpha-content\n";
        if (format_path(path, "%s/data/usr/lib/alpha-alias",
                fixture->seed) < 0 ||
                write_file(path, alpha, sizeof(alpha) - 1, 0600) < 0 ||
                format_path(path, "%s/data/usr/lib/alpha-middle",
                        fixture->seed) < 0 ||
                write_file(path, alpha, sizeof(alpha) - 1, 0600) < 0 ||
                format_path(path, "%s/data/usr/lib/alpha-source",
                        fixture->seed) < 0 ||
                write_file(path, alpha, sizeof(alpha) - 1, 0600) < 0)
            return -1;
    }
    if (hardlink_groups >= 2) {
        static const char beta[] = "beta-content\n";
        if (format_path(path, "%s/data/usr/lib/beta-alias",
                fixture->seed) < 0 ||
                write_file(path, beta, sizeof(beta) - 1, 0600) < 0 ||
                format_path(path, "%s/data/usr/lib/beta-source",
                        fixture->seed) < 0 ||
                write_file(path, beta, sizeof(beta) - 1, 0600) < 0)
            return -1;
    }
    if (write_manifest(fixture, "official", digest_a) < 0 ||
            write_hardlink_manifest(fixture) < 0 ||
            create_database(fixture) < 0)
        return -1;
    return 0;
}

static int verify_hardlink_group(
        const char *root, const char *const *names, size_t count) {
    struct stat canonical = {0};
    for (size_t i = 0; i < count; i++) {
        char path[PATH_MAX];
        struct stat metadata;
        if (format_path(path, "%s/data/usr/lib/%s", root, names[i]) < 0 ||
                stat(path, &metadata) < 0 ||
                !S_ISREG(metadata.st_mode) ||
                metadata.st_nlink != (nlink_t) count)
            return 0;
        if (i == 0)
            canonical = metadata;
        else if (canonical.st_dev != metadata.st_dev ||
                canonical.st_ino != metadata.st_ino)
            return 0;
    }
    return 1;
}

static int verify_database_inode(const char *root) {
    char path[PATH_MAX];
    if (format_path(path, "%s/meta.db", root) < 0)
        return 0;
    struct stat metadata;
    if (stat(path, &metadata) < 0)
        return 0;
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL) !=
            SQLITE_OK) {
        sqlite3_close(database);
        return 0;
    }
    sqlite3_stmt *statement = NULL;
    int valid = sqlite3_prepare_v2(database,
            "select db_inode from meta", -1, &statement, NULL) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW &&
            sqlite3_column_int64(statement, 0) >= 0 &&
            (uintmax_t) sqlite3_column_int64(statement, 0) ==
                    (uintmax_t) metadata.st_ino &&
            sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return valid;
}

static int verify_receipt(const char *root, const char *digest) {
    char path[PATH_MAX];
    if (format_path(path, "%s/rootfs-installation.txt", root) < 0)
        return 0;
    char receipt[256];
    int length = snprintf(receipt, sizeof(receipt),
            "format=ish-rootfs-install-v1\nseed_archive_sha256=%s\n",
            digest);
    return length >= 0 && (size_t) length < sizeof(receipt) &&
            file_equals(path, receipt, (size_t) length);
}

static int verify_private_staging_entries(
        const struct fixture *fixture, const char *allowed_name) {
    static const char prefix[] = ".root.installing.";
    DIR *directory = opendir(fixture->persistent);
    if (directory == NULL)
        return 0;
    bool found_allowed = allowed_name == NULL;
    int valid = 1;
    int iteration_error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            iteration_error = errno;
            break;
        }
        if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1) != 0)
            continue;
        if (allowed_name != NULL && !found_allowed &&
                strcmp(entry->d_name, allowed_name) == 0) {
            found_allowed = true;
            continue;
        }
        valid = 0;
        break;
    }
    if (closedir(directory) < 0)
        valid = 0;
    return valid && iteration_error == 0 && found_allowed;
}

static int verify_no_private_staging(const struct fixture *fixture) {
    return verify_private_staging_entries(fixture, NULL);
}

static int format_staging_name(
        char output[NAME_MAX + 1], const char *token) {
    int length = snprintf(output, NAME_MAX + 1,
            ".root.installing.%s", token);
    return length < 0 || length > NAME_MAX ? -1 : 0;
}

static int format_staging_path(
        const struct fixture *fixture, const char *staging_name,
        char output[PATH_MAX]) {
    return format_path(output, "%s/%s", fixture->persistent, staging_name);
}

static int create_owner_marker(
        const struct fixture *fixture, const char *staging_name,
        uintmax_t device, uintmax_t inode) {
    char owner_path[PATH_MAX];
    if (format_path(owner_path, "%s/.root.installing.owner",
            fixture->persistent) < 0)
        return -1;
    char target[512];
    int length = snprintf(target, sizeof(target),
            "%s\nstaging=%s\ndevice=%" PRIxMAX "\ninode=%" PRIxMAX "\n",
            owner_magic, staging_name, device, inode);
    if (length < 0 || (size_t) length >= sizeof(target))
        return -1;
    return symlink(target, owner_path);
}

static int create_owner_for_staging(
        const struct fixture *fixture, const char *staging_name,
        bool mismatch_inode) {
    char staging_path[PATH_MAX];
    struct stat metadata;
    if (format_staging_path(fixture, staging_name, staging_path) < 0 ||
            lstat(staging_path, &metadata) < 0)
        return -1;
    uintmax_t inode = (uintmax_t) metadata.st_ino;
    if (mismatch_inode)
        inode = inode == UINTMAX_MAX ? inode - 1 : inode + 1;
    return create_owner_marker(fixture, staging_name,
            (uintmax_t) metadata.st_dev, inode);
}

static int create_owned_staging(
        const struct fixture *fixture, const char *token,
        char staging_name[NAME_MAX + 1], char staging_path[PATH_MAX]) {
    if (format_staging_name(staging_name, token) < 0 ||
            format_staging_path(fixture, staging_name, staging_path) < 0 ||
            mkdir(staging_path, 0700) < 0)
        return -1;
    return create_owner_for_staging(fixture, staging_name, false);
}

static int verify_installed_root_with_private(
        const struct fixture *fixture, const char *allowed_private) {
    char root[PATH_MAX];
    CHECK(format_path(root, "%s/root", fixture->persistent) == 0,
            "构造已安装 root 路径");
    struct stat metadata;
    CHECK(stat(root, &metadata) == 0 && S_ISDIR(metadata.st_mode),
            "发布持久 root 目录");
    CHECK(verify_receipt(root, digest_a), "写入精确安装 receipt");
    CHECK(verify_database_inode(root), "meta.db 记录复制后的真实 inode");
    if (fixture->hardlink_groups >= 1) {
        static const char *alpha_names[] = {
            "alpha-alias", "alpha-middle", "alpha-source",
        };
        CHECK(verify_hardlink_group(root, alpha_names,
                sizeof(alpha_names) / sizeof(alpha_names[0])),
                "恢复第一组 hardlink");
    }
    if (fixture->hardlink_groups >= 2) {
        static const char *beta_names[] = {
            "beta-alias", "beta-source",
        };
        CHECK(verify_hardlink_group(root, beta_names,
                sizeof(beta_names) / sizeof(beta_names[0])),
                "恢复第二组 hardlink");
        char alpha[PATH_MAX];
        char beta[PATH_MAX];
        CHECK(format_path(alpha, "%s/data/usr/lib/alpha-alias", root) == 0 &&
                format_path(beta, "%s/data/usr/lib/beta-alias", root) == 0,
                "构造跨组 hardlink 路径");
        struct stat alpha_metadata;
        struct stat beta_metadata;
        CHECK(stat(alpha, &alpha_metadata) == 0 &&
                stat(beta, &beta_metadata) == 0 &&
                (alpha_metadata.st_dev != beta_metadata.st_dev ||
                        alpha_metadata.st_ino != beta_metadata.st_ino),
                "不同 hardlink 组不能被误合并");
    }
    char artifact[PATH_MAX];
    static const char *suffixes[] = {
        "meta.db-wal", "meta.db-shm", "meta.db-journal",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        CHECK(format_path(artifact, "%s/%s", root, suffixes[i]) == 0 &&
                path_absent(artifact), "首次安装不留下 SQLite 临时文件");
    }
    CHECK(verify_private_staging_entries(fixture, allowed_private),
            allowed_private == NULL ?
                    "首次安装清理 staging 和 owner" :
                    "首次安装只保留无 owner 的既有 orphan");
    return 0;
}

static int verify_installed_root(const struct fixture *fixture) {
    return verify_installed_root_with_private(fixture, NULL);
}

static int test_install_and_reuse(const char *workspace) {
    struct fixture fixture;
    CHECK(create_fixture(workspace, "install-and-reuse", 2, &fixture) == 0,
            "创建两组 hardlink 的 official seed");
    char alpha_alias[PATH_MAX];
    char alpha_middle[PATH_MAX];
    char alpha_source[PATH_MAX];
    CHECK(format_path(alpha_alias, "%s/data/usr/lib/alpha-alias",
            fixture.seed) == 0 &&
            format_path(alpha_middle, "%s/data/usr/lib/alpha-middle",
                    fixture.seed) == 0 &&
            format_path(alpha_source, "%s/data/usr/lib/alpha-source",
                    fixture.seed) == 0, "构造 seed hardlink 路径");
    struct stat alias_metadata;
    struct stat middle_metadata;
    struct stat source_metadata;
    CHECK(stat(alpha_alias, &alias_metadata) == 0 &&
            stat(alpha_middle, &middle_metadata) == 0 &&
            stat(alpha_source, &source_metadata) == 0 &&
            alias_metadata.st_ino != middle_metadata.st_ino &&
            alias_metadata.st_ino != source_metadata.st_ino &&
            middle_metadata.st_ino != source_metadata.st_ino,
            "bundle 夹具中的链接应已被打散");

    enum ish_apple_rootfs_seed_result result;
    CHECK(ish_apple_rootfs_seed_install(fixture.seed, fixture.persistent,
            "root", &result) == 0 &&
            result == ISH_APPLE_ROOTFS_SEED_INSTALLED,
            "首次调用安装持久 root");
    CHECK(verify_installed_root(&fixture) == 0, "验证首次安装结果");

    char root[PATH_MAX];
    char path[PATH_MAX];
    CHECK(format_path(root, "%s/root", fixture.persistent) == 0 &&
            format_path(alpha_alias, "%s/data/usr/lib/alpha-alias", root) == 0 &&
            format_path(alpha_source,
                    "%s/data/usr/lib/alpha-source", root) == 0,
            "构造用户 root 修改路径");
    CHECK(unlink(alpha_source) == 0, "打断已安装 root 的初始 hardlink");
    static const char user_change[] = "user-change\n";
    CHECK(write_file(alpha_source, user_change,
            sizeof(user_change) - 1, 0600) == 0,
            "写入用户修改后的 hardlink 成员");
    CHECK(format_path(path, "%s/rootfs-manifest.txt", root) == 0 &&
            unlink(path) == 0 &&
            format_path(path, "%s/rootfs-hardlinks.tsv", root) == 0 &&
            unlink(path) == 0, "用户可删除初始 seed 清单");
    static const char user_file[] = "persistent-user-data\n";
    CHECK(format_path(path, "%s/data/user-file", root) == 0 &&
            write_file(path, user_file, sizeof(user_file) - 1, 0600) == 0,
            "创建用户持久文件");
    CHECK(format_path(path, "%s/data/guest-fifo", root) == 0 &&
            mkfifo(path, 0600) == 0, "创建 guest 后续产生的 FIFO");
    static const char sqlite_runtime[] = "runtime-state\n";
    CHECK(format_path(path, "%s/meta.db-wal", root) == 0 &&
            write_file(path, sqlite_runtime,
                    sizeof(sqlite_runtime) - 1, 0600) == 0 &&
            format_path(path, "%s/meta.db-shm", root) == 0 &&
            write_file(path, sqlite_runtime,
                    sizeof(sqlite_runtime) - 1, 0600) == 0,
            "模拟运行中 SQLite WAL/SHM");
    CHECK(write_manifest(&fixture, "official", digest_b) == 0,
            "模拟 App 更新携带新 seed 版本");

    CHECK(ish_apple_rootfs_seed_install(fixture.seed, fixture.persistent,
            "root", &result) == 0 &&
            result == ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT,
            "二次调用复用既有用户 root");
    CHECK(stat(alpha_alias, &alias_metadata) == 0 &&
            stat(alpha_source, &source_metadata) == 0 &&
            alias_metadata.st_ino != source_metadata.st_ino,
            "二次调用不得重放初始 hardlink");
    CHECK(file_equals(alpha_source, user_change, sizeof(user_change) - 1),
            "二次调用保留用户文件内容");
    CHECK(format_path(path, "%s/data/user-file", root) == 0 &&
            file_equals(path, user_file, sizeof(user_file) - 1),
            "二次调用保留新增用户文件");
    CHECK(format_path(path, "%s/data/guest-fifo", root) == 0 &&
            lstat(path, &source_metadata) == 0 &&
            S_ISFIFO(source_metadata.st_mode),
            "二次调用接受用户 root 中的 FIFO");
    CHECK(format_path(path, "%s/meta.db-wal", root) == 0 &&
            file_equals(path, sqlite_runtime, sizeof(sqlite_runtime) - 1) &&
            format_path(path, "%s/meta.db-shm", root) == 0 &&
            file_equals(path, sqlite_runtime, sizeof(sqlite_runtime) - 1),
            "二次调用保留运行中的 WAL/SHM");
    CHECK(format_path(path, "%s/rootfs-manifest.txt", root) == 0 &&
            path_absent(path) &&
            format_path(path, "%s/rootfs-hardlinks.tsv", root) == 0 &&
            path_absent(path), "二次调用不要求初始清单仍存在");
    CHECK(verify_receipt(root, digest_a),
            "seed 更新不能替换既有安装 receipt");
    CHECK(verify_no_private_staging(&fixture), "复用路径不留下 staging");
    return 0;
}

static int test_empty_manifest(const char *workspace) {
    struct fixture fixture;
    CHECK(create_fixture(workspace, "empty-hardlinks", 0, &fixture) == 0,
            "创建空 hardlink 清单夹具");
    enum ish_apple_rootfs_seed_result result;
    CHECK(ish_apple_rootfs_seed_install(fixture.seed, fixture.persistent,
            "root", &result) == 0 &&
            result == ISH_APPLE_ROOTFS_SEED_INSTALLED,
            "空 hardlink 清单可以安装");
    CHECK(verify_installed_root(&fixture) == 0,
            "验证空 hardlink 清单安装结果");
    return 0;
}

static int mutate_extra_top(const struct fixture *fixture) {
    char path[PATH_MAX];
    static const char extra[] = "unexpected\n";
    return format_path(path, "%s/unexpected", fixture->seed) < 0 ? -1 :
            write_file(path, extra, sizeof(extra) - 1, 0600);
}

static int mutate_nonofficial(const struct fixture *fixture) {
    return write_manifest(fixture, "test-fixture", digest_a);
}

static int mutate_wrong_elf(const struct fixture *fixture) {
    return write_busybox(fixture, 3);
}

static int mutate_seed_symlink(const struct fixture *fixture) {
    char path[PATH_MAX];
    return format_path(path, "%s/data/symlink-node", fixture->seed) < 0 ? -1 :
            symlink("../../outside", path);
}

static int mutate_seed_fifo(const struct fixture *fixture) {
    char path[PATH_MAX];
    return format_path(path, "%s/data/fifo-node", fixture->seed) < 0 ? -1 :
            mkfifo(path, 0600);
}

static int overwrite_hardlinks_bytes(
        const struct fixture *fixture, const void *contents, size_t length) {
    char path[PATH_MAX];
    return format_path(path, "%s/rootfs-hardlinks.tsv", fixture->seed) < 0 ?
            -1 : write_file(path, contents, length, 0600);
}

static int overwrite_hardlinks(
        const struct fixture *fixture, const char *contents) {
    return overwrite_hardlinks_bytes(fixture, contents, strlen(contents));
}

static int mutate_path_escape(const struct fixture *fixture) {
    return overwrite_hardlinks(fixture,
            "../escape\t../escape\n../escape\tusr/lib/alpha-source\n");
}

static int mutate_duplicate_member(const struct fixture *fixture) {
    return overwrite_hardlinks(fixture,
            "usr/lib/alpha-alias\tusr/lib/alpha-alias\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n");
}

static int mutate_missing_hardlink_group(const struct fixture *fixture) {
    return overwrite_hardlinks(fixture,
            "usr/lib/alpha-alias\tusr/lib/alpha-alias\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-middle\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n");
}

static int mutate_missing_hardlink_member(const struct fixture *fixture) {
    return overwrite_hardlinks(fixture,
            "usr/lib/alpha-alias\tusr/lib/alpha-alias\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n"
            "usr/lib/beta-alias\tusr/lib/beta-alias\n"
            "usr/lib/beta-alias\tusr/lib/beta-source\n");
}

static int mutate_first_member_not_self(const struct fixture *fixture) {
    return overwrite_hardlinks(fixture,
            "usr/lib/alpha-alias\tusr/lib/alpha-middle\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n");
}

static int mutate_embedded_nul(const struct fixture *fixture) {
    static const char contents[] =
            "usr/lib/alpha-alias\tusr/lib/alpha-alias\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-\0middle\n"
            "usr/lib/alpha-alias\tusr/lib/alpha-source\n";
    return overwrite_hardlinks_bytes(
            fixture, contents, sizeof(contents) - 1);
}

static int mutate_too_deep_tree(const struct fixture *fixture) {
    struct rlimit files;
    const rlim_t required_files = (rlim_t) 2048;
    if (getrlimit(RLIMIT_NOFILE, &files) < 0)
        return -1;
    if (files.rlim_cur < required_files) {
        if (files.rlim_max != RLIM_INFINITY &&
                files.rlim_max < required_files) {
            errno = ENOTSUP;
            return -1;
        }
        files.rlim_cur = required_files;
        if (setrlimit(RLIMIT_NOFILE, &files) < 0)
            return -1;
    }
    char path[PATH_MAX];
    if (format_path(path, "%s/data", fixture->seed) < 0)
        return -1;
    size_t length = strlen(path);
    for (unsigned depth = 0; depth <= 256; depth++) {
        if (length + 2 >= sizeof(path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        path[length++] = '/';
        path[length++] = 'd';
        path[length] = '\0';
        if (mkdir(path, 0700) < 0)
            return -1;
    }
    return 0;
}

static int mutate_content_mismatch(const struct fixture *fixture) {
    char path[PATH_MAX];
    static const char mismatch[] = "different-content\n";
    return format_path(path, "%s/data/usr/lib/alpha-source",
            fixture->seed) < 0 ? -1 :
            write_file(path, mismatch, sizeof(mismatch) - 1, 0600);
}

static int open_fixture_database(
        const struct fixture *fixture, sqlite3 **database) {
    char path[PATH_MAX];
    if (format_path(path, "%s/meta.db", fixture->seed) < 0)
        return -1;
    return sqlite3_open_v2(path, database, SQLITE_OPEN_READWRITE, NULL) ==
            SQLITE_OK ? 0 : -1;
}

static int mutate_schema_version(const struct fixture *fixture) {
    sqlite3 *database = NULL;
    if (open_fixture_database(fixture, &database) < 0) {
        sqlite3_close(database);
        return -1;
    }
    int result = sqlite_exec_checked(database, "pragma user_version=2");
    if (sqlite3_close(database) != SQLITE_OK)
        result = -1;
    return result;
}

static int mutate_meta_update_trigger(const struct fixture *fixture) {
    sqlite3 *database = NULL;
    if (open_fixture_database(fixture, &database) < 0) {
        sqlite3_close(database);
        return -1;
    }
    int result = sqlite_exec_checked(database,
            "create trigger reset_db_inode after update of db_inode on meta "
            "begin update meta set db_inode = 0 where rowid = new.rowid; end");
    if (sqlite3_close(database) != SQLITE_OK)
        result = -1;
    return result;
}

static int mutate_cross_group_database(const struct fixture *fixture) {
    sqlite3 *database = NULL;
    if (open_fixture_database(fixture, &database) < 0) {
        sqlite3_close(database);
        return -1;
    }
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(database,
            "update paths set inode = ? where path = ?", -1,
            &statement, NULL);
    static const char beta_source[] = "/usr/lib/beta-source";
    if (result == SQLITE_OK)
        result = sqlite3_bind_int64(statement, 1, 3);
    if (result == SQLITE_OK)
        result = sqlite3_bind_blob(statement, 2, beta_source,
                (int) (sizeof(beta_source) - 1), SQLITE_STATIC);
    if (result == SQLITE_OK)
        result = sqlite3_step(statement);
    int finalize_result = sqlite3_finalize(statement);
    int close_result = sqlite3_close(database);
    return result == SQLITE_DONE && finalize_result == SQLITE_OK &&
            close_result == SQLITE_OK ? 0 : -1;
}

static int mutate_corrupt_database(const struct fixture *fixture) {
    char path[PATH_MAX];
    static const char corrupt[] = "not-a-sqlite-database\n";
    return format_path(path, "%s/meta.db", fixture->seed) < 0 ? -1 :
            write_file(path, corrupt, sizeof(corrupt) - 1, 0600);
}

struct rejection_case {
    const char *name;
    int (*mutate)(const struct fixture *fixture);
};

static int test_rejections(const char *workspace) {
    static const struct rejection_case cases[] = {
        {"额外顶层资源", mutate_extra_top},
        {"非官方来源", mutate_nonofficial},
        {"错误 ELF 架构", mutate_wrong_elf},
        {"seed 符号链接", mutate_seed_symlink},
        {"seed FIFO", mutate_seed_fifo},
        {"hardlink 路径逃逸", mutate_path_escape},
        {"hardlink 重复成员", mutate_duplicate_member},
        {"hardlink TSV 漏掉整组", mutate_missing_hardlink_group},
        {"hardlink TSV 漏掉组内成员", mutate_missing_hardlink_member},
        {"hardlink TSV 首行不是 self", mutate_first_member_not_self},
        {"hardlink TSV 含内嵌 NUL", mutate_embedded_nul},
        {"hardlink 内容不一致", mutate_content_mismatch},
        {"seed 目录树超过复制深度", mutate_too_deep_tree},
        {"SQLite schema 版本错误", mutate_schema_version},
        {"SQLite meta UPDATE trigger", mutate_meta_update_trigger},
        {"SQLite 跨组 inode", mutate_cross_group_database},
        {"损坏 SQLite", mutate_corrupt_database},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char fixture_name[64];
        int length = snprintf(fixture_name, sizeof(fixture_name),
                "reject-%zu", i);
        CHECK(length >= 0 && (size_t) length < sizeof(fixture_name),
                "构造拒绝用例名称");
        struct fixture fixture;
        CHECK(create_fixture(workspace, fixture_name, 2, &fixture) == 0,
                "创建拒绝用例 official seed");
        CHECK(cases[i].mutate(&fixture) == 0, cases[i].name);
        enum ish_apple_rootfs_seed_result result =
                ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
        int error = ish_apple_rootfs_seed_install(
                fixture.seed, fixture.persistent, "root", &result);
        if (error == 0) {
            fprintf(stderr, "拒绝用例被错误接受：%s\n", cases[i].name);
            return 1;
        }
        if (cases[i].mutate == mutate_too_deep_tree && error != ELOOP) {
            fprintf(stderr, "深度拒绝返回 %d，而不是 ELOOP\n", error);
            return 1;
        }
        char root[PATH_MAX];
        CHECK(format_path(root, "%s/root", fixture.persistent) == 0 &&
                path_absent(root), "验证失败不能发布 final root");
        CHECK(verify_no_private_staging(&fixture),
                "验证失败清理 owned staging 和 owner");
    }
    return 0;
}

static int test_staging_recovery(const char *workspace) {
    struct fixture owned;
    CHECK(create_fixture(workspace, "owned-staging", 1, &owned) == 0,
            "创建 owned staging 恢复夹具");
    char staging_name[NAME_MAX + 1];
    char staging[PATH_MAX];
    char sentinel[PATH_MAX];
    CHECK(create_owned_staging(&owned,
            "00000000000000000000000000000001",
            staging_name, staging) == 0 &&
            format_path(sentinel, "%s/stale", staging) == 0 &&
            write_file(sentinel, "stale\n", 6, 0600) == 0,
            "建立 v2 owner 对应的残留 staging");
    enum ish_apple_rootfs_seed_result result;
    CHECK(ish_apple_rootfs_seed_install(owned.seed, owned.persistent,
            "root", &result) == 0 &&
            result == ISH_APPLE_ROOTFS_SEED_INSTALLED,
            "清理 owned staging 后继续安装");
    CHECK(verify_installed_root(&owned) == 0,
            "验证 owned staging 恢复后的 root");

    struct fixture owner_only;
    CHECK(create_fixture(workspace, "owner-only", 1, &owner_only) == 0 &&
            format_staging_name(staging_name,
                    "00000000000000000000000000000002") == 0 &&
            create_owner_marker(&owner_only, staging_name, 1, 1) == 0,
            "创建 staging 已消失的有效 owner");
    CHECK(ish_apple_rootfs_seed_install(owner_only.seed,
            owner_only.persistent, "root", &result) == 0 &&
            result == ISH_APPLE_ROOTFS_SEED_INSTALLED,
            "owner-only 恢复后继续安装");
    CHECK(verify_installed_root(&owner_only) == 0,
            "owner-only 恢复最终收敛");

    struct fixture mismatch;
    CHECK(create_fixture(workspace, "owner-inode-mismatch", 1,
            &mismatch) == 0 &&
            format_staging_name(staging_name,
                    "00000000000000000000000000000003") == 0 &&
            format_staging_path(&mismatch, staging_name, staging) == 0 &&
            mkdir(staging, 0700) == 0 &&
            format_path(sentinel, "%s/keep", staging) == 0 &&
            write_file(sentinel, "keep\n", 5, 0600) == 0 &&
            create_owner_for_staging(&mismatch,
                    staging_name, true) == 0,
            "创建 inode 错配的 owner 与 staging");
    char owner[PATH_MAX];
    CHECK(format_path(owner, "%s/.root.installing.owner",
            mismatch.persistent) == 0, "构造 inode 错配 owner 路径");
    struct stat staging_before;
    struct stat owner_before;
    CHECK(lstat(staging, &staging_before) == 0 &&
            lstat(owner, &owner_before) == 0,
            "记录 inode 错配条目身份");
    CHECK(ish_apple_rootfs_seed_install(mismatch.seed,
            mismatch.persistent, "root", &result) == EEXIST,
            "inode 错配 owner 必须拒绝恢复");
    CHECK(same_entry(&staging_before, staging) &&
            same_entry(&owner_before, owner) &&
            file_equals(sentinel, "keep\n", 5),
            "inode 错配条目必须原样保留");
    char root[PATH_MAX];
    CHECK(format_path(root, "%s/root", mismatch.persistent) == 0 &&
            path_absent(root), "inode 错配不能发布 final root");

    struct fixture regular_owner;
    CHECK(create_fixture(workspace, "regular-owner", 1,
            &regular_owner) == 0 &&
            format_path(owner, "%s/.root.installing.owner",
                    regular_owner.persistent) == 0 &&
            write_file(owner, "foreign-owner\n", 14, 0600) == 0 &&
            lstat(owner, &owner_before) == 0,
            "创建未知 regular owner");
    CHECK(ish_apple_rootfs_seed_install(regular_owner.seed,
            regular_owner.persistent, "root", &result) == EEXIST,
            "regular owner 必须拒绝恢复");
    CHECK(same_entry(&owner_before, owner) &&
            file_equals(owner, "foreign-owner\n", 14),
            "regular owner 必须原样保留");
    CHECK(format_path(root, "%s/root", regular_owner.persistent) == 0 &&
            path_absent(root), "regular owner 不能发布 final root");

    struct fixture invalid_owner;
    static const char invalid_owner_target[] = "format=foreign-owner\n";
    CHECK(create_fixture(workspace, "invalid-symlink-owner", 1,
            &invalid_owner) == 0 &&
            format_path(owner, "%s/.root.installing.owner",
                    invalid_owner.persistent) == 0 &&
            symlink(invalid_owner_target, owner) == 0 &&
            lstat(owner, &owner_before) == 0,
            "创建格式未知的 symlink owner");
    CHECK(ish_apple_rootfs_seed_install(invalid_owner.seed,
            invalid_owner.persistent, "root", &result) == EEXIST,
            "格式未知的 symlink owner 必须拒绝恢复");
    CHECK(same_entry(&owner_before, owner) &&
            symlink_equals(owner, invalid_owner_target),
            "格式未知的 symlink owner 必须原样保留");
    CHECK(format_path(root, "%s/root", invalid_owner.persistent) == 0 &&
            path_absent(root), "格式未知的 owner 不能发布 final root");

    struct fixture staging_symlink;
    CHECK(create_fixture(workspace, "staging-symlink", 1,
            &staging_symlink) == 0,
            "创建 staging symlink 拒绝夹具");
    char outside[PATH_MAX];
    CHECK(format_path(outside, "%s/outside", staging_symlink.base) == 0 &&
            mkdir(outside, 0700) == 0 &&
            format_path(sentinel, "%s/sentinel", outside) == 0 &&
            write_file(sentinel, "keep\n", 5, 0600) == 0 &&
            format_staging_name(staging_name,
                    "00000000000000000000000000000004") == 0 &&
            format_staging_path(&staging_symlink,
                    staging_name, staging) == 0 &&
            symlink(outside, staging) == 0 &&
            create_owner_for_staging(&staging_symlink,
                    staging_name, false) == 0 &&
            format_path(owner, "%s/.root.installing.owner",
                    staging_symlink.persistent) == 0 &&
            lstat(staging, &staging_before) == 0 &&
            lstat(owner, &owner_before) == 0,
            "建立 owner 指向的 staging 符号链接");
    CHECK(ish_apple_rootfs_seed_install(staging_symlink.seed,
            staging_symlink.persistent,
            "root", &result) == EEXIST,
            "staging 符号链接必须返回 EEXIST");
    CHECK(same_entry(&staging_before, staging) &&
            same_entry(&owner_before, owner) &&
            symlink_equals(staging, outside) &&
            file_equals(sentinel, "keep\n", 5),
            "staging 符号链接与外部目标必须原样保留");
    CHECK(format_path(root, "%s/root", staging_symlink.persistent) == 0 &&
            path_absent(root), "staging 符号链接不能发布 final root");

    struct fixture orphan;
    CHECK(create_fixture(workspace, "unowned-orphan", 1, &orphan) == 0 &&
            format_staging_name(staging_name,
                    "00000000000000000000000000000005") == 0 &&
            format_staging_path(&orphan, staging_name, staging) == 0 &&
            mkdir(staging, 0700) == 0 &&
            format_path(sentinel, "%s/keep", staging) == 0 &&
            write_file(sentinel, "orphan\n", 7, 0600) == 0 &&
            lstat(staging, &staging_before) == 0,
            "创建没有 owner 的随机 staging orphan");
    CHECK(ish_apple_rootfs_seed_install(orphan.seed,
            orphan.persistent, "root", &result) == 0 &&
            result == ISH_APPLE_ROOTFS_SEED_INSTALLED,
            "无 owner 的随机 orphan 不阻塞新安装");
    CHECK(same_entry(&staging_before, staging) &&
            file_equals(sentinel, "orphan\n", 7),
            "安装器不得认领无 owner 的随机 orphan");
    CHECK(verify_installed_root_with_private(&orphan, staging_name) == 0,
            "安装成功且只保留原有随机 orphan");
    return 0;
}

static int test_transaction_recovery(const char *workspace) {
    for (int phase = ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC;
            phase <= ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC; phase++) {
        char fixture_name[64];
        char token[33];
        int name_length = snprintf(fixture_name, sizeof(fixture_name),
                "transaction-phase-%d", phase);
        int token_length = snprintf(token, sizeof(token), "%032x", phase);
        CHECK(name_length >= 0 &&
                (size_t) name_length < sizeof(fixture_name) &&
                token_length == 32, "构造事务故障夹具名称");

        struct fixture fixture;
        CHECK(create_fixture(workspace, fixture_name, 2, &fixture) == 0,
                "创建事务故障夹具");
        char staging_name[NAME_MAX + 1] = {0};
        char staging[PATH_MAX] = {0};
        if (phase <= ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC) {
            CHECK(create_owned_staging(&fixture, token,
                    staging_name, staging) == 0,
                    "创建待清理的有效 staging");
        }
        char owner[PATH_MAX];
        char root[PATH_MAX];
        CHECK(format_path(owner, "%s/.root.installing.owner",
                fixture.persistent) == 0 &&
                format_path(root, "%s/root", fixture.persistent) == 0,
                "构造事务故障路径");

        ish_apple_rootfs_seed_test_fail_phase = phase;
        enum ish_apple_rootfs_seed_result result =
                ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
        CHECK(ish_apple_rootfs_seed_install(fixture.seed,
                fixture.persistent, "root", &result) == EIO,
                "注入的目录 fsync 故障必须向调用方返回");
        CHECK(ish_apple_rootfs_seed_test_fail_phase ==
                ROOTFS_SEED_TEST_NONE,
                "单次故障注入必须在命中后复位");

        if (phase == ROOTFS_SEED_TEST_CLEANUP_STAGING_SYNC) {
            CHECK(path_absent(staging) && !path_absent(owner) &&
                    path_absent(root),
                    "清理第一阶段故障保留 owner 提供恢复凭据");
        } else if (phase == ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC) {
            CHECK(path_absent(staging) && path_absent(owner) &&
                    path_absent(root),
                    "清理第二阶段故障留下可重试的空状态");
        } else {
            struct stat root_metadata;
            CHECK(lstat(root, &root_metadata) == 0 &&
                    S_ISDIR(root_metadata.st_mode),
                    "发布阶段故障发生时 final 已成为提交点");
            CHECK((phase == ROOTFS_SEED_TEST_PUBLISH_ROOT_SYNC &&
                    !path_absent(owner)) ||
                    (phase == ROOTFS_SEED_TEST_PUBLISH_OWNER_SYNC &&
                    path_absent(owner)),
                    "发布两阶段故障留下预期 owner 状态");
        }

        result = ISH_APPLE_ROOTFS_SEED_INSTALLED;
        CHECK(ish_apple_rootfs_seed_install(fixture.seed,
                fixture.persistent, "root", &result) == 0,
                "第二次调用必须从持久中间状态收敛");
        CHECK(result == (phase <= ROOTFS_SEED_TEST_CLEANUP_OWNER_SYNC ?
                ISH_APPLE_ROOTFS_SEED_INSTALLED :
                ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT),
                "收敛结果必须反映 final 是否已发布");
        CHECK(verify_installed_root(&fixture) == 0,
                "事务故障恢复后 root 与私有状态完整收敛");
    }
    return 0;
}

static int create_existing_root(
        const struct fixture *fixture, bool create_meta, bool create_data) {
    char root[PATH_MAX];
    char path[PATH_MAX];
    char receipt[256];
    int receipt_length = snprintf(receipt, sizeof(receipt),
            "format=ish-rootfs-install-v1\nseed_archive_sha256=%s\n",
            digest_a);
    if (receipt_length < 0 ||
            (size_t) receipt_length >= sizeof(receipt) ||
            format_path(root, "%s/root", fixture->persistent) < 0 ||
            mkdir(root, 0700) < 0 ||
            format_path(path, "%s/rootfs-installation.txt", root) < 0 ||
            write_file(path, receipt, (size_t) receipt_length, 0600) < 0)
        return -1;
    if (create_meta && (format_path(path, "%s/meta.db", root) < 0 ||
            write_file(path, "meta\n", 5, 0600) < 0))
        return -1;
    if (create_data && (format_path(path, "%s/data", root) < 0 ||
            mkdir(path, 0700) < 0))
        return -1;
    return 0;
}

static int test_existing_root_rejections(const char *workspace) {
    enum ish_apple_rootfs_seed_result result =
            ISH_APPLE_ROOTFS_SEED_INSTALLED;
    char root[PATH_MAX];
    char path[PATH_MAX];
    char outside[PATH_MAX];
    char sentinel[PATH_MAX];
    struct stat before;

    struct fixture bad_receipt;
    static const char broken_receipt[] = "broken-receipt\n";
    CHECK(create_fixture(workspace, "bad-final-receipt", 1,
            &bad_receipt) == 0 &&
            create_existing_root(&bad_receipt, true, true) == 0 &&
            format_path(root, "%s/root", bad_receipt.persistent) == 0 &&
            format_path(path, "%s/rootfs-installation.txt", root) == 0 &&
            write_file(path, broken_receipt,
                    sizeof(broken_receipt) - 1, 0600) == 0 &&
            lstat(path, &before) == 0,
            "创建 receipt 损坏的既有 final");
    CHECK(ish_apple_rootfs_seed_install(bad_receipt.seed,
            bad_receipt.persistent, "root", &result) == EEXIST,
            "损坏 receipt 的 final 必须拒绝复用");
    CHECK(same_entry(&before, path) && file_equals(path,
            broken_receipt, sizeof(broken_receipt) - 1) &&
            verify_no_private_staging(&bad_receipt),
            "损坏 receipt 与 final 必须原样保留");

    struct fixture final_symlink;
    CHECK(create_fixture(workspace, "final-symlink", 1,
            &final_symlink) == 0 &&
            format_path(outside, "%s/outside", final_symlink.base) == 0 &&
            mkdir(outside, 0700) == 0 &&
            format_path(sentinel, "%s/sentinel", outside) == 0 &&
            write_file(sentinel, "keep\n", 5, 0600) == 0 &&
            format_path(root, "%s/root", final_symlink.persistent) == 0 &&
            symlink(outside, root) == 0 && lstat(root, &before) == 0,
            "创建指向外部目录的 final symlink");
    CHECK(ish_apple_rootfs_seed_install(final_symlink.seed,
            final_symlink.persistent, "root", &result) == EEXIST,
            "final symlink 必须拒绝复用");
    CHECK(same_entry(&before, root) && symlink_equals(root, outside) &&
            file_equals(sentinel, "keep\n", 5) &&
            verify_no_private_staging(&final_symlink),
            "final symlink 与外部目标必须原样保留");

    struct fixture meta_symlink;
    CHECK(create_fixture(workspace, "meta-symlink", 1,
            &meta_symlink) == 0 &&
            create_existing_root(&meta_symlink, false, true) == 0 &&
            format_path(outside, "%s/outside-meta", meta_symlink.base) == 0 &&
            write_file(outside, "outside-meta\n", 13, 0600) == 0 &&
            format_path(root, "%s/root", meta_symlink.persistent) == 0 &&
            format_path(path, "%s/meta.db", root) == 0 &&
            symlink(outside, path) == 0 && lstat(path, &before) == 0,
            "创建指向外部文件的 meta.db symlink");
    CHECK(ish_apple_rootfs_seed_install(meta_symlink.seed,
            meta_symlink.persistent, "root", &result) == EEXIST,
            "meta.db symlink 必须拒绝复用");
    CHECK(same_entry(&before, path) && symlink_equals(path, outside) &&
            file_equals(outside, "outside-meta\n", 13) &&
            verify_no_private_staging(&meta_symlink),
            "meta.db symlink 与外部文件必须原样保留");

    struct fixture data_symlink;
    CHECK(create_fixture(workspace, "data-symlink", 1,
            &data_symlink) == 0 &&
            create_existing_root(&data_symlink, true, false) == 0 &&
            format_path(outside, "%s/outside-data", data_symlink.base) == 0 &&
            mkdir(outside, 0700) == 0 &&
            format_path(sentinel, "%s/sentinel", outside) == 0 &&
            write_file(sentinel, "keep\n", 5, 0600) == 0 &&
            format_path(root, "%s/root", data_symlink.persistent) == 0 &&
            format_path(path, "%s/data", root) == 0 &&
            symlink(outside, path) == 0 && lstat(path, &before) == 0,
            "创建指向外部目录的 data symlink");
    CHECK(ish_apple_rootfs_seed_install(data_symlink.seed,
            data_symlink.persistent, "root", &result) == EEXIST,
            "data symlink 必须拒绝复用");
    CHECK(same_entry(&before, path) && symlink_equals(path, outside) &&
            file_equals(sentinel, "keep\n", 5) &&
            verify_no_private_staging(&data_symlink),
            "data symlink 与外部目录必须原样保留");

    struct fixture final_with_unknown_owner;
    struct stat root_before;
    struct stat owner_before;
    CHECK(create_fixture(workspace, "valid-final-unknown-owner", 1,
            &final_with_unknown_owner) == 0 &&
            create_existing_root(&final_with_unknown_owner, true, true) == 0 &&
            format_path(root, "%s/root",
                    final_with_unknown_owner.persistent) == 0 &&
            format_path(path, "%s/.root.installing.owner",
                    final_with_unknown_owner.persistent) == 0 &&
            write_file(path, "foreign-owner\n", 14, 0600) == 0 &&
            lstat(root, &root_before) == 0 &&
            lstat(path, &owner_before) == 0,
            "创建合法 final 与非 symlink canonical owner");
    CHECK(ish_apple_rootfs_seed_install(final_with_unknown_owner.seed,
            final_with_unknown_owner.persistent,
            "root", &result) == EEXIST,
            "合法 final 旁的未知 canonical owner 仍必须拒绝");
    CHECK(same_entry(&root_before, root) &&
            same_entry(&owner_before, path) &&
            file_equals(path, "foreign-owner\n", 14),
            "合法 final 与未知 canonical owner 必须原样保留");
    return 0;
}

static int parse_file_descriptor(const char *value, int *file) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
            parsed < 0 || parsed > INT_MAX)
        return -1;
    *file = (int) parsed;
    return 0;
}

static int run_install_child(int argc, char **argv) {
    if (argc != 6)
        return 119;
    int start_file;
    int report_file;
    if (parse_file_descriptor(argv[4], &start_file) < 0 ||
            parse_file_descriptor(argv[5], &report_file) < 0)
        return 119;
    unsigned char token;
    if (read_all(start_file, &token, 1) < 0)
        return 120;
    close(start_file);
    enum ish_apple_rootfs_seed_result result =
            ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT;
    struct child_report report = {
        .error = ish_apple_rootfs_seed_install(
                argv[2], argv[3], "root", &result),
        .result = (int) result,
    };
    int write_result = write_all(report_file, &report, sizeof(report));
    close(report_file);
    return write_result == 0 ? 0 : 121;
}

static pid_t spawn_install_child(
        const struct fixture *fixture, int start_pipe[2],
        int report_pipe[2]) {
    char start_file[32];
    char report_file[32];
    int start_length = snprintf(start_file, sizeof(start_file),
            "%d", start_pipe[0]);
    int report_length = snprintf(report_file, sizeof(report_file),
            "%d", report_pipe[1]);
    if (start_length < 0 || (size_t) start_length >= sizeof(start_file) ||
            report_length < 0 ||
            (size_t) report_length >= sizeof(report_file)) {
        errno = EOVERFLOW;
        return -1;
    }
    char *const arguments[] = {
        executable_path,
        "--install-child",
        (char *) fixture->seed,
        (char *) fixture->persistent,
        start_file,
        report_file,
        NULL,
    };
    pid_t child = fork();
    if (child != 0)
        return child;
    close(start_pipe[1]);
    close(report_pipe[0]);
    execv(executable_path, arguments);
    _exit(122);
}

static int test_concurrent_install(const char *workspace) {
    struct fixture fixture;
    CHECK(create_fixture(workspace, "concurrent", 2, &fixture) == 0,
            "创建并发安装夹具");
    int start_pipe[2];
    int report_pipe[2];
    CHECK(pipe(start_pipe) == 0 && pipe(report_pipe) == 0,
            "创建并发安装同步管道");
    pid_t first = spawn_install_child(&fixture, start_pipe, report_pipe);
    CHECK(first > 0, "创建第一个并发安装子进程");
    pid_t second = spawn_install_child(&fixture, start_pipe, report_pipe);
    CHECK(second > 0, "创建第二个并发安装子进程");
    close(start_pipe[0]);
    close(report_pipe[1]);
    unsigned char tokens[2] = {1, 1};
    CHECK(write_all(start_pipe[1], tokens, sizeof(tokens)) == 0,
            "同时释放两个安装子进程");
    close(start_pipe[1]);
    struct child_report reports[2];
    CHECK(read_all(report_pipe[0], reports, sizeof(reports)) == 0,
            "收集并发安装结果");
    close(report_pipe[0]);
    int first_status;
    int second_status;
    CHECK(waitpid(first, &first_status, 0) == first &&
            waitpid(second, &second_status, 0) == second &&
            WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0 &&
            WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0,
            "并发安装子进程正常退出");
    unsigned installed = 0;
    unsigned already_present = 0;
    for (size_t i = 0; i < 2; i++) {
        if (reports[i].error != 0)
            fprintf(stderr,
                    "并发安装子进程 %zu 返回 errno=%d、result=%d\n",
                    i, reports[i].error, reports[i].result);
        CHECK(reports[i].error == 0, "并发安装调用都应成功");
        if (reports[i].result == ISH_APPLE_ROOTFS_SEED_INSTALLED)
            installed++;
        else if (reports[i].result ==
                ISH_APPLE_ROOTFS_SEED_ALREADY_PRESENT)
            already_present++;
    }
    CHECK(installed == 1 && already_present == 1,
            "并发安装恰好一次发布、一次复用");
    CHECK(verify_installed_root(&fixture) == 0,
            "验证并发安装最终 root");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--install-child") == 0)
        return run_install_child(argc, argv);
    uint32_t executable_capacity = (uint32_t) sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &executable_capacity) != 0) {
        fprintf(stderr, "无法取得 Apple rootfs seed 测试程序路径\n");
        return 1;
    }
    char resolved_path[PATH_MAX];
    if (realpath(executable_path, resolved_path) == NULL) {
        perror("无法解析 Apple rootfs seed 测试程序路径");
        return 1;
    }
    memcpy(executable_path, resolved_path, strlen(resolved_path) + 1);

    const char *temporary = getenv("TMPDIR");
    if (temporary == NULL || temporary[0] == '\0')
        temporary = "/tmp";
    char workspace[PATH_MAX];
    int length = snprintf(workspace, sizeof(workspace),
            "%s/ish-apple-rootfs-seed-test.XXXXXX", temporary);
    if (length < 0 || (size_t) length >= sizeof(workspace) ||
            mkdtemp(workspace) == NULL) {
        perror("无法创建 Apple rootfs seed 测试目录");
        return 1;
    }

    int status = test_install_and_reuse(workspace);
    if (status == 0)
        status = test_empty_manifest(workspace);
    if (status == 0)
        status = test_rejections(workspace);
    if (status == 0)
        status = test_staging_recovery(workspace);
    if (status == 0)
        status = test_transaction_recovery(workspace);
    if (status == 0)
        status = test_existing_root_rejections(workspace);
    if (status == 0)
        status = test_concurrent_install(workspace);
    if (status != 0 && getenv("ISH_KEEP_ROOTFS_SEED_TEST_TEMP") != NULL) {
        fprintf(stderr, "保留失败现场：%s\n", workspace);
        return status;
    }
    if (remove_tree(workspace) < 0) {
        perror("无法清理 Apple rootfs seed 测试目录");
        status = 1;
    }
    if (status == 0)
        puts("Apple rootfs seed 测试通过");
    return status;
}

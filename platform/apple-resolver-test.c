#include "platform/apple-resolver.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "失败：%s\n", message); \
        failures++; \
    } \
} while (0)

static void test_ipv4_ipv6_and_search(void) {
    struct sockaddr_in ipv4 = {
        .sin_len = sizeof(ipv4),
        .sin_family = AF_INET,
    };
    struct sockaddr_in6 ipv6 = {
        .sin6_len = sizeof(ipv6),
        .sin6_family = AF_INET6,
    };
    CHECK(inet_pton(AF_INET, "192.0.2.53", &ipv4.sin_addr) == 1,
            "构造 IPv4 测试地址");
    CHECK(inet_pton(AF_INET6, "2001:db8::53", &ipv6.sin6_addr) == 1,
            "构造 IPv6 测试地址");

    const char *search[] = {"example.test", "lan"};
    const struct sockaddr *servers[] = {
        (const struct sockaddr *) &ipv4,
        (const struct sockaddr *) &ipv6,
    };
    static const char expected[] =
            "search example.test lan\n"
            "nameserver 192.0.2.53\n"
            "nameserver 2001:db8::53\n";
    char output[sizeof(expected)];
    ssize_t length = ish_apple_resolver_format(
            output, sizeof(output), search, 2, servers, 2);
    CHECK(length == (ssize_t) strlen(expected),
            "返回不含终止符的精确长度");
    CHECK(strcmp(output, expected) == 0,
            "按顺序格式化 search、IPv4 与 IPv6");

    char too_small[sizeof(expected) - 1];
    CHECK(ish_apple_resolver_format(
            too_small, sizeof(too_small), search, 2, servers, 2) ==
                    _ENOSPC,
            "拒绝缺少终止符空间的输出缓冲区");
}

static void test_missing_servers(void) {
    char output[128];
    const char *search[] = {"example.test"};
    CHECK(ish_apple_resolver_format(
            output, sizeof(output), search, 1, NULL, 0) == _ENODATA,
            "没有 nameserver 时拒绝发布仅含 search 的配置");

    struct sockaddr unsupported = {
        .sa_len = sizeof(unsupported),
        .sa_family = AF_UNSPEC,
    };
    struct sockaddr_in missing_length = {
        .sin_family = AF_INET,
    };
    const struct sockaddr *servers[] = {
        &unsupported,
        (const struct sockaddr *) &missing_length,
        NULL,
    };
    CHECK(ish_apple_resolver_format(
            output, sizeof(output), NULL, 0, servers, 3) == _ENODATA,
            "跳过空地址、零长度地址与不支持的地址族");
}

static void test_invalid_arguments(void) {
    char output[32];
    CHECK(ish_apple_resolver_format(
            NULL, sizeof(output), NULL, 0, NULL, 0) == _EINVAL,
            "拒绝空输出指针");
    CHECK(ish_apple_resolver_format(
            output, 0, NULL, 0, NULL, 0) == _EINVAL,
            "拒绝零容量输出");
    CHECK(ish_apple_resolver_format(
            output, sizeof(output), NULL, 1, NULL, 0) == _EINVAL,
            "拒绝缺失的 search 数组");
    CHECK(ish_apple_resolver_format(
            output, sizeof(output), NULL, 0, NULL, 1) == _EINVAL,
            "拒绝缺失的 nameserver 数组");
}

int main(void) {
    test_ipv4_ipv6_and_search();
    test_missing_servers();
    test_invalid_arguments();
    return failures == 0 ? 0 : 1;
}

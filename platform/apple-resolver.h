#ifndef PLATFORM_APPLE_RESOLVER_H
#define PLATFORM_APPLE_RESOLVER_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "misc.h"

// 将 Apple 宿主解析器配置刷新到指定 guest 进程的 /etc/resolv.conf。
int ish_apple_guest_configure_dns_pid(dword_t pid);

#ifdef ISH_APPLE_RESOLVER_TESTING
ssize_t ish_apple_resolver_format(
        char *output, size_t capacity,
        const char *const *search_domains, size_t search_count,
        const struct sockaddr *const *servers, size_t server_count);
#endif

#endif

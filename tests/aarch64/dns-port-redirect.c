#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define GUEST_DNS_ADDRESS UINT32_C(0x7f000035)
#define GUEST_DNS_PORT 53
#define REDIRECT_PORT_ENV "ISH_AARCH64_E2E_DNS_PORT"

static in_port_t redirect_port(void) {
    const char *value = getenv(REDIRECT_PORT_ENV);
    if (value == NULL || *value == '\0')
        return 0;

    unsigned port = 0;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9')
            return 0;
        unsigned digit = (unsigned) (*cursor - '0');
        if (port > (UINT16_MAX - digit) / 10)
            return 0;
        port = port * 10 + digit;
    }
    return port == 0 ? 0 : htons((uint16_t) port);
}

static bool redirect_destination(const struct sockaddr *source,
        socklen_t length, struct sockaddr_storage *destination) {
    in_port_t port = redirect_port();
    if (source == NULL || port == 0 ||
            length < sizeof(struct sockaddr_in) ||
            length > sizeof(*destination))
        return false;
    if (source->sa_family != AF_INET)
        return false;

    const struct sockaddr_in *source_ipv4 =
            (const struct sockaddr_in *) source;
    if (source_ipv4->sin_port != htons(GUEST_DNS_PORT) ||
            ntohl(source_ipv4->sin_addr.s_addr) != GUEST_DNS_ADDRESS)
        return false;

    memcpy(destination, source, length);
    struct sockaddr_in *redirected_ipv4 =
            (struct sockaddr_in *) destination;
    redirected_ipv4->sin_port = port;
    redirected_ipv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return true;
}

static void restore_source(struct sockaddr *address,
        socklen_t capacity, socklen_t returned_length) {
    in_port_t port = redirect_port();
    socklen_t available = capacity < returned_length ?
            capacity : returned_length;
    if (address == NULL || port == 0 ||
            available < sizeof(struct sockaddr_in))
        return;
    if (address->sa_family != AF_INET)
        return;

    struct sockaddr_in *source_ipv4 =
            (struct sockaddr_in *) address;
    if (source_ipv4->sin_port == port &&
            source_ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
        source_ipv4->sin_port = htons(GUEST_DNS_PORT);
        source_ipv4->sin_addr.s_addr = htonl(GUEST_DNS_ADDRESS);
    }
}

static void restore_and_copy_source(struct sockaddr *destination,
        socklen_t capacity, struct sockaddr_storage *source,
        socklen_t source_length) {
    restore_source((struct sockaddr *) source, sizeof(*source), source_length);
    socklen_t copy_length = capacity < source_length ?
            capacity : source_length;
    if (copy_length > sizeof(*source))
        copy_length = sizeof(*source);
    memcpy(destination, source, copy_length);
}

static int redirected_connect(int socket, const struct sockaddr *address,
        socklen_t length) {
    struct sockaddr_storage redirected;
    if (redirect_destination(address, length, &redirected))
        address = (const struct sockaddr *) &redirected;
    return connect(socket, address, length);
}

static ssize_t redirected_sendto(int socket, const void *buffer,
        size_t length, int flags, const struct sockaddr *address,
        socklen_t address_length) {
    struct sockaddr_storage redirected;
    if (redirect_destination(address, address_length, &redirected))
        address = (const struct sockaddr *) &redirected;
    return sendto(socket, buffer, length, flags,
            address, address_length);
}

static ssize_t redirected_sendmsg(
        int socket, const struct msghdr *message, int flags) {
    struct msghdr redirected_message;
    struct sockaddr_storage redirected_address;
    if (message != NULL && redirect_destination(message->msg_name,
                message->msg_namelen, &redirected_address)) {
        redirected_message = *message;
        redirected_message.msg_name = &redirected_address;
        message = &redirected_message;
    }
    return sendmsg(socket, message, flags);
}

static ssize_t redirected_recvfrom(int socket, void *buffer,
        size_t length, int flags, struct sockaddr *address,
        socklen_t *address_length) {
    if (address == NULL || address_length == NULL)
        return recvfrom(socket, buffer, length, flags,
                address, address_length);

    socklen_t capacity = *address_length;
    struct sockaddr_storage source;
    socklen_t source_length = sizeof(source);
    ssize_t result = recvfrom(socket, buffer, length, flags,
            (struct sockaddr *) &source, &source_length);
    if (result >= 0) {
        restore_and_copy_source(address, capacity, &source, source_length);
        *address_length = source_length;
    }
    return result;
}

static ssize_t redirected_recvmsg(
        int socket, struct msghdr *message, int flags) {
    if (message == NULL || message->msg_name == NULL)
        return recvmsg(socket, message, flags);

    void *destination = message->msg_name;
    socklen_t capacity = message->msg_namelen;
    struct sockaddr_storage source;
    struct msghdr staged_message = *message;
    staged_message.msg_name = &source;
    staged_message.msg_namelen = sizeof(source);
    ssize_t result = recvmsg(socket, &staged_message, flags);
    if (result >= 0) {
        restore_and_copy_source(destination, capacity,
                &source, staged_message.msg_namelen);
        message->msg_namelen = staged_message.msg_namelen;
        message->msg_controllen = staged_message.msg_controllen;
        message->msg_flags = staged_message.msg_flags;
    }
    return result;
}

static int redirected_getpeername(int socket,
        struct sockaddr *address, socklen_t *address_length) {
    if (address == NULL || address_length == NULL) {
        errno = EFAULT;
        return -1;
    }

    socklen_t capacity = *address_length;
    struct sockaddr_storage source;
    socklen_t source_length = sizeof(source);
    int result = getpeername(socket,
            (struct sockaddr *) &source, &source_length);
    if (result == 0) {
        restore_and_copy_source(address, capacity, &source, source_length);
        *address_length = source_length;
    }
    return result;
}

// 探针通过该入口验证 libc 非空契约之外的内核 EFAULT 边界。
__attribute__((visibility("default")))
int ish_aarch64_e2e_redirected_getpeername(int socket,
        struct sockaddr *address, socklen_t *address_length) {
    return redirected_getpeername(socket, address, address_length);
}

// dyld 只重定向其他映像的导入；替代函数内部仍调用 libSystem 原符号。
#define DYLD_INTERPOSE(replacement, replacee) \
    __attribute__((used)) static struct { \
        const void *replacement; \
        const void *replacee; \
    } interpose_##replacee \
    __attribute__((section("__DATA,__interpose"))) = { \
        (const void *) replacement, (const void *) replacee, \
    }

DYLD_INTERPOSE(redirected_connect, connect);
DYLD_INTERPOSE(redirected_sendto, sendto);
DYLD_INTERPOSE(redirected_sendmsg, sendmsg);
DYLD_INTERPOSE(redirected_recvfrom, recvfrom);
DYLD_INTERPOSE(redirected_recvmsg, recvmsg);
DYLD_INTERPOSE(redirected_getpeername, getpeername);

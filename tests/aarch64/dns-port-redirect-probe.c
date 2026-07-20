#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define ADDRESS_CANARY UINT8_C(0xa5)
#define DNS_PORT 53
#define GUEST_DNS_ADDRESS UINT32_C(0x7f000035)
#define SHORT_ADDRESS_CAPACITY offsetof(struct sockaddr_in, sin_port)

static const uint8_t query_template[] = {
    0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x05, 'p', 'r', 'o', 'b', 'e',
    0x07, 'i', 's', 'h', '-', 'd', 'n', 's',
    0x04, 't', 'e', 's', 't', 0x00,
    0x00, 0x01, 0x00, 0x01,
};

union address_buffer {
    struct sockaddr_in alignment;
    uint8_t bytes[sizeof(struct sockaddr_in)];
};

static int fail(const char *message) {
    fprintf(stderr, "DNS 端口重定向测试失败：%s\n", message);
    return 1;
}

static int open_socket(void) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
        return -1;

    struct timeval timeout = {.tv_sec = 2};
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                &timeout, sizeof(timeout)) < 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static bool response_is_expected(
        const uint8_t *response, ssize_t received, uint16_t query_id) {
    return received >= (ssize_t) sizeof(query_template) + 16 &&
        response[0] == (uint8_t) (query_id >> 8) &&
        response[1] == (uint8_t) query_id &&
        (response[2] & 0x80) != 0 && (response[3] & 0x0f) == 0 &&
        response[6] == 0 && response[7] == 1 &&
        memcmp(response + received - 4, "\x7f\x00\x00\x01", 4) == 0;
}

static bool source_is_guest_dns(
        const struct sockaddr_in *source, socklen_t length) {
    return length >= sizeof(*source) && source->sin_family == AF_INET &&
        source->sin_port == htons(DNS_PORT) &&
        ntohl(source->sin_addr.s_addr) == GUEST_DNS_ADDRESS;
}

static bool canary_is_untouched(
        const union address_buffer *address, size_t offset) {
    for (size_t index = offset; index < sizeof(address->bytes); index++) {
        if (address->bytes[index] != ADDRESS_CANARY)
            return false;
    }
    return true;
}

static int send_query_with_sendto(
        int socket_fd, const struct sockaddr_in *destination,
        uint16_t query_id) {
    uint8_t query[sizeof(query_template)];
    memcpy(query, query_template, sizeof(query));
    query[0] = (uint8_t) (query_id >> 8);
    query[1] = (uint8_t) query_id;
    ssize_t sent = sendto(socket_fd, query, sizeof(query), 0,
            (const struct sockaddr *) destination, sizeof(*destination));
    return sent == (ssize_t) sizeof(query) ? 0 : fail("sendto 发送 A 查询");
}

static int send_query_with_sendmsg(int socket_fd,
        const struct sockaddr_in *destination, uint16_t query_id) {
    uint8_t query[sizeof(query_template)];
    memcpy(query, query_template, sizeof(query));
    query[0] = (uint8_t) (query_id >> 8);
    query[1] = (uint8_t) query_id;
    struct iovec vector = {
        .iov_base = (void *) query,
        .iov_len = sizeof(query),
    };
    struct msghdr message = {
        .msg_name = (void *) destination,
        .msg_namelen = destination == NULL ? 0 : sizeof(*destination),
        .msg_iov = &vector,
        .msg_iovlen = 1,
    };
    ssize_t sent = sendmsg(socket_fd, &message, 0);
    return sent == (ssize_t) sizeof(query) ? 0 : fail("sendmsg 发送 A 查询");
}

static int receive_with_recvfrom(
        int socket_fd, bool short_address, uint16_t query_id) {
    uint8_t response[512];
    union address_buffer source;
    memset(source.bytes, ADDRESS_CANARY, sizeof(source.bytes));
    socklen_t capacity = short_address ?
            SHORT_ADDRESS_CAPACITY : sizeof(source);
    socklen_t source_length = capacity;
    ssize_t received = recvfrom(socket_fd, response, sizeof(response), 0,
            (struct sockaddr *) source.bytes, &source_length);
    if (received < 0) {
        perror("DNS 端口重定向测试 recvfrom");
        return 1;
    }
    if (!response_is_expected(response, received, query_id))
        return fail("recvfrom 没有收到预期 A 记录");
    if (short_address) {
        if (source_length < sizeof(struct sockaddr_in))
            return fail("recvfrom 没有报告完整来源地址长度");
        if (!canary_is_untouched(&source, capacity))
            return fail("recvfrom 改写了短地址缓冲区边界外的字节");
    } else if (!source_is_guest_dns(&source.alignment, source_length)) {
        return fail("recvfrom 来源没有恢复为 127.0.0.53:53");
    }
    return 0;
}

static int receive_with_recvmsg(
        int socket_fd, bool short_address, uint16_t query_id) {
    uint8_t response[512];
    union address_buffer source;
    memset(source.bytes, ADDRESS_CANARY, sizeof(source.bytes));
    socklen_t capacity = short_address ?
            SHORT_ADDRESS_CAPACITY : sizeof(source);
    struct iovec vector = {
        .iov_base = response,
        .iov_len = sizeof(response),
    };
    struct msghdr message = {
        .msg_name = source.bytes,
        .msg_namelen = capacity,
        .msg_iov = &vector,
        .msg_iovlen = 1,
    };
    ssize_t received = recvmsg(socket_fd, &message, 0);
    if (received < 0) {
        perror("DNS 端口重定向测试 recvmsg");
        return 1;
    }
    if (!response_is_expected(response, received, query_id))
        return fail("recvmsg 没有收到预期 A 记录");
    if (short_address) {
        if (message.msg_namelen < sizeof(struct sockaddr_in))
            return fail("recvmsg 没有报告完整来源地址长度");
        if (!canary_is_untouched(&source, capacity))
            return fail("recvmsg 改写了短地址缓冲区边界外的字节");
    } else if (!source_is_guest_dns(
                &source.alignment, message.msg_namelen)) {
        return fail("recvmsg 来源没有恢复为 127.0.0.53:53");
    }
    return 0;
}

static int check_peer_name(int socket_fd, bool short_address) {
    union address_buffer peer;
    memset(peer.bytes, ADDRESS_CANARY, sizeof(peer.bytes));
    socklen_t capacity = short_address ?
            SHORT_ADDRESS_CAPACITY : sizeof(peer);
    socklen_t peer_length = capacity;
    if (getpeername(socket_fd,
                (struct sockaddr *) peer.bytes, &peer_length) < 0)
        return fail("getpeername 读取已连接 DNS 地址");
    if (short_address) {
        if (peer_length < sizeof(struct sockaddr_in))
            return fail("getpeername 没有报告完整 peer 地址长度");
        if (!canary_is_untouched(&peer, capacity))
            return fail("getpeername 改写了短地址缓冲区边界外的字节");
    } else if (!source_is_guest_dns(&peer.alignment, peer_length)) {
        return fail("getpeername 没有恢复为 127.0.0.53:53");
    }
    return 0;
}

static int send_with_tiny_destination(int socket_fd) {
    uint8_t *address = malloc(1);
    if (address == NULL)
        return fail("分配一字节 destination 缓冲区");
    address[0] = 0;
    ssize_t sent = sendto(socket_fd, query_template,
            sizeof(query_template), 0,
            (const struct sockaddr *) address, 1);
    free(address);
    return sent < 0 ? 0 : fail("sendto 接受了一字节 destination");
}

static int receive_with_tiny_recvfrom(
        int socket_fd, uint16_t query_id) {
    uint8_t response[512];
    uint8_t *source = malloc(1);
    if (source == NULL)
        return fail("分配一字节 recvfrom 地址缓冲区");
    socklen_t source_length = 1;
    ssize_t received = recvfrom(socket_fd, response, sizeof(response), 0,
            (struct sockaddr *) source, &source_length);
    free(source);
    if (received < 0) {
        perror("DNS 端口重定向测试一字节 recvfrom");
        return 1;
    }
    if (source_length < sizeof(struct sockaddr_in))
        return fail("一字节 recvfrom 没有报告完整来源地址长度");
    return response_is_expected(response, received, query_id) ?
        0 : fail("一字节 recvfrom 没有收到预期 A 记录");
}

static int receive_with_tiny_recvmsg(
        int socket_fd, uint16_t query_id) {
    uint8_t response[512];
    union address_buffer *source = malloc(sizeof(*source));
    if (source == NULL)
        return fail("分配一字节 recvmsg 地址缓冲区");
    memset(source->bytes, ADDRESS_CANARY, sizeof(source->bytes));
    struct iovec vector = {
        .iov_base = response,
        .iov_len = sizeof(response),
    };
    struct msghdr message = {
        .msg_name = source->bytes,
        .msg_namelen = 1,
        .msg_iov = &vector,
        .msg_iovlen = 1,
    };
    ssize_t received = recvmsg(socket_fd, &message, 0);
    int result = 0;
    if (received < 0) {
        perror("DNS 端口重定向测试一字节 recvmsg");
        result = 1;
    } else if (message.msg_namelen < sizeof(struct sockaddr_in)) {
        result = fail("一字节 recvmsg 没有报告完整来源地址长度");
    } else if (!canary_is_untouched(source, 1)) {
        result = fail("recvmsg 改写了一字节容量外的地址缓冲区");
    } else if (!response_is_expected(response, received, query_id)) {
        result = fail("一字节 recvmsg 没有收到预期 A 记录");
    }
    free(source);
    return result;
}

static int check_peer_name_with_tiny_buffer(int socket_fd) {
    uint8_t *peer = malloc(1);
    if (peer == NULL)
        return fail("分配一字节 getpeername 地址缓冲区");
    socklen_t peer_length = 1;
    int result = getpeername(
            socket_fd, (struct sockaddr *) peer, &peer_length);
    free(peer);
    if (result < 0)
        return fail("一字节 getpeername 读取已连接 DNS 地址");
    return peer_length >= sizeof(struct sockaddr_in) ?
        0 : fail("一字节 getpeername 没有报告完整 peer 地址长度");
}

static int check_peer_name_faults(int socket_fd) {
    typedef int (*redirected_getpeername_fn)(
            int, struct sockaddr *, socklen_t *);
    void *symbol = dlsym(
            RTLD_DEFAULT, "ish_aarch64_e2e_redirected_getpeername");
    if (symbol == NULL)
        return fail("查找 getpeername 测试入口");
    _Static_assert(sizeof(symbol) == sizeof(redirected_getpeername_fn),
            "Darwin 函数指针宽度必须等于数据指针宽度");
    redirected_getpeername_fn call_getpeername;
    memcpy(&call_getpeername, &symbol, sizeof(call_getpeername));
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    errno = 0;
    if (call_getpeername(socket_fd, NULL, &peer_length) != -1 ||
            errno != EFAULT)
        return fail("getpeername 空地址没有返回 EFAULT");
    errno = 0;
    if (call_getpeername(socket_fd,
                (struct sockaddr *) &peer, NULL) != -1 || errno != EFAULT)
        return fail("getpeername 空长度指针没有返回 EFAULT");
    return 0;
}

int main(void) {
    struct sockaddr_in destination = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
    };
    if (inet_pton(AF_INET, "127.0.0.53", &destination.sin_addr) != 1)
        return fail("构造 guest DNS 地址");

    int socket_fd = open_socket();
    if (socket_fd < 0)
        return fail("创建未连接 UDP socket");

    int result = 0;
    if (send_with_tiny_destination(socket_fd) != 0 ||
            send_query_with_sendto(socket_fd, &destination, 0x6142) != 0 ||
            receive_with_recvfrom(socket_fd, false, 0x6142) != 0 ||
            send_query_with_sendto(socket_fd, &destination, 0x6143) != 0 ||
            receive_with_recvfrom(socket_fd, true, 0x6143) != 0 ||
            send_query_with_sendto(socket_fd, &destination, 0x6144) != 0 ||
            receive_with_tiny_recvfrom(socket_fd, 0x6144) != 0 ||
            send_query_with_sendmsg(
                socket_fd, &destination, 0x6150) != 0 ||
            receive_with_recvmsg(socket_fd, false, 0x6150) != 0 ||
            send_query_with_sendmsg(
                socket_fd, &destination, 0x6151) != 0 ||
            receive_with_recvmsg(socket_fd, true, 0x6151) != 0 ||
            send_query_with_sendmsg(
                socket_fd, &destination, 0x6152) != 0 ||
            receive_with_tiny_recvmsg(socket_fd, 0x6152) != 0)
        result = 1;
    close(socket_fd);
    if (result != 0)
        return result;

    socket_fd = open_socket();
    if (socket_fd < 0)
        return fail("创建已连接 UDP socket");
    if (connect(socket_fd, (const struct sockaddr *) &destination,
                sizeof(destination)) < 0 ||
            check_peer_name(socket_fd, false) != 0 ||
            check_peer_name(socket_fd, true) != 0 ||
            check_peer_name_with_tiny_buffer(socket_fd) != 0 ||
            check_peer_name_faults(socket_fd) != 0 ||
            send_query_with_sendmsg(socket_fd, NULL, 0x6160) != 0 ||
            receive_with_recvmsg(socket_fd, false, 0x6160) != 0)
        result = 1;
    close(socket_fd);
    return result;
}

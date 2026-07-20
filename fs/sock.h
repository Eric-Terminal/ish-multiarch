#ifndef SYS_SOCK_H
#define SYS_SOCK_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "kernel/errno.h"
#include "fs/fd.h"
#include "misc.h"
#include "debug.h"

extern const struct fd_ops socket_fdops;

int_t sys_socketcall(dword_t call_num, addr_t args_addr);

int_t sys_socket(dword_t domain, dword_t type, dword_t protocol);
int_t socket_create_task(struct task *task,
        dword_t domain, dword_t type, dword_t protocol);

struct socket_ref {
    struct fd *fd;
};
struct socket_address;
struct inode_data;
struct unix_abstract;
struct scm;

// 强引用跨越 guest 回调与 host I/O，成功获取后必须配对 release。
int socket_ref_get_task(struct task *task, fd_t sock_fd,
        struct socket_ref *socket);
void socket_ref_release(struct socket_ref *socket);
int_t socket_connect_ref_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length);
int_t socket_connect_retained_task(struct task *task,
        struct fd *fd, const void *address, size_t address_length);
int_t socket_bind_ref_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length);
int socket_address_prepare_task(struct task *task,
        const struct socket_ref *socket,
        const void *address, size_t address_length,
        struct socket_address *prepared);
int socket_address_validate_for_socket(const struct socket_ref *socket,
        const void *address, size_t address_length);
void socket_address_release(struct socket_address *address);
int socket_sendto_validate(const struct socket_ref *socket,
        size_t length, dword_t flags);
size_t socket_sendto_prefix_size(const struct socket_ref *socket);
int socket_sendto_prefix_validate(const struct socket_ref *socket,
        const void *prefix, size_t prefix_size);
size_t socket_sendto_transaction_size(const struct socket_ref *socket,
        size_t length, dword_t flags);
int socket_sendto_transaction_validate(const struct socket_ref *socket,
        size_t length);
int socket_sendto_destination_check(const struct socket_ref *socket,
        const struct socket_address *address, dword_t flags);
ssize_t socket_sendto_ref(const struct socket_ref *socket,
        const void *buffer, size_t length, dword_t flags,
        const struct socket_address *address);
ssize_t socket_recvfrom_ref(const struct socket_ref *socket,
        void *buffer, size_t length, dword_t flags,
        struct socket_address *address);
// send 成功后接管并清空 scm；recv 返回的 scm 由调用方释放或安装。
ssize_t socket_sendmsg_ref(const struct socket_ref *socket,
        const void *buffer, size_t length, dword_t flags,
        const struct socket_address *address, struct scm **scm);
ssize_t socket_recvmsg_ref(const struct socket_ref *socket,
        void *buffer, size_t length, dword_t flags,
        struct socket_address *address, dword_t *message_flags,
        struct scm **scm);

enum socket_guest_abi {
    SOCKET_GUEST_I386,
    SOCKET_GUEST_AARCH64,
};

ssize_t socket_setsockopt_value_size(
        sdword_t level, sdword_t option, sdword_t value_length,
        enum socket_guest_abi guest_abi);
int_t socket_setsockopt_ref(const struct socket_ref *socket,
        sdword_t level, sdword_t option,
        const void *value, sdword_t value_length,
        enum socket_guest_abi guest_abi);

int_t socket_getname_ref(const struct socket_ref *socket, bool peer,
        struct sockaddr_storage *address, dword_t *length);

#define SOCKET_OPTION_VALUE_MAX 256

enum socket_option_copy_order {
    SOCKET_OPTION_VALUE_FIRST,
    SOCKET_OPTION_LENGTH_FIRST,
};

struct socket_option_result {
    byte_t value[SOCKET_OPTION_VALUE_MAX];
    dword_t length;
    enum socket_option_copy_order copy_order;
};

int_t socket_getsockopt_ref(const struct socket_ref *socket,
        sdword_t level, sdword_t option, sdword_t capacity,
        enum socket_guest_abi guest_abi,
        struct socket_option_result *result);
int_t socket_shutdown_ref(
        const struct socket_ref *socket, sdword_t how);

// socketpair 在创建协议对象前先由调用方完成 fd 预留与编号写回。
int socket_pair_flags_validate(dword_t type);
int socket_pair_create_task(struct task *task,
        dword_t domain, dword_t type, dword_t protocol,
        struct fd *pair[2]);

int_t socket_listen_ref_task(struct task *task,
        const struct socket_ref *socket, sdword_t backlog);
int socket_accept_flags_validate(dword_t flags);

struct socket_accept_result {
    struct fd *fd;
    struct fd *connecting_peer;
    struct sockaddr_storage address;
    dword_t address_length;
};

int socket_accept_retained_task(struct task *task,
        struct fd *listener, bool want_address,
        struct socket_accept_result *accepted);
void socket_accept_finish(struct socket_accept_result *accepted);
void socket_accept_reject(struct socket_accept_result *accepted);

#define SOCKET_IO_TRANSACTION_LIMIT UINT32_C(0x100000)
#define SOCKET_STREAM_NONBLOCK_TRANSACTION_LIMIT UINT32_C(0x10000)

int_t sys_bind(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len);
int_t sys_connect(fd_t sock_fd, addr_t sockaddr_addr, uint_t sockaddr_len);
int_t sys_listen(fd_t sock_fd, int_t backlog);
int_t sys_accept(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_getsockname(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_getpeername(fd_t sock_fd, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_socketpair(dword_t domain, dword_t type, dword_t protocol, addr_t sockets_addr);
int_t sys_sendto(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, dword_t sockaddr_len);
int_t sys_recvfrom(fd_t sock_fd, addr_t buffer_addr, dword_t len, dword_t flags, addr_t sockaddr_addr, addr_t sockaddr_len_addr);
int_t sys_shutdown(fd_t sock_fd, dword_t how);
int_t sys_setsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t value_len);
int_t sys_getsockopt(fd_t sock_fd, dword_t level, dword_t option, addr_t value_addr, dword_t len_addr);
int_t sys_sendmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags);
int_t sys_recvmsg(fd_t sock_fd, addr_t msghdr_addr, int_t flags);
int_t sys_sendmmsg(fd_t sock_fd, addr_t msgvec_addr, uint_t msgvec_len, int_t flags);

#define SOCKADDR_DATA_MAX 108

struct sockaddr_ {
    uint16_t family;
    char data[14];
};
struct sockaddr_max_ {
    uint16_t family;
    char data[SOCKADDR_DATA_MAX];
};

struct socket_address {
    struct sockaddr_storage storage;
    socklen_t length;
    struct inode_data *lookup_inode;
    struct unix_abstract *lookup_abstract;
    bool unix_name_valid;
    uint8_t unix_name_length;
    char unix_name[SOCKADDR_DATA_MAX + 1];
};

size_t sockaddr_size(void *p);
// result comes from malloc
struct sockaddr *sockaddr_to_real(void *p);

struct msghdr_ {
    addr_t msg_name;
    uint_t msg_namelen;
    addr_t msg_iov;
    uint_t msg_iovlen;
    addr_t msg_control;
    uint_t msg_controllen;
    int_t msg_flags;
};

struct cmsghdr_ {
    dword_t len;
    int_t level;
    int_t type;
    uint8_t data[];
};
#define SCM_RIGHTS_ 1
#define SCM_CREDENTIALS_ 2
// copied and ported from musl
#define CMSG_LEN_(cmsg) (((cmsg)->len + sizeof(dword_t) - 1) & ~(dword_t)(sizeof(dword_t) - 1))
#define CMSG_NEXT_(cmsg) ((uint8_t *)(cmsg) + CMSG_LEN_(cmsg))
#define CMSG_NXTHDR_(cmsg, mhdr_end) ((cmsg)->len < sizeof (struct cmsghdr_) || \
        CMSG_LEN_(cmsg) + sizeof(struct cmsghdr_) >= (size_t) (mhdr_end - (uint8_t *)(cmsg)) \
        ? NULL : (struct cmsghdr_ *)CMSG_NEXT_(cmsg))

struct scm {
    struct list queue;
    // 成功发布后同时进入全局 inflight 队列；接收、丢弃与 GC 只摘除一次。
    struct list inflight_queue;
    // 弱引用仅在全局 SCM 锁下访问，物理队列保证接收 socket 的生命周期。
    struct fd *receiver;
    uid_t_ sender_uid;
    qword_t sender_nofile;
    bool sender_limit_exempt;
    bool inflight;
    unsigned num_fds;
    struct fd *fds[];
};

#define SOCKET_SCM_MAX_FDS UINT32_C(253)

int socket_scm_create_task(struct task *task,
        const fd_t *numbers, unsigned count, struct scm **scm);
void socket_scm_release(struct scm *scm);
// 显式入口用于安全点与确定性测试；checkpoint 在没有请求时仅做原子读取。
void socket_scm_collect_now(void);
void socket_scm_collect_checkpoint(void);
qword_t socket_scm_inflight_count(uid_t_ uid);

struct socket_scm_ref_drop {
    uint64_t edge_generation;
    unsigned incoming;
    bool tracked;
};

// drop 前只读取活对象；drop 后仅凭栈快照触发全局请求，避免并发 final free。
void socket_scm_ref_drop_prepare(
        struct fd *fd, struct socket_scm_ref_drop *drop);
void socket_scm_ref_drop_complete(
        const struct socket_scm_ref_drop *drop, unsigned remaining);

#define PF_LOCAL_ 1
#define PF_INET_ 2
#define PF_INET6_ 10
#define AF_LOCAL_ PF_LOCAL_
#define AF_INET_ PF_INET_
#define AF_INET6_ PF_INET6_
static inline int sock_family_to_real(int fake) {
    switch (fake) {
        case PF_LOCAL_: return PF_LOCAL;
        case PF_INET_: return PF_INET;
        case PF_INET6_: return PF_INET6;
    }
    return -1;
}
static inline int sock_family_from_real(int fake) {
    switch (fake) {
        case PF_LOCAL: return PF_LOCAL_;
        case PF_INET: return PF_INET_;
        case PF_INET6: return PF_INET6_;
    }
    return -1;
}

#define SOCK_STREAM_ 1
#define SOCK_DGRAM_ 2
#define SOCK_RAW_ 3
#define SOCK_SEQPACKET_ 5
#define SOCK_NONBLOCK_ 0x800
#define SOCK_CLOEXEC_ 0x80000

static inline int sock_type_to_real(int type, int protocol) {
    switch (type & 0xff) {
        case SOCK_STREAM_:
            if (protocol != 0 && protocol != IPPROTO_TCP)
                return -1;
            return SOCK_STREAM;
        case SOCK_DGRAM_:
            switch (protocol) {
                default:
                    return -1;
                case 0:
                case IPPROTO_UDP:
                case IPPROTO_ICMP:
                case IPPROTO_ICMPV6:
                    break;
            }
            return SOCK_DGRAM;
        case SOCK_RAW_:
            switch (protocol) {
                default:
                    return -1;
                case IPPROTO_RAW:
                case IPPROTO_UDP:
                case IPPROTO_ICMP:
                case IPPROTO_ICMPV6:
                    break;
            }
            return SOCK_DGRAM;
    }
    return -1;
}

#define MSG_OOB_ 0x1
#define MSG_PEEK_ 0x2
#define MSG_DONTROUTE_ 0x4
#define MSG_CTRUNC_  0x8
#define MSG_TRUNC_  0x20
#define MSG_DONTWAIT_ 0x40
#define MSG_EOR_    0x80
#define MSG_WAITALL_ 0x100
#define MSG_CONFIRM_ 0x800
#define MSG_ERRQUEUE_ 0x2000
#define MSG_NOSIGNAL_ 0x4000
#define MSG_MORE_ 0x8000

static inline int sock_flags_to_real(int fake) {
    int real = 0;
    if (fake & MSG_OOB_) real |= MSG_OOB;
    if (fake & MSG_PEEK_) real |= MSG_PEEK;
    if (fake & MSG_DONTROUTE_) real |= MSG_DONTROUTE;
    if (fake & MSG_CTRUNC_) real |= MSG_CTRUNC;
    if (fake & MSG_TRUNC_) real |= MSG_TRUNC;
    if (fake & MSG_DONTWAIT_) real |= MSG_DONTWAIT;
    if (fake & MSG_EOR_) real |= MSG_EOR;
    if (fake & MSG_WAITALL_) real |= MSG_WAITALL;
#ifdef MSG_CONFIRM
    if (fake & MSG_CONFIRM_) real |= MSG_CONFIRM;
#endif
#ifdef MSG_ERRQUEUE
    if (fake & MSG_ERRQUEUE_) real |= MSG_ERRQUEUE;
#endif
#ifdef MSG_NOSIGNAL
    if (fake & MSG_NOSIGNAL_) real |= MSG_NOSIGNAL;
#endif
#ifdef MSG_MORE
    if (fake & MSG_MORE_) real |= MSG_MORE;
#endif
    if (fake & ~(MSG_OOB_|MSG_PEEK_|MSG_DONTROUTE_|MSG_CTRUNC_|
            MSG_TRUNC_|MSG_DONTWAIT_|MSG_EOR_|MSG_WAITALL_|
            MSG_CONFIRM_|MSG_ERRQUEUE_|MSG_NOSIGNAL_|MSG_MORE_))
        // Linux 不在 syscall 层统一拒绝未知消息位；UDP 未定义位由回归测试固定。
        TRACE("忽略未映射的 Linux socket flags：%d\n", fake);
    return real;
}
static inline int sock_flags_from_real(int real) {
    int fake = 0;
    if (real & MSG_OOB) fake |= MSG_OOB_;
    if (real & MSG_PEEK) fake |= MSG_PEEK_;
    if (real & MSG_DONTROUTE) fake |= MSG_DONTROUTE_;
    if (real & MSG_CTRUNC) fake |= MSG_CTRUNC_;
    if (real & MSG_TRUNC) fake |= MSG_TRUNC_;
    if (real & MSG_DONTWAIT) fake |= MSG_DONTWAIT_;
    if (real & MSG_EOR) fake |= MSG_EOR_;
    if (real & MSG_WAITALL) fake |= MSG_WAITALL_;
#ifdef MSG_CONFIRM
    if (real & MSG_CONFIRM) fake |= MSG_CONFIRM_;
#endif
#ifdef MSG_ERRQUEUE
    if (real & MSG_ERRQUEUE) fake |= MSG_ERRQUEUE_;
#endif
#ifdef MSG_NOSIGNAL
    if (real & MSG_NOSIGNAL) fake |= MSG_NOSIGNAL_;
#endif
#ifdef MSG_MORE
    if (real & MSG_MORE) fake |= MSG_MORE_;
#endif
    return fake;
}

#define SOL_SOCKET_ 1

#define SO_REUSEADDR_ 2
#define SO_TYPE_ 3
#define SO_ERROR_ 4
#define SO_BROADCAST_ 6
#define SO_SNDBUF_ 7
#define SO_RCVBUF_ 8
#define SO_KEEPALIVE_ 9
#define SO_LINGER_ 13
#define SO_REUSEPORT_ 15
#define SO_PEERCRED_ 17
#define SO_TIMESTAMP_ 29
#define SO_ACCEPTCONN_ 30
#define SO_PROTOCOL_ 38
#define SO_DOMAIN_ 39
#define SO_RCVTIMEO_OLD_ 20
#define SO_SNDTIMEO_OLD_ 21
#define SO_RCVTIMEO_NEW_ 66
#define SO_SNDTIMEO_NEW_ 67
#define SO_RCVTIMEO_ SO_RCVTIMEO_NEW_
#define SO_SNDTIMEO_ SO_SNDTIMEO_NEW_
#define IP_TOS_ 1
#define IP_TTL_ 2
#define IP_HDRINCL_ 3
#define IP_RETOPTS_ 7
#define IP_MTU_DISCOVER_ 10
#define IP_RECVERR_ 11
#define IP_RECVTTL_ 12
#define IP_RECVTOS_ 13
#define TCP_NODELAY_ 1
#define TCP_DEFER_ACCEPT_ 9
#define TCP_INFO_ 11
#define TCP_CONGESTION_ 13
#define IPV6_UNICAST_HOPS_ 16
#define IPV6_RECVERR_ 25
#define IPV6_V6ONLY_ 26
#define IPV6_TCLASS_ 67
#define ICMP6_FILTER_ 1

static inline int sock_opt_to_real(int fake, int level) {
    switch (level) {
        case SOL_SOCKET_: switch (fake) {
            case SO_REUSEADDR_: return SO_REUSEADDR;
            case SO_TYPE_: return SO_TYPE;
            case SO_ERROR_: return SO_ERROR;
            case SO_BROADCAST_: return SO_BROADCAST;
            case SO_KEEPALIVE_: return SO_KEEPALIVE;
            case SO_LINGER_: return SO_LINGER;
#ifdef SO_REUSEPORT
            case SO_REUSEPORT_: return SO_REUSEPORT;
#endif
            case SO_SNDBUF_: return SO_SNDBUF;
            case SO_RCVBUF_: return SO_RCVBUF;
            case SO_TIMESTAMP_: return SO_TIMESTAMP;
            case SO_RCVTIMEO_OLD_:
            case SO_RCVTIMEO_NEW_: return SO_RCVTIMEO;
            case SO_SNDTIMEO_OLD_:
            case SO_SNDTIMEO_NEW_: return SO_SNDTIMEO;
        } break;
        case IPPROTO_TCP: switch (fake) {
            case TCP_NODELAY_: return TCP_NODELAY;
            case TCP_DEFER_ACCEPT_: return 0; // unimplemented
#if defined(__linux__)
            case TCP_INFO_: return TCP_INFO;
            case TCP_CONGESTION_: return TCP_CONGESTION;
#endif
        } break;
        case IPPROTO_IP: switch (fake) {
            case IP_TOS_: return IP_TOS;
            case IP_TTL_: return IP_TTL;
            case IP_HDRINCL_: return IP_HDRINCL;
            case IP_RETOPTS_: return IP_RETOPTS;
            case IP_RECVTTL_: return IP_RECVTTL;
            case IP_RECVTOS_: return IP_RECVTOS;
        } break;
        case IPPROTO_IPV6: switch (fake) {
            case IPV6_UNICAST_HOPS_: return IPV6_UNICAST_HOPS;
            case IPV6_TCLASS_: return IPV6_TCLASS;
            case IPV6_V6ONLY_: return IPV6_V6ONLY;
        } break;
    }
    return -1;
}

static inline int sock_level_to_real(int fake) {
    if (fake == SOL_SOCKET_)
        return SOL_SOCKET;
    return fake;
}

extern const char *sock_tmp_prefix;

struct tcp_info_ {
    uint8_t state;
    uint8_t ca_state;
    uint8_t retransmits;
    uint8_t probes;
    uint8_t backoff;
    uint8_t options;
    uint8_t snd_wscale:4, rcv_wscale:4;

    uint32_t rto;
    uint32_t ato;
    uint32_t snd_mss;
    uint32_t rcv_mss;

    uint32_t unacked;
    uint32_t sacked;
    uint32_t lost;
    uint32_t retrans;
    uint32_t fackets;

    uint32_t last_data_sent;
    uint32_t last_ack_sent;
    uint32_t last_data_recv;
    uint32_t last_ack_recv;

    uint32_t pmtu;
    uint32_t rcv_ssthresh;
    uint32_t rtt;
    uint32_t rttvar;
    uint32_t snd_ssthresh;
    uint32_t snd_cwnd;
    uint32_t advmss;
    uint32_t reordering;

    uint32_t rcv_rtt;
    uint32_t rcv_space;

    uint32_t total_retrans;
};

#endif

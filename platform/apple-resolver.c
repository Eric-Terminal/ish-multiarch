#include "platform/apple-resolver.h"

#include <netdb.h>
#include <resolv.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"

#define APPLE_RESOLVER_SERVER_CAPACITY 32
#define APPLE_RESOLV_CONF_CAPACITY (16 * 1024)

static int resolver_append(
        char *output, size_t capacity, size_t *length,
        const char *format, ...) {
    size_t available = capacity - *length;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(
            output + *length, available, format, arguments);
    va_end(arguments);
    if (written < 0)
        return _EIO;
    if ((size_t) written >= available)
        return _ENOSPC;
    *length += (size_t) written;
    return 0;
}

#ifdef ISH_APPLE_RESOLVER_TESTING
ssize_t
#else
static ssize_t
#endif
ish_apple_resolver_format(
        char *output, size_t capacity,
        const char *const *search_domains, size_t search_count,
        const struct sockaddr *const *servers, size_t server_count) {
    if (output == NULL || capacity == 0 ||
            (search_count != 0 && search_domains == NULL) ||
            (server_count != 0 && servers == NULL))
        return _EINVAL;

    output[0] = '\0';
    size_t length = 0;
    bool wrote_search = false;
    for (size_t index = 0; index < search_count; index++) {
        const char *domain = search_domains[index];
        if (domain == NULL || domain[0] == '\0')
            continue;
        int error = resolver_append(
                output, capacity, &length,
                wrote_search ? " %s" : "search %s", domain);
        if (error < 0)
            return error;
        wrote_search = true;
    }
    if (wrote_search) {
        int error = resolver_append(output, capacity, &length, "\n");
        if (error < 0)
            return error;
    }

    size_t nameserver_count = 0;
    for (size_t index = 0; index < server_count; index++) {
        const struct sockaddr *server = servers[index];
        if (server == NULL || server->sa_len == 0)
            continue;

        socklen_t address_length;
        switch (server->sa_family) {
            case AF_INET:
                address_length = (socklen_t) sizeof(struct sockaddr_in);
                break;
            case AF_INET6:
                address_length = (socklen_t) sizeof(struct sockaddr_in6);
                break;
            default:
                continue;
        }

        char address[NI_MAXHOST];
        if (getnameinfo(
                server, address_length, address, sizeof(address),
                NULL, 0, NI_NUMERICHOST) != 0)
            continue;
        int error = resolver_append(
                output, capacity, &length,
                "nameserver %s\n", address);
        if (error < 0)
            return error;
        nameserver_count++;
    }

    if (nameserver_count == 0)
        return _ENODATA;
    return (ssize_t) length;
}

static int write_guest_resolv_conf(
        struct task *task, const char *contents, size_t length) {
    struct fd *fd = generic_openat_task(
            task, AT_PWD, "/etc/resolv.conf",
            O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0644);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);

    int result = 0;
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = file_write_fd(
                fd, contents + offset, length - offset);
        if (written < 0) {
            result = (int) written;
            break;
        }
        if (written == 0) {
            result = _EIO;
            break;
        }
        offset += (size_t) written;
    }
    int close_error = fd_close(fd);
    if (result == 0 && close_error < 0)
        result = close_error;
    return result;
}

static int snapshot_guest_io_task(
        dword_t pid, struct task *snapshot) {
    lock(&pids_lock);
    struct task *task = pid_get_process_task(pid);
    if (task == NULL || task->fs == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    snapshot->pid = task->pid;
    snapshot->uid = task->uid;
    snapshot->gid = task->gid;
    snapshot->euid = task->euid;
    snapshot->egid = task->egid;
    snapshot->suid = task->suid;
    snapshot->sgid = task->sgid;
    snapshot->fs = task->fs;
    fs_info_retain(snapshot->fs);
    unlock(&pids_lock);
    return 0;
}

int ish_apple_guest_configure_dns_pid(dword_t pid) {
    if (pid == 0)
        return _EINVAL;

    struct __res_state resolver = {0};
    if (res_ninit(&resolver) != 0)
        return _EIO;

    union res_sockaddr_union resolver_servers[
            APPLE_RESOLVER_SERVER_CAPACITY] = {0};
    int found = res_getservers(
            &resolver, resolver_servers,
            APPLE_RESOLVER_SERVER_CAPACITY);
    if (found < 0) {
        res_ndestroy(&resolver);
        return _EIO;
    }
    if (found > APPLE_RESOLVER_SERVER_CAPACITY)
        found = APPLE_RESOLVER_SERVER_CAPACITY;

    const struct sockaddr *servers[APPLE_RESOLVER_SERVER_CAPACITY];
    for (int index = 0; index < found; index++)
        servers[index] = (const struct sockaddr *) &resolver_servers[index];

    const char *search_domains[MAXDNSRCH];
    size_t search_count = 0;
    while (search_count < MAXDNSRCH &&
            resolver.dnsrch[search_count] != NULL) {
        search_domains[search_count] = resolver.dnsrch[search_count];
        search_count++;
    }

    char *contents = malloc(APPLE_RESOLV_CONF_CAPACITY);
    if (contents == NULL) {
        res_ndestroy(&resolver);
        return _ENOMEM;
    }
    ssize_t length = ish_apple_resolver_format(
            contents, APPLE_RESOLV_CONF_CAPACITY,
            search_domains, search_count,
            servers, (size_t) found);
    res_ndestroy(&resolver);
    if (length < 0) {
        free(contents);
        return (int) length;
    }

    struct task io_task = {0};
    int result = snapshot_guest_io_task(pid, &io_task);
    if (result == 0) {
        result = write_guest_resolv_conf(
                &io_task, contents, (size_t) length);
        fs_info_release(io_task.fs);
    }
    free(contents);
    return result;
}

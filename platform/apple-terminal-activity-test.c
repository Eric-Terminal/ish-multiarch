#include "platform/apple-watch-runtime.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "kernel/errno.h"

static void *produce_output(void *opaque) {
    ish_watch_session_id session = *(ish_watch_session_id *) opaque;
    for (unsigned index = 0; index < 4096; index++) {
        unsigned char byte = (unsigned char) (index % 251);
        ish_watch_runtime_test_append_session_output(session, &byte, 1);
        sched_yield();
    }
    ish_watch_runtime_test_mark_session_exited(session, 0);
    return NULL;
}

// 使用真实的通知管道和输出环，覆盖通知合并、迟订阅、退出和槽位复用。
void test_terminal_activity(void) {
    ish_watch_session_id session;
    assert(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &session) == 0);
    int32_t fd = -1;
    assert(ish_watch_session_copy_activity_fd(0, &fd) == _ESTALE && fd == -1);
    assert(ish_watch_session_copy_activity_fd(session, NULL) == _EINVAL);
    ish_watch_runtime_test_append_session_output(session, "a", 1);
    assert(ish_watch_session_copy_activity_fd(session, &fd) == 0);
    assert((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    assert((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    unsigned char bytes[64];
    assert(read(fd, bytes, sizeof(bytes)) > 0);
    uint64_t dropped = 0;
    assert(ish_watch_session_read_output(session, bytes, sizeof(bytes), &dropped) == 1);
    struct pollfd event = {.fd = fd, .events = POLLIN};
    assert(poll(&event, 1, 10) == 0);

    // 消费输出但故意不消费通知；超过管道容量仍须非阻塞且不丢 PTY 字节。
    for (int index = 0; index < 100000; index++) {
        ish_watch_runtime_test_append_session_output(session, "b", 1);
        assert(ish_watch_session_read_output(session, bytes, sizeof(bytes), &dropped) == 1);
        assert(bytes[0] == 'b' && dropped == 0);
    }
    assert(poll(&event, 1, 0) == 1);
    while (read(fd, bytes, sizeof(bytes)) > 0) {}
    assert(errno == EAGAIN);
    assert(poll(&event, 1, 10) == 0);

    ish_watch_runtime_test_append_session_output(session, "last", 4);
    ish_watch_runtime_test_mark_session_exited(session, 7 << 8);
    assert(poll(&event, 1, 0) == 1);
    assert(read(fd, bytes, sizeof(bytes)) > 0);
    assert(ish_watch_session_read_output(session, bytes, sizeof(bytes), &dropped) == 4);
    struct ish_watch_session_status status;
    assert(ish_watch_session_status(session, &status) == 0);
    assert(status.phase == ISH_WATCH_SESSION_EXITED && status.wait_status == (7 << 8));
    assert(ish_watch_session_close(session) == 0);
    while (read(fd, bytes, sizeof(bytes)) > 0) {}
    assert(read(fd, bytes, sizeof(bytes)) == 0);

    ish_watch_session_id replacement;
    assert(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &replacement) == 0);
    ish_watch_runtime_test_append_session_output(replacement, "new", 3);
    assert(read(fd, bytes, sizeof(bytes)) == 0);
    close(fd);
    ish_watch_runtime_test_mark_session_exited(replacement, 0);
    // 无输出退出也需要初始通知，不能等待永远不会出现的下一段输出。
    assert(ish_watch_session_copy_activity_fd(replacement, &fd) == 0);
    assert(read(fd, bytes, sizeof(bytes)) > 0);
    close(fd);
    assert(ish_watch_session_close(replacement) == 0);

    assert(ish_watch_runtime_test_add_session(
            ISH_WATCH_SESSION_RUNNING, &session) == 0);
    assert(ish_watch_session_copy_activity_fd(session, &fd) == 0);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, produce_output, &session) == 0);
    event.fd = fd;
    unsigned consumed = 0;
    while (consumed < 4096) {
        assert(poll(&event, 1, 1000) == 1);
        while (read(fd, bytes, sizeof(bytes)) > 0) {}
        ssize_t count;
        while ((count = ish_watch_session_read_output(
                session, bytes, sizeof(bytes), &dropped)) > 0) {
            for (ssize_t index = 0; index < count; index++)
                assert(bytes[index] == (unsigned char) (consumed++ % 251));
            assert(dropped == 0);
        }
        assert(count == 0);
    }
    assert(pthread_join(producer, NULL) == 0);
    assert(ish_watch_session_close(session) == 0);
    close(fd);
}

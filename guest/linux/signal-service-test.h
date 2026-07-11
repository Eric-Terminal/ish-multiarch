#ifndef GUEST_LINUX_SIGNAL_SERVICE_TEST_H
#define GUEST_LINUX_SIGNAL_SERVICE_TEST_H

#include "guest/linux/signal-service.h"

enum guest_linux_signal_test_mode {
    GUEST_LINUX_SIGNAL_TEST_IDLE,
    GUEST_LINUX_SIGNAL_TEST_KILL,
    GUEST_LINUX_SIGNAL_TEST_TIMER,
    GUEST_LINUX_SIGNAL_TEST_CHILD,
    GUEST_LINUX_SIGNAL_TEST_FAULT,
    GUEST_LINUX_SIGNAL_TEST_SIGSYS,
    GUEST_LINUX_SIGNAL_TEST_STOP,
    GUEST_LINUX_SIGNAL_TEST_TERMINATE,
    GUEST_LINUX_SIGNAL_TEST_FORCE_HANDLER,
    GUEST_LINUX_SIGNAL_TEST_FORCE_TERMINATE,
};

extern const struct guest_linux_signal_service
        guest_linux_signal_test_service;

void guest_linux_signal_test_configure(
        dword_t mode, void *expected_task_opaque);
dword_t guest_linux_signal_test_poll_count(void);

#endif

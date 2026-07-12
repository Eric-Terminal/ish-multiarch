#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"

struct signal_pending_account {
    struct list accounts;
    uid_t_ uid;
    size_t count;
};

static lock_t pending_accounts_lock = LOCK_INITIALIZER;
static struct list pending_accounts = LIST_INITIALIZER(pending_accounts);
static atomic_size_t allocation_index = ATOMIC_VAR_INIT(0);
static atomic_size_t failed_allocation = ATOMIC_VAR_INIT(SIZE_MAX);

void signal_queue_test_fail_allocation_at(size_t index) {
    atomic_store_explicit(&allocation_index, 0, memory_order_relaxed);
    atomic_store_explicit(&failed_allocation, index, memory_order_relaxed);
}

static void *signal_queue_malloc(size_t size) {
    size_t index = atomic_fetch_add_explicit(
            &allocation_index, 1, memory_order_relaxed);
    if (index == atomic_load_explicit(
            &failed_allocation, memory_order_relaxed))
        return NULL;
    return malloc(size);
}

static struct signal_pending_account *find_account_locked(uid_t_ uid) {
    struct signal_pending_account *account;
    list_for_each_entry(&pending_accounts, account, accounts) {
        if (account->uid == uid)
            return account;
    }
    return NULL;
}

static struct signal_pending_account *charge_pending(
        uid_t_ uid, qword_t limit, bool ignore_limit) {
    lock(&pending_accounts_lock);
    struct signal_pending_account *account = find_account_locked(uid);
    if (!ignore_limit && account != NULL && account->count >= limit) {
        unlock(&pending_accounts_lock);
        return NULL;
    }
    if (!ignore_limit && account == NULL && limit == 0) {
        unlock(&pending_accounts_lock);
        return NULL;
    }
    if (account == NULL) {
        account = signal_queue_malloc(sizeof(*account));
        if (account == NULL) {
            unlock(&pending_accounts_lock);
            return NULL;
        }
        *account = (struct signal_pending_account) {
            .uid = uid,
        };
        list_add_tail(&pending_accounts, &account->accounts);
    }
    account->count++;
    unlock(&pending_accounts_lock);
    return account;
}

static int enqueue_without_node(
        struct task *task, int signal, enum signal_queue_policy policy) {
    if (policy == SIGNAL_QUEUE_EXPLICIT)
        return _EAGAIN;
    sigset_add(&task->pending, signal);
    return SIGNAL_ENQUEUE_BIT_ONLY;
}

static bool has_queued_signal(const struct task *task, int signal) {
    struct sigqueue *queued;
    list_for_each_entry(&task->queue, queued, queue) {
        if (queued->info.sig == signal)
            return true;
    }
    return false;
}

int signal_enqueue_locked(struct task *task, int signal,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, qword_t limit) {
    assert(task != NULL && signal >= 1 && signal <= NUM_SIGS);
    // Linux 对 SIGKILL 只保留 pending 位，不分配无从观察的 siginfo。
    if (signal == SIGKILL_) {
        bool already_pending = sigset_has(task->pending, signal);
        sigset_add(&task->pending, signal);
        return already_pending ?
                SIGNAL_ENQUEUE_COALESCED : SIGNAL_ENQUEUE_BIT_ONLY;
    }
    if (signal < SIGRTMIN_ && sigset_has(task->pending, signal) &&
            (policy != SIGNAL_QUEUE_FORCE ||
                    has_queued_signal(task, signal)))
        return SIGNAL_ENQUEUE_COALESCED;

    struct sigqueue *queued = signal_queue_malloc(sizeof(*queued));
    if (queued == NULL)
        return enqueue_without_node(task, signal, policy);

    bool ignore_limit = policy == SIGNAL_QUEUE_FORCE ||
            (policy == SIGNAL_QUEUE_LEGACY && signal < SIGRTMIN_);
    struct signal_pending_account *account =
            charge_pending(uid, limit, ignore_limit);
    if (account == NULL) {
        free(queued);
        return enqueue_without_node(task, signal, policy);
    }

    *queued = (struct sigqueue) {
        .info = info,
        .account = account,
    };
    queued->info.sig = signal;
    list_add_tail(&task->queue, &queued->queue);
    sigset_add(&task->pending, signal);
    return SIGNAL_ENQUEUE_QUEUED;
}

void signal_queue_release(struct sigqueue *queued) {
    assert(queued != NULL);
    struct signal_pending_account *account = queued->account;
    if (account != NULL) {
        lock(&pending_accounts_lock);
        assert(account->count != 0);
        account->count--;
        if (account->count == 0) {
            list_remove(&account->accounts);
            free(account);
        }
        unlock(&pending_accounts_lock);
    }
    free(queued);
}

void signal_discard_pending_locked(struct task *task, int signal) {
    sigset_del(&task->pending, signal);
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(&task->queue, queued, temporary, queue) {
        if (queued->info.sig == signal) {
            list_remove(&queued->queue);
            signal_queue_release(queued);
        }
    }
}

void signal_flush_pending(struct task *task) {
    task->pending = 0;
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(&task->queue, queued, temporary, queue) {
        list_remove(&queued->queue);
        signal_queue_release(queued);
    }
}

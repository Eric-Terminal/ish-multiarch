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

void signal_group_pending_init(struct tgroup *group) {
    assert(group != NULL);
    if (list_null(&group->shared_queue))
        list_init(&group->shared_queue);
}

static int enqueue_without_node(sigset_t_ *pending,
        sigset_t_ *bit_only, sigset_t_ *timer_bit_only,
        int signal, const struct siginfo_ *info,
        enum signal_queue_policy policy) {
    if (policy == SIGNAL_QUEUE_EXPLICIT)
        return _EAGAIN;
    sigset_add(pending, signal);
    if (info->code == SI_TIMER_)
        sigset_add(timer_bit_only, signal);
    else
        sigset_add(bit_only, signal);
    return SIGNAL_ENQUEUE_BIT_ONLY;
}

static bool has_queued_signal(struct list *queue, int signal) {
    struct sigqueue *queued;
    list_for_each_entry(queue, queued, queue) {
        if (queued->info.sig == signal)
            return true;
    }
    return false;
}

static int enqueue_signal_locked(struct task *task,
        sigset_t_ *pending, struct list *queue,
        sigset_t_ *bit_only, sigset_t_ *timer_bit_only, int signal,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, qword_t limit) {
    assert(task != NULL && signal >= 1 && signal <= NUM_SIGS);
    // Linux 对 SIGKILL 只保留 pending 位，不分配无从观察的 siginfo。
    if (signal == SIGKILL_) {
        bool already_pending = sigset_has(*pending, signal);
        sigset_add(pending, signal);
        if (!already_pending) {
            if (info.code == SI_TIMER_)
                sigset_add(timer_bit_only, signal);
            else
                sigset_add(bit_only, signal);
        }
        return already_pending ?
                SIGNAL_ENQUEUE_COALESCED : SIGNAL_ENQUEUE_BIT_ONLY;
    }
    bool replace_bit_only = false;
    if (signal < SIGRTMIN_ && sigset_has(*pending, signal)) {
        bool has_node = has_queued_signal(queue, signal);
        if (policy != SIGNAL_QUEUE_FORCE || has_node)
            return SIGNAL_ENQUEUE_COALESCED;
        // FORCE 为已有普通信号补精确信息，替换而不是追加旧 fallback。
        replace_bit_only = true;
    }

    struct sigqueue *queued = signal_queue_malloc(sizeof(*queued));
    if (queued == NULL) {
        if (replace_bit_only) {
            sigset_del(bit_only, signal);
            sigset_del(timer_bit_only, signal);
        }
        return enqueue_without_node(pending, bit_only,
                timer_bit_only, signal, &info, policy);
    }

    bool ignore_limit = policy == SIGNAL_QUEUE_FORCE ||
            (policy == SIGNAL_QUEUE_LEGACY && signal < SIGRTMIN_);
    struct signal_pending_account *account =
            charge_pending(uid, limit, ignore_limit);
    if (account == NULL) {
        free(queued);
        if (replace_bit_only) {
            sigset_del(bit_only, signal);
            sigset_del(timer_bit_only, signal);
        }
        return enqueue_without_node(pending, bit_only,
                timer_bit_only, signal, &info, policy);
    }

    if (replace_bit_only) {
        sigset_del(bit_only, signal);
        sigset_del(timer_bit_only, signal);
    }

    *queued = (struct sigqueue) {
        .info = info,
        .account = account,
    };
    queued->info.sig = signal;
    list_add_tail(queue, &queued->queue);
    sigset_add(pending, signal);
    return SIGNAL_ENQUEUE_QUEUED;
}

int signal_enqueue_locked(struct task *task, int signal,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, qword_t limit) {
    return enqueue_signal_locked(task,
            &task->pending, &task->queue,
            &task->pending_bit_only, &task->pending_timer_bit_only,
            signal, info, policy, uid, limit);
}

int signal_enqueue_process_locked(struct task *representative, int signal,
        struct siginfo_ info, enum signal_queue_policy policy,
        uid_t_ uid, qword_t limit) {
    struct tgroup *group = representative->group;
    signal_group_pending_init(group);
    return enqueue_signal_locked(representative,
            &group->shared_pending, &group->shared_queue,
            &group->shared_bit_only, &group->shared_timer_bit_only,
            signal, info, policy, uid, limit);
}

sigset_t_ signal_pending_mask_locked(struct task *task) {
    assert(task != NULL);
    if (task->group == NULL)
        return task->pending;
    signal_group_pending_init(task->group);
    return task->pending | task->group->shared_pending;
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

static void discard_pending_locked(
        sigset_t_ *pending, struct list *queue,
        sigset_t_ *bit_only, sigset_t_ *timer_bit_only,
        int signal) {
    sigset_del(pending, signal);
    sigset_del(bit_only, signal);
    sigset_del(timer_bit_only, signal);
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(queue, queued, temporary, queue) {
        if (queued->info.sig == signal) {
            list_remove(&queued->queue);
            signal_queue_release(queued);
        }
    }
}

void signal_discard_pending_locked(struct task *task, int signal) {
    discard_pending_locked(&task->pending, &task->queue,
            &task->pending_bit_only, &task->pending_timer_bit_only,
            signal);
}

void signal_discard_group_pending_locked(
        struct tgroup *group, int signal) {
    signal_group_pending_init(group);
    discard_pending_locked(
            &group->shared_pending, &group->shared_queue,
            &group->shared_bit_only, &group->shared_timer_bit_only,
            signal);
}

void signal_flush_pending(struct task *task) {
    task->pending = 0;
    task->pending_bit_only = 0;
    task->pending_timer_bit_only = 0;
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(&task->queue, queued, temporary, queue) {
        list_remove(&queued->queue);
        signal_queue_release(queued);
    }
}

void signal_flush_group_pending(struct tgroup *group) {
    signal_group_pending_init(group);
    group->shared_pending = 0;
    group->shared_bit_only = 0;
    group->shared_timer_bit_only = 0;
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(
            &group->shared_queue, queued, temporary, queue) {
        list_remove(&queued->queue);
        signal_queue_release(queued);
    }
}

static void flush_timer_queue_locked(
        sigset_t_ *pending, struct list *queue,
        sigset_t_ *bit_only, sigset_t_ *timer_bit_only) {
    sigset_t_ removed_signals = *timer_bit_only;
    sigset_t_ remaining_signals = 0;
    *timer_bit_only = 0;
    struct sigqueue *queued, *temporary;
    list_for_each_entry_safe(queue, queued, temporary, queue) {
        if (queued->info.code != SI_TIMER_) {
            sigset_add(&remaining_signals, queued->info.sig);
            continue;
        }
        sigset_add(&removed_signals, queued->info.sig);
        list_remove(&queued->queue);
        signal_queue_release(queued);
    }
    // 同号的非 timer 节点或 bit-only 来源仍需保留 pending。
    *pending &= ~(removed_signals & ~(remaining_signals | *bit_only));
}

void signal_flush_exec_timer_pending(struct task *task) {
    assert(task != NULL && task->sighand != NULL);
    lock(&task->sighand->lock);
    flush_timer_queue_locked(&task->pending, &task->queue,
            &task->pending_bit_only, &task->pending_timer_bit_only);
    if (task->group != NULL) {
        signal_group_pending_init(task->group);
        flush_timer_queue_locked(&task->group->shared_pending,
                &task->group->shared_queue,
                &task->group->shared_bit_only,
                &task->group->shared_timer_bit_only);
    }
    unlock(&task->sighand->lock);
}

#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "kernel/calls.h"
#include "guest/linux/futex-abi.h"
#include "guest/aarch64/linux-futex-abi.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-time-abi.h"
#include "guest/memory/address-space.h"
#include "kernel/futex.h"
#include "util/timer.h"

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3
#define FUTEX_PRIVATE_FLAG_ UINT32_C(128)
#define FUTEX_CMD_MASK_ (~FUTEX_PRIVATE_FLAG_)

enum futex_key_kind {
    FUTEX_KEY_I386_POINTER,
    FUTEX_KEY_AARCH64_VIRTUAL,
    FUTEX_KEY_AARCH64_PHYSICAL,
};

struct futex_key {
    enum futex_key_kind kind;
    union {
        struct {
            const void *memory_identity;
            addr_t address;
        } i386;
        struct {
            qword_t memory_identity;
            qword_t address;
        } aarch64_virtual;
        struct {
            qword_t shared_identity;
            qword_t page_offset;
        } aarch64_physical;
    } identity;
};

struct futex {
    // 引用计数、队列和哈希链均由 futex_lock 保护。
    unsigned refcount;
    struct futex_key key;
    struct list queue;
    struct list chain;
};

struct futex_wait {
    cond_t cond;
    // REQUEUE 会在持有 futex_lock 时转移这份等待者引用。
    struct futex *futex;
    struct list queue;
};

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];
static atomic_size_t futex_allocation_index = ATOMIC_VAR_INIT(0);
static atomic_size_t futex_failed_allocation = ATOMIC_VAR_INIT(SIZE_MAX);
static atomic_uint futex_live_count = ATOMIC_VAR_INIT(0);

static void __attribute__((constructor)) init_futex_hash(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

void futex_test_fail_allocation_at(size_t index) {
    atomic_store_explicit(
            &futex_allocation_index, 0, memory_order_relaxed);
    atomic_store_explicit(
            &futex_failed_allocation, index, memory_order_relaxed);
}

unsigned futex_test_live_count(void) {
    return atomic_load_explicit(
            &futex_live_count, memory_order_relaxed);
}

static void *futex_malloc(size_t size) {
    size_t index = atomic_fetch_add_explicit(
            &futex_allocation_index, 1, memory_order_relaxed);
    size_t failed = atomic_load_explicit(
            &futex_failed_allocation, memory_order_relaxed);
    if (failed != SIZE_MAX && index == failed)
        return NULL;
    return malloc(size);
}

static struct futex_key i386_futex_key_for_memory(
        const struct mem *memory, addr_t addr) {
    return (struct futex_key) {
        .kind = FUTEX_KEY_I386_POINTER,
        .identity.i386 = {
            .memory_identity = memory,
            .address = addr,
        },
    };
}

static struct futex_key i386_futex_key(addr_t addr) {
    return i386_futex_key_for_memory(current->mem, addr);
}

static struct futex_key aarch64_virtual_futex_key(
        qword_t memory_identity, qword_t address) {
    return (struct futex_key) {
        .kind = FUTEX_KEY_AARCH64_VIRTUAL,
        .identity.aarch64_virtual = {
            .memory_identity = memory_identity,
            .address = address,
        },
    };
}

static struct futex_key aarch64_physical_futex_key(
        qword_t shared_identity, qword_t page_offset) {
    assert(shared_identity != 0);
    return (struct futex_key) {
        .kind = FUTEX_KEY_AARCH64_PHYSICAL,
        .identity.aarch64_physical = {
            .shared_identity = shared_identity,
            .page_offset = page_offset,
        },
    };
}

static bool futex_keys_equal(
        const struct futex_key *left, const struct futex_key *right) {
    if (left->kind != right->kind)
        return false;
    switch (left->kind) {
        case FUTEX_KEY_I386_POINTER:
            return left->identity.i386.memory_identity ==
                            right->identity.i386.memory_identity &&
                    left->identity.i386.address ==
                            right->identity.i386.address;
        case FUTEX_KEY_AARCH64_VIRTUAL:
            return left->identity.aarch64_virtual.memory_identity ==
                            right->identity.aarch64_virtual.memory_identity &&
                    left->identity.aarch64_virtual.address ==
                            right->identity.aarch64_virtual.address;
        case FUTEX_KEY_AARCH64_PHYSICAL:
            return left->identity.aarch64_physical.shared_identity ==
                            right->identity.aarch64_physical.shared_identity &&
                    left->identity.aarch64_physical.page_offset ==
                            right->identity.aarch64_physical.page_offset;
    }
    assert(false);
    return false;
}

static dword_t fold_qword(qword_t value) {
    return (dword_t) value ^ (dword_t) (value >> 32);
}

static dword_t fold_pointer(const void *pointer) {
    uintptr_t value = (uintptr_t) pointer;
    dword_t folded = (dword_t) value;
#if UINTPTR_MAX > UINT32_MAX
    folded ^= (dword_t) (value >> 32);
#endif
    return folded;
}

static dword_t mix_hash_word(dword_t hash, dword_t value) {
    return hash ^ (value + UINT32_C(0x9e3779b9) +
            (hash << 6) + (hash >> 2));
}

static size_t futex_hash_index(const struct futex_key *key) {
    dword_t mixed = mix_hash_word(
            UINT32_C(0x811c9dc5), (dword_t) key->kind);
    switch (key->kind) {
        case FUTEX_KEY_I386_POINTER:
            mixed = mix_hash_word(mixed, fold_pointer(
                    key->identity.i386.memory_identity));
            mixed = mix_hash_word(
                    mixed, key->identity.i386.address);
            break;
        case FUTEX_KEY_AARCH64_VIRTUAL:
            mixed = mix_hash_word(mixed, fold_qword(
                    key->identity.aarch64_virtual.memory_identity));
            mixed = mix_hash_word(mixed, fold_qword(
                    key->identity.aarch64_virtual.address));
            break;
        case FUTEX_KEY_AARCH64_PHYSICAL:
            mixed = mix_hash_word(mixed, fold_qword(
                    key->identity.aarch64_physical.shared_identity));
            mixed = mix_hash_word(mixed, fold_qword(
                    key->identity.aarch64_physical.page_offset));
            break;
        default:
            assert(false);
    }
    return (size_t) (mixed & (FUTEX_HASH_SIZE - 1));
}

static void futex_retain_unlocked(struct futex *futex) {
    assert(futex != NULL && futex->refcount != UINT_MAX);
    futex->refcount++;
}

// 只查找并持有现有对象；空 WAKE 等路径不得在这里分配。
static struct futex *futex_find_unlocked(const struct futex_key *key) {
    struct list *bucket = &futex_hash[futex_hash_index(key)];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (futex_keys_equal(&futex->key, key)) {
            futex_retain_unlocked(futex);
            return futex;
        }
    }
    return NULL;
}

static struct futex *futex_get_or_create_unlocked(
        const struct futex_key *key) {
    struct futex *futex = futex_find_unlocked(key);
    if (futex != NULL)
        return futex;

    futex = futex_malloc(sizeof(*futex));
    if (futex == NULL)
        return NULL;
    *futex = (struct futex) {
        .refcount = 1,
        .key = *key,
    };
    list_init(&futex->queue);
    list_add(&futex_hash[futex_hash_index(key)], &futex->chain);
    unsigned previous = atomic_fetch_add_explicit(
            &futex_live_count, 1, memory_order_relaxed);
    assert(previous != UINT_MAX);
    return futex;
}

static void futex_put_unlocked(struct futex *futex) {
    assert(futex != NULL && futex->refcount != 0);
    if (--futex->refcount == 0) {
        assert(list_empty(&futex->queue));
        list_remove(&futex->chain);
        unsigned previous = atomic_fetch_sub_explicit(
                &futex_live_count, 1, memory_order_relaxed);
        assert(previous != 0);
        free(futex);
    }
}

static bool i386_futex_load(addr_t address, dword_t *value) {
    read_wrlock(&current->mem->lock);
    dword_t *ptr = mem_ptr(
            current->mem, address, MEM_READ);
    if (ptr != NULL)
        *value = *ptr;
    read_wrunlock(&current->mem->lock);
    return ptr != NULL;
}

// 调用和返回时均持有 futex_lock；wait_for 会在睡眠期间暂时释放它。
static int futex_wait_locked(const struct futex_key *key,
        dword_t expected, dword_t observed,
        struct timespec *timeout,
        const struct timer_time *deadline) {
    if (observed != expected)
        return _EAGAIN;

    struct futex *futex = futex_get_or_create_unlocked(key);
    if (futex == NULL)
        return _ENOMEM;
    int err = 0;
    struct futex_wait wait = {
        .futex = futex,
    };
    cond_init(&wait.cond);
    list_add_tail(&futex->queue, &wait.queue);
    if (deadline == NULL) {
        err = wait_for(&wait.cond, &futex_lock, timeout);
    } else {
        do {
            struct timespec slice;
            struct timer_time now = timer_time_from_timespec(
                    timespec_now(CLOCK_MONOTONIC));
            if (!timer_time_deadline_slice(
                    *deadline, now, &slice)) {
                err = _ETIMEDOUT;
                break;
            }
            err = wait_for(&wait.cond, &futex_lock, &slice);
        } while (err == _ETIMEDOUT);
    }
    futex = wait.futex;
    list_remove_safe(&wait.queue);
    futex_put_unlocked(futex);
    cond_destroy(&wait.cond);
    return err;
}

static dword_t wake_waiters_locked(
        struct futex *futex, dword_t wake_max) {
    struct futex_wait *wait, *temporary;
    dword_t woken = 0;
    list_for_each_entry_safe(
            &futex->queue, wait, temporary, queue) {
        if (woken == wake_max)
            break;
        notify(&wait->cond);
        list_remove(&wait->queue);
        woken++;
    }
    return woken;
}

static int wake_futex_locked(
        const struct futex_key *key, dword_t wake_max) {
    struct futex *futex = futex_find_unlocked(key);
    if (futex == NULL)
        return 0;
    dword_t woken = wake_waiters_locked(futex, wake_max);
    futex_put_unlocked(futex);
    assert((qword_t) woken <= (qword_t) INT_MAX);
    return (int) woken;
}

static dword_t count_requeue_candidates_locked(
        const struct futex *source, dword_t wake_max,
        dword_t requeue_max) {
    dword_t skipped = 0;
    dword_t candidates = 0;
    struct futex_wait *wait;
    list_for_each_entry(&source->queue, wait, queue) {
        if (skipped != wake_max) {
            skipped++;
        } else if (candidates != requeue_max) {
            candidates++;
        } else {
            break;
        }
    }
    return candidates;
}

static int requeue_futex_locked(const struct futex_key *source_key,
        dword_t wake_max, dword_t requeue_max,
        const struct futex_key *target_key) {
    struct futex *source = futex_find_unlocked(source_key);
    if (source == NULL)
        return 0;

    bool same_key = futex_keys_equal(source_key, target_key);
    dword_t migration_count = count_requeue_candidates_locked(
            source, wake_max, requeue_max);
    struct futex *target = NULL;
    if (!same_key && migration_count != 0) {
        // 目标对象必须在任何唤醒前准备完毕，确保 OOM 零副作用。
        target = futex_get_or_create_unlocked(target_key);
        if (target == NULL) {
            futex_put_unlocked(source);
            return _ENOMEM;
        }
        assert(target != source);
    }

    dword_t woken = wake_waiters_locked(source, wake_max);
    dword_t requeued = 0;
    if (same_key) {
        // 同键 REQUEUE 只报告仍在队列中的匹配数量，不转移节点或引用。
        struct futex_wait *wait;
        list_for_each_entry(&source->queue, wait, queue) {
            if (requeued == requeue_max)
                break;
            requeued++;
        }
    } else if (migration_count != 0) {
        struct futex_wait *wait, *temporary;
        list_for_each_entry_safe(
                &source->queue, wait, temporary, queue) {
            if (requeued == migration_count)
                break;
            list_remove(&wait->queue);
            list_add_tail(&target->queue, &wait->queue);
            assert(source->refcount > 1);
            source->refcount--;
            futex_retain_unlocked(target);
            wait->futex = target;
            requeued++;
        }
        assert(requeued == migration_count);
    }

    if (target != NULL)
        futex_put_unlocked(target);
    futex_put_unlocked(source);
    qword_t total = (qword_t) woken + (qword_t) requeued;
    assert(total <= (qword_t) INT_MAX);
    return (int) total;
}

static bool snapshot_aarch64_futex_keys_locked(
        struct aarch64_linux_process *process,
        const qword_t *addresses, size_t count,
        struct futex_key *keys, dword_t *first_value,
        struct guest_linux_user_fault *fault) {
    assert(process != NULL && addresses != NULL &&
            count != 0 && count <= 2 && keys != NULL);
    struct aarch64_linux_futex_word_snapshot snapshots[2];
    if (!aarch64_linux_process_snapshot_futex_words(
            process, addresses, count, snapshots, first_value, fault))
        return false;

    qword_t memory_identity =
            aarch64_linux_process_memory_identity(process);
    for (size_t index = 0; index < count; index++) {
        if (snapshots[index].shared_identity != 0) {
            keys[index] = aarch64_physical_futex_key(
                    snapshots[index].shared_identity,
                    snapshots[index].page_offset);
        } else {
            keys[index] = aarch64_virtual_futex_key(
                    memory_identity, addresses[index]);
        }
    }
    return true;
}

static struct futex_key aarch64_futex_key_from_snapshot(
        struct aarch64_linux_process *process, qword_t address,
        const struct aarch64_linux_futex_word_snapshot *snapshot) {
    if (snapshot->shared_identity != 0) {
        return aarch64_physical_futex_key(
                snapshot->shared_identity, snapshot->page_offset);
    }
    return aarch64_virtual_futex_key(
            aarch64_linux_process_memory_identity(process), address);
}

static bool aarch64_futex_address_is_in_range(
        struct aarch64_linux_process *process, qword_t address,
        struct guest_linux_user_fault *fault) {
    if (aarch64_linux_process_contains_user_range(
            process, address, sizeof(dword_t)))
        return true;
    if (fault != NULL) {
        *fault = (struct guest_linux_user_fault) {
            .address = address,
            .access = GUEST_MEMORY_READ,
            .kind = GUEST_MEMORY_FAULT_ADDRESS_SIZE,
        };
    }
    return false;
}

static int wait_i386_futex(struct futex_key key, dword_t expected,
        struct timespec *timeout) {
    lock(&futex_lock);
    dword_t observed;
    int result = i386_futex_load(
            key.identity.i386.address, &observed) ?
            futex_wait_locked(
                    &key, expected, observed, timeout, NULL) : _EFAULT;
    unlock(&futex_lock);
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return result;
}

static int wait_aarch64_futex(
        struct aarch64_linux_process *process, qword_t address,
        bool private_mapping, dword_t expected,
        const struct timer_time *deadline,
        struct guest_linux_user_fault *fault) {
    lock(&futex_lock);
    struct futex_key key;
    dword_t observed;
    bool loaded;
    if (private_mapping) {
        key = aarch64_virtual_futex_key(
                aarch64_linux_process_memory_identity(process), address);
        loaded = aarch64_linux_process_read_u32(
                process, address, &observed, fault);
    } else {
        qword_t addresses[] = {address};
        loaded = snapshot_aarch64_futex_keys_locked(
                process, addresses, 1, &key, &observed, fault);
    }
    int result = loaded ? futex_wait_locked(
            &key, expected, observed, NULL, deadline) : _EFAULT;
    unlock(&futex_lock);
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return result;
}

static int wake_aarch64_futex(
        struct aarch64_linux_process *process, qword_t address,
        bool private_mapping, dword_t wake_max,
        struct guest_linux_user_fault *fault) {
    if (private_mapping && !aarch64_futex_address_is_in_range(
            process, address, fault))
        return _EFAULT;
    lock(&futex_lock);
    struct futex_key key;
    int result;
    if (private_mapping) {
        key = aarch64_virtual_futex_key(
                aarch64_linux_process_memory_identity(process), address);
        result = wake_futex_locked(&key, wake_max);
    } else {
        qword_t addresses[] = {address};
        result = snapshot_aarch64_futex_keys_locked(
                process, addresses, 1, &key, NULL, fault) ?
                wake_futex_locked(&key, wake_max) : _EFAULT;
    }
    unlock(&futex_lock);
    return result;
}

static int requeue_aarch64_futex(
        struct aarch64_linux_process *process,
        qword_t source_address, qword_t target_address,
        bool private_mapping, dword_t wake_max, dword_t requeue_max,
        struct guest_linux_user_fault *fault) {
    if (private_mapping &&
            (!aarch64_futex_address_is_in_range(
                    process, source_address, fault) ||
            !aarch64_futex_address_is_in_range(
                    process, target_address, fault)))
        return _EFAULT;
    lock(&futex_lock);
    struct futex_key keys[2];
    int result;
    if (private_mapping) {
        qword_t memory_identity =
                aarch64_linux_process_memory_identity(process);
        keys[0] = aarch64_virtual_futex_key(
                memory_identity, source_address);
        keys[1] = aarch64_virtual_futex_key(
                memory_identity, target_address);
        result = requeue_futex_locked(
                &keys[0], wake_max, requeue_max, &keys[1]);
    } else {
        const qword_t addresses[] = {
            source_address,
            target_address,
        };
        result = snapshot_aarch64_futex_keys_locked(
                process, addresses, 2, keys, NULL, fault) ?
                requeue_futex_locked(
                        &keys[0], wake_max,
                        requeue_max, &keys[1]) : _EFAULT;
    }
    unlock(&futex_lock);
    return result;
}

int futex_wake(addr_t uaddr, dword_t wake_max) {
    struct futex_key key = i386_futex_key(uaddr);
    lock(&futex_lock);
    int result = wake_futex_locked(&key, wake_max);
    unlock(&futex_lock);
    return result;
}

int futex_wake_aarch64(struct aarch64_linux_process *process,
        qword_t uaddr, dword_t wake_max) {
    assert(process != NULL);
    if ((uaddr & (sizeof(dword_t) - 1)) != 0)
        return _EINVAL;
    struct guest_linux_user_fault fault;
    // clear-child-tid 使用共享语义；私有页会自然退回虚拟键。
    return wake_aarch64_futex(
            process, uaddr, false, wake_max, &fault);
}

dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    if (!(op & FUTEX_PRIVATE_FLAG_)) {
        STRACE("!FUTEX_PRIVATE ");
    }
    struct timespec timeout = {0};
    if ((op & FUTEX_CMD_MASK_) == FUTEX_WAIT_ && timeout_or_val2) {
        struct timespec_ timeout_;
        if (user_get(timeout_or_val2, timeout_))
            return _EFAULT;
        timeout.tv_sec = timeout_.sec;
        timeout.tv_nsec = timeout_.nsec;
    }
    switch (op & FUTEX_CMD_MASK_) {
        case FUTEX_WAIT_:
            STRACE("futex(FUTEX_WAIT, %#x, %d, 0x%x {%ds %dns}) = ...\n", uaddr, val, timeout_or_val2, timeout.tv_sec, timeout.tv_nsec);
            return wait_i386_futex(i386_futex_key(uaddr), val,
                    timeout_or_val2 ? &timeout : NULL);
        case FUTEX_WAKE_:
            STRACE("futex(FUTEX_WAKE, %#x, %d)", uaddr, val);
            return futex_wake(uaddr, val);
        case FUTEX_REQUEUE_:
            STRACE("futex(FUTEX_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            lock(&futex_lock);
            struct futex_key source_key = i386_futex_key(uaddr);
            struct futex_key target_key = i386_futex_key(uaddr2);
            int result = requeue_futex_locked(
                    &source_key, val, timeout_or_val2, &target_key);
            unlock(&futex_lock);
            return result;
    }
    STRACE("futex(%#x, %d, %d, timeout=%#x, %#x, %d) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
    FIXME("unsupported futex operation %d", op);
    return _ENOSYS;
}

dword_t sys_futex_aarch64(qword_t uaddr, dword_t op, dword_t val,
        qword_t timeout_or_val2, qword_t uaddr2, dword_t val3,
        struct guest_linux_user_fault *fault) {
    struct aarch64_linux_process *process = current->aarch64_process;
    assert(process != NULL);
    dword_t command = op & FUTEX_CMD_MASK_;
    bool private_mapping = (op & FUTEX_PRIVATE_FLAG_) != 0;
    use(val3);
    if (command != FUTEX_WAIT_ && command != FUTEX_WAKE_ &&
            command != FUTEX_REQUEUE_)
        return _ENOSYS;
    if ((uaddr & (sizeof(dword_t) - 1)) != 0)
        return _EINVAL;
    if (command == FUTEX_REQUEUE_ &&
            (uaddr2 & (sizeof(dword_t) - 1)) != 0)
        return _EINVAL;
    struct timer_time deadline;
    const struct timer_time *deadline_pointer = NULL;
    if (command == FUTEX_WAIT_ && timeout_or_val2 != 0) {
        struct aarch64_linux_timespec wire;
        if (!aarch64_linux_process_read_memory(process,
                timeout_or_val2, &wire, sizeof(wire), fault))
            return _EFAULT;
        if (wire.sec < 0 ||
                wire.nsec < 0 || wire.nsec >= 1000000000)
            return _EINVAL;
        deadline = timer_time_add(
                timer_time_from_timespec(
                        timespec_now(CLOCK_MONOTONIC)),
                (struct timer_time) {wire.sec, wire.nsec});
        deadline_pointer = &deadline;
    }
    switch (command) {
        case FUTEX_WAIT_:
            return wait_aarch64_futex(process, uaddr,
                    private_mapping, val, deadline_pointer, fault);
        case FUTEX_WAKE_:
            return wake_aarch64_futex(process, uaddr,
                    private_mapping, val, fault);
        case FUTEX_REQUEUE_:
            return requeue_aarch64_futex(process, uaddr, uaddr2,
                    private_mapping, val,
                    (dword_t) timeout_or_val2, fault);
    }
    return _ENOSYS;
}

int_t sys_set_robust_list_aarch64(qword_t robust_list, qword_t len) {
    assert(current != NULL);
    if (len != sizeof(struct aarch64_linux_robust_list_head))
        return _EINVAL;
    lock(&pids_lock);
    current->aarch64_robust_list = robust_list;
    unlock(&pids_lock);
    return 0;
}

int_t sys_get_robust_list_aarch64(
        pid_t_ pid, qword_t *robust_list) {
    assert(current != NULL && robust_list != NULL);
    lock(&pids_lock);
    struct task *task = pid == 0 || pid == current->pid ?
            current : pid_get_task((dword_t) pid);
    int_t result = 0;
    if (task == NULL) {
        result = _ESRCH;
    } else if (task != current) {
        // AArch64 ptrace 尚未提供 READ_REALCREDS 等价检查，保守拒绝跨任务读取。
        result = _EPERM;
    } else {
        *robust_list = task->aarch64_robust_list;
    }
    unlock(&pids_lock);
    return result;
}

static qword_t take_aarch64_robust_list(struct task *task) {
    lock(&pids_lock);
    qword_t robust_list = task->aarch64_robust_list;
    task->aarch64_robust_list = 0;
    unlock(&pids_lock);
    return robust_list;
}

static bool add_robust_futex_offset(
        qword_t entry, sqword_t offset, qword_t *address) {
    if (offset >= 0) {
        qword_t positive = (qword_t) offset;
        if (entry > UINT64_MAX - positive)
            return false;
        *address = entry + positive;
        return true;
    }
    qword_t magnitude = (qword_t) (-(offset + 1)) + 1;
    if (entry < magnitude)
        return false;
    *address = entry - magnitude;
    return true;
}

static bool cleanup_robust_futex_aarch64(
        struct aarch64_linux_process *process, qword_t entry,
        sqword_t offset, pid_t_ tid, bool pi, bool pending) {
    qword_t address;
    if (!add_robust_futex_offset(entry, offset, &address) ||
            (address & (sizeof(dword_t) - 1)) != 0 ||
            !aarch64_linux_process_contains_user_range(
                    process, address, sizeof(dword_t)))
        return false;

    lock(&futex_lock);
    const qword_t addresses[] = {address};
    struct aarch64_linux_futex_word_snapshot snapshot;
    dword_t observed;
    if (!aarch64_linux_process_snapshot_futex_words(
            process, addresses, 1, &snapshot, &observed, NULL)) {
        unlock(&futex_lock);
        return false;
    }

    for (;;) {
        struct futex_key key = aarch64_futex_key_from_snapshot(
                process, address, &snapshot);
        dword_t owner = observed & AARCH64_LINUX_FUTEX_TID_MASK;
        if (pending && !pi && owner == 0) {
            (void) wake_futex_locked(&key, 1);
            unlock(&futex_lock);
            return true;
        }
        if (owner != (dword_t) tid) {
            unlock(&futex_lock);
            return true;
        }

        dword_t replacement =
                (observed & AARCH64_LINUX_FUTEX_WAITERS) |
                AARCH64_LINUX_FUTEX_OWNER_DIED;
        enum aarch64_linux_process_compare_exchange_result result =
                aarch64_linux_process_compare_exchange_futex_u32(
                        process, address, observed, replacement,
                        &observed, &snapshot, NULL);
        if (result == AARCH64_LINUX_PROCESS_COMPARE_EXCHANGE_FAULT) {
            unlock(&futex_lock);
            return false;
        }
        if (result == AARCH64_LINUX_PROCESS_COMPARE_EXCHANGE_MISMATCH)
            continue;

        if (!pi && (observed & AARCH64_LINUX_FUTEX_WAITERS) != 0) {
            key = aarch64_futex_key_from_snapshot(
                    process, address, &snapshot);
            (void) wake_futex_locked(&key, 1);
        }
        unlock(&futex_lock);
        return true;
    }
}

static qword_t robust_entry_address(qword_t pointer) {
    return pointer & AARCH64_LINUX_ROBUST_LIST_ADDRESS_MASK;
}

static bool robust_entry_is_pi(qword_t pointer) {
    return (pointer & AARCH64_LINUX_ROBUST_LIST_PI) != 0;
}

void futex_cleanup_robust_list_aarch64(
        struct task *task, struct aarch64_linux_process *process) {
    assert(task != NULL);
    qword_t head_address = take_aarch64_robust_list(task);
    if (head_address == 0 || process == NULL)
        return;

    struct aarch64_linux_robust_list_head head;
    if (!aarch64_linux_process_read_memory(process,
            head_address, &head, sizeof(head), NULL))
        return;

    qword_t entry = robust_entry_address(head.next);
    bool entry_pi = robust_entry_is_pi(head.next);
    qword_t pending = robust_entry_address(head.list_op_pending);
    bool pending_pi = robust_entry_is_pi(head.list_op_pending);
    for (dword_t count = 0;
            entry != head_address &&
            count < AARCH64_LINUX_ROBUST_LIST_LIMIT;
            count++) {
        qword_t next_pointer;
        bool next_loaded = aarch64_linux_process_read_memory(process,
                entry, &next_pointer, sizeof(next_pointer), NULL);
        if (entry != pending && !cleanup_robust_futex_aarch64(
                process, entry, head.futex_offset,
                task->pid, entry_pi, false))
            return;
        // Linux 会先修复当前节点，再以预先读取的 next 故障终止遍历。
        if (!next_loaded)
            return;
        entry = robust_entry_address(next_pointer);
        entry_pi = robust_entry_is_pi(next_pointer);
    }

    if (pending != 0) {
        (void) cleanup_robust_futex_aarch64(
                process, pending, head.futex_offset,
                task->pid, pending_pi, true);
    }
}

void futex_cleanup_task_aarch64(
        struct task *task, struct aarch64_linux_process *process) {
    futex_cleanup_robust_list_aarch64(task, process);
    if (process == NULL)
        return;
    assert(task->mm != NULL);

    // 活动旧 mm 只由 CLONE_VM task 共同持有；exec 候选使用独立 mm。
    // opaque wrapper 还包含构造期与观察者引用，不能代替 task 用户数。
    bool has_other_mm_user = atomic_load_explicit(
            &task->mm->refcount, memory_order_relaxed) > 1;
    qword_t clear_tid =
            aarch64_linux_process_take_clear_child_tid(process);
    if (clear_tid == 0 || !has_other_mm_user)
        return;

    (void) aarch64_linux_process_write_u32(
            process, clear_tid, 0, NULL);
    // Linux 即使清零故障也会用共享键尝试唤醒，两个地址翻译彼此独立。
    (void) futex_wake_aarch64(process, clear_tid, 1);
}

static addr_t i386_robust_entry_address(dword_t pointer) {
    return pointer & I386_LINUX_ROBUST_LIST_ADDRESS_MASK;
}

static bool i386_robust_entry_is_pi(dword_t pointer) {
    return (pointer & I386_LINUX_ROBUST_LIST_PI) != 0;
}

static bool cleanup_robust_futex_i386(struct task *task,
        addr_t entry, sdword_t offset, bool pi, bool pending) {
    // compat Linux 先按 32 位模加法计算地址，负偏移与边界回绕都保留。
    addr_t address = entry + (dword_t) offset;
    if ((address & (sizeof(dword_t) - 1)) != 0)
        return false;

    struct futex_key key =
            i386_futex_key_for_memory(task->mem, address);
    lock(&futex_lock);
    read_wrlock(&task->mem->lock);
    void *raw_pointer = mem_ptr(
            task->mem, address, MEM_READ);
    if (raw_pointer == NULL ||
            (uintptr_t) raw_pointer % _Alignof(dword_t) != 0) {
        read_wrunlock(&task->mem->lock);
        unlock(&futex_lock);
        return false;
    }
    dword_t *pointer = raw_pointer;
    dword_t observed = __atomic_load_n(pointer, __ATOMIC_SEQ_CST);
    read_wrunlock(&task->mem->lock);
    for (;;) {
        dword_t owner = observed & GUEST_LINUX_FUTEX_TID_MASK;
        if (pending && !pi && owner == 0) {
            (void) wake_futex_locked(&key, 1);
            break;
        }
        if (owner != (dword_t) task->pid)
            break;

        dword_t replacement =
                (observed & GUEST_LINUX_FUTEX_WAITERS) |
                GUEST_LINUX_FUTEX_OWNER_DIED;
        dword_t next_observed = observed;
        enum mem_compare_exchange_result result =
                mem_compare_exchange_u32(task->mem,
                        address, observed, replacement,
                        &next_observed);
        if (result == MEM_COMPARE_EXCHANGE_FAULT) {
            unlock(&futex_lock);
            return false;
        }
        if (result == MEM_COMPARE_EXCHANGE_MISMATCH) {
            observed = next_observed;
            continue;
        }
        if (!pi && (observed & GUEST_LINUX_FUTEX_WAITERS) != 0)
            (void) wake_futex_locked(&key, 1);
        break;
    }

    unlock(&futex_lock);
    return true;
}

static void cleanup_robust_list_i386(struct task *task) {
    addr_t head_address = task->robust_list;
    task->robust_list = 0;
    if (head_address == 0)
        return;

    struct i386_linux_robust_list_head head;
    if (user_read_task(task, head_address, &head, sizeof(head)))
        return;

    addr_t entry = i386_robust_entry_address(head.next);
    bool entry_pi = i386_robust_entry_is_pi(head.next);
    addr_t pending = i386_robust_entry_address(
            head.list_op_pending);
    bool pending_pi = i386_robust_entry_is_pi(
            head.list_op_pending);
    for (dword_t count = 0;
            entry != head_address &&
            count < GUEST_LINUX_ROBUST_LIST_LIMIT;
            count++) {
        dword_t next_pointer;
        bool next_loaded = user_read_task(task, entry,
                &next_pointer, sizeof(next_pointer)) == 0;
        if (entry != pending && !cleanup_robust_futex_i386(
                task, entry, head.futex_offset,
                entry_pi, false))
            return;
        // Linux 会先修复当前节点，再以预先读取的 next 故障终止遍历。
        if (!next_loaded)
            return;
        entry = i386_robust_entry_address(next_pointer);
        entry_pi = i386_robust_entry_is_pi(next_pointer);
    }

    if (pending != 0) {
        (void) cleanup_robust_futex_i386(task, pending,
                head.futex_offset, pending_pi, true);
    }
}

void futex_cleanup_task_i386(struct task *task) {
    assert(task != NULL && task == current &&
            task->mm != NULL && task->mem != NULL);
    cleanup_robust_list_i386(task);

    bool has_other_mm_user = atomic_load_explicit(
            &task->mm->refcount, memory_order_relaxed) > 1;
    addr_t clear_tid = task->clear_tid;
    task->clear_tid = 0;
    if (clear_tid == 0 || !has_other_mm_user)
        return;

    pid_t_ zero = 0;
    (void) user_put_task(task, clear_tid, zero);
    // Linux 即使清零故障也会继续尝试唤醒；注册在此之前已经消费。
    struct futex_key key =
            i386_futex_key_for_memory(task->mem, clear_tid);
    lock(&futex_lock);
    (void) wake_futex_locked(&key, 1);
    unlock(&futex_lock);
}

int_t sys_set_robust_list(addr_t robust_list, dword_t len) {
    STRACE("set_robust_list(%#x, %d)", robust_list, len);
    if (len != sizeof(struct i386_linux_robust_list_head))
        return _EINVAL;
    current->robust_list = robust_list;
    return 0;
}

int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr) {
    STRACE("get_robust_list(%d, %#x, %#x)", pid, robust_list_ptr, len_ptr);

    lock(&pids_lock);
    struct task *task = pid == 0 || pid == current->pid ?
            current : pid_get_task((dword_t) pid);
    int_t lookup_result = task == NULL ? _ESRCH :
            task != current ? _EPERM : 0;
    unlock(&pids_lock);
    if (lookup_result != 0)
        return lookup_result;

    dword_t robust_list_length =
            (dword_t) sizeof(struct i386_linux_robust_list_head);
    if (user_put(len_ptr, robust_list_length))
        return _EFAULT;
    if (user_put(robust_list_ptr, current->robust_list))
        return _EFAULT;
    return 0;
}

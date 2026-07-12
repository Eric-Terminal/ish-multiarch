#include "kernel/calls.h"
#include "guest/aarch64/linux-process.h"
#include "guest/aarch64/linux-time-abi.h"
#include "util/timer.h"

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_REQUEUE_ 3
#define FUTEX_CMP_REQUEUE_ 4
#define FUTEX_PRIVATE_FLAG_ 128
#define FUTEX_CMD_MASK_ ~(FUTEX_PRIVATE_FLAG_)

struct futex {
    atomic_uint refcount;
    const void *memory_identity;
    qword_t addr;
    bool aarch64;
    struct list queue;
    struct list chain; // locked by futex_hash_lock
};

struct futex_wait {
    cond_t cond;
    struct futex *futex; // will be changed by a requeue
    struct list queue;
};

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];

static void __attribute__((constructor)) init_futex_hash(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

struct futex_key {
    const void *memory_identity;
    qword_t addr;
    bool aarch64;
};

static struct futex_key i386_futex_key(addr_t addr) {
    return (struct futex_key) {
        .memory_identity = current->mem,
        .addr = addr,
    };
}

static struct futex_key aarch64_futex_key(
        struct aarch64_linux_process *process, qword_t addr) {
    return (struct futex_key) {
        .memory_identity =
                aarch64_linux_process_memory_identity(process),
        .addr = addr,
        .aarch64 = true,
    };
}

static struct futex *futex_get_unlocked(struct futex_key key) {
    uintptr_t mixed = (uintptr_t) key.memory_identity ^
            (uintptr_t) key.addr ^ (uintptr_t) (key.addr >> 32);
    size_t hash = mixed % FUTEX_HASH_SIZE;
    struct list *bucket = &futex_hash[hash];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (futex->addr == key.addr &&
                futex->memory_identity == key.memory_identity &&
                futex->aarch64 == key.aarch64) {
            futex->refcount++;
            return futex;
        }
    }

    futex = malloc(sizeof(struct futex));
    if (futex == NULL)
        return NULL;
    futex->refcount = 1;
    futex->memory_identity = key.memory_identity;
    futex->addr = key.addr;
    futex->aarch64 = key.aarch64;
    list_init(&futex->queue);
    list_add(bucket, &futex->chain);
    return futex;
}

// Returns the futex for the current process at the given addr, and locks it
// Unlocked variant is available for times when you need to get two futexes at once
static struct futex *futex_get(struct futex_key key) {
    lock(&futex_lock);
    struct futex *futex = futex_get_unlocked(key);
    if (futex == NULL)
        unlock(&futex_lock);
    return futex;
}

static void futex_put_unlocked(struct futex *futex) {
    if (--futex->refcount == 0) {
        assert(list_empty(&futex->queue));
        list_remove(&futex->chain);
        free(futex);
    }
}

// Must be called on the result of futex_get when you're done with it
// Also has an unlocked version, for releasing the result of futex_get_unlocked
static void futex_put(struct futex *futex) {
    futex_put_unlocked(futex);
    unlock(&futex_lock);
}

static int futex_load(struct futex *futex, dword_t *out,
        struct guest_linux_user_fault *fault) {
    if (futex->aarch64) {
        assert(task_has_aarch64_process(current) &&
                futex->memory_identity ==
                        aarch64_linux_process_memory_identity(
                                current->aarch64_process));
        return aarch64_linux_process_read_u32(
                current->aarch64_process, futex->addr,
                out, fault) ? 0 : 1;
    }
    assert(futex->memory_identity == current->mem);
    read_wrlock(&current->mem->lock);
    dword_t *ptr = mem_ptr(
            current->mem, (addr_t) futex->addr, MEM_READ);
    read_wrunlock(&current->mem->lock);
    if (ptr == NULL)
        return 1;
    *out = *ptr;
    return 0;
}

static int futex_wait(struct futex_key key, dword_t val,
        struct timespec *timeout,
        const struct timer_time *deadline,
        struct guest_linux_user_fault *fault) {
    struct futex *futex = futex_get(key);
    if (futex == NULL)
        return _ENOMEM;
    int err = 0;
    dword_t tmp;
    if (futex_load(futex, &tmp, fault))
        err = _EFAULT;
    else if (tmp != val)
        err = _EAGAIN;
    else {
        struct futex_wait wait;
        wait.cond = COND_INITIALIZER;
        wait.futex = futex;
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
    }
    futex_put(futex);
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return err;
}

static int futex_wakelike(int op, struct futex_key key,
        dword_t wake_max, dword_t requeue_max,
        struct futex_key requeue_key) {
    struct futex *futex = futex_get(key);
    if (futex == NULL)
        return _ENOMEM;

    struct futex_wait *wait, *tmp;
    unsigned woken = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (woken >= wake_max)
            break;
        notify(&wait->cond);
        list_remove(&wait->queue);
        woken++;
    }

    if (op == FUTEX_REQUEUE_) {
        struct futex *futex2 = futex_get_unlocked(requeue_key);
        if (futex2 == NULL) {
            futex_put(futex);
            return _ENOMEM;
        }
        unsigned requeued = 0;
        list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
            if (requeued >= requeue_max)
                break;
            // sketchy as hell
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            assert(futex->refcount > 1); // should be true because this function keeps a reference
            futex->refcount--;
            futex2->refcount++;
            wait->futex = futex2;
            requeued++;
        }
        futex_put_unlocked(futex2);
        woken += requeued;
    }

    futex_put(futex);
    return woken;
}

int futex_wake(addr_t uaddr, dword_t wake_max) {
    return futex_wakelike(FUTEX_WAKE_, i386_futex_key(uaddr),
            wake_max, 0, (struct futex_key) {0});
}

int futex_wake_aarch64(struct aarch64_linux_process *process,
        qword_t uaddr, dword_t wake_max) {
    return futex_wakelike(FUTEX_WAKE_,
            aarch64_futex_key(process, uaddr), wake_max, 0,
            (struct futex_key) {0});
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
            return futex_wait(i386_futex_key(uaddr), val,
                    timeout_or_val2 ? &timeout : NULL, NULL, NULL);
        case FUTEX_WAKE_:
            STRACE("futex(FUTEX_WAKE, %#x, %d)", uaddr, val);
            return futex_wakelike(op & FUTEX_CMD_MASK_,
                    i386_futex_key(uaddr), val, 0,
                    (struct futex_key) {0});
        case FUTEX_REQUEUE_:
            STRACE("futex(FUTEX_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_wakelike(op & FUTEX_CMD_MASK_,
                    i386_futex_key(uaddr), val, timeout_or_val2,
                    i386_futex_key(uaddr2));
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
    if ((uaddr & (sizeof(dword_t) - 1)) != 0)
        return _EINVAL;
    if ((command == FUTEX_REQUEUE_ || command == FUTEX_CMP_REQUEUE_) &&
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
    struct futex_key key = aarch64_futex_key(process, uaddr);
    switch (command) {
        case FUTEX_WAIT_:
            return futex_wait(
                    key, val, NULL, deadline_pointer, fault);
        case FUTEX_WAKE_:
            return futex_wakelike(FUTEX_WAKE_, key, val, 0,
                    (struct futex_key) {0});
        case FUTEX_REQUEUE_:
            return futex_wakelike(FUTEX_REQUEUE_, key, val,
                    (dword_t) timeout_or_val2,
                    aarch64_futex_key(process, uaddr2));
    }
    use(val3);
    return _ENOSYS;
}

struct robust_list_head_ {
    addr_t list;
    dword_t offset;
    addr_t list_op_pending;
};

int_t sys_set_robust_list(addr_t robust_list, dword_t len) {
    STRACE("set_robust_list(%#x, %d)", robust_list, len);
    if (len != sizeof(struct robust_list_head_))
        return _EINVAL;
    current->robust_list = robust_list;
    return 0;
}

int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr) {
    STRACE("get_robust_list(%d, %#x, %#x)", pid, robust_list_ptr, len_ptr);

    lock(&pids_lock);
    struct task *task = pid_get_task(pid);
    unlock(&pids_lock);
    if (task != current)
        return _EPERM;

    if (user_put(robust_list_ptr, current->robust_list))
        return _EFAULT;
    if (user_put(len_ptr, (int[]) {sizeof(struct robust_list_head_)}))
        return _EFAULT;
    return 0;
}

#if __linux__
// pull in RUSAGE_THREAD
#define _GNU_SOURCE
#include <sys/resource.h>
#elif __APPLE__
// pull in thread_info and friends
#include <mach/mach.h>
#else
#error
#endif

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "kernel/calls.h"

#define LINUX_NR_OPEN_DEFAULT UINT64_C(1048576)

static bool resource_valid(int resource) {
    return resource >= 0 && resource < RLIMIT_NLIMITS_;
}

static rlim_t_ i386_rlim64_to_internal(rlim_t_ value) {
    return value >= UINT32_MAX ? RLIM_INFINITY_ : value;
}

static rlim_t_ i386_internal_to_rlim64(rlim_t_ value) {
    return value >= UINT32_MAX ? UINT64_MAX : value;
}

static int rlimit_get(struct task *task, int resource, struct rlimit_ *limit) {
    if (!resource_valid(resource))
        return _EINVAL;
    struct tgroup *group = task->group;
    lock(&group->lock);
    *limit = group->limits[resource];
    unlock(&group->lock);
    return 0;
}

rlim_t_ rlimit_task(struct task *task, int resource) {
    struct rlimit_ limit;
    if (rlimit_get(task, resource, &limit) != 0)
        die("invalid resource %d", resource);
    return limit.cur;
}

rlim_t_ rlimit(int resource) {
    return rlimit_task(current, resource);
}

static int do_getrlimit32(int resource, struct rlimit32_ *rlimit32) {
    STRACE("getlimit(%d)", resource);
    struct rlimit_ rlimit;
    int err = rlimit_get(current, resource, &rlimit);
    if (err < 0)
        return err;
    STRACE(" {cur=%#llx, max=%#llx}",
            (unsigned long long) rlimit.cur,
            (unsigned long long) rlimit.max);

    rlimit32->cur = rlimit.cur >= UINT32_MAX ?
            UINT32_MAX : (rlim32_t_) rlimit.cur;
    rlimit32->max = rlimit.max >= UINT32_MAX ?
            UINT32_MAX : (rlim32_t_) rlimit.max;
    return 0;
}

dword_t sys_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;
    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

dword_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ rlimit;
    int err = do_getrlimit32(resource, &rlimit);
    if (err < 0)
        return err;

    // This version of the call is for programs that aren't aware of rlim_t
    // being 64 bit. RLIM_INFINITY looks like -1 when truncated to 32 bits.
    if (rlimit.cur > INT_MAX)
        rlimit.cur = INT_MAX;
    if (rlimit.max > INT_MAX)
        rlimit.max = INT_MAX;

    if (user_put(rlim_addr, rlimit))
        return _EFAULT;
    return 0;
}

// 调用方持有 pids_lock。项目尚无 user namespace 与 capability 模型，
// 因而用有效 root 身份近似 Linux 的 CAP_SYS_RESOURCE。
static bool prlimit_access_allowed_locked(
        const struct task *caller, const struct task *target) {
    if (caller == target || caller->euid == 0)
        return true;
    return caller->uid == target->uid &&
            caller->uid == target->euid &&
            caller->uid == target->suid &&
            caller->gid == target->gid &&
            caller->gid == target->egid &&
            caller->gid == target->sgid;
}

int resource_prlimit_task(struct task *caller, pid_t_ pid,
        dword_t resource, const struct rlimit_ *new_limit,
        struct rlimit_ *old_limit) {
    assert(caller != NULL);
    lock(&pids_lock);
    struct task *target = pid == 0 ?
            caller : pid_get_task_zombie((dword_t) pid);
    int error = 0;
    if (target == NULL) {
        error = _ESRCH;
    } else if (!prlimit_access_allowed_locked(caller, target)) {
        error = _EPERM;
    } else if (resource >= RLIMIT_NLIMITS_) {
        error = _EINVAL;
    } else if (new_limit != NULL && new_limit->cur > new_limit->max) {
        error = _EINVAL;
    } else if (new_limit != NULL && resource == RLIMIT_NOFILE_ &&
            new_limit->max > LINUX_NR_OPEN_DEFAULT) {
        error = _EPERM;
    } else if (target->zombie || target->exiting) {
        error = _ESRCH;
    } else {
        struct tgroup *group = target->group;
        lock(&group->lock);
        struct rlimit_ previous = group->limits[resource];
        if (new_limit != NULL && new_limit->max > previous.max &&
                caller->euid != 0) {
            error = _EPERM;
        } else {
            if (old_limit != NULL)
                *old_limit = previous;
            if (new_limit != NULL)
                group->limits[resource] = *new_limit;
        }
        unlock(&group->lock);
    }
    unlock(&pids_lock);
    return error;
}

dword_t sys_setrlimit32(dword_t resource, addr_t rlim_addr) {
    struct rlimit32_ wire;
    if (user_get(rlim_addr, wire))
        return _EFAULT;
    struct rlimit_ rlimit = {
        .cur = i386_rlim64_to_internal(wire.cur),
        .max = i386_rlim64_to_internal(wire.max),
    };
    STRACE("setrlimit(%d, {cur=%#llx, max=%#llx})", resource,
            (unsigned long long) rlimit.cur,
            (unsigned long long) rlimit.max);
    return resource_prlimit_task(
            current, 0, resource, &rlimit, NULL);
}

dword_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_limit_addr, addr_t old_limit_addr) {
    STRACE("prlimit64(%d, %d)", pid, resource);
    struct rlimit_ new_limit;
    if (new_limit_addr != 0) {
        struct rlimit_ wire;
        if (user_get(new_limit_addr, wire))
            return _EFAULT;
        new_limit = (struct rlimit_) {
            .cur = i386_rlim64_to_internal(wire.cur),
            .max = i386_rlim64_to_internal(wire.max),
        };
        STRACE(" new={cur=%#llx, max=%#llx}",
                (unsigned long long) new_limit.cur,
                (unsigned long long) new_limit.max);
    }

    struct rlimit_ old_limit;
    int error = resource_prlimit_task(current, pid, resource,
            new_limit_addr != 0 ? &new_limit : NULL,
            old_limit_addr != 0 ? &old_limit : NULL);
    if (error < 0)
        return error;
    if (old_limit_addr != 0) {
        STRACE(" old={cur=%#llx, max=%#llx}",
                (unsigned long long) old_limit.cur,
                (unsigned long long) old_limit.max);
        struct rlimit_ wire = {
            .cur = i386_internal_to_rlim64(old_limit.cur),
            .max = i386_internal_to_rlim64(old_limit.max),
        };
        if (user_put(old_limit_addr, wire))
            return _EFAULT;
    }
    return 0;
}

struct rusage_ rusage_get_current(void) {
    // only the time fields are currently implemented
    struct rusage_ rusage = {0};
#if __linux__
    struct rusage usage;
    int err = getrusage(RUSAGE_THREAD, &usage);
    assert(err == 0);
    rusage.utime.sec = usage.ru_utime.tv_sec;
    rusage.utime.usec = usage.ru_utime.tv_usec;
    rusage.stime.sec = usage.ru_stime.tv_sec;
    rusage.stime.usec = usage.ru_stime.tv_usec;
#elif __APPLE__
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    mach_port_t thread = mach_thread_self();
    kern_return_t error = thread_info(thread,
            THREAD_BASIC_INFO, (thread_info_t) &info, &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (error != KERN_SUCCESS)
        return rusage;
    rusage.utime.sec = info.user_time.seconds;
    rusage.utime.usec = info.user_time.microseconds;
    rusage.stime.sec = info.system_time.seconds;
    rusage.stime.usec = info.system_time.microseconds;
#endif
    return rusage;
}

static void timeval_add(struct timeval_ *dst, struct timeval_ *src) {
    dst->sec += src->sec;
    dst->usec += src->usec;
    if (dst->usec >= 1000000) {
        dst->usec -= 1000000;
        dst->sec++;
    }
}

void rusage_add(struct rusage_ *dst, struct rusage_ *src) {
    timeval_add(&dst->utime, &src->utime);
    timeval_add(&dst->stime, &src->stime);
}

int resource_getrusage_task(
        struct task *task, sdword_t who, struct rusage_ *rusage) {
    assert(task != NULL && task == current && rusage != NULL);
    switch (who) {
        case RUSAGE_SELF_: {
            *rusage = rusage_get_current();
            lock(&task->group->lock);
            struct rusage_ finished_threads = task->group->rusage;
            unlock(&task->group->lock);
            rusage_add(rusage, &finished_threads);
            return 0;
        }
        case RUSAGE_CHILDREN_:
            lock(&task->group->lock);
            *rusage = task->group->children_rusage;
            unlock(&task->group->lock);
            return 0;
        case RUSAGE_THREAD_:
            *rusage = rusage_get_current();
            return 0;
        default:
            return _EINVAL;
    }
}

dword_t sys_getrusage(dword_t who, addr_t rusage_addr) {
    struct rusage_ rusage;
    int error = resource_getrusage_task(
            current, (sdword_t) who, &rusage);
    if (error < 0)
        return error;
    if (user_put(rusage_addr, rusage))
        return _EFAULT;
    return 0;
}

int_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr) {
    STRACE("sched_getaffinity(%d, %d, %#x)", pid, cpusetsize, cpuset_addr);
    if (pid != 0) {
        lock(&pids_lock);
        struct task *task = pid_get_task(pid);
        unlock(&pids_lock);
        if (task == NULL)
            return _ESRCH;
    }

    unsigned cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char cpuset[cpus / 8 + 1];
    if (cpusetsize < sizeof(cpuset))
        return _EINVAL;
    memset(cpuset, 0, sizeof(cpuset));
    for (unsigned i = 0; i < cpus; i++)
        bit_set(i, cpuset);
    if (user_write(cpuset_addr, cpuset, sizeof(cpuset)))
        return _EFAULT;
    // return the number of bytes written
    return sizeof(cpuset);
}
int_t sys_sched_setaffinity(pid_t_ UNUSED(pid), dword_t UNUSED(cpusetsize), addr_t UNUSED(cpuset_addr)) {
    // meh
    return 0;
}

int_t sys_getpriority(int_t which, pid_t_ who) {
    STRACE("getpriority(%d, %d)", which, who);
    return 20;
}
int_t sys_setpriority(int_t which, pid_t_ who, int_t prio) {
    STRACE("setpriority(%d, %d, %d)", which, who, prio);
    return 0;
}

// realtime scheduling stubs
int_t sys_sched_getparam(pid_t_ UNUSED(pid), addr_t param_addr) {
    int_t sched_priority = 0;
    if (user_put(param_addr, sched_priority))
        return _EFAULT;
    return 0;
}
#define SCHED_OTHER_ 0
int_t sys_sched_getscheduler(pid_t_ UNUSED(pid)) {
    return SCHED_OTHER_;
}
int_t sys_sched_setscheduler(pid_t_ UNUSED(pid), int_t policy, addr_t param_addr) {
    if (policy != SCHED_OTHER_)
        return _EINVAL;
    int_t sched_priority;
    if (user_get(param_addr, sched_priority))
        return _EFAULT;
    if (sched_priority != 0)
        return _EINVAL;
    return 0;
}

int_t sys_sched_get_priority_max(int_t policy) {
    STRACE("sched_get_priority_max(%d)", policy);
    if (policy == 0)
        return 0;
    return _EINVAL;
}

int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}
int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio)) {
    return 0;
}

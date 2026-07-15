#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/personality.h"

pid_t_ task_getpid(const struct task *task) {
    return task->tgid;
}

pid_t_ task_gettid(const struct task *task) {
    return task->pid;
}

pid_t_ task_getppid(const struct task *task) {
    pid_t_ ppid;
    lock(&pids_lock);
    const struct task *parent = task->group->leader->parent;
    if (parent != NULL)
        ppid = parent->tgid;
    else
        ppid = 0;
    unlock(&pids_lock);
    return ppid;
}

pid_t_ sys_getpid(void) {
    STRACE("getpid()");
    return task_getpid(current);
}
pid_t_ sys_gettid(void) {
    STRACE("gettid()");
    return task_gettid(current);
}
pid_t_ sys_getppid(void) {
    STRACE("getppid()");
    return task_getppid(current);
}

dword_t sys_getuid32(void) {
    STRACE("getuid32()");
    return current->uid;
}
dword_t sys_getuid(void) {
    STRACE("getuid()");
    return current->uid & 0xffff;
}

dword_t sys_geteuid32(void) {
    STRACE("geteuid32()");
    return current->euid;
}
dword_t sys_geteuid(void) {
    STRACE("geteuid()");
    return current->euid & 0xffff;
}

int_t sys_setuid(uid_t_ uid) {
    STRACE("setuid(%d)", uid);
    int_t error = 0;
    lock(&pids_lock);
    if (current->euid == 0) {
        current->uid = current->suid = uid;
    } else if (uid != current->uid && uid != current->suid) {
        error = _EPERM;
    }
    if (error == 0)
        current->euid = uid;
    unlock(&pids_lock);
    return error;
}

dword_t sys_setresuid(uid_t_ ruid, uid_t_ euid, uid_t_ suid) {
    STRACE("setresuid(%d, %d, %d)", ruid, euid, suid);
    lock(&pids_lock);
    if (current->euid != 0) {
        if (ruid != (uid_t) -1 && ruid != current->uid && ruid != current->euid && ruid != current->suid)
            goto denied;
        if (euid != (uid_t) -1 && euid != current->uid && euid != current->euid && euid != current->suid)
            goto denied;
        if (suid != (uid_t) -1 && suid != current->uid && suid != current->euid && suid != current->suid)
            goto denied;
    }

    if (ruid != (uid_t) -1)
        current->uid = ruid;
    if (euid != (uid_t) -1)
        current->euid = euid;
    if (suid != (uid_t) -1)
        current->suid = suid;
    unlock(&pids_lock);
    return 0;

denied:
    unlock(&pids_lock);
    return _EPERM;
}

int_t sys_getresuid(addr_t ruid_addr, addr_t euid_addr, addr_t suid_addr) {
    STRACE("getresuid(%#x, %#x, %#x)", ruid_addr, euid_addr, suid_addr);
    if (user_put(ruid_addr, current->uid))
        return _EFAULT;
    if (user_put(euid_addr, current->euid))
        return _EFAULT;
    if (user_put(suid_addr, current->suid))
        return _EFAULT;
    return 0;
}

int_t sys_setreuid(uid_t_ ruid, uid_t_ euid) {
    return sys_setresuid(ruid, euid, -1);
}

dword_t sys_getgid32(void) {
    STRACE("getgid32()");
    return current->gid;
}
dword_t sys_getgid(void) {
    STRACE("getgid()");
    return current->gid & 0xffff;
}

dword_t sys_getegid32(void) {
    STRACE("getegid32()");
    return current->egid;
}
dword_t sys_getegid(void) {
    STRACE("getegid()");
    return current->egid & 0xffff;
}

int_t sys_setgid(uid_t_ gid) {
    STRACE("setgid(%d)", gid);
    int_t error = 0;
    lock(&pids_lock);
    if (current->euid == 0) {
        current->gid = current->sgid = gid;
    } else if (gid != current->gid && gid != current->sgid) {
        error = _EPERM;
    }
    if (error == 0)
        current->egid = gid;
    unlock(&pids_lock);
    return error;
}

dword_t sys_setresgid(uid_t_ rgid, uid_t_ egid, uid_t_ sgid) {
    STRACE("setresgid(%d, %d, %d)", rgid, egid, sgid);
    lock(&pids_lock);
    if (current->euid != 0) {
        if (rgid != (uid_t) -1 && rgid != current->gid && rgid != current->egid && rgid != current->sgid)
            goto denied;
        if (egid != (uid_t) -1 && egid != current->gid && egid != current->egid && egid != current->sgid)
            goto denied;
        if (sgid != (uid_t) -1 && sgid != current->gid && sgid != current->egid && sgid != current->sgid)
            goto denied;
    }

    if (rgid != (uid_t) -1)
        current->gid = rgid;
    if (egid != (uid_t) -1)
        current->egid = egid;
    if (sgid != (uid_t) -1)
        current->sgid = sgid;
    unlock(&pids_lock);
    return 0;

denied:
    unlock(&pids_lock);
    return _EPERM;
}

int_t sys_getresgid(addr_t rgid_addr, addr_t egid_addr, addr_t sgid_addr) {
    STRACE("getresgid(%#x, %#x, %#x)", rgid_addr, egid_addr, sgid_addr);
    if (user_put(rgid_addr, current->gid))
        return _EFAULT;
    if (user_put(egid_addr, current->egid))
        return _EFAULT;
    if (user_put(sgid_addr, current->sgid))
        return _EFAULT;
    return 0;
}

int_t sys_setregid(uid_t_ rgid, uid_t_ egid) {
    return sys_setresgid(rgid, egid, -1);
}

int_t sys_getgroups(dword_t size, addr_t list) {
    STRACE("getgroups(%d, %#x)", size, list);
    if (size == 0)
        return current->ngroups;
    if (size < current->ngroups)
        return _EINVAL;
    for (unsigned i = 0; i < current->ngroups; i++)
        STRACE(" %d", current->groups[i]);
    if (user_write(list, current->groups, current->ngroups * sizeof(uid_t_)))
        return _EFAULT;
    return current->ngroups;
}

int_t sys_setgroups(dword_t size, addr_t list) {
    STRACE("setgroups(%d, %#x)", size, list);
    if (size > MAX_GROUPS)
        return _EINVAL;
    if (user_read(list, current->groups, size * sizeof(uid_t_)))
        return _EFAULT;
    for (unsigned i = 0; i < size; i++)
        STRACE(" %d", current->groups[i]);
    current->ngroups = size;
    return 0;
}

// this does not really work
int_t sys_capget(addr_t header_addr, addr_t data_addr) {
    STRACE("capget(%#x, %#x)", header_addr, data_addr);
    return 0;
}
int_t sys_capset(addr_t header_addr, addr_t data_addr) {
    STRACE("capset(%#x, %#x)", header_addr, data_addr);
    return 0;
}

// minimal version according to Linux sys/personality.h
int_t sys_personality(dword_t persona) {
    STRACE("personality(%#x)", persona);
    // Get the personality
    if (persona == 0xffffffff)
        return current->group->personality;

    // ADDR_NO_RANDOMIZE is the only thing we support, and you can't turn it off
    if (persona != ADDR_NO_RANDOMIZE_)
        return _EINVAL;

    return current->group->personality;
}

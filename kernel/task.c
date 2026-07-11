#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "emu/tlb.h"

__thread struct task *current;

static struct pid pids[MAX_PID + 1] = {};
lock_t pids_lock = LOCK_INITIALIZER;

static bool pid_empty(struct pid *pid) {
    return !pid->reserved && pid->task == NULL &&
            list_empty(&pid->session) && list_empty(&pid->pgroup);
}

static struct pid *pid_slot(dword_t id) {
    if (id >= array_size(pids))
        return NULL;
    return &pids[id];
}

struct pid *pid_get(dword_t id) {
    struct pid *pid = pid_slot(id);
    if (pid == NULL || pid->reserved || pid_empty(pid))
        return NULL;
    return pid;
}

struct task *pid_get_task_zombie(dword_t id) {
    struct pid *pid = pid_get(id);
    if (pid == NULL)
        return NULL;
    struct task *task = pid->task;
    return task;
}

struct task *pid_get_task(dword_t id) {
    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && (task->zombie || task->exiting))
        return NULL;
    return task;
}

struct task *task_create_(struct task *parent) {
    struct task *task = malloc(sizeof(struct task));
    if (task == NULL)
        return NULL;
    *task = (struct task) {};
    if (parent != NULL) {
        *task = *parent;
    } else {
        task_altstack_reset(task);
    }

    task_thread_store(task, zero_init(pthread_t));
    atomic_init(&task->start_ready, false);
    task->threadid = 0;
    task->cpu.poked_ptr = NULL;
    task->cpu._poked = false;
    list_init(&task->group_links);
    list_init(&task->children);
    list_init(&task->siblings);
    task->parent = parent;
    task->pending = 0;
    task->waiting = 0;
    list_init(&task->queue);
    task->saved_mask = 0;
    task->has_saved_mask = false;
    task->clear_tid = 0;
    task->robust_list = 0;
    task->did_exec = false;
    task->exit_code = 0;
    task->zombie = false;
    task->exiting = false;
    task->vfork = NULL;
    task->exit_signal = 0;

    task->general_lock = (lock_t) {0};
    lock_init(&task->general_lock);
    task->sockrestart = (struct task_sockrestart) {};
    list_init(&task->sockrestart.listen);
    task->waiting_cond = NULL;
    task->waiting_lock = NULL;
    task->waiting_cond_lock = (lock_t) {0};
    lock_init(&task->waiting_cond_lock);
    task->pause = (cond_t) {0};
    cond_init(&task->pause);
    task->ptrace = (typeof(task->ptrace)) {0};
    lock_init(&task->ptrace.lock);
    cond_init(&task->ptrace.cond);

    lock(&pids_lock);
    static int cur_pid = 0;
    do {
        cur_pid++;
        if (cur_pid > MAX_PID) cur_pid = 1;
    } while (!pid_empty(&pids[cur_pid]));
    struct pid *pid = &pids[cur_pid];
    pid->id = cur_pid;
    pid->reserved = true;
    list_init(&pid->session);
    list_init(&pid->pgroup);
    task->pid = pid->id;
    unlock(&pids_lock);
    return task;
}

void task_publish_locked(struct task *task) {
    struct pid *pid = pid_slot(task->pid);
    assert(pid != NULL && pid->reserved && pid->task == NULL);

    struct tgroup *group = task->group;
    if (task_is_leader(task)) {
        struct pid *session = pid_slot(group->sid);
        struct pid *pgroup = pid_slot(group->pgid);
        assert(session != NULL && pgroup != NULL);
        list_add(&session->session, &group->session);
        list_add(&pgroup->pgroup, &group->pgroup);
    }
    list_add(&group->threads, &task->group_links);
    if (task->parent != NULL)
        list_add(&task->parent->children, &task->siblings);

    pid->task = task;
    pid->reserved = false;
}

void task_publish(struct task *task) {
    lock(&pids_lock);
    lock(&task->group->lock);
    task_publish_locked(task);
    unlock(&task->group->lock);
    unlock(&pids_lock);
}

void task_abort_create(struct task *task) {
    lock(&pids_lock);
    struct pid *pid = pid_slot(task->pid);
    assert(pid != NULL && pid->reserved && pid->task == NULL);
    pid->reserved = false;
    unlock(&pids_lock);
    cond_destroy(&task->pause);
    cond_destroy(&task->ptrace.cond);
    free(task);
}

void task_destroy(struct task *task) {
    list_remove(&task->siblings);
    struct pid *pid = pid_slot(task->pid);
    assert(pid != NULL && !pid->reserved && pid->task == task);
    pid->task = NULL;
    free(task);
}

void task_run_current(void) {
    struct cpu_state *cpu = &current->cpu;
    struct tlb tlb = {};
    tlb_refresh(&tlb, &current->mem->mmu);
    while (true) {
        read_wrlock(&current->mem->lock);
        int interrupt = cpu_run_to_interrupt(cpu, &tlb);
        read_wrunlock(&current->mem->lock);
        handle_interrupt(interrupt);
    }
}

static void *task_thread(void *task) {
    current = task;
    while (!atomic_load_explicit(&current->start_ready, memory_order_acquire))
        sched_yield();
    update_thread_name();
    task_run_current();
    die("task_thread returned"); // above function call should never return
}

static pthread_attr_t task_thread_attr;
__attribute__((constructor)) static void create_attr(void) {
    pthread_attr_init(&task_thread_attr);
    pthread_attr_setdetachstate(&task_thread_attr, PTHREAD_CREATE_DETACHED);
}

void task_start_suspended(struct task *task) {
    atomic_store_explicit(&task->start_ready, false, memory_order_relaxed);
    lock(&task->sighand->lock);
    pthread_t thread;
    if (pthread_create(&thread, &task_thread_attr, task_thread, task) != 0)
        die("could not create thread");
    task_thread_store(task, thread);
    unlock(&task->sighand->lock);
}

void task_release_start(struct task *task) {
    atomic_store_explicit(&task->start_ready, true, memory_order_release);
}

void task_start(struct task *task) {
    lock(&pids_lock);
    task_start_suspended(task);
    unlock(&pids_lock);
    task_release_start(task);
}

int_t sys_sched_yield(void) {
    STRACE("sched_yield()");
    sched_yield();
    return 0;
}

void update_thread_name(void) {
    char name[16]; // As long as Linux will let us make this
    snprintf(name, sizeof(name), "-%d", current->pid);
    size_t pid_width = strlen(name);
    size_t name_width = snprintf(name, sizeof(name), "%s", current->comm);
    sprintf(name + (name_width < sizeof(name) - 1 - pid_width ? name_width : sizeof(name) - 1 - pid_width), "-%d", current->pid);
#if __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "bpf/bpf.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/cpu.h"
#include "core/string.h"

void proc_set_name(task_t *t, const char *name)
{
    if (!t)
        return;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
}

void *proc_scratch_buffer(size_t size)
{
    task_t *t = proc_current();
    if (!t || size == 0)
        return NULL;
    if (t->scratch_buf && t->scratch_size >= size)
        return t->scratch_buf;

    void *buf = kmalloc(size);
    if (!buf)
        return NULL;
    if (t->scratch_buf)
        kfree(t->scratch_buf);
    t->scratch_buf = buf;
    t->scratch_size = size;
    return buf;
}

void proc_task_init_common(task_t *t, task_t *parent)
{
    int parent_pgid = parent ? parent->pgid : 0;
    int parent_sid = parent ? parent->sid : 0;

    t->ppid      = parent ? parent->pid : 0;
    t->tgid      = t->pid;
    t->state     = PROC_READY;
    t->parent    = parent;
    t->exit_code = 0;
    t->priority  = parent ? parent->priority : 0;
    t->sched_level = parent ? parent->sched_level : 0;
    t->cpu_id    = parent ? parent->cpu_id : cpu_current_id();
    t->on_rq     = 0;
    t->rq_next   = NULL;
    t->rq_prev   = NULL;
    t->cfs_next  = NULL;
    t->cfs_prev  = NULL;
    t->wait_next = NULL;
    t->vruntime  = 0;
    t->exec_start = 0;
    t->cfs_slice = 0;
    t->cfs_weight = sched_weight_for_nice(t->priority);
    t->sched_policy = parent ? parent->sched_policy : SCHED_NORMAL;
    t->waiting_for_child = 0;
    t->wake_time = 0;
    t->alarm_expire = 0;
    t->itimer_real_interval = 0;
    memset(t->itimer_values, 0, sizeof(t->itimer_values));
    t->total_time = 0;
    t->child_utime = 0;
    t->child_stime = 0;
    t->pgid      = parent ? (parent_pgid > 0 ? parent_pgid : parent->pid) : t->pid;
    t->sid       = parent ? (parent_sid > 0 ? parent_sid : parent->pid) : t->pid;
    if (t->pgid <= 0)
        t->pgid = t->pid;
    if (t->sid <= 0)
        t->sid = t->pid;
    t->fs.umask     = parent ? parent->fs.umask : 022;
    t->cred.uid       = parent ? parent->cred.uid : 0;
    t->cred.euid      = parent ? parent->cred.euid : 0;
    t->cred.suid      = parent ? parent->cred.suid : 0;
    t->cred.fsuid     = parent ? parent->cred.fsuid : t->cred.euid;
    t->cred.gid       = parent ? parent->cred.gid : 0;
    t->cred.egid      = parent ? parent->cred.egid : 0;
    t->cred.sgid      = parent ? parent->cred.sgid : 0;
    t->cred.fsgid     = parent ? parent->cred.fsgid : t->cred.egid;
    t->cred.ngroups   = parent ? parent->cred.ngroups : 0;
    if (parent)
        memcpy(t->cred.groups, parent->cred.groups, sizeof(t->cred.groups));
    t->cred.cap_effective = parent ? parent->cred.cap_effective : ~(uint64_t)0;
    t->cred.cap_permitted = parent ? parent->cred.cap_permitted : ~(uint64_t)0;
    t->cred.cap_inheritable = parent ? parent->cred.cap_inheritable : 0;
    t->cred.cap_bounding = parent ? parent->cred.cap_bounding : ~(uint64_t)0;
    t->policy.oom_score_adj = parent ? parent->policy.oom_score_adj : 0;
    t->policy.thp_disabled = parent ? parent->policy.thp_disabled : 0;
    t->clone_flags = 0;
    t->exit_signal = SIGCHLD;
    t->clear_child_tid = NULL;
    t->robust_list_head = 0;
    t->sigaltstack.ss_sp = NULL;
    t->sigaltstack.ss_flags = SS_DISABLE;
    t->sigaltstack.ss_size = 0;
    t->sig_handling = 0;
    t->sigsuspend_active = 0;
    t->sig_saved_ctx = (trap_context_t){0};
    t->sig_old_blocked = 0;
    t->thread_pending = 0;
    t->limits.stack = parent ? parent->limits.stack : USER_STACK_MAX_SIZE;
    t->limits.nofile = parent ? parent->limits.nofile : MAX_FILES;
    t->mm        = NULL;

    if (parent) {
        memcpy(t->fs.cwd, parent->fs.cwd, MAX_PATH_LEN);
        memcpy(t->fs.root_path, parent->fs.root_path, MAX_PATH_LEN);
        memcpy(t->exec_path, parent->exec_path, MAX_PATH_LEN);
    } else {
        t->fs.cwd[0] = '/';
        t->fs.cwd[1] = '\0';
        t->fs.root_path[0] = '/';
        t->fs.root_path[1] = '\0';
        t->exec_path[0] = '\0';
    }
    if (t->fs.cwd[0] == '\0') {
        t->fs.cwd[0] = '/';
        t->fs.cwd[1] = '\0';
    }
    if (t->fs.root_path[0] == '\0') {
        t->fs.root_path[0] = '/';
        t->fs.root_path[1] = '\0';
    }

    if (parent)
        fdtable_copy(t, parent);
    else
        fdtable_init(t);

    t->signals = kmalloc(sizeof(signal_state_t));
    if (t->signals) {
        if (parent && parent->signals)
            signal_copy((signal_state_t *)parent->signals,
                        (signal_state_t *)t->signals);
        else
            signal_init((signal_state_t *)t->signals);
    }
}

void proc_task_release_resources(task_t *t)
{
    if (!t)
        return;

    fdtable_close_all(t);
    bpf_release_process(t->pid);

    if (t->mm) {
        mm_destroy(t->mm);
        t->mm = NULL;
    }
    t->pgdir = NULL;

    if (t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        t->signals = NULL;
        if (refcount_dec_and_test(&ss->refcount))
            kfree(ss);
    }
    if (t->scratch_buf) {
        kfree(t->scratch_buf);
        t->scratch_buf = NULL;
        t->scratch_size = 0;
    }

    if (t->kstack) {
        kfree(t->kstack_base);
        t->kstack = 0;
        t->kstack_base = NULL;
    }
}

void proc_destroy_task(task_t *t)
{
    if (!t)
        return;
    int free_storage = t->dynamic_alloc;

    vfs_release_process_locks(t->pid);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    proc_runq_remove_locked(t);
    proc_unlink_task_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);
    proc_pid_unregister(t);

    proc_task_release_resources(t);

    memset(t, 0, sizeof(*t));
    if (free_storage)
        kfree(t);
}

void proc_free_pid(int pid)
{
    task_t *t = proc_find(pid);
    if (!t)
        return;
    proc_destroy_task(t);
}

#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/progress.h"
#include "core/timer.h"
#include "core/smp.h"
#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"

#ifdef CONFIG_ABI_LINUX
extern void posix_timer_tick(void);
#endif

#ifdef CONFIG_ABI_NATIVE
extern void a20_timer_tick(void);
#endif

uint64_t a20_sched_ticks_per_sec(void)
{
    return TICKS_PER_SEC;
}

uint32_t a20_sched_nr_cpus(void)
{
    return CONFIG_NR_CPUS;
}

uint32_t a20_sched_current_cpu_id(void)
{
    return cpu_current_id();
}

void a20_sched_send_reschedule(uint32_t cpu)
{
    if (cpu >= CONFIG_NR_CPUS || cpu == cpu_current_id())
        return;
#if defined(CONFIG_RISCV64) || defined(CONFIG_LOONGARCH64) || defined(CONFIG_AARCH64)
    smp_send_reschedule(cpu);
#else
    (void)cpu;
#endif
}

void a20_sched_posix_timer_tick(void)
{
#ifdef CONFIG_ABI_LINUX
    posix_timer_tick();
#endif
}

void a20_sched_native_timer_tick(void)
{
#ifdef CONFIG_ABI_NATIVE
    a20_timer_tick();
#endif
}

void a20_sched_low_level_switch(task_t *old, uint64_t next_kstack)
{
    if (old)
        arch_set_task_pointer(old);
    __switch(next_kstack);
}

uint32_t a20_sched_task_cpu_mask(task_t *task)
{
#if CONFIG_NR_CPUS >= 32
    uint32_t mask = ~0U;
#else
    uint32_t mask = (1U << CONFIG_NR_CPUS) - 1U;
#endif

    if (task)
        mask &= task->cpus_allowed;
    if (task && task->cgroup) {
        cg_node_t *node = (cg_node_t *)task->cgroup;
        mask &= node->res.cpuset.effective_cpus;
    }
    return mask;
}

int a20_sched_task_sched_policy(task_t *task) { return task ? task->sched_policy : 0; }
int a20_sched_task_sched_level(task_t *task) { return task ? task->sched_level : 0; }
void a20_sched_task_set_sched_level(task_t *task, int level) { if (task) task->sched_level = level; }
int a20_sched_task_priority(task_t *task) { return task ? task->priority : 0; }
int a20_sched_task_state(task_t *task) { return task ? (int)task->state : PROC_UNUSED; }
void a20_sched_task_set_state(task_t *task, int state) { if (task) task->state = (proc_state_t)state; }
uint32_t a20_sched_task_cpu_id(task_t *task) { return task ? task->cpu_id : 0; }
void a20_sched_task_set_cpu_id(task_t *task, uint32_t cpu) { if (task) task->cpu_id = cpu; }
int a20_sched_task_on_rq(task_t *task) { return task ? task->on_rq : 0; }
void a20_sched_task_set_on_rq(task_t *task, int on_rq) { if (task) task->on_rq = on_rq; }
uint64_t a20_sched_task_ready_since(task_t *task) { return task ? task->ready_since : 0; }
void a20_sched_task_set_ready_since(task_t *task, uint64_t ready_since) { if (task) task->ready_since = ready_since; }
uint64_t a20_sched_task_exec_start(task_t *task) { return task ? task->exec_start : 0; }
void a20_sched_task_set_exec_start(task_t *task, uint64_t exec_start) { if (task) task->exec_start = exec_start; }
uint64_t a20_sched_task_wake_time(task_t *task) { return task ? __atomic_load_n(&task->wake_time, __ATOMIC_RELAXED) : 0; }
void a20_sched_task_set_wake_time(task_t *task, uint64_t wake_time) { if (task) __atomic_store_n(&task->wake_time, wake_time, __ATOMIC_RELAXED); }
uint64_t a20_sched_task_alarm_expire(task_t *task) { return task ? __atomic_load_n(&task->alarm_expire, __ATOMIC_RELAXED) : 0; }
void a20_sched_task_set_alarm_expire(task_t *task, uint64_t alarm_expire) { if (task) __atomic_store_n(&task->alarm_expire, alarm_expire, __ATOMIC_RELAXED); }
uint64_t a20_sched_task_itimer_real_interval(task_t *task) { return task ? task->itimer_real_interval : 0; }
uint64_t a20_sched_task_kstack(task_t *task) { return task ? task->kstack : 0; }
int a20_sched_task_cg_throttled(task_t *task) { return task ? task->cg_throttled : 0; }
void a20_sched_task_set_cg_throttled(task_t *task, int throttled) { if (task) task->cg_throttled = throttled; }
void *a20_sched_task_cgroup(task_t *task) { return task ? task->cgroup : NULL; }
uint64_t a20_sched_task_cg_cpu_start(task_t *task) { return task ? task->cg_cpu_start : 0; }
void a20_sched_task_set_cg_cpu_start(task_t *task, uint64_t start) { if (task) task->cg_cpu_start = start; }
int a20_sched_task_pid(task_t *task) { return task ? task->pid : -1; }
int a20_sched_task_ppid(task_t *task) { return task ? task->ppid : -1; }
int a20_sched_task_clone_flags(task_t *task) { return task ? task->clone_flags : 0; }
task_t *a20_sched_task_rq_next(task_t *task) { return task ? task->rq_next : NULL; }
void a20_sched_task_set_rq_next(task_t *task, task_t *next) { if (task) task->rq_next = next; }
task_t *a20_sched_task_rq_prev(task_t *task) { return task ? task->rq_prev : NULL; }
void a20_sched_task_set_rq_prev(task_t *task, task_t *prev) { if (task) task->rq_prev = prev; }

int a20_sched_task_should_reap_zombie(task_t *task)
{
    if (!task)
        return 0;
    task_t *parent = task->parent;
    if (!parent || parent == proc_idle_task() || task->ppid == 0 || (task->clone_flags & CLONE_THREAD))
        return 1;
    if (parent->signals) {
        signal_state_t *ss = (signal_state_t *)parent->signals;
        sigaction_t *act = &ss->actions[SIGCHLD];
        if (act->sa_handler == SIG_IGN || (act->sa_flags & SA_NOCLDWAIT))
            return 1;
    }
    return 0;
}

void a20_sched_trace_ctxsw(int prev_pid, int next_pid)
{
    ktrace_sched("[SCHED] ctxsw: %d -> %d\n", prev_pid, next_pid);
}

void a20_sched_trace_fall_to_idle(int cur_pid, int state)
{
    ktrace_sched("[SCHED] fall-to-idle: cur=%d state=%d\n", cur_pid, state);
}

void a20_sched_trace_yield(int pid)
{
    ktrace_sched("[SCHED] yield: pid=%d\n", pid);
}

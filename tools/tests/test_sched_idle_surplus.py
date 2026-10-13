#!/usr/bin/env python3
"""Exercise the real idle-steal body; a running task counts toward surplus."""
from pathlib import Path
import subprocess
import tempfile

from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_NR_CPUS 3
#define PROC_READY 1
#define PROC_RUNNING 2
#define PROC_CPU_NONE 99
#define NULL_TASK ((task_t *)0)
typedef struct task {
    int state, on_rq, kstack, cg_throttled, dispatching;
    unsigned cpu_id, owner_cpu, mask;
    uint64_t ready_since;
    struct task *rq_next, *rq_prev;
} task_t;
typedef struct {
    int lock, deny_lock;
    unsigned nr_running, weight;
    task_t *eevdf_last;
} proc_runq_t;
static proc_runq_t sched_runq[CONFIG_NR_CPUS];
static task_t idle_tasks[CONFIG_NR_CPUS], worker;
static task_t *slots[CONFIG_NR_CPUS];
static unsigned online, irq_enabled, removed, sched_runqueue_migrations;
static task_t *proc_current_on_cpu(unsigned cpu) {
    return cpu < CONFIG_NR_CPUS ? slots[cpu] : NULL;
}
static int smp_cpu_is_online(unsigned cpu) { return online & (1U << cpu); }
static int spin_trylock_irqsave(int *lock, uint64_t *flags) {
    proc_runq_t *rq = (proc_runq_t *)lock;
    if (*lock || rq->deny_lock) return 0;
    *flags = irq_enabled; irq_enabled = 0; *lock = 1; return 1;
}
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
    assert(*lock && !irq_enabled); *lock = 0; irq_enabled = flags;
}
static unsigned sched_task_cpu_mask(task_t *t) { return t->mask; }
static void eevdf_remove_locked(proc_runq_t *rq, task_t *t) {
    assert(rq->lock && !irq_enabled && rq->eevdf_last == t);
    rq->eevdf_last = NULL; removed++;
}
static void sched_runq_eevdf_del_weight(proc_runq_t *rq, task_t *t) {
    assert(rq->lock && !irq_enabled && t == &worker); rq->weight--;
}
'''
HARNESS = r'''
static void reset(unsigned remote, unsigned count, int idle) {
    memset(sched_runq, 0, sizeof(sched_runq));
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
        slots[cpu] = &idle_tasks[cpu];
    /* Deliberately unreadable: production classification must not dereference
     * a remote current pointer after its publication slot has changed. */
    if (!idle) slots[remote] = (task_t *)(uintptr_t)1;
    worker = (task_t){.state=PROC_READY, .on_rq=1, .kstack=1,
        .cpu_id=remote, .owner_cpu=PROC_CPU_NONE, .mask=7,
        .ready_since=123, .rq_next=&worker, .rq_prev=&worker};
    sched_runq[remote] = (proc_runq_t){.nr_running=count,
        .weight=count, .eevdf_last=count ? &worker : NULL};
    online=7; removed=0; sched_runqueue_migrations=0;
    sched_runq[0].lock=1; irq_enabled=0;
}
static void expect(unsigned remote, int steal) {
    unsigned count=sched_runq[remote].nr_running;
    task_t before=worker;
    task_t *picked=sched_runq_steal_locked(&sched_runq[0], 0);
    assert(!irq_enabled && sched_runq[0].lock && !sched_runq[remote].lock);
    assert((picked == &worker) == steal);
    assert(removed == (unsigned)steal && sched_runqueue_migrations == (unsigned)steal);
    assert(sched_runq[remote].nr_running == count - steal);
    assert(sched_runq[remote].weight == count - steal);
    if (steal) {
        assert(worker.cpu_id == 0 && worker.owner_cpu == 0 && worker.dispatching);
        assert(!worker.on_rq && !worker.ready_since && !worker.rq_next && !worker.rq_prev);
    } else {
        assert(!memcmp(&worker, &before, sizeof(worker)));
    }
}
int main(void) {
    assert(proc_cpu_is_idle(CONFIG_NR_CPUS));
    for (unsigned remote=1; remote<CONFIG_NR_CPUS; remote++) {
        reset(remote,1,0); expect(remote,1); /* One running + one ready. */
        reset(remote,1,1); expect(remote,0); /* Lone work on idle peer. */
        reset(remote,1,1); slots[remote]=NULL; expect(remote,0);
        reset(remote,2,1); expect(remote,1); /* Existing queued surplus. */
        reset(remote,0,0); expect(remote,0);
        reset(remote,1,0); worker.mask=1U<<remote; expect(remote,0);
        reset(remote,1,0); worker.cg_throttled=1; expect(remote,0);
        reset(remote,1,0); worker.state=PROC_RUNNING; expect(remote,0);
        reset(remote,1,0); worker.on_rq=0; expect(remote,0);
        reset(remote,1,0); worker.kstack=0; expect(remote,0);
        reset(remote,1,0); sched_runq[remote].deny_lock=1; expect(remote,0);
        reset(remote,1,0); online &= ~(1U<<remote); expect(remote,0);
        /* RT-only queue has no EEVDF candidate. */
        reset(remote,2,0); sched_runq[remote].eevdf_last=NULL; expect(remote,0);
    }
    puts("idle surplus: PASS");
}
'''


def main():
    source = (ROOT / 'kernel/proc/sched.c').read_text()
    body = extract_function(source, 'static task_t *sched_runq_steal_locked(')
    classification = extract_function((ROOT / 'kernel/proc/proc.c').read_text(),
                                      'int proc_cpu_is_idle(unsigned cpu)')
    old = body.replace('!rrq->eevdf_last || (queued < 2 && proc_cpu_is_idle(remote))',
                       '!rrq->eevdf_last || queued < 2')
    assert old != body
    with tempfile.TemporaryDirectory(prefix='a20-idle-surplus-') as directory:
        directory = Path(directory)
        for label, code, negative in [('fixed', body, False), ('legacy', old, True)]:
            src, exe = directory / (label + '.c'), directory / label
            src.write_text(PRELUDE + classification + code + HARNESS)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if (not negative and result.returncode != 0 or
                    negative and (result.returncode != -6 or 'expect' not in result.stderr)):
                raise SystemExit(f'{label}: unexpected result {result}')
    print('test-sched-idle-surplus: PASS (real bodies, ownership, restrictions, legacy negative)')


if __name__ == '__main__':
    main()

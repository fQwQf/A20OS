"""Inject a wake at source unlock during the real runqueue steal function."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index('static ', source.index(name) - 40)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class DispatchOwnershipTest(unittest.TestCase):
    def test_cancel_dispatch_after_stop_or_exit(self):
        source = (ROOT / 'kernel/proc/sched.c').read_text()
        unpick = function(source, 'sched_runq_unpick_locked(')
        harness = r'''
#include <assert.h>
#include <stdint.h>
#define CONFIG_NR_CPUS 2
#define PROC_READY 1
#define PROC_CPU_NONE (~0U)
typedef struct {
    int dispatching, on_rq, on_cpu, state, sched_level;
    unsigned owner_cpu, cpu_id;
    void *kstack;
    uint64_t ready_since;
} task_t;
typedef struct { unsigned nr_running; } proc_runq_t;
static proc_runq_t sched_runq[2];
static unsigned sched_violations, drops, queued, locked;
#define RUNQ_LOCK_IRQ(cpu) (assert((cpu) < 2 && !locked), locked=1, 0)
#define RUNQ_UNLOCK_IRQ(cpu, flags) do { (void)(cpu); (void)(flags); \
    assert(locked); locked=0; } while(0)
static int sched_task_rt(task_t *t) { (void)t; return 0; }
static int sched_level_clamp(int level) { return level; }
static uint64_t timer_get_ticks(void) { return 42; }
static void proc_put(task_t *t) { (void)t; assert(!locked); drops++; }
static void rt_enqueue_locked(proc_runq_t *rq, task_t *t) {
    (void)rq; (void)t; assert(locked); queued++;
}
static void eevdf_insert_locked(proc_runq_t *rq, task_t *t) {
    rt_enqueue_locked(rq,t);
}
'''
        check = r'''
int main(void) {
    /* A stop/exit wins after selection but before context publication. */
    for (int state=2; state<=4; state++) {
        task_t t={.dispatching=1,.owner_cpu=1,.state=state,.kstack=&t};
        sched_runq_unpick_locked(&t);
        assert(!t.dispatching && t.owner_cpu==PROC_CPU_NONE && !t.on_rq);
        assert(drops==(unsigned)(state-1) && !queued && !sched_violations);
    }
    task_t t={.dispatching=1,.owner_cpu=1,.state=PROC_READY,.kstack=&t};
    sched_runq_unpick_locked(&t);
    assert(!t.dispatching && t.owner_cpu==PROC_CPU_NONE && t.on_rq);
    assert(drops==3 && queued==1 && sched_runq[1].nr_running==1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/tmp') as tmp:
            src, exe = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            src.write_text(harness + unpick + check)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(src),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_steal_wake_at_source_unlock(self):
        source = (ROOT / 'kernel/proc/sched.c').read_text()
        steal = function(source, 'sched_runq_steal_locked(')
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_NR_CPUS 2
#define PROC_READY 1
#define PROC_CPU_NONE (~0U)
typedef struct task {
    int state, on_rq, dispatching, cg_throttled;
    void *kstack;
    unsigned cpu_id, owner_cpu;
    uint64_t ready_since;
    struct task *rq_next, *rq_prev;
} task_t;
typedef struct { int lock; unsigned nr_running; task_t *eevdf_last; } proc_runq_t;
static proc_runq_t sched_runq[2];
static unsigned long sched_runqueue_migrations;
static task_t target;
static int smp_cpu_is_online(unsigned cpu) { (void)cpu; return 1; }
/* The ownership scenario has two queued tasks; idle is only a hint. */
static int proc_cpu_is_idle(unsigned cpu) { assert(cpu < 2); return 0; }
static int spin_trylock_irqsave(int *lock, uint64_t *flags) {
    assert(!*lock); *lock = 1; *flags = 0; return 1;
}
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
    (void)flags; assert(*lock); *lock = 0;
    /* At the old unlock point a simultaneous waker could claim a second
     * queue reference. With dispatch ownership published it cannot. */
    if (!target.on_rq && !target.dispatching) target.on_rq = 1;
}
static void eevdf_remove_locked(proc_runq_t *rq, task_t *t) {
    assert(rq->lock && t->on_rq); rq->eevdf_last = NULL;
}
static void sched_runq_eevdf_del_weight(proc_runq_t *rq, task_t *t) {
    (void)t; assert(rq->lock);
}
static unsigned sched_task_cpu_mask(task_t *t) { (void)t; return 3; }
static void proc_sched_set_task_cpu_locked(task_t *t, unsigned cpu) { t->cpu_id = cpu; }
'''
        check = r'''
int main(void) {
    target = (task_t){.state=PROC_READY,.on_rq=1,.kstack=&target,
                     .cpu_id=1,.owner_cpu=PROC_CPU_NONE};
    sched_runq[0].lock = 1;
    sched_runq[1].nr_running = 2;
    sched_runq[1].eevdf_last = &target;
    assert(sched_runq_steal_locked(&sched_runq[0],0) == &target);
    assert(target.dispatching && target.owner_cpu == 0);
    assert(target.on_rq == EXPECT_DUPLICATE);
    assert(sched_runq[1].nr_running == 1);
    assert(sched_runqueue_migrations == 1);
    return 0;
}
'''
        # Negative control reproduces the old publication gap in the same body:
        # release the source queue before the dispatch owner is published, so
        # the stub unlock observes an unqueued, unowned task and lets the
        # simulated waker claim a second queue reference.
        migrations = ('        __atomic_fetch_add(&sched_runqueue_migrations, '
                      '1, __ATOMIC_RELAXED);\n')
        unlock = '        spin_unlock_irqrestore(&rrq->lock, rrf);\n'
        publish = '        proc_sched_set_task_cpu_locked(t, local);\n'
        release_after_remove = unlock + migrations
        if steal.count(release_after_remove) != 1 or steal.count(publish) != 1:
            raise ValueError(
                'steal body no longer has exactly one ownership publication '
                'and one trailing source-queue release; the negative control '
                'cannot be derived from the current body')
        old = steal.replace(release_after_remove, migrations)
        old = old.replace(publish, unlock + publish)
        if old.count(unlock) != steal.count(unlock):
            raise ValueError('negative control changed the unlock count')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build/tmp') as tmp:
            src, exe = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
            for body, duplicate in [(steal, 0), (old, 1)]:
                src.write_text(harness + body + check)
                subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                                f'-DEXPECT_DUPLICATE={duplicate}', str(src),
                                '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Real scheduler bodies, deterministic timer IRQ at the yield clock sample.

The IRQ seam charges the real task after sampling but before returning the
sample. This represents the vulnerable instruction window, without QEMU
breakpoints, kernel state writes, or timing-dependent stress as a unit oracle.
"""
from pathlib import Path
import subprocess
import tempfile

from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define EEVDF_NICE0_LOAD 1024
#define TICKS_PER_SEC 1000
#define EEVDF_BASE_SLICE 10
#define PROC_RUNNING 2
#define PROC_READY 1
typedef struct {
    uint32_t cfs_weight, pi_boost_weight;
    uint64_t eevdf_vruntime, eevdf_deadline, eevdf_last_account;
    int pid, state, on_cpu, park_lock;
    unsigned owner_cpu, cpu_id;
} task_t;
typedef struct { uint64_t eevdf_weight, eevdf_vtime; } proc_runq_t;
static proc_runq_t sched_runq[2];
static task_t current, idle;
static unsigned cpu;
static int irq_enabled, inject, pending;
static int g_sched_base_slice_ms;
static int sched_task_rt(task_t *t) { (void)t; return 0; }
static task_t *proc_current(void) { return &current; }
static task_t *proc_idle_task(void) { return &idle; }
static unsigned cpu_current_id(void) { return cpu; }
static void ktrace_sched(const char *fmt, ...) { (void)fmt; }
static void panic(const char *fmt, ...) { (void)fmt; exit(99); }
static void eevdf_charge(proc_runq_t *, task_t *, uint64_t);
static void tick(void) {
    if (current.state == PROC_RUNNING)
        eevdf_charge(&sched_runq[cpu], &current, 200);
}
static uint64_t timer_get_ticks(void) {
    uint64_t sample = 100;
    if (inject) {
        inject = 0;
        if (irq_enabled) tick();
        else pending = 1;
    }
    return sample;
}
static uint64_t spin_lock_irqsave(int *lock) {
    assert(!*lock);
    uint64_t flags = irq_enabled;
    irq_enabled = 0;
    *lock = 1;
    return flags;
}
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
    assert(*lock);
    *lock = 0;
    irq_enabled = flags;
    if (pending && irq_enabled) { pending = 0; tick(); }
}
static void proc_sched_assert_task_locked(task_t *t) {
    assert(t->park_lock && !irq_enabled && t->state == PROC_READY);
}
static void sched(void) {
    /* Actual switch path accounts the remainder with IRQs masked. */
    int saved = irq_enabled;
    irq_enabled = 0;
    eevdf_charge(&sched_runq[cpu], &current, 200);
    current.state = PROC_RUNNING;
    irq_enabled = saved;
}
'''
HARNESS = r'''
int main(void) {
    for (cpu = 0; cpu < 2; cpu++) {
        for (int enabled = 0; enabled < 2; enabled++) {
            for (int interrupt = 0; interrupt < 2; interrupt++) {
#ifndef CONFIG_KERNEL_PREEMPT
                /* Cooperative kernel profiles do not take this nested IRQ. */
                if (interrupt) continue;
#endif
                current = (task_t){.pid=4, .state=PROC_RUNNING, .on_cpu=1,
                    .owner_cpu=cpu, .cfs_weight=1024, .eevdf_last_account=50};
                sched_runq[cpu] = (proc_runq_t){.eevdf_weight=1024};
                irq_enabled = enabled; inject = interrupt; pending = 0;
                proc_yield();
                if (current.eevdf_vruntime != 150 ||
                    current.eevdf_last_account != 200 ||
                    sched_runq[cpu].eevdf_vtime != 150 ||
                    irq_enabled != enabled || current.park_lock) {
                    printf("incorrect accounting cpu=%u irq=%d inject=%d runtime=%llu vtime=%llu\n",
                        cpu, enabled, interrupt,
                        (unsigned long long)current.eevdf_vruntime,
                        (unsigned long long)sched_runq[cpu].eevdf_vtime);
                    return 23;
                }
            }
        }
    }
    return 0;
}
'''

def main():
    source = (ROOT / 'kernel/proc/sched.c').read_text()
    bodies = '\n'.join(extract_function(source, signature) for signature in [
        'static inline uint64_t eevdf_weight(task_t *t)',
        'static inline uint64_t eevdf_vslice(task_t *t)',
        'static void eevdf_account_run(task_t *t, uint64_t dt)',
        'static void eevdf_charge(proc_runq_t *rq, task_t *t, uint64_t now)',
    ])
    yielding = extract_function(source, 'void proc_yield(void)')
    # Restore only the original location of clock sampling/accounting.
    masked_start = yielding.index('#ifdef CONFIG_KERNEL_PREEMPT')
    masked_end = yielding.index('#endif', masked_start) + len('#endif')
    legacy = yielding[:masked_start] + yielding[masked_end:]
    legacy = legacy.replace('#ifndef CONFIG_KERNEL_PREEMPT\n', '', 1)
    legacy = legacy.replace('#endif\n', '', 1)
    with tempfile.TemporaryDirectory(prefix='a20-yield-accounting-') as directory:
        directory = Path(directory)
        for name, code, flags, expected in [
            ('preempt', yielding, ['-DCONFIG_KERNEL_PREEMPT=1'], 0),
            ('cooperative', yielding, [], 0),
            ('legacy-negative', legacy, ['-DCONFIG_KERNEL_PREEMPT=1'], 23),
        ]:
            src, exe = directory / (name + '.c'), directory / name
            src.write_text(PRELUDE + bodies + '\n' + code + HARNESS)
            subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                            '-Werror', *flags, str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{name}: expected exit {expected}, got {result.returncode}')
    print('test-sched-yield-accounting: PASS (real bodies, IRQ restoration, both CPUs, legacy negative)')

if __name__ == '__main__':
    main()

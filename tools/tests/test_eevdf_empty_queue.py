"""Exercise real fair accounting/enqueue bodies with an incumbent and newcomer."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <stdint.h>
#include <stdlib.h>
typedef struct node { struct node *left, *right, *parent; void *owner; unsigned heap_prio; } eevdf_node_t;
typedef struct {
    uint64_t eevdf_vruntime, eevdf_deadline, eevdf_last_account;
    unsigned cfs_weight, pi_boost_weight;
    int pid;
    eevdf_node_t eevdf_node;
} task_t;
typedef struct {
    uint64_t eevdf_weight, eevdf_vtime;
    eevdf_node_t *eevdf_root;
    task_t *eevdf_first, *eevdf_last;
} proc_runq_t;
#define EEVDF_MAX_LAG 10
extern uint64_t a20_eevdf_effective_weight(uint32_t, uint32_t);
extern uint64_t a20_eevdf_advance_vruntime(uint64_t, uint64_t, uint64_t);
extern uint64_t a20_eevdf_advance_vtime(uint64_t, uint64_t, uint64_t);
extern uint64_t a20_eevdf_clamp_sleeper(uint64_t, uint64_t, uint64_t);
static int sched_task_rt(task_t *t) { (void)t; return 0; }
static int sched_task_dl(task_t *t) { (void)t; return 0; }
static void panic(const char *p) { (void)p; abort(); }
static unsigned eevdf_heap_prio(void) { return 1; }
static void eevdf_reset_deadline(task_t *t) { t->eevdf_deadline = t->eevdf_vruntime + 10; }
static void eevdf_account_run(task_t *t, uint64_t dt) {
    t->eevdf_vruntime = a20_eevdf_advance_vruntime(t->eevdf_vruntime, dt, t->cfs_weight);
    eevdf_reset_deadline(t);
}
static uint32_t a20_eevdf_tree_insert(eevdf_node_t **root, eevdf_node_t *node,
    void *owner, uint64_t deadline, uint64_t runtime, uint32_t priority) {
    (void)deadline; (void)runtime; (void)priority;
    node->owner = owner; *root = node; return 1;
}
static void *a20_eevdf_tree_first(eevdf_node_t *root) { return root->owner; }
static void *a20_eevdf_tree_last(eevdf_node_t *root) { return root->owner; }
'''
HARNESS = r'''
int main(int argc, char **argv) {
    (void)argv;
    task_t incumbent = {.pid=1, .cfs_weight=1024, .eevdf_vruntime=1000000,
                        .eevdf_last_account=100};
    proc_runq_t rq = {.eevdf_vtime=1000000};
    if (argc == 1) {
        /* A running task is not in the tree. Its lone CPU time must still
         * advance virtual time, so later arrivals see a current clock. */
        eevdf_charge(&rq, &incumbent, 150);
        if (rq.eevdf_vtime != incumbent.eevdf_vruntime) return 33;
    } else {
        task_t newcomer = {.pid=2, .cfs_weight=1024};
        eevdf_insert_locked(&rq, &newcomer);
        /* The queued newcomer may lead by one bounded sleeper bonus, never
         * by the incumbent's entire history (one million ticks here). */
        if (newcomer.eevdf_vruntime + EEVDF_MAX_LAG < incumbent.eevdf_vruntime)
            return 33;
        if (newcomer.eevdf_vruntime > incumbent.eevdf_vruntime) return 34;
    }
    return 0;
}
'''


class EevdfEmptyQueue(unittest.TestCase):
    def test_clock_and_arrival_credit_with_running_incumbent(self):
        source = (ROOT / 'kernel/proc/sched.c').read_text()
        weight = extract_function(source, 'static inline uint64_t eevdf_weight(task_t *t)')
        charge = extract_function(source, 'static void eevdf_charge(proc_runq_t *rq, task_t *t, uint64_t now)')
        enqueue = extract_function(source, 'static void eevdf_insert_locked(proc_runq_t *rq, task_t *t)')
        with tempfile.TemporaryDirectory(prefix='a20-eevdf-empty-') as d:
            path = Path(d)
            obj = path / 'math.o'
            subprocess.run(['rustc', '--edition=2021', '--crate-type=lib', '--emit=obj',
                            '-C', 'panic=abort', '-C', 'opt-level=2',
                            str(ROOT / 'kernel/proc/eevdf_math.rs'), '-o', str(obj)], check=True)
            versions = [
                ('fixed', charge, enqueue, [0, 0]),
                ('missing-current', charge.replace('rq->eevdf_weight + eevdf_weight(t)',
                                                   'rq->eevdf_weight'), enqueue, [33, 0]),
                ('unbounded-arrival', charge, enqueue.replace(
                    '    {\n        uint64_t clamped',
                    '    if (rq->eevdf_root) {\n        uint64_t clamped'), [0, 33]),
            ]
            for name, accounting, insert, expected in versions:
                c, binary = path / (name + '.c'), path / name
                c.write_text(PRELUDE + weight + accounting + insert + HARNESS)
                subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                str(c), str(obj), '-o', str(binary)], check=True)
                for args, wanted in zip(([], ['enqueue']), expected):
                    with self.subTest(version=name, args=args):
                        self.assertEqual(subprocess.run([str(binary), *args]).returncode, wanted)


if __name__ == '__main__':
    unittest.main()

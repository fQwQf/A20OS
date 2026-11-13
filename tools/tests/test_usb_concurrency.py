"""Exercise lifecycle topology coordination with real pthread contention."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE

# Blocking acquisition is recursive; native mutex_trylock is deliberately not.
LOCKS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include "core/types.h"
#define _CORE_LOCK_H
#define _CORE_SYNC_H
#define _CORE_CPU_H
typedef pthread_mutex_t spinlock_t;
typedef struct {pthread_mutex_t lock;unsigned depth;} mutex_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define MUTEX_INIT {PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP,0}
static void spin_init(spinlock_t *l) {assert(!pthread_mutex_init(l,NULL));}
static uint64_t spin_lock_irqsave(spinlock_t *l) {assert(!pthread_mutex_lock(l));return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {(void)f;assert(!pthread_mutex_unlock(l));}
static void mutex_init(mutex_t *m) {
 pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));
 assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));
 assert(!pthread_mutex_init(&m->lock,&a));m->depth=0;assert(!pthread_mutexattr_destroy(&a));
}
static int mutex_trylock(mutex_t *m) {
 if(pthread_mutex_trylock(&m->lock))return 0;
 if(m->depth){assert(!pthread_mutex_unlock(&m->lock));return 0;}
 m->depth=1;return 1;
}
static void mutex_lock(mutex_t *m) {assert(!pthread_mutex_lock(&m->lock));m->depth++;}
static void mutex_unlock(mutex_t *m) {assert(m->depth);m->depth--;assert(!pthread_mutex_unlock(&m->lock));}
static unsigned cpu_current_id(void) {return 0;}
'''


class UsbConcurrency(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-usb-concurrency-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = LOCKS + C_FIXTURE[C_FIXTURE.index('#include "drivers/core/driver_core.c"'):]
        source = source.replace('struct task_t *proc_current(void) {return NULL;}',
                                'extern _Thread_local char task_identity;\nextern _Thread_local int boot_context;\nstruct task_t *proc_current(void) {return boot_context ? NULL : (void *)&task_identity;}')
        marker = 'int main(int argc,char **argv) {'
        model = (ROOT / 'tools/tests/usb_concurrency_host.c').read_text().replace('static _Thread_local', '_Thread_local')
        source = source.replace(marker, model + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"concurrent-",11))concurrency_case(argv[1]+11);\n else if(!strcmp(argv[1],"pci-retired"))')
        fixture = work / 'fixture.c'
        fixture.write_text(source)
        cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch/x86_64/include'),
                        '-I'+str(ROOT / 'kernel/include'), '-I'+str(ROOT / 'kernel'),
                        str(fixture), '-o', str(cls.binary)], check=True)

    def run_cases(self, *cases):
        for case in cases:
            with self.subTest(case=case):
                subprocess.run([str(self.binary), 'concurrent-'+case], check=True, timeout=15)

    def test_lifecycle_try_and_blocking_entries_preserve_probe_policy(self):
        self.run_cases('lifecycle', 'boot')

    def test_progress_and_class_poll_skip_a_busy_lifecycle_without_waiting(self):
        self.run_cases('busy')

    def test_scan_and_enumeration_hold_graph_until_unregister_can_proceed(self):
        self.run_cases('scan', 'enumerate')

    def test_reentrant_mutation_duplicate_registration_and_parent_lifetime(self):
        self.run_cases('registry')

    def test_stopping_ancestor_denies_poll_enumeration_and_new_descendants(self):
        self.run_cases('stopping-parent', 'reset-stopping')

    def test_capacity_is_reserved_across_reentrant_hcd_start(self):
        self.run_cases('capacity')

    def test_scan_reselects_registry_after_callback_compaction_and_addition(self):
        self.run_cases('compaction')

    def test_retiring_port_blocks_shared_controller_allocation_without_retry_charge(self):
        self.run_cases('retire', 'reset-retiring')

"""Explicit requester-set leases with production PCI/core and real threads."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

if __package__:
    from .test_native_bus_lifetime import ROOT, C_FIXTURE
else:
    from test_native_bus_lifetime import ROOT, C_FIXTURE


LOCKS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include "core/types.h"
#define _CORE_LOCK_H
#define _CORE_SYNC_H
#define _CORE_CPU_H
typedef pthread_mutex_t spinlock_t;
typedef pthread_mutex_t mutex_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
static void spin_init(spinlock_t *l) {assert(!pthread_mutex_init(l,NULL));}
static uint64_t spin_lock_irqsave(spinlock_t *l) {assert(!pthread_mutex_lock(l));return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {(void)f;assert(!pthread_mutex_unlock(l));}
static void mutex_init(mutex_t *l) {
 pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));
 assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));
 assert(!pthread_mutex_init(l,&a));assert(!pthread_mutexattr_destroy(&a));
}
static int mutex_trylock(mutex_t *l) {return !pthread_mutex_trylock(l);}
static void mutex_lock(mutex_t *l) {assert(!pthread_mutex_lock(l));}
static void mutex_unlock(mutex_t *l) {assert(!pthread_mutex_unlock(l));}
static unsigned cpu_current_id(void) {return 0;}
'''

CASES = r'''
static void requester_failure(device_t *owner,uint16_t segment,const uint16_t *ids,size_t n,int expected) {
 pci_requester_reservation_t *lease=(void *)1;unsigned refs=owner?owner->references:0;
 int before=allocated;
 assert(pci_requester_reserve(owner,segment,ids,n,&lease)==expected&&!lease);
 assert(allocated==before&&(!owner||owner->references==refs));
}
static pthread_barrier_t requester_start, requester_ready, requester_finish;
static void requester_barrier(pthread_barrier_t *b) {
 int ret=pthread_barrier_wait(b);assert(!ret||ret==PTHREAD_BARRIER_SERIAL_THREAD);
}
struct requester_worker {
 device_t *owner;
 uint16_t ids[2];
 int result;
 pci_requester_reservation_t *lease;
};
static void *requester_compete(void *context) {
 struct requester_worker *w=context;requester_barrier(&requester_start);
 w->result=pci_requester_reserve(w->owner,0,w->ids,2,&w->lease);
 requester_barrier(&requester_ready);requester_barrier(&requester_finish);
 pci_requester_release(w->lease);return NULL;
}
static void *requester_unregister(void *context) {
 requester_barrier(&requester_start);device_unregister(context);return NULL;
}
static void *requester_race_reserve(void *context) {
 struct requester_worker *w=context;requester_barrier(&requester_start);
 w->result=pci_requester_reserve(w->owner,0,w->ids,2,&w->lease);return NULL;
}
static void requester_case(const char *name) {
 device_t a={.name="requester-a",.references=1},b={.name="requester-b",.references=1};
 assert(!device_register(&a)&&!device_register(&b));
 uint16_t ids[]={0xffff,0x20,0,0x20},other[]={0x30,0x20},free_id=0x30;
 pci_requester_reservation_t *lease=NULL,*second=NULL;
 if(!strcmp(name,"arguments")) {
  requester_failure(NULL,0,ids,1,-EINVAL);requester_failure(&a,0,NULL,1,-EINVAL);
  requester_failure(&a,0,ids,0,-EINVAL);requester_failure(&a,1,ids,1,-EOPNOTSUPP);
  requester_failure(&a,0,ids,PCI_REQUESTER_SET_MAX+1,-E2BIG);
  assert(pci_requester_reserve(&a,0,ids,1,NULL)==-EINVAL);pci_requester_release(NULL);
 } else if(!strcmp(name,"copy")) {
  assert(!pci_requester_reserve(&a,0,ids,4,&lease)&&a.references==3);
  memset(ids,0xaa,sizeof(ids));
  uint16_t taken[]={0,0x20,0xffff};
  for(size_t i=0;i<3;i++)requester_failure(&b,0,&taken[i],1,-EBUSY);
  uint16_t vacant=0xaaaa;assert(!pci_requester_reserve(&b,0,&vacant,1,&second));
  pci_requester_release(second);pci_requester_release(lease);
 } else if(!strcmp(name,"atomic")) {
  assert(!pci_requester_reserve(&a,0,ids,4,&lease));
  requester_failure(&a,0,other,2,-EBUSY);requester_failure(&b,0,other,2,-EBUSY);
  assert(!pci_requester_reserve(&b,0,&free_id,1,&second));
  pci_requester_release(lease);pci_requester_release(second);
  assert(!pci_requester_reserve(&a,0,other,2,&lease));pci_requester_release(lease);
 } else if(!strcmp(name,"lifetime")) {
  assert(!pci_requester_reserve(&a,0,ids,4,&lease));device_unregister(&a);
  assert(!device_is_registered(&a)&&a.references==2);
  requester_failure(&a,0,&free_id,1,-ENODEV);requester_failure(&b,0,ids,4,-EBUSY);
  pci_requester_release(lease);assert(a.references==1);
  assert(!pci_requester_reserve(&b,0,ids,4,&lease));pci_requester_release(lease);
 } else if(!strcmp(name,"allocation")) {
  fail_at=allocations+1;requester_failure(&a,0,ids,4,-ENOMEM);fail_at=0;
  assert(!pci_requester_reserve(&b,0,ids,4,&lease));pci_requester_release(lease);
 } else if(!strcmp(name,"maximum")) {
  uint16_t keys[PCI_REQUESTER_SET_MAX];
  for(unsigned i=0;i<PCI_REQUESTER_SET_MAX;i++)keys[i]=0xffff-i;
  assert(!pci_requester_reserve(&a,0,keys,PCI_REQUESTER_SET_MAX,&lease));
  uint16_t end=0xffff-(PCI_REQUESTER_SET_MAX-1);
  requester_failure(&b,0,&end,1,-EBUSY);end--;
  assert(!pci_requester_reserve(&b,0,&end,1,&second));
  pci_requester_release(lease);pci_requester_release(second);
 } else if(!strcmp(name,"concurrent")) {
  assert(!pthread_barrier_init(&requester_start,NULL,3));
  assert(!pthread_barrier_init(&requester_ready,NULL,3));
  assert(!pthread_barrier_init(&requester_finish,NULL,3));
  for(unsigned i=0;i<64;i++) {
   struct requester_worker x={.owner=&a,.ids={0x20,0x10}},y={.owner=&b,.ids={0x30,0x20}};
   pthread_t t,u;assert(!pthread_create(&t,NULL,requester_compete,&x));
   assert(!pthread_create(&u,NULL,requester_compete,&y));
   requester_barrier(&requester_start);requester_barrier(&requester_ready);
   assert((!x.result&&y.result==-EBUSY)||(x.result==-EBUSY&&!y.result));
   uint16_t unused=x.result?0x10:0x30;
   assert(!pci_requester_reserve(&a,0,&unused,1,&second));pci_requester_release(second);
   requester_barrier(&requester_finish);assert(!pthread_join(t,NULL)&&!pthread_join(u,NULL));
  }
  assert(!pthread_barrier_destroy(&requester_start)&&!pthread_barrier_destroy(&requester_ready));
  assert(!pthread_barrier_destroy(&requester_finish));
 } else if(!strcmp(name,"unregister-race")) {
  assert(!pthread_barrier_init(&requester_start,NULL,3));
  for(unsigned i=0;i<64;i++) {
   struct requester_worker x={.owner=&a,.ids={0x10,0x20}};pthread_t t,u;
   assert(!pthread_create(&t,NULL,requester_race_reserve,&x));
   assert(!pthread_create(&u,NULL,requester_unregister,&a));requester_barrier(&requester_start);
   assert(!pthread_join(t,NULL)&&!pthread_join(u,NULL));
   assert(!device_is_registered(&a));
   if(!x.result){assert(x.lease);requester_failure(&b,0,x.ids,2,-EBUSY);}
   else assert(x.result==-ENODEV&&!x.lease);
   pci_requester_release(x.lease);assert(a.references==1);assert(!device_register(&a));
  }
  assert(!pthread_barrier_destroy(&requester_start));
 } else assert(0);
 assert(!pci_requester_reservations);device_unregister(&a);device_unregister(&b);
 assert(a.references==1&&b.references==1);
}
'''


class PciRequesterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        machine = platform.machine().lower()
        if machine in ('x86_64', 'amd64'):
            arch, macro = 'x86_64', 'CONFIG_X86_64'
        elif machine in ('aarch64', 'arm64'):
            arch, macro = 'aarch64', 'CONFIG_AARCH64'
        else:
            raise unittest.SkipTest('host architecture headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-pci-requester-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = LOCKS + C_FIXTURE[C_FIXTURE.index('#include "drivers/core/driver_core.c"'):]
        old = 'struct task_t *proc_current(void) {return NULL;}'
        assert source.count(old) == 1
        source = source.replace(old, 'static _Thread_local char task;\nstruct task_t *proc_current(void) {return (struct task_t *)&task;}')
        source = source.replace('int main(int argc,char **argv) {', CASES + '\nint main(int argc,char **argv) {')
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))', 'if(!strncmp(argv[1],"requester-",10))requester_case(argv[1]+10);\n else if(!strcmp(argv[1],"pci-retired"))')
        path = work / 'fixture.c'
        path.write_text(source)
        cls.binary = work / 'test'
        subprocess.run([
            'cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
            '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-D' + macro, '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
            '-I' + str(ROOT / 'kernel/arch' / arch / 'include'),
            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel'),
            str(path), '-o', str(cls.binary),
        ], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), 'requester-' + name], check=True, timeout=15)

    def test_arguments_and_supported_segment(self):
        self.run_case('arguments')

    def test_copied_set_duplicates_and_full_requester_bits(self):
        self.run_case('copy')

    def test_atomic_conflicts_and_same_owner_exclusion(self):
        self.run_case('atomic')

    def test_owner_pin_survives_core_unregistration(self):
        self.run_case('lifetime')

    def test_failed_allocation_publishes_nothing(self):
        self.run_case('allocation')

    def test_bounded_maximum_descending_set(self):
        self.run_case('maximum')

    def test_competing_atomic_sets_with_real_threads(self):
        self.run_case('concurrent')

    def test_reservation_vs_core_unregistration(self):
        self.run_case('unregister-race')


if __name__ == '__main__':
    unittest.main()

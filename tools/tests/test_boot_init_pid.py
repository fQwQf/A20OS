"""Early workers must never consume PID1, including allocator wrap and races."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define PID_MAX_LIMIT 16
#define PID_BITMAP_SIZE 1
#define PID_HASH_SIZE 2
typedef void task_t;
typedef pthread_mutex_t spinlock_t;
static spinlock_t pid_lock=PTHREAD_MUTEX_INITIALIZER;
static task_t *pid_hash[PID_HASH_SIZE];
static int next_pid=1,pid_max=PID_MAX_LIMIT,boot_init_claimed;
static uint64_t pid_bitmap[PID_BITMAP_SIZE];
static void spin_init(spinlock_t *p){(void)p;}
static uint64_t spin_lock_irqsave(spinlock_t *p){assert(!pthread_mutex_lock(p));return 0;}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)f;assert(!pthread_mutex_unlock(p));}
#define panic(...) abort()
'''
MAIN = r'''
static int claims[32];
static void *claim(void *n){claims[(uintptr_t)n]=proc_pid_claim_init();return NULL;}
int main(void){
 proc_pid_init();
 assert(proc_pid_alloc()==2); /* a device worker before userspace init */
 pthread_t threads[32];
 for(uintptr_t i=0;i<32;i++)assert(!pthread_create(&threads[i],NULL,claim,(void*)i));
 int success=0;
 for(unsigned i=0;i<32;i++){pthread_join(threads[i],NULL);assert(claims[i]==1||claims[i]==-EEXIST);success+=claims[i]==1;}
 assert(success==1);
 for(int i=3;i<=PID_MAX_LIMIT;i++)assert(proc_pid_alloc()==i);
 assert(proc_pid_alloc()==-EAGAIN);
 proc_pid_release_id(1);assert(proc_pid_alloc()==-EAGAIN);
 proc_pid_release_id(5);assert(proc_pid_alloc()==5);
 assert(proc_pid_alloc()==-EAGAIN);
 proc_pid_init();assert(proc_pid_claim_init()==1);
 assert(proc_pid_alloc()==2&&proc_pid_claim_init()==-EEXIST);
 return 0;
}
'''


class BootInitPid(unittest.TestCase):
    def test_reservation_wrap_and_single_claim(self):
        source = (ROOT / 'kernel/proc/pid.c').read_text()
        names = ('pid_bitmap_set', 'pid_bitmap_clear', 'ctz64', 'pid_bitmap_find_free',
                 'pid_alloc_locked', 'proc_pid_release_id', 'proc_pid_init',
                 'proc_pid_claim_init', 'proc_pid_alloc')
        code = '\n'.join(function(source, name) for name in names)
        old = code.replace('    int reserved = pid_alloc_locked();', '    int reserved = 1;')
        self.assertNotEqual(code, old)
        with tempfile.TemporaryDirectory() as directory:
            def run(body, name):
                path = Path(directory) / (name + '.c')
                exe = path.with_suffix('')
                path.write_text(PRE + body + MAIN)
                subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', str(path), '-o', str(exe)],
                               check=True, capture_output=True)
                return subprocess.run([str(exe)], capture_output=True, timeout=5).returncode
            self.assertEqual(run(code, 'reserved'), 0)
            self.assertNotEqual(run(old, 'unreserved'), 0)


if __name__ == '__main__':
    unittest.main()

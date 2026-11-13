"""Production I2C registry references and HID workqueue teardown contract."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]


def run(code):
    with tempfile.TemporaryDirectory() as folder:
        src = Path(folder) / 'test.c'
        src.write_text(code)
        binary = src.with_suffix('')
        result = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                 '-fsanitize=undefined', str(src), '-o', str(binary)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        subprocess.run([str(binary)], check=True, timeout=10)


class I2cLifetime(unittest.TestCase):
    def test_adapter_references_block_remove_and_transfer_is_unlocked(self):
        header = (ROOT / 'kernel/include/drivers/i2c/i2c.h').read_text()
        source = (ROOT / 'kernel/drivers/i2c/i2c-core.c').read_text()
        strip = lambda text: re.sub(r'^#include.*\n', '', text, flags=re.M)
        run(r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
typedef struct {int held;} spinlock_t;
#define SPINLOCK_INIT {0}
static uint64_t spin_lock_irqsave(spinlock_t *l){assert(!l->held);l->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f){(void)f;assert(l->held);l->held=0;}
struct device {int id;};
''' + strip(header) + strip(source) + r'''
static int transfer(i2c_adapter_t *a,i2c_msg_t *m,int n){
 assert(!g_i2c_lock.held && m && n==1);
 assert(i2c_adapter_remove(a)==-EBUSY);return -ENXIO;
}
int main(void){
 struct device x={1},y={2};i2c_adapter_t a={.nr=0,.transfer=transfer},b={.nr=1,.transfer=transfer};
 assert(i2c_adapter_alloc_nr()==0);
 assert(!i2c_adapter_add_for_device(&x,&a));assert(i2c_adapter_add(&a)==-EEXIST);
 assert(!i2c_adapter_add_for_device(&y,&b));
 i2c_adapter_t *held=i2c_adapter_get_for_device(&x);assert(held==&a);
 assert(i2c_adapter_remove(&a)==-EBUSY);assert(!i2c_adapter_remove(&b));
 assert(!i2c_adapter_get(1));i2c_adapter_put(held);assert(!i2c_adapter_remove(&a));
 assert(!i2c_adapter_get_for_device(&x));assert(!i2c_adapter_remove(&a));
 assert(!i2c_adapter_add_for_device(&y,&a));i2c_msg_t msg={0};
 assert(i2c_transfer(0,&msg,1)==-ENXIO);assert(!i2c_adapter_remove(&a));
 assert(i2c_transfer(0,&msg,1)==-ENODEV);
 assert(i2c_transfer(0,NULL,1)==-EINVAL);
}
''')

    def test_hid_teardown_keeps_context_until_worker_has_exited(self):
        source = (ROOT / 'kernel/drivers/hid/i2c-hid.c').read_text()
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef struct {int held;} mutex_t;
typedef struct {void *queue,*work,*adapter,*report_desc,*input_buf;mutex_t io_lock;int present,running;} i2c_hid_device_t;
typedef struct {i2c_hid_device_t *drv_priv;} device_t;
static unsigned freed,put_work,put_adapter;static int fail;
static mutex_t *io;
#define MS_TO_TICKS(x) (x)
static uint64_t timer_get_ticks(void){return 1;}
static void mutex_lock(mutex_t *m){assert(!m->held);m->held=1;}
static void mutex_unlock(mutex_t *m){assert(m->held);m->held=0;}
static int driver_workqueue_destroy(void **q,uint64_t deadline){assert(!io->held && deadline>1);if(fail)return -ETIMEDOUT;*q=NULL;return 0;}
static void driver_work_put(void *p){assert(p);put_work++;}
static void i2c_adapter_put(void *p){assert(p);put_adapter++;}
static void kfree(void *p){assert(p);freed++;}
''' + function(source, 'ihid_remove') + r'''
int main(void){
 int token;i2c_hid_device_t h={.queue=&token,.work=&token,.adapter=&token,.report_desc=&token,.input_buf=&token,.present=1,.running=1};
 device_t d={&h};io=&h.io_lock;fail=1;
 assert(ihid_remove(&d)==-ETIMEDOUT && d.drv_priv==&h && h.queue);
 assert(!h.present&&!h.running&&!freed&&!put_work&&!put_adapter);
 fail=0;assert(!ihid_remove(&d)&&!d.drv_priv&&!h.queue);
 assert(freed==3&&put_work==1&&put_adapter==1);
 assert(!ihid_remove(&d)&&freed==3);
}
''')


if __name__ == '__main__':
    unittest.main()

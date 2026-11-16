"""Production I2C registry references and HID workqueue teardown contract."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

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

    def test_designware_checked_stop_and_partial_probe(self):
        strip = lambda text: re.sub(r'^#include.*\n', '', text, flags=re.M)
        generated = (ROOT / 'tools/tests/dw_i2c_lifecycle_host.c').read_text()
        for marker, name in [('HEADER', 'kernel/include/drivers/i2c/i2c.h'),
                             ('REGISTRY', 'kernel/drivers/i2c/i2c-core.c'),
                             ('PRODUCTION', 'kernel/drivers/i2c/i2c-designware.c')]:
            generated = generated.replace('/* ' + marker + ' */', strip((ROOT / name).read_text()))
        run(generated)

    def test_hid_teardown_keeps_context_until_worker_has_exited(self):
        source = (ROOT / 'kernel/drivers/hid/i2c-hid.c').read_text()
        strip = lambda text: re.sub(r'^#include.*\n', '', text, flags=re.M)
        generated = (ROOT / 'tools/tests/i2c_hid_lifecycle_host.c').read_text()
        for marker, name in [('HEADER', 'kernel/include/drivers/i2c/i2c.h'),
                             ('REGISTRY', 'kernel/drivers/i2c/i2c-core.c')]:
            generated = generated.replace('/* ' + marker + ' */', strip((ROOT / name).read_text()))
        names = ('ihid_xfer', 'ihid_stop', 'ihid_release', 'ihid_probe',
                 'ihid_remove', 'ihid_poll_work')
        callbacks = []
        for name in names:
            found = re.search(r'^static [^;{}]*\b' + name +
                              r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
            self.assertIsNotNone(found, name)
            callbacks.append(found.group())
        callbacks = '\n'.join(callbacks)
        run(generated.replace('/* CALLBACKS */', callbacks))


if __name__ == '__main__':
    unittest.main()

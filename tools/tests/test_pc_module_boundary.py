"""Build the real PC module wrappers and audit their loader API imports."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from tools.tests.test_i2c_transport_lifetime import run, ROOT
from tools.tests.test_usb_hid_readiness import function


class PcModuleBoundary(unittest.TestCase):
    @unittest.skipUnless(shutil.which('x86_64-linux-gnu-gcc'), 'x86 compiler unavailable')
    def test_shared_module_imports_are_exported(self):
        exports = set(re.findall(r'\{\s*"([^"\n]+)"\s*,',
                                (ROOT/'kernel/drvmod/framework.c').read_text()))
        with tempfile.TemporaryDirectory(prefix='a20-pc-modules-') as tmp:
            for name in ('virtio_gpu', 'nvme', 'ps2', 'r8169', 'xhci', 'usb_hid', 'usb_hub', 'usb_storage'):
                with self.subTest(module=name):
                    output = Path(tmp)/(name+'.a20drv')
                    subprocess.run(['x86_64-linux-gnu-gcc', '-ffreestanding', '-nostdlib',
                        '-mno-red-zone', '-mgeneral-regs-only', '-fno-pic', '-fno-pie',
                        '-mcmodel=large', '-DCONFIG_X86_64', '-DCONFIG_64BIT',
                        '-DCONFIG_NR_CPUS=16', '-DCONFIG_DRIVER_MODULE', '-std=gnu99',
                        '-I'+str(ROOT/'kernel/arch/x86_64/include'),
                        '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'kernel'),
                        '-c', str(ROOT/'kernel/drvmod/examples'/(name+'.c')),
                        '-o', str(output)], check=True, capture_output=True)
                    imports = {line.split()[-1] for line in subprocess.check_output(
                        ['nm', '-u', str(output)], text=True).splitlines()}
                    self.assertFalse(imports-exports, f'unresolved loader imports: {imports-exports}')
                    symbols = subprocess.check_output(['nm', str(output)], text=True)
                    self.assertRegex(symbols, r'\bT DriverEntry\b')

    def test_legacy_irq_route_uses_architecture_adapter(self):
        source = (ROOT/'kernel/arch/x86_64/trap/irqchip.c').read_text()
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#define PLATFORM_IRQ_MAX_LINES 256
static void host_log(const char *fmt,...){(void)fmt;}
#define kinfo host_log
static int route, forced, calls;
static int arch_firmware_irq_map(uint32_t h,uint32_t f){assert(h==1&&f==1);return route;}
static int x86_64_route_isa_irq_forced(uint32_t h,uint32_t v){assert(h==1&&v==33);calls++;return forced;}
''' + function(source, 'firmware_legacy_irq_map') + r'''
int main(void){
 assert(firmware_legacy_irq_map(1,256)==-EINVAL);
 route=33;assert(firmware_legacy_irq_map(1,33)==33&&!calls);
 route=1;forced=0;assert(firmware_legacy_irq_map(1,33)==33&&calls==1);
 route=-1;forced=-1;assert(firmware_legacy_irq_map(1,33)==33&&calls==2);
}
''')


if __name__ == '__main__':
    unittest.main()

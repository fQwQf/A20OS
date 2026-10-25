"""A cached virtio-pci kick must not write emulated queue-selection registers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class VirtioNotifyTest(unittest.TestCase):
    def test_cached_doorbell_does_not_reselect_or_reread_offset(self):
        fixture=r'''
#include <assert.h>
#include "drivers/bus/pci_bus.c"
int main(void) {
 unsigned char common[128] __attribute__((aligned(8)))={0};
 unsigned short notify[64]={0};
 pci_virtio_transport_t pci={.common=(uintptr_t)common,.notify=(uintptr_t)notify,.notify_multiplier=4};
 virtio_transport_t transport={.priv=&pci};
 *(uint16_t *)(common+PCOMMON_QUEUE_NOTIFY_OFF)=3;
 pci_virtio_write32(&transport,VIRTIO_MMIO_QUEUE_NOTIFY,1);
 assert(pci.notify_off_cached&(1U<<1));assert(pci.notify_off_cache[1]==3);
 assert(notify[6]==1);
 *(uint16_t *)(common+PCOMMON_QUEUE_SEL)=7;
 *(uint16_t *)(common+PCOMMON_QUEUE_NOTIFY_OFF)=63;
 pci_virtio_write32(&transport,VIRTIO_MMIO_QUEUE_NOTIFY,1);
 assert(*(uint16_t *)(common+PCOMMON_QUEUE_SEL)==7);assert(notify[6]==1);
 pci.notify_off_cache[0]=5;pci.notify_off_cached|=1;
 notify[10]=123;
 pci_virtio_write32(&transport,VIRTIO_MMIO_QUEUE_NOTIFY,0);
 assert(notify[10]==0);assert(*(uint16_t *)(common+PCOMMON_QUEUE_SEL)==7);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'notify.c';source.write_text(fixture);exe=Path(tmp)/'notify'
            subprocess.run(['cc','-std=gnu99','-O2','-Wall','-Wextra','-Werror',
                '-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-DCONFIG_X86_64','-DCONFIG_NR_CPUS=1',f'-I{ROOT}/kernel/arch/x86_64/include',
                f'-I{ROOT}/kernel/include',f'-I{ROOT}/kernel',str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':
    unittest.main()

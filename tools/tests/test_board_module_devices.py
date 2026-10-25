"""Board deployment policy uses canonical devices and static provider fallbacks."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARDS = {
    'rv64': 'riscv64/boards/qemu-virt-riscv64',
    'aa64': 'aarch64/boards/qemu-virt-aarch64',
    'la64': 'loongarch64/boards/qemu-virt-loongarch64',
    'x86': 'x86_64/boards/qemu-virt-x86_64',
    'pc': 'x86_64/boards/x86_64-pc',
    'vf2': 'riscv64/boards/visionfive2/fallback_devices.c',
}


class BoardModuleDevices(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='a20-module-devices-')
        work = Path(cls.tmp.name)
        stub = work / 'drivers/core/driver_core.h'
        stub.parent.mkdir(parents=True)
        stub.write_text('''#pragma once
#include "core/types.h"
#define IORESOURCE_MMIO_32BIT 4
struct device { int user_owned; uint64_t physical; const char *id; int refs;
                int irq_count; uint32_t irq; const char *irq_provider; };
typedef struct device device_t;
device_t *device_find_get_by_mmio(uint64_t);
device_t *device_find_get_by_firmware_id(const char *);
void device_put(device_t *);
''')
        (work / 'core').mkdir()
        (work / 'core/panic.h').write_text('void panic(const char *, ...) __attribute__((noreturn));\n')
        (work / 'host.c').write_text('''
#include <stdlib.h>
#include <string.h>
#include "drivers/core/driver_core.h"
#include "firmware/device.h"
#include "boot/context.h"
static device_t devices[16]; static int count, created, gui;
static boot_context_t context;
const boot_context_t *boot_context_get(void) { return &context; }
int fw_node_valid(const fw_node_t *node) { return node && node->ops; }
void firmware_present(void) { static const fw_node_ops_t ops={0};context.hardware_root.ops=&ops; }
void reset(void) { memset(devices, 0, sizeof(devices)); memset(&context,0,sizeof(context));count=created=gui=0; }
void gui_mode(int enabled) { gui=enabled; }
void existing(uint64_t physical, const char *id) {
 devices[count++] = (device_t){.physical=physical, .id=id};
}
device_t *device_find_get_by_mmio(uint64_t pa) {
 for(int i=0;i<count;i++) if(pa==devices[i].physical) { devices[i].refs++; return &devices[i]; }
 return NULL;
}
device_t *device_find_get_by_firmware_id(const char *id) {
 for(int i=0;i<count;i++) if(devices[i].id && !strcmp(id,devices[i].id)) { devices[i].refs++; return &devices[i]; }
 return NULL;
}
void device_put(device_t *dev) { dev->refs--; }
int udriver_mmio_user_owned(uint64_t pa) {
 return pa==0x101000 || pa==0x10004000 || (pa==0x10006000 && !gui);
}
void panic(const char *s, ...) { (void)s; abort(); }
int firmware_device_enumerate(const fw_node_t *root) {
 fw_node_t child, previous; int have=0, rc;
 while(!(rc=root->ops->next_child(root,have?&previous:NULL,&child))) {
  previous=child; have=1;
  if(child.ops->available(&child)!=1) continue;
  fw_property_t p; fw_resource_t r;
  if(child.ops->property(&child,"compatible",&p) && child.ops->property(&child,"hardware-id",&p)) abort();
  uint64_t pa=0;
  if(!child.ops->resource(&child,0,&r) && r.type==FW_RESOURCE_MEMORY) pa=r.address;
  existing(pa,p.data); created++;
  fw_reference_t interrupt;
  if(!child.ops->irq(&child,0,&interrupt)) {
   device_t *d=&devices[count-1];d->irq_count=1;d->irq=interrupt.args[0];
   if(!interrupt.node.ops->property(&interrupt.node,"compatible",&p))d->irq_provider=p.data;
  }
 }
 return created;
}
int created_count(void) { return created; }
int device_count(void) { return count; }
int owned(unsigned i) { return i<(unsigned)count ? devices[i].user_owned : -1; }
int references(void) { int n=0;for(int i=0;i<count;i++)n+=devices[i].refs;return n; }
int irq_count(unsigned i) { return i<(unsigned)count ? devices[i].irq_count : -1; }
int irq_number(unsigned i) { return i<(unsigned)count ? (int)devices[i].irq : -1; }
const char *irq_provider(unsigned i) { return i<(unsigned)count ? devices[i].irq_provider : NULL; }
uint64_t physical(unsigned i) { return i<(unsigned)count ? devices[i].physical : 0; }
''')
        cls.libs = {}
        for key, board in BOARDS.items():
            output = work / (key+'.so')
            source=ROOT/'kernel/arch'/board
            if source.suffix!='.c':source=source/'module_devices.c'
            subprocess.run(['cc','-shared','-fPIC','-std=gnu99','-fno-builtin',
                '-Wall','-Wextra','-Werror','-I',str(work),'-I',str(ROOT/'kernel/include'),
                '-Dvf2_enumerate_static_devices=board_configure_devices',
                str(work/'host.c'),str(ROOT/'kernel/firmware/static.c'),
                str(source),'-o',str(output)],check=True)
            lib=ctypes.CDLL(str(output));lib.existing.argtypes=[ctypes.c_uint64,ctypes.c_char_p]
            lib.irq_provider.restype=ctypes.c_char_p
            lib.physical.restype=ctypes.c_uint64
            cls.libs[key]=lib

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def configure(self,key,existing=(),gui=False):
        lib=self.libs[key];lib.reset()
        for physical,identity in existing: lib.existing(physical,identity)
        lib.gui_mode(gui);lib.board_configure_devices()
        self.assertEqual(lib.references(),0,'every canonical lookup must release its reference')
        return lib

    def test_rv64_firmware_nodes_are_reused_with_ownership(self):
        lib=self.configure('rv64',[(0x101000,b'google,goldfish-rtc'),
            (0x10004000,b'virtio,mmio'),(0x10006000,b'virtio,mmio')])
        self.assertEqual(lib.created_count(),0)
        self.assertEqual([lib.owned(i) for i in range(3)],[1,1,1])

    def test_rv64_gui_slot5_exception_is_preserved(self):
        lib=self.configure('rv64',[(0x101000,b'google,goldfish-rtc'),
            (0x10004000,b'virtio,mmio'),(0x10006000,b'virtio,mmio')],gui=True)
        self.assertEqual([lib.owned(i) for i in range(3)],[1,1,0])

    def test_rv64_no_firmware_uses_static_provider(self):
        lib=self.configure('rv64')
        self.assertEqual(lib.created_count(),3)
        self.assertEqual([lib.owned(i) for i in range(3)],[1,1,1])
        lib.board_configure_devices()
        self.assertEqual(lib.created_count(),3,'reconfiguration must reuse static fallback nodes')
        self.assertEqual(lib.references(),0)

    def test_aa64_has_input_policy_without_fictitious_goldfish(self):
        lib=self.configure('aa64',[(0xa000a00,b'virtio,mmio')])
        self.assertEqual((lib.created_count(),lib.device_count(),lib.owned(0)),(0,1,1))
        lib=self.configure('aa64');self.assertEqual(lib.created_count(),1)

    def test_la64_does_not_invent_an_rv64_rtc(self):
        lib=self.configure('la64');self.assertEqual(lib.device_count(),0)
        lib=self.configure('la64',[(0x123000,b'google,goldfish-rtc')])
        self.assertEqual((lib.created_count(),lib.owned(0)),(0,1))

    def test_x86_canonical_ps2_and_fallback(self):
        for key in ['x86','pc']:
            lib=self.configure(key,[(0,b'PNP0303')]);self.assertEqual(lib.created_count(),0)
            lib=self.configure(key);self.assertEqual(lib.created_count(),1)

    def test_vf2_fallback_has_physical_resources_and_typed_plic_irq(self):
        lib=self.configure('vf2')
        self.assertEqual(lib.created_count(),3)
        self.assertEqual([lib.physical(i) for i in (1,2)],[0x16020000,0x16040000])
        self.assertEqual(lib.irq_count(1),0,'SDIO fallback must retain polling')
        self.assertEqual((lib.irq_count(2),lib.irq_number(2),lib.irq_provider(2)),
                         (1,78,b'riscv,plic0'))

    def test_vf2_valid_firmware_never_creates_static_duplicates(self):
        lib=self.libs['vf2'];lib.reset();lib.firmware_present();lib.board_configure_devices()
        self.assertEqual(lib.created_count(),0)

if __name__=='__main__': unittest.main()

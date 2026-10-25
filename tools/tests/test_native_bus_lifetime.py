"""PCI/USB lifetime behavior with the production bus and driver-core code.

Only hardware, scheduler synchronization, allocation, and the class publication
backend are host substitutes. Registration, reference counting, discovery,
unbind/devres ordering, native bus publication and rollback are production C.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
C_FIXTURE = r'''
#include <assert.h>
#include "core/types.h"
/* No privileged IRQ/scheduler instructions in this single-threaded fixture. */
#define _CORE_LOCK_H
#define _CORE_SYNC_H
#define _CORE_CPU_H
typedef struct { unsigned held; } spinlock_t;
typedef struct { unsigned held; } mutex_t;
#define SPINLOCK_INIT {0}
#define MUTEX_INIT {0}
static void spin_init(spinlock_t *l) {l->held=0;}
static uint64_t spin_lock_irqsave(spinlock_t *l) {assert(!l->held);l->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) {(void)f;assert(l->held);l->held=0;}
static void mutex_init(mutex_t *l) {l->held=0;}
static int mutex_trylock(mutex_t *l) {if(l->held)return 0;l->held=1;return 1;}
static void mutex_lock(mutex_t *l) {assert(!l->held);l->held=1;}
static void mutex_unlock(mutex_t *l) {assert(l->held);l->held=0;}
static unsigned cpu_current_id(void) {return 0;}
#include "drivers/core/driver_core.c"
#include "drivers/bus/pci_bus.c"
#include "drivers/usb/core/usb_core.c"
extern void *malloc(size_t);
extern void *realloc(void *,size_t);
extern void free(void *);
extern void abort(void) __attribute__((noreturn));
struct allocation { uint64_t magic; size_t bytes; };
static int allocated, allocations, fail_at;
static int probes, removes, expected_interfaces, aborts, saw_stop, hold_class;
static device_t *held;
static usb_hcd_t *active_hcd;
static size_t baseline;
void *kmalloc(size_t bytes) {
 if (++allocations == fail_at) return NULL;
 struct allocation *p=malloc(sizeof(*p)+bytes);assert(p);p->magic=0xdeadbeef;p->bytes=bytes;allocated++;return p+1;
}
void *kcalloc(size_t n,size_t bytes) {void *p=kmalloc(n*bytes);if(p)memset(p,0,n*bytes);return p;}
void kfree(void *p) {if(!p)return;struct allocation *a=(struct allocation *)p-1;assert(a->magic==0xdeadbeef);a->magic=0;allocated--;free(a);}
void *krealloc(void *old,size_t bytes) {
 if(!old)return kmalloc(bytes);
 if(++allocations==fail_at)return NULL;
 struct allocation *p=(struct allocation *)old-1;assert(p->magic==0xdeadbeef);p=realloc(p,sizeof(*p)+bytes);assert(p);p->bytes=bytes;return p+1;
}
int klog_level=0;static unsigned capacity_errors;
void klog_write(const char *fmt,...) {if(strstr(fmt,"slots exhausted"))capacity_errors++;}
void panic(const char *fmt,...) {(void)fmt;abort();}
struct task_t *proc_current(void) {return NULL;}
int firmware_device_prepare(device_t *dev) {(void)dev;return 0;}
int class_device_publish(device_t *dev) {
 if(hold_class&&!held)held=device_get(dev);
 return 0;
}
void class_device_unpublish(device_t *dev) {(void)dev;}
static void init_core(void) {
 g_device_cap=256;g_devices=kcalloc(g_device_cap,sizeof(*g_devices));
 g_driver_cap=4;g_drivers=kcalloc(g_driver_cap,sizeof(*g_drivers));
 g_bus_cap=4;g_buses=kcalloc(g_bus_cap,sizeof(*g_buses));baseline=allocated;
}
static void finish_core(void) {
 assert(!g_device_count);assert(allocated==(int)baseline);
 kfree(g_devices);kfree(g_drivers);kfree(g_buses);assert(!allocated);
}
static uint32_t cfg[64], masks[6];static uint8_t sizing[6];static int present;
void arch_pci_host_init(uintptr_t base) {(void)base;}
int arch_pci_mmio_window(uint64_t *base,uint64_t *size) {*base=0x10000000;*size=0x100000;return 0;}
int arch_pci_bar_reachable(uint64_t base,uint64_t size) {return base>=0x10000000&&size<=0x100000&&base<=0x10100000-size;}
uint32_t arch_pci_config_read32(int bus,int dev,int fn,uint32_t reg) {
 if(bus||dev||fn||!present||reg>=256)return 0xffffffff;
 reg&=~3U;
 if(reg>=0x10&&reg<0x28&&sizing[(reg-0x10)/4])return masks[(reg-0x10)/4];
 return cfg[reg/4];
}
void arch_pci_config_write32(int bus,int dev,int fn,uint32_t reg,uint32_t value) {
 assert(!bus&&!dev&&!fn&&present&&reg<256);reg&=~3U;
 if(reg>=0x10&&reg<0x28) {unsigned b=(reg-0x10)/4;if(value==0xffffffff){sizing[b]=1;return;}sizing[b]=0;}
 if(reg==4) {cfg[1]=(cfg[1]&0xffff0000&~value)|(value&0xffff);return;}
 cfg[reg/4]=value;
}
static uint8_t mmio[4096] __attribute__((aligned(4096)));
void *arch_device_map_mmio(uint64_t pa,size_t bytes,uint32_t flags) {(void)flags;assert(pa==0x10000000&&bytes==4096);return mmio;}
void arch_device_unmap_mmio(void *p,size_t bytes) {assert(p==mmio&&bytes==4096);}
int pci_msix_capability(const device_t *d,pci_msix_info_t *out) {(void)d;(void)out;return -ENODEV;}
int pci_msix_enable(device_t *d,unsigned n) {(void)d;(void)n;return -ENODEV;}
int pci_msix_program_vector(device_t *d,unsigned n,uint32_t v) {(void)d;(void)n;(void)v;return -ENODEV;}
void pci_msix_set_vector_mask(device_t *d,unsigned n,int m) {(void)d;(void)n;(void)m;}
int pci_msix_commit(device_t *d) {(void)d;return -ENODEV;}
void pci_msix_disable(device_t *d) {(void)d;}
int irq_alloc_vectors(unsigned n) {(void)n;return -ENOSPC;}
void irq_free_vectors(uint32_t base,unsigned n) {(void)base;(void)n;}
static device_t *pci_current(void) {
 for(unsigned i=0;i<PCI_MAX_PUBLISHED_DEVICES;i++)if(g_pci_in_use[i])return &g_pci_devs[i];
 return NULL;
}
static void pci_insert(void) {present=1;cfg[0]=0x56781234;pci_enumerate(0,0,1);assert(pci_current());}
static void pci_remove(void) {present=0;pci_rescan();assert(!pci_current());}
static void check_pci_retired(int replacement) {
 pci_insert();device_t *old=device_get(pci_current());assert(old&&old->references==3);
 char name[32];memcpy(name,old->name,sizeof(name));void *info=old->plat_data;
 if(replacement) {cfg[0]=0x56789999;pci_rescan();} else {pci_remove();present=1;cfg[0]=0x56789999;pci_rescan();}
 device_t *newdev=pci_current();assert(newdev&&newdev!=old);
 assert(old->references==2&&old->state==DEV_STATE_REMOVED&&old->plat_data==info);
 assert(!memcmp(name,old->name,sizeof(name))&&pci_device_id(old)==0x12345678);
 device_put(old);pci_remove();
 assert(pci_slot_for_bdf(0,0,0)==0); /* The final external put makes it reusable. */
}
static void check_pci_exhaustion(void) {
 device_t *pins[PCI_MAX_PUBLISHED_DEVICES];
 for(unsigned i=0;i<PCI_MAX_PUBLISHED_DEVICES;i++) {
  present=1;cfg[0]=0x56781234;pci_enumerate(0,0,1);pins[i]=device_get(pci_current());assert(pins[i]);pci_remove();
 }
 present=1;pci_rescan();assert(!pci_current()&&pci_slot_for_bdf(0,0,0)==-1&&!g_device_count&&capacity_errors==1);
 for(unsigned i=0;i<PCI_MAX_PUBLISHED_DEVICES;i++){assert(pins[i]->references==2);device_put(pins[i]);}
 pci_rescan();assert(pci_current());pci_remove();
}
static virtio_transport_t binding;
static int virtio_probe(device_t *d) {return pci_virtio_transport_init(d,1,&binding);}
static int virtio_remove(device_t *d) {
 (void)d;pci_virtio_transport_t *vt=binding.priv;
 assert(vt&&vt->common==(uintptr_t)mmio);saw_stop++;return 0;
}
static void virtio_caps(void) {
 cfg[1]|=(uint32_t)PCI_STATUS_CAP_LIST<<16;cfg[0x34/4]=0x40;
 cfg[4]=0x10000000;masks[0]=0xfffff000;
 cfg[0x40/4]=PCI_CAP_ID_VNDR|(0x50<<8)|(16<<16)|(VIRTIO_PCI_CAP_COMMON_CFG<<24);
 cfg[0x44/4]=0;cfg[0x48/4]=0;cfg[0x4c/4]=56;
 cfg[0x50/4]=PCI_CAP_ID_VNDR|(0x64<<8)|(20<<16)|(VIRTIO_PCI_CAP_NOTIFY_CFG<<24);
 cfg[0x54/4]=0;cfg[0x58/4]=0x100;cfg[0x5c/4]=2;cfg[0x60/4]=4;
 cfg[0x64/4]=PCI_CAP_ID_VNDR|(16<<16)|(VIRTIO_PCI_CAP_DEVICE_CFG<<24);
 cfg[0x68/4]=0;cfg[0x6c/4]=0x200;cfg[0x70/4]=16;
}
static int any_match(device_t *d) {(void)d;return 1;}
static void check_virtio(int fault) {
 static driver_t drv={.name="virtio-test",.match=any_match,.probe=virtio_probe,.remove=virtio_remove};
 virtio_caps();pci_insert();device_t *d=pci_current();
 /* pci bus match checks drv vendor/device rather than match alone. */
 static device_id_t ids[]={{.vendor=0x1234,.device=0x5678,.subvendor=VENDOR_ANY,.subdevice=DEVICE_ANY},{0}};
 drv.id_table=ids;g_drivers[0]=&drv;g_driver_count=1;
 int before=allocated;
 if(fault) {
  /* Three MMIO maps each allocate their record and devres action. */
  fail_at=allocations+fault;
  assert(bus_probe_device(d)<0);fail_at=0;
  assert(!d->drv&&!d->resource_actions&&allocated==before);assert(!saw_stop);
 } else {
  for(int i=0;i<96;i++) {
   assert(!bus_probe_device(d));assert(binding.priv&&d->resource_actions);
   device_unbind(d);assert(!d->resource_actions&&allocated==before);
  }
  assert(saw_stop==96);
 }
 pci_remove();
}
static int usb_probe(device_t *d) {
 assert(!discovery_depth&&g_device_count==expected_interfaces);
 usb_interface_t *iface=d->plat_data;assert(iface&&iface->device==d);
 assert(d->parent==active_hcd->hcd_dev&&d->parent_reference==d->parent);
 probes++;return 0;
}
static int usb_remove(device_t *d) {
 assert(aborts||!active_hcd->port_devices[0]);assert(d->plat_data);
 usb_interface_t *iface=d->plat_data;assert(iface->eps);removes++;return 0;
}
static int usb_abort(usb_hcd_t *h,uint8_t slot) {(void)h;(void)slot;aborts++;return 0;}
static usb_device_t *usb_create(usb_hcd_t *h,unsigned n) {
 usb_device_t *u=kcalloc(1,sizeof(*u));u->hcd=h;u->iface_count=n;u->ifaces=kcalloc(n,sizeof(*u->ifaces));
 for(unsigned i=0;i<n;i++){u->ifaces[i].dev=u;u->ifaces[i].interface_class=3;u->ifaces[i].eps=kcalloc(1,sizeof(usb_endpoint_t));}
 return u;
}
static void usb_discard(usb_device_t *u) {
 for(unsigned i=0;i<u->iface_count;i++){assert(!u->ifaces[i].device);kfree(u->ifaces[i].eps);}
 kfree(u->ifaces);kfree(u);
}
static void check_usb(int fault,int registry_fault,int held_device) {
 static device_t parent={.name="hcd",.references=1};
 static const usb_hcd_ops_t ops={.abort_slot=usb_abort};
 usb_device_t *ports[1]={NULL};uint8_t state[1]={0};
 usb_hcd_t h={.hcd_dev=&parent,.ops=&ops,.max_ports=1,.port_devices=ports,.port_state=state};active_hcd=&h;
 static device_id_t ids[]={{.vendor=3,.device=DEVICE_ANY,.subvendor=VENDOR_ANY,.subdevice=DEVICE_ANY},{0}};
 static driver_t drv={.name="usb-test",.id_table=ids,.probe=usb_probe,.remove=usb_remove};
 g_drivers[0]=&drv;g_driver_count=1;expected_interfaces=3;
 usb_device_t *u=usb_create(&h,3);int before=allocated;
 if(fault)fail_at=allocations+fault;
 if(registry_fault){g_device_cap=1;driver_registry_test_fail_next(DRIVER_REGISTRY_TEST_DEVICE);}
 hold_class=held_device;
 int ret=usb_publish_interfaces(u);fail_at=0;
 if(fault||registry_fault) {
  assert(ret==-ENOMEM&&!g_device_count&&!probes&&!removes&&!discovery_depth&&parent.references==1);
  assert(allocated==before);usb_discard(u);
 } else {
  assert(!ret&&probes==3&&g_device_count==3&&parent.references==4);
  if(held_device)assert(held&&held->references==3);
  ports[0]=u;state[0]=1;usb_disconnect_port(&h,1);
  assert(!g_device_count&&removes==3&&aborts==1&&!ports[0]&&!state[0]);
  if(held_device) {
   assert(held->references==1&&held->plat_data==NULL&&held->hardware_id->vendor==3);
   assert(parent.references==2&&held->state==DEV_STATE_REMOVED);
   device_put(held);held=NULL;
  }
  assert(parent.references==1);
 }
}
int main(int argc,char **argv) {
 assert(argc==2);init_core();
 if(!strcmp(argv[1],"pci-retired"))check_pci_retired(0);
 else if(!strcmp(argv[1],"pci-replacement"))check_pci_retired(1);
 else if(!strcmp(argv[1],"pci-exhaustion"))check_pci_exhaustion();
 else if(!strcmp(argv[1],"virtio-rebind"))check_virtio(0);
 else if(!strcmp(argv[1],"virtio-alloc-fault"))check_virtio(7);
 else if(!strcmp(argv[1],"virtio-action-fault"))check_virtio(8);
 else if(!strcmp(argv[1],"usb-held"))check_usb(0,0,1);
 else if(!strcmp(argv[1],"usb-success"))check_usb(0,0,0);
 else if(!strcmp(argv[1],"usb-register-fault"))check_usb(0,1,0);
 else if(argv[1][0]=='f')check_usb(argv[1][1]-'0',0,0);
 else assert(0);
 finish_core();return 0;
}
'''


class NativeBusLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('host C compiler required')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        source = work / 'fixture.c'
        source.write_text(C_FIXTURE)
        cls.exe = work / 'fixture'
        result = subprocess.run([
            'cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
            '-fno-builtin', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=undefined', '-fno-sanitize-recover=all', '-Wl,--gc-sections',
            '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
            f'-I{ROOT}/kernel/arch/x86_64/include', f'-I{ROOT}/kernel/include',
            f'-I{ROOT}/kernel', str(source), '-o', str(cls.exe)
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_pci_removed_slot_pinned_by_external_reference(self):
        self.run_case('pci-retired')

    def test_pci_bdf_replacement_preserves_old_identity(self):
        self.run_case('pci-replacement')

    def test_pci_retired_slot_exhaustion_recovers_after_final_put(self):
        self.run_case('pci-exhaustion')

    def test_virtio_rebind_storage_reclaimed_after_remove(self):
        self.run_case('virtio-rebind')

    def test_virtio_binding_allocation_failure_rolls_back_mappings(self):
        self.run_case('virtio-alloc-fault')

    def test_virtio_devres_allocation_failure_frees_binding(self):
        self.run_case('virtio-action-fault')

    def test_usb_held_device_survives_disconnect_and_pins_parent(self):
        self.run_case('usb-held')

    def test_usb_probes_only_after_complete_interface_publication(self):
        self.run_case('usb-success')

    def test_usb_partial_registration_failure_rolls_back(self):
        self.run_case('usb-register-fault')

    def test_usb_each_interface_allocation_failure_rolls_back(self):
        for fault in range(1, 7):
            with self.subTest(allocation=fault):
                self.run_case(f'f{fault}')


if __name__ == '__main__':
    unittest.main()

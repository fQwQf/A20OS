"""Production PCI resource discovery/allocation with aligned config-cycle hardware.

The C fixture includes pci_bus.c unchanged and uses real kernel headers. Only
configuration-space hardware and driver-core transaction boundaries are stubbed.
Each scenario runs in a fresh process, preserving production static state rules.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
C_FIXTURE = r'''
#include <assert.h>
#include "drivers/bus/pci_bus.c"

#define NBUSES 3
struct function { uint32_t cfg[64], masks[6]; uint8_t sizing[6]; int present; };
static struct function hardware[NBUSES][32][8];
static uint64_t window_base=0x10000000, window_size=0x1000000, reachable_limit=~(uint64_t)0;
static int window_status, added, removed, depth, transactions, premature_probe, retried, host_calls;
static uintptr_t host_base;
static unsigned nonbar_writes, status_clears;
int fixture_routed_irq=-1, fixture_route_calls;
int klog_level=10;
void klog_write(const char *fmt, ...) {(void)fmt;}
int klog_write_ex(int level,const char *fmt, ...) {(void)level;(void)fmt;return 0;}
static pci_virtio_transport_t transport_storage;
static unsigned mapped_bars;
void *kmalloc(size_t size) {assert(size==sizeof(transport_storage));return &transport_storage;}
void kfree(void *ptr) {(void)ptr;}
int device_add_resource_action(device_t *dev,void (*release)(void *),void *data) {
 (void)dev;(void)release;assert(data==&transport_storage);return 0;
}
void *device_map_resource(device_t *dev,const resource_t *res) {
 (void)dev;assert(res&&res->type==RES_MMIO);mapped_bars++;return (void *)(uintptr_t)res->start;
}
int pci_msix_capability(const device_t *dev,pci_msix_info_t *out) {(void)dev;(void)out;return -ENODEV;}
int pci_msix_enable(device_t *dev,unsigned n) {(void)dev;(void)n;return -ENODEV;}
int pci_msix_program_vector(device_t *dev,unsigned i,uint32_t v) {(void)dev;(void)i;(void)v;return -ENODEV;}
void pci_msix_set_vector_mask(device_t *dev,unsigned i,int mask) {(void)dev;(void)i;(void)mask;}
int pci_msix_commit(device_t *dev) {(void)dev;return -ENODEV;}
void pci_msix_disable(device_t *dev) {(void)dev;}
int irq_alloc_vectors(unsigned n) {(void)n;return -ENODEV;}
void irq_free_vectors(uint32_t first,unsigned n) {(void)first;(void)n;}
void arch_pci_host_init(uintptr_t base) {host_base=base;host_calls++;}
int arch_pci_mmio_window(uint64_t *base,uint64_t *size) {*base=window_base;*size=window_size;return window_status;}
int arch_pci_bar_reachable(uint64_t base,uint64_t size) {return size && base<reachable_limit && size<=reachable_limit-base;}
static struct function *hw(int b,int d,int f) {
 if(b<0||b>=NBUSES||d<0||d>=32||f<0||f>=8) return NULL;
 struct function *h=&hardware[b][d][f];return h->present?h:NULL;
}
uint32_t arch_pci_config_read32(int b,int d,int f,uint32_t reg) {
 struct function *h=hw(b,d,f);if(!h||reg>=256)return 0xffffffff;
 reg&=~3U; /* Exactly the production HAL's dword alignment. */
 if(reg>=0x10&&reg<0x28&&h->sizing[(reg-0x10)/4])return h->masks[(reg-0x10)/4];
 return h->cfg[reg/4];
}
void arch_pci_config_write32(int b,int d,int f,uint32_t reg,uint32_t value) {
 struct function *h=hw(b,d,f);assert(h&&reg<256);reg&=~3U;
 unsigned header=(h->cfg[3]>>16)&0x7f,bars=header==0?6:header==1?2:0;
 if(reg>=0x10&&reg<0x10+bars*4) {
  unsigned bar=(reg-0x10)/4;
  if(value==0xffffffff){h->sizing[bar]=1;return;}
  h->sizing[bar]=0;h->cfg[reg/4]=value;return;
 }
 if(reg>=0x10&&value==0xffffffff)nonbar_writes++;
 if(reg==4) {
  if(value&0xffff0000)status_clears++;
  h->cfg[1]=(h->cfg[1]&0xffff0000&~value)|(value&0xffff);return;
 }
 h->cfg[reg/4]=value;
}
int bus_register(bus_type_t *bus) {assert(bus==&pci_bus);return host_calls>1?-EEXIST:0;}
int device_hotplug(device_t *dev,int event) {
 if(!depth)premature_probe++;
 assert(dev->bus==&pci_bus);
 if(event==BUS_EVENT_ADD)added++;else if(event==BUS_EVENT_REMOVE)removed++;else assert(0);
 return 0;
}
void driver_discovery_begin(void) {depth++;transactions++;}
void driver_discovery_end(void) {assert(depth>0);depth--;}
int driver_retry_deferred(void) {assert(!depth);retried++;return 0;}
int driver_resource_check_range(device_t *owner,const resource_t *res,uint64_t *busy_end) {
 (void)owner;(void)res; if(busy_end)*busy_end=0;return 0;
}
int driver_resource_check_claims(device_t *owner,const resource_t *res) {
 (void)owner;(void)res;assert(depth>0);return 0;
}

static struct function *function_at(int b,int d,unsigned header) {
 struct function *h=&hardware[b][d][0];h->present=1;h->cfg[0]=0x56781234;
 h->cfg[1]=0x20000007; /* Status has a latched W1C bit, command I/O+MEM+MASTER. */
 h->cfg[3]=header<<16;h->cfg[0x3c/4]=0xff;return h;
}
static void bar(struct function *h,unsigned index,uint64_t address,uint64_t size,int wide) {
 assert(index<6&&size&&!(size&(size-1)));
 uint64_t mask=~(size-1);uint32_t flags=wide?4:0;
 h->cfg[4+index]=(uint32_t)address|flags;h->masks[index]=((uint32_t)mask&~15U)|flags;
 if(wide&&index<5) {h->cfg[5+index]=(uint32_t)(address>>32);h->masks[index+1]=(uint32_t)(mask>>32);}
}
static device_t *device_at(int bus,int slot) {
 for(unsigned i=0;i<PCI_MAX_PUBLISHED_DEVICES;i++)
  if(g_pci_in_use[i]&&g_pci_infos[i].bus==bus&&g_pci_infos[i].dev==slot)return &g_pci_devs[i];
 assert(0);return NULL;
}
static void scan(void) {pci_enumerate(0,0,1);assert(!depth&&!premature_probe);}
static void check_allocation_windows(void) {
 uint64_t out=0;
 assert(!pci_allocate_mmio(0x1000,0,&out)&&out==window_base);
 assert(!pci_allocate_mmio(0x4000,0,&out)&&out==window_base+0x4000);
 window_status=-EOPNOTSUPP;assert(pci_allocate_mmio(0x1000,0,&out)==-EOPNOTSUPP);
}
static void check_allocation_limits(void) {
 uint64_t out=0;
 window_base=0xfffff000;window_size=0x3000;
 assert(pci_allocate_mmio(0x2000,0,&out)==-ERANGE); /* 32-bit BAR cannot cross 4GiB. */
 assert(!pci_allocate_mmio(0x2000,1,&out)&&out==0x100000000ULL);
 g_pci_mmio_alloc=0;window_base=~(uint64_t)0-0xfff;window_size=0x2000;
 assert(pci_allocate_mmio(0x1000,1,&out)==-ERANGE); /* host window wraps */
 g_pci_mmio_alloc=~(uint64_t)0-7;window_base=0;window_size=0xfffffffffffffff0ULL;
 assert(pci_allocate_mmio(0x1000,1,&out)==-ERANGE); /* alignment wraps */
 g_pci_mmio_alloc=0;window_base=0x10000000;window_size=0x1000;
 assert(pci_allocate_mmio(0x2000,0,&out)==-ERANGE); /* exhaustion */
 assert(pci_allocate_mmio(0,0,&out)==-ERANGE);
}
static void check_unknown_device_reservation(void) {
 struct function *first=function_at(0,0,0),*unknown=function_at(0,1,0);
 bar(first,0,0,0x1000,0);bar(unknown,0,window_base,0x4000,0);scan();
 device_t *u=device_at(0,1);assert(!u->drv&&u->res_count==1);
 assert(u->res[0].start==window_base&&u->res[0].end==window_base+0x3fff);
 assert(!pci_enable_and_assign_bars(device_at(0,0)));
 assert(g_pci_infos[0].bar[0]==window_base+0x4000);
}
static void check_all_published_reservations(void) {
 for(int d=0;d<3;d++){struct function*h=function_at(0,d,0);bar(h,0,window_base+d*0x2000,0x2000,0);}scan();
 uint64_t out=0;assert(!pci_allocate_mmio(0x2000,0,&out)&&out==window_base+0x6000);
}
static void check_same_device_rebuild_reservations(void) {
 struct function*h=function_at(0,0,0);
 bar(h,0,window_base+0x80000,0x1000,0);bar(h,1,0,0x1000,0);
 bar(h,2,window_base+0x10000,0x10000,0);bar(h,3,0,0x10000,0);scan();
 assert(!pci_enable_and_assign_bars(device_at(0,0)));
 assert(g_pci_infos[0].bar[1]==window_base);
 assert(g_pci_infos[0].bar[3]==window_base+0x20000);
}
static void check_relocation(void) {
 struct function*h=function_at(0,0,0);bar(h,0,0x100000000ULL,0x2000,1);
 reachable_limit=0x100000000ULL;scan();assert(!pci_enable_and_assign_bars(device_at(0,0)));
 assert(g_pci_infos[0].bar[0]==window_base&&h->cfg[5]==0&&(h->cfg[4]&4));
 assert(pci_get_bar_resource(device_at(0,0),0)->flags==IORESOURCE_MMIO_64BIT);
}
static void check_io_does_not_reserve_mmio(void) {
 struct function*h=function_at(0,0,0);
 h->cfg[4]=(uint32_t)window_base|1;h->masks[0]=0xfffff001;
 scan();uint64_t out=0;
 assert(g_pci_infos[0].bar_io[0]);
 assert(!pci_allocate_mmio(0x1000,0,&out)&&out==window_base);
}
static void check_no_host_window_failure(void) {
 struct function*h=function_at(0,0,0);bar(h,0,0x100000000ULL,0x2000,1);
 reachable_limit=0x100000000ULL;window_status=-EOPNOTSUPP;scan();
 assert(pci_enable_and_assign_bars(device_at(0,0))==-EOPNOTSUPP);
 assert(h->cfg[4]==4&&h->cfg[5]==1&&h->cfg[1]==0x20000007);
}
static void check_invalid_scan_ranges(void) {
 function_at(0,0,0);
 pci_enumerate(0,-1,1);pci_enumerate(0,0,257);pci_enumerate(0,2,2);pci_rescan();
 assert(!g_pci_initialized&&!host_calls&&!transactions&&!added&&!retried);
}
static void check_legacy_rescan(void) {
 function_at(0,0,0);scan();assert(added==1&&host_base==0&&g_pci_initialized);
 hardware[0][0][0].present=0;function_at(0,2,0);pci_rescan();
 assert(added==2&&removed==1&&retried==1&&transactions==2&&!premature_probe&&!depth);
}
static void check_bridge(unsigned header) {
 struct function*h=function_at(0,0,header);h->cfg[6]=0x00010100; /* primary 0, secondary/subordinate 1 */
 function_at(1,3,0);scan();assert(added==2);assert(device_at(1,3));
 assert(!nonbar_writes);pci_rescan();assert(added==2&&!removed);
}
static void check_bridge_assignment(void) {
 struct function*h=function_at(0,0,1);h->cfg[6]=0x00010100;function_at(1,3,0);scan();
 assert(!pci_enable_and_assign_bars(device_at(0,0)));assert(!nonbar_writes);
}
static void check_status_preserved(void) {
 struct function*h=function_at(0,0,0);bar(h,0,window_base,0x1000,0);scan();
 assert(!status_clears&&(h->cfg[1]&0x20000000));
 assert(!pci_enable_and_assign_bars(device_at(0,0)));assert(!status_clears&&(h->cfg[1]&0x20000000));
 pci_cfg_write16(device_at(0,0),4,7);assert(!status_clears&&(h->cfg[1]&0x20000000));
}
static resource_t *resource_type(device_t *device,enum resource_type type) {
 for(int i=0;i<device->res_count;i++)if(device->res[i].type==type)return &device->res[i];
 return NULL;
}
static void check_routed_irq(int valid,int pin) {
 struct function*h=function_at(0,0,0);h->cfg[0x3c/4]=(pin?0x100:0)|11;
 bar(h,0,window_base,0x1000,0);fixture_routed_irq=valid?0x4b:-1;scan();
 device_t*d=device_at(0,0);resource_t*r=resource_type(d,RES_IRQ);
 if(valid&&pin)assert(r&&r->start==0x4b&&r->end==0x4b);
 else assert(!r);
 assert(!pci_enable_and_assign_bars(d));r=resource_type(d,RES_IRQ);
 if(valid&&pin)assert(r&&r->start==0x4b&&r->end==0x4b);
 else assert(!r);
 if(!pin)assert(!fixture_route_calls);
}
static void check_io_resource(int assigned) {
 struct function*h=function_at(0,0,0);h->cfg[1]=0x20000000;
 h->cfg[4]=(assigned?0xc000U:0)|1;h->masks[0]=0xffffff01;scan();
 device_t*d=device_at(0,0);resource_t*r=pci_get_bar_resource(d,0);
 if(assigned)assert(r&&r->type==RES_IOPORT&&r->start==0xc000&&r->end==0xc0ff);
 else assert(!r);
 assert(!pci_enable_and_assign_bars(d));r=pci_get_bar_resource(d,0);
 if(assigned)assert(r&&r->type==RES_IOPORT&&r->start==0xc000&&r->end==0xc0ff&&(h->cfg[1]&1));
 else assert(!r&&h->cfg[4]==1&&!(h->cfg[1]&1));
}
static void check_prefetch_preserved(void) {
 struct function*h=function_at(0,0,0);bar(h,0,0,0x1000,0);
 h->cfg[4]|=8;h->masks[0]|=8;scan();
 assert(!pci_enable_and_assign_bars(device_at(0,0)));
 resource_t*r=pci_get_bar_resource(device_at(0,0),0);
 assert(r&&(r->flags&IORESOURCE_PREFETCH)&&(h->cfg[4]&8));
}
static void check_six_bars_plus_irq(void) {
 struct function*h=function_at(0,0,0);h->cfg[0x3c/4]=0x1ff;fixture_routed_irq=0x4b;
 for(unsigned i=0;i<6;i++)bar(h,i,window_base+i*0x1000,0x1000,0);
 scan();device_t*d=device_at(0,0);assert(d->res_count==7);
 assert(!pci_enable_and_assign_bars(d)&&d->res_count==7);
 assert(pci_get_bar_resource(d,5)->start==window_base+0x5000);
 assert(resource_type(d,RES_IRQ)->start==0x4b);
}
static void check_invalid_wide(unsigned header,unsigned last) {
 struct function*h=function_at(0,0,header);bar(h,last,window_base,0x1000,1);scan();
 assert(!nonbar_writes);assert(pci_get_bar_resource(device_at(0,0),last)==NULL);
 assert(pci_enable_and_assign_bars(device_at(0,0))<0);assert(!nonbar_writes);
}
static void check_virtio_capabilities(unsigned optional_type,int malformed) {
 struct function *h=function_at(0,0,0);
 bar(h,0,0xc000,0x80,0);h->cfg[4]|=1;h->masks[0]=0xffffff81; /* QEMU legacy I/O BAR. */
 bar(h,4,window_base,0x4000,1);
 h->cfg[1]|=PCI_STATUS_CAP_LIST<<16;h->cfg[0x34/4]=0x40;
 /* QEMU's optional PCI_CFG capability comes first, with a zero length. */
 h->cfg[0x40/4]=PCI_CAP_ID_VNDR|(0x54U<<8)|(20U<<16)|(optional_type<<24);
 h->cfg[0x44/4]=0;h->cfg[0x48/4]=0;h->cfg[0x4c/4]=0;
 unsigned types[]={1,2,3,4},offsets[]={0,0x1000,0x2000,0x3000},lengths[]={56,2,1,64};
 for(unsigned i=0;i<4;i++) {
  unsigned p=0x54+i*24,next=i==3?0:p+24;
  h->cfg[p/4]=PCI_CAP_ID_VNDR|(next<<8)|(20U<<16)|(types[i]<<24);
  h->cfg[(p+4)/4]=4;h->cfg[(p+8)/4]=offsets[i];
  h->cfg[(p+12)/4]=(malformed&&i==0)?0:lengths[i];h->cfg[(p+16)/4]=4;
 }
 scan();virtio_transport_t t={0};
 int result=pci_virtio_transport_init(device_at(0,0),2,&t);
 if(malformed) {assert(result==-EINVAL);assert(!t.priv);return;}
 assert(!result&&mapped_bars==4&&t.priv==&transport_storage);
 assert(transport_storage.common==window_base&&transport_storage.notify==window_base+0x1000);
 assert(transport_storage.isr==window_base+0x2000&&transport_storage.config==window_base+0x3000);
 assert(transport_storage.notify_multiplier==4);
}
int main(int argc,char **argv) {
 assert(argc==2);
 if(!strcmp(argv[1],"window"))check_allocation_windows();
 else if(!strcmp(argv[1],"limits"))check_allocation_limits();
 else if(!strcmp(argv[1],"unknown"))check_unknown_device_reservation();
 else if(!strcmp(argv[1],"all-reservations"))check_all_published_reservations();
 else if(!strcmp(argv[1],"same-device"))check_same_device_rebuild_reservations();
 else if(!strcmp(argv[1],"relocation"))check_relocation();
 else if(!strcmp(argv[1],"io-namespace"))check_io_does_not_reserve_mmio();
 else if(!strcmp(argv[1],"no-window"))check_no_host_window_failure();
 else if(!strcmp(argv[1],"invalid-ranges"))check_invalid_scan_ranges();
 else if(!strcmp(argv[1],"legacy-rescan"))check_legacy_rescan();
 else if(!strcmp(argv[1],"bridge"))check_bridge(1);
 else if(!strcmp(argv[1],"multi-bridge"))check_bridge(0x81);
 else if(!strcmp(argv[1],"bridge-assign"))check_bridge_assignment();
 else if(!strcmp(argv[1],"status"))check_status_preserved();
 else if(!strcmp(argv[1],"routed-irq"))check_routed_irq(1,1);
 else if(!strcmp(argv[1],"unrouted-irq"))check_routed_irq(0,1);
 else if(!strcmp(argv[1],"no-pin"))check_routed_irq(1,0);
 else if(!strcmp(argv[1],"io-resource"))check_io_resource(1);
 else if(!strcmp(argv[1],"unassigned-io"))check_io_resource(0);
 else if(!strcmp(argv[1],"prefetch"))check_prefetch_preserved();
 else if(!strcmp(argv[1],"six-bars-irq"))check_six_bars_plus_irq();
 else if(!strcmp(argv[1],"wide-last"))check_invalid_wide(0,5);
 else if(!strcmp(argv[1],"bridge-wide-last"))check_invalid_wide(1,1);
 else if(!strcmp(argv[1],"virtio-pci-cfg"))check_virtio_capabilities(5,0);
 else if(!strcmp(argv[1],"virtio-unknown-cap"))check_virtio_capabilities(0xff,0);
 else if(!strcmp(argv[1],"virtio-invalid-mmio"))check_virtio_capabilities(5,1);
 else assert(0);
 return 0;
}
'''

class PciResourcesTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not shutil.which('cc'):raise unittest.SkipTest('host C compiler required')
  cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
  work=Path(cls.temp.name);source=work/'fixture.c';source.write_text(C_FIXTURE);cls.exe=work/'fixture'
  route=work/'route.c';route.write_text('extern int fixture_routed_irq,fixture_route_calls;\nint arch_pci_intx_irq(int bus,int dev,int function,int pin) {(void)bus;(void)dev;(void)function;(void)pin;fixture_route_calls++;return fixture_routed_irq;}\n')
  result=subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
   '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DCONFIG_X86_64','-DCONFIG_NR_CPUS=1',
   f'-I{ROOT}/kernel/arch/x86_64/include',f'-I{ROOT}/kernel/include',f'-I{ROOT}/kernel',str(source),str(route),'-o',str(cls.exe)],capture_output=True,text=True)
  if result.returncode:raise AssertionError(result.stderr)
 def run_case(self,name):
  result=subprocess.run([str(self.exe),name],capture_output=True,text=True)
  self.assertEqual(result.returncode,0,result.stderr)
 def test_host_owned_window(self):self.run_case('window')
 def test_window_overflow_exhaustion_and_32bit_limit(self):self.run_case('limits')
 def test_unknown_device_reserves_assigned_bar(self):self.run_case('unknown')
 def test_all_published_bars_reserved(self):self.run_case('all-reservations')
 def test_same_device_resource_rebuild_keeps_reservations(self):self.run_case('same-device')
 def test_unreachable_64bit_bar_relocated(self):self.run_case('relocation')
 def test_io_and_mmio_address_spaces_do_not_alias(self):self.run_case('io-namespace')
 def test_missing_host_window_preserves_configuration_on_failure(self):self.run_case('no-window')
 def test_invalid_bus_ranges_do_not_start_scan(self):self.run_case('invalid-ranges')
 def test_legacy_zero_ecam_rescan_and_discovery_transactions(self):self.run_case('legacy-rescan')
 def test_bridge_bus_numbers_use_aligned_config_dword(self):self.run_case('bridge')
 def test_multifunction_bridge_is_traversed(self):self.run_case('multi-bridge')
 def test_bridge_assignment_never_sizes_window_registers(self):self.run_case('bridge-assign')
 def test_command_updates_preserve_w1c_status(self):self.run_case('status')
 def test_interrupt_resource_uses_host_routing(self):self.run_case('routed-irq')
 def test_unrouted_interrupt_line_is_not_a_resource(self):self.run_case('unrouted-irq')
 def test_no_interrupt_pin_does_not_route_line(self):self.run_case('no-pin')
 def test_assigned_io_bar_published_and_enabled(self):self.run_case('io-resource')
 def test_unassigned_io_bar_never_guesses_ports(self):self.run_case('unassigned-io')
 def test_prefetch_attribute_survives_relocation(self):self.run_case('prefetch')
 def test_six_memory_bars_and_irq_fit_resource_array(self):self.run_case('six-bars-irq')
 def test_last_slot_cannot_start_64bit_bar(self):self.run_case('wide-last')
 def test_bridge_last_slot_cannot_start_64bit_bar(self):self.run_case('bridge-wide-last')
 def test_virtio_pci_cfg_zero_length_is_skipped(self):self.run_case('virtio-pci-cfg')
 def test_virtio_unknown_capability_is_skipped(self):self.run_case('virtio-unknown-cap')
 def test_virtio_required_mmio_capability_still_validated(self):self.run_case('virtio-invalid-mmio')

if __name__=='__main__':unittest.main()

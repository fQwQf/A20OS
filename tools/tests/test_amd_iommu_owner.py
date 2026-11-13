"""Passive AMD-Vi binding with real PCI, core, IVRS and ACPI table code.

Only hardware, allocation, class publication and task synchronization are
substitutes. Assertions observe claim/ref/table lifetime across production
probe/unbind paths, including a legacy aperture discovered through MMIO.
"""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

if __package__:
    from .test_native_bus_lifetime import ROOT, C_FIXTURE
else:
    from test_native_bus_lifetime import ROOT, C_FIXTURE


CASES = r'''
#include "drivers/core/amd_iommu_owner.c"
#include "drivers/core/amd_iommu.c"
#include "drivers/core/memory.h"
static uint8_t xsdt[44], ivrs[512];
static size_t ivrs_length;
static int table_views, mmio_maps, mmio_unmaps, mmio_attempts, mmio_fail;
static device_t peer = {.name="foreign-owner", .references=1};
static acpi_context_t acpi;
static void put_bytes(uint8_t *p, unsigned offset, uint64_t value, unsigned n) {
 for(unsigned i=0;i<n;i++)p[offset+i]=value>>(8*i);
}
static void checksum_table(uint8_t *p, size_t n) {
 p[9]=0;unsigned sum=0;for(size_t i=0;i<n;i++)sum+=p[i];p[9]=(uint8_t)-sum;
}
static const void *map_table(void *context,uint64_t address,size_t n) {
 (void)context;const void *p=NULL;
 if(address==0x1000&&n<=sizeof(xsdt))p=xsdt;
 if(address==0x2000&&n<=ivrs_length)p=ivrs;
 if(p)table_views++;
 return p;
}
static void unmap_table(void *context,const void *p,size_t n) {
 (void)context;(void)n;assert((p==xsdt||p==ivrs)&&table_views>0);table_views--;
}
const acpi_context_t *arch_acpi_context(void) {return &acpi;}
static void tables_begin(void) {
 memset(xsdt,0,sizeof(xsdt));memset(ivrs,0,sizeof(ivrs));
 memcpy(xsdt,"XSDT",4);put_bytes(xsdt,4,sizeof(xsdt),4);xsdt[8]=1;
 put_bytes(xsdt,36,0x2000,8);checksum_table(xsdt,sizeof(xsdt));
 memcpy(ivrs,"IVRS",4);ivrs[8]=2;ivrs[36]=1;ivrs_length=48;
 acpi=(acpi_context_t){.mapper={.map=map_table,.unmap=unmap_table},.root=0x1000,.entry_width=8};
 cfg[0]=0x14191022;cfg[1]=1U<<20;cfg[2]=0x08060000;
 cfg[0x34/4]=0x40;
}
static void unit_add(unsigned type,unsigned cap,uint64_t base,uint64_t efr) {
 size_t off=ivrs_length,prefix=type==0x10?24:40;
 ivrs[off]=type;put_bytes(ivrs,off+2,prefix+4,2);
 put_bytes(ivrs,off+6,cap,2);put_bytes(ivrs,off+8,base,8);
 if(type!=0x10)put_bytes(ivrs,off+24,efr,8);
 /* Explicit SELECT entries avoid two units claiming the same requester. */
 ivrs[off+prefix]=2;put_bytes(ivrs,off+prefix+1,cap,2);
 ivrs_length+=prefix+4;
 put_bytes(ivrs,4,ivrs_length,4);checksum_table(ivrs,ivrs_length);
 cfg[cap/4]=0x0f|(3U<<16)|(1U<<19)|(1U<<27);
 cfg[(cap+4)/4]=(uint32_t)base;cfg[(cap+8)/4]=base>>32;
 cfg[(cap+16)/4]=(40U<<8)|(48U<<15);
}
static void amd_map_check(void) {
 resource_t range={.type=RES_MMIO,.start=0xfed80000,.end=0xfed83fff};
 assert(table_views==1&&driver_resource_check_claims(&peer,&range)==-EBUSY);
}
static void *amd_map(uint64_t pa,size_t n,uint32_t flags) {
 (void)flags;assert(pa==0xfed80000&&n==0x4000);amd_map_check();
 mmio_attempts++;
 if(mmio_fail)return NULL;
 mmio_maps++;return mmio;
}
static void amd_unmap(void *p,size_t n) {
 assert(p==mmio&&n==0x4000);amd_map_check();mmio_unmaps++;
}
static void pci_find_tests(void) {
 device_t *d=(void *)1;assert(pci_device_find_get(1,0,&d)==-EOPNOTSUPP&&!d);
 assert(pci_device_find_get(0,0,&d)==-ENODEV&&!d);
 assert(pci_device_find_get(0,0,NULL)==-EINVAL);
 pci_insert();assert(!pci_device_find_get(0,0,&d)&&d==pci_current());
 assert(d->references==3);pci_remove();
 device_t *missing=(void *)1;assert(pci_device_find_get(0,0,&missing)==-ENODEV&&!missing);
 present=1;cfg[0]=0x99991234;pci_rescan();device_t *replacement=NULL;
 assert(!pci_device_find_get(0,0,&replacement)&&replacement!=d);
 assert(pci_device_id(d)==0x12345678&&d->state==DEV_STATE_REMOVED);
 device_put(replacement);device_put(d);pci_remove();
}
static int bystander_probes;
static int bystander_probe(device_t *dev) {(void)dev;bystander_probes++;return -ENODEV;}
static int bystander_match(device_t *dev,const driver_t *drv) {(void)dev;(void)drv;return 1;}
static void amd_case(const char *name) {
 tables_begin();int legacy=strstr(name,"legacy")!=NULL;
 unit_add(legacy?0x10:0x11,0x40,0xfed80000,strstr(name,"counters")?1ULL<<9:0);
 int enabled=strstr(name,"enabled")!=NULL||strstr(name,"expand")!=NULL;
 if(enabled)cfg[0x44/4]|=1;
 if(strstr(name,"overlap")) {
  /* The first full span includes the second unit's disjoint minimum span. */
  tables_begin();unit_add(0x11,0x40,0xfed80000,1ULL<<9);
  unit_add(0x11,0x80,0xfed84000,0);cfg[0x40/4]|=0x80<<8;
 }
 if(strstr(name,"reverse")) {
  tables_begin();unit_add(0x11,0x40,0xfed84000,0);
  unit_add(0x11,0x80,0xfed80000,1ULL<<9);cfg[0x40/4]|=0x80<<8;
 }
 if(strstr(name,"legacy-expand-overlap")) {
  tables_begin();unit_add(0x11,0x40,0xfed84000,0);
  unit_add(0x10,0x80,0xfed80000,0);cfg[0x40/4]|=0x80<<8;cfg[0x84/4]|=1;
 }
 if(strstr(name,"two-units")) {
  unit_add(0x11,0x80,0xfedc0000,0);cfg[0x40/4]|=0x80<<8;
 }
 if(strstr(name,"expand"))put_bytes(mmio,0x30,1ULL<<9,8);
 if(strstr(name,"legacy-no-efr"))cfg[0x40/4]&=~(1U<<27);
 device_t blocker={.name="unbound-reservation",.references=1};
 resource_t reservation={.type=RES_MEM,.start=0xfed80000,.end=0xfed83fff};
 int blocked=strstr(name,"blocked")!=NULL;
 if(blocked) {
  if(strstr(name,"expand")||strstr(name,"counters"))reservation=(resource_t){.type=RES_MEM,.start=0xfed90000,.end=0xfed93fff};
  blocker.res=&reservation;blocker.res_count=1;assert(!device_register(&blocker));
 }
 driver_memory_reservation_t *protected=NULL;
 int guarded=strstr(name,"reserved")!=NULL;
 if(guarded) {
  driver_memory_range_t storage={0xfed80000,0xfed83fff};
  assert(!driver_memory_reserve(&storage,1,&protected));
 }
 present=1;pci_enumerate(0,0,1);device_t *dev=pci_current();assert(dev);
 if(guarded)assert(amd_iommu_pci_report(&acpi)==-EIO&&!mmio_attempts);
 dev->user_owned=strstr(name,"user-owned")!=NULL;
 bus_type_t bystander_bus={.name="bystander",.match=bystander_match};
 driver_t bystander_driver={.name="failed-bystander",.bus=&bystander_bus,.probe=bystander_probe};
 device_t bystander={.name="bystander",.bus=&bystander_bus,.references=1};
 assert(!bus_register(&bystander_bus)&&!driver_register(&bystander_driver));
 assert(!device_register(&bystander)&&bystander_probes==1);
 int before=allocated;unsigned refs=dev->references;
 protect_writes=1;config_writes=0;
 driver_discovery_begin();assert(!driver_register(&amdvi_driver));
 assert(!dev->drv);driver_discovery_end();
 const char *allocation=strstr(name,"alloc-");
 if(allocation)fail_at=allocations+(allocation[6]-'0');
 mmio_fail=strstr(name,"map-fail")!=NULL;
 int result=bus_probe_device(dev);fail_at=0;
 int unknown=legacy&&!enabled&&!strstr(name,"legacy-no-efr");
 int bad=blocked||guarded||unknown||dev->user_owned||strstr(name,"overlap")||allocation||mmio_fail;
 if(bad) {
  assert(result<0&&!dev->drv&&!dev->drv_priv&&!table_views);
  assert(allocated==before&&dev->references==refs);
  if(blocked||guarded||strstr(name,"overlap"))assert(result==-EBUSY);
  if(unknown)assert(result==-EOPNOTSUPP);
  if(allocation||mmio_fail)assert(result==-ENOMEM);
  if(blocked&&!strstr(name,"expand"))assert(!mmio_attempts);
 } else {
  assert(!result&&dev->drv==&amdvi_driver&&dev->drv_priv&&table_views==1);
  struct amdvi_owner *owner=dev->drv_priv;
  size_t unit_count=owner->count;
  assert(unit_count==(strstr(name,"two-units")?2:1));
  assert(dev->references==refs+unit_count);
  assert(owner->units[0].snapshot.registers_read==enabled);
  if(strstr(name,"expand")||strstr(name,"counters"))assert(owner->units[0].snapshot.register_size==0x80000);
  if(!enabled)assert(!mmio_maps);
  resource_t aperture=owner->units[0].aperture;
  driver_resource_claim_t *conflict=(void *)1;
  assert(driver_resource_request(&peer,&aperture,&conflict)==-EBUSY&&!conflict);
  blocker.res=&aperture;blocker.res_count=1;
  assert(device_register(&blocker)==-EBUSY);
  assert(!amd_iommu_owner_selftest());
  assert(dev->drv==&amdvi_driver&&dev->references==refs+unit_count);
  assert(bystander_probes==1); /* Recovery must not probe unrelated devices. */
 }
 assert(!config_writes);
 assert(!driver_unregister(&amdvi_driver));
 assert(!dev->drv&&!dev->drv_priv&&dev->references==refs);
 device_t *unbound=NULL;
 assert(!pci_device_find_get(0,0,&unbound)&&unbound==dev);device_put(unbound);
 assert(!table_views&&allocated==before&&mmio_maps==mmio_unmaps);
 protect_writes=0;pci_remove();
 if(blocked)device_unregister(&blocker);
 assert(!driver_unregister(&bystander_driver));device_unregister(&bystander);bus_unregister(&bystander_bus);
 assert(bystander.references==1);
 driver_memory_release(protected);
 assert(blocker.references==1&&peer.references==1);
}
'''


class AmdIommuOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86_64 hardware-access headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-amdvi-owner-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = C_FIXTURE.replace('static uint8_t mmio[4096]', 'static uint8_t mmio[0x4000]')
        marker = 'void arch_pci_config_write32(int bus,int dev,int fn,uint32_t reg,uint32_t value) {'
        assert source.count(marker) == 1
        source = source.replace(marker, 'static int protect_writes; static unsigned config_writes;\n' + marker + '\n if(protect_writes)config_writes++;')
        marker = 'void *arch_device_map_mmio(uint64_t pa,size_t bytes,uint32_t flags) {(void)flags;assert(pa==0x10000000&&bytes==4096);return mmio;}\nvoid arch_device_unmap_mmio(void *p,size_t bytes) {assert(p==mmio&&bytes==4096);}'
        assert source.count(marker) == 1
        source = source.replace(marker, 'static void *amd_map(uint64_t,size_t,uint32_t);\nstatic void amd_unmap(void *,size_t);\nvoid *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){return amd_map(pa,n,f);}\nvoid arch_device_unmap_mmio(void *p,size_t n){amd_unmap(p,n);}')
        marker = 'int main(int argc,char **argv) {'
        assert source.count(marker) == 1
        source = source.replace(marker, CASES + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))', 'if(!strcmp(argv[1],"pci-find"))pci_find_tests();\n else if(!strncmp(argv[1],"amd-",4))amd_case(argv[1]);\n else if(!strcmp(argv[1],"pci-retired"))')
        path = work / 'fixture.c'
        path.write_text(source)
        cls.binary = work / 'test'
        subprocess.run([
            'cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
            '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-DCONFIG_X86_64', '-DCONFIG_ACPI', '-DCONFIG_NR_CPUS=1',
            '-DCONFIG_DRIVER_LIFECYCLE_TEST',
            '-I' + str(ROOT / 'kernel/arch/x86_64/include'),
            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel'),
            str(path), str(ROOT / 'kernel/drivers/core/amd_iommu_probe.c'),
            str(ROOT / 'kernel/drivers/core/amd_iommu_scope.c'),
            str(ROOT / 'kernel/firmware/acpi/tables.c'),
            str(ROOT / 'kernel/firmware/acpi/ivrs.c'),
            str(ROOT / 'kernel/firmware/acpi/ivrs_policy.c'),
            '-o', str(cls.binary),
        ], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=10)

    def test_referenced_bdf_lookup_survives_replacement(self):
        self.run_case('pci-find')

    def test_disabled_decoder_claim_and_rebind_without_mmio(self):
        self.run_case('amd-disabled')

    def test_enabled_decoder_claim_precedes_map_and_outlives_unmap(self):
        self.run_case('amd-enabled')

    def test_unbound_declaration_blocks_before_mmio(self):
        self.run_case('amd-enabled-blocked')

    def test_disabled_legacy_unknown_aperture_rejected(self):
        self.run_case('amd-legacy')
        self.run_case('amd-legacy-no-efr')

    def test_legacy_expansion_and_late_conflict_cleanup(self):
        self.run_case('amd-legacy-enabled-expand')
        self.run_case('amd-legacy-enabled-expand-blocked')

    def test_same_owner_full_apertures_cannot_overlap(self):
        self.run_case('amd-overlap')
        self.run_case('amd-overlap-reverse')
        self.run_case('amd-legacy-expand-overlap')

    def test_disabled_counter_aperture_claims_full_extent(self):
        self.run_case('amd-counters')
        self.run_case('amd-counters-blocked')

    def test_two_capabilities_and_each_allocation_failure(self):
        self.run_case('amd-two-units')
        for stage in range(1, 4):
            with self.subTest(stage=stage):
                self.run_case('amd-two-units-alloc-' + str(stage))

    def test_mapper_failure_releases_claim_and_firmware(self):
        self.run_case('amd-enabled-map-fail')

    def test_user_owned_function_never_bound(self):
        self.run_case('amd-user-owned')

    def test_reserved_storage_blocks_diagnostic_and_binding_before_mmio(self):
        self.run_case('amd-enabled-reserved')


if __name__ == '__main__':
    unittest.main()

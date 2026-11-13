"""Retained AMD route objects with production PCI/core, owner and IVRS policy.

The hardware matrix, memory allocator and scheduler are host substitutes.
No fixture substitutes routing, capability validation or lifetime decisions.
"""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

if __package__:
    from .test_native_bus_lifetime import ROOT, C_FIXTURE
    from .test_pci_scope import HARDWARE, CASES as SCOPE_CASES
else:
    from test_native_bus_lifetime import ROOT, C_FIXTURE
    from test_pci_scope import HARDWARE, CASES as SCOPE_CASES


WRAPPERS = r'''
static int checked_acquire(device_t *client,amd_iommu_route_t **out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_acquire(client,out);assert(scope_writes==n);return ret;
}
static int checked_validate(amd_iommu_route_t *route,amd_iommu_route_info_t *out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_validate(route,out);assert(scope_writes==n);return ret;
}
static int checked_memory(amd_iommu_route_t *route,size_t index,acpi_ivrs_memory_t *out) {
 unsigned n=scope_writes;int ret=amd_iommu_route_memory(route,index,out);assert(scope_writes==n);return ret;
}
static void checked_release(amd_iommu_route_t *route) {
 unsigned n=scope_writes;amd_iommu_route_release(route);assert(scope_writes==n);
}
'''

CASES = r'''
#include "drivers/core/amd_iommu_owner.c"
static uint8_t route_xsdt[44], route_ivrs[512];
static size_t route_length;
static int route_tables, route_maps, route_unmaps;
static acpi_context_t route_acpi;
static device_t route_peer={.name="peer",.references=1};
static void route_put(uint8_t *p,unsigned offset,uint64_t value,unsigned n) {
 for(unsigned i=0;i<n;i++)p[offset+i]=value>>(8*i);
}
static void route_checksum(uint8_t *p,size_t n) {
 p[9]=0;unsigned sum=0;for(size_t i=0;i<n;i++)sum+=p[i];p[9]=(uint8_t)-sum;
}
static const void *route_map_table(void *context,uint64_t address,size_t n) {
 (void)context;const void *p=NULL;
 if(address==0x1000&&n<=sizeof(route_xsdt))p=route_xsdt;
 if(address==0x2000&&n<=route_length)p=route_ivrs;
 if(p)route_tables++;
 return p;
}
static void route_unmap_table(void *context,const void *p,size_t n) {
 (void)context;(void)n;assert((p==route_xsdt||p==route_ivrs)&&route_tables>0);route_tables--;
}
const acpi_context_t *arch_acpi_context(void) {return &route_acpi;}
static void route_hw_map_check(void) {
 resource_t range={.type=RES_MMIO,.start=0xfed80000,.end=0xfed83fff};
 assert(route_tables>0&&driver_resource_check_claims(&route_peer,&range)==-EBUSY);
}
static void *route_map(uint64_t pa,size_t n,uint32_t flags) {
 (void)flags;assert(pa==0xfed80000&&n==0x4000);route_hw_map_check();route_maps++;return mmio;
}
static void route_unmap(void *p,size_t n) {
 assert(p==mmio&&n==0x4000);route_hw_map_check();route_unmaps++;
}
static void route_select(unsigned requester,unsigned settings) {
 route_ivrs[route_length]=2;route_put(route_ivrs,route_length+1,requester,2);
 route_ivrs[route_length+3]=settings;route_length+=4;
}
static void route_ivmd(unsigned requester,uint64_t start,unsigned flags) {
 size_t at=route_length;route_ivrs[at]=0x21;route_ivrs[at+1]=flags;
 route_put(route_ivrs,at+2,32,2);route_put(route_ivrs,at+4,requester,2);
 route_put(route_ivrs,at+16,start,8);route_put(route_ivrs,at+24,0x2000,8);route_length+=32;
}
static void route_firmware(int alias) {
 memcpy(route_xsdt,"XSDT",4);route_put(route_xsdt,4,sizeof(route_xsdt),4);route_xsdt[8]=1;
 route_put(route_xsdt,36,0x2000,8);route_checksum(route_xsdt,sizeof(route_xsdt));
 memcpy(route_ivrs,"IVRS",4);route_ivrs[8]=2;route_ivrs[36]=1;route_length=88;
 route_ivrs[48]=0x11;route_put(route_ivrs,52,0x10,2);route_put(route_ivrs,54,0x40,2);
 route_put(route_ivrs,56,0xfed80000,8);
 if(alias) {
  route_ivrs[route_length]=0x42;route_put(route_ivrs,route_length+1,0x21a,2);
  route_ivrs[route_length+3]=0x13;route_put(route_ivrs,route_length+5,0x220,2);route_length+=8;
  route_select(0x220,0x27);
 } else route_select(0x21a,0x13);
 route_put(route_ivrs,50,route_length-48,2);
 route_ivmd(0x21a,0x100000,7);if(alias)route_ivmd(0x220,0x200000,5);
 route_put(route_ivrs,4,route_length,4);route_checksum(route_ivrs,route_length);
 route_acpi=(acpi_context_t){.mapper={.map=route_map_table,.unmap=route_unmap_table},.root=0x1000,.entry_width=8};
}
static void route_error(amd_iommu_route_t *route,int expected) {
 amd_iommu_route_info_t info;memset(&info,0xa5,sizeof(info));
 assert(amd_iommu_route_validate(route,&info)==expected);
 assert(!info.requester&&!info.unit.base&&!info.memory_count&&!info.alias);
}
static void route_acquire_error(device_t *client,int expected) {
 amd_iommu_route_t *route=(void *)1;
 assert(amd_iommu_route_acquire(client,&route)==expected&&!route);
}
static void route_case(const char *name) {
 int alias=strstr(name,"alias")!=NULL,valid=strstr(name,"range")!=NULL;
 struct scope_function *root=scope_bridge(0,1,1,2);
 scope_bridge(1,2,2,2);scope_add(2,3,2,0);
 if(alias)scope_add(2,4,0,0);
 struct scope_function *controller=scope_add(0,2,0,0);
 controller->config[0]=0x14191022;controller->config[2]=0x08060000;
 controller->config[1]=1U<<20;controller->config[0x34/4]=0x40;
 controller->config[0x40/4]=0x0f|(3U<<16)|(1U<<19)|(1U<<27);
 controller->config[0x44/4]=0xfed80001; /* Enabled, read-only MMIO observation. */
 controller->config[0x4c/4]=valid?0x08080080:0xff000000;
 if(!strcmp(name,"range-missing"))controller->config[0x4c/4]=0x09090080;
 controller->config[0x50/4]=(40U<<8)|(48U<<15);
 route_firmware(alias);
 if(!strcmp(name,"alias-range-outside")) {
  scope_hw[2][4][0].present=0;scope_add(0,4,0,0);
  route_put(route_ivrs,93,0x20,2);route_put(route_ivrs,97,0x20,2);
 }
 if(!strcmp(name,"ambiguous")) {
  size_t at=route_length;route_ivrs[at]=0x11;route_put(route_ivrs,at+2,44,2);
  route_put(route_ivrs,at+4,0x10,2);route_put(route_ivrs,at+6,0x80,2);
  route_put(route_ivrs,at+8,0xfedc0000,8);route_length+=40;route_select(0x21a,0);
  controller->config[0x40/4]|=0x80<<8;
  controller->config[0x80/4]=0x0f|(3U<<16)|(1U<<19)|(1U<<27);
  controller->config[0x84/4]=0xfedc0000;controller->config[0x90/4]=(40U<<8)|(48U<<15);
 }
 route_put(route_ivrs,4,route_length,4);route_checksum(route_ivrs,route_length);
 pci_enumerate(0,0,1);
 device_t *client=scope_device(0x21a),*provider=scope_device(0x10);
 if(!strcmp(name,"unbound")) {
  route_acquire_error(client,-ENODEV);goto cleanup;
 }
 driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
 assert(!bus_probe_device(provider)&&provider->drv==&amdvi_driver&&route_tables==1);
 int before=allocated;unsigned client_refs=client->references;
 if(!strcmp(name,"arguments")) {
  route_acquire_error(NULL,-EINVAL);assert(amd_iommu_route_acquire(client,NULL)==-EINVAL);
  route_error(NULL,-EINVAL);
  assert(amd_iommu_route_validate(NULL,NULL)==-EINVAL);
  acpi_ivrs_memory_t m;memset(&m,0xa5,sizeof(m));
  assert(amd_iommu_route_memory(NULL,0,&m)==-EINVAL&&!m.start);
  assert(amd_iommu_route_memory(NULL,0,NULL)==-EINVAL);amd_iommu_route_release(NULL);
 } else if(!strcmp(name,"allocation")) {
  fail_at=allocations+1;route_acquire_error(client,-ENOMEM);fail_at=0;
  assert(client->references==client_refs&&allocated==before&&route_tables==1);
 } else if(!strcmp(name,"alias-missing")) {
  scope_hw[2][4][0].present=0;pci_rescan();route_acquire_error(client,-ENODEV);
 } else if(!strcmp(name,"range-missing")) {
  route_acquire_error(client,-ENODEV);
 } else if(!strcmp(name,"range-root-type")) {
  root->config[0x40/4]=0x10|(2U<<16)|(6U<<20);route_acquire_error(client,-EOPNOTSUPP);
 } else if(!strcmp(name,"alias-range-outside")) {
  route_acquire_error(client,-ENODEV);
 } else if(!strcmp(name,"ambiguous")) {
  route_acquire_error(client,-EBUSY);
 } else {
  amd_iommu_route_t *route=NULL;assert(!amd_iommu_route_acquire(client,&route)&&route);
  assert(!client->dependencies&&!provider->dependencies);
  amd_iommu_route_info_t info;assert(!amd_iommu_route_validate(route,&info));
  assert(info.requester==0x21a&&info.dma_requester==(alias?0x220:0x21a));
  assert(info.unit.requester==0x10&&info.unit.base==0xfed80000&&info.settings==0x13);
  assert(info.alias==alias&&info.hardware_range_checked==valid&&info.memory_count==(alias?2:1));
  acpi_ivrs_memory_t memory;
  assert(!amd_iommu_route_memory(route,0,&memory)&&memory.start==0x100000&&memory.flags==7);
  if(alias)assert(!amd_iommu_route_memory(route,1,&memory)&&memory.start==0x200000&&memory.flags==5);
  assert(amd_iommu_route_memory(route,info.memory_count,&memory)==-ENODEV&&!memory.start);
  if(!strcmp(name,"changed-unit")) {
   controller->config[0x4c/4]=0x08080080;route_error(route,-ESTALE);
  } else if(!strcmp(name,"changed-control")) {
   mmio[0x18]=1;route_error(route,-ESTALE);mmio[0x18]=0;
  } else if(!strcmp(name,"removed-client")) {
   device_unregister(client);route_error(route,-ENODEV);
  } else if(!strcmp(name,"replacement")) {
   scope_hw[2][3][2].config[0]=0x99991234;pci_rescan();route_error(route,-ENODEV);
   device_t *newclient=scope_device(0x21a);assert(newclient!=client);
   amd_iommu_route_t *newroute=NULL;assert(!amd_iommu_route_acquire(newclient,&newroute));
   amd_iommu_route_release(newroute);device_put(newclient);
  } else if(!strcmp(name,"range-anchor")) {
   root->config[0]=0x99991234;pci_rescan();route_error(route,-ESTALE);
  } else if(!strcmp(name,"alias-replacement")) {
   scope_hw[2][4][0].config[0]=0x99991234;pci_rescan();route_error(route,-ESTALE);
  } else if(!strcmp(name,"controller-removed")) {
   controller->present=0;pci_rescan();route_error(route,-ESHUTDOWN);
   assert(route_tables==1&&!route_unmaps);route_hw_map_check();
  } else if(!strcmp(name,"detach")) {
   amd_iommu_route_t *second=NULL;assert(!amd_iommu_route_acquire(client,&second));
   struct amdvi_owner *old=provider->drv_priv;
   assert(!driver_unregister(&amdvi_driver)&&!provider->drv&&old->detached&&old->routes==2);
   route_error(route,-ESHUTDOWN);route_acquire_error(client,-ENODEV);
   assert(!amd_iommu_route_memory(route,0,&memory)&&memory.start==0x100000);
   assert(route_tables==1&&!route_unmaps);route_hw_map_check();
   amd_iommu_route_release(second);assert(route_tables==1&&!route_unmaps);
   driver_discovery_begin();assert(!driver_register(&amdvi_driver));driver_discovery_end();
   assert(!bus_probe_device(provider)&&provider->drv_priv!=old&&route_tables==2);
   route_error(route,-ESHUTDOWN);
   amd_iommu_route_t *fresh=NULL;assert(!amd_iommu_route_acquire(client,&fresh));
   amd_iommu_route_release(route);route=fresh;
   assert(route_tables==1&&route_unmaps==1&&!amd_iommu_route_validate(route,&info));
  }
  amd_iommu_route_release(route);
 }
 assert(!driver_unregister(&amdvi_driver)&&!provider->drv&&!provider->drv_priv);
 assert(!amdvi_owners&&!route_tables&&route_maps==route_unmaps);
 if(strcmp(name,"removed-client")&&strcmp(name,"replacement"))assert(client->references==client_refs);
cleanup:
 device_put(client);device_put(provider);memset(scope_hw,0,sizeof(scope_hw));pci_rescan();
 assert(!g_device_count&&route_peer.references==1);
 for(unsigned i=0;i<PCI_MAX_PUBLISHED_DEVICES;i++)assert(g_pci_devs[i].references<=1);
}
'''


class AmdIommuRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86_64 hardware-access headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-amdvi-route-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        first = C_FIXTURE.index('uint32_t arch_pci_config_read32(')
        last = C_FIXTURE.index('static uint8_t mmio[4096]', first)
        source = C_FIXTURE[:first] + HARDWARE + C_FIXTURE[last:]
        source = source.replace('static uint8_t sizing[6];', '')
        source = source.replace('static uint8_t mmio[4096]', 'static uint8_t mmio[0x4000]')
        old = 'void *arch_device_map_mmio(uint64_t pa,size_t bytes,uint32_t flags) {(void)flags;assert(pa==0x10000000&&bytes==4096);return mmio;}\nvoid arch_device_unmap_mmio(void *p,size_t bytes) {assert(p==mmio&&bytes==4096);}'
        assert source.count(old) == 1
        source = source.replace(old, 'static void *route_map(uint64_t,size_t,uint32_t);\nstatic void route_unmap(void *,size_t);\nvoid *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f){return route_map(pa,n,f);}\nvoid arch_device_unmap_mmio(void *p,size_t n){route_unmap(p,n);}')
        marker = 'int main(int argc,char **argv) {'
        cases = CASES
        for suffix in ('acquire', 'validate', 'memory', 'release'):
            cases = cases.replace('amd_iommu_route_' + suffix + '(', 'checked_' + suffix + '(')
        source = source.replace(marker, SCOPE_CASES.split('static void scope_expect(')[0] + WRAPPERS + cases + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))', 'if(!strncmp(argv[1],"route-",6))route_case(argv[1]+6);\n else if(!strcmp(argv[1],"pci-retired"))')
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
            str(ROOT / 'kernel/firmware/acpi/ivrs_policy.c'), '-o', str(cls.binary),
        ], check=True)

    def run_cases(self, *names):
        for name in names:
            with self.subTest(case=name):
                subprocess.run([str(self.binary), 'route-' + name], check=True, timeout=10)

    def test_firmware_topology_and_optional_hardware_range(self):
        self.run_cases('firmware', 'range')

    def test_alias_paths_and_both_ivmd_sources(self):
        self.run_cases('alias', 'alias-range')

    def test_arguments_and_allocation_rollback(self):
        self.run_cases('arguments', 'allocation')

    def test_unbound_provider_and_missing_alias_target(self):
        self.run_cases('unbound', 'alias-missing')

    def test_changed_or_insufficient_hardware_scope(self):
        self.run_cases('range-missing', 'range-root-type', 'alias-range-outside', 'changed-unit', 'changed-control')

    def test_ambiguous_firmware_ownership_cannot_select_first_unit(self):
        self.run_cases('ambiguous')

    def test_live_client_registration_and_identity(self):
        self.run_cases('removed-client', 'replacement')

    def test_anchor_and_alias_replacement_do_not_refresh_old_handles(self):
        self.run_cases('range-anchor', 'alias-replacement')

    def test_last_handle_retains_aperture_and_firmware_across_rebind(self):
        self.run_cases('detach', 'controller-removed')


if __name__ == '__main__':
    unittest.main()

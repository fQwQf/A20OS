"""Published PCI/core scope observations with live, bounded config hardware."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

if __package__:
    from .test_native_bus_lifetime import ROOT, C_FIXTURE
else:
    from test_native_bus_lifetime import ROOT, C_FIXTURE


HARDWARE = r'''
struct scope_function { uint32_t config[64]; int present; };
static struct scope_function scope_hw[4][32][8];
static unsigned scope_writes;
static struct scope_function *scope_function(int bus,int slot,int fn) {
 if(bus<0||bus>=4||slot<0||slot>=32||fn<0||fn>=8)return NULL;
 struct scope_function *p=&scope_hw[bus][slot][fn];return p->present?p:NULL;
}
uint32_t arch_pci_config_read32(int bus,int slot,int fn,uint32_t reg) {
 struct scope_function *p=scope_function(bus,slot,fn);
 return p&&reg<256?p->config[(reg&~3U)/4]:0xffffffff;
}
void arch_pci_config_write32(int bus,int slot,int fn,uint32_t reg,uint32_t value) {
 struct scope_function *p=scope_function(bus,slot,fn);assert(p&&reg<256);scope_writes++;
 reg&=~3U;
 if(reg>=0x10&&reg<0x28&&value==0xffffffff)return; /* Unimplemented BARs. */
 if(reg==4)p->config[1]=(p->config[1]&0xffff0000&~value)|(value&0xffff);
 else p->config[reg/4]=value;
}
'''

CASES = r'''
#include "drivers/core/amd_iommu.h"
static struct scope_function *scope_add(unsigned bus,unsigned slot,unsigned fn,int bridge) {
 struct scope_function *p=&scope_hw[bus][slot][fn];p->present=1;
 p->config[0]=0x56781234;p->config[2]=bridge?0x06040000:0x03000000;
 p->config[3]=bridge?0x00010000:0;return p;
}
static struct scope_function *scope_bridge(unsigned bus,unsigned slot,unsigned secondary,unsigned subordinate) {
 struct scope_function *p=scope_add(bus,slot,0,1);
 p->config[6]=bus|(secondary<<8)|(subordinate<<16);
 p->config[1]|=1U<<20;p->config[0x34/4]=0x40;
 p->config[0x40/4]=0x10|(2U<<16)|(4U<<20);return p;
}
static device_t *scope_device(unsigned requester) {
 device_t *d=NULL;assert(!pci_device_find_get(0,requester,&d));return d;
}
static void scope_expect(device_t *client,unsigned bus,unsigned first,unsigned last,int error,device_t *expected) {
 unsigned before=scope_writes;device_t *out=(void *)1;
 unsigned refs=expected?expected->references:0;
 assert(pci_device_scope_get(client,bus,first,last,&out)==error);
 assert(out==expected&&scope_writes==before);
 if(out){assert(out->references==refs+1);device_put(out);}
}
static void scope_case(const char *name) {
 struct scope_function *root=scope_bridge(0,1,1,2);
 struct scope_function *middle=scope_bridge(1,2,2,2);
 struct scope_function *leaf=scope_add(2,3,2,0);
 pci_enumerate(0,0,1);
 device_t *client=scope_device(0x21a),*top=scope_device(8),*mid=scope_device(0x110);
 if(!strcmp(name,"path")) {
  scope_expect(client,0,8,8,0,top);
  scope_expect(client,1,0x10,0x10,0,mid);
  scope_expect(client,2,0x1a,0x1a,0,client);
  scope_expect(client,2,0x18,0x19,-ENODEV,NULL); /* Function bits matter. */
  scope_expect(client,3,0,255,-ENODEV,NULL);
  scope_expect(client,0,9,8,-EINVAL,NULL);
  device_t *out=(void *)1;assert(pci_device_scope_get(NULL,0,0,255,&out)==-EINVAL&&!out);
  assert(pci_device_scope_get(client,0,0,255,NULL)==-EINVAL);
  device_t foreign={.name="not-pci",.references=1};scope_expect(&foreign,0,0,255,-ENODEV,NULL);
 } else if(!strcmp(name,"overlap")) {
  /* Wider sibling starts on bus 2, which is not on the 3 -> 1 -> 0 path.
   * This must exercise overlap detection, not duplicate-secondary checks. */
  root->config[6]=0x00030100;middle->config[6]=0x00030301;
  leaf->present=0;scope_add(3,3,2,0);scope_bridge(0,4,2,3);pci_rescan();
  device_put(client);client=scope_device(0x31a);
  scope_expect(client,0,0,255,-EBUSY,NULL);
 } else if(!strcmp(name,"duplicate")) {
  scope_bridge(1,4,2,2);pci_rescan();scope_expect(client,0,0,255,-EBUSY,NULL);
 } else if(!strcmp(name,"window")) {
  root->config[6]=0x00010100;scope_expect(client,0,0,255,-EINVAL,NULL);
 } else if(!strcmp(name,"primary")) {
  middle->config[6]=0x00020200;scope_expect(client,0,0,255,-EINVAL,NULL);
 } else if(!strcmp(name,"cycle")) {
  root->present=0;scope_bridge(2,4,1,2);
  pci_enumerate(0,0,3);pci_rescan();scope_expect(client,0,0,255,-ELOOP,NULL);
 } else if(!strcmp(name,"vanished")) {
  root->present=0;scope_expect(client,0,0,255,-ENODEV,NULL);
 } else if(!strcmp(name,"replaced")) {
  leaf->config[0]=0x99991234;scope_expect(client,0,0,255,-ENODEV,NULL);
  pci_rescan();scope_expect(client,0,0,255,-ENODEV,NULL);
  device_t *replacement=scope_device(0x21a);assert(replacement!=client);
  scope_expect(replacement,0,0,255,0,top);device_put(replacement);
 } else if(!strcmp(name,"pinned")) {
  device_t *anchor=NULL;assert(!pci_device_scope_get(client,0,0,255,&anchor)&&anchor==top);
  root->config[0]=0x99991234;pci_rescan();
  assert(pci_device_id(anchor)==0x12345678&&anchor->state==DEV_STATE_REMOVED);
  device_t *replacement=scope_device(8);assert(replacement!=anchor);device_put(replacement);device_put(anchor);
 } else if(!strcmp(name,"missing")) {
  middle->present=0;pci_enumerate(0,0,3);pci_rescan();scope_expect(client,0,0,255,-EBUSY,NULL);
 } else if(!strcmp(name,"class")) {
  middle->config[2]=0x03000000;scope_expect(client,0,0,255,-EOPNOTSUPP,NULL);
 } else if(!strcmp(name,"amd")) {
  amd_iommu_snapshot_t snapshot={.range=0x08080080,.range_valid=true};
  device_t *anchor=NULL;unsigned before=scope_writes;
  assert(!amd_iommu_scope_get(&snapshot,client,&anchor)&&anchor==top);device_put(anchor);
  root->config[0x40/4]=0x10|(1U<<16)|(4U<<20);
  assert(!amd_iommu_scope_get(&snapshot,client,&anchor)&&anchor==top);device_put(anchor);
  snapshot.range=0x1a1a0280;
  assert(!amd_iommu_scope_get(&snapshot,client,&anchor)&&anchor==client);device_put(anchor);
  snapshot.range=0xff000000;anchor=(void *)1;
  assert(amd_iommu_scope_get(&snapshot,client,&anchor)==-EOPNOTSUPP&&!anchor);
  snapshot.segment=1;snapshot.range=0x08080080;
  assert(amd_iommu_scope_get(&snapshot,client,&anchor)==-EOPNOTSUPP&&!anchor);
  assert(scope_writes==before);
 } else if(!strncmp(name,"cap-",4)) {
  amd_iommu_snapshot_t snapshot={.range=0x08080080,.range_valid=true};
  int expected=-EINVAL;
  if(!strcmp(name,"cap-tail"))root->config[0x40/4]|=0x42<<8;
  else if(!strcmp(name,"cap-payload")){root->config[0x40/4]|=0x50<<8;root->config[0x50/4]=5;}
  else if(!strcmp(name,"cap-truncated")){root->config[0x34/4]=0xfc;root->config[0xfc/4]=0x10|(2U<<16)|(4U<<20);}
  else if(!strcmp(name,"cap-cycle"))root->config[0x40/4]|=0x40<<8;
  else if(!strcmp(name,"cap-duplicate")) {root->config[0x40/4]|=0x50<<8;root->config[0x50/4]=0x10|(2U<<16)|(4U<<20);}
  else if(!strcmp(name,"cap-type")){root->config[0x40/4]=0x10|(2U<<16)|(6U<<20);expected=-EOPNOTSUPP;}
  else if(!strcmp(name,"cap-version")){root->config[0x40/4]=0x10|(3U<<16)|(4U<<20);expected=-EOPNOTSUPP;}
  else {assert(!strcmp(name,"cap-missing"));root->config[1]=0;expected=-EOPNOTSUPP;}
  device_t *anchor=(void *)1;assert(amd_iommu_scope_get(&snapshot,client,&anchor)==expected&&!anchor);
 } else assert(0);
 device_put(client);device_put(top);device_put(mid);
 memset(scope_hw,0,sizeof(scope_hw));pci_rescan();assert(!g_device_count);
}
'''


class PciScopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64"):
            arch, macro = "x86_64", "CONFIG_X86_64"
        elif machine in ("aarch64", "arm64"):
            arch, macro = "aarch64", "CONFIG_AARCH64"
        else:
            raise unittest.SkipTest("host architecture headers required")
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-pci-scope-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        first = C_FIXTURE.index('uint32_t arch_pci_config_read32(')
        last = C_FIXTURE.index('static uint8_t mmio[4096]', first)
        source = C_FIXTURE[:first] + HARDWARE + C_FIXTURE[last:]
        assert source.count('static uint8_t sizing[6];') == 1
        source = source.replace('static uint8_t sizing[6];', '')
        marker = 'int main(int argc,char **argv) {'
        assert source.count(marker) == 1
        source = source.replace(marker, CASES + '\n' + marker)
        marker = 'if(!strcmp(argv[1],"pci-retired"))'
        source = source.replace(marker, 'if(!strncmp(argv[1],"scope-",6))scope_case(argv[1]+6);\n else ' + marker)
        path = work / "fixture.c"
        path.write_text(source)
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-D" + macro, "-DCONFIG_NR_CPUS=1", "-DCONFIG_DRIVER_LIFECYCLE_TEST",
            "-I" + str(ROOT / "kernel/arch" / arch / "include"),
            "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
            str(path), str(ROOT / "kernel/drivers/core/amd_iommu_scope.c"),
            "-o", str(cls.binary),
        ], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), "scope-" + name], check=True, timeout=10)

    def test_nested_and_integrated_scope_references(self):
        self.run_case("path")

    def test_overlapping_sibling_and_duplicate_parent_rejected(self):
        self.run_case("overlap")
        self.run_case("duplicate")

    def test_missing_bridge_is_not_inferred_from_subordinate_range(self):
        self.run_case("missing")

    def test_invalid_windows_class_and_cycles(self):
        for case in ["window", "primary", "cycle", "class"]:
            with self.subTest(case=case): self.run_case(case)

    def test_live_identity_and_retired_scope_reference(self):
        for case in ["vanished", "replaced", "pinned"]:
            with self.subTest(case=case): self.run_case(case)

    def test_amd_range_requires_validity_and_root_port(self):
        self.run_case("amd")

    def test_entire_pcie_capability_chain_is_checked(self):
        for case in ["cap-tail", "cap-payload", "cap-truncated", "cap-cycle", "cap-duplicate", "cap-type", "cap-version", "cap-missing"]:
            with self.subTest(case=case): self.run_case(case)


if __name__ == '__main__':
    unittest.main()

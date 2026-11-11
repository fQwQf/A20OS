"""Real PCI/core integration: guarded apertures and atomic BAR planning."""
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
static void check_pci_checked_aperture(int mode) {
 device_t owner={.name="iommu-owner",.references=1};
 assert(!device_register(&owner));
 resource_t aperture={.type=RES_MMIO,.start=0x10000000,.end=0x10003fff};
 driver_resource_claim_t *claim;
 if(mode==1) {
  aperture.start=0x10008000;aperture.end=0x10008fff;
  assert(!driver_resource_request(&owner,&aperture,&claim));
  masks[0]=0xfffff000;cfg[4]=0;
  masks[1]=0xffffff01;cfg[5]=0xc001;
  masks[2]=0xfffff000;cfg[6]=0x10008000;
  cfg[1]=0x20000006; /* I/O remains disabled if a later BAR fails. */
  pci_insert();device_t *dev=pci_current();
  pci_dev_info_t before=*(pci_dev_info_t *)dev->plat_data;
  resource_t old_resources[7];memcpy(old_resources,dev->res,sizeof(old_resources));
  int old_count=dev->res_count;
  assert(pci_enable_and_assign_bars(dev)==-EBUSY);
  assert(cfg[4]==0&&cfg[5]==0xc001&&cfg[6]==0x10008000&&cfg[1]==0x20000006);
  assert(!memcmp(&before,dev->plat_data,sizeof(before)));
  assert(old_count==dev->res_count&&!memcmp(old_resources,dev->res,sizeof(old_resources)));
  driver_resource_release(claim);pci_remove();
 } else {
  assert(!driver_resource_request_checked(&owner,&aperture,&claim));
  present=1;cfg[0]=0x56781234;masks[0]=0xfffff000;
  cfg[4]=mode==2?0x10000000:0;
  pci_enumerate(0,0,1);
  if(mode==2) {
   assert(!pci_current()&&g_device_count==1); /* Publication cannot bypass lease. */
   driver_resource_release(claim);pci_rescan();assert(pci_current());pci_remove();
  } else {
   device_t *dev=pci_current();assert(dev);
   assert(!pci_enable_and_assign_bars(dev));
   assert(cfg[4]==0x10004000&&pci_get_bar_resource(dev,0)->start==0x10004000);
   pci_remove();driver_resource_release(claim);
  }
 }
 device_unregister(&owner);assert(owner.references==1);device_put(&owner);
}
'''


class ResourcePciIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64"):
            arch, macro = "x86_64", "CONFIG_X86_64"
        elif machine in ("aarch64", "arm64"):
            arch, macro = "aarch64", "CONFIG_AARCH64"
        else:
            raise unittest.SkipTest("host architecture headers required")
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-resource-pci-")
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        marker = "int main(int argc,char **argv) {"
        assert C_FIXTURE.count(marker) == 1
        source = C_FIXTURE.replace(marker, CASES + "\n" + marker)
        marker = 'if(!strcmp(argv[1],"pci-retired"))'
        assert source.count(marker) == 1
        source = source.replace(marker,
            'if(!strcmp(argv[1],"checked-aperture"))check_pci_checked_aperture(0);\n'
            ' else if(!strcmp(argv[1],"late-bar-conflict"))check_pci_checked_aperture(1);\n'
            ' else if(!strcmp(argv[1],"checked-publication"))check_pci_checked_aperture(2);\n'
            ' else ' + marker)
        path = work / "fixture.c"
        path.write_text(source)
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
            "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-fsanitize=undefined", "-fno-sanitize-recover=all",
            "-D" + macro, "-DCONFIG_NR_CPUS=1", "-DCONFIG_DRIVER_LIFECYCLE_TEST",
            "-I" + str(ROOT / "kernel/arch" / arch / "include"),
            "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
            str(path), "-o", str(cls.binary),
        ], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=10)

    def test_allocator_skips_checked_iommu_aperture(self):
        self.run_case("checked-aperture")

    def test_late_bar_conflict_preserves_hardware_and_metadata(self):
        self.run_case("late-bar-conflict")

    def test_hotplug_publication_rejects_and_recovers(self):
        self.run_case("checked-publication")


if __name__ == "__main__":
    unittest.main()

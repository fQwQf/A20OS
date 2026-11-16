"""Host fake-HCD closure of nested disconnect, URB retirement and address reuse."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT,C_FIXTURE
from .test_usb_hid_readiness import function

class UsbDisconnectTree(unittest.TestCase):
 def test_nested_failure_retry_and_64_complete_address_reuse_cycles(self):
  arch={'x86_64':('x86_64','CONFIG_X86_64'),'aarch64':('aarch64','CONFIG_AARCH64')}.get(platform.machine().lower())
  if not arch:self.skipTest('host architecture headers required')
  hub=(ROOT/'kernel/drivers/usb/class/usb_hub.c').read_text()
  a=hub.index('typedef struct usb_hub');b=hub.index('} usb_hub_t;',a)+len('} usb_hub_t;')
  remove=function(hub,'usb_hub_prepare')+'\n'+function(hub,'usb_hub_remove')
  model=(ROOT/'tools/tests/usb_disconnect_tree_host.c').read_text().replace('/* HUB_SHAPE */',hub[a:b])
  core=(ROOT/'kernel/drivers/usb/core/usb_core.c').read_text()
  abort='int ret = hcd->ops->abort_slot(hcd, udev->slot);'
  address='if (address_owned) usb_core_free_address(address);'
  self.assertEqual(core.count(abort),1);self.assertEqual(core.count(address),1)
  with tempfile.TemporaryDirectory(prefix='a20-usb-tree-') as d:
   p=Path(d)
   for name,usb,hub_remove in [('fixed',core,remove),
     ('missing-abort',core.replace(abort,'int ret = 0;'),remove),
     ('leaked-address',core.replace(address,'(void)address_owned;'),remove),
     ('missing-downstream',core,remove.replace('int ret = usb_core_unregister_hcd(&hub->hcd);','int ret = 0;'))]:
    source=C_FIXTURE.replace('#include "drivers/usb/core/usb_core.c"',usb)
    marker='int main(int argc,char **argv) {'
    source=source.replace(marker,model.replace('/* HUB_REMOVE */',hub_remove)+'\n'+marker)
    source=source.replace('if(!strcmp(argv[1],"pci-retired"))','if(!strcmp(argv[1],"tree"))tree_case();\n else if(!strcmp(argv[1],"pci-retired"))')
    c=p/(name+'.c');c.write_text(source);exe=p/name
    subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin',
     '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',
     '-D'+arch[1],'-DCONFIG_NR_CPUS=1','-DCONFIG_DRIVER_LIFECYCLE_TEST',
     '-I'+str(ROOT/'kernel/arch'/arch[0]/'include'),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),str(c),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe),'tree'],capture_output=True,text=True,timeout=15)
    if name=='fixed':self.assertEqual(r.returncode,0,r.stdout+r.stderr)
    else:self.assertNotEqual(r.returncode,0,name)
if __name__=='__main__':unittest.main()

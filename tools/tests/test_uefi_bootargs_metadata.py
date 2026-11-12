"""EFI-generated boot counters do not shadow explicit emulator options."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define FW_CFG_SELECTOR_PORT 0x510
#define FW_CFG_SIGNATURE 0
#define FW_CFG_CMDLINE_FILE "opt/x86/cmdline"
static char g_bootargs[256];
static int qemu=1,writes;
static int fw_cfg_is_qemu(void){return qemu;}
static void outw(unsigned p,unsigned v){assert(p==0x510&&v==0);writes++;}
static uint32_t fw_cfg_read32(void){return 0x51454d55U;}
static int fw_cfg_read_file(const char*n,char*out,unsigned cap){(void)n;assert(cap>20);strcpy(out,"a20.hpet32=1");return 0;}
'''
MAIN=r'''
int main(void){
 assert(firmware_bootcount_only(" a20.bootcount=13"));
 assert(!firmware_bootcount_only("a20.bootcount="));
 assert(!firmware_bootcount_only("a20.bootcount=13 a20.hpet32=0"));
 strcpy(g_bootargs," a20.bootcount=13");fw_cfg_append_loader_options();
 assert(!strcmp(g_bootargs," a20.bootcount=13 a20.hpet32=1")&&writes==1);
 strcpy(g_bootargs,"a20.hpet32=0 a20.bootcount=13");fw_cfg_append_loader_options();
 assert(!strcmp(g_bootargs,"a20.hpet32=0 a20.bootcount=13")&&writes==1);
 qemu=0;strcpy(g_bootargs," a20.bootcount=13");fw_cfg_append_loader_options();
 assert(!strcmp(g_bootargs," a20.bootcount=13")&&writes==1);return 0;
}
'''
class BootargsMetadata(unittest.TestCase):
 def test_metadata_merge_and_physical_io_guard(self):
  s=(ROOT/'kernel/arch/x86_64/platform/firmware.c').read_text()
  body=function(s,'firmware_bootcount_only')+function(s,'fw_cfg_append_loader_options')
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'options.c';exe=p.with_suffix('');p.write_text(PRE+body+MAIN)
   subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()

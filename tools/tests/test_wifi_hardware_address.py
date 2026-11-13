"""Production BSD MAC publication updates protocol and observable interface state."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define BSD_MAX_NICS 4
#define NIC_CARRIER_UP 2
#define ETH_HWADDR_LEN 6
typedef int device_t;
typedef struct {device_t *device;void *context;unsigned attached,carrier;int has_ip;uint8_t mac[6];} bsd_nic_slot_t;
static bsd_nic_slot_t nics[4];
typedef struct {device_t *device;uint8_t mac[6];} hwaddr_job_t;
static unsigned protocol_calls,notifies;static int protocol_error;
static uint8_t protocol_mac[6];
static int a20_bsd_port_nic_set_hwaddr(void *c,const uint8_t *m){assert(c==(void*)123);protocol_calls++;
 if(protocol_error)return protocol_error;memcpy(protocol_mac,m,6);return 0;}
static unsigned public_index_for_slot(int i){return i?i+2:1;}
static void net_netlink_link_notify_one(unsigned i,int up){assert(i==3&&!up);notifies++;}
static int a20_bsd_executor_call(int (*f)(void*),void *arg){return f(arg);}
'''
MAIN=r'''
int main(void){
 device_t device=0;uint8_t mac[6]={0x34,0x12,0x34,0x56,0x78,0x9a};
 assert(device_hwaddr(&device,mac)==-ENODEV);
 nics[1]=(bsd_nic_slot_t){.device=&device,.context=(void*)123,.attached=1};
 protocol_error=EIO;assert(device_hwaddr(&device,mac)==-EIO&&!notifies&&nics[1].mac[0]==0);
 protocol_error=0;assert(!device_hwaddr(&device,mac));
 assert(!memcmp(nics[1].mac,protocol_mac,6)&&!memcmp(protocol_mac,mac,6));
 assert(protocol_calls==2&&notifies==1);
 nics[1].carrier=NIC_CARRIER_UP;assert(device_hwaddr(&device,mac)==-EBUSY);
 nics[1].carrier=0;nics[1].has_ip=1;assert(device_hwaddr(&device,mac)==-EBUSY);
 nics[1].has_ip=0;mac[0]=0x35;assert(device_hwaddr(&device,mac)==-EINVAL);
 memset(mac,0,6);assert(device_hwaddr(&device,mac)==-EINVAL);
 assert(protocol_calls==2);return 0;
}
'''
class HardwareAddress(unittest.TestCase):
 def test_bsd_executor_publication(self):
  text=(ROOT/'kernel/net/backends/bsd/runtime.c').read_text()
  code=function(text,'hwaddr_job_run')+function(text,'device_hwaddr')
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'mac.c';exe=p.with_suffix('');p.write_text(PRE+code+MAIN)
   subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=undefined',str(p),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()

"""Exercise production Wi-Fi ioctls with A20OS byte-count usercopy semantics."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "kernel/include/uapi/a20/wifi.h"
#define WIFI_DMA_SLOTS 4
#define WIFI_DMA_BUDGET (32U<<20)
#define PAGE_SIZE 4096
#define ROUND_UP(n,a) (((n)+(a)-1)&~((a)-1))
#define PCI_COMMAND 4
#define PCI_INTX_DISABLE 0x400
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
typedef struct {int unused;} vfile_t;
static struct {void *mm;} task;
static void *proc_current_dummy;
#define proc_current() (&task)
static unsigned char config[4096];
static int device;
struct wifi_dma {void *memory;size_t size;uint64_t bus,token,mapped;};
static struct {
 void *device,*owner;size_t bar_size,dma_bytes;int control,carrier,mac_ready;
 uint64_t next_token;uint8_t mac[6];
 struct wifi_dma dma[WIFI_DMA_SLOTS];
} wifi;
static void mutex_lock(int*x){(void)x;}
static void mutex_unlock(int*x){(void)x;}
/* Production usercopy returns n on success, -EFAULT on a bad pointer. */
static long copy_to_user(void*d,const void*s,size_t n){
 if(!d)return -EFAULT;memcpy(d,s,n);return n;
}
static long copy_from_user(void*d,const void*s,size_t n){
 if(!s)return -EFAULT;memcpy(d,s,n);return n;
}
static uint32_t pci_cfg_read32(void*d,unsigned o){uint32_t v;(void)d;memcpy(&v,config+o,4);return v;}
static uint8_t pci_cfg_read8(void*d,unsigned o){(void)d;return config[o];}
static uint16_t pci_cfg_read16(void*d,unsigned o){uint16_t v;(void)d;memcpy(&v,config+o,2);return v;}
static void pci_cfg_write32(void*d,unsigned o,uint32_t v){(void)d;memcpy(config+o,&v,4);}
static void pci_cfg_write16(void*d,unsigned o,uint16_t v){(void)d;memcpy(config+o,&v,2);}
static void *dma_alloc_coherent_aligned(void*d,size_t n,size_t a,uint64_t*b){
 (void)d;void *p=aligned_alloc(a,n);*b=0x100000;return p;
}
#define wifi_dma_free(b) do{free((b)->memory);wifi.dma_bytes-=(b)->size;memset((b),0,sizeof(*(b)));}while(0)
static uint64_t dma_get_mask(void*d){(void)d;return UINT32_MAX;}
static int master;
static int mac_error;
static uint8_t published_mac[6];
static int net_stack_device_hwaddr(void *d,const uint8_t *m){
 (void)d;if(mac_error)return mac_error;memcpy(published_mac,m,6);return 0;
}
static void wifi_bus_master(int x){master=x;}
'''
MAIN = r'''
int main(void){
 (void)proc_current_dummy;task.mm=&task;wifi.owner=task.mm;wifi.device=&device;
 wifi.bar_size=1U<<20;wifi.next_token=1;memcpy(wifi.mac,"abcdef",6);
 pci_cfg_write16(&device,2,0xb852);pci_cfg_write32(&device,0x2c,0x485317aa);
 struct a20_wifi_info info={0};
 assert(wifi_ioctl(NULL,A20_WIFI_INFO,&info)==0);
 assert(info.version==A20_WIFI_ABI && info.device==0xb852 && info.subvendor==0x17aa);
 assert(info.mmio_size==1U<<20 && !memcmp(info.mac,"abcdef",6));
 assert(wifi_ioctl(NULL,A20_WIFI_INFO,NULL)==-EFAULT);
 for(unsigned width=1;width<=4;width*=2){
  struct a20_wifi_cfg cfg={.offset=0x80,.width=width,.value=0x12345678};
  assert(wifi_ioctl(NULL,A20_WIFI_CFG_WRITE,&cfg)==0);cfg.value=0;
  assert(wifi_ioctl(NULL,A20_WIFI_CFG_READ,&cfg)==0);
  assert(cfg.value==(0x12345678U & (UINT32_MAX >> ((4-width)*8))));
 }
 struct a20_wifi_cfg bad={.offset=4096,.width=4};
 assert(wifi_ioctl(NULL,A20_WIFI_CFG_READ,&bad)==-EINVAL);
 assert(wifi_ioctl(NULL,A20_WIFI_CFG_WRITE,NULL)==-EFAULT);
 struct a20_wifi_dma dma={.size=5000};
 assert(wifi_ioctl(NULL,A20_WIFI_DMA_ALLOC,&dma)==0);
 assert(dma.size==8192 && dma.token==1 && dma.bus_address==0x100000 && wifi.dma_bytes==8192);
 assert(wifi_ioctl(NULL,A20_WIFI_DMA_FREE,NULL)==-EFAULT);
 assert(wifi_ioctl(NULL,A20_WIFI_DMA_FREE,&dma)==0 && wifi.dma_bytes==0);
 struct a20_wifi_link link={.authorized=1};
 assert(wifi_ioctl(NULL,A20_WIFI_LINK,&link)==-ENODATA && !wifi.carrier);
 struct a20_wifi_hwaddr address={.mac={0x34,0x12,0x34,0x56,0x78,0x9a}};
 mac_error=-ENODEV;
 assert(wifi_ioctl(NULL,A20_WIFI_HWADDR,&address)==-ENODEV && !wifi.mac_ready);
 mac_error=0;
 address.reserved[0]=1;assert(wifi_ioctl(NULL,A20_WIFI_HWADDR,&address)==-EINVAL);
 address.reserved[0]=0;address.mac[0]=0x35;
 assert(wifi_ioctl(NULL,A20_WIFI_HWADDR,&address)==-EINVAL);
 address.mac[0]=0x34;
 assert(wifi_ioctl(NULL,A20_WIFI_HWADDR,&address)==0 && wifi.mac_ready);
 assert(!memcmp(wifi.mac,address.mac,6) && !memcmp(published_mac,address.mac,6));
 address.mac[5]++;assert(wifi_ioctl(NULL,A20_WIFI_HWADDR,&address)==-EBUSY);
 address.mac[5]--;
 assert(wifi_ioctl(NULL,A20_WIFI_LINK,&link)==0 && wifi.carrier==1);
 assert(wifi_ioctl(NULL,A20_WIFI_HWADDR,&address)==-EBUSY);
 assert(wifi_ioctl(NULL,A20_WIFI_LINK,NULL)==-EFAULT && wifi.carrier==1);
 link.authorized=2;assert(wifi_ioctl(NULL,A20_WIFI_LINK,&link)==-EINVAL && wifi.carrier==1);
 master=1;assert(wifi_ioctl(NULL,A20_WIFI_QUIESCE,NULL)==0 && !wifi.carrier && !master);
 task.mm=NULL;assert(wifi_ioctl(NULL,A20_WIFI_INFO,&info)==-EPERM);
 return 0;
}
'''

class WifiBrokerTest(unittest.TestCase):
    def test_real_usercopy_contract(self):
        source = (ROOT / "kernel/drivers/net/rtw89_broker.c").read_text()
        production = function(source, "wifi_ioctl")
        with tempfile.TemporaryDirectory(prefix="a20-wifi-broker-") as temporary:
            path = Path(temporary)
            def run(body, name):
                code = path / (name + ".c")
                code.write_text(PRELUDE + body + MAIN)
                exe = path / name
                subprocess.run(["cc", "-std=c11", "-I", str(ROOT), "-fsanitize=undefined",
                                str(code), "-o", str(exe)], check=True, capture_output=True)
                return subprocess.run([str(exe)], capture_output=True)
            self.assertEqual(run(production, "current").returncode, 0)
            broken = production.replace("copy_to_user(arg, &info, sizeof(info)) < 0",
                                        "copy_to_user(arg, &info, sizeof(info))")
            self.assertNotEqual(run(broken, "negative").returncode, 0)

if __name__ == "__main__":
    unittest.main()

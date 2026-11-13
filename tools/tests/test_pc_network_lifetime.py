"""Production PCI network teardown and userspace transport ownership."""
import unittest
from tools.tests.test_i2c_transport_lifetime import run, ROOT
from tools.tests.test_usb_hid_readiness import function


class PcNetworkLifetime(unittest.TestCase):
    def test_radio_rebind_cannot_overwrite_live_or_quarantined_owner(self):
        source = (ROOT / 'kernel/drivers/net/rtw89_broker.c').read_text()
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {int dummy;} device_t;
static struct {void *device,*owner;unsigned dma_bytes;int quarantined,carrier,control;} wifi;
static int wifi_open_lock,master;
static void mutex_lock(int *p){(void)p;}
static void mutex_unlock(int *p){(void)p;}
static void wifi_bus_master(int x){master=x;}
''' + function(source, 'wifi_transport_available') +
            function(source, 'wifi_remove') + r'''
int main(void){
 int token;assert(wifi_transport_available());wifi.device=&token;
 assert(!wifi_transport_available());wifi_remove(NULL);assert(wifi_transport_available()&&!master);
 wifi.owner=&token;assert(!wifi_transport_available());wifi.device=&token;wifi_remove(NULL);
 assert(!wifi_transport_available()&&wifi.quarantined);
 wifi.owner=NULL;assert(!wifi_transport_available());wifi.quarantined=0;wifi.dma_bytes=4096;
 assert(!wifi_transport_available());wifi_remove(NULL);assert(wifi.quarantined);
}
''')

    def test_nic_retains_dma_after_stop_timeout_and_retry_frees_once(self):
        source = (ROOT / 'kernel/drivers/net/r8169.c').read_text()
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef struct {unsigned ready,irq_registered,irq,irq_count,error_irqs,tx_packets,rx_packets,tx_dropped,rx_dropped;} r8169_device_t;
typedef struct {r8169_device_t *drv_priv;} device_t;
#define R8169_INTRMASK 1
#define R8169_INTRSTAT 2
#define R8169_CHIPCMD 3
#define R8169_CMD_TE 4
#define R8169_CMD_RE 8
#define kinfo(...) ((void)0)
static int stuck=1,irqs,rings,freed,delays;
static void free_irq(uint32_t i,device_t *d){(void)i;assert(d);irqs++;}
static void r8169_write16(r8169_device_t*n,int r,int v){(void)n;(void)r;(void)v;}
static void r8169_write8(r8169_device_t*n,int r,int v){(void)n;(void)r;(void)v;}
static int r8169_read8(r8169_device_t*n,int r){(void)n;(void)r;return stuck?12:0;}
static void udelay(unsigned x){assert(x==1);delays++;}
static void r8169_free_rings(r8169_device_t*n){assert(n);rings++;}
static void kfree(void *p){assert(p);freed++;}
''' + function(source, 'r8169_remove') + r'''
int main(void){
 r8169_device_t n={.ready=1,.irq_registered=1};device_t d={&n};
 assert(r8169_remove(&d)==-ETIMEDOUT&&d.drv_priv==&n);
 assert(!n.ready&&!n.irq_registered&&irqs==1&&!rings&&!freed&&delays==1000);
 stuck=0;assert(!r8169_remove(&d)&&!d.drv_priv&&irqs==1&&rings==1&&freed==1);
 assert(!r8169_remove(&d)&&freed==1);
}
''')


if __name__ == '__main__':
    unittest.main()

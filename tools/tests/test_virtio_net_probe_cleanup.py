"""Production net probe/init/remove with partial queue-publication failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
MODEL = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_NET_ROUTER 0
#define SPINLOCK_INIT 0
#define kinfo(...) ((void)0)
#define printf(...) ((void)0)
#define IRQF_SHARED 1
#define RES_MMIO 1
#define RES_IRQ 2
#define DRIVER_PROTOCOL_NET_STATS 1
#define mb() ((void)0)
typedef int spinlock_t;
static void spin_init(spinlock_t *p){*p=0;}
static uint64_t spin_lock_irqsave(spinlock_t *p){assert(!*p);*p=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)f;assert(*p);*p=0;}
typedef struct {uint64_t addr;uint32_t len;uint16_t flags,next;} virtq_desc_t;
typedef struct {uint32_t id,len;} virtq_used_elem_t;
typedef struct virtio_transport virtio_transport_t;
struct virtio_transport {
 uint32_t (*read32)(virtio_transport_t *,uint32_t);
 void (*write32)(virtio_transport_t *,uint32_t,uint32_t);
 int (*msix_prepare)(virtio_transport_t *,int);
 void (*msix_arm)(virtio_transport_t *);
 void (*msix_teardown)(virtio_transport_t *);
 void *priv;int irq,msix_vectors,msix_base,shared_irq;
};
static int pci_bus;
typedef struct {uint64_t start;} resource_t;
typedef struct {const char *name;void *bus,*drv_priv;int managed;} device_t;
struct driver_net_stats_ops {int unused;};
struct driver_protocol_descriptor {struct {int id,version;size_t size;int flags;} info;const void *ops;};
static struct {uint32_t status;unsigned resets,feature_writes,allocated,freed,queues,confirmed;int stuck,fail_tx;} hw;
static uint64_t ticks;
static uint64_t clock_ticks_per_sec(void){return 1000;}
static uint64_t clock_get_ticks(void){return ticks+=10;}
static void arch_cpu_relax(void){}
static void *proc_current(void){return NULL;}
static void proc_yield(void){}
static int device_add_protocol(device_t *d,const struct driver_protocol_descriptor *p){assert(d&&p);return 0;}
static int device_probe_use_remove(device_t *d){d->managed=1;return 0;}
static int device_add_execution_action(device_t *d,int (*stop)(void *),void (*release)(void *),void *context){assert(d->managed&&d->drv_priv==context&&stop&&!release);return 0;}
static int pci_virtio_transport_init(device_t *d,int type,virtio_transport_t *t){(void)t;assert(d->managed&&type==1);return -EIO;}
static resource_t *device_get_resource(device_t *d,int type,int n){static resource_t res;(void)d;assert(!n);return type==RES_MMIO?&res:NULL;}
static void *device_map_mmio(device_t *d,int n){assert(d->managed&&!n);return &hw;}
static int request_irq(uint32_t n,int (*h)(int,void *),unsigned long f,void *p){(void)n;(void)h;(void)f;(void)p;return 0;}
static void free_irq(uint32_t n,void *p){(void)n;(void)p;assert(hw.confirmed);}
static void net_stack_device_interrupt(device_t *d){(void)d;}
'''
IO = r'''
static uint32_t readl(const volatile void *address){
 uintptr_t off=(uintptr_t)address-(uintptr_t)&hw;
 switch(off){
 case VIRTIO_MMIO_MAGIC:return 0x74726976;
 case VIRTIO_MMIO_VERSION:return 2;
 case VIRTIO_MMIO_DEVICE_ID:return 1;
 case VIRTIO_MMIO_DEVICE_FEATURES:return 1;
 case VIRTIO_MMIO_STATUS:
  if(!hw.status)hw.confirmed=1;
  return hw.status;
 default:return 0;
 }
}
static void writel(uint32_t value,volatile void *address){
 uintptr_t off=(uintptr_t)address-(uintptr_t)&hw;
 if(off==VIRTIO_MMIO_STATUS){
  if(!value){hw.resets++;hw.confirmed=0;if(!hw.stuck)hw.status=0;}
  else hw.status=value;
 } else hw.feature_writes++;
}
static int virtio_net_alloc_ring(virtio_net_inst_t *net){net->ring_mem=malloc(64);assert(net->ring_mem);hw.allocated++;return 0;}
static void virtio_net_free_ring(virtio_net_inst_t *net){
 if(net->ring_mem){assert(hw.confirmed);free(net->ring_mem);net->ring_mem=NULL;hw.freed++;}
}
static int virtio_net_setup_queue(virtio_net_inst_t *net,virtio_net_queue_t *queue,int index,unsigned size){
 (void)queue;(void)size;assert(net->device->managed&&net->ring_mem);
 if(index==VIRTIO_NET_QUEUE_TX&&hw.fail_tx)return -EIO;
 hw.queues++;hw.confirmed=0;net->qsize=4;return 0;
}
static void virtio_net_seed_rx_locked(virtio_net_inst_t *net){assert(net->ring_mem);}
static void virtio_net_kick(virtio_net_inst_t *net,int queue){(void)net;(void)queue;}
static const struct driver_net_stats_ops virtio_net_stats_ops;
'''
MAIN = r'''
int main(void){
 device_t a={.name="a"},b={.name="b"},c={.name="c"};
 hw.status=7;hw.fail_tx=1;
 assert(virtio_net_driver_probe(&a)<0&&a.managed&&a.drv_priv);
 virtio_net_inst_t *first=a.drv_priv;void *first_ring=first->ring_mem;
 assert(first_ring&&first->transport_live&&!virtio_net_ready(first->slot)&&!hw.freed);
 hw.stuck=1;hw.status=7;
 assert(virtio_net_driver_remove(&a)==-ETIMEDOUT&&a.drv_priv==first&&first->device==&a);
 /* Another probe must not memset the failed instance or its live ring. */
 assert(virtio_net_driver_probe(&b)==-ETIMEDOUT&&b.drv_priv&&b.drv_priv!=first);
 assert(first->ring_mem==first_ring&&!hw.freed);
 assert(virtio_net_driver_probe(&c)==-ENOSPC&&!c.drv_priv);
 hw.stuck=0;
 assert(!virtio_net_driver_remove(&a)&&!a.drv_priv&&!first->device&&hw.freed==1);
 hw.fail_tx=0;
 /* More bindings than the static capacity reuse only fully retired slots. */
 for(unsigned round=0;round<VIRTIO_NET_MAX_DEVS+2;round++){
  assert(!virtio_net_driver_probe(&c)&&c.drv_priv==first);
  assert(virtio_net_ready(first->slot)&&first->published);
  assert(!virtio_net_call_begin(first)&&first->active_calls==1);
  assert(virtio_net_stop(first)==-EBUSY&&first->valid);
  virtio_net_call_end(first);
  assert(!virtio_net_driver_remove(&c)&&!c.drv_priv&&!first->device);
 }
 assert(!virtio_net_driver_remove(&b)&&!b.drv_priv&&hw.allocated==hw.freed);
 /* Transport construction failure never resets an unvalidated transport. */
 device_t early={.name="early",.bus=&pci_bus};unsigned resets=hw.resets;
 assert(virtio_net_driver_probe(&early)<0&&early.drv_priv);
 assert(!virtio_net_driver_remove(&early)&&!early.drv_priv&&hw.resets==resets);
 /* Initial reset must finish before feature writes or ring allocation. */
 device_t stuck={.name="stuck"};hw.stuck=1;hw.status=7;
 unsigned features=hw.feature_writes,allocated=hw.allocated;
 assert(virtio_net_driver_probe(&stuck)==-ETIMEDOUT&&stuck.drv_priv);
 assert(hw.feature_writes==features&&hw.allocated==allocated);
 assert(virtio_net_driver_remove(&stuck)==-ETIMEDOUT&&stuck.drv_priv);
 hw.stuck=0;assert(!virtio_net_driver_remove(&stuck)&&!stuck.drv_priv);
 return 0;
}
'''


class VirtioNetProbeCleanup(unittest.TestCase):
    def test_partial_queue_probe_keeps_owner_and_reuses_only_retired_slots(self):
        source = (ROOT / 'kernel/drivers/net/virtio_net.c').read_text()
        # Keep the production private layout and configuration constants.
        layout = source[source.index('#if CONFIG_NET_ROUTER'):source.index('static void virtio_net_select_queue(')]
        header = (ROOT / 'kernel/include/drivers/block/virtio_blk.h').read_text()
        constants = '\n'.join(line for line in header.splitlines()
                              if line.startswith('#define VIRTIO_MMIO_') or line.startswith('#define VIRTIO_STATUS_')
                              or line.startswith('#define VIRTIO_F_VERSION_1_BIT'))
        reset = function((ROOT / 'kernel/drivers/bus/virtio_transport.c').read_text(), 'virtio_transport_reset')
        reset += function(source[source.index('static int virtio_net_stop(void *context)\n{'):], 'virtio_net_stop')
        names = ('virtio_net_init_instance', 'virtio_net_ready', 'virtio_net_mmio_read32',
                 'virtio_net_mmio_write32', 'virtio_net_driver_probe', 'virtio_net_driver_remove')
        code = '\n'.join(function(source, name) for name in names)
        irq = function(source[source.index('static int virtio_net_irq_handler(int irq, void *priv) {'):],
                       'virtio_net_irq_handler')
        with tempfile.TemporaryDirectory(prefix='a20-net-probe-') as directory:
            path = Path(directory) / 'probe.c'
            binary = path.with_suffix('')
            path.write_text(MODEL + constants + '\n' + layout + IO + reset + irq + code + MAIN)
            built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-variable', '-fsanitize=address,undefined',
                                    str(path), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

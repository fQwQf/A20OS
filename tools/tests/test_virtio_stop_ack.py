"""Production block/net removal must retain DMA ownership until reset ACK."""
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
#define VIRTIO_MMIO_STATUS 0x70U
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x60U
#define VIRTIO_MMIO_INTERRUPT_ACK 0x64U
#define mb() ((void)0)
typedef struct virtio_transport virtio_transport_t;
struct virtio_transport {
 uint32_t (*read32)(virtio_transport_t *,uint32_t);
 void (*write32)(virtio_transport_t *,uint32_t,uint32_t);
 void (*msix_teardown)(virtio_transport_t *);
 int msix_vectors,msix_base,irq;
};
typedef struct {void *drv_priv;} device_t;
typedef struct {virtio_transport_t vt;device_t *device;int valid,transport_live,published,irq_registered,irq;void *ring_mem;} virtio_net_inst_t;
typedef struct {struct {int valid;} blk;virtio_transport_t vt;int in_flight,irq_registered,transport_live,ever_published,closing,lock;uint32_t active_calls,consumer_refs;device_t *device;void *queue_dma_mem;} virtio_blk_inst_t;
static struct {unsigned reads,resets,acks,frees,teardowns,ring_frees,stack_calls,confirmed;int stuck;} hw;
static uint64_t ticks;
static int g_instance_lock;
static uint64_t spin_lock_irqsave(int *lock){assert(!*lock);*lock=1;return 0;}
static void spin_unlock_irqrestore(int *lock,uint64_t flags){(void)flags;assert(*lock);*lock=0;}
static uint64_t clock_ticks_per_sec(void){return 1000;}
static uint64_t clock_get_ticks(void){return ticks+=10;}
static void arch_cpu_relax(void){}
static void *proc_current(void){return NULL;}
static void proc_yield(void){}
static uint32_t read32(virtio_transport_t *t,uint32_t off){
 (void)t;if(off==VIRTIO_MMIO_INTERRUPT_STATUS)return 1;
 assert(off==VIRTIO_MMIO_STATUS);hw.reads++;
 if(!hw.stuck&&hw.reads>=4){hw.confirmed=1;return 0;}return 7;
}
static void write32(virtio_transport_t *t,uint32_t off,uint32_t value){
 (void)t;if(off==VIRTIO_MMIO_INTERRUPT_ACK){assert(value==1);hw.acks++;return;}
 assert(off==VIRTIO_MMIO_STATUS&&!value);hw.resets++;hw.reads=hw.confirmed=0;
}
static void free_irq(uint32_t irq,void *owner){(void)irq;assert(owner&&hw.confirmed);hw.frees++;}
static void teardown(virtio_transport_t *t){assert(hw.confirmed&&hw.frees==(unsigned)t->msix_vectors);hw.teardowns++;}
static void virtio_net_free_ring(virtio_net_inst_t *net){assert(hw.confirmed&&!net->irq_registered&&net->ring_mem);net->ring_mem=NULL;hw.ring_frees++;}
static void virtio_blk_free_dma(virtio_blk_inst_t *inst){assert(hw.confirmed&&!inst->irq_registered&&!inst->active_calls);inst->queue_dma_mem=NULL;}
static void net_stack_device_interrupt(device_t *d){assert(d);hw.stack_calls++;}
'''
MAIN = r'''
int main(void){
 for(unsigned msix=0;msix<2;msix++){
  virtio_transport_t vt={.read32=read32,.write32=write32,.msix_teardown=teardown,
   .msix_vectors=msix?2:0,.msix_base=32,.irq=7};
  hw=(__typeof__(hw)){.stuck=1};
  virtio_net_inst_t net={.vt=vt,.valid=1,.transport_live=1,.published=1,.irq_registered=1,.irq=7,.ring_mem=&net};
  device_t dev={.drv_priv=&net};net.device=&dev;
  assert(virtio_net_driver_remove(&dev)==-ETIMEDOUT);
  assert(dev.drv_priv==&net&&!net.valid&&net.irq_registered&&net.ring_mem==&net);
  assert(!hw.frees&&!hw.teardowns&&!hw.ring_frees&&hw.resets==1);
  assert(!virtio_net_irq_handler(7,&net)&&hw.acks==1&&!hw.stack_calls);
  hw.stuck=0;
  assert(!virtio_net_driver_remove(&dev)&&!dev.drv_priv&&!net.irq_registered&&!net.ring_mem);
  assert(hw.resets==2&&hw.reads==4&&hw.ring_frees==1&&hw.frees==(msix?2U:1U));
  assert(!virtio_net_driver_remove(&dev)&&hw.resets==2);

  hw=(__typeof__(hw)){.stuck=1};
  virtio_blk_inst_t blk={.vt=vt,.blk={.valid=1},.irq_registered=1,.transport_live=1,.ever_published=1,
   .in_flight=3,.queue_dma_mem=&blk};dev.drv_priv=&blk;
  blk.active_calls=1;
  assert(virtio_blk_stop(&blk)==-EBUSY&&blk.closing&&blk.blk.valid&&blk.irq_registered);
  assert(!hw.resets&&!hw.frees&&dev.drv_priv==&blk);
  blk.active_calls=0;
  assert(virtio_blk_driver_remove(&dev)==-ETIMEDOUT);
  assert(dev.drv_priv==&blk&&!blk.blk.valid&&blk.irq_registered&&blk.in_flight==3);
  assert(blk.queue_dma_mem==&blk&&!hw.frees&&!hw.teardowns&&hw.resets==1);
  hw.stuck=0;
  assert(!virtio_blk_driver_remove(&dev)&&!dev.drv_priv&&!blk.irq_registered&&!blk.in_flight&&!blk.queue_dma_mem);
  assert(hw.resets==2&&hw.reads==4&&hw.frees==(msix?2U:1U));
  assert(!virtio_blk_driver_remove(&dev)&&hw.resets==2);
 }
 return 0;
}
'''


WRAPPER_MODEL = r'''
#define VIRTIO_MAX_DEVS 1
#define VIRTIO_BLK_MAX_TRANSFER_SECTORS 2
#define VIRTIO_BLK_SECTOR_SIZE 512
static virtio_blk_inst_t g_insts[1];
static unsigned io_calls,admitted;
static int virtio_blk_rw(int idx,uint64_t lba,void *buf,size_t count,int write,int flush){
 (void)lba;(void)buf;(void)count;(void)write;(void)flush;assert(!idx);
 virtio_blk_inst_t *inst=&g_insts[idx];assert(inst->active_calls==1);io_calls++;
 if(inst->closing)return -ESHUTDOWN;
 admitted++;assert(virtio_blk_stop(inst)==-EBUSY&&inst->blk.valid&&!hw.resets);
 return 0;
}
'''
WRAPPER_MAIN = r'''
static void check_call_wrappers(void){
 char data[3*512];
 for(unsigned writing=0;writing<2;writing++){
  hw=(__typeof__(hw)){0};io_calls=admitted=0;
  g_insts[0]=(virtio_blk_inst_t){.vt={.read32=read32,.write32=write32},
   .blk={.valid=1},.ever_published=1,.transport_live=1};
  int rc=writing?virtio_blk_write(0,0,data,3):virtio_blk_read(0,0,data,3);
  assert(rc==-ESHUTDOWN&&io_calls==2&&admitted==1&&!g_insts[0].active_calls&&!g_insts[0].consumer_refs);
  assert(!virtio_blk_stop(&g_insts[0])&&!g_insts[0].blk.valid&&hw.resets==1);
 }
 hw=(__typeof__(hw)){0};io_calls=admitted=0;
 g_insts[0]=(virtio_blk_inst_t){.vt={.read32=read32,.write32=write32},
  .blk={.valid=1},.ever_published=1,.transport_live=1};
 assert(!virtio_blk_flush(0)&&io_calls==1&&admitted==1&&!g_insts[0].active_calls&&!g_insts[0].consumer_refs);
 assert(!virtio_blk_stop(&g_insts[0])&&hw.resets==1);
}
'''


class VirtioStopAck(unittest.TestCase):
    def test_delayed_ack_timeout_and_retry_retain_hardware_ownership(self):
        net = (ROOT / 'kernel/drivers/net/virtio_net.c').read_text()
        code = function((ROOT / 'kernel/drivers/bus/virtio_transport.c').read_text(), 'virtio_transport_reset')
        # Skip the IRQ forward declaration; compile its production definition.
        code += function(net[net.index('static int virtio_net_irq_handler(int irq, void *priv) {'):],
                         'virtio_net_irq_handler')
        code += function(net, 'virtio_net_driver_remove')
        block = (ROOT / 'kernel/drivers/block/virtio_blk.c').read_text()
        code += function(block[block.index('static int virtio_blk_stop(void *context)\n{'):], 'virtio_blk_stop')
        code += function(block, 'virtio_blk_driver_remove')
        code += WRAPPER_MODEL
        code += function(block, 'virtio_blk_get_instance')
        code += function(block, 'virtio_blk_put_instance')
        code += '\n'.join(function(block, name) for name in (
            'virtio_blk_call_begin', 'virtio_blk_call_end', 'virtio_blk_transfer',
            'virtio_blk_read', 'virtio_blk_write', 'virtio_blk_flush'))
        code += WRAPPER_MAIN
        with tempfile.TemporaryDirectory(prefix='a20-virtio-stop-') as directory:
            source = Path(directory) / 'stop.c'
            binary = source.with_suffix('')
            source.write_text(MODEL + code + MAIN.replace("int main(void){", "int main(void){check_call_wrappers();", 1))
            built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=undefined', str(source), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

"""Block failed-probe DMA survives until reset ACK and cannot be overwritten."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
from tools.tests.test_virtio_net_probe_cleanup import MODEL as TRANSPORT_MODEL

ROOT = Path(__file__).resolve().parents[2]
TYPES = r'''
typedef int wait_queue_t;
typedef struct {int valid,legacy;uint64_t capacity;} virtio_blk_t;
typedef struct {uint64_t unused[2];} virtio_blk_req_hdr_t;
typedef struct block_dev {
 uint64_t capacity;uint32_t sector_size;void *priv;
 int (*read_sector)(struct block_dev *,uint64_t,void *,size_t);
 int (*write_sector)(struct block_dev *,uint64_t,const void *,size_t);
} block_dev_t;
#define VIRTIO_BLK_MAX_REQ_SLOTS 21
#define VIRTIO_BLK_BOUNCE_BYTES (64U*1024U)
#define VIRTIO_BLK_QUEUE_DMA_BYTES (4096U*3U)
#define VIRTIO_BLK_REQUEST_DMA_BYTES 4096U
#define VIRTIO_BLK_QUEUES 1
#define VIRTIO_BLK_SECTOR_SIZE 512U
'''
IO = r'''
static uint32_t readl(const volatile void *address){
 uintptr_t off=(uintptr_t)address-(uintptr_t)&hw;
 if(off==VIRTIO_MMIO_MAGIC)return 0x74726976;
 if(off==VIRTIO_MMIO_VERSION)return 2;
 if(off==VIRTIO_MMIO_DEVICE_ID)return 2;
 if(off==VIRTIO_MMIO_STATUS){if(!hw.status)hw.confirmed=1;return hw.status;}
 return 0;
}
static void writel(uint32_t value,volatile void *address){
 uintptr_t off=(uintptr_t)address-(uintptr_t)&hw;
 if(off==VIRTIO_MMIO_STATUS&&!value){hw.resets++;hw.confirmed=0;if(!hw.stuck)hw.status=0;}
}
static void dma_free_coherent_aligned(void *p,size_t size,uint64_t dma){
 (void)size;(void)dma;assert(p&&hw.confirmed);free(p);hw.freed++;
}
static int virtio_blk_alloc_dma(virtio_blk_inst_t *inst,device_t *dev){
 assert(dev->managed&&inst->device==dev);
 inst->queue_dma_mem=malloc(16);inst->request_dma_mem=malloc(16);hw.allocated+=2;
 for(unsigned i=0;i<VIRTIO_BLK_MAX_REQ_SLOTS;i++){inst->req[i].dma_buf=malloc(16);hw.allocated++;}
 return 0;
}
static int virtio_blk_device_init_locked(virtio_blk_inst_t *inst){
 assert(inst->queue_dma_mem&&inst->request_dma_mem&&inst->lock);
 /* Model failure after queue publication, not failure before allocations. */
 hw.queues++;hw.confirmed=0;hw.status=7;inst->blk.valid=1;inst->blk.capacity=64;
 return hw.fail_tx?-EIO:0;
}
static int virtio_blk_irq_handler(int irq,void *priv){(void)irq;(void)priv;return 0;}
'''
MAIN = r'''
int main(void){
 device_t failed[VIRTIO_MAX_DEVS];memset(failed,0,sizeof(failed));hw.fail_tx=1;
 for(unsigned i=0;i<VIRTIO_MAX_DEVS;i++){
  failed[i].name="failed";
  assert(virtio_blk_driver_probe(&failed[i])==-EIO&&failed[i].drv_priv);
  virtio_blk_inst_t *inst=failed[i].drv_priv;
  assert(inst->device==&failed[i]&&inst->queue_dma_mem&&!inst->ever_published);
  assert(!virtio_blk_ready(inst->slot)&&!hw.freed);
 }
 virtio_blk_inst_t *first=failed[0].drv_priv,*peer=failed[1].drv_priv;
 void *peer_dma=peer->queue_dma_mem;device_t extra={.name="extra"};
 assert(virtio_blk_driver_probe(&extra)==-ENOSPC&&!extra.drv_priv);
 hw.stuck=1;
 assert(virtio_blk_driver_remove(&failed[0])==-ETIMEDOUT&&failed[0].drv_priv==first);
 assert(first->device==&failed[0]&&first->queue_dma_mem&&!hw.freed);
 assert(virtio_blk_driver_probe(&extra)==-ENOSPC&&peer->queue_dma_mem==peer_dma);
 hw.stuck=0;assert(!virtio_blk_driver_remove(&failed[0])&&!failed[0].drv_priv&&!first->device);
 for(unsigned round=0;round<VIRTIO_MAX_DEVS+2;round++){
  assert(virtio_blk_driver_probe(&extra)==-EIO&&extra.drv_priv==first);
  assert(peer->queue_dma_mem==peer_dma&&!virtio_blk_ready(first->slot));
  assert(!virtio_blk_driver_remove(&extra)&&!extra.drv_priv&&!first->device);
 }
 for(unsigned i=1;i<VIRTIO_MAX_DEVS;i++)assert(!virtio_blk_driver_remove(&failed[i]));
 assert(hw.allocated==hw.freed);
 device_t early={.name="early",.bus=&pci_bus};unsigned resets=hw.resets;
 assert(virtio_blk_driver_probe(&early)<0&&early.drv_priv);
 assert(!virtio_blk_driver_remove(&early)&&!early.drv_priv&&hw.resets==resets);
 /* Published block_dev metadata must not be retargeted by slot reuse. */
 hw.fail_tx=0;device_t published={.name="published"};
 assert(!virtio_blk_driver_probe(&published));virtio_blk_inst_t *old=published.drv_priv;
 block_dev_t *borrowed=&old->blk_dev;
 assert(virtio_blk_ready(old->slot)&&!virtio_blk_driver_remove(&published));
 assert(borrowed->priv==old&&old->ever_published&&!virtio_blk_ready(old->slot));
 hw.fail_tx=1;
 assert(virtio_blk_driver_probe(&extra)==-EIO&&extra.drv_priv!=old);
 assert(borrowed->priv==old&&!old->queue_dma_mem&&old->closing);
 assert(!virtio_blk_driver_remove(&extra));
 return 0;
}
'''


class VirtioBlkProbeCleanup(unittest.TestCase):
    def test_failed_probe_retains_dma_until_ack_and_preserves_borrowed_metadata(self):
        source = (ROOT / 'kernel/drivers/block/virtio_blk.c').read_text()
        layout = source[source.index('typedef struct {'):source.index('static void virtio_blk_fail_queue_locked(')]
        header = (ROOT / 'kernel/include/drivers/block/virtio_blk.h').read_text()
        constants = '\n'.join(line for line in header.splitlines()
                              if line.startswith('#define VIRTIO_MMIO_') or line.startswith('#define VIRTIO_MAX_DEVS'))
        reset = function((ROOT / 'kernel/drivers/bus/virtio_transport.c').read_text(), 'virtio_transport_reset')
        free_dma = function(source, 'virtio_blk_free_dma')
        names = ('virtio_blk_init_instance', 'virtio_blk_ready', 'virtio_blk_mmio_read32',
                 'virtio_blk_mmio_write32', 'virtio_blk_driver_probe', 'virtio_blk_driver_remove')
        stop = function(source[source.index('static int virtio_blk_stop(void *context)\n{'):], 'virtio_blk_stop')
        code = stop + '\n' + '\n'.join(function(source, name) for name in names)
        model = TRANSPORT_MODEL.replace('assert(d->managed&&type==1)', 'assert(d->managed&&type==2)')
        model += '\nstatic int device_add_execution_action(device_t *d,int (*stop)(void *),void (*release)(void *),void *context){assert(d->managed&&d->drv_priv==context&&stop&&!release);return 0;}\n'
        model = model.replace('int irq,msix_vectors,msix_base,shared_irq;', 'int irq,msix_vectors,msix_base,shared_irq,legacy;')
        with tempfile.TemporaryDirectory(prefix='a20-blk-probe-') as directory:
            path = Path(directory) / 'probe.c'
            binary = path.with_suffix('')
            path.write_text(model + constants + '\n' + TYPES + layout + IO + free_dma + reset + code + MAIN)
            built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-variable', '-Wno-unused-function',
                                    '-fsanitize=address,undefined', str(path), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

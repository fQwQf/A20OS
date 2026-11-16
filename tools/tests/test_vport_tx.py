"""Production virtio-console TX against a device that retains DMA ownership."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VportTransmitTest(unittest.TestCase):
    def test_backpressure_timeout_interruption_wrap_and_concurrent_writers(self):
        text = (ROOT/'kernel/drivers/char/virtio_console.c').read_text()
        constants = text[text.index('#define VPORT_Q_RX'):text.index('typedef struct {')]
        structs = text[text.index('typedef struct {'):text.index('/* One port, one instance')]
        helpers = text[text.index('static int vport_call_begin('):text.index('/* ------------------------------------------------------------------ */', text.index('static int vport_call_begin('))]
        functions = text[text.index('/* Called with the instance lock and tx_lock held.'):text.index('static int vport_class_poll(')]
        prefix = r'''
#include "drivers/char/virtio_console_config.h"
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#define ALIGNED(n) __attribute__((aligned(n)))
#define EINVAL 22
#define EIO 5
#define ENODEV 19
#define ETIMEDOUT 110
#define ERESTARTSYS 512
#define VPORT_MMIO_QUEUE_NOTIFY 0x050u
#define MS_TO_TICKS(x) (x)
typedef pthread_mutex_t spinlock_t;
typedef pthread_mutex_t mutex_t;
static uint64_t spin_lock_irqsave(spinlock_t *p) {pthread_mutex_lock(p);return 0;}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f) {(void)f;pthread_mutex_unlock(p);}
static void mutex_lock(mutex_t *p) {pthread_mutex_lock(p);}
static void mutex_unlock(mutex_t *p) {pthread_mutex_unlock(p);}
typedef struct virtio_transport {
 void (*write32)(struct virtio_transport *,uint32_t,uint32_t);
} virtio_transport_t;
typedef struct device {void *drv_priv;} device_t;
static uint64_t ticks;
static int interrupted;
static uint64_t clock_get_ticks(void) {return ++ticks*1000;}
static void *proc_current(void) {return NULL;}
static int signal_task_has_unblocked(void *p) {(void)p;return interrupted;}
static void proc_yield(void) {}
#define dma_sync_for_cpu(p,n) ((void)(p),(void)(n))
#define dma_sync_for_device(p,n) ((void)(p),(void)(n))
#define rmb() __sync_synchronize()
#define wmb() __sync_synchronize()
#define mb() __sync_synchronize()
'''
        fixture = prefix + constants + '\n#define TEST_BYTES ((VPORT_BUF_SIZE < 512) ? VPORT_BUF_SIZE : 512)\n' + structs + helpers + functions + r'''
static vport_inst_t port;
static device_t dev={&port};
static unsigned notifications, consumed;
static uint16_t device_avail;
static int immediate;
static unsigned char owned[VPORT_TX_SLOTS],snapshot[VPORT_TX_SLOTS][VPORT_BUF_SIZE];
static void complete(unsigned slot) {
 assert(slot<VPORT_TX_SLOTS&&owned[slot]);
 assert(!memcmp(snapshot[slot],port.tx_mem->buf[slot],port.tx_mem->desc[slot].len));
 owned[slot]=0;
 unsigned idx=port.tx_mem->used.idx;
 port.tx_mem->used.ring[idx%VPORT_TX_SLOTS].id=slot;
 port.tx_mem->used.idx=(uint16_t)(idx+1);consumed++;
}
static void notify(virtio_transport_t *vt,uint32_t reg,uint32_t queue) {
 (void)vt;assert(reg==VPORT_MMIO_QUEUE_NOTIFY&&queue==VPORT_Q_TX);
 while(device_avail!=port.tx_mem->avail.idx) {
  unsigned slot=port.tx_mem->avail.ring[device_avail%VPORT_TX_SLOTS];
  assert(slot<VPORT_TX_SLOTS&&!owned[slot]);owned[slot]=1;
  memcpy(snapshot[slot],port.tx_mem->buf[slot],port.tx_mem->desc[slot].len);
  assert(port.tx_mem->desc[slot].addr==port.tx_dma+offsetof(vport_tx_mem_t,buf)+(uint64_t)slot*VPORT_BUF_SIZE);
  device_avail++;notifications++;if(immediate)complete(slot);
 }
}
static void fill(void) {
 unsigned char data[TEST_BYTES];
 for(unsigned i=0;i<VPORT_TX_SLOTS;i++) {memset(data,i+1,sizeof(data));assert(vport_class_write(&dev,data,sizeof(data))==sizeof(data));}
 assert(port.tx_busy==(1U<<VPORT_TX_SLOTS)-1);
}
static void *writer(void *arg) {
 unsigned char data[64];memset(data,(int)(uintptr_t)arg,sizeof(data));
 for(unsigned i=0;i<1000;i++)assert(vport_class_write(&dev,data,sizeof(data))==sizeof(data));
 return NULL;
}
int main(void) {
 memset(&port,0,sizeof(port));pthread_mutex_init(&port.lock,NULL);pthread_mutex_init(&port.tx_lock,NULL);
 port.tx_mem=calloc(1,sizeof(*port.tx_mem));assert(port.tx_mem);port.tx_dma=0x100000;
 port.vt.write32=notify;port.tx_queue=VPORT_Q_TX;port.valid=1;port.console_output=1;
 fill();unsigned char data[TEST_BYTES];memset(data,42,sizeof(data));unsigned before=notifications;
 assert(vport_class_write(&dev,data,sizeof(data))==-ETIMEDOUT);assert(notifications==before);
 for(unsigned i=0;i<VPORT_TX_SLOTS;i++)assert(!memcmp(snapshot[i],port.tx_mem->buf[i],sizeof(data)));
 interrupted=1;assert(vport_class_write(&dev,data,sizeof(data))==-ERESTARTSYS);interrupted=0;
 assert(notifications==before&&port.tx_busy==(1U<<VPORT_TX_SLOTS)-1);
 complete(VPORT_TX_SLOTS-1);assert(vport_class_write(&dev,data,sizeof(data))==sizeof(data));assert(owned[VPORT_TX_SLOTS-1]);
 for(unsigned i=0;i<VPORT_TX_SLOTS;i++)complete(i);
 assert(!vport_tx_wait(&port,1,0));assert(!port.tx_busy);
 /* Cross the 16-bit avail/used-index rollover, validating every frame. */
 immediate=1;
 for(unsigned i=0;i<70000;i++)assert(vport_class_write(&dev,data,sizeof(data))==sizeof(data));
 assert(!vport_tx_wait(&port,1,0));assert(!port.tx_busy);
 /* Suppressed kicks still publish a frame for an already-running device. */
 unsigned previous=notifications;port.tx_mem->used.flags=1;
 assert(vport_class_write(&dev,data,sizeof(data))==sizeof(data));assert(notifications==previous);
 port.tx_mem->used.flags=0;notify(&port.vt,VPORT_MMIO_QUEUE_NOTIFY,VPORT_Q_TX);
 assert(notifications==previous+1);assert(!vport_tx_wait(&port,1,0));
 /* Raw writes retain their completion contract under simultaneous callers. */
 port.console_output=0;pthread_t threads[4];
 for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,writer,(void *)(uintptr_t)(i+1)));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 assert(!port.tx_busy&&notifications==consumed);
 /* A device-provided id must never escape the descriptor allocation. */
 immediate=0;port.console_output=1;assert(vport_class_write(&dev,data,sizeof(data))==sizeof(data));
 unsigned idx=port.tx_mem->used.idx;port.tx_mem->used.ring[idx%VPORT_TX_SLOTS].id=999;
 port.tx_mem->used.idx=(uint16_t)(idx+1);
 assert(vport_class_write(&dev,data,sizeof(data))==-EIO);
 free(port.tx_mem);return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'tx.c';source.write_text(fixture);exe=Path(tmp)/'tx'
            for name, config in [
                ('throughput', []),
                ('compact', ['-DCONFIG_VIRTIO_CONSOLE_RX_SLOTS=2',
                             '-DCONFIG_VIRTIO_CONSOLE_TX_SLOTS=2',
                             '-DCONFIG_VIRTIO_CONSOLE_BUFFER_SIZE=512',
                             '-DCONFIG_VIRTIO_CONSOLE_RING_SIZE=512']),
                ('minimum', ['-DCONFIG_VIRTIO_CONSOLE_RX_SLOTS=1',
                             '-DCONFIG_VIRTIO_CONSOLE_TX_SLOTS=1',
                             '-DCONFIG_VIRTIO_CONSOLE_BUFFER_SIZE=64',
                             '-DCONFIG_VIRTIO_CONSOLE_RING_SIZE=128']),
            ]:
                with self.subTest(profile=name):
                    subprocess.run(['cc','-std=gnu99','-O2','-Wall','-Wextra','-Werror','-pthread',
                                    '-fsanitize=address,undefined',f'-I{ROOT}/kernel/include',
                                    *config,str(source),'-o',str(exe)],check=True)
                    subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    unittest.main()

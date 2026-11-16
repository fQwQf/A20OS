#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/block/virtio_blk.h"
#include "drivers/char/virtio_console.h"
#include "drivers/char/virtio_console_config.h"
#define ERESTARTSYS 512
#include "drivers/bus/virtio_transport.h"
#define ALIGNED(n) __attribute__((aligned(n)))
#define MS_TO_TICKS(n) (n)
#define IRQF_SHARED 1U
#define RES_MMIO 1
#define RES_IRQ 2
#define mb() ((void)0)
#define wmb() ((void)0)
#define rmb() ((void)0)
static void host_log(const char *fmt, ...) { (void)fmt; }
#define kinfo host_log
typedef struct { int held; } spinlock_t;
static void spin_init(spinlock_t *l) { l->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *l) { assert(!l->held); l->held = 1; return 0; }
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { assert(!f && l->held); l->held = 0; }
static int spin_trylock_irqsave(spinlock_t *l, uint64_t *f) {
    if (l->held) return 0;
    l->held = 1; *f = 0; return 1;
}
static void *proc_current(void) { return (void *)1; }
static void arch_cpu_relax(void) {}
static uint64_t ticks;
static uint64_t clock_get_ticks(void) { return ++ticks; }
static uint64_t clock_ticks_per_sec(void) { return 1000; }
static void proc_yield(void);
typedef struct { int unused; } bus_type_t;
static bus_type_t pci_bus;
typedef struct { uint64_t start, end; } resource_t;
struct hardware;
typedef struct device {
    bus_type_t *bus;
    void *drv_priv;
    struct hardware *hw;
    int managed;
    int (*stop)(void *);
    void *context;
    resource_t bar, irq;
} device_t;
struct queue { uint32_t regs[0x110 / 4]; uint16_t consumed; };
struct hardware {
    uint32_t regs[0x110 / 4], selected;
    struct queue q[6];
    int dead, stuck_reset, confirmed, fail_action, fail_transport, legacy;
    int alloc_calls, fail_alloc, fail_irq, no_version, no_multi, reject_features, bad_queue, fail_name;
    int stall_tx, stall_control, stop_notify, stop_wait, stop_echo, busy_stops;
    int irq_owned, irq_frees, allocs, frees, resets, notifications, echo_bytes;
    void *dma[4];
};
static struct hardware hardware[2];
static uint32_t readl(const volatile void *);
static void writel(uint32_t, volatile void *);
static void *dma_alloc_coherent_aligned(device_t *, size_t, size_t, uint64_t *);
static void dma_free_coherent_aligned(void *, size_t, uint64_t);
static void dma_sync_for_device(void *p, size_t n) { assert(p && n); }
static void dma_sync_for_cpu(void *p, size_t n) { assert(p && n); }
typedef struct { int held; } mutex_t;
static void mutex_init(mutex_t *m) { m->held = 0; }
static void mutex_lock(mutex_t *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(mutex_t *m) { assert(m->held); m->held = 0; }
static const char *options;
static const char *bootargs_get(void) { return options; }
static int signal_task_has_unblocked(void *p) { (void)p; return 0; }
static void uart_receive_char(char);
static int copy_from_user(void *d, const void *s, size_t n) { memcpy(d,s,n); return 0; }
static int copy_to_user(void *d, const void *s, size_t n) { memcpy(d,s,n); return 0; }
static int request_irq(uint32_t, int (*)(int, void *), unsigned long, void *);
static void free_irq(uint32_t, void *);
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*s)(void *), void (*r)(void *), void *p) {
    assert(d->managed && d->drv_priv == p && !r);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = s; d->context = p; return 0;
}
static int drv_device_set_devfs_name(device_t *d, const char *n) { assert(!strcmp(n, "vport0")); return d->hw->fail_name ? -ENOMEM : 0; }
static int pci_virtio_transport_init(device_t *, uint32_t, virtio_transport_t *);
/* PRODUCTION_RESET */
/* PRODUCTION_CONSOLE */
static struct hardware *current(void) { assert(g_vport.owner); return g_vport.owner->hw; }
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs)) return &hardware[i];
    abort();
}
static void *dma_alloc_coherent_aligned(device_t *d, size_t bytes, size_t align, uint64_t *dma) {
    struct hardware *h = d->hw;
    assert(d->managed && d->stop && align == PAGE_SIZE);
    if (++h->alloc_calls == h->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p,align,bytes));
    for (unsigned i=0;i<4;i++) if (!h->dma[i]) { h->dma[i]=p; h->allocs++; *dma=(uintptr_t)p; return p; }
    abort();
}
static void dma_free_coherent_aligned(void *p, size_t n, uint64_t dma) {
    struct hardware *h=current(); assert(n && (uintptr_t)p==dma);
    assert(g_vport.closing && g_vport.hardware_stopped && !g_vport.active_calls);
    assert(!h->irq_owned && h->confirmed && !h->regs[VPORT_MMIO_STATUS/4]);
    for(unsigned i=0;i<4;i++) if(h->dma[i]==p) { h->dma[i]=NULL;h->frees++;free(p);return; }
    abort();
}
static void active_stop(int *hook) {
    struct hardware *h=current();*hook=0;
    assert(g_vport.active_calls && !g_vport.lock.held);
    assert(vport_stop(&g_vport)==-EBUSY && !h->frees && !g_vport.hardware_stopped);
    h->busy_stops++;
}
static void proc_yield(void) { if(g_vport.owner && current()->stop_wait) active_stop(&current()->stop_wait); }
static void uart_receive_char(char c) {
    struct hardware *h=current();assert(c=='r' && !g_vport.lock.held && g_vport.active_calls);
    h->echo_bytes++;if(h->stop_echo)active_stop(&h->stop_echo);
}
static int request_irq(uint32_t irq, int(*handler)(int,void*),unsigned long flags,void *p) {
    struct hardware *h=current();assert(irq==12 && flags==IRQF_SHARED && p==&g_vport && !g_vport.valid);
    if(h->fail_irq)return -EBUSY;
    h->irq_owned=1;h->regs[VPORT_MMIO_INTR_STATUS/4]=1;
    assert(!handler(12,p) && !h->regs[VPORT_MMIO_INTR_STATUS/4]);return 0;
}
static void free_irq(uint32_t irq,void *p) {
    struct hardware *h=current();assert(irq==12 && p==&g_vport && h->irq_owned);
    assert(g_vport.hardware_stopped && !g_vport.active_calls && !g_vport.lock.held && h->confirmed);
    h->irq_owned=0;h->irq_frees++;
}
static uint64_t address(struct queue *q,unsigned off) { return q->regs[off/4] | ((uint64_t)q->regs[(off+4)/4]<<32); }
static void control_event(struct hardware *h,unsigned event) {
    struct queue *q=&h->q[VPORT_Q_CTRL_RX];
    vport_desc_t *d=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DESC_LOW);
    vport_avail_t *a=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DRIVER_LOW);
    vport_used_t *u=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DEVICE_LOW);
    assert(q->consumed!=a->idx);unsigned id=a->ring[q->consumed++%VPORT_RX_SLOTS];
    uint8_t *packet=(void*)(uintptr_t)d[id].addr;memset(packet,0,8);packet[0]=1;packet[4]=event;packet[6]=1;
    u->ring[u->idx++%VPORT_RX_SLOTS]=(vport_used_elem_t){id,8};
}
static void notify(struct hardware *h,unsigned index) {
    assert(index<6 && !g_vport.lock.held);h->notifications++;
    struct queue *q=&h->q[index];
    if(index==g_vport.rx_queue) {
        if(h->stop_notify)active_stop(&h->stop_notify);
        return;
    }
    if(index==VPORT_Q_CTRL_RX)return;
    if((index==VPORT_Q_CTRL_TX && h->stall_control) || (index==g_vport.tx_queue && h->stall_tx))return;
    vport_desc_t *d=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DESC_LOW);
    vport_avail_t *a=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DRIVER_LOW);
    vport_used_t *u=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DEVICE_LOW);
    while(q->consumed!=a->idx) {
        unsigned slots=index==VPORT_Q_CTRL_TX ? 1 : VPORT_TX_SLOTS;
        unsigned id=a->ring[q->consumed++%slots];assert(id<slots && !d[id].flags);
        if(index==VPORT_Q_CTRL_TX) {
            uint8_t *packet=(void*)(uintptr_t)d[id].addr;assert(d[id].len==8 && packet[6]==1);
            if(packet[4]==0)control_event(h,1);
            if(packet[4]==3)control_event(h,6);
        } else {
            const uint8_t *data=(void*)(uintptr_t)d[id].addr;
            for(unsigned i=0;i<d[id].len;i++)assert(data[i]=='t');
        }
        u->ring[u->idx++%slots]=(vport_used_elem_t){id,d[id].len};
    }
    if(index==g_vport.tx_queue && h->stop_notify)active_stop(&h->stop_notify);
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h=owner(p);unsigned off=(uintptr_t)p-(uintptr_t)h->regs;
    if(h->dead)return UINT32_MAX;
    if(off==VPORT_MMIO_DEV_FEATURES) return h->regs[VPORT_MMIO_DEV_FEATURES_SEL/4] ? !h->no_version : (h->no_multi ? 0 : 2);
    if(off==VPORT_MMIO_QUEUE_NUM_MAX)return h->bad_queue ? 0 : 128;
    if(off==VPORT_MMIO_QUEUE_READY)return h->q[h->selected].regs[off/4];
    uint32_t val=h->regs[off/4];if(off==VPORT_MMIO_INTR_STATUS)h->regs[off/4]=0;return val;
}
static void writel(uint32_t val,volatile void *p) {
    struct hardware *h=owner(p);unsigned off=(uintptr_t)p-(uintptr_t)h->regs;
    if(off==VPORT_MMIO_QUEUE_SEL) {assert(val<6);h->selected=val;return;}
    if(off==VPORT_MMIO_QUEUE_NOTIFY) {notify(h,val);return;}
    if(off==VPORT_MMIO_INTR_ACK) {h->regs[VPORT_MMIO_INTR_STATUS/4]&=~val;return;}
    if(off==VPORT_MMIO_QUEUE_READY || (off>=VPORT_MMIO_QUEUE_DESC_LOW && off<=VPORT_MMIO_QUEUE_DEVICE_HIGH)) {
        h->q[h->selected].regs[off/4]=val;return;
    }
    if(off==VPORT_MMIO_STATUS) {
        assert(!g_vport.lock.held);
        if(!val) {
            h->resets++;
            if(h->stuck_reset) {h->regs[off/4]|=VPORT_STATUS_ACK;return;}
            h->confirmed=1;memset(h->q,0,sizeof(h->q));
        } else h->confirmed=0;
        if(h->reject_features)val&=~VPORT_STATUS_FEATURES_OK;
    }
    h->regs[off/4]=val;
}
static uint32_t pci_read(virtio_transport_t *t,uint32_t off) {return readl((uint8_t*)t->priv+off);}
static void pci_write(virtio_transport_t *t,uint32_t off,uint32_t val) {writel(val,(uint8_t*)t->priv+off);}
static int pci_virtio_transport_init(device_t *d,uint32_t id,virtio_transport_t *t) {
    assert(d->managed && d->stop && id==VPORT_DEVICE_ID);
    if(d->hw->fail_transport)return -ENODEV;
    *t=(virtio_transport_t){.read32=pci_read,.write32=pci_write,.priv=d->hw->regs,.irq=12,.shared_irq=1,.legacy=d->hw->legacy};return 0;
}
static device_t fresh(unsigned i,int bulk) {
    assert(!g_vport.owner);struct hardware *h=&hardware[i];
    assert(!h->irq_owned);for(unsigned n=0;n<4;n++)assert(!h->dma[n]);memset(h,0,sizeof(*h));
    options=bulk ? "a20.console=vport0" : "";
    return (device_t){.bus=&pci_bus,.hw=h};
}
/* The core ordering is modeled: stop must pass before class drain/remove. */
static int teardown(device_t *d) {
    if(d->stop) {int ret=d->stop(d->context);if(ret)return ret;}
    return vport_remove(d);
}
static void receive(device_t *d,int irq) {
    struct hardware *h=d->hw;struct queue *q=&h->q[g_vport.rx_queue];
    vport_avail_t *a=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DRIVER_LOW);
    vport_used_t *u=(void*)(uintptr_t)address(q,VPORT_MMIO_QUEUE_DEVICE_LOW);
    unsigned id=a->ring[q->consumed++%VPORT_RX_SLOTS];
    memset(g_vport.rx_mem->buf[id],'r',100);u->ring[u->idx++%VPORT_RX_SLOTS]=(vport_used_elem_t){id,100};
    if(irq) {h->regs[VPORT_MMIO_INTR_STATUS/4]=1;vport_irq_handler(12,&g_vport);}
}
static void normal(int bulk,int no_irq) {
    device_t d=fresh(0,bulk);struct hardware *h=d.hw;h->fail_irq=no_irq;
    assert(!vport_probe(&d));unsigned allocations=bulk ? 4 : 2;assert(h->allocs==(int)allocations);
    int32_t mirror=0;assert(!vport_class_ioctl(&d,VPORT_IOCTL_SET_CONSOLE,&mirror));
    assert(vport_class_ioctl(&d,VPORT_IOCTL_CONSOLE_READY,NULL)==(bulk ? 0 : -ENODEV));
    receive(&d,!no_irq);char bytes[100];assert(vport_class_read(&d,bytes,sizeof(bytes))==100);
    for(unsigned i=0;i<100;i++)assert(bytes[i]=='r');
    memset(bytes,'t',100);
    assert(vport_class_write(&d,bytes,100)==100 && !vport_class_ioctl(&d,VPORT_IOCTL_DRAIN,NULL));
    h->stuck_reset=1;void *rx=g_vport.rx_mem,*tx=g_vport.tx_mem;
    assert(teardown(&d)==-ETIMEDOUT && g_vport.rx_mem==rx && g_vport.tx_mem==tx && !h->frees);
    device_t peer={.hw=&hardware[1]};assert(vport_probe(&peer)==-EBUSY && !peer.drv_priv);
    assert(vport_class_write(&d,bytes,1)==-ENODEV && !vport_class_poll(&d,1));
    assert(vport_class_read(&d,bytes,1)==-ENODEV && vport_class_ioctl(&d,VPORT_IOCTL_DRAIN,NULL)==-ENODEV);
    h->dead=1;assert(teardown(&d)==-ENODEV && !h->frees);h->dead=0;h->stuck_reset=0;
    assert(!teardown(&d) && !g_vport.owner && !d.drv_priv && h->frees==(int)allocations && !teardown(&d));
}
static void race(unsigned hook) {
    device_t d=fresh(0,0);struct hardware *h=d.hw;assert(!vport_probe(&d));
    char data[100];memset(data,'t',100);
    if(hook==0) {h->stop_wait=1;h->stall_tx=1;assert(vport_class_write(&d,data,100)==-ENODEV);}
    if(hook==1) {h->stop_notify=1;assert(vport_class_write(&d,data,100)==-ENODEV);}
    if(hook==2) {h->stop_notify=1;receive(&d,1);}
    if(hook==3) {h->stop_echo=1;receive(&d,1);}
    assert(h->busy_stops==1 && !g_vport.active_calls && h->irq_owned && !h->frees);
    assert(!teardown(&d) && h->frees==2);
}
static void failure(unsigned kind) {
    device_t d=fresh(0,1);struct hardware *h=d.hw;
    switch(kind) {
    case 0:h->fail_action=1;break;
    case 1:h->fail_transport=1;break;
    case 2:h->legacy=1;break;
    case 3:h->stuck_reset=1;break;
    case 4:h->no_version=1;break;
    case 5:h->no_multi=1;break;
    case 6:h->reject_features=1;break;
    case 7:case 8:case 9:case 10:h->fail_alloc=(int)kind-6;break;
    case 11:h->bad_queue=1;break;
    case 12:h->fail_name=1;break;
    case 13:h->stall_control=1;break;
    default:abort();
    }
    assert(vport_probe(&d)<0 && g_vport.owner==&d && d.drv_priv==&g_vport);
    if(g_vport.reset_required) {
        h->stuck_reset=1;int irq=h->irq_owned;int allocs=h->allocs;
        assert(teardown(&d)==-ETIMEDOUT && h->irq_owned==irq && !h->frees && h->allocs==allocs);
        device_t peer={.hw=&hardware[1]};assert(vport_probe(&peer)==-EBUSY);h->stuck_reset=0;
    }
    assert(!teardown(&d) && !g_vport.owner && !d.drv_priv && h->allocs==h->frees && !h->irq_owned);
}
int main(void) {
    normal(0,0);normal(1,0);normal(0,1);
    for(unsigned i=0;i<4;i++)race(i);
    for(unsigned i=0;i<14;i++)failure(i);
    puts("virtio-console lifecycle: raw/bulk RX/TX, four active-call stops and 14 partial probe failures passed");
    return 0;
}

"""Exercise production firmware nodes, boot phases and driver graph on the host.
Only allocation, locking, class publication and MMIO hardware are replaced.
The descriptor parsers, enumeration, matching, lifetime and rollback are real.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    'core/defs.h': '#include "core/types.h"\n#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n',
    'core/lock.h': '''#pragma once
#include "core/types.h"
typedef struct { unsigned held; } spinlock_t;
#define SPINLOCK_INIT {0}
static inline void spin_init(spinlock_t *l) { l->held=0; }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) { l->held++; return 0; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; l->held--; }
''',
    'core/sync.h': '''#pragma once
#include "core/lock.h"
typedef struct { unsigned depth; } mutex_t;
extern int host_trylock_blocked;
#define MUTEX_INIT {0}
static inline void mutex_init(mutex_t *m) { m->depth=0; }
static inline void mutex_lock(mutex_t *m) { if(m->depth)__builtin_trap();m->depth=1; }
static inline int mutex_trylock(mutex_t *m) { if(m->depth||host_trylock_blocked)return 0; m->depth=1;return 1; }
static inline void mutex_unlock(mutex_t *m) { if(!m->depth)__builtin_trap();m->depth--; }
''',
    'core/cpu.h': '''#pragma once
#define CONFIG_NR_CPUS 2
extern unsigned host_cpu;
static inline unsigned cpu_current_id(void) { return host_cpu; }
''',
    'core/stdio.h': '#include <stdio.h>\n',
    'core/klog.h': '''#pragma once
void host_log(const char *, ...);
#define kinfo host_log
#define kdebug host_log
#define kwarn host_log
#define kerr host_log
''',
    'core/panic.h': 'void panic(const char *, ...) __attribute__((noreturn));\n',
    'mm/mm.h': '#include "mm/slab.h"\n',
    'drivers/core/driver_hwapi.h': '''#pragma once
#include "core/types.h"
static inline uint32_t readl(const volatile void *p) { return *(const volatile uint32_t *)p; }
''',
    'drivers/bus/virtio_mmio_hal.h': '''#pragma once
#include "core/types.h"
static inline size_t arch_virtio_mmio_slot_size(void) { return 0x200; }
''',
}
HARNESS = r'''
#include "core/types.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "firmware/static.h"
#include "firmware/device.h"
#include "firmware/irq.h"
#include "boot/context.h"
#include "core/errno.h"
unsigned host_cpu;
struct task_t *proc_current(void) { return NULL; }
int host_trylock_blocked;
static int fail_after=-1, maps, unmaps, cleanup, removed, released;
static uint32_t virtio_registers[0x200/4];
void *kmalloc(size_t n) { if(fail_after==0)return NULL;if(fail_after>0)fail_after--;return malloc(n); }
void *kcalloc(size_t n,size_t s) { void *p=kmalloc(n*s);if(p)memset(p,0,n*s);return p; }
void *krealloc(void *p,size_t n) { return realloc(p,n); }
void kfree(void *p) { free(p); }
void host_log(const char *fmt,...) { (void)fmt; }
void panic(const char *fmt,...) { (void)fmt;abort(); }
void *arch_device_map_mmio(uint64_t pa,size_t n,uint32_t f) {
    (void)f;
    if(pa==0x5000 && n==0x200) { maps++;return virtio_registers; }
    if(pa!=0x3000 || n!=0x100)return NULL;
    maps++; return (void *)0x10003000;
}
void arch_device_unmap_mmio(void *p,size_t n) {
 assert((p==(void *)0x10003000&&n==0x100)||(p==virtio_registers&&n==0x200));unmaps++;
}
int udriver_mmio_user_owned(uint64_t pa) { (void)pa;return 0; }
static class_device_t fake_class;
int class_device_publish(device_t *d) { d->class_dev=&fake_class;return 0; }
void class_device_mark_offline(device_t *d) { (void)d; }
void class_device_unpublish(device_t *d) { d->class_dev=NULL; }
class_device_t *class_device_ref_for_device(const device_t *d) { return d->class_dev; }
class_device_t *class_device_get_by_type(uint32_t type,unsigned index) { (void)type;(void)index;return NULL; }
int class_device_call_begin(class_device_t *d) { (void)d;return 0; }
void class_device_call_end(class_device_t *d) { (void)d; }
void class_device_put(class_device_t *d) { (void)d; }
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
static const char consumer_id[]="vendor,consumer", provider_id[]="vendor,clock", plic_id[]="riscv,plic0";
static const char unknown_id[]="vendor,unbound", disabled_id[]="vendor,disabled";
static const uint8_t big_value[]={0x12,0x34,0x56,0x78};
static const fw_static_property_t cprops[]={
 {"compatible",{consumer_id,sizeof(consumer_id),FW_ORDER_NATIVE}},
 {"test",{big_value,sizeof(big_value),FW_ORDER_BIG_ENDIAN}},
};
static const fw_static_property_t pprops[]={{"compatible",{provider_id,sizeof(provider_id),FW_ORDER_NATIVE}}};
static const fw_static_property_t iprops[]={{"compatible",{plic_id,sizeof(plic_id),FW_ORDER_NATIVE}}};
static const fw_static_property_t uprops[]={{"compatible",{unknown_id,sizeof(unknown_id),FW_ORDER_NATIVE}}};
static const fw_static_property_t dprops[]={{"compatible",{disabled_id,sizeof(disabled_id),FW_ORDER_NATIVE}}};
static const fw_resource_t memory={FW_RESOURCE_MEMORY,0x3000,0x100,0,"registers"};
static const uint32_t clock_arg[]={7}, irq_arg[]={5};
static const fw_static_reference_t clock_ref={"clocks",2,clock_arg,1}, irq_ref={"interrupts",3,irq_arg,1};
static fw_static_node_t nodes[]={
 {.name="root",.parent=SIZE_MAX},
 {.name="consumer",.parent=0,.properties=cprops,.property_count=2,.resources=&memory,.resource_count=1,
  .references=&clock_ref,.reference_count=1,.irqs=&irq_ref,.irq_count=1},
 {.name="clock",.parent=0,.properties=pprops,.property_count=1},
 {.name="plic",.parent=0,.properties=iprops,.property_count=1},
 {.name="unbound",.parent=0,.properties=uprops,.property_count=1},
 {.name="disabled",.parent=0,.properties=dprops,.property_count=1,.disabled=1},
};
static const fw_static_tree_t tree={nodes,6};
static void release_action(void *p) { assert(p==(void *)7);cleanup++; }
static int clock_probe(device_t *d) { (void)d; return 0; }
static int clock_remove(device_t *d) { (void)d;assert(removed==cleanup&&cleanup==unmaps);return 0; }
static int consumer_probe(device_t *d) {
 device_t *provider; fw_reference_t reference;
 int rc=device_dependency_get(d,"clocks","#clock-cells",0,&provider,&reference);
 if(rc)return rc;
 assert(reference.count==1 && reference.args[0]==7);
 device_put(provider);
 assert(device_get_resource(d,RES_MMIO,0)->start==0x3000);
 assert(platform_device_irq(d)==5);
 assert(device_map_mmio(d,0)==(void *)0x10003000);
 assert(device_add_resource_action(d,release_action,(void *)7)==0);
 return 0;
}
static int consumer_remove(device_t *d) { (void)d;removed++;return 0; }
static const firmware_device_id_t cids[]={{.id="vendor,consumer"},{0}};
static const firmware_device_id_t pids[]={{.id="vendor,clock"},{0}};
static driver_t consumer={.name="consumer",.bus=&platform_bus,.firmware_ids=cids,
 .probe=consumer_probe,.remove=consumer_remove};
static driver_t provider={.name="provider",.bus=&platform_bus,.firmware_ids=pids,
 .probe=clock_probe,.remove=clock_remove};
static void dynamic_release(device_t *d) { released++;free(d); }
static int generic_probe(device_t *d) { (void)d;return 0; }
static int direct_match(device_t *d) { return d->name && !strcmp(d->name,"dynamic"); }
static void progress(device_t *d) { assert(d->references>0); }
static driver_t dynamic_driver={.name="dynamic",.match=direct_match,.probe=generic_probe,
 .progress=progress,.class_type=DEV_CLASS_BLOCK};
static void malformed_static(void) {
 fw_static_node_t bad_nodes[]={{.name="root",.parent=SIZE_MAX},
  {.name="first",.parent=2},{.name="later",.parent=99}};
 fw_static_tree_t bad_tree={bad_nodes,3};fw_node_t root,node,foreign={.ops=(void *)1,.context=(void *)1};
 assert(fw_static_init(&bad_tree,&root)==-EINVAL);
 assert(fw_static_init(&tree,&root)==0);
 assert(fw_node_next_child(&root,&foreign,&node)==-EINVAL);
 assert(fw_static_get(&tree,1,&node)==0);
 assert(fw_node_parent(&node,&node)==0 && fw_node_equal(&node,&root));
}
static int custom_decode(const fw_reference_t *r,uint32_t *hw,uint32_t *flags) {
 *hw=r->args[0];*flags=4;return 0;
}
static int map_result=PLATFORM_IRQ_MAX_LINES;
static int bad_map(uint32_t hw,uint32_t flags) { (void)hw;(void)flags;return map_result; }
static void irq_sources(fw_node_t root) {
 fw_static_node_t other_nodes[]={{.name="other",.parent=SIZE_MAX},
  {.name="plic",.parent=0,.properties=iprops,.property_count=1}};
 fw_static_tree_t other_tree={other_nodes,2};fw_node_t other,custom;
 assert(fw_static_init(&other_tree,&other)==0);
 assert(firmware_irq_domains_init(&root)==0);
 assert(firmware_irq_domains_init(&other)==-EBUSY);
 fw_reference_t r;assert(fw_static_get(&tree,1,&custom)==0);
 assert(fw_node_irq(&custom,0,&r)==0);uint32_t irq,flags;
 assert(fw_irq_resolve(&r,&irq,&flags)==0 && irq==5);
 device_t *early=firmware_device_find(&custom);assert(early&&!early->drv);
 assert(!device_get_resource(early,RES_IRQ,0));
 assert(firmware_device_irq(early,0)==5);
 assert(firmware_device_irq(early,1)==-ENODEV);
 assert(!device_get_resource(early,RES_IRQ,0)&&!early->drv);
 device_put(early);
 resource_t direct={.type=RES_IRQ,.start=7,.end=7};
 device_t raw={.res=&direct,.res_count=1};
 assert(firmware_device_irq(&raw,0)==7);
 direct.end=8;assert(firmware_device_irq(&raw,0)==-EINVAL);
 direct.start=direct.end=PLATFORM_IRQ_MAX_LINES;
 assert(firmware_device_irq(&raw,0)==-ERANGE);
 raw.res_count=0;assert(firmware_device_irq(&raw,0)==-ENODEV);
 assert(firmware_device_irq(NULL,0)==-EINVAL);
}
static void custom_irq(void) {
 fw_node_t other;assert(fw_static_get(&tree,4,&other)==0);
 fw_reference_t r;uint32_t irq,flags;
 fw_irq_domain_t domain={.controller=other,.hardware_base=1000,.logical_base=100,.count=2,
  .decode=custom_decode,.map=bad_map};
 assert(fw_irq_domain_register(&domain)==0);
 r=(fw_reference_t){.node=other,.args={1000},.count=1};
 assert(fw_irq_resolve(&r,&irq,&flags)==-ERANGE);
 map_result=99;assert(fw_irq_resolve(&r,&irq,&flags)==-ERANGE);
 domain.map=NULL;assert(fw_irq_resolve(&r,&irq,&flags)==0 && irq==100);
 assert(fw_irq_domain_unregister(&domain)==0);
}
static void enumeration_rollback(void) {
 fw_resource_t invalid={FW_RESOURCE_MEMORY,0,0,0,NULL};
 fw_static_node_t tx_nodes[]={{.name="root",.parent=SIZE_MAX},
  {.name="first",.parent=0,.properties=uprops,.property_count=1},
  {.name="second",.parent=0,.properties=uprops,.property_count=1,.resources=&invalid,.resource_count=1}};
 fw_static_tree_t tx_tree={tx_nodes,3};fw_node_t root,first,second;
 assert(fw_static_init(&tx_tree,&root)==0);int before=driver_device_count();
 assert(firmware_device_enumerate(&root)==-EINVAL && driver_device_count()==before);
 assert(fw_static_get(&tx_tree,1,&first)==0 && !firmware_device_find(&first));
 tx_nodes[2].resource_count=0;
 fail_after=1;
 assert(firmware_device_enumerate(&root)==-ENOMEM && driver_device_count()==before);
 fail_after=-1;
 assert(firmware_device_enumerate(&root)==2);
 assert(fw_static_get(&tx_tree,2,&second)==0);
 device_t *a=firmware_device_find(&first),*b=firmware_device_find(&second);assert(a&&b);
 device_unregister(b);device_put(b);device_unregister(a);device_put(a);
 assert(driver_device_count()==before && !firmware_device_find(&first));
}
static void parent_lifetime(void) {
 int before=released;
 device_t *parent=calloc(1,sizeof(*parent)),*child=calloc(1,sizeof(*child));
 parent->name="parent";parent->release=dynamic_release;
 child->name="child";child->release=dynamic_release;child->parent=parent;
 assert(device_register(parent)==0 && device_register(child)==0);
 assert(device_add_dependency(parent,child)==-ELOOP);
 device_unregister(parent);device_put(parent);assert(released==before);
 assert(child->parent->references==1);
 device_unregister(child);device_put(child);assert(released==before+2);
}
static int defer_calls,poll_calls,fallback_calls;
static int defer_match(device_t *d) { return !strcmp(d->name,"forever-deferred"); }
static int defer_probe(device_t *d) { (void)d;defer_calls++;return -EPROBE_DEFER; }
static int fallback_probe(device_t *d) { (void)d;fallback_calls++;return 0; }
static void deferred_termination(void) {
 driver_t d={.name="deferred",.match=defer_match,.probe=defer_probe};
 driver_t fallback={.name="fallback",.match=defer_match,.probe=fallback_probe};
 device_t dev={.name="forever-deferred"};
 assert(driver_register(&d)==0 && driver_register(&fallback)==0 && device_register(&dev)==0);
 assert(defer_calls==1 && driver_retry_deferred()==0 && defer_calls==2 && !fallback_calls);
 assert(dev.probe_error==-EPROBE_DEFER && dev.state==DEV_STATE_DEFERRED);
 assert(driver_unregister(&d)==0 && driver_retry_deferred()==1 && fallback_calls==1);
 device_unregister(&dev);device_put(&dev);assert(driver_unregister(&fallback)==0);
}
static int poll_probe(device_t *d) { assert(platform_device_irq(d)==-ENODEV);poll_calls++;return 0; }
static void optional_irq(void) {
 const firmware_device_id_t ids[]={{.id="poll,device"},{0}};
 const char id[]="poll,device";
 fw_static_property_t props={"compatible",{id,sizeof(id),FW_ORDER_NATIVE}};
 fw_static_reference_t ir={"interrupts",2,irq_arg,1};
 fw_static_node_t ns[]={{.name="root",.parent=SIZE_MAX},
  {.name="poll",.parent=0,.properties=&props,.property_count=1,.irqs=&ir,.irq_count=1},
  {.name="unknown-irq-controller",.parent=0}};
 fw_static_tree_t t={ns,3};fw_node_t root,node;
 driver_t drv={.name="poll",.bus=&platform_bus,.firmware_ids=ids,.irq_optional=1,.probe=poll_probe};
 assert(fw_static_init(&t,&root)==0 && firmware_device_enumerate(&root)==1);
 assert(driver_register(&drv)==0 && poll_calls==1);
 assert(fw_static_get(&t,1,&node)==0);device_t *dev=firmware_device_find(&node);assert(dev&&dev->drv==&drv);
 ir.count=FW_REFERENCE_MAX_ARGS+1;assert(firmware_device_prepare(dev)==-EINVAL);ir.count=1;
 assert(driver_unregister(&drv)==0);device_unregister(dev);device_put(dev);
}
#include "drivers/bus/virtio_mmio_bus.c"
static device_t *transport_child;
static int transport_removes;
static int transport_probe(device_t *d) {
 assert(device_get_resource(d,RES_IRQ,0)->flags==IORESOURCE_IRQ_EDGE);
 transport_child=device_get(d);return 0;
}
static int transport_remove(device_t *d) { assert(d==transport_child);transport_removes++;return 0; }
static void transport_lifecycle(void) {
 const device_id_t ids[]={{.vendor=VENDOR_ANY,.device=2},{0}};
 driver_t child_driver={.name="virtio-child",.bus=get_virtio_mmio_bus(),.id_table=ids,
  .probe=transport_probe,.remove=transport_remove};
 const char id[]="virtio,mmio";
 const fw_static_property_t prop={"compatible",{id,sizeof(id),FW_ORDER_NATIVE}};
 const fw_static_node_t ns={.name="virtio",.parent=SIZE_MAX,.properties=&prop,.property_count=1};
 const fw_static_tree_t t={&ns,1};fw_node_t node;assert(fw_static_init(&t,&node)==0);
 resource_t resources[]={{.type=RES_MMIO,.start=0x5000,.end=0x51ff},
  {.type=RES_IRQ,.start=5,.end=5,.flags=IORESOURCE_IRQ_EDGE}};
 platform_device_t parent={.dev={.name="virtio",.firmware_node=node,.res=resources,.res_count=2}};
 virtio_registers[0]=0x74726976;virtio_registers[1]=2;virtio_registers[2]=2;
 assert(driver_register(&child_driver)==0 && driver_register(&firmware_transport)==0);
 assert(platform_device_register(&parent)==0 && parent.dev.drv==&firmware_transport);
 assert(transport_child && transport_child->parent==&parent.dev && parent.dev.references==3);
 assert(driver_unregister(&firmware_transport)==0 && transport_removes==1);
 assert(!transport_child->drv && parent.dev.references==3);
 device_put(transport_child);transport_child=NULL;assert(parent.dev.references==2);
 platform_device_unregister(&parent);device_put(&parent.dev);
 /* Child registration succeeds, then managed-action allocation fails. The
  * child must already be removed and all ownership pins remain balanced. */
 parent=(platform_device_t){.dev={.name="virtio",.firmware_node=node,.res=resources,.res_count=2}};
 assert(driver_register(&firmware_transport)==0);
 fail_after=1;assert(platform_device_register(&parent)==0);fail_after=-1;
 assert(!parent.dev.drv && parent.dev.probe_error==-ENOMEM && transport_removes==2);
 assert(transport_child && !transport_child->drv);
 device_put(transport_child);transport_child=NULL;assert(parent.dev.references==2);
 platform_device_unregister(&parent);device_put(&parent.dev);
 assert(driver_unregister(&firmware_transport)==0 && driver_unregister(&child_driver)==0);
}
int main(void) {
 malformed_static();
 custom_irq();
 fw_node_t root, node, disabled;
 assert(fw_static_init(&tree,&root)==0);
 assert(fw_static_get(&tree,1,&node)==0);
 uint32_t value=0;assert(fw_node_read_u32(&node,"test",&value)==0 && value==0x12345678);
 assert(fw_static_get(&tree,5,&disabled)==0 && fw_node_available(&disabled)==0);
 fw_resource_t bad={FW_RESOURCE_MEMORY,~0ULL,2,0,NULL};
 nodes[1].resources=&bad; assert(fw_node_resource(&node,0,&bad)==-EINVAL);
 nodes[1].resources=&memory;
 size_t oldparent=nodes[2].parent;nodes[2].parent=1;nodes[1].parent=2;
 assert(fw_static_init(&tree,&root)==-ELOOP);nodes[1].parent=0;nodes[2].parent=oldparent;
 assert(fw_static_init(&tree,&root)==0);
 boot_context_t handoff={.protocol=BOOT_PROTOCOL_STATIC};
 assert(boot_context_advance(BOOT_PHASE_HANDOFF,BOOT_PHASE_EARLY)==-EALREADY);
 assert(boot_context_publish(&handoff)==0 && boot_context_publish(&handoff)==-EALREADY);
 assert(boot_context_set_hardware_root(&root)==0);
 assert(boot_context_advance(BOOT_PHASE_HANDOFF,BOOT_PHASE_MEMORY)==-EINVAL);
 assert(boot_context_advance(BOOT_PHASE_HANDOFF,BOOT_PHASE_EARLY)==0);
 driver_core_init();
 nodes[4].resources=&bad;nodes[4].resource_count=1;
 assert(firmware_device_enumerate(&root)==-EINVAL && driver_device_count()==0);
 fw_reference_t early_irq;uint32_t early_line,early_flags;
 assert(fw_node_irq(&node,0,&early_irq)==0);
 assert(fw_irq_resolve(&early_irq,&early_line,&early_flags)==-EPROBE_DEFER);
 nodes[4].resources=NULL;nodes[4].resource_count=0;
 assert(firmware_device_enumerate(&root)==4);
 assert(firmware_device_enumerate(&root)==0);
 assert(driver_device_count()==4);
 irq_sources(root);enumeration_rollback();parent_lifetime();deferred_termination();optional_irq();
 device_t *c=firmware_device_find(&node);assert(c && !c->drv);
 assert(driver_register(&consumer)==0 && c->probe_error==-EPROBE_DEFER && !c->drv);
 fw_node_t waiting_node;assert(fw_static_get(&tree,2,&waiting_node)==0);
 device_t *waiting=firmware_device_find(&waiting_node);assert(waiting&&c->dependencies);
 device_unregister(waiting);assert(!c->dependencies&&c->probe_error==-EPROBE_DEFER);device_put(waiting);
 assert(firmware_device_enumerate(&root)==1);
 assert(driver_register(&provider)==0 && c->drv==&consumer);
 assert(maps==1 && unmaps==0 && cleanup==0);
 fw_node_t pnode;assert(fw_static_get(&tree,2,&pnode)==0);
 device_t *p=firmware_device_find(&pnode);assert(p);
 assert(device_add_dependency(p,c)==-ELOOP);
 assert(driver_unregister(&provider)==0 && !c->drv && !p->drv);
 assert(unmaps==1 && cleanup==1 && removed==1);
 assert(driver_register(&provider)==0 && c->drv==&consumer);
 assert(driver_unregister(&consumer)==0 && unmaps==2 && cleanup==2);
 device_put(c);device_put(p);
 assert(driver_register(&dynamic_driver)==0);
 released=0;
 device_t *dyn=calloc(1,sizeof(*dyn));dyn->name="dynamic";dyn->release=dynamic_release;
 assert(device_register(dyn)==0 && dyn->references==2);
 driver_progress_class(DEV_CLASS_BLOCK);assert(dyn->references==3);
 host_cpu=1;driver_progress_class(DEV_CLASS_BLOCK);assert(dyn->references==4);host_cpu=0;
 device_unregister(dyn);device_put(dyn);assert(released==0);
 host_trylock_blocked=1;dyn->drv=(driver_t *)1;
 driver_progress_class(DEV_CLASS_BLOCK); /* Stale module pointer must not be read. */
 dyn->drv=NULL;host_trylock_blocked=0;
 driver_progress_class(DEV_CLASS_BLOCK);assert(released==0);
 host_cpu=1;driver_progress_class(DEV_CLASS_BLOCK);assert(released==1);host_cpu=0;
 transport_lifecycle();
 assert(boot_context_advance(BOOT_PHASE_EARLY,BOOT_PHASE_MEMORY)==0);
 assert(boot_context_advance(BOOT_PHASE_MEMORY,BOOT_PHASE_DEVICES)==0);
 assert(boot_context_set_hardware_root(&root)==-EINVAL);
 assert(boot_context_advance(BOOT_PHASE_DEVICES,BOOT_PHASE_READY)==0);
 puts("firmware model: PASS");
 return 0;
}
'''

class FirmwareModelTest(unittest.TestCase):
    def test_production_graph(self):
        with tempfile.TemporaryDirectory(prefix='a20-firmware-model-') as temp:
            work=Path(temp)
            for name, content in STUBS.items():
                p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
            harness=work/'test.c';harness.write_text(HARNESS)
            executable=work/'test'
            sources=['kernel/firmware/node.c','kernel/firmware/static.c','kernel/firmware/irq.c',
                     'kernel/firmware/device.c','kernel/boot/context.c',
                     'kernel/drivers/core/driver_core.c','kernel/drivers/bus/platform_bus.c']
            subprocess.run(['cc','-std=gnu99','-g','-O1','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-DCONFIG_NR_CPUS=2',
                '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),
                str(harness),*[str(ROOT/s) for s in sources],'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=15,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

if __name__=='__main__': unittest.main()

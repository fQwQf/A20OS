"""Production class/device lifetime, block identity and network operation guards."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS

HARNESS = r'''
#include "core/types.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "fs/mount_setup.h"
#include "core/errno.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
unsigned host_cpu;
int host_trylock_blocked;
struct task_t *proc_current(void) { return NULL; }
static int fail_next,released,removed,uevents,sends,polls;
void *kmalloc(size_t n) { if(fail_next){fail_next=0;return NULL;}return malloc(n); }
void *kcalloc(size_t n,size_t z) { void*p=kmalloc(n*z);if(p)memset(p,0,n*z);return p; }
void *krealloc(void *p,size_t n) { return realloc(p,n); }
void kfree(void *p) { free(p); }
void panic(const char *fmt,...) { (void)fmt;abort(); }
void host_log(const char *fmt,...) { (void)fmt; }
int firmware_device_prepare(device_t *d) { (void)d;return 0; }
void *arch_device_map_mmio(uint64_t p,size_t n,uint32_t f) { (void)p;(void)n;(void)f;return NULL; }
void arch_device_unmap_mmio(void *p,size_t n) { (void)p;(void)n; }
void netlink_uevent_emit(const char *a,const char*s,const char*n,uint64_t d) {
 (void)s;(void)n;(void)d;if(!strcmp(a,"change"))uevents++;
}
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
static void dev_release(device_t*d) { released++;free(d); }
static int match(device_t*d) { return !strcmp(d->name,"test-device"); }
static int probe(device_t*d) { (void)d;return 0; }
static int remove_dev(device_t*d) { (void)d;__atomic_add_fetch(&removed,1,__ATOMIC_RELEASE);return 0; }
static int read_disk(device_t*d,uint64_t l,void*b,size_t n) { (void)d;(void)l;memset(b,0x5a,n*512);return 0; }
static int write_disk(device_t*d,uint64_t l,const void*b,size_t n) { (void)d;(void)l;(void)b;(void)n;return 0; }
static uint64_t capacity(device_t*d) { (void)d;return 4096; }
static uint32_t sector_size(device_t*d) { (void)d;return 512; }
static int flush(device_t*d) { (void)d;return 0; }
static void *unregister_thread(void *p) { assert(driver_unregister(p)==0);return NULL; }
static int net_send(device_t*d,const void*p,size_t n) { (void)d;(void)p;sends++;return (int)n; }
static int net_recv(device_t*d,void*p,size_t n) { (void)d;(void)p;(void)n;return 0; }
static const uint8_t *net_mac(device_t*d) { static const uint8_t mac[6]={2,3,4,5,6,7};(void)d;return mac; }
static void net_poll(device_t*d) { (void)d;polls++; }
static uint32_t net_caps(device_t*d) { (void)d;return NET_DEV_CAP_TX_SG; }
static class_device_t *observer_pin;
static void offline_observer(class_device_t*c,void*p) {
 assert(p==(void *)7 && !c->state_lock.held);
 assert(class_device_call_begin(c)==-ENODEV);
 assert(observer_pin==c);observer_pin=NULL;class_device_put(c);
}
int main(void) {
 driver_core_init();
 block_dev_ops_t *ops=malloc(sizeof(*ops));*ops=(block_dev_ops_t){.read=read_disk,.write=write_disk,
  .capacity=capacity,.sector_size=sector_size,.flush=flush};
 driver_t *drv=calloc(1,sizeof(*drv));*drv=(driver_t){.name="block",.match=match,.probe=probe,
  .remove=remove_dev,.class_type=DEV_CLASS_BLOCK,.class_ops=ops};
 assert(driver_register(drv)==0);
 device_t *dev=calloc(1,sizeof(*dev));dev->name="test-device";dev->release=dev_release;
 assert(device_register(dev)==0 && dev->references==3);
 device_t *lookup=device_find_get_by_class(DEV_CLASS_BLOCK,0);assert(lookup==dev);device_put(lookup);
 class_device_t *open=class_device_ref_for_device(dev);assert(open&&open->index==0);
 block_dev_t *disk=mount_setup_block_device(0);assert(disk&&disk==mount_setup_block_device(0));
 unsigned char data[512];assert(disk->read_sector(disk,0,data,1)==0&&data[511]==0x5a);
 const void *table;assert(class_device_ops_begin(open,DEV_CLASS_BLOCK,&table)==0);
 pthread_t thread;assert(!pthread_create(&thread,NULL,unregister_thread,drv));
 while(__atomic_load_n(&open->online,__ATOMIC_ACQUIRE))sched_yield();
 assert(__atomic_load_n(&removed,__ATOMIC_ACQUIRE)==0);
 class_device_call_end(open);assert(!pthread_join(thread,NULL));assert(removed==1);
 free(ops);free(drv);dev->drv=(void *)1;
 assert(disk->read_sector(disk,0,data,1)==-ENODEV && disk->flush(disk)==-ENODEV);
 assert(!mount_setup_block_device(0));
 dev->drv=NULL;device_unregister(dev);device_put(dev);assert(!released);
 class_device_put(open);assert(released==1);
 net_dev_ops_t *net_ops=malloc(sizeof(*net_ops));*net_ops=(net_dev_ops_t){.send=net_send,.recv=net_recv,
  .mac=net_mac,.poll=net_poll,.caps=net_caps};
 driver_t net_drv={.name="network",.match=match,.probe=probe,.remove=remove_dev,
  .class_type=DEV_CLASS_NET,.class_ops=net_ops};
 assert(driver_register(&net_drv)==0);dev=calloc(1,sizeof(*dev));dev->name="test-device";dev->release=dev_release;
 assert(device_register(dev)==0);open=class_device_ref_for_device(dev);assert(class_net_ready(open));
 observer_pin=open;class_device_get(open);assert(!class_device_set_offline(open,offline_observer,(void *)7));
 uint8_t mac[6];assert(!class_net_copy_mac(open,mac)&&mac[0]==2&&mac[5]==7);
 assert(class_net_caps(open)==0);assert(class_net_send(open,data,7)==7&&sends==1);
 assert(class_net_recv(open,data,sizeof(data))==0&&class_net_link_up(open)==1);
 assert(class_net_send_sg(open,NULL,0)==-EOPNOTSUPP);class_net_poll(open);assert(polls==1);
 assert(driver_unregister(&net_drv)==0&&!observer_pin);free(net_ops);dev->drv=(void *)1;
 assert(!class_net_ready(open)&&class_net_send(open,data,7)==-ENODEV);
 assert(class_net_recv(open,data,7)==-ENODEV&&class_net_copy_mac(open,mac)==-ENODEV);
 class_net_poll(open);assert(polls==1);dev->drv=NULL;device_unregister(dev);device_put(dev);
 assert(released==1);class_device_put(open);assert(released==2);
 /* Block indices are a lifetime budget, including offline publications. */
 driver_t budget_drv={.class_type=DEV_CLASS_BLOCK,.class_ops=&(block_dev_ops_t){.read=read_disk,
  .write=write_disk,.capacity=capacity,.sector_size=sector_size}};
 for(unsigned i=1;i<256;i++) {
  device_t d={.name="budget",.drv=&budget_drv,.references=1};
  assert(!class_device_publish(&d)&&d.class_dev->index==i);
  if(i==1){uevents=0;class_device_emit_uevents("change");assert(uevents==1);}
  assert(!class_device_get_by_index(DEV_CLASS_BLOCK,0));
  class_device_unpublish(&d);device_put(&d);
 }
 device_t exhausted={.name="exhausted",.drv=&budget_drv,.references=1};
 assert(class_device_publish(&exhausted)==-ENOSPC && !exhausted.class_dev && exhausted.references==1);
 assert(!mount_setup_block_device(0));assert(disk->read_sector(disk,0,data,1)==-ENODEV);
 puts("class binding lifetime: PASS");return 0;
}
'''

class ClassBindingTest(unittest.TestCase):
    def test_production_hotunplug_and_identity(self):
        stubs = dict(STUBS)
        stubs.update({
            'net/socket/socket_internal.h': '#include "core/types.h"\nvoid netlink_uevent_emit(const char *,const char *,const char *,uint64_t);\n',
            'fs/vfs.h': '', 'fs/block_cache.h': '',
        })
        with tempfile.TemporaryDirectory(prefix='a20-class-binding-') as name:
            work=Path(name)
            for path, text in stubs.items():
                p=work/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
            (work/'test.c').write_text(HARNESS)
            executable=work/'test'
            sources=['kernel/drivers/core/driver_core.c','kernel/drivers/core/driver_class.c', 'kernel/drivers/core/driver_protocol.c',
                     'kernel/firmware/node.c','kernel/fs/mount_setup.c']
            subprocess.run(['cc','-std=gnu99','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
                '-DBRINGUP','-DCONFIG_NR_CPUS=2','-ffunction-sections','-fdata-sections',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                '-I'+str(work),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),
                str(work/'test.c'),*[str(ROOT/p) for p in sources],'-Wl,--gc-sections','-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=15,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

if __name__=='__main__': unittest.main()

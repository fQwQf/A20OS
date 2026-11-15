"""Production TTY routing must lease bindings and never replay accepted bytes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class BulkTtyTests(unittest.TestCase):
    def test_capability_fallback_partial_errors_and_binding_revocation(self):
        source=(ROOT/'kernel/fs/devfs/tty.c').read_text()
        body=source[source.index('int tty_console_write('):source.index('#define DEVFS_KDSETMODE')]
        fixture=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ENOSYS 38
#define EIO 5
#define ERESTARTSYS 512
#define VPORT_IOCTL_CONSOLE_READY 0x9004
#define TTY_EMIT_CHUNK 256
#define DEV_CLASS_CHAR 1
typedef struct {} vfile_t;
typedef struct {} device_t;
typedef struct {device_t *dev;} class_device_t;
typedef struct {int(*write)(device_t*,const void*,size_t);int(*ioctl)(device_t*,unsigned long,void*);} char_dev_ops_t;
static device_t device;static class_device_t binding={&device};
static class_device_t *g_tty_bulk_console;static int g_tty_use_bulk_console=1,g_tty_bulk_ready;
static int g_tty_write_lock,klog_console_lock,spin_held,mutex_held,active,refs,gets,puts,online=1,supported,interrupted,fail_after;
static char serial[1024],bulk[1024];static size_t ns,nb;static int capabilities;
static int access_error,revoked,revoke_on_lock;
static int tty_control_access(vfile_t *vf,int writing,int force){(void)vf;assert(writing==1&&force==0&&!mutex_held);return access_error;}
static int tty_control_revoked(const vfile_t *vf){(void)vf;assert(mutex_held);return revoked;}
static void mutex_lock(int *p){(void)p;assert(!mutex_held);mutex_held=1;if(revoke_on_lock)revoked=1;}
static void mutex_unlock(int *p){(void)p;assert(mutex_held);mutex_held=0;}
static uint64_t spin_lock_irqsave(int *p){(void)p;assert(!spin_held);spin_held=1;return 0;}
static void spin_unlock_irqrestore(int *p,uint64_t f){(void)p;(void)f;assert(spin_held);spin_held=0;}
static int write_bulk(device_t *d,const void *p,size_t n){
 assert(d==&device&&active==1&&mutex_held&&!spin_held);
 if(fail_after&&nb>=(size_t)fail_after)return -EIO;
 if(n>16)n=16;memcpy(bulk+nb,p,n);nb+=n;return (int)n;
}
static int ioctl_bulk(device_t *d,unsigned long r,void *p){
 (void)p;assert(d==&device&&r==VPORT_IOCTL_CONSOLE_READY&&active==1);capabilities++;return supported?0:-25;
}
static const char_dev_ops_t ops={write_bulk,ioctl_bulk};
static class_device_t *class_device_get_by_name(const char *name){assert(!strcmp(name,"vport0"));gets++;if(!online)return NULL;refs++;return &binding;}
static int class_device_ops_begin(class_device_t *p,unsigned type,const void **out){assert(p==&binding&&type==1);if(!online)return -19;active++;*out=&ops;return 0;}
static void class_device_call_end(class_device_t *p){assert(p==&binding&&active==1);active--;}
static void class_device_put(class_device_t *p){assert(p==&binding&&refs);refs--;puts++;}
static void *proc_current(void){return NULL;}
static int signal_task_has_unblocked(void *p){(void)p;return interrupted;}
static void tty_parse_mouse_mode(const char *p,size_t n){(void)p;(void)n;}
static void uart_write(const char *p,size_t n){assert(mutex_held&&spin_held);memcpy(serial+ns,p,n);ns+=n;}
'''+body+r'''
int main(void){
 char data[80];memset(data,'x',sizeof(data));
 assert(tty_console_write(NULL,data,0)==0&&!gets);
 /* Old modules cannot silently consume terminal data through a raw port. */
 assert(tty_console_write(NULL,data,20)==20);assert(ns==20&&!nb&&!refs&&puts==1);
 supported=1;ns=0;assert(tty_console_write(NULL,data,40)==40);assert(nb==40&&!ns&&refs==1);
 int old=capabilities;assert(tty_console_write(NULL,data,1)==1);assert(capabilities==old);
 /* A failed next submission must not replay a completed prefix on UART. */
 nb=0;fail_after=16;assert(tty_console_write(NULL,data,40)==16);assert(nb==16&&!ns);
 fail_after=0;interrupted=1;nb=0;assert(tty_console_write(NULL,data,40)==-ERESTARTSYS);assert(!nb&&!ns);interrupted=0;
 online=0;assert(tty_console_write(NULL,data,10)==10);assert(ns==10&&!nb&&!refs&&!g_tty_bulk_ready);
 online=1;ns=0;assert(tty_console_write(NULL,data,10)==10);assert(nb==10&&!ns&&capabilities==old+1);
 vfile_t file;int before_gets=gets;size_t before_bulk=nb,before_serial=ns;
 access_error=-ERESTARTSYS;
 assert(tty_console_write(&file,data,10)==-ERESTARTSYS&&!mutex_held);
 assert(gets==before_gets&&nb==before_bulk&&ns==before_serial);
 access_error=0;revoke_on_lock=1;
 assert(tty_console_write(&file,data,10)==-EIO&&!mutex_held);
 assert(gets==before_gets&&nb==before_bulk&&ns==before_serial);
 revoke_on_lock=revoked=0;
 assert(!active&&!mutex_held&&!spin_held);class_device_put(g_tty_bulk_console);return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'tty.c';path.write_text(fixture);exe=Path(tmp)/'tty'
            subprocess.run(['cc','-std=gnu99','-O2','-Wall','-Wextra','-Werror',
                            '-Wno-misleading-indentation','-fsanitize=address,undefined',
                            str(path),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':
    unittest.main()

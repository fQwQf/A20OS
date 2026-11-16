"""Terminal entry points must refuse handoff after a fallible device stop."""
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
#include <setjmp.h>
#include <stdlib.h>
#define kinfo(...) ((void)0)
#define CAP_SYS_BOOT 1
#define A20_ARG(n) (args->arg[n])
typedef struct {uint64_t entry; unsigned nr;} a20_elf_kexec_ledger_t;
typedef struct {uint64_t arg[6];} a20_syscall_args_t;
typedef struct {struct {unsigned euid;} cred;} task_t;
static task_t task;
static task_t *proc_current(void){return &task;}
static int proc_has_cap(task_t *t,int c){(void)t;(void)c;return 1;}
static a20_elf_kexec_ledger_t g_kexec_image;
static int g_kexec_lock,g_kexec_executing,g_kexec_loading;
static unsigned events[8],count,fail_stage,cancels,firmware_calls;
static int backend_available=1;
static uint32_t machine_kexec_backend_id(void){return backend_available;}
static jmp_buf terminal;
static uint64_t spin_lock_irqsave(int *p){(void)p;return 0;}
static void spin_unlock_irqrestore(int *p,uint64_t f){(void)p;(void)f;}
static int kexec_may_manage(void){return 0;}
static int a20_elf_kexec_loaded(const void *p){(void)p;return 1;}
static int step(unsigned n){events[count++]=n;return fail_stage==n?-ETIMEDOUT:0;}
static int driver_core_kexec_prepare(void){return step(1);}
static int vfs_system_shutdown(void){return step(2);}
static int driver_core_shutdown_all(void){return step(3);}
static int machine_kexec_prepare(const void *p){(void)p;return step(4);}
static void machine_kexec_cancel(void){cancels++;}
static void machine_kexec(const void *p){(void)p;events[count++]=5;longjmp(terminal,1);}
static void firmware_shutdown(void){firmware_calls++;longjmp(terminal,1);}
static void firmware_reboot(void){firmware_calls++;longjmp(terminal,1);}
static void klog_disk_mark_clean(void){}
static void klog_disk_mark_ready(void){}
static int mntns_shutdown_active(void){return 1;}
static void panic(const char *s){(void)s;abort();}
static void reset(unsigned failed){count=cancels=firmware_calls=0;fail_stage=failed;g_kexec_executing=0;}
'''
MAIN = r'''
int main(void){
 reset(0);backend_available=0;
 assert(kexec_execute()==-EOPNOTSUPP&&!count&&!cancels&&!g_kexec_executing);
 backend_available=1;
 for(unsigned failed=1;failed<=4;failed++){
  reset(failed);assert(kexec_execute()==-ETIMEDOUT&&!g_kexec_executing);
  assert(count==failed&&cancels==(failed==4));
  for(unsigned i=0;i<count;i++)assert(events[i]==i+1);
 }
 reset(0);
 if(!setjmp(terminal)){kexec_execute();abort();}
 assert(count==5&&!cancels);for(unsigned i=0;i<count;i++)assert(events[i]==i+1);
 /* Both reboot/poweroff commands stop before firmware on a stop error. */
 for(unsigned cmd=0;cmd<2;cmd++){
  reset(3);a20_syscall_args_t args={.arg={cmd}};
  assert(sys_a20_system_reboot(&args)==-A20_ERR_TIMED_OUT);
  assert(count==2&&events[0]==2&&events[1]==3&&!firmware_calls);
  reset(3);
  assert(sys_reboot(0xfee1deadUL,672274793UL,cmd?0x01234567UL:0x4321fedcUL)==-ETIMEDOUT);
  assert(count==2&&events[0]==2&&events[1]==3&&!firmware_calls);
  reset(0);
  if(!setjmp(terminal)){sys_a20_system_reboot(&args);abort();}
  assert(count==2&&events[0]==2&&events[1]==3&&firmware_calls==1);
 }
 return 0;
}
'''


class DriverTerminalEntry(unittest.TestCase):
    def test_kexec_and_reboot_order_and_error_propagation(self):
        native = (ROOT / 'kernel/abi/native/sys_native_system.c').read_text()
        code = function((ROOT / 'kernel/core/kexec.c').read_text(), 'kexec_execute')
        code += function(native, 'kexec_native_status')
        code += function(native, 'sys_a20_system_reboot')
        code += function((ROOT / 'kernel/abi/linux/sys_proc.c').read_text(), 'sys_reboot')
        statuses = '\n'.join(line for line in (ROOT / 'kernel/include/ipc/ipc.h').read_text().splitlines()
                             if line.startswith('#define A20_ERR_') or line.startswith('#define A20_OK '))
        with tempfile.TemporaryDirectory(prefix='a20-terminal-entry-') as directory:
            source = Path(directory) / 'entry.c'
            binary = source.with_suffix('')
            source.write_text(MODEL + statuses + '\n' + code + MAIN)
            built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=undefined', str(source), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

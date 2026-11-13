"""Run real libevdev initialization against the production evdev ioctl code."""
import ctypes.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#define _GNU_SOURCE
#include <linux/input.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include "kernel/drivers/input/mt_state.h"
#define A20_IOC_TYPE _IOC_TYPE
#define A20_IOC_NR _IOC_NR
#define A20_IOC_SIZE _IOC_SIZE
#define EVIOCGCLOCKID _IOR('E',0xa0,int)
#define EVIOCGNAME_NR 6
#define EVIOCGPHYS_NR 7
#define EVIOCGUNIQ_NR 8
#define EVIOCGPROP_NR 9
#define EVIOCGMTSLOTS_NR 10
#define EVIOCGKEY_NR 0x18
#define EVIOCGBIT_NR_BASE 0x20
#define EVIOCGABS_NR_BASE 0x40
#define INPUT_BITMAP_BYTES 128
#define INPUT_EVENT_TYPES 32
#define INPUT_KEY_BYTES ((KEY_MAX+8)/8)
typedef struct {int online;} class_device_t;
struct input_device_info {
 char name[128]; struct input_id id;
 uint8_t properties[128],bits[32][128];
 struct input_absinfo abs[64];
};
typedef struct {
 class_device_t *device;struct input_device_info info;
 uint8_t keys[INPUT_KEY_BYTES];int clock_id,revoked,grabbed;
 struct input_client *next;
} input_client_t;
/* The real ioctl traverses next only for grabs; none are requested here. */
typedef struct {void *priv;} vfile_t;
typedef struct {struct input_mt_state mt;} input_state_t;
static input_state_t state;
static int g_input_lock;
static input_client_t *g_clients;
static void mutex_lock(int *x){(void)x;}
static void mutex_unlock(int *x){(void)x;}
static void input_pump(input_client_t *x){(void)x;}
static input_state_t *input_state(class_device_t *x){(void)x;return &state;}
static void input_mux_wake(void){}
static uint32_t input_led_state(input_client_t *c){(void)c;return 0;}
static int copy_from_user(void*d,const void*s,size_t n){memcpy(d,s,n);return 0;}
static int copy_to_user(void*d,const void*s,size_t n){memcpy(d,s,n);return 0;}
'''
POSTLUDE = r'''
static class_device_t device={1};
static input_client_t client;
static int initialized;
static void bit(unsigned type,unsigned code){
 client.info.bits[0][type/8]|=1U<<(type%8);
 client.info.bits[type][code/8]|=1U<<(code%8);
}
int ioctl(int fd,unsigned long req,...) {
 va_list ap;va_start(ap,req);void *arg=va_arg(ap,void*);va_end(ap);
 if(fd!=2222) {
  int (*real)(int,unsigned long,...)=dlsym(RTLD_NEXT,"ioctl");
  return real(fd,req,arg);
 }
 if(!initialized){
  initialized=1;client.device=&device;strcpy(client.info.name,"A20 touchpad fixture");
  client.info.id=(struct input_id){BUS_I2C,0x06cb,0xce78,1};
  client.info.properties[0]=5;
  bit(EV_KEY,BTN_LEFT);bit(EV_KEY,BTN_TOUCH);bit(EV_KEY,BTN_TOOL_FINGER);
  unsigned axes[]={ABS_X,ABS_Y,ABS_MT_SLOT,ABS_MT_TRACKING_ID,ABS_MT_POSITION_X,ABS_MT_POSITION_Y};
  for(unsigned i=0;i<6;i++)bit(EV_ABS,axes[i]);
  client.info.abs[ABS_X]=(struct input_absinfo){.maximum=1404,.resolution=12};
  client.info.abs[ABS_Y]=(struct input_absinfo){.maximum=864,.resolution=12};
  client.info.abs[ABS_MT_POSITION_X]=client.info.abs[ABS_X];
  client.info.abs[ABS_MT_POSITION_Y]=client.info.abs[ABS_Y];
  client.info.abs[ABS_MT_SLOT].maximum=4;
  client.info.abs[ABS_MT_TRACKING_ID]=(struct input_absinfo){.value=-1,.maximum=65535};
  input_mt_init(&state.mt);
  input_mt_update(&state.mt,ABS_MT_TRACKING_ID,42);
  input_mt_update(&state.mt,ABS_MT_POSITION_X,100);
  input_mt_update(&state.mt,ABS_MT_SLOT,3);
  input_mt_update(&state.mt,ABS_MT_TRACKING_ID,99);
  input_mt_update(&state.mt,ABS_MT_POSITION_X,800);
 }
 if(getenv("A20_MT_NEGATIVE") && _IOC_NR(req)==10){errno=ENOTTY;return -1;}
 vfile_t file={&client};int ret=input_ioctl(&file,req,arg);
 if(ret<0){errno=-ret;return -1;}return ret;
}
'''


class LibevdevMultitouchTest(unittest.TestCase):
    @unittest.skipUnless(ctypes.util.find_library("evdev"), "libevdev runtime required")
    def test_real_library_reads_distinct_slots(self):
        production = function((ROOT / "kernel/drivers/input/input_mux.c").read_text(), "input_ioctl")
        # Match the self-referential production client list type.
        prelude = PRELUDE.replace("typedef struct {\n class_device_t", "typedef struct input_client {\n class_device_t")
        probe = r'''
import ctypes
lib=ctypes.CDLL('libevdev.so.2')
lib.libevdev_new_from_fd.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_void_p)]
lib.libevdev_get_slot_value.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint]
lib.libevdev_free.argtypes=[ctypes.c_void_p]
d=ctypes.c_void_p();rc=lib.libevdev_new_from_fd(2222,ctypes.byref(d))
assert rc==0,rc
assert lib.libevdev_get_slot_value(d,0,0x39)==42
assert lib.libevdev_get_slot_value(d,3,0x39)==99
assert lib.libevdev_get_slot_value(d,1,0x39)==-1
assert lib.libevdev_get_slot_value(d,0,0x35)==100
assert lib.libevdev_get_slot_value(d,3,0x35)==800
lib.libevdev_free(d)
'''
        with tempfile.TemporaryDirectory(prefix="a20-libevdev-") as directory:
            path = Path(directory)
            (path / "shim.c").write_text(prelude + production + POSTLUDE)
            subprocess.run(["cc", "-shared", "-fPIC", "-I", str(ROOT),
                            str(path/"shim.c"), "-ldl", "-o", str(path/"shim.so")],
                           check=True, capture_output=True)
            env = {**os.environ, "LD_PRELOAD": str(path/"shim.so")}
            subprocess.run([sys.executable, "-c", probe], env=env, check=True,
                           capture_output=True, timeout=10)
            negative = subprocess.run([sys.executable, "-c", probe],
                env={**env, "A20_MT_NEGATIVE": "1"}, capture_output=True, timeout=10)
            self.assertNotEqual(negative.returncode, 0, "old missing ioctl must fail the slot snapshot")


if __name__ == "__main__":
    unittest.main()

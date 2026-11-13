"""Exercise production USB report construction and evdev LED write routing."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRELUDE=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include "kernel/include/core/input.h"
#define INPUT_IOCTL_LED_EVENT 0xa201U
#define INPUT_IOCTL_GET_LEDS 0xa202U
#define USB_HID_KBD 1
#define ITE_8910_REPORT_ID 1
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_INTERFACE 1
#define DEV_CLASS_INPUT 1
#define kinfo(...) ((void)0)
#define kwarn(...) ((void)0)
struct input_event {uint64_t sec,usec;uint16_t type,code;int32_t value;};
typedef int mutex_t;
static int locks;
static void mutex_lock(int *m){(void)m;locks++;}
static void mutex_unlock(int *m){(void)m;locks--;}
typedef struct {unsigned vendor,product;} usb_device_t;
typedef struct {usb_device_t *dev;unsigned interface_number;} usb_interface_t;
typedef struct {usb_interface_t *iface;int kind,ite_keyboard;mutex_t led_lock;uint32_t leds;uint8_t leds_valid;} usb_hid_dev_t;
static unsigned calls,fail,record_value,record_length,record_index;
static unsigned char record[2];
static int usb_control_msg(usb_device_t *d,unsigned type,unsigned req,unsigned val,unsigned idx,void *buf,unsigned len){
 (void)d;assert(type==0x21&&req==9&&len<=2);assert(locks==1);
 calls++;record_value=val;record_length=len;record_index=idx;memcpy(record,buf,len);return fail?-EIO:0;
}
'''
MAIN=r'''
int main(void){
 usb_device_t u={0x048d,0xc102};usb_interface_t iface={&u,0};
 usb_hid_dev_t h={.iface=&iface,.kind=USB_HID_KBD,.ite_keyboard=1};
 struct input_event e={.type=EV_LED,.code=LED_CAPSL,.value=1};
 assert(!usb_hid_led_event(&h,&e));assert(h.leds==2&&calls==1);
 assert(record_value==0x201&&record_length==2&&record[0]==1&&record[1]==2&&record_index==0);
 assert(!usb_hid_led_event(&h,&e)&&calls==1);
 e.code=LED_NUML;assert(!usb_hid_led_event(&h,&e)&&h.leds==3&&record[1]==3);
 fail=1;e.value=0;assert(usb_hid_led_event(&h,&e)==-EIO&&h.leds==3);
 fail=0;assert(!usb_hid_led_event(&h,&e)&&h.leds==2);
 h.ite_keyboard=0;h.leds_valid=0;e.code=LED_SCROLLL;
 assert(!usb_hid_led_event(&h,&e)&&record_value==0x200&&record_length==1&&record[0]==2);
 e.code=LED_MAX;assert(usb_hid_led_event(&h,&e)==-EOPNOTSUPP);
 assert(!locks);return 0;
}
'''
class UsbKeyboardLeds(unittest.TestCase):
 def test_ite_report_and_boot_protocol(self):
  code=function((ROOT/'kernel/drivers/usb/class/usb_hid.c').read_text(),'usb_hid_led_event')
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'led.c';exe=p.with_suffix('');p.write_text(PRELUDE+code+MAIN)
   subprocess.run(['cc','-std=gnu11','-I',str(ROOT),'-fsanitize=undefined',str(p),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()

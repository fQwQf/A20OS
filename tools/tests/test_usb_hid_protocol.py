"""Select and observe ITE report protocol instead of inheriting firmware state."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define USB_HID_KBD 1
#define USB_HID_MOUSE 2
#define USB_DIR_IN 0x80
#define USB_TYPE_CLASS 0x20
#define USB_RECIP_INTERFACE 1
#define USB_REQ_SET_PROTOCOL 11
#define USB_REQ_SET_IDLE 10
#define kinfo(...) ((void)printf(__VA_ARGS__))
#define kwarn(...) ((void)0)
typedef struct {unsigned vendor,product;} usb_device_t;
typedef struct {usb_device_t *dev;unsigned interface_number;} usb_interface_t;
typedef struct {usb_interface_t *iface;int kind,ite_keyboard;} usb_hid_dev_t;
static unsigned protocol,sets,gets,idles;
static int get_error;
static int usb_control_msg(usb_device_t *d,unsigned type,unsigned req,unsigned val,unsigned idx,void *buf,unsigned len){
 assert(d&&idx==0);
 if(req==11){assert(type==0x21&&val<=1&&!buf&&!len);protocol=val;sets++;return 0;}
 if(req==3){assert(type==0xa1&&!val&&buf&&len==1);gets++;if(get_error)return -EIO;*(uint8_t*)buf=protocol;return 0;}
 assert(req==10&&type==0x21&&!val&&!buf&&!len);idles++;return 0;
}
'''
MAIN = r'''
int main(void){
 usb_device_t device={0x048d,0xc102};usb_interface_t iface={&device,0};
 usb_hid_dev_t h={.iface=&iface,.kind=USB_HID_KBD,.ite_keyboard=1};
 protocol=0; /* Deliberately model firmware leaving boot protocol active. */
 usb_hid_select_protocol(&h);
 assert(protocol==1&&sets==1&&gets==1&&idles==0);
 /* An unavailable observation must not prevent interrupt input setup. */
 get_error=1;usb_hid_select_protocol(&h);assert(protocol==1&&sets==2&&gets==2);
 h.ite_keyboard=0;usb_hid_select_protocol(&h);
 assert(protocol==0&&sets==3&&gets==2&&idles==1);
 h.kind=USB_HID_MOUSE;protocol=1;usb_hid_select_protocol(&h);
 assert(protocol==0&&sets==4&&gets==2&&idles==2);
 return 0;
}
'''


class HidProtocol(unittest.TestCase):
    def test_explicit_report_and_boot_modes(self):
        source = (ROOT / 'kernel/drivers/usb/class/usb_hid.c').read_text()
        code = function(source, 'usb_hid_select_protocol')
        old = code.replace('uint16_t protocol = h->ite_keyboard ? 1 : 0;',
                           'uint16_t protocol = 0;')
        self.assertNotEqual(code, old)
        with tempfile.TemporaryDirectory() as directory:
            def run(body, name):
                path = Path(directory) / (name + '.c')
                exe = path.with_suffix('')
                path.write_text(PRE + body + MAIN)
                subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=undefined', str(path), '-o', str(exe)],
                               check=True, capture_output=True)
                return subprocess.run([str(exe)], capture_output=True, timeout=5).returncode
            self.assertEqual(run(code, 'report'), 0)
            self.assertNotEqual(run(old, 'boot'), 0)


if __name__ == '__main__':
    unittest.main()

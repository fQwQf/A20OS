"""Run production USB HID report completion and the evdev wake entry point.

The interrupt model rejects readiness notification while a driver spinlock is
held and verifies that empty reports cannot consume the first-key diagnostic.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    found = re.search(r"^(?:static )?[^\n]*\b" + name +
                      r"\([^{}]*\)\s*\{.*?^\}", source, re.M | re.S)
    if not found:
        raise AssertionError(f"missing production function {name}")
    return found.group()


PRELUDE = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#define ALIGNED(n) __attribute__((aligned(n)))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define SYN_REPORT 0
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
#define ABS_X 0
#define ABS_Y 1
#define USB_HID_REPORT_MAX 64U
#define USB_HID_EVENTS 64U
#define USB_HID_KBD 1U
#define USB_HID_MOUSE 2U
#define USB_HID_TABLET 3U
#define BTN_LEFT 0x110U
#define BTN_RIGHT 0x111U
#define BTN_MIDDLE 0x112U
typedef struct {int held;} spinlock_t;
typedef struct {int unused;} mutex_t;
typedef struct {int dummy;} usb_interface_t;
typedef struct {int dummy;} usb_endpoint_t;
typedef struct {void *ctx;int status;} usb_urb_t;
struct input_event {uint64_t time_sec,time_usec;uint16_t type,code;int32_t value;};
static unsigned locked,wakes,rearms,announces,warnings,console_keys;
static uint8_t console_usage, console_mods;
/* The separate console test exercises HID-to-tty delivery. */
static int usb_hid_console_key(uint8_t usage, uint8_t mods) {
 if (usage == 0x52)
  return 0;
 console_keys++;
 console_usage=usage;
 console_mods=mods;
 return 1;
}
static int fbcon_active(void) {return 1;}
static uint64_t spin_lock_irqsave(spinlock_t*l) {assert(!l->held);l->held=1;locked++;return 0;}
static void spin_unlock_irqrestore(spinlock_t*l,uint64_t f) {(void)f;assert(l->held);l->held=0;locked--;}
static int usb_submit_urb(usb_urb_t*u) {(void)u;assert(!locked);rearms++;return 0;}
static void host_log(const char*f,...) {(void)f;assert(!locked);announces++;}
#define kinfo host_log
static void host_warn(const char*f,...) {(void)f;assert(!locked);warnings++;}
#define kwarn host_warn
typedef struct {unsigned marker;} wait_queue_t;
#define PROC_WAKE_EVENT 3
static wait_queue_t g_input_waiters;
static unsigned wait_queue_wake_all(wait_queue_t*q,uintptr_t key,unsigned reason) {
 assert(q==&g_input_waiters && key==0 && reason==PROC_WAKE_EVENT);
 assert(!locked); wakes++;return 1;
}
void input_mux_wake(void);
'''

MAIN = r'''
int main(void) {
 usb_hid_dev_t h={.kind=USB_HID_KBD,.report_size=8,.running=1};
 h.urb.ctx=&h;
 /* Successful empty initial boot report is not a key transition. */
 usb_hid_complete(&h.urb);
 assert(!h.key_logged && !wakes && !announces && rearms==1);
 /* Meta+Enter is published before the evdev readiness notification. */
 h.report[0]=8;h.report[2]=0x28;usb_hid_complete(&h.urb);
 assert(wakes==1 && announces==1 && h.key_logged && rearms==2);
 assert(h.head==3 && h.events[0].type==EV_KEY && h.events[0].code==125);
 assert(h.events[1].code==28 && h.events[1].value==1);
 assert(h.events[2].type==EV_SYN && h.first_key_code==125);
 /* Repeated identical report creates neither events nor spurious wakes. */
 usb_hid_complete(&h.urb);assert(wakes==1&&announces==1&&rearms==3);
 /* Clear an already drained queue, release both keys, then a normal key. */
 h.tail=h.head;memset(h.report,0,8);usb_hid_complete(&h.urb);
 assert(wakes==2&&announces==1&&h.events[3].value==0);
 h.tail=h.head;h.report[2]=4;usb_hid_complete(&h.urb);
 assert(wakes==3&&announces==1&&h.events[6].code==30);
 /* Errors and a stopped device must never manufacture a readiness edge. */
 h.urb.status=-1;usb_hid_complete(&h.urb);assert(wakes==3&&rearms==6);
 h.running=0;h.urb.status=0;usb_hid_complete(&h.urb);assert(wakes==3&&rearms==6);
 /* The shared path also notifies relative mice and absolute tablets. */
 usb_hid_dev_t m={.kind=USB_HID_MOUSE,.report_size=4,.running=1};m.urb.ctx=&m;
 m.report[1]=12;usb_hid_complete(&m.urb);
 assert(wakes==4&&m.events[0].type==EV_REL&&m.events[0].value==12);
 usb_hid_dev_t t={.kind=USB_HID_TABLET,.report_size=8,.running=1};t.urb.ctx=&t;
 t.report[4]=10;t.report[6]=20;usb_hid_complete(&t.urb);
 assert(wakes==5&&t.events[0].type==EV_ABS&&t.events[0].value==10);
 /* A full queue cannot publish an event, so should not wake readers again. */
 h.running=1;h.head=63;h.tail=0;h.report[2]=5;
 usb_hid_complete(&h.urb);assert(wakes==5);

 usb_hid_dev_t ite={.kind=USB_HID_KBD,.ite_keyboard=1,.report_size=64,.running=1};
 ite.urb.ctx=&ite;
 wakes=announces=warnings=console_keys=0;
 /* A consumer-control report is visible but does not alter keyboard state. */
 ite.report[0]=2;ite.report[3]=4;usb_hid_complete(&ite.urb);
 assert(announces==1&&!wakes&&!console_keys&&ite.report_logged);
 /* The physical boot's first decoded key was Up, which the tty bridge skips. */
 ite.report[0]=1;ite.report[3]=0x52;usb_hid_complete(&ite.urb);
 assert(announces==2&&wakes==1&&!console_keys&&!ite.console_input_logged);
 assert(ite.events[0].code==103&&ite.events[0].value==1);
 memset(ite.previous,0,sizeof(ite.previous));ite.head=0;
 wakes=0;ite.report[3]=4;
 /* A key reported in both the array and bitmap is delivered once. */
 ite.report[0]=1;ite.report[1]=2;ite.report[9]=1U<<4;
 usb_hid_complete(&ite.urb);
 assert(wakes==1&&announces==3&&console_keys==1&&console_usage==4&&console_mods==2);
 assert(ite.console_input_logged);
 assert(ite.events[0].code==42&&ite.events[1].code==30&&ite.events[1].value==1);
 ite.report[0]=3;usb_hid_complete(&ite.urb);assert(wakes==1);
 memset(ite.report,0,sizeof(ite.report));ite.report[0]=1;
 usb_hid_complete(&ite.urb);
 assert(wakes==2&&console_keys==1&&ite.events[4].code==30&&ite.events[4].value==0);
 /* Bitmap-only Enter also reaches the console bridge. */
 ite.report[9+0x28/8]=1;usb_hid_complete(&ite.urb);
 assert(wakes==3&&announces==3&&console_keys==2&&console_usage==0x28);
 ite.urb.status=-4;usb_hid_complete(&ite.urb);usb_hid_complete(&ite.urb);
 assert(warnings==1&&wakes==3);
 assert(!locked);
 return 0;
}
'''


class UsbHidReadiness(unittest.TestCase):
    def test_production_report_to_readiness(self):
        hid = (ROOT / "kernel/drivers/usb/class/usb_hid.c").read_text()
        mux = (ROOT / "kernel/drivers/input/input_mux.c").read_text()
        structure = re.search(r"typedef struct usb_hid_dev \{.*?\} usb_hid_dev_t;",
                              hid, re.S).group()
        table = re.search(r"static const uint16_t hid_usage_to_linux.*?^\};",
                          hid, re.M | re.S).group()
        ite_defines = "\n".join(re.findall(r"^#define ITE_8910_.*$", hid, re.M))
        bodies = "\n".join(function(hid, n) for n in (
            "usb_hid_emit", "usb_hid_key_present", "usb_hid_keyboard_modifiers",
            "usb_hid_parse_keyboard", "usb_hid_ite_key_present", "usb_hid_parse_ite_keyboard",
            "usb_hid_parse_mouse", "usb_hid_parse_tablet", "usb_hid_complete"))
        wake = function(mux, "input_mux_wake")
        with tempfile.TemporaryDirectory(prefix="a20-hid-wake-") as tmp:
            path = Path(tmp) / "model.c"
            path.write_text(PRELUDE + ite_defines + "\n" + structure + table + wake + bodies + MAIN)
            exe = Path(tmp) / "model"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_evdev_readiness_source_uses_notified_queue(self):
        mux = (ROOT / "kernel/drivers/input/input_mux.c").read_text()
        self.assertIn("&g_input_waiters", function(mux, "input_poll_sources"))
        self.assertIn(".poll_sources = input_poll_sources", mux)
        self.assertIn("wait_queue_link(&g_input_waiters", function(mux, "input_read"))


if __name__ == "__main__":
    unittest.main()

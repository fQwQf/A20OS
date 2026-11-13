"""Production connect transaction must preserve an already authorized link."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]
MODEL = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "user/wifi/control.h"
#define RTW89_GLUE_LINK_DOWN 0
#define RTW89_GLUE_LINK_CONNECTED 3
struct rtw89_glue_link { int state, authorized, last_error; };
static struct rtw89_glue_link current;
static uint8_t joined_ssid[32];static uint32_t joined_ssid_length;
static unsigned scans,joins,leaves,starts,stops,lease;
static int stopping;static char interface[]="en3";
static void IOLog(const char *format,...){(void)format;}
static void rtw89_glue_link(struct rtw89_glue_link *out){*out=current;}
static int access_stub(const char *p,int mode){(void)p;(void)mode;return lease?0:-1;}
#define access access_stub
static int address_text(char *out,size_t n){strncpy(out,"10.1.2.3",n);return lease;}
static void stop_dhcp(void){stops++;lease=0;}
static int start_dhcp(void){starts++;lease=1;return 0;}
static void rtw89_glue_leave(void){leaves++;current=(struct rtw89_glue_link){0};}
static void publish_link(void){}
static int rtw89_glue_scan_for(const uint8_t *s,size_t n){(void)s;(void)n;scans++;return 0;}
static int wait_scan(void){return 0;}
static int rtw89_glue_join(const uint8_t*s,size_t n,const char*p,size_t pn){
 (void)s;(void)n;(void)p;(void)pn;joins++;
 if(current.state==RTW89_GLUE_LINK_CONNECTED)return -EBUSY;
 current=(struct rtw89_glue_link){.state=RTW89_GLUE_LINK_CONNECTED,.authorized=1};return 0;
}
static uint64_t monotonic_ms(void){return 0;}
static int usleep_stub(unsigned n){(void)n;return 0;}
#define usleep usleep_stub
'''
MAIN = r'''
int main(void){
 struct wifi_command cmd={.ssid_length=8};memcpy(cmd.ssid,"WHU-WLAN",8);
 assert(!connect_network(&cmd));assert(scans==1&&joins==1&&starts==1&&lease);
 unsigned stopped=stops;
 assert(!connect_network(&cmd));assert(scans==1&&joins==1&&starts==1&&lease&&stops==stopped);
 lease=0;assert(!connect_network(&cmd));assert(scans==1&&joins==1&&starts==2&&lease);
 /* SSIDs are exact bytes, including embedded NULs. */
 cmd.ssid_length=4;memcpy(cmd.ssid,"a\0bc",4);
 assert(!connect_network(&cmd));assert(scans==2&&joins==2&&leaves==1);
 assert(!connect_network(&cmd));assert(scans==2&&joins==2&&leaves==1);
 cmd.ssid[3]='d';assert(!connect_network(&cmd));assert(scans==3&&joins==3&&leaves==2);
 return 0;
}
'''

class WifiConnectReuse(unittest.TestCase):
    def test_production_connect_preserves_lease_and_retries_dhcp(self):
        code = function((ROOT / 'user/wifi/rtw89d.c').read_text(), 'connect_network')
        with tempfile.TemporaryDirectory() as temp:
            source, binary = Path(temp) / 'test.c', Path(temp) / 'test'
            source.write_text(MODEL + code + MAIN)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=undefined', '-I', str(ROOT),
                            str(source), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)

if __name__ == '__main__':
    unittest.main()

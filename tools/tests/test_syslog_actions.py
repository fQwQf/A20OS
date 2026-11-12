"""Linux dmesg READ_ALL must never clear the retained kernel log."""
from pathlib import Path
import re,subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
class SyslogActions(unittest.TestCase):
 def test_read_clear_capacity_and_unsupported_stream(self):
  s=(ROOT/'kernel/abi/linux/sys_misc.c').read_text()
  constants='\n'.join(re.findall(r'^#define LINUX_SYSLOG_ACTION[^\n]+',s,re.M))
  code=function(s,'syslog_read_ring')+function(s,'sys_syslog')
  pre=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define KLOG_BUF_SIZE 4096
#define KLOG_DEBUG 0
#define KLOG_INFO 1
#define KLOG_WARN 2
#define KLOG_ERR 3
static int klog_level=1,syslog_saved_console_level=1;
static char text[16]="abc";
static int klog_read(char*b,size_t n,size_t*p){size_t len=strlen(text);if(*p>=len)return 0;if(n>len-*p)n=len-*p;memcpy(b,text+*p,n);*p+=n;return n;}
static void klog_clear(void){text[0]=0;}
static int copy_to_user(void*d,const void*s,size_t n){memcpy(d,s,n);return 0;}
'''
  main=r'''
int main(void){char b[16]={0};
 assert(sys_syslog(3,b,16)==3&&!memcmp(b,"abc",3)&&!strcmp(text,"abc"));
 assert(sys_syslog(3,b,2)==2&&!strcmp(text,"abc"));
 assert(sys_syslog(10,0,0)==4096);
 assert(sys_syslog(2,b,16)==-EOPNOTSUPP&&!strcmp(text,"abc"));
 assert(sys_syslog(9,0,0)==-EOPNOTSUPP&&klog_level==1);
 assert(sys_syslog(4,b,16)==3&&text[0]==0);
 strcpy(text,"abc");assert(sys_syslog(5,0,0)==0&&text[0]==0);
 assert(sys_syslog(6,0,0)==0&&klog_level==4);
 assert(sys_syslog(7,0,0)==0&&klog_level==1);
 assert(sys_syslog(8,0,8)==0&&klog_level==0);
 assert(sys_syslog(8,0,9)==-EINVAL);
 assert(sys_syslog(3,0,3)==-EFAULT);return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'syslog.c';exe=p.with_suffix('');p.write_text(pre+constants+"\n"+code+main)
   subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()

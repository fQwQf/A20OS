"""Run the real player against a padded mock fbdev and RGB24 input stream."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class VideoPlay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-videoplay-')
        work=Path(cls.temp.name)
        (work/'test.c').write_text(r'''
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <uapi/a20/framebuffer.h>
static uint8_t screen[4096], rgb[18]={255,0,0,0,255,0,0,0,255,255,255,255,0,0,0,128,64,32};
static size_t cursor, input_bytes;static int scenario,flushes;static uint64_t clock_ns;
static int fake_open(const char *path,int flags,...){(void)flags;return !strcmp(path,"/dev/fb0")?11:10;}
static int fake_close(int fd){(void)fd;return 0;}
static ssize_t fake_read(int fd,void *p,size_t count){
 (void)fd;if(cursor==input_bytes)return 0;
 if(count>input_bytes-cursor)count=input_bytes-cursor;
 memcpy(p,rgb+cursor,count);cursor+=count;return count;
}
static int fake_ioctl(int fd,unsigned long request,...){
 (void)fd;va_list a;va_start(a,request);void *p=va_arg(a,void *);va_end(a);
 if(request==FBIOGET_VSCREENINFO){
  struct fb_var_screeninfo *v=p;memset(v,0,sizeof(*v));v->xres=3;v->yres=2;v->bits_per_pixel=32;
  v->red=(struct fb_bitfield){scenario==1?0:16,8,0};v->green=(struct fb_bitfield){8,8,0};
  v->blue=(struct fb_bitfield){scenario==1?16:0,8,0};
  if(scenario==3)v->red.offset=8;
 }else if(request==FBIOGET_FSCREENINFO){
  struct fb_fix_screeninfo *f=p;memset(f,0,sizeof(*f));f->smem_len=4096;f->line_length=64;
 }else if(request==FBIO_FLUSH)flushes++;
 else return -1;
 return 0;
}
static void *fake_mmap(void *a,size_t n,int p,int f,int d,off_t o){
 (void)a;(void)p;(void)f;(void)d;(void)o;return n==4096?screen:MAP_FAILED;
}
static int fake_munmap(void *a,size_t b){(void)a;(void)b;return 0;}
static long fake_sysconf(int n){(void)n;return 4096;}
static int fake_clock(clockid_t id,struct timespec *t){(void)id;t->tv_sec=clock_ns/1000000000ULL;t->tv_nsec=clock_ns%1000000000ULL;return 0;}
static int fake_sleep(const struct timespec *t,struct timespec *r){(void)r;clock_ns+=(uint64_t)t->tv_sec*1000000000ULL+t->tv_nsec;return 0;}
#define main player_main
#define open fake_open
#define close fake_close
#define read fake_read
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define sysconf fake_sysconf
#define clock_gettime fake_clock
#define nanosleep fake_sleep
#include "user/cmds/core/videoplay.c"
#undef main
int run(int which){scenario=which;cursor=0;clock_ns=0;flushes=0;input_bytes=which==2?17:18;
 memset(screen,0xa5,sizeof(screen));
 char *args[]={"videoplay","input.rgb","3","2","30",NULL};
 return player_main(5,args);
}
unsigned pixel(unsigned offset){return offset<4096?screen[offset]:0;}
int flushed(void){return flushes;}
''')
        subprocess.run(['cc','-shared','-fPIC','-O1','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT),'-I',str(ROOT/'kernel/include'),str(work/'test.c'),
                        '-o',str(work/'test.so')],check=True,capture_output=True)
        cls.lib=ctypes.CDLL(str(work/'test.so'))
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_bgr_stride_and_padding(self):
        self.assertEqual(self.lib.run(0),0)
        self.assertEqual(self.lib.flushed(),1)
        self.assertEqual([self.lib.pixel(i) for i in range(12)],
                         [0,0,255,0,0,255,0,0,255,0,0,0])
        self.assertEqual(self.lib.pixel(12),0xa5)
        self.assertEqual([self.lib.pixel(i) for i in range(64,68)],[255,255,255,0])
    def test_rgb_order(self):
        self.assertEqual(self.lib.run(1),0)
        self.assertEqual([self.lib.pixel(i) for i in range(4)],[255,0,0,0])
    def test_truncated_frame_never_presented(self):
        self.assertEqual(self.lib.run(2),1)
        self.assertEqual(self.lib.flushed(),0)
        self.assertEqual(self.lib.pixel(0),0xa5)
    def test_overlapping_channels_rejected(self):
        self.assertEqual(self.lib.run(3),1)
        self.assertEqual(self.lib.flushed(),0)

if __name__=='__main__':unittest.main()

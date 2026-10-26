"""Drive the production ALSA adapter with the host's real libasound.

The backend is a byte-counting mock: this verifies Linux ioctl negotiation and
SDL3's empty-stream START -> avail -> WRITEI sequence, not physical playback.
When libasound is absent, the small production START test still runs.
"""
from pathlib import Path
import ctypes.util
import platform
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include "uapi/a20/audio.h"
#define DEV_CLASS_AUDIO 6
struct device;
typedef struct device device_t;
typedef struct { a20_audio_caps_t caps;a20_audio_format_t pcm_format;int(*read)(device_t*,void*,size_t);int(*write)(device_t*,const void*,size_t);int(*set_format)(device_t*,const a20_audio_format_t*);int(*tone)(device_t*,const a20_audio_tone_t*);int(*stop)(device_t*);int(*drain)(device_t*);int(*poll)(device_t*,short);int(*ioctl)(device_t*,unsigned long,void*);int(*close)(device_t*);} audio_dev_ops_t;
typedef struct {void*class_ops;} driver_t;
struct device {driver_t*drv;};
typedef struct {device_t*dev;} class_device_t;
struct vfile;
typedef struct {int(*read)(struct vfile*,char*,size_t);int(*write)(struct vfile*,const char*,size_t);int(*ioctl)(struct vfile*,unsigned long,void*);int(*close)(struct vfile*);} vfile_ops_t;
typedef struct vfile {void*priv;int flags;vfile_ops_t*ops;} vfile_t;
static unsigned written;
static int backend_write(device_t*d,const void*p,size_t n){(void)d;(void)p;written+=n;return n;}
static audio_dev_ops_t backend={.caps={1,A20_AUDIO_CAP_PCM,48000,48000},.pcm_format={48000,2,1},.write=backend_write};
static driver_t drv={&backend};static device_t dev={&drv};static class_device_t cdev={&dev};
static class_device_t*class_device_get_by_type(int t,unsigned i){(void)t;return i?NULL:&cdev;}
static void*kcalloc(size_t n,size_t z){return calloc(n,z);}static void kfree(void*p){free(p);}
static vfile_t*vfile_alloc(void){return calloc(1,sizeof(vfile_t));}static void vfile_free(vfile_t*p){free(p);}
static void vfile_ref_init(vfile_t*p,int n){(void)p;(void)n;}
static int anonfd_install_vfile(vfile_t*p,int n){(void)p;(void)n;return -1;}
static int copy_from_user(void*d,const void*s,size_t n){if(!s)return -EFAULT;memcpy(d,s,n);return 0;}
static int copy_to_user(void*d,const void*s,size_t n){if(!d)return -EFAULT;memcpy(d,s,n);return 0;}
static void*proc_scratch_buffer(size_t n){static void*p;static size_t z;if(n>z){p=realloc(p,n);z=n;}return p;}
static void timekeeping_get_monotonic(uint64_t n[2]){n[0]=1;n[1]=0;}
static void timekeeping_get_realtime(uint64_t n[2]){n[0]=1;n[1]=0;}

'''

PROBE = r'''

static vfile_t *files[1024];
static unsigned calls;
int open64(const char*p,int flags,...){
 int fd=syscall(SYS_openat,AT_FDCWD,"/dev/null",flags,0);
 if(!strncmp(p,"/dev/snd/",9))files[fd]=strstr(p,"control")?alsa_control_create_vfile():alsa_pcm_create_vfile(1);
 else {syscall(SYS_close,fd);va_list ap;va_start(ap,flags);int mode=(flags&O_CREAT)?va_arg(ap,int):0;va_end(ap);fd=syscall(SYS_openat,AT_FDCWD,p,flags,mode);}
 return fd;
}
int open(const char*p,int flags,...){return open64(p,flags,0);}
int __open_2(const char*p,int flags){return open64(p,flags,0);}
int access(const char*p,int flags){if(!strncmp(p,"/dev/snd/",9))return 0;return syscall(SYS_access,p,flags);}
int ioctl(int fd,unsigned long req,...){
 va_list ap;va_start(ap,req);void*arg=va_arg(ap,void*);va_end(ap);
 if(fd>=0&&fd<1024&&files[fd]){
  int r=files[fd]->ops->ioctl(files[fd],req,arg);
  if(calls++<80)fprintf(stderr,"ioctl %lx -> %d\n",req,r);
  if(r<0){errno=-r;return -1;}return r;
 }
 return syscall(SYS_ioctl,fd,req,arg);
}
void*mmap64(void*p,size_t n,int prot,int flags,int fd,off_t offset){
 if(fd>=0&&fd<1024&&files[fd]){errno=ENODEV;return MAP_FAILED;}
 return (void*)syscall(SYS_mmap,p,n,prot,flags,fd,offset);
}
void*mmap(void*p,size_t n,int prot,int flags,int fd,off_t offset){return mmap64(p,n,prot,flags,fd,offset);}
int close(int fd){if(fd>=0&&fd<1024&&files[fd]){files[fd]->ops->close(files[fd]);vfile_free(files[fd]);files[fd]=NULL;}return syscall(SYS_close,fd);}
int snd_pcm_hw_open(void**,const char*,int,int,int,int,int,int,int);
int snd_pcm_hw_params_malloc(void**);
int snd_pcm_hw_params_any(void*,void*);
int snd_pcm_hw_params_set_access(void*,void*,int);
int snd_pcm_hw_params_set_format(void*,void*,int);
int snd_pcm_hw_params_set_channels(void*,void*,unsigned);
int snd_pcm_hw_params_set_rate_near(void*,void*,unsigned*,int*);
int snd_pcm_hw_params_set_period_size_near(void*,void*,unsigned long*,int*);
int snd_pcm_hw_params_set_buffer_size_near(void*,void*,unsigned long*);
int snd_pcm_hw_params(void*,void*);
int snd_pcm_prepare(void*);
int snd_pcm_start(void*);
long snd_pcm_avail(void*);
long snd_pcm_writei(void*,const void*,unsigned long);
int main(void){void*pcm,*params;int r,dir=0;unsigned rate=48000;unsigned long period=2048,buffer=8192;
#define CALL(x) do{fprintf(stderr,"call %s\n",#x);r=(x);fprintf(stderr,"result %d\n",r);if(r<0)return 1;}while(0)
 CALL(snd_pcm_hw_open(&pcm,"a20",0,0,-1,0,1,0,1));
 CALL(snd_pcm_hw_params_malloc(&params));CALL(snd_pcm_hw_params_any(pcm,params));
 CALL(snd_pcm_hw_params_set_access(pcm,params,3));CALL(snd_pcm_hw_params_set_format(pcm,params,2));CALL(snd_pcm_hw_params_set_channels(pcm,params,2));CALL(snd_pcm_hw_params_set_rate_near(pcm,params,&rate,&dir));CALL(snd_pcm_hw_params_set_period_size_near(pcm,params,&period,&dir));CALL(snd_pcm_hw_params_set_buffer_size_near(pcm,params,&buffer));CALL(snd_pcm_hw_params(pcm,params));CALL(snd_pcm_prepare(pcm));r=snd_pcm_start(pcm);assert(r==-EPIPE);CALL(snd_pcm_avail(pcm));int16_t samples[4096]={0};CALL(snd_pcm_writei(pcm,samples,2048));assert(written==8192);return 0;}

'''


class AlsaLibProtocol(unittest.TestCase):
    def test_real_libasound_sdl3_startup(self):
        library = ctypes.util.find_library("asound")
        if not library or platform.system() != "Linux":
            self.skipTest("host libasound is unavailable")
        source = ROOT / "kernel/drivers/audio/alsa.c"
        with tempfile.TemporaryDirectory(prefix="a20-alsa-lib-") as tmp:
            work = Path(tmp)
            stubs = work / "stubs"
            for name in re.findall(r'^#include "([^"]+)"', source.read_text(), re.M):
                path = stubs / name
                path.parent.mkdir(parents=True, exist_ok=True)
                if name == "drivers/audio/alsa.h":
                    shutil.copyfile(ROOT / "kernel/include" / name, path)
                else:
                    path.write_text("")
            for name in ("core/types.h", "uapi/a20/audio.h"):
                path = stubs / name
                path.parent.mkdir(parents=True, exist_ok=True)
                if name.startswith("uapi/"):
                    shutil.copyfile(ROOT / "kernel/include" / name, path)
                else:
                    path.write_text("")
            probe = work / "probe.c"
            probe.write_text(PRELUDE + '\n#include "' + str(source) + '"\n' + PROBE)
            exe = work / "probe"
            subprocess.run(["cc", "-O0", "-g", "-rdynamic", "-fsanitize=undefined",
                            "-I" + str(stubs), str(probe), "-l:" + library,
                            "-o", str(exe)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], timeout=10, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_start_is_rejected_without_state_change(self):
        source = (ROOT / "kernel/drivers/audio/alsa.c").read_text()
        start = source.index("    case SNDRV_PCM_IOCTL_START:")
        end = source.index("    case SNDRV_PCM_IOCTL_DROP:", start)
        body = source[start:end]
        self.assertLess(body.index("return -EPIPE"), body.index("ctx->state ="))
        self.assertIn("ctx->state != SNDRV_PCM_STATE_PREPARED", body)


if __name__ == "__main__":
    unittest.main()

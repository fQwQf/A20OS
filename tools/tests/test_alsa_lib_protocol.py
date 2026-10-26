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
#include <poll.h>
#include "uapi/a20/audio.h"
#include "drivers/audio/backend.h"
#define DEV_CLASS_AUDIO 6
struct device;
typedef struct device device_t;
typedef struct { a20_audio_caps_t caps;a20_audio_format_t pcm_format;int(*read)(device_t*,void*,size_t);int(*write)(device_t*,const void*,size_t);int(*set_format)(device_t*,const a20_audio_format_t*);int(*tone)(device_t*,const a20_audio_tone_t*);int(*stop)(device_t*);int(*drain)(device_t*);int(*poll)(device_t*,short);int(*ioctl)(device_t*,unsigned long,void*);int(*close)(device_t*);} audio_dev_ops_t;
typedef struct {void*class_ops;} driver_t;
struct device {driver_t*drv;};
typedef struct {device_t*dev;} class_device_t;
struct vfile;
typedef struct {int(*read)(struct vfile*,char*,size_t);int(*write)(struct vfile*,const char*,size_t);int(*ioctl)(struct vfile*,unsigned long,void*);int(*close)(struct vfile*);int(*poll)(struct vfile*,short);} vfile_ops_t;
typedef struct vfile {void*priv;int flags;vfile_ops_t*ops;} vfile_t;
static unsigned written,queued,running,drains,refs,active,device_lock;
static int backend_write(device_t*d,const void*p,size_t n){(void)d;(void)p;(void)n;abort();}
static int backend_stop(device_t*d){(void)d;queued=running=0;return 0;}
static int backend_drain(device_t*d){(void)d;queued=running=0;drains++;return 0;}
static int backend_ioctl(device_t*d,unsigned long req,void*arg){
 (void)d;assert(active);
 if(req==AUDIO_BACKEND_QUERY){*(audio_backend_status_t*)arg=(audio_backend_status_t){65536,queued,4,running,0};return 0;}
 if(req==AUDIO_BACKEND_START){if(!queued)return -EPIPE;running=1;return 0;}
 if(req==AUDIO_BACKEND_WRITE_NONBLOCK){
  audio_backend_write_t*w=arg;size_t n=w->bytes<65536-queued?w->bytes:65536-queued;
  if(!n)return -EAGAIN;queued+=n;written+=n;
  if(w->start_after_bytes&&n>=w->start_after_bytes)running=1;
  return (int)n;
 }
 return -ENOTTY;
}
static audio_dev_ops_t backend={.caps={1,A20_AUDIO_CAP_PCM,48000,48000},.pcm_format={48000,2,1},.write=backend_write,.ioctl=backend_ioctl,.stop=backend_stop,.drain=backend_drain};
static driver_t drv={&backend};static device_t dev={&drv};static class_device_t cdev={&dev};
static class_device_t*class_device_get_by_type(int t,unsigned i){(void)t;if(i)return NULL;refs++;return &cdev;}
static int class_device_ops_begin(class_device_t*c,uint32_t t,const void**p){(void)t;assert(c==&cdev&&refs);active++;*p=&backend;return 0;}
static void class_device_call_end(class_device_t*c){assert(c==&cdev&&active);active--;}
static void class_device_put(class_device_t*c){assert(c==&cdev&&refs);refs--;}
typedef struct{int held;}mutex_t;
static void mutex_init(mutex_t*m){m->held=0;}
static int mutex_trylock(mutex_t*m){if(m->held)return 0;m->held=1;return 1;}
static void mutex_lock(mutex_t*m){assert(mutex_trylock(m));}
static void mutex_unlock(mutex_t*m){assert(m->held);m->held=0;}
typedef struct{void*task;}proc_wait_token_t;
typedef int proc_wake_reason_t;
#define PROC_WAIT_INTERRUPTIBLE 1
#define MS_TO_TICKS(x) (x)
#define ERESTARTSYS 512
static uint64_t timer_get_ticks(void){return 1;}
static proc_wait_token_t proc_park_prepare(int f,uint64_t n){(void)f;(void)n;abort();}
static proc_wake_reason_t proc_park_commit(proc_wait_token_t t){(void)t;abort();}
static void proc_park_finish(proc_wait_token_t t){(void)t;abort();}
static int proc_wake_reason_is_task_interrupt(proc_wake_reason_t r){(void)r;return 0;}
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
int poll(struct pollfd*f,nfds_t n,int timeout){
 assert(!device_lock); /* SDL3 WaitDevice is outside its application mutex. */
 int owned=0;
 for(nfds_t i=0;i<n;i++)if(f[i].fd>=0&&f[i].fd<1024&&files[f[i].fd])owned=1;
 if(!owned)return syscall(SYS_poll,f,n,timeout);
 for(unsigned tick=0;tick<100;tick++){
  int ready=0;
  for(nfds_t i=0;i<n;i++){
   vfile_t*vf=f[i].fd>=0&&f[i].fd<1024?files[f[i].fd]:NULL;
   f[i].revents=vf&&vf->ops->poll?vf->ops->poll(vf,f[i].events):0;
   if(f[i].revents)ready++;
  }
  if(ready||timeout==0)return ready;
  /* Hardware retirement happens while the application's poll is waiting. */
  if(running&&queued)queued-=queued<1024?queued:1024;
 }
 return 0;
}
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
int snd_pcm_hw_params_set_period_time_near(void*,void*,unsigned*,int*);
int snd_pcm_hw_params_set_periods_min(void*,void*,unsigned*,int*);
int snd_pcm_hw_params_set_periods_first(void*,void*,unsigned*,int*);
int snd_pcm_hw_params_set_periods(void*,void*,unsigned,int);
int snd_pcm_hw_params_set_buffer_size_near(void*,void*,unsigned long*);
int snd_pcm_hw_params_get_buffer_size(const void*,unsigned long*);
int snd_pcm_hw_params(void*,void*);
int snd_pcm_prepare(void*);
int snd_pcm_start(void*);
int snd_pcm_wait(void*,int);
int snd_pcm_delay(void*,long*);
int snd_pcm_close(void*);
long snd_pcm_avail(void*);
long snd_pcm_writei(void*,const void*,unsigned long);
static void constraints_regression(void){
 struct snd_interval i={.min=16,.max=32,.openmin=1};
 struct snd_interval limit={.min=16,.max=100,.integer=1};
 assert(!hw_params_interval_intersect(&i,&limit)&&i.min==17&&i.max==32&&!i.openmin);
 i=(struct snd_interval){.min=16,.max=32,.openmax=1};
 assert(!hw_params_interval_intersect(&i,&limit)&&i.min==16&&i.max==31&&!i.openmax);
 i=(struct snd_interval){.min=16,.max=16,.openmin=1};
 assert(hw_params_interval_intersect(&i,&limit)==-EINVAL);
 struct snd_pcm_hw_params h;
#define ANY() do{memset(&h,0,sizeof(h));hw_params_fill_supported(&h,&backend);}while(0)
#define PIN(p,v) hw_params_interval_set(&h.intervals[HW_PARAM_INT_IDX(p)],v,v)
 ANY();PIN(SNDRV_PCM_HW_PARAM_PERIOD_SIZE,2048);PIN(SNDRV_PCM_HW_PARAM_PERIODS,2);
 assert(!hw_params_refine_constraints(&h,&backend));
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)].min==4096);
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)].max==4096);
 struct snd_interval*time=&h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_TIME)];
 assert(time->min==42666&&time->max==42667&&time->openmin&&time->openmax&&!time->integer);
 PIN(SNDRV_PCM_HW_PARAM_PERIOD_TIME,42667);
 assert(hw_params_refine_constraints(&h,&backend)==-EINVAL);
 ANY();PIN(SNDRV_PCM_HW_PARAM_PERIOD_BYTES,4096);
 assert(!hw_params_refine_constraints(&h,&backend));
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].min==1024);
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].max==1024);
 ANY();PIN(SNDRV_PCM_HW_PARAM_BUFFER_BYTES,16384);PIN(SNDRV_PCM_HW_PARAM_PERIODS,2);
 assert(!hw_params_refine_constraints(&h,&backend));
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)].min==4096);
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].min==2048);
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].max==2048);
 ANY();PIN(SNDRV_PCM_HW_PARAM_PERIOD_SIZE,2048);PIN(SNDRV_PCM_HW_PARAM_PERIODS,3);
 PIN(SNDRV_PCM_HW_PARAM_BUFFER_SIZE,4096);
 assert(hw_params_refine_constraints(&h,&backend)==-EINVAL);
 ANY();PIN(SNDRV_PCM_HW_PARAM_PERIOD_SIZE,2048);PIN(SNDRV_PCM_HW_PARAM_PERIOD_TIME,10000);
 assert(hw_params_refine_constraints(&h,&backend)==-EINVAL);
 ANY();PIN(SNDRV_PCM_HW_PARAM_PERIOD_TIME,10000);PIN(SNDRV_PCM_HW_PARAM_PERIODS,2);
 assert(!hw_params_refine_constraints(&h,&backend));
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].min==480);
 assert(h.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)].max==960);
}
int main(int argc,char**argv){void*pcm,*params;int r,dir=0;unsigned rate=48000,periods=2;unsigned long period=2048,buffer=8192;
 constraints_regression();
 int scenario=argc>1?atoi(argv[1]):0;
#define CALL(x) do{fprintf(stderr,"call %s\n",#x);r=(x);fprintf(stderr,"result %d\n",r);if(r<0)return 1;}while(0)
 CALL(snd_pcm_hw_open(&pcm,"a20",0,0,-1,0,1,0,1));
 CALL(snd_pcm_hw_params_malloc(&params));CALL(snd_pcm_hw_params_any(pcm,params));
 CALL(snd_pcm_hw_params_set_access(pcm,params,3));
 CALL(snd_pcm_hw_params_set_format(pcm,params,2));
 CALL(snd_pcm_hw_params_set_channels(pcm,params,2));
 CALL(snd_pcm_hw_params_set_rate_near(pcm,params,&rate,&dir));
 if(scenario==3){
  unsigned time=10000;CALL(snd_pcm_hw_params_set_period_time_near(pcm,params,&time,&dir));
  assert(time==10000);period=480;
 }else{
  if(scenario==2)period=1024;
  CALL(snd_pcm_hw_params_set_period_size_near(pcm,params,&period,&dir));
 }
 if(!scenario){
  CALL(snd_pcm_hw_params_set_buffer_size_near(pcm,params,&buffer));
 }else if(scenario==2){
  CALL(snd_pcm_hw_params_set_periods(pcm,params,3,0));
 }else{
  /* SDL3 3.2.26 ALSA_pcm_cfg(): no explicit buffer-size request. */
  CALL(snd_pcm_hw_params_set_periods_min(pcm,params,&periods,NULL));
  CALL(snd_pcm_hw_params_set_periods_first(pcm,params,&periods,NULL));
  assert(periods==2);
 }
 CALL(snd_pcm_hw_params(pcm,params));
 CALL(snd_pcm_hw_params_get_buffer_size(params,&buffer));
 unsigned long expected=!scenario?8192:scenario==2?3072:period*2;
 fprintf(stderr,"NEGOTIATED period=%lu buffer=%lu expected=%lu\n",period,buffer,expected);
 assert(buffer==expected);
 CALL(snd_pcm_prepare(pcm));r=snd_pcm_start(pcm);assert(r==-EPIPE);
 CALL(snd_pcm_avail(pcm));int16_t samples[4096]={0};
 device_lock=1;CALL(snd_pcm_writei(pcm,samples,period));device_lock=0;
 assert(written==period*4&&!drains);
 if(scenario==1){
  device_lock=1;CALL(snd_pcm_writei(pcm,samples,period));device_lock=0;
  assert(snd_pcm_avail(pcm)==0);
  vfile_t*vf=NULL;for(unsigned fd=0;fd<1024;fd++)if(files[fd]&&files[fd]->priv)vf=files[fd];
  assert(vf);vf->flags|=O_NONBLOCK;
  struct snd_xferi transfer={.buf=samples,.frames=period};
  assert(alsa_pcm_ioctl(vf,SNDRV_PCM_IOCTL_WRITEI_FRAMES,&transfer)==-EAGAIN);
  long delay;CALL(snd_pcm_delay(pcm,&delay));assert(delay==4096);
  /* The real library waits on actual capacity outside SDL's device lock. */
  assert(snd_pcm_wait(pcm,100)==1);
  assert(snd_pcm_avail(pcm)>=2048);
  CALL(snd_pcm_delay(pcm,&delay));assert(delay<=2048);
 }
 CALL(snd_pcm_close(pcm));assert(!refs&&!active&&!drains);return 0;}

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
                if name in ("drivers/audio/alsa.h", "drivers/audio/backend.h"):
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
            for scenario in range(4):
                with self.subTest(scenario=scenario):
                    result = subprocess.run([str(exe), str(scenario)], timeout=10,
                                            capture_output=True, text=True)
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

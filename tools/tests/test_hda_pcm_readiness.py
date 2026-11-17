"""Production HDA nonblocking PCM adapter against a register/ownership model.

No scheduler wait is permitted in a query or nonblocking submission. Real
LPIB movement retires queued bytes; polling never invents hardware progress.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?[^\n]*\b" + name +
                      r"\([^{}]*\)\s*\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match.group()


MODEL = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "uapi/a20/audio.h"
#include "drivers/audio/backend.h"
#define POLLOUT 4
#define POLLERR 8
#define MS_TO_TICKS(ms) (ms)
typedef struct {int held;} mutex_t;
typedef struct {int held;} spinlock_t;
static int mutex_trylock(mutex_t*m){if(m->held)return 0;m->held=1;return 1;}
static void mutex_unlock(mutex_t*m){assert(m->held);m->held=0;}
static uint64_t spin_lock_irqsave(spinlock_t*l){assert(!l->held);l->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t*l,uint64_t f){(void)f;assert(l->held);l->held=0;}
#include "drivers/audio/hda_internal.h"
typedef struct {void *drv_priv;} device_t;
static unsigned stopped,prepared,syncs;
struct driver_work { int pending; };
static struct driver_work progress_work;
static uint64_t timer_get_ticks(void){return 100;}
static int driver_work_schedule(struct driver_work*w,uint64_t deadline){assert(w&&deadline);w->pending=1;return 1;}
static uint32_t mmio[64];
static uint32_t readl(const volatile void*p){return *(const volatile uint32_t*)p;}
static uint8_t readb(const volatile void*p){return *(const volatile uint8_t*)p;}
static void writel(uint32_t v,volatile void*p){*(volatile uint32_t*)p=v;}
static void writeb(uint8_t v,volatile void*p){*(volatile uint8_t*)p&=~v;}
static void dma_sync_for_device(void*p,size_t n){(void)p;(void)n;syncs++;}
static volatile void*hda_stream_reg(hda_controller_t*h,uint32_t o){return (void*)(h->stream+o);}
/* The model's stop/reset acknowledge immediately. The production helpers
 * under test still determine all queue, register and capacity decisions. */
static int hda_stream_stop(hda_controller_t*h){h->stream_running=0;h->queued_bytes=0;h->write_pos=h->last_lpib=0;stopped++;return 0;}
static int hda_stream_prepare(hda_controller_t*h){assert(!h->queued_bytes&&!h->stream_running);h->write_pos=h->last_lpib=0;prepared++;return 0;}
static int hda_adopt_generation(hda_controller_t*h,uint32_t g){assert(g==h->generation);return 0;}
typedef struct audio_dev_ops {
 a20_audio_caps_t caps;
 a20_audio_format_t pcm_format;
 int(*stop)(device_t*);
 int(*drain)(device_t*);
 int(*set_format)(device_t*,const a20_audio_format_t*);
 int(*tone)(device_t*,const a20_audio_tone_t*);
 int(*ioctl)(device_t*,unsigned long,void*);
} audio_dev_ops_t;
static int copy_from_user(void*d,const void*s,size_t n){(void)d;(void)s;(void)n;abort();}
static int copy_to_user(void*d,const void*s,size_t n){(void)d;(void)s;(void)n;abort();}
static int user_forward(device_t*d,unsigned long r,void*p){(void)d;(void)r;(void)p;abort();}
'''

MAIN = r'''
int main(void){
 uint8_t pcm[HDA_DMA_BYTES],data[HDA_DMA_BYTES];memset(data,0x5a,sizeof(data));
 hda_controller_t h={.stream=(uintptr_t)mmio,.pcm=pcm,.progress_work=&progress_work};device_t d={&h};
 audio_dev_ops_t user_ops={.ioctl=user_forward};
 /* Real user-facing dispatcher must reject private requests before touching
  * this deliberately invalid pointer or calling the driver's callback. */
 assert(audio_device_ioctl(&d,&user_ops,AUDIO_BACKEND_QUERY,(void*)1)==-ENOTTY);
 assert(audio_device_ioctl(&d,&user_ops,AUDIO_BACKEND_START,(void*)1)==-ENOTTY);
 assert(audio_device_ioctl(&d,&user_ops,AUDIO_BACKEND_WRITE_NONBLOCK,(void*)1)==-ENOTTY);
 audio_backend_status_t status;
 audio_backend_write_t write={.data=data,.bytes=8192};
 assert(!hda_backend_ioctl(&d,AUDIO_BACKEND_QUERY,&status));
 assert(status.capacity_bytes==65536&&!status.queued_bytes&&!status.running);
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_START,NULL)==-EPIPE);
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==8192);
 assert(h.queued_bytes==8192&&!h.stream_running&&prepared==1);
 assert(!hda_backend_ioctl(&d,AUDIO_BACKEND_START,NULL)&&h.stream_running);
 assert(readl((void*)(h.stream+HDA_SD_CTL))&HDA_SD_CTL_RUN);
 assert(!hda_backend_ioctl(&d,AUDIO_BACKEND_QUERY,&status)&&status.queued_bytes==8192);
 /* Only a hardware LPIB transition makes bytes available. */
 mmio[HDA_SD_LPIB/4]=4096;
 assert(!hda_backend_ioctl(&d,AUDIO_BACKEND_QUERY,&status)&&status.queued_bytes==4096);
 write.bytes=sizeof(data);
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==61440);
 assert(h.queued_bytes==65536&&!hda_poll(&d,POLLOUT));
 write.bytes=4;
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==-EAGAIN);
 mmio[HDA_SD_LPIB/4]=8192;
 assert(hda_poll(&d,POLLOUT)==POLLOUT&&h.queued_bytes==61440);
 /* A stale query cannot overwrite space filled by another submission. */
 assert(!hda_backend_ioctl(&d,AUDIO_BACKEND_QUERY,&status));
 write.bytes=4092;assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==4092);
 write.bytes=8192;assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==4);
 assert(h.queued_bytes==65536);
 h.lock.held=1;
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_QUERY,&status)==-EAGAIN);
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==-EAGAIN);
 assert(!hda_poll(&d,POLLOUT));h.lock.held=0;
 write.bytes=3;assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==-EINVAL);
 write.bytes=4;h.pending_len=1;
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==-EINVAL);h.pending_len=0;
 assert(hda_backend_ioctl(&d,0x1234,&write)==-ENOTTY);
 assert(!stopped&&syncs>0&&h.stream_starts==1&&h.stream_underruns==0);
 /* The first prepared chunk can start in the same critical section, so an
  * EAGAIN between enqueue and START can never cause duplicate submission. */
 assert(!hda_stream_stop(&h));mmio[HDA_SD_LPIB/4]=0;
 write.bytes=4096;write.start_after_bytes=4;
 assert(hda_backend_ioctl(&d,AUDIO_BACKEND_WRITE_NONBLOCK,&write)==4096);
 assert(h.stream_running&&h.queued_bytes==4096&&h.stream_starts==2);
 return 0;
}
'''


class HdaPcmReadiness(unittest.TestCase):
    def test_production_nonblocking_capacity_and_cursor(self):
        source = (ROOT / "kernel/drvmod/examples/hda.c").read_text()
        defines = "\n".join(re.findall(r"^#define HDA_.*$", source, re.M))
        bodies = "\n".join(function(source, name) for name in (
            "hda_stream_run", "hda_stream_refresh", "hda_ring_copy",
            "hda_backend_ioctl", "hda_poll"))
        core = (ROOT / "kernel/drivers/audio/audio_core.c").read_text()
        bodies += "\n" + function(core, "audio_format_equal")
        bodies += "\n" + function(core, "audio_device_ioctl")
        with tempfile.TemporaryDirectory(prefix="a20-hda-ready-") as tmp:
            work = Path(tmp)
            (work / "core").mkdir()
            (work / "core/sync.h").write_text("")
            (work / "core/types.h").write_text("")
            path = work / "model.c"
            path.write_text(MODEL + defines + "\n" + bodies + MAIN)
            exe = work / "model"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-I" + str(work),
                            "-I" + str(ROOT / "kernel/include"),
                            "-I" + str(ROOT / "kernel"), str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_internal_requests_cannot_reach_userspace_pointer_forwarding(self):
        source = (ROOT / "kernel/drivers/audio/audio_core.c").read_text()
        body = function(source, "audio_device_ioctl")
        for name in ("AUDIO_BACKEND_QUERY", "AUDIO_BACKEND_START",
                     "AUDIO_BACKEND_WRITE_NONBLOCK"):
            self.assertLess(body.index(name), body.index("return ops->ioctl"))
        self.assertIn("return -ENOTTY", body[:body.index("A20_AUDIO_IOCTL_STOP")])


if __name__ == "__main__":
    unittest.main()

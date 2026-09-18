#include "drivers/audio/alsa.h"

#include "core/errno.h"
#include "core/string.h"
#include "drivers/audio/audio_core.h"
#include "drivers/core/driver_class.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * Minimal ALSA-compatible PCM/control backend.
 *
 * The ALSA PCM ioctl flow that ALSA userland (aplay, alsa-lib) uses is:
 *   HW_PARAMS (describe/set format+rate+period) -> SW_PARAMS (start/avail
 *   thresholds) -> PREPARE -> WRITEI_FRAMES (transfer) -> START -> DRAIN.
 *
 * We map that onto the A20 audio_dev_ops_t:
 *   - HW_PARAMS records the negotiated format/rate/period size.
 *   - WRITEI_FRAMES copies user audio into the backend via ops->write.
 *   - START/DRAIN map to ops->start/drain (start via set_format then write).
 */

#define SNDRV_PCM_STATE_OPEN 0
#define SNDRV_PCM_STATE_SETUP 1
#define SNDRV_PCM_STATE_PREPARED 2
#define SNDRV_PCM_STATE_RUNNING 3
#define SNDRV_PCM_STATE_XRUN 4
#define SNDRV_PCM_STATE_DRAINING 5
#define SNDRV_PCM_STATE_PAUSED 6

#define SNDRV_PCM_FORMAT_S16_LE 2
#define SNDRV_PCM_FORMAT_U8 1
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 3
#define SNDRV_PCM_SUBFORMAT_STD 0

#define SNDRV_PCM_HW_PARAM_ACCESS 0
#define SNDRV_PCM_HW_PARAM_FORMAT 1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT 2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK 0
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS 8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS 9
#define SNDRV_PCM_HW_PARAM_CHANNELS 10
#define SNDRV_PCM_HW_PARAM_RATE 11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME 12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE 13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS 15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME 16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE 17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME 19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL 8
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL 19

#define HW_PARAM_MASK_IDX(p) ((p) - SNDRV_PCM_HW_PARAM_FIRST_MASK)
#define HW_PARAM_INT_IDX(p) ((p) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL)

#define SNDRV_PCM_INFO_BLOCK_TRANSFER 0x00010000

typedef struct {
    int playback;
    int state;
    uint32_t format;
    uint32_t rate;
    uint32_t channels;
    uint32_t period_size;
    uint32_t buffer_size;
    uint64_t hw_ptr;
} alsa_pcm_ctx_t;

/* ALSA wire structs (Linux ABI, 64-bit). */
struct snd_mask { uint32_t bits[8]; };
struct snd_interval {
    unsigned int min, max;
    unsigned int openmin:1, openmax:1, integer:1, empty:1;
};
struct snd_pcm_hw_params {
    unsigned int flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    unsigned int rmask;
    unsigned int cmask;
    unsigned int info;
    unsigned int msbits;
    unsigned int rate_num;
    unsigned int rate_den;
    uint64_t fifo_size;
    unsigned char sync[16];
    unsigned char reserved[48];
};
struct snd_pcm_sw_params {
    int tstamp_mode;
    unsigned int period_step;
    unsigned int sleep_min;
    uint64_t avail_min;
    uint64_t xfer_align;
    uint64_t start_threshold;
    uint64_t stop_threshold;
    uint64_t silence_threshold;
    uint64_t silence_size;
    uint64_t boundary;
    unsigned int proto;
    unsigned int tstamp_type;
    unsigned char reserved[56];
};
struct snd_timespec { int64_t sec, nsec; };
struct snd_pcm_status {
    int state;
    int pad;
    struct snd_timespec trigger_tstamp;
    struct snd_timespec tstamp;
    uint64_t appl_ptr;
    uint64_t hw_ptr;
    int64_t delay;
    uint64_t avail;
    uint64_t avail_max;
    uint64_t overrange;
    int suspended_state;
    unsigned int audio_tstamp_data;
    struct snd_timespec audio_tstamp;
    struct snd_timespec driver_tstamp;
    unsigned int audio_tstamp_accuracy;
    unsigned char reserved[20];
};
struct snd_xferi {
    int64_t result;
    void *buf;
    uint64_t frames;
};
struct snd_ctl_card_info {
    unsigned int card, pad;
    unsigned char card_[16], driver[16], shortname[32], longname[80];
    unsigned char mixer_name[80], components[128], reserved[64];
};
struct snd_pcm_info {
    unsigned int device, subdevice;
    int stream, card;
    unsigned char id[64], name[80], subname[32];
    unsigned int dev_class, dev_subclass, subdevices_count, subdevices_avail;
    unsigned char pad1[16];
    unsigned char reserved[64];
};

/* Find the A20 audio device + ops. */
static int alsa_audio_get(device_t **dev_out, audio_dev_ops_t **ops_out)
{
    *dev_out = NULL;
    *ops_out = NULL;
    class_device_t *cdev = class_device_get_by_type(DEV_CLASS_AUDIO, 0);
    if (!cdev || !cdev->dev)
        return -ENODEV;
    *dev_out = cdev->dev;
    *ops_out = (audio_dev_ops_t *)cdev->dev->drv->class_ops;
    return 0;
}

/* ---- PCM ioctls ---- */

static void hw_params_mask_set(struct snd_mask *mask, unsigned int value)
{
    memset(mask, 0, sizeof(*mask));
    mask->bits[value / 32] |= 1u << (value % 32);
}

static void hw_params_interval_set(struct snd_interval *itv, unsigned int lo,
                                   unsigned int hi)
{
    memset(itv, 0, sizeof(*itv));
    itv->min = lo;
    itv->max = hi;
    itv->integer = 1;
}

static void hw_params_fill_supported(struct snd_pcm_hw_params *hp,
                                     audio_dev_ops_t *ops)
{
    unsigned int rate_min = ops->caps.min_rate ? ops->caps.min_rate : 8000;
    unsigned int rate_max = ops->caps.max_rate ? ops->caps.max_rate : 48000;

    hw_params_mask_set(&hp->masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                       SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_params_mask_set(&hp->masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                       SNDRV_PCM_FORMAT_S16_LE);
    hw_params_mask_set(&hp->masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_SUBFORMAT)],
                       SNDRV_PCM_SUBFORMAT_STD);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_SAMPLE_BITS)], 16, 16);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_FRAME_BITS)], 16, 32);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)], 1, 2);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)], rate_min, rate_max);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_TIME)], 0, 0xffffffffu);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)], 16, 1u << 20);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_BYTES)], 16, 1u << 20);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIODS)], 2, 1024);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_TIME)], 0, 0xffffffffu);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)], 16, 1u << 22);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_BYTES)], 16, 1u << 22);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_TICK_TIME)], 0, 0xffffffffu);

    hp->flags = 0;
    hp->rmask = 0x000fffff;
    hp->cmask = 0x000fffff;
    hp->info = SNDRV_PCM_INFO_BLOCK_TRANSFER;
    hp->msbits = 16;
    hp->fifo_size = 0;
}

static unsigned int hw_params_clamp(unsigned int value, unsigned int lo,
                                    unsigned int hi, unsigned int fallback)
{
    if (value == 0)
        value = fallback;
    if (value < lo)
        value = lo;
    if (value > hi)
        value = hi;
    return value;
}

static int alsa_pcm_hw_refine(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_pcm_hw_params hp;
    device_t *dev;
    audio_dev_ops_t *ops;
    int r;

    (void)ctx;
    if (copy_from_user(&hp, arg, sizeof(hp)) < 0)
        return -EFAULT;
    r = alsa_audio_get(&dev, &ops);
    if (r < 0)
        return r;
    if (!(ops->caps.flags & A20_AUDIO_CAP_PCM))
        return -EOPNOTSUPP;
    hw_params_fill_supported(&hp, ops);
    return copy_to_user(arg, &hp, sizeof(hp)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_info(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_pcm_info info;

    memset(&info, 0, sizeof(info));
    memcpy(info.id, "A20PCM", 6);
    memcpy(info.name, "A20OS Audio", 11);
    memcpy(info.subname, "A20 PCM", 7);
    info.stream = ctx->playback ? 0 : 1;
    info.subdevices_count = 1;
    info.subdevices_avail = 1;
    return copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_hw_params(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_pcm_hw_params hp;
    if (copy_from_user(&hp, arg, sizeof(hp)) < 0)
        return -EFAULT;

    device_t *dev;
    audio_dev_ops_t *ops;
    int r = alsa_audio_get(&dev, &ops);
    if (r < 0)
        return r;
    if (!(ops->caps.flags & A20_AUDIO_CAP_PCM))
        return -EOPNOTSUPP;

    /* Negotiate format/rate/channels from the user's requested values. */
    unsigned int want_channels =
        hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)].min;
    unsigned int want_rate = hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)].min;
    unsigned int want_period =
        hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)].min;
    unsigned int rate_min = ops->caps.min_rate ? ops->caps.min_rate : 8000;
    unsigned int rate_max = ops->caps.max_rate ? ops->caps.max_rate : 48000;

    hw_params_fill_supported(&hp, ops);

    ctx->format = SNDRV_PCM_FORMAT_S16_LE;
    ctx->channels = hw_params_clamp(want_channels, 1, 2, 2);
    ctx->rate = hw_params_clamp(want_rate, rate_min, rate_max, 48000);
    ctx->period_size = hw_params_clamp(want_period, 16, 1u << 20, 1024);
    ctx->buffer_size = ctx->period_size;

    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)],
                           ctx->channels, ctx->channels);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)],
                           ctx->rate, ctx->rate);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)],
                           ctx->period_size, ctx->period_size);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIODS)], 2, 2);

    /* Configure the A20 backend. */
    a20_audio_format_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.rate = ctx->rate;
    fmt.channels = (uint16_t)ctx->channels;
    fmt.format = A20_AUDIO_FORMAT_S16_LE;
    if (ops->set_format) {
        r = ops->set_format(dev, &fmt);
        if (r < 0)
            return r;
    }

    ctx->state = SNDRV_PCM_STATE_SETUP;
    return copy_to_user(arg, &hp, sizeof(hp)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_sw_params(alsa_pcm_ctx_t *ctx, void *arg)
{
    (void)ctx;
    struct snd_pcm_sw_params sp;
    if (copy_from_user(&sp, arg, sizeof(sp)) < 0)
        return -EFAULT;
    /* Accept and store thresholds; playback is block-transfer driven. */
    ctx->state = SNDRV_PCM_STATE_SETUP;
    return copy_to_user(arg, &sp, sizeof(sp)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_status(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_pcm_status st;
    memset(&st, 0, sizeof(st));
    st.state = ctx->state;
    st.hw_ptr = ctx->hw_ptr;
    st.appl_ptr = ctx->hw_ptr;
    st.avail = ctx->buffer_size;
    st.avail_max = ctx->buffer_size;
    return copy_to_user(arg, &st, sizeof(st)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_writei(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_xferi x;
    if (copy_from_user(&x, arg, sizeof(x)) < 0)
        return -EFAULT;
    if (!x.buf || x.frames == 0) {
        x.result = 0;
        return copy_to_user(arg, &x, sizeof(x)) < 0 ? -EFAULT : 0;
    }

    device_t *dev;
    audio_dev_ops_t *ops;
    int r = alsa_audio_get(&dev, &ops);
    if (r < 0)
        return r;
    if (!ops->write)
        return -EOPNOTSUPP;

    size_t frame_bytes = ctx->channels * (ctx->format == SNDRV_PCM_FORMAT_S16_LE ? 2 : 1);
    size_t bytes = (size_t)x.frames * frame_bytes;
    char *kbuf = proc_scratch_buffer(bytes ? bytes : 1);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, x.buf, bytes) < 0)
        return -EFAULT;
    int n = ops->write(dev, kbuf, bytes);
    if (n < 0)
        return n;
    ctx->hw_ptr += x.frames;
    x.result = (int64_t)x.frames;
    return copy_to_user(arg, &x, sizeof(x)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_readi(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_xferi x;
    if (copy_from_user(&x, arg, sizeof(x)) < 0)
        return -EFAULT;
    if (!x.buf || x.frames == 0) {
        x.result = 0;
        return copy_to_user(arg, &x, sizeof(x)) < 0 ? -EFAULT : 0;
    }
    device_t *dev;
    audio_dev_ops_t *ops;
    int r = alsa_audio_get(&dev, &ops);
    if (r < 0)
        return r;
    if (!ops->read)
        return -EOPNOTSUPP;
    size_t frame_bytes = ctx->channels * (ctx->format == SNDRV_PCM_FORMAT_S16_LE ? 2 : 1);
    size_t bytes = (size_t)x.frames * frame_bytes;
    char *kbuf = proc_scratch_buffer(bytes ? bytes : 1);
    if (!kbuf)
        return -ENOMEM;
    int n = ops->read(dev, kbuf, bytes);
    if (n < 0)
        return n;
    if (n > 0 && copy_to_user(x.buf, kbuf, (size_t)n) < 0)
        return -EFAULT;
    ctx->hw_ptr += (uint64_t)(n / (frame_bytes ? frame_bytes : 1));
    x.result = (int64_t)(n / (frame_bytes ? frame_bytes : 1));
    return copy_to_user(arg, &x, sizeof(x)) < 0 ? -EFAULT : 0;
}

static int alsa_pcm_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    alsa_pcm_ctx_t *ctx = vf ? vf->priv : NULL;
    if (!ctx)
        return -EBADF;

    switch (req) {
    case SNDRV_PCM_IOCTL_PVERSION: {
        int version = SNDRV_PCM_VERSION;
        return copy_to_user(arg, &version, sizeof(version)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_PCM_IOCTL_INFO:
        return alsa_pcm_info(ctx, arg);
    case SNDRV_PCM_IOCTL_HW_REFINE:
        return alsa_pcm_hw_refine(ctx, arg);
    case SNDRV_PCM_IOCTL_HW_PARAMS:
        return alsa_pcm_hw_params(ctx, arg);
    case SNDRV_PCM_IOCTL_SW_PARAMS:
        return alsa_pcm_sw_params(ctx, arg);
    case SNDRV_PCM_IOCTL_STATUS:
        return alsa_pcm_status(ctx, arg);
    case SNDRV_PCM_IOCTL_WRITEI_FRAMES:
        if (!ctx->playback)
            return -EBADF;
        return alsa_pcm_writei(ctx, arg);
    case SNDRV_PCM_IOCTL_READI_FRAMES:
        if (ctx->playback)
            return -EBADF;
        return alsa_pcm_readi(ctx, arg);
    case SNDRV_PCM_IOCTL_PREPARE:
    case SNDRV_PCM_IOCTL_RESET:
        ctx->state = SNDRV_PCM_STATE_PREPARED;
        return 0;
    case SNDRV_PCM_IOCTL_START:
        ctx->state = SNDRV_PCM_STATE_RUNNING;
        return 0;
    case SNDRV_PCM_IOCTL_DROP:
    case SNDRV_PCM_IOCTL_HW_FREE:
        ctx->state = SNDRV_PCM_STATE_OPEN;
        return 0;
    case SNDRV_PCM_IOCTL_DRAIN:
        ctx->state = SNDRV_PCM_STATE_DRAINING;
        return 0;
    case SNDRV_PCM_IOCTL_PAUSE: {
        int pause = 0;
        if (copy_from_user(&pause, arg, sizeof(pause)) < 0)
            return -EFAULT;
        ctx->state = pause ? SNDRV_PCM_STATE_PAUSED : SNDRV_PCM_STATE_RUNNING;
        return 0;
    }
    case SNDRV_PCM_IOCTL_TSTAMP: {
        int mode = 0;
        return copy_to_user(arg, &mode, sizeof(mode)) < 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

static int alsa_pcm_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    return 0;
}

static int alsa_pcm_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    return (int)count;
}

static int alsa_pcm_close(vfile_t *vf)
{
    alsa_pcm_ctx_t *ctx = vf ? vf->priv : NULL;
    if (ctx) {
        kfree(ctx);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_alsa_pcm_ops = {
    .read = alsa_pcm_read,
    .write = alsa_pcm_write,
    .ioctl = alsa_pcm_ioctl,
    .close = alsa_pcm_close,
};

/* ---- control ioctls ---- */

static int alsa_control_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    (void)vf;
    switch (req) {
    case SNDRV_CTL_IOCTL_PVERSION: {
        int ver = 0x010106; /* ALSA 1.1.6 */
        return copy_to_user(arg, &ver, sizeof(ver)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_CARD_INFO: {
        struct snd_ctl_card_info info;
        memset(&info, 0, sizeof(info));
        info.card = 0;
        strncpy((char *)info.card_, "a20", sizeof(info.card_) - 1);
        strncpy((char *)info.driver, "a20sound", sizeof(info.driver) - 1);
        strncpy((char *)info.shortname, "A20OS Audio", sizeof(info.shortname) - 1);
        strncpy((char *)info.longname, "A20OS Audio Device", sizeof(info.longname) - 1);
        return copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE: {
        int dev = 0;
        if (copy_from_user(&dev, arg, sizeof(dev)) < 0)
            return -EFAULT;
        device_t *d;
        audio_dev_ops_t *o;
        if (alsa_audio_get(&d, &o) < 0)
            return -ENODEV;
        dev = (dev == 0) ? 0 : -1;
        return copy_to_user(arg, &dev, sizeof(dev)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_PCM_INFO: {
        struct snd_pcm_info info;
        memset(&info, 0, sizeof(info));
        info.device = 0;
        info.subdevice = 0;
        info.stream = 0;
        info.card = 0;
        strncpy((char *)info.id, "a20pcm", sizeof(info.id) - 1);
        strncpy((char *)info.name, "A20OS PCM", sizeof(info.name) - 1);
        info.subdevices_count = 1;
        info.subdevices_avail = 1;
        return copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

static vfile_ops_t g_alsa_ctl_ops = {
    .read = alsa_pcm_read,
    .write = alsa_pcm_write,
    .ioctl = alsa_control_ioctl,
    .close = alsa_pcm_close,
};

int alsa_pcm_create_file(int playback)
{
    vfile_t *vf = alsa_pcm_create_vfile(playback);
    if (!vf)
        return -ENOMEM;
    return anonfd_install_vfile(vf, 0);
}

vfile_t *alsa_pcm_create_vfile(int playback)
{
    alsa_pcm_ctx_t *ctx = kcalloc(1, sizeof(*ctx));
    vfile_t *vf = vfile_alloc();
    if (!ctx || !vf) {
        if (ctx) kfree(ctx);
        if (vf) vfile_free(vf);
        return NULL;
    }
    ctx->playback = playback;
    ctx->state = SNDRV_PCM_STATE_OPEN;
    vfile_ref_init(vf, 1);
    vf->flags = playback ? O_WRONLY : O_RDONLY;
    vf->ops = &g_alsa_pcm_ops;
    vf->priv = ctx;
    return vf;
}

int alsa_control_create_file(void)
{
    vfile_t *vf = alsa_control_create_vfile();
    if (!vf)
        return -ENOMEM;
    return anonfd_install_vfile(vf, 0);
}

vfile_t *alsa_control_create_vfile(void)
{
    vfile_t *vf = vfile_alloc();
    if (!vf)
        return NULL;
    vfile_ref_init(vf, 1);
    vf->flags = O_RDWR;
    vf->ops = &g_alsa_ctl_ops;
    return vf;
}

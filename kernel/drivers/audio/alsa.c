#include "drivers/audio/alsa.h"

#include "core/errno.h"
#include "core/timekeeping.h"
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
    int tstamp_type;
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
/* Linux UAPI layout (sound/asound.h).  alsa-lib encodes the struct size in
 * the ioctl number and reads fields by offset, so reserved_ is ABI. */
struct snd_ctl_card_info {
    int card;
    int pad;
    unsigned char id[16];
    unsigned char driver[16];
    unsigned char name[32];
    unsigned char longname[80];
    unsigned char reserved_[16];
    unsigned char mixername[80];
    unsigned char components[128];
};
struct snd_pcm_info {
    unsigned int device, subdevice;
    int stream, card;
    unsigned char id[64], name[80], subname[32];
    unsigned int dev_class, dev_subclass, subdevices_count, subdevices_avail;
    unsigned char pad1[16];
    unsigned char reserved[64];
};
struct snd_pcm_mmap_status {
    int state;
    int pad1;
    uint64_t hw_ptr;
    struct snd_timespec tstamp;
    int suspended_state;
    uint32_t pad3;
    struct snd_timespec audio_tstamp;
};
struct snd_pcm_mmap_control {
    uint64_t appl_ptr;
    uint64_t avail_min;
};
struct snd_pcm_sync_ptr {
    uint32_t flags;
    union {
        struct snd_pcm_mmap_status status;
        uint8_t reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        uint8_t reserved[64];
    } c;
};

/* Find the A20 audio device + ops. */
static int alsa_audio_get(device_t **dev_out, audio_dev_ops_t **ops_out)
{
    *dev_out = NULL;
    *ops_out = NULL;
    for (unsigned i = 0; i < 16; i++) {
        class_device_t *cdev = class_device_get_by_type(DEV_CLASS_AUDIO, i);
        if (!cdev || !cdev->dev)
            break;
        audio_dev_ops_t *ops = (audio_dev_ops_t *)cdev->dev->drv->class_ops;
        if (ops && (ops->caps.flags & A20_AUDIO_CAP_PCM)) {
            *dev_out = cdev->dev;
            *ops_out = ops;
            return 0;
        }
    }
    return -ENODEV;
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
    unsigned int channels_min = 1, channels_max = 2;
    if (ops->pcm_format.rate)
        rate_min = rate_max = ops->pcm_format.rate;
    if (ops->pcm_format.channels)
        channels_min = channels_max = ops->pcm_format.channels;

    hw_params_mask_set(&hp->masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                       SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_params_mask_set(&hp->masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                       SNDRV_PCM_FORMAT_S16_LE);
    hw_params_mask_set(&hp->masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_SUBFORMAT)],
                       SNDRV_PCM_SUBFORMAT_STD);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_SAMPLE_BITS)], 16, 16);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_FRAME_BITS)], 16 * channels_min, 16 * channels_max);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)], channels_min, channels_max);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)], rate_min, rate_max);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_TIME)], 0, 0xffffffffu);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)], 16, 1u << 20);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_BYTES)], 16, 1u << 20);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIODS)], 2, 1024);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_TIME)], 0, 0xffffffffu);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)], 16, 1u << 22);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_BYTES)], 16, 1u << 22);
    hw_params_interval_set(&hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_TICK_TIME)], 0, 0);
    /* Durations are rational microseconds, unlike integral frame counts. */
    hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_TIME)].integer = 0;
    hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_TIME)].integer = 0;

    hp->flags = 0;
    hp->rmask = 0x000fffff;
    hp->cmask = 0x000fffff;
    hp->info = SNDRV_PCM_INFO_BLOCK_TRANSFER;
    hp->msbits = 16;
    hp->fifo_size = 0;
}

static int hw_params_mask_refine(struct snd_mask *mask,
                                 const struct snd_mask *limit)
{
    unsigned int any = 0;
    for (unsigned i = 0; i < 8; i++) {
        mask->bits[i] &= limit->bits[i];
        any |= mask->bits[i];
    }
    return any ? 0 : -EINVAL;
}

static int hw_params_interval_intersect(struct snd_interval *itv,
                                        const struct snd_interval *limit)
{
    if (itv->empty || limit->empty)
        return -EINVAL;
    if (itv->min < limit->min) {
        itv->min = limit->min;
        itv->openmin = limit->openmin;
    } else if (itv->min == limit->min) {
        itv->openmin |= limit->openmin;
    }
    if (itv->max > limit->max) {
        itv->max = limit->max;
        itv->openmax = limit->openmax;
    } else if (itv->max == limit->max) {
        itv->openmax |= limit->openmax;
    }
    itv->integer |= limit->integer;
    if (itv->integer) {
        if ((itv->openmin && itv->min == 0xffffffffU) ||
            (itv->openmax && !itv->max))
            return -EINVAL;
        itv->min += itv->openmin;
        itv->max -= itv->openmax;
        itv->openmin = itv->openmax = 0;
    }
    if (itv->min > itv->max ||
        (itv->min == itv->max && (itv->openmin || itv->openmax)))
        return -EINVAL;
    return 0;
}

static uint64_t hw_params_div_ceil(uint64_t n, uint64_t d)
{
    return n / d + (n % d != 0);
}

static int hw_params_interval_bound(struct snd_interval *itv,
                                    uint64_t lo, uint64_t hi)
{
    if (lo > 0xffffffffU || lo > hi)
        return -EINVAL;
    struct snd_interval limit = {
        .min = (unsigned int)lo,
        .max = hi > 0xffffffffU ? 0xffffffffU : (unsigned int)hi,
        .integer = 1,
    };
    return hw_params_interval_intersect(itv, &limit);
}

/* Positive integer relation c = a*b/div. Propagate both directions, keeping
 * every application's exact/near constraint; no client-specific geometry. */
static int hw_params_product(struct snd_pcm_hw_params *hp,
                              unsigned a_id, unsigned b_id, unsigned c_id,
                              unsigned div)
{
    struct snd_interval *a = &hp->intervals[HW_PARAM_INT_IDX(a_id)];
    struct snd_interval *b = &hp->intervals[HW_PARAM_INT_IDX(b_id)];
    struct snd_interval *c = &hp->intervals[HW_PARAM_INT_IDX(c_id)];
    if (!a->min || !b->min)
        return -EINVAL;
    if (hw_params_interval_bound(c,
            hw_params_div_ceil((uint64_t)a->min * b->min, div),
            (uint64_t)a->max * b->max / div) < 0 ||
        hw_params_interval_bound(a,
            hw_params_div_ceil((uint64_t)c->min * div, b->max),
            (uint64_t)c->max * div / b->min) < 0 ||
        hw_params_interval_bound(b,
            hw_params_div_ceil((uint64_t)c->min * div, a->max),
            (uint64_t)c->max * div / a->min) < 0)
        return -EINVAL;
    return 0;
}

static int hw_params_duration(struct snd_pcm_hw_params *hp,
                               unsigned size_id, unsigned time_id)
{
    struct snd_interval *rate = &hp->intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)];
    struct snd_interval *size = &hp->intervals[HW_PARAM_INT_IDX(size_id)];
    struct snd_interval *time = &hp->intervals[HW_PARAM_INT_IDX(time_id)];
    uint64_t min = (uint64_t)size->min * 1000000U;
    uint64_t max = (uint64_t)size->max * 1000000U;
    struct snd_interval limit = {
        .min = (unsigned int)(min / rate->max),
        .max = hw_params_div_ceil(max, rate->min) > 0xffffffffU ?
                    0xffffffffU : (unsigned int)hw_params_div_ceil(max, rate->min),
        .openmin = min % rate->max != 0,
        .openmax = max % rate->min != 0 && max / rate->min < 0xffffffffU,
    };
    if (min / rate->max > 0xffffffffU ||
        hw_params_interval_intersect(time, &limit) < 0)
        return -EINVAL;
    min = (uint64_t)time->min * rate->min;
    max = (uint64_t)time->max * rate->max;
    return hw_params_interval_bound(size,
        hw_params_div_ceil(min, 1000000U) +
            (time->openmin && min % 1000000U == 0),
        max / 1000000U - (time->openmax && max % 1000000U == 0));
}

static int hw_params_refine_constraints(struct snd_pcm_hw_params *hp,
                                         audio_dev_ops_t *ops)
{
    struct snd_pcm_hw_params supported;
    memset(&supported, 0, sizeof(supported));
    hw_params_fill_supported(&supported, ops);
    for (unsigned i = 0; i < 3; i++)
        if (hw_params_mask_refine(&hp->masks[i], &supported.masks[i]) < 0)
            return -EINVAL;
    for (unsigned i = 0; i < 12; i++)
        if (hw_params_interval_intersect(&hp->intervals[i],
                                         &supported.intervals[i]) < 0)
            return -EINVAL;
    /* Monotonic interval contraction. All products fit in 64 bits because
     * each wire operand is 32 bits. Refuse malformed nonconvergence. */
    for (unsigned pass = 0; pass < 64; pass++) {
        struct snd_interval before[12];
        memcpy(before, hp->intervals, sizeof(before));
        if (hw_params_product(hp, SNDRV_PCM_HW_PARAM_SAMPLE_BITS,
                SNDRV_PCM_HW_PARAM_CHANNELS, SNDRV_PCM_HW_PARAM_FRAME_BITS, 1) < 0 ||
            hw_params_product(hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                SNDRV_PCM_HW_PARAM_FRAME_BITS, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 8) < 0 ||
            hw_params_product(hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
                SNDRV_PCM_HW_PARAM_FRAME_BITS, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 8) < 0 ||
            hw_params_product(hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                SNDRV_PCM_HW_PARAM_PERIODS, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 1) < 0 ||
            hw_params_duration(hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                SNDRV_PCM_HW_PARAM_PERIOD_TIME) < 0 ||
            hw_params_duration(hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
                SNDRV_PCM_HW_PARAM_BUFFER_TIME) < 0)
            return -EINVAL;
        if (memcmp(before, hp->intervals, sizeof(before)) == 0)
            return 0;
    }
    return -EINVAL;
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

    /* Refine must intersect the application's constraints with what the
     * hardware supports; replacing them (as an earlier version did) silently
     * widens an exact channel count back to a range and alsa-lib then rejects
     * the object. */
    r = hw_params_refine_constraints(&hp, ops);
    if (r < 0)
        return r;
    hp.flags = 0;
    hp.rmask = 0;
    hp.cmask = 0;
    hp.info = SNDRV_PCM_INFO_BLOCK_TRANSFER;
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

static int hw_params_interval_allows(const struct snd_interval *itv,
                                     unsigned int value)
{
    if (itv->empty)
        return 0;
    if (value < itv->min || value > itv->max)
        return 0;
    if (itv->openmin && value == itv->min)
        return 0;
    if (itv->openmax && value == itv->max)
        return 0;
    return 1;
}

static int hw_params_mask_has(const struct snd_mask *mask, unsigned int value)
{
    return !!(mask->bits[value / 32] & (1u << (value % 32)));
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

    r = hw_params_refine_constraints(&hp, ops);
    if (r < 0)
        return r;

    if (!hw_params_mask_has(&hp.masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                            SNDRV_PCM_ACCESS_RW_INTERLEAVED) ||
        !hw_params_mask_has(&hp.masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                            SNDRV_PCM_FORMAT_S16_LE))
        return -EINVAL;

    const struct snd_interval *ch_itv =
        &hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)];
    const struct snd_interval *rate_itv =
        &hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)];
    const struct snd_interval *period_itv =
        &hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)];
    const struct snd_interval *buffer_itv =
        &hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)];
    /* Every negotiated parameter must become a single value inside the range
     * the application offered; alsa-lib validates the returned params and
     * rejects a range where it asked for a value. */
    unsigned int channels = ch_itv->min;
    unsigned int rate = rate_itv->min;
    unsigned int period = period_itv->min;
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)], channels, channels);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)], rate, rate);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)], period, period);
    if (hw_params_refine_constraints(&hp, ops) < 0)
        return -EINVAL;
    uint64_t buffer_candidate = hw_params_div_ceil(buffer_itv->min, period) * period;
    if (buffer_candidate > 0xffffffffU)
        return -EINVAL;
    unsigned int buffer = (unsigned int)buffer_candidate;
    if (!hw_params_interval_allows(buffer_itv, buffer))
        return -EINVAL;
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)], buffer, buffer);
    if (hw_params_refine_constraints(&hp, ops) < 0)
        return -EINVAL;

    ctx->format = SNDRV_PCM_FORMAT_S16_LE;
    ctx->channels = channels;
    ctx->rate = rate;
    ctx->period_size = period;
    ctx->buffer_size = buffer;

    hw_params_mask_set(&hp.masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_ACCESS)],
                       SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_params_mask_set(&hp.masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_FORMAT)],
                       SNDRV_PCM_FORMAT_S16_LE);
    hw_params_mask_set(&hp.masks[HW_PARAM_MASK_IDX(SNDRV_PCM_HW_PARAM_SUBFORMAT)],
                       SNDRV_PCM_SUBFORMAT_STD);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_SAMPLE_BITS)], 16, 16);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_FRAME_BITS)],
                           16 * channels, 16 * channels);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_CHANNELS)],
                           channels, channels);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_RATE)],
                           rate, rate);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE)],
                           period, period);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIOD_BYTES)],
                           period * channels * 2, period * channels * 2);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_PERIODS)],
                           buffer / period, buffer / period);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_SIZE)],
                           buffer, buffer);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_BUFFER_BYTES)],
                           buffer * channels * 2, buffer * channels * 2);
    hw_params_interval_set(&hp.intervals[HW_PARAM_INT_IDX(SNDRV_PCM_HW_PARAM_TICK_TIME)], 0, 0);
    hp.flags = 0;
    hp.rmask = 0;
    hp.cmask = 0;
    hp.info = SNDRV_PCM_INFO_BLOCK_TRANSFER;
    hp.msbits = 16;
    hp.rate_num = rate;
    hp.rate_den = 1;
    hp.fifo_size = 0;

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

static void alsa_pcm_now(const alsa_pcm_ctx_t *ctx, struct snd_timespec *ts)
{
    uint64_t now[2];
    if (ctx->tstamp_type == 0)
        timekeeping_get_realtime(now);
    else
        timekeeping_get_monotonic(now);
    ts->sec = (int64_t)now[0];
    ts->nsec = (int64_t)now[1];
}

static int alsa_pcm_status(alsa_pcm_ctx_t *ctx, void *arg)
{
    struct snd_pcm_status st;
    memset(&st, 0, sizeof(st));
    st.state = ctx->state;
    alsa_pcm_now(ctx, &st.trigger_tstamp);
    st.tstamp = st.trigger_tstamp;
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
        /* Linux pcm_native.c:snd_pcm_pre_start rejects an empty playback
         * stream without changing PREPARED. SDL3 deliberately attempts this
         * once in ALSA_ThreadInit before its first write. Claiming RUNNING
         * here makes alsa-lib detect an immediate underrun and prevents that
         * first buffer from ever reaching the hardware. */
        if (ctx->state != SNDRV_PCM_STATE_PREPARED)
            return -EBADFD;
        if (ctx->playback && ctx->hw_ptr == 0)
            return -EPIPE;
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
    case SNDRV_PCM_IOCTL_TTSTAMP: {
        int type;
        if (copy_from_user(&type, arg, sizeof(type)) < 0)
            return -EFAULT;
        if (type < 0 || type > 2)
            return -EINVAL;
        ctx->tstamp_type = type;
        return 0;
    }
    case SNDRV_PCM_IOCTL_USER_PVERSION: {
        /* alsa-lib announces the UAPI version it was built against; it is
         * advisory, and rejecting it fails every snd_pcm_open. */
        unsigned int version;
        if (copy_from_user(&version, arg, sizeof(version)) < 0)
            return -EFAULT;
        return 0;
    }
    case SNDRV_PCM_IOCTL_SYNC_PTR: {
        struct snd_pcm_sync_ptr sp;
        if (copy_from_user(&sp, arg, sizeof(sp)) < 0)
            return -EFAULT;
        sp.s.status.state = ctx->state;
        sp.s.status.hw_ptr = ctx->hw_ptr;
        sp.s.status.suspended_state = 0;
        alsa_pcm_now(ctx, &sp.s.status.tstamp);
        sp.s.status.audio_tstamp = sp.s.status.tstamp;
        sp.c.control.appl_ptr = ctx->hw_ptr;
        sp.c.control.avail_min = ctx->buffer_size;
        return copy_to_user(arg, &sp, sizeof(sp)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_PCM_IOCTL_STATUS_EXT:
        return alsa_pcm_status(ctx, arg);
    case SNDRV_PCM_IOCTL_DELAY: {
        int64_t delay = 0;
        return copy_to_user(arg, &delay, sizeof(delay)) < 0 ? -EFAULT : 0;
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
        int ver = SNDRV_CTL_VERSION;
        return copy_to_user(arg, &ver, sizeof(ver)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_CARD_INFO: {
        struct snd_ctl_card_info info;
        memset(&info, 0, sizeof(info));
        info.card = 0;
        strncpy((char *)info.id, "a20", sizeof(info.id) - 1);
        strncpy((char *)info.driver, "a20sound", sizeof(info.driver) - 1);
        strncpy((char *)info.name, "A20OS Audio", sizeof(info.name) - 1);
        strncpy((char *)info.longname, "A20OS Audio Device",
                sizeof(info.longname) - 1);
        return copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE: {
        int dev;
        if (copy_from_user(&dev, arg, sizeof(dev)) < 0)
            return -EFAULT;
        device_t *d;
        audio_dev_ops_t *o;
        if (alsa_audio_get(&d, &o) < 0)
            return -ENODEV;
        /* One PCM device: -1 starts the scan at 0, any non-negative index
         * ends it. */
        dev = dev < 0 ? 0 : -1;
        return copy_to_user(arg, &dev, sizeof(dev)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_PCM_INFO: {
        struct snd_pcm_info info;
        if (copy_from_user(&info, arg, sizeof(info)) < 0)
            return -EFAULT;
        if (info.device != 0)
            return -ENOENT;
        if (info.stream != 0 && info.stream != 1)
            return -EINVAL;
        info.subdevice = 0;
        info.card = 0;
        memset(info.id, 0, sizeof(info.id));
        memset(info.name, 0, sizeof(info.name));
        memset(info.subname, 0, sizeof(info.subname));
        strncpy((char *)info.id, "A20PCM", sizeof(info.id) - 1);
        strncpy((char *)info.name, "A20OS Audio", sizeof(info.name) - 1);
        strncpy((char *)info.subname, "A20 PCM", sizeof(info.subname) - 1);
        info.dev_class = 0;
        info.dev_subclass = 0;
        info.subdevices_count = 1;
        info.subdevices_avail = 1;
        return copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE: {
        int subdevice;
        if (copy_from_user(&subdevice, arg, sizeof(subdevice)) < 0)
            return -EFAULT;
        /* alsa-lib calls this with -1 ("no preference") before opening the
         * PCM; only a real subdevice beyond the single one we expose is
         * refused. */
        return subdevice < 0 || subdevice == 0 ? 0 : -ENODEV;
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

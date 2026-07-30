/*
 * Intel High Definition Audio PCM playback driver
 */
#include "drivers/audio/hda.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "uapi/a20/audio.h"

#define HDA_REG_GCAP       0x00U
#define HDA_REG_STATESTS   0x0eU
#define HDA_REG_GCTL       0x08U
#define HDA_REG_INTCTL     0x20U
#define HDA_REG_ICOI       0x60U
#define HDA_REG_ICII       0x64U
#define HDA_REG_ICIS       0x68U

#define HDA_GCTL_RESET       0x01U
#define HDA_ICIS_BUSY         0x01U
#define HDA_ICIS_VALID        0x02U

#define HDA_SD_CTL           0x00U
#define HDA_SD_STS           0x03U
#define HDA_SD_LPIB          0x04U
#define HDA_SD_CBL           0x08U
#define HDA_SD_LVI           0x0cU
#define HDA_SD_FORMAT        0x12U
#define HDA_SD_BDPL          0x18U
#define HDA_SD_BDPU          0x1cU
#define HDA_SD_CTL_SRST      0x01U
#define HDA_SD_CTL_RUN       0x02U
#define HDA_SD_STS_BCIS      0x04U
#define HDA_SD_STS_FIFO_ERR  0x08U
#define HDA_SD_STS_DESC_ERR  0x10U

#define HDA_PARAM_NODE_COUNT 0x04U
#define HDA_PARAM_FUNC_TYPE  0x05U
#define HDA_PARAM_AUDIO_CAPS 0x0aU
#define HDA_PARAM_STREAM_FMT 0x0bU
#define HDA_PARAM_PIN_CAPS   0x0cU
#define HDA_PARAM_INPUT_AMP  0x0dU
#define HDA_PARAM_CONN_LEN   0x0eU
#define HDA_PARAM_OUTPUT_AMP 0x12U
#define HDA_PARAM_WIDGET_CAP 0x09U
#define HDA_WIDGET_OUTPUT    0U
#define HDA_WIDGET_PIN       4U
#define HDA_PIN_CAP_OUTPUT   (1U << 4)
#define HDA_PIN_CAP_EAPD     (1U << 16)

#define HDA_VERB_GET_PARAM       0xf00U
#define HDA_VERB_GET_CONN        0xf02U
#define HDA_VERB_SET_CONN        0x701U
#define HDA_VERB_SET_POWER       0x705U
#define HDA_VERB_SET_STREAM      0x706U
#define HDA_VERB_SET_PIN_CTL     0x707U
#define HDA_VERB_SET_EAPD        0x70cU
#define HDA_VERB_GET_CONFIG      0xf1cU
#define HDA_VERB_SET_AMP         0x03U
#define HDA_VERB_SET_FORMAT      0x02U

#define HDA_PCM_RATE       48000U
#define HDA_PCM_CHANNELS   2U
#define HDA_PCM_FRAME      4U
#define HDA_PCM_FORMAT     0x0011U
#define HDA_DMA_BYTES      (64U * 1024U)
#define HDA_TIMEOUT_MS     1000U
#define HDA_MAX_NODES      64U

typedef struct __attribute__((packed)) hda_bdle {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} hda_bdle_t;

typedef struct hda_controller {
    uintptr_t regs;
    uintptr_t stream;
    hda_bdle_t *bdl;
    uint64_t bdl_dma;
    uint8_t *pcm;
    uint64_t pcm_dma;
    uint8_t codec;
    uint8_t afg;
    uint8_t dac;
    uint8_t pin;
    uint8_t path[HDA_MAX_NODES];
    uint8_t path_select[HDA_MAX_NODES];
    uint8_t path_len;
    uint32_t afg_pcm;
    uint32_t afg_formats;
    uint8_t pending[HDA_PCM_FRAME];
    uint8_t pending_len;
    uint32_t pending_generation;
    int codec_configured;
    int dma64;
    volatile uint32_t generation;
    spinlock_t state_lock;
    mutex_t lock;
} hda_controller_t;

static volatile void *hda_reg(hda_controller_t *hda, uint32_t offset)
{
    return (volatile void *)(hda->regs + offset);
}

static volatile void *hda_stream_reg(hda_controller_t *hda, uint32_t offset)
{
    return (volatile void *)(hda->stream + offset);
}

static int hda_wait32(hda_controller_t *hda, uint32_t reg,
                      uint32_t mask, uint32_t expected)
{
    for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
        if ((readl(hda_reg(hda, reg)) & mask) == expected)
            return 0;
        mdelay(1);
    }
    return -ETIMEDOUT;
}

static int hda_reset(hda_controller_t *hda)
{
    writel(readl(hda_reg(hda, HDA_REG_GCTL)) & ~HDA_GCTL_RESET,
           hda_reg(hda, HDA_REG_GCTL));
    if (hda_wait32(hda, HDA_REG_GCTL, HDA_GCTL_RESET, 0) < 0)
        return -ETIMEDOUT;
    mdelay(1);
    writel(readl(hda_reg(hda, HDA_REG_GCTL)) | HDA_GCTL_RESET,
           hda_reg(hda, HDA_REG_GCTL));
    if (hda_wait32(hda, HDA_REG_GCTL, HDA_GCTL_RESET,
                   HDA_GCTL_RESET) < 0)
        return -ETIMEDOUT;
    mdelay(1);
    return 0;
}

static int hda_command(hda_controller_t *hda, uint32_t command,
                       uint32_t *response)
{
    for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
        if (!(readw(hda_reg(hda, HDA_REG_ICIS)) & HDA_ICIS_BUSY))
            break;
        if (ms + 1U == HDA_TIMEOUT_MS)
            return -ETIMEDOUT;
        mdelay(1);
    }
    writew(HDA_ICIS_VALID, hda_reg(hda, HDA_REG_ICIS));
    writel(command, hda_reg(hda, HDA_REG_ICOI));
    writew(HDA_ICIS_BUSY, hda_reg(hda, HDA_REG_ICIS));
    for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
        if (readw(hda_reg(hda, HDA_REG_ICIS)) & HDA_ICIS_VALID) {
            if (response)
                *response = readl(hda_reg(hda, HDA_REG_ICII));
            writew(HDA_ICIS_VALID, hda_reg(hda, HDA_REG_ICIS));
            return 0;
        }
        mdelay(1);
    }
    return -ETIMEDOUT;
}

static uint32_t hda_cmd12(hda_controller_t *hda, uint8_t nid,
                          uint16_t verb, uint8_t payload, int *error)
{
    uint32_t response = 0;
    uint32_t command = ((uint32_t)hda->codec << 28) |
                       ((uint32_t)nid << 20) |
                       ((uint32_t)verb << 8) | payload;
    int ret = hda_command(hda, command, &response);
    if (error && ret < 0)
        *error = ret;
    return response;
}

static int hda_set16(hda_controller_t *hda, uint8_t nid,
                     uint8_t verb, uint16_t payload)
{
    uint32_t command = ((uint32_t)hda->codec << 28) |
                       ((uint32_t)nid << 20) |
                       ((uint32_t)verb << 16) | payload;
    return hda_command(hda, command, NULL);
}

static uint32_t hda_param(hda_controller_t *hda, uint8_t nid,
                          uint8_t param, int *error)
{
    return hda_cmd12(hda, nid, HDA_VERB_GET_PARAM, param, error);
}

static int hda_find_codec(hda_controller_t *hda)
{
    uint16_t codecs = readw(hda_reg(hda, HDA_REG_STATESTS));
    if (!codecs)
        return -ENODEV;
    for (uint8_t codec = 0; codec < 15; codec++) {
        if (!(codecs & (1U << codec)))
            continue;
        hda->codec = codec;
        int error = 0;
        uint32_t nodes = hda_param(hda, 0, HDA_PARAM_NODE_COUNT, &error);
        uint8_t start = (uint8_t)(nodes >> 16);
        uint8_t count = (uint8_t)nodes;
        for (uint8_t i = 0; !error && i < count; i++) {
            uint8_t nid = (uint8_t)(start + i);
            if ((hda_param(hda, nid, HDA_PARAM_FUNC_TYPE, &error) & 0xffU) == 1U) {
                hda->afg = nid;
                return 0;
            }
        }
        if (error)
            return error;
    }
    return -ENODEV;
}

static int hda_pin_priority(hda_controller_t *hda, uint8_t nid, int *error)
{
    uint32_t config = hda_cmd12(hda, nid, HDA_VERB_GET_CONFIG, 0, error);
    if ((config >> 30) == 1U)
        return 0;
    uint8_t device = (uint8_t)((config >> 20) & 0x0fU);
    if (device == 0U) return 3;
    if (device == 1U) return 2;
    if (device == 2U) return 1;
    return 1;
}

static int hda_connections(hda_controller_t *hda, uint8_t nid,
                           uint8_t *nodes, uint8_t *selectors,
                           int max_nodes, int *error)
{
    uint32_t info = hda_param(hda, nid, HDA_PARAM_CONN_LEN, error);
    uint8_t raw_count = (uint8_t)(info & 0x7fU);
    int long_form = (info & 0x80U) != 0;
    int count = 0;
    uint16_t previous = 0;
    for (uint8_t raw = 0; *error >= 0 && raw < raw_count; ) {
        uint32_t packed = hda_cmd12(hda, nid, HDA_VERB_GET_CONN, raw, error);
        uint8_t per_command = long_form ? 2U : 4U;
        for (uint8_t slot = 0; slot < per_command && raw < raw_count;
             slot++, raw++) {
            uint16_t entry = long_form ? (uint16_t)(packed >> (slot * 16U)) :
                                         (uint8_t)(packed >> (slot * 8U));
            uint16_t range_bit = long_form ? 0x8000U : 0x0080U;
            uint16_t value_mask = long_form ? 0x7fffU : 0x007fU;
            uint16_t value = entry & value_mask;
            uint16_t first = (entry & range_bit) ? (uint16_t)(previous + 1U) : value;
            for (uint16_t child = first; child <= value && count < max_nodes;
                 child++) {
                nodes[count] = (uint8_t)child;
                selectors[count] = (uint8_t)count;
                count++;
            }
            previous = value;
        }
    }
    return *error < 0 ? *error : count;
}

static int hda_find_path(hda_controller_t *hda, uint8_t nid,
                         uint8_t *visited, uint8_t depth, int *error)
{
    if (depth >= HDA_MAX_NODES || visited[nid])
        return 0;
    visited[nid] = 1;
    hda->path[depth] = nid;
    uint32_t caps = hda_param(hda, nid, HDA_PARAM_WIDGET_CAP, error);
    if (*error < 0)
        return 0;
    if (((caps >> 20) & 0x0fU) == HDA_WIDGET_OUTPUT) {
        uint32_t pcm = (caps & (1U << 4)) ?
                       hda_param(hda, nid, HDA_PARAM_AUDIO_CAPS, error) :
                       hda->afg_pcm;
        uint32_t formats = (caps & (1U << 4)) ?
                           hda_param(hda, nid, HDA_PARAM_STREAM_FMT, error) :
                           hda->afg_formats;
        if (*error >= 0 && (caps & 1U) && !(caps & (1U << 9)) &&
            (formats & 1U) && (pcm & (1U << 6)) && (pcm & (1U << 17))) {
            hda->dac = nid;
            hda->path_len = (uint8_t)(depth + 1U);
            return 1;
        }
        return 0;
    }
    uint8_t children[HDA_MAX_NODES];
    uint8_t selectors[HDA_MAX_NODES];
    int count = hda_connections(hda, nid, children, selectors,
                                HDA_MAX_NODES, error);
    for (int i = 0; *error >= 0 && i < count; i++) {
        if (hda_find_path(hda, children[i], visited,
                          (uint8_t)(depth + 1U), error)) {
            hda->path_select[depth] = selectors[i];
            return 1;
        }
    }
    return 0;
}

static int hda_find_widgets(hda_controller_t *hda)
{
    int error = 0;
    uint32_t node_info = hda_param(hda, hda->afg, HDA_PARAM_NODE_COUNT,
                                   &error);
    uint8_t start = (uint8_t)(node_info >> 16);
    uint8_t count = (uint8_t)node_info;
    if (count > HDA_MAX_NODES)
        count = HDA_MAX_NODES;
    hda->afg_pcm = hda_param(hda, hda->afg, HDA_PARAM_AUDIO_CAPS, &error);
    hda->afg_formats = hda_param(hda, hda->afg, HDA_PARAM_STREAM_FMT, &error);
    for (int priority = 3; !error && priority >= 1; priority--) {
        for (uint8_t i = 0; !error && i < count; i++) {
            uint8_t nid = (uint8_t)(start + i);
            uint32_t caps = hda_param(hda, nid, HDA_PARAM_WIDGET_CAP, &error);
            if (((caps >> 20) & 0x0fU) != HDA_WIDGET_PIN ||
                (caps & (1U << 9)))
                continue;
            uint32_t pin_caps = hda_param(hda, nid, HDA_PARAM_PIN_CAPS, &error);
            if (!(pin_caps & HDA_PIN_CAP_OUTPUT) ||
                hda_pin_priority(hda, nid, &error) != priority)
                continue;
            uint8_t visited[256];
            memset(visited, 0, sizeof(visited));
            hda->path_len = 0;
            if (hda_find_path(hda, nid, visited, 0, &error)) {
                hda->pin = nid;
                return 0;
            }
        }
    }
    return error < 0 ? error : -ENODEV;
}

static uint8_t hda_amp_zero_db(hda_controller_t *hda, uint8_t nid,
                               uint32_t widget_caps, int input, int *error)
{
    uint8_t target = (widget_caps & (1U << 3)) ? nid : hda->afg;
    uint8_t param = input ? HDA_PARAM_INPUT_AMP : HDA_PARAM_OUTPUT_AMP;
    return (uint8_t)(hda_param(hda, target, param, error) & 0x7fU);
}

static int hda_codec_setup(hda_controller_t *hda)
{
    int error = 0;
    hda_cmd12(hda, hda->afg, HDA_VERB_SET_POWER, 0, &error);
    for (uint8_t i = 0; !error && i < hda->path_len; i++) {
        uint8_t nid = hda->path[i];
        hda_cmd12(hda, nid, HDA_VERB_SET_POWER, 0, &error);
        uint32_t caps = hda_param(hda, nid, HDA_PARAM_WIDGET_CAP, &error);
        if (i + 1U < hda->path_len) {
            hda_cmd12(hda, nid, HDA_VERB_SET_CONN,
                      hda->path_select[i], &error);
            if (caps & (1U << 1)) {
                if (hda->path_select[i] > 15U)
                    return -EOPNOTSUPP;
                uint8_t gain = hda_amp_zero_db(hda, nid, caps, 1, &error);
                if (error < 0 || hda_set16(hda, nid, HDA_VERB_SET_AMP,
                    (uint16_t)(0x7000U | (hda->path_select[i] << 8) | gain)) < 0)
                    return -EIO;
            }
        }
        if (caps & (1U << 2)) {
            uint8_t gain = hda_amp_zero_db(hda, nid, caps, 0, &error);
            if (error < 0 || hda_set16(hda, nid, HDA_VERB_SET_AMP,
                                       (uint16_t)(0xb000U | gain)) < 0)
                return -EIO;
        }
    }
    if (error < 0)
        return error;
    hda_cmd12(hda, hda->pin, HDA_VERB_SET_PIN_CTL, 0x40U, &error);
    uint32_t pin_caps = hda_param(hda, hda->pin, HDA_PARAM_PIN_CAPS, &error);
    if (pin_caps & HDA_PIN_CAP_EAPD)
        hda_cmd12(hda, hda->pin, HDA_VERB_SET_EAPD, 0x02U, &error);
    if (hda_set16(hda, hda->dac, HDA_VERB_SET_FORMAT, HDA_PCM_FORMAT) < 0)
        return -EIO;
    hda_cmd12(hda, hda->dac, HDA_VERB_SET_STREAM, 0x10U, &error);
    if (!error)
        hda->codec_configured = 1;
    return error;
}

static int hda_stream_reset(hda_controller_t *hda)
{
    uint32_t ctl = readl(hda_stream_reg(hda, HDA_SD_CTL)) & 0x00ffffffU;
    writel(ctl & ~HDA_SD_CTL_RUN, hda_stream_reg(hda, HDA_SD_CTL));
    for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
        if (!(readb(hda_stream_reg(hda, HDA_SD_CTL)) & HDA_SD_CTL_RUN))
            goto stopped;
        mdelay(1);
    }
    return -ETIMEDOUT;

stopped:
    writeb(HDA_SD_STS_BCIS | HDA_SD_STS_FIFO_ERR | HDA_SD_STS_DESC_ERR,
           hda_stream_reg(hda, HDA_SD_STS));
    writel((ctl & ~HDA_SD_CTL_RUN) | HDA_SD_CTL_SRST,
           hda_stream_reg(hda, HDA_SD_CTL));
    for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
        if (readb(hda_stream_reg(hda, HDA_SD_CTL)) & HDA_SD_CTL_SRST)
            goto reset_asserted;
        mdelay(1);
    }
    return -ETIMEDOUT;

reset_asserted:
    writel(ctl & ~(HDA_SD_CTL_RUN | HDA_SD_CTL_SRST),
           hda_stream_reg(hda, HDA_SD_CTL));
    for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
        if (!(readb(hda_stream_reg(hda, HDA_SD_CTL)) & HDA_SD_CTL_SRST))
            return 0;
        mdelay(1);
    }
    return -ETIMEDOUT;
}

static int hda_play_chunk(hda_controller_t *hda, const uint8_t *data,
                          size_t bytes, uint32_t generation)
{
    if (!bytes || bytes > HDA_DMA_BYTES || (bytes % HDA_PCM_FRAME))
        return -EINVAL;
    if (data != hda->pcm)
        memcpy(hda->pcm, data, bytes);
    dma_sync_for_device(hda->pcm, bytes);
    hda->bdl[0].address = hda->pcm_dma;
    hda->bdl[0].length = (uint32_t)bytes;
    hda->bdl[0].flags = 1U;
    dma_sync_for_device(hda->bdl, sizeof(hda->bdl[0]));
    if (hda_stream_reset(hda) < 0)
        return -EIO;
    writel((uint32_t)hda->bdl_dma, hda_stream_reg(hda, HDA_SD_BDPL));
    writel((uint32_t)(hda->bdl_dma >> 32), hda_stream_reg(hda, HDA_SD_BDPU));
    writel((uint32_t)bytes, hda_stream_reg(hda, HDA_SD_CBL));
    writew(0, hda_stream_reg(hda, HDA_SD_LVI));
    writew(HDA_PCM_FORMAT, hda_stream_reg(hda, HDA_SD_FORMAT));
    uint64_t flags = spin_lock_irqsave(&hda->state_lock);
    if (__atomic_load_n(&hda->generation, __ATOMIC_ACQUIRE) != generation) {
        spin_unlock_irqrestore(&hda->state_lock, flags);
        return -EINTR;
    }
    uint32_t ctl = readl(hda_stream_reg(hda, HDA_SD_CTL)) & 0x000fffffU;
    writel(ctl | (1U << 20) | HDA_SD_CTL_RUN,
           hda_stream_reg(hda, HDA_SD_CTL));
    spin_unlock_irqrestore(&hda->state_lock, flags);

    uint64_t duration_ms = ((uint64_t)bytes * 1000U) /
                           (HDA_PCM_RATE * HDA_PCM_FRAME) + HDA_TIMEOUT_MS;
    int ret = -ETIMEDOUT;
    for (uint64_t ms = 0; ms < duration_ms; ms++) {
        if (__atomic_load_n(&hda->generation, __ATOMIC_ACQUIRE) != generation) {
            ret = -EINTR;
            break;
        }
        uint8_t status = readb(hda_stream_reg(hda, HDA_SD_STS));
        if (status & (HDA_SD_STS_FIFO_ERR | HDA_SD_STS_DESC_ERR)) {
            ret = -EIO;
            break;
        }
        if (status & HDA_SD_STS_BCIS) {
            ret = 0;
            break;
        }
        proc_yield();
    }
    writel(readl(hda_stream_reg(hda, HDA_SD_CTL)) & ~HDA_SD_CTL_RUN,
           hda_stream_reg(hda, HDA_SD_CTL));
    writeb(HDA_SD_STS_BCIS | HDA_SD_STS_FIFO_ERR | HDA_SD_STS_DESC_ERR,
           hda_stream_reg(hda, HDA_SD_STS));
    return ret;
}

static int hda_write(device_t *dev, const void *buf, size_t count)
{
    hda_controller_t *hda = dev ? dev->drv_priv : NULL;
    if (!hda || !buf || !count)
        return -EINVAL;
    mutex_lock(&hda->lock);
    uint32_t generation = __atomic_load_n(&hda->generation, __ATOMIC_ACQUIRE);
    if (hda->pending_generation != generation)
        hda->pending_len = 0;
    const uint8_t *input = buf;
    uint8_t pending[HDA_PCM_FRAME];
    uint8_t pending_len = hda->pending_len;
    memcpy(pending, hda->pending, pending_len);
    size_t consumed = 0;
    size_t output = 0;
    while (pending_len && pending_len < HDA_PCM_FRAME && consumed < count)
        pending[pending_len++] = input[consumed++];
    if (pending_len == HDA_PCM_FRAME) {
        memcpy(hda->pcm, pending, HDA_PCM_FRAME);
        output = HDA_PCM_FRAME;
        pending_len = 0;
    }
    size_t available = count - consumed;
    size_t full = available & ~(size_t)(HDA_PCM_FRAME - 1U);
    if (full > HDA_DMA_BYTES - output)
        full = (HDA_DMA_BYTES - output) & ~(size_t)(HDA_PCM_FRAME - 1U);
    if (full) {
        memcpy(hda->pcm + output, input + consumed, full);
        output += full;
        consumed += full;
    }
    while (consumed < count && pending_len < HDA_PCM_FRAME)
        pending[pending_len++] = input[consumed++];
    int ret = output ? hda_play_chunk(hda, hda->pcm, output, generation) : 0;
    if (ret == 0 &&
        __atomic_load_n(&hda->generation, __ATOMIC_ACQUIRE) == generation) {
        memcpy(hda->pending, pending, pending_len);
        hda->pending_len = pending_len;
        hda->pending_generation = generation;
    } else if (ret == 0) {
        ret = -EINTR;
    }
    mutex_unlock(&hda->lock);
    return ret < 0 ? ret : (int)consumed;
}

static int hda_ioctl(device_t *dev, unsigned long req, void *arg)
{
    hda_controller_t *hda = dev ? dev->drv_priv : NULL;
    if (!hda)
        return -ENODEV;
    if (req == A20_AUDIO_IOCTL_STOP) {
        uint64_t flags = spin_lock_irqsave(&hda->state_lock);
        __atomic_add_fetch(&hda->generation, 1U, __ATOMIC_ACQ_REL);
        writel(readl(hda_stream_reg(hda, HDA_SD_CTL)) & ~HDA_SD_CTL_RUN,
               hda_stream_reg(hda, HDA_SD_CTL));
        spin_unlock_irqrestore(&hda->state_lock, flags);
        return 0;
    }
    if (!arg)
        return -EFAULT;
    if (req == A20_AUDIO_IOCTL_GET_CAPS) {
        a20_audio_caps_t caps = {
            .version = 1,
            .flags = A20_AUDIO_CAP_PCM,
            .min_rate = HDA_PCM_RATE,
            .max_rate = HDA_PCM_RATE,
        };
        return copy_to_user(arg, &caps, sizeof(caps)) < 0 ? -EFAULT : 0;
    }
    if (req == A20_AUDIO_IOCTL_SET_FORMAT) {
        a20_audio_format_t format;
        if (copy_from_user(&format, arg, sizeof(format)) < 0)
            return -EFAULT;
        return format.rate == HDA_PCM_RATE &&
               format.channels == HDA_PCM_CHANNELS &&
               format.format == A20_AUDIO_FORMAT_S16_LE ? 0 : -EINVAL;
    }
    return -ENOTTY;
}

static int hda_quiesce(hda_controller_t *hda)
{
    __atomic_add_fetch(&hda->generation, 1U, __ATOMIC_ACQ_REL);
    if (hda->codec_configured) {
        int error = 0;
        hda_cmd12(hda, hda->dac, HDA_VERB_SET_STREAM, 0, &error);
        hda_cmd12(hda, hda->pin, HDA_VERB_SET_PIN_CTL, 0, &error);
        hda_cmd12(hda, hda->pin, HDA_VERB_SET_EAPD, 0, &error);
        hda->codec_configured = 0;
    }
    if (hda->stream) {
        writel(readl(hda_stream_reg(hda, HDA_SD_CTL)) & ~HDA_SD_CTL_RUN,
               hda_stream_reg(hda, HDA_SD_CTL));
        for (unsigned ms = 0; ms < HDA_TIMEOUT_MS; ms++) {
            if (!(readb(hda_stream_reg(hda, HDA_SD_CTL)) & HDA_SD_CTL_RUN))
                goto stream_stopped;
            mdelay(1);
        }
    }

stream_stopped:
    if (!hda->regs)
        return 0;
    writel(0, hda_reg(hda, HDA_REG_INTCTL));
    writel(readl(hda_reg(hda, HDA_REG_GCTL)) & ~HDA_GCTL_RESET,
           hda_reg(hda, HDA_REG_GCTL));
    return hda_wait32(hda, HDA_REG_GCTL, HDA_GCTL_RESET, 0);
}

static void hda_release(hda_controller_t *hda)
{
    if (!hda)
        return;
    if (hda_quiesce(hda) < 0) {
        kerr("[HDA] controller did not quiesce; retaining DMA allocations\n");
        return;
    }
    if (hda->pcm)
        dma_free_coherent_aligned(hda->pcm, HDA_DMA_BYTES, hda->pcm_dma);
    if (hda->bdl)
        dma_free_coherent_aligned(hda->bdl, PAGE_SIZE, hda->bdl_dma);
    kfree(hda);
}

static int hda_probe(device_t *dev)
{
    if (pci_class_code(dev) != 0x040300U)
        return -ENODEV;
    if (pci_enable_and_assign_bars(dev) < 0)
        return -ENODEV;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1U < 0x100U)
        return -ENODEV;

    hda_controller_t *hda = kcalloc(1, sizeof(*hda));
    if (!hda)
        return -ENOMEM;
    hda->regs = (uintptr_t)bar->start;
    mutex_init(&hda->lock);
    spin_init(&hda->state_lock);
    uint16_t gcap = readw(hda_reg(hda, HDA_REG_GCAP));
    hda->dma64 = (gcap & 1U) != 0;
    uint8_t input_streams = (uint8_t)((gcap >> 8) & 0x0fU);
    uint8_t output_streams = (uint8_t)((gcap >> 12) & 0x0fU);
    uintptr_t stream = hda->regs + 0x80U + (uintptr_t)input_streams * 0x20U;
    if (!output_streams || stream + 0x20U > (uintptr_t)bar->end + 1U)
        goto fail_nodev;
    hda->stream = stream;
    writel(0, hda_reg(hda, HDA_REG_INTCTL));
    const char *stage = "reset";
    int ret = hda_reset(hda);
    if (ret < 0)
        goto fail_io;
    stage = "codec";
    ret = hda_find_codec(hda);
    if (ret < 0)
        goto fail_io;
    stage = "widgets";
    ret = hda_find_widgets(hda);
    if (ret < 0)
        goto fail_io;
    stage = "codec-setup";
    ret = hda_codec_setup(hda);
    if (ret < 0)
        goto fail_io;
    hda->bdl = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE, &hda->bdl_dma);
    hda->pcm = dma_alloc_coherent_aligned(HDA_DMA_BYTES, PAGE_SIZE,
                                           &hda->pcm_dma);
    if (!hda->bdl || !hda->pcm)
        goto fail_nomem;
    if (!hda->dma64 && ((hda->bdl_dma >> 32) || (hda->pcm_dma >> 32))) {
        stage = "dma-address";
        ret = -EOPNOTSUPP;
        goto fail_io;
    }
#ifdef CONFIG_HDA_SMOKE_TEST
    stage = "stream-smoke";
    memset(hda->pcm, 0, PAGE_SIZE);
    ret = hda_play_chunk(hda, hda->pcm, PAGE_SIZE, hda->generation);
    if (ret < 0)
        goto fail_io;
    kinfo("HDA_STREAM_SMOKE: PASS\n");
#endif
    dev->drv_priv = hda;
    kinfo("[HDA] %s codec=%u afg=%u dac=%u pin=%u, 48000 Hz stereo S16_LE\n",
          dev->name, hda->codec, hda->afg, hda->dac, hda->pin);
    return 0;

fail_nomem:
    hda_release(hda);
    return -ENOMEM;
fail_io:
    kerr("[HDA] %s probe failed at %s: %d (codec=%u afg=%u dac=%u pin=%u)\n",
         dev->name, stage, ret, hda->codec, hda->afg, hda->dac, hda->pin);
    hda_release(hda);
    return ret;
fail_nodev:
    hda_release(hda);
    return -ENODEV;
}

static int hda_remove(device_t *dev)
{
    hda_controller_t *hda = dev ? dev->drv_priv : NULL;
    if (dev)
        dev->drv_priv = NULL;
    hda_release(hda);
    return 0;
}

static const audio_dev_ops_t hda_ops = {
    .write = hda_write,
    .ioctl = hda_ioctl,
};

static const device_id_t hda_ids[] = {
    { .vendor = VENDOR_ANY, .device = DEVICE_ANY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static int hda_match(device_t *dev)
{
    return pci_class_code(dev) == 0x040300U;
}

static driver_t hda_driver = {
    .name = "hda",
    .id_table = hda_ids,
    .bus = &pci_bus,
    .match = hda_match,
    .probe = hda_probe,
    .remove = hda_remove,
    .class_ops = &hda_ops,
    .class_type = DEV_CLASS_AUDIO,
};

DRIVER_REGISTER(hda_driver);

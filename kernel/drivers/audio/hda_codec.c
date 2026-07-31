/*
 * Intel High Definition Audio codec discovery and setup
 */
#include "drivers/audio/hda_internal.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/timer.h"

#define HDA_REG_STATESTS   0x0eU
#define HDA_REG_ICOI       0x60U
#define HDA_REG_ICII       0x64U
#define HDA_REG_ICIS       0x68U

#define HDA_ICIS_BUSY      0x01U
#define HDA_ICIS_VALID     0x02U

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

static int hda_codec_command(hda_controller_t *hda, uint32_t command,
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

static uint32_t hda_codec_cmd12(hda_controller_t *hda, uint8_t nid,
                                uint16_t verb, uint8_t payload, int *error)
{
    uint32_t response = 0;
    uint32_t command = ((uint32_t)hda->codec << 28) |
                       ((uint32_t)nid << 20) |
                       ((uint32_t)verb << 8) | payload;
    int ret = hda_codec_command(hda, command, &response);
    if (error && ret < 0)
        *error = ret;
    return response;
}

static int hda_codec_set16(hda_controller_t *hda, uint8_t nid,
                           uint8_t verb, uint16_t payload)
{
    uint32_t command = ((uint32_t)hda->codec << 28) |
                       ((uint32_t)nid << 20) |
                       ((uint32_t)verb << 16) | payload;
    return hda_codec_command(hda, command, NULL);
}

static uint32_t hda_codec_param(hda_controller_t *hda, uint8_t nid,
                                uint8_t param, int *error)
{
    return hda_codec_cmd12(hda, nid, HDA_VERB_GET_PARAM, param, error);
}

static int hda_codec_find(hda_controller_t *hda)
{
    uint16_t codecs = readw(hda_reg(hda, HDA_REG_STATESTS));
    if (!codecs)
        return -ENODEV;
    for (uint8_t codec = 0; codec < 15; codec++) {
        if (!(codecs & (1U << codec)))
            continue;
        hda->codec = codec;
        int error = 0;
        uint32_t nodes = hda_codec_param(hda, 0, HDA_PARAM_NODE_COUNT,
                                         &error);
        uint8_t start = (uint8_t)(nodes >> 16);
        uint8_t count = (uint8_t)nodes;
        for (uint8_t i = 0; !error && i < count; i++) {
            uint8_t nid = (uint8_t)(start + i);
            if ((hda_codec_param(hda, nid, HDA_PARAM_FUNC_TYPE, &error) &
                 0xffU) == 1U) {
                hda->afg = nid;
                return 0;
            }
        }
        if (error)
            return error;
    }
    return -ENODEV;
}

static int hda_codec_pin_priority(hda_controller_t *hda, uint8_t nid,
                                  int *error)
{
    uint32_t config = hda_codec_cmd12(hda, nid, HDA_VERB_GET_CONFIG, 0,
                                      error);
    if ((config >> 30) == 1U)
        return 0;
    uint8_t device = (uint8_t)((config >> 20) & 0x0fU);
    if (device == 0U) return 3;
    if (device == 1U) return 2;
    if (device == 2U) return 1;
    return 1;
}

static int hda_codec_connections(hda_controller_t *hda, uint8_t nid,
                                 uint8_t *nodes, uint8_t *selectors,
                                 int max_nodes, int *error)
{
    uint32_t info = hda_codec_param(hda, nid, HDA_PARAM_CONN_LEN, error);
    uint8_t raw_count = (uint8_t)(info & 0x7fU);
    int long_form = (info & 0x80U) != 0;
    int count = 0;
    uint16_t previous = 0;
    for (uint8_t raw = 0; *error >= 0 && raw < raw_count; ) {
        uint32_t packed = hda_codec_cmd12(hda, nid, HDA_VERB_GET_CONN, raw,
                                          error);
        uint8_t per_command = long_form ? 2U : 4U;
        for (uint8_t slot = 0; slot < per_command && raw < raw_count;
             slot++, raw++) {
            uint16_t entry = long_form ?
                             (uint16_t)(packed >> (slot * 16U)) :
                             (uint8_t)(packed >> (slot * 8U));
            uint16_t range_bit = long_form ? 0x8000U : 0x0080U;
            uint16_t value_mask = long_form ? 0x7fffU : 0x007fU;
            uint16_t value = entry & value_mask;
            uint16_t first = (entry & range_bit) ?
                             (uint16_t)(previous + 1U) : value;
            for (uint16_t child = first;
                 child <= value && count < max_nodes; child++) {
                nodes[count] = (uint8_t)child;
                selectors[count] = (uint8_t)count;
                count++;
            }
            previous = value;
        }
    }
    return *error < 0 ? *error : count;
}

static int hda_codec_find_path(hda_controller_t *hda, uint8_t nid,
                               uint8_t *visited, uint8_t depth, int *error)
{
    if (depth >= HDA_MAX_NODES || visited[nid])
        return 0;
    visited[nid] = 1;
    hda->path[depth] = nid;
    uint32_t caps = hda_codec_param(hda, nid, HDA_PARAM_WIDGET_CAP, error);
    if (*error < 0)
        return 0;
    if (((caps >> 20) & 0x0fU) == HDA_WIDGET_OUTPUT) {
        uint32_t pcm = (caps & (1U << 4)) ?
                       hda_codec_param(hda, nid, HDA_PARAM_AUDIO_CAPS,
                                       error) : hda->afg_pcm;
        uint32_t formats = (caps & (1U << 4)) ?
                           hda_codec_param(hda, nid, HDA_PARAM_STREAM_FMT,
                                           error) : hda->afg_formats;
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
    int count = hda_codec_connections(hda, nid, children, selectors,
                                      HDA_MAX_NODES, error);
    for (int i = 0; *error >= 0 && i < count; i++) {
        if (hda_codec_find_path(hda, children[i], visited,
                                (uint8_t)(depth + 1U), error)) {
            hda->path_select[depth] = selectors[i];
            return 1;
        }
    }
    return 0;
}

static int hda_codec_find_widgets(hda_controller_t *hda)
{
    int error = 0;
    uint32_t node_info = hda_codec_param(hda, hda->afg,
                                         HDA_PARAM_NODE_COUNT, &error);
    uint8_t start = (uint8_t)(node_info >> 16);
    uint8_t count = (uint8_t)node_info;
    if (count > HDA_MAX_NODES)
        count = HDA_MAX_NODES;
    hda->afg_pcm = hda_codec_param(hda, hda->afg, HDA_PARAM_AUDIO_CAPS,
                                   &error);
    hda->afg_formats = hda_codec_param(hda, hda->afg, HDA_PARAM_STREAM_FMT,
                                       &error);
    for (int priority = 3; !error && priority >= 1; priority--) {
        for (uint8_t i = 0; !error && i < count; i++) {
            uint8_t nid = (uint8_t)(start + i);
            uint32_t caps = hda_codec_param(hda, nid,
                                            HDA_PARAM_WIDGET_CAP, &error);
            if (((caps >> 20) & 0x0fU) != HDA_WIDGET_PIN ||
                (caps & (1U << 9)))
                continue;
            uint32_t pin_caps = hda_codec_param(hda, nid,
                                                HDA_PARAM_PIN_CAPS, &error);
            if (!(pin_caps & HDA_PIN_CAP_OUTPUT) ||
                hda_codec_pin_priority(hda, nid, &error) != priority)
                continue;
            uint8_t visited[256];
            memset(visited, 0, sizeof(visited));
            hda->path_len = 0;
            if (hda_codec_find_path(hda, nid, visited, 0, &error)) {
                hda->pin = nid;
                return 0;
            }
        }
    }
    return error < 0 ? error : -ENODEV;
}

static uint8_t hda_codec_amp_zero_db(hda_controller_t *hda, uint8_t nid,
                                     uint32_t widget_caps, int input,
                                     int *error)
{
    uint8_t target = (widget_caps & (1U << 3)) ? nid : hda->afg;
    uint8_t param = input ? HDA_PARAM_INPUT_AMP : HDA_PARAM_OUTPUT_AMP;
    return (uint8_t)(hda_codec_param(hda, target, param, error) & 0x7fU);
}

static int hda_codec_setup(hda_controller_t *hda)
{
    int error = 0;
    hda_codec_cmd12(hda, hda->afg, HDA_VERB_SET_POWER, 0, &error);
    for (uint8_t i = 0; !error && i < hda->path_len; i++) {
        uint8_t nid = hda->path[i];
        hda_codec_cmd12(hda, nid, HDA_VERB_SET_POWER, 0, &error);
        uint32_t caps = hda_codec_param(hda, nid, HDA_PARAM_WIDGET_CAP,
                                        &error);
        if (i + 1U < hda->path_len) {
            hda_codec_cmd12(hda, nid, HDA_VERB_SET_CONN,
                            hda->path_select[i], &error);
            if (caps & (1U << 1)) {
                if (hda->path_select[i] > 15U)
                    return -EOPNOTSUPP;
                uint8_t gain = hda_codec_amp_zero_db(hda, nid, caps, 1,
                                                      &error);
                if (error < 0 ||
                    hda_codec_set16(hda, nid, HDA_VERB_SET_AMP,
                                    (uint16_t)(0x7000U |
                                    (hda->path_select[i] << 8) | gain)) < 0)
                    return -EIO;
            }
        }
        if (caps & (1U << 2)) {
            uint8_t gain = hda_codec_amp_zero_db(hda, nid, caps, 0, &error);
            if (error < 0 ||
                hda_codec_set16(hda, nid, HDA_VERB_SET_AMP,
                                (uint16_t)(0xb000U | gain)) < 0)
                return -EIO;
        }
    }
    if (error < 0)
        return error;
    hda_codec_cmd12(hda, hda->pin, HDA_VERB_SET_PIN_CTL, 0x40U, &error);
    uint32_t pin_caps = hda_codec_param(hda, hda->pin, HDA_PARAM_PIN_CAPS,
                                        &error);
    if (pin_caps & HDA_PIN_CAP_EAPD)
        hda_codec_cmd12(hda, hda->pin, HDA_VERB_SET_EAPD, 0x02U, &error);
    if (hda_codec_set16(hda, hda->dac, HDA_VERB_SET_FORMAT,
                        HDA_PCM_FORMAT) < 0)
        return -EIO;
    hda_codec_cmd12(hda, hda->dac, HDA_VERB_SET_STREAM, 0x10U, &error);
    if (!error)
        hda->codec_configured = 1;
    return error;
}

int hda_codec_discover_and_setup(hda_controller_t *hda, const char **stage)
{
    if (stage)
        *stage = "codec";
    int ret = hda_codec_find(hda);
    if (ret < 0)
        return ret;
    if (stage)
        *stage = "widgets";
    ret = hda_codec_find_widgets(hda);
    if (ret < 0)
        return ret;
    if (stage)
        *stage = "codec-setup";
    return hda_codec_setup(hda);
}

void hda_codec_disable(hda_controller_t *hda)
{
    if (!hda->codec_configured)
        return;
    int error = 0;
    hda_codec_cmd12(hda, hda->dac, HDA_VERB_SET_STREAM, 0, &error);
    hda_codec_cmd12(hda, hda->pin, HDA_VERB_SET_PIN_CTL, 0, &error);
    hda_codec_cmd12(hda, hda->pin, HDA_VERB_SET_EAPD, 0, &error);
    hda->codec_configured = 0;
}

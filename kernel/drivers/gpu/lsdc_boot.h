/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_LSDC_BOOT_H
#define A20_LSDC_BOOT_H
#include "core/types.h"
/* Architecture-owned scanout authorization for a canonical firmware device.
 * No DT mutation is needed when vendor firmware omits an aperture property. */
#define LSDC_BOOT_POLICY_MAGIC 0x4c534442U
#define LSDC_BOOT_POLICY_VERSION 2U
typedef struct lsdc_boot_policy {
    uint32_t magic, version;
    uint64_t register_base, aperture_base, aperture_bytes;
    int (*prepare)(uint64_t physical, size_t bytes);
} lsdc_boot_policy_t;

/* Register facts: Loongson vendor 2K0300 lsdc_regs.h/plane.c/crtc.c,
 * commit 74fcec70d009f06c4a120c674c80ffdbdb2f6e47. No modesetting code. */
#define LSDC_BOOT_CFG       0x1240U
#define LSDC_BOOT_ADDR0     0x1260U
#define LSDC_BOOT_STRIDE    0x1280U
#define LSDC_BOOT_ORIGIN    0x1300U
#define LSDC_BOOT_HDISPLAY  0x1400U
#define LSDC_BOOT_VDISPLAY  0x1480U
#define LSDC_BOOT_ADDR1     0x1580U
#define LSDC_BOOT_HI0       0x15a0U
#define LSDC_BOOT_HI1       0x15c0U
#define LSDC_BOOT_REG_BYTES 0x1600U
#define LSDC_BOOT_OUTPUT    (1U << 8)
#define LSDC_BOOT_RESET     (1U << 20)
#define LSDC_BOOT_INDEX     (1U << 11)

typedef struct lsdc_boot_snapshot {
    uint32_t config, origin, stride, horizontal, vertical;
    uint32_t low[2], high[2];
} lsdc_boot_snapshot_t;
typedef struct lsdc_boot_mode {
    uint64_t physical, bytes;
    uint32_t width, height, bpp, stride;
    uint32_t red, green, blue, reserved;
} lsdc_boot_mode_t;

/* Keep this decoder independent of access policy so the production arithmetic
 * is executable against a host register model. A zero result means rejection. */
static inline int lsdc_boot_decode(const lsdc_boot_snapshot_t *s,
                                  uint64_t aperture, uint64_t aperture_bytes,
                                  lsdc_boot_mode_t *m)
{
    const uint32_t allowed = 7U | LSDC_BOOT_OUTPUT | LSDC_BOOT_RESET |
                             LSDC_BOOT_INDEX | (1U << 12); /* gamma */
    if ((s->config & (LSDC_BOOT_OUTPUT | LSDC_BOOT_RESET)) !=
        (LSDC_BOOT_OUTPUT | LSDC_BOOT_RESET) || (s->config & ~allowed) || s->origin)
        return 0; /* includes FB_SWITCH and PANEL_SWITCH, unknown modes */
    unsigned format = s->config & 7U;
    if (format != 3 && format != 4)
        return 0;
    m->width = s->horizontal & 0xffffU;
    m->height = s->vertical & 0xffffU;
    m->bpp = format == 3 ? 16 : 32;
    m->stride = s->stride;
    if (!m->width || m->width > 4096 || !m->height || m->height > 4096 ||
        (s->horizontal >> 16) <= m->width ||
        (s->vertical >> 16) <= m->height ||
        !m->stride || (m->stride & 255U) ||
        m->stride < m->width * (m->bpp / 8))
        return 0;
    unsigned index = !!(s->config & LSDC_BOOT_INDEX);
    m->physical = ((uint64_t)s->high[index] << 32) | s->low[index];
    m->bytes = ((uint64_t)m->stride * m->height + 4095) & ~(uint64_t)4095;
    if ((m->physical & 4095U) || !aperture_bytes ||
        aperture_bytes > (uint64_t)-1 - aperture ||
        m->physical < aperture || m->physical - aperture >= aperture_bytes ||
        m->bytes > aperture_bytes - (m->physical - aperture) ||
        m->physical > (uintptr_t)-1 || m->bytes > (size_t)-1)
        return 0;
    m->red = format == 3 ? 0xf800U : 0xff0000U;
    m->green = format == 3 ? 0x07e0U : 0x00ff00U;
    m->blue = format == 3 ? 0x001fU : 0x0000ffU;
    m->reserved = format == 3 ? 0 : 0xff000000U;
    return 1;
}
#endif

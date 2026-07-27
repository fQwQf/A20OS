#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H

#include "core/types.h"

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIO_MAP_FB         0x4603 /* A20OS specific: maps fb to arg (vaddr) */
#define FBIO_FLUSH          0x4604 /* A20OS specific: flushes fb */

#define FB_TYPE_PACKED_PIXELS      0
#define FB_VISUAL_TRUECOLOR        2
#define FB_VISUAL_DIRECTCOLOR      3

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

/*
 * Linux-compatible layout (sizeof == 160 on 64-bit).  Standard fbdev
 * userspace (Xfbdev, weston, LVGL upstream driver) reads xres_virtual,
 * xoffset and the RGB bitfields at these exact offsets.
 */
struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

/*
 * Linux-compatible layout (sizeof == 80 on 64-bit).  The implicit padding
 * after ywrapstep and after line_length matches the userspace ABI.
 */
struct fb_fix_screeninfo {
    char id[16];             /* identification string eg "TT Builtin" */
    unsigned long smem_start;/* Start of frame buffer mem (physical address) */
    uint32_t smem_len;       /* Length of frame buffer mem */
    uint32_t type;           /* FB_TYPE_* */
    uint32_t type_aux;
    uint32_t visual;         /* FB_VISUAL_* */
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;    /* length of a line in bytes    */
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

/*
 * Linux mmap(2) backend for /dev/fb0.  Maps fb_phys + off into the current
 * address space (PFNMAP, device memory attributes).  Returns the mapped
 * virtual address or a negative errno.  On NOMMU returns fb_phys + off.
 */
int64_t fbdev_linux_mmap(uint64_t addr, size_t len, int prot, int flags,
                         uint64_t off);

#endif

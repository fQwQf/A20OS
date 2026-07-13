#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H

#include "core/types.h"

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIO_MAP_FB         0x4603 /* A20OS specific: maps fb to arg (vaddr) */
#define FBIO_FLUSH          0x4604 /* A20OS specific: flushes fb */

struct fb_var_screeninfo {
    uint32_t xres;           /* visible resolution		*/
    uint32_t yres;
    uint32_t bits_per_pixel; /* guess what			*/
};

struct fb_fix_screeninfo {
    char id[16];             /* identification string eg "TT Builtin" */
    unsigned long smem_start;/* Start of frame buffer mem (physical address) */
    uint32_t smem_len;       /* Length of frame buffer mem */
    uint32_t line_length;    /* length of a line in bytes    */
};

#endif

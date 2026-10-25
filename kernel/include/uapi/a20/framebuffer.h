#ifndef A20_UAPI_FRAMEBUFFER_H
#define A20_UAPI_FRAMEBUFFER_H
typedef __UINT32_TYPE__ a20_fb_u32_t;
typedef __UINT16_TYPE__ a20_fb_u16_t;

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP 0x4604     /* Linux standard */
#define FBIOPUTCMAP 0x4605     /* Linux standard */
#define FBIOPAN_DISPLAY 0x4606 /* Linux standard */
#define FBIO_MAP_FB 0x4603     /* A20OS specific: maps fb to arg (vaddr) */
#define FBIO_FLUSH 0x4609      /* A20OS specific: flushes fb */
#define FBIOBLANK 0x4611       /* Linux standard */

#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR 2
#define FB_VISUAL_DIRECTCOLOR 3

struct fb_bitfield {
    a20_fb_u32_t offset;
    a20_fb_u32_t length;
    a20_fb_u32_t msb_right;
};

/*
 * Linux-compatible layout (sizeof == 160 on 64-bit).  Standard fbdev
 * userspace (Xfbdev, weston, LVGL upstream driver) reads xres_virtual,
 * xoffset and the RGB bitfields at these exact offsets.
 */
struct fb_var_screeninfo {
    a20_fb_u32_t xres;
    a20_fb_u32_t yres;
    a20_fb_u32_t xres_virtual;
    a20_fb_u32_t yres_virtual;
    a20_fb_u32_t xoffset;
    a20_fb_u32_t yoffset;
    a20_fb_u32_t bits_per_pixel;
    a20_fb_u32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    a20_fb_u32_t nonstd;
    a20_fb_u32_t activate;
    a20_fb_u32_t height;
    a20_fb_u32_t width;
    a20_fb_u32_t accel_flags;
    a20_fb_u32_t pixclock;
    a20_fb_u32_t left_margin;
    a20_fb_u32_t right_margin;
    a20_fb_u32_t upper_margin;
    a20_fb_u32_t lower_margin;
    a20_fb_u32_t hsync_len;
    a20_fb_u32_t vsync_len;
    a20_fb_u32_t sync;
    a20_fb_u32_t vmode;
    a20_fb_u32_t rotate;
    a20_fb_u32_t colorspace;
    a20_fb_u32_t reserved[4];
};

/*
 * Linux-compatible layout (sizeof == 80 on 64-bit).  The implicit padding
 * after ywrapstep and after line_length matches the userspace ABI.
 */
struct fb_fix_screeninfo {
    char id[16];              /* identification string eg "TT Builtin" */
    unsigned long smem_start; /* Start of frame buffer mem (physical address) */
    a20_fb_u32_t smem_len;    /* Length of frame buffer mem */
    a20_fb_u32_t type;        /* FB_TYPE_* */
    a20_fb_u32_t type_aux;
    a20_fb_u32_t visual; /* FB_VISUAL_* */
    a20_fb_u16_t xpanstep;
    a20_fb_u16_t ypanstep;
    a20_fb_u16_t ywrapstep;
    a20_fb_u32_t line_length; /* length of a line in bytes    */
    unsigned long mmio_start;
    a20_fb_u32_t mmio_len;
    a20_fb_u32_t accel;
    a20_fb_u16_t capabilities;
    a20_fb_u16_t reserved[2];
};

/* Linux color map (FBIOGETCMAP/FBIOPUTCMAP). */
struct fb_cmap {
    a20_fb_u32_t start;
    a20_fb_u32_t len;
    a20_fb_u16_t *red;
    a20_fb_u16_t *green;
    a20_fb_u16_t *blue;
    a20_fb_u16_t *transp;
};

#endif

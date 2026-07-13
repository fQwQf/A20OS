#if 1
#include "lv_port_disp.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* Use A20OS framebuffer structures manually to avoid header path issues */
struct a20os_fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t bits_per_pixel;
};

struct a20os_fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len;
    uint32_t line_length;
};

#define A20OS_FBIOGET_VSCREENINFO 0x4600
#define A20OS_FBIOGET_FSCREENINFO 0x4602
#define A20OS_FBIO_MAP_FB         0x4603
#define A20OS_FBIO_FLUSH          0x4604

static int fb_fd = -1;
static uint32_t fb_width = 1024;
static uint32_t fb_height = 768;
static uint32_t fb_bpp = 32;
static uint32_t fb_size = 0;
static void *fb_mem = NULL;

static void disp_init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("Failed to open /dev/fb0\n");
        return;
    }

    struct a20os_fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, A20OS_FBIOGET_VSCREENINFO, &vinfo) == 0) {
        fb_width = vinfo.xres;
        fb_height = vinfo.yres;
        fb_bpp = vinfo.bits_per_pixel;
        printf("FB dimensions: %dx%d, %d bpp\n", fb_width, fb_height, vinfo.bits_per_pixel);
    } else {
        printf("Failed to get vscreeninfo\n");
    }

    struct a20os_fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, A20OS_FBIOGET_FSCREENINFO, &finfo) < 0 ||
        finfo.smem_start == 0 || finfo.smem_len == 0) {
        printf("Failed to get framebuffer address\n");
        return;
    }
    fb_size = finfo.smem_len;

    /* Map framebuffer memory */
#ifdef A20OS_NOMMU
    fb_mem = (void *)(uintptr_t)finfo.smem_start;
    if (ioctl(fb_fd, A20OS_FBIO_MAP_FB, NULL) < 0) {
        printf("Failed to access framebuffer\n");
        fb_mem = NULL;
    }
#else
    fb_mem = (void *)0x60000000; /* Arbitrary aligned address */
    if (ioctl(fb_fd, A20OS_FBIO_MAP_FB, fb_mem) < 0) {
        printf("Failed to map framebuffer\n");
        fb_mem = NULL;
    }
#endif
}

static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    (void)area;
    (void)px_map;
    if (fb_mem) {
        /* We are using DIRECT mode, so we just need to flush.
         * The px_map is actually fb_mem. */
        ioctl(fb_fd, A20OS_FBIO_FLUSH, 0);
    }
    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    disp_init();

    lv_display_t * disp = lv_display_create(fb_width, fb_height);
    lv_display_set_flush_cb(disp, disp_flush);

    if (fb_mem) {
        lv_display_set_buffers(disp, fb_mem, NULL, fb_size,
                               LV_DISPLAY_RENDER_MODE_DIRECT);
    }
}

#endif

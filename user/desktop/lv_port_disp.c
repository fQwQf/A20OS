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

#define A20OS_FBIOGET_VSCREENINFO 0x4600
#define A20OS_FBIO_MAP_FB         0x4603
#define A20OS_FBIO_FLUSH          0x4604

static int fb_fd = -1;
static uint32_t fb_width = 1024;
static uint32_t fb_height = 768;
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
        printf("FB dimensions: %dx%d, %d bpp\n", fb_width, fb_height, vinfo.bits_per_pixel);
    } else {
        printf("Failed to get vscreeninfo\n");
    }

    /* Map framebuffer memory */
    fb_mem = (void *)0x60000000; /* Arbitrary aligned address */
    if (ioctl(fb_fd, A20OS_FBIO_MAP_FB, fb_mem) < 0) {
        printf("Failed to map framebuffer\n");
        fb_mem = NULL;
    }
}

static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
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
        lv_display_set_buffers(disp, fb_mem, NULL, fb_width * fb_height * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
    }
}

#endif

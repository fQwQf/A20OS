#if 1
#include "lv_port_disp.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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
static uint32_t fb_stride = 0;
static void *fb_mem = NULL;

static int disp_init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("Failed to open /dev/fb0\n");
        return -1;
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
        return -1;
    }
    fb_size = finfo.smem_len;
    fb_stride = finfo.line_length;
    if (fb_stride < fb_width * (fb_bpp / 8U) ||
        (uint64_t)fb_stride * fb_height > fb_size) {
        printf("Invalid framebuffer stride: %u\n", fb_stride);
        return -1;
    }

    /* Map framebuffer memory */
#ifdef A20OS_NOMMU
    fb_mem = (void *)(uintptr_t)finfo.smem_start;
    if (ioctl(fb_fd, A20OS_FBIO_MAP_FB, NULL) < 0) {
        printf("Failed to access framebuffer\n");
        fb_mem = NULL;
        return -1;
    }
#else
    /*
     * Keep the framebuffer below the initial user stack (0x40000000).  The
     * old 0x60000000 address is valid in the abstract user VA range, but it
     * lies above the stack on the 32-bit-style layout used by A20OS userland
     * and conflicts with the address-space assumptions in the current MMU
     * implementation.  This otherwise made FBIO_MAP_FB fail on both ARM64
     * and x86_64, leaving LVGL with no draw buffer and a black QEMU window.
     */
    fb_mem = (void *)0x30000000;
    if (ioctl(fb_fd, A20OS_FBIO_MAP_FB, fb_mem) < 0) {
        printf("Failed to map framebuffer: errno=%d\n", errno);
        fb_mem = NULL;
        return -1;
    }
#endif
    printf("Framebuffer mapped: va=%p size=%u stride=%u\n",
           fb_mem, fb_size, fb_stride);
    return 0;
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
    if (disp_init() < 0) {
        fprintf(stderr, "FATAL: framebuffer initialization failed\n");
        exit(1);
    }

    lv_display_t * disp = lv_display_create(fb_width, fb_height);
    lv_display_set_flush_cb(disp, disp_flush);

    lv_display_set_buffers_with_stride(disp, fb_mem, NULL, fb_size,
                                       fb_stride,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);
}

#endif

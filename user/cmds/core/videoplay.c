/* Bounded RGB24 stream player for linear fbdev scanouts. Input is decoded on
 * the host; this program performs pixel conversion and monotonic frame pacing. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <uapi/a20/framebuffer.h>

static int number(const char *text, unsigned limit, unsigned *out) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || !*text || *end || !n || n > limit)
        return -1;
    *out = (unsigned)n;
    return 0;
}
static int channel_valid(struct fb_bitfield c, unsigned bpp) {
    return !c.msb_right && c.length && c.length <= 16 && c.offset < bpp &&
           c.length <= bpp - c.offset;
}
static uint32_t component(uint8_t value, struct fb_bitfield c) {
    return (((uint32_t)value * ((1U << c.length) - 1) + 127) / 255) << c.offset;
}
static int frame_read(int fd, void *buffer, size_t bytes) {
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = read(fd, (uint8_t *)buffer + done, bytes - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (!n)
            return done ? -2 : 0;
        done += (size_t)n;
    }
    return 1;
}
static uint64_t nanoseconds(struct timespec t) {
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static int pace(uint64_t deadline) {
    struct timespec now;
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            return -1;
        uint64_t current = nanoseconds(now);
        if (current >= deadline)
            return 0;
        uint64_t delta = deadline - current;
        struct timespec delay = {(time_t)(delta / 1000000000ULL),
                                 (long)(delta % 1000000000ULL)};
        if (nanosleep(&delay, NULL) && errno != EINTR)
            return -1;
    }
}
int main(int argc, char **argv) {
    unsigned width, height, fps;
    if ((argc != 5 && argc != 6) || number(argv[2], 4096, &width) ||
        number(argv[3], 4096, &height) || number(argv[4], 120, &fps)) {
        fprintf(stderr, "usage: videoplay RGB24_FILE|-|--test WIDTH HEIGHT FPS "
                        "[/dev/fb0]\n");
        return 2;
    }
    int test = !strcmp(argv[1], "--test");
    int input =
        test ? STDIN_FILENO
             : (strcmp(argv[1], "-") ? open(argv[1], O_RDONLY) : STDIN_FILENO);
    if (input < 0) {
        perror("videoplay: input");
        return 1;
    }
    int fb = open(argc == 6 ? argv[5] : "/dev/fb0", O_RDWR);
    if (fb < 0) {
        perror("videoplay: framebuffer");
        if (input != 0)
            close(input);
        return 1;
    }
    int result = 1;
    uint8_t *pixels = NULL, *scanout = MAP_FAILED;
    size_t map_bytes = 0;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) ||
        ioctl(fb, FBIOGET_FSCREENINFO, &fix)) {
        perror("videoplay: framebuffer info");
        goto out;
    }
    if (width > var.xres || height > var.yres || var.xoffset || var.yoffset ||
        (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 &&
         var.bits_per_pixel != 32) ||
        !channel_valid(var.red, var.bits_per_pixel) ||
        !channel_valid(var.green, var.bits_per_pixel) ||
        !channel_valid(var.blue, var.bits_per_pixel) ||
        (uint64_t)fix.line_length <
            (uint64_t)var.xres * (var.bits_per_pixel / 8) ||
        (uint64_t)fix.line_length * var.yres > fix.smem_len) {
        fprintf(
            stderr,
            "videoplay: unsupported framebuffer or input exceeds display\n");
        goto out;
    }
    uint32_t rmask = ((1U << var.red.length) - 1) << var.red.offset;
    uint32_t gmask = ((1U << var.green.length) - 1) << var.green.offset;
    uint32_t bmask = ((1U << var.blue.length) - 1) << var.blue.offset;
    if ((rmask & gmask) || (rmask & bmask) || (gmask & bmask)) {
        fprintf(stderr, "videoplay: overlapping framebuffer channels\n");
        goto out;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        fprintf(stderr, "videoplay: invalid page size\n");
        goto out;
    }
    map_bytes = (size_t)fix.smem_len & ~((size_t)page - 1);
    if ((uint64_t)fix.line_length * var.yres > map_bytes) {
        fprintf(stderr, "videoplay: incomplete framebuffer mapping\n");
        goto out;
    }
    scanout = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (scanout == MAP_FAILED) {
        perror("videoplay: mmap");
        goto out;
    }
    size_t frame_bytes = (size_t)width * height * 3;
    pixels = malloc(frame_bytes);
    if (!pixels) {
        perror("videoplay: frame buffer");
        goto out;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        perror("videoplay: clock");
        goto out;
    }
    uint64_t start = nanoseconds(now), frame = 0, period = 1000000000ULL / fps;
    for (;;) {
        int rc;
        if (test) {
            rc = frame < (uint64_t)fps * 10 ? 1 : 0;
            for (unsigned y = 0; y < height && rc; y++)
                for (unsigned x = 0; x < width; x++) {
                    unsigned color = (uint64_t)x * 3 / width;
                    size_t at = ((size_t)y * width + x) * 3;
                    pixels[at] = color == 0 ? 255 : 0;
                    pixels[at + 1] = color == 1 ? 255 : 0;
                    pixels[at + 2] = color == 2 ? 255 : 0;
                    /* A moving white marker distinguishes frames. */
                    if (y < 8 && x >= frame % width && x - frame % width < 8)
                        pixels[at] = pixels[at + 1] = pixels[at + 2] = 255;
                }
        } else
            rc = frame_read(input, pixels, frame_bytes);
        if (!rc) {
            if (!frame) {
                fprintf(stderr, "videoplay: empty input\n");
                goto out;
            }
            result = 0;
            break;
        }
        if (rc < 0) {
            if (rc == -2)
                fprintf(stderr, "videoplay: truncated RGB24 frame\n");
            else
                perror("videoplay: read");
            goto out;
        }
        if (frame > (UINT64_MAX - start) / period ||
            pace(start + frame * period)) {
            fprintf(stderr, "videoplay: frame clock failed\n");
            goto out;
        }
        unsigned bytes = var.bits_per_pixel / 8;
        unsigned x0 = (var.xres - width) / 2, y0 = (var.yres - height) / 2;
        for (unsigned y = 0; y < height; y++) {
            volatile uint8_t *destination =
                scanout + (size_t)(y + y0) * fix.line_length + x0 * bytes;
            const uint8_t *source = pixels + (size_t)y * width * 3;
            for (unsigned x = 0; x < width; x++) {
                uint32_t pixel = component(source[x * 3], var.red) |
                                 component(source[x * 3 + 1], var.green) |
                                 component(source[x * 3 + 2], var.blue);
                for (unsigned n = 0; n < bytes; n++)
                    destination[x * bytes + n] = pixel >> (8 * n);
            }
        }
        if (ioctl(fb, FBIO_FLUSH, NULL)) {
            perror("videoplay: flush");
            goto out;
        }
        frame++;
    }
    printf("videoplay: presented %llu frames\n", (unsigned long long)frame);
out:
    free(pixels);
    if (scanout != MAP_FAILED)
        munmap(scanout, map_bytes);
    close(fb);
    if (input != STDIN_FILENO)
        close(input);
    return result;
}

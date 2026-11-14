#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <uapi/a20/framebuffer.h>

#define CHECK(x) do { if (!(x)) { printf("FBDEV_MAP: FAIL line=%d errno=%d %s\n", __LINE__, errno, #x); return 1; } } while (0)

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    CHECK(fd >= 0);
    struct fb_fix_screeninfo fix;
    CHECK(!ioctl(fd, FBIOGET_FSCREENINFO, &fix));
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0 && fix.smem_len >= (unsigned long)page * 2);
    size_t p = (size_t)page;
    unsigned char *base = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED);
    base[p] = 0x5a;
    errno = 0;
    CHECK(mmap(base, p, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) == MAP_FAILED && errno == EEXIST);
    CHECK(base[p] == 0x5a);
    CHECK(mmap(base, p, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, fd, 0) == base);
    volatile unsigned char *pixel = base;
    unsigned char saved = *pixel;
    *pixel = saved ^ 0x55;
    CHECK(*pixel == (unsigned char)(saved ^ 0x55) && base[p] == 0x5a);
    *pixel = saved;
    CHECK(!mprotect(base, p, PROT_READ));
    CHECK(!mprotect(base, p, PROT_READ | PROT_WRITE));
    CHECK(!munmap(base, 2 * p));

    base = mmap(NULL, 3 * p, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED);
    base[p] = 0xa5;
    CHECK(!munmap(base, p));
    void *hint = mmap(base, 2 * p, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(hint != MAP_FAILED && hint != base && base[p] == 0xa5);
    CHECK(!munmap(hint, 2 * p) && !munmap(base + p, 2 * p));

    void *ro = mmap(NULL, p, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(ro != MAP_FAILED);
    errno = 0;
    CHECK(mprotect(ro, p, PROT_READ | PROT_WRITE) < 0 && errno == EACCES);
    CHECK(!munmap(ro, p));
    errno = 0;
    CHECK(mmap(NULL, p, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);

    size_t rounded = ((size_t)fix.smem_len + p - 1) & ~(p - 1);
    void *legacy = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(legacy != MAP_FAILED && !munmap(legacy, rounded));
    CHECK(!ioctl(fd, FBIO_MAP_FB, legacy));
    errno = 0;
    CHECK(ioctl(fd, FBIO_MAP_FB, legacy) < 0 && errno == EEXIST);
    CHECK(!munmap(legacy, rounded) && !close(fd));
    printf("FBDEV_MAP: PASS fixed=1 noreplace=1 hint=1 ceiling=1 legacy=1\n");
    return 0;
}

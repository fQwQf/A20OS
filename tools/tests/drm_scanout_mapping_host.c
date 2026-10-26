#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/gpu/drm_scanout_mapping.h"
#include "drivers/gpu/drm_scanout.h"
extern int puts(const char *);

pfa_t pfa;
static boot_context_t boot;
static unsigned char pixels[4096], io_pixels[4096];
static unsigned boot_maps, mmio_maps, mmio_unmaps;
static int fail_boot, fail_mmio;

const boot_context_t *boot_context_get(void) { return &boot; }
void *boot_framebuffer_map(void) {
    boot_maps++;
    return fail_boot ? NULL : pixels;
}
void *arch_device_map_mmio(uint64_t physical, size_t bytes, uint32_t flags) {
    assert(physical == 0x90000000 && bytes == 4096 && flags == 0);
    mmio_maps++;
    return fail_mmio ? NULL : io_pixels;
}
void arch_device_unmap_mmio(void *address, size_t bytes) {
    assert(address == io_pixels && bytes == 4096);
    mmio_unmaps++;
}
static void reset(void) {
    memset(&pfa, 0, sizeof(pfa)); memset(&boot, 0, sizeof(boot));
    memset(pixels, 0, sizeof(pixels)); memset(io_pixels, 0, sizeof(io_pixels));
    /* A compact PFN index intentionally differs from the physical page number. */
    pfa.nr_ranges = 1;
    pfa.ranges[0] = (pfa_range_t){0x400000, 0x410000, 123, 139};
    boot.framebuffer = (boot_framebuffer_t){
        .magic=BOOT_FRAMEBUFFER_MAGIC, .physical=0x80000000, .aperture_bytes=4096,
    };
    boot_maps = mmio_maps = mmio_unmaps = fail_boot = fail_mmio = 0;
}
static int reader(void *opaque, uint64_t offset, void *out, size_t bytes) {
    const unsigned char *src = opaque;
    assert(offset <= 16 && bytes <= 16 - offset);
    memcpy(out, src + offset, bytes); return 0;
}
static void retained(void) {
    reset();
    drm_scanout_mapping_t mapped;
    /* Exact integration failure: GOP VRAM is reserved and has no allocator PFN. */
    assert(phys_to_pfn(0x80000000) == PFN_NONE);
    assert(!pfn_to_virt(PFN_NONE));
    assert(!drm_scanout_acquire(0x80000000, 4096, &mapped));
    assert(mapped.address == pixels && !mapped.mmio && boot_maps == 1);
    const unsigned char source[] = {0,0,255,0, 0,255,0,0, 255,0,0,0, 3,2,1,0};
    drm_scanout_t dst = {2,2,16,32,0xff0000,0xff00,0xff,4096,mapped.address};
    uint32_t w, h;
    assert(!drm_scanout_blit(&dst,2,2,8,16,0,0,reader,(void *)source,&w,&h));
    assert(w == 2 && h == 2);
    assert(!memcmp(pixels,source,8) && !memcmp(pixels+16,source+8,8));
    drm_scanout_release(&mapped);
    assert(!mapped.address && !mmio_maps && !mmio_unmaps);
    fail_boot = 1;
    assert(drm_scanout_acquire(0x80000000,4096,&mapped) == -ENXIO);
    assert(!mapped.address && !mmio_maps); /* no unsafe Device/RAM fallback */
    assert(drm_scanout_acquire(0x80000000,8192,&mapped) == -EINVAL);
}
static void allocated_ram(void) {
    reset();
    drm_scanout_mapping_t mapped;
    assert(phys_to_pfn(0x403000) == 126);
    assert(!drm_scanout_acquire(0x403000,8192,&mapped));
    assert(mapped.address == (void *)(PAGE_OFFSET+0x403000) && !mapped.mmio);
    assert(!boot_maps && !mmio_maps);
    drm_scanout_release(&mapped);
    assert(!mapped.address && !mmio_unmaps);
}
static void device_vram(void) {
    reset();
    drm_scanout_mapping_t mapped;
    assert(!drm_scanout_acquire(0x90000000,4096,&mapped));
    assert(mapped.address == io_pixels && mapped.mmio && mmio_maps == 1);
    drm_scanout_release(&mapped);
    assert(!mapped.address && mmio_unmaps == 1);
    drm_scanout_release(&mapped);
    assert(mmio_unmaps == 1);
    fail_mmio = 1;
    assert(drm_scanout_acquire(0x90000000,4096,&mapped) == -ENXIO);
    assert(!mapped.address && !mapped.mmio);
}
static void invalid(void) {
    reset();
    drm_scanout_mapping_t mapped;
    assert(drm_scanout_acquire(0x90000000,0,&mapped) == -EINVAL);
    assert(drm_scanout_acquire(0x90000001,4096,&mapped) == -EINVAL);
    assert(drm_scanout_acquire((uintptr_t)-4096,8192,&mapped) == -EINVAL);
    assert(drm_scanout_acquire(0x40f000,8192,&mapped) == -EINVAL);
    assert(drm_scanout_acquire(0x3ff000,8192,&mapped) == -EINVAL);
    assert(drm_scanout_acquire(0x90000000,4096,NULL) == -EINVAL);
    assert(!boot_maps && !mmio_maps && !mmio_unmaps);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: retained(); break;
    case 1: allocated_ram(); break;
    case 2: device_vram(); break;
    case 3: invalid(); break;
    default: return 1;
    }
    puts("DRM scanout mapping: PASS"); return 0;
}

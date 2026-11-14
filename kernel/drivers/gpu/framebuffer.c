#include "drivers/gpu/framebuffer.h"
#include "boot/framebuffer.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "fs/devfs.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/errno.h"
#include "core/mman.h"
#include "mm/vm.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

static void fb_fill_var_screeninfo(struct fb_var_screeninfo *var,
                                   uint32_t w, uint32_t h, uint32_t bpp) {
    memset(var, 0, sizeof(*var));
    var->xres = w;
    var->yres = h;
    var->xres_virtual = w;
    var->yres_virtual = h;
    var->bits_per_pixel = bpp;
    switch (bpp) {
    case 32:
        var->red.offset = 16;   var->red.length = 8;
        var->green.offset = 8;  var->green.length = 8;
        var->blue.offset = 0;   var->blue.length = 8;
        break;
    case 24:
        var->red.offset = 16;   var->red.length = 8;
        var->green.offset = 8;  var->green.length = 8;
        var->blue.offset = 0;   var->blue.length = 8;
        break;
    case 16:
        var->red.offset = 11;   var->red.length = 5;
        var->green.offset = 5;  var->green.length = 6;
        var->blue.offset = 0;   var->blue.length = 5;
        break;
    default:
        break;
    }
    var->height = (uint32_t)-1;
    var->width = (uint32_t)-1;
}

static int fb_apply_layout(struct device *dev, const gpu_dev_ops_t *ops,
                           struct fb_var_screeninfo *var, uint32_t *stride) {
    if (!ops->get_layout) return 0;
    gpu_scanout_layout_t layout;
    int rc = ops->get_layout(dev, &layout);
    if (rc) return rc;
    if (!var->yres || !var->bits_per_pixel ||
        layout.stride < (uint64_t)var->xres * (var->bits_per_pixel / 8))
        return -EINVAL;
    uint32_t masks[] = {layout.red_mask, layout.green_mask, layout.blue_mask};
    struct fb_bitfield *fields[] = {&var->red, &var->green, &var->blue};
    for (unsigned i = 0; i < 3; i++) {
        *fields[i] = (struct fb_bitfield){0};
        uint32_t mask = masks[i];
        if (!mask) continue;
        while (!(mask & 1)) { fields[i]->offset++; mask >>= 1; }
        while (mask & 1) { fields[i]->length++; mask >>= 1; }
        if (mask) return -EINVAL;
    }
    if (stride) *stride = layout.stride;
    return 0;
}

/*
 * Periodic framebuffer refresh.  Compositors written for Linux fbdev
 * semantics (weston, X) draw into the mmap'd scanout and never issue a
 * flush ioctl, because on real hardware the scanout is the memory they
 * write.  Virtualized GPUs (virtio-gpu, vmsvga) instead require an
 * explicit transfer+flush to make writes visible.  Rather than patching
 * every userspace, refresh the whole scanout at ~30 Hz once anyone has
 * mmap'd the framebuffer; drivers without a flush op simply skip.
 */
static volatile int g_fb_autoflush_started;

static void fb_autoflush_once(void) {
    struct gpu_device_access access;
    if (gpu_device_default_begin(&access)) return;
    const gpu_dev_ops_t *ops = access.ops;
    if (ops->get_info && ops->flush) {
        uint32_t w = 0, h = 0, bpp = 0;
        if (!ops->get_info(access.device, &w, &h, &bpp) && w && h)
            (void)ops->flush(access.device, 0, 0, w, h);
    }
    gpu_device_access_end(&access);
}

static void fb_autoflush_thread(void) {
    for (;;) {
        fb_autoflush_once();
        proc_sleep_until(timer_get_ticks() + TICKS_PER_SEC / 30);
    }
}

static void fb_autoflush_kick(void) {
    if (g_fb_autoflush_started)
        return;
    g_fb_autoflush_started = 1;
    if (proc_alloc(fb_autoflush_thread) < 0)
        g_fb_autoflush_started = 0;
}

#ifndef CONFIG_NOMMU
/* Keep the boot scanout's CPU memory type, while the MM transaction owns
 * placement, PTE rollback, remote TLB invalidation and VMA publication. */
static int64_t fb_map_physical(mm_struct_t *mm, uint64_t addr, uint64_t physical,
                               size_t len, int prot, int flags) {
    if (!len || len > SIZE_MAX - (PAGE_SIZE - 1)) return -EINVAL;
    if (addr != (vaddr_t)addr) return -EINVAL;
    size_t rounded = ROUND_UP(len, PAGE_SIZE);
    uintptr_t pte_flags;
    int result = boot_framebuffer_user_flags(physical, rounded,
                    mm_prot_to_pte_flags(prot) | PTE_DEVICE, &pte_flags);
    if (result) return result;
    vaddr_t mapped = mm_mmap_physical(mm, addr, rounded, prot, flags, physical,
                                      pte_flags & (PTE_DEVICE | PTE_MAT1));
    return mm_addr_is_error(mapped) ? mm_addr_error(mapped) : (int64_t)mapped;
}
#endif

static int64_t fbdev_linux_mmap_active(struct gpu_device_access *access,
                         uint64_t addr, size_t len, int prot, int flags,
                         uint64_t off) {
    static int first_mmap = 1;
    struct device *dev = access->device;
    if (first_mmap) {
        printf("[FB] fbdev_linux_mmap: dev=%p addr=0x%lx len=0x%lx prot=%d flags=0x%x off=0x%lx\n",
               dev, addr, len, prot, flags, off);
        first_mmap = 0;
    }
    const gpu_dev_ops_t *ops = access->ops;
    if (!ops->get_fb) return -EOPNOTSUPP;

    fb_autoflush_kick();

    uintptr_t fb_phys;
    size_t fb_size;
    int r = ops->get_fb(dev, &fb_phys, &fb_size);
    if (r < 0)
        return r;
    if (fb_size && fb_phys > (uintptr_t)-1 - (fb_size - 1)) return -EOVERFLOW;

    if (off & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len > SIZE_MAX - (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0)
        return -EINVAL;
    if (off >= fb_size || len > fb_size - off)
        return -ENXIO;

#ifdef CONFIG_NOMMU
    /*
     * NOMMU user and kernel code share the physical address space; the
     * caller uses the returned physical address directly.
     */
    (void)addr; (void)prot; (void)flags;
    return (int64_t)(fb_phys + off);
#else
    task_t *curr = proc_current();
    if (!curr || !curr->mm) return -EFAULT;
    return fb_map_physical(curr->mm, addr, fb_phys + off, len, prot, flags);
#endif
}

int64_t fbdev_linux_mmap(uint64_t addr, size_t len, int prot, int flags, uint64_t off) {
    struct gpu_device_access access;
    int result = gpu_device_default_begin(&access);
    if (result) return result;
    int64_t mapped = fbdev_linux_mmap_active(&access, addr, len, prot, flags, off);
    gpu_device_access_end(&access);
    return mapped;
}

static int fb_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    (void)buf;
    (void)count;
    return -ENOSYS;
}

static int fb_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf;
    (void)buf;
    (void)count;
    return -ENOSYS;
}

static int fb_ioctl_active(struct gpu_device_access *access, vfile_t *vf,
                            unsigned long req, void *arg) {
    (void)vf;
    struct device *dev = access->device;
    const gpu_dev_ops_t *ops = access->ops;
    if ((req == FBIOGET_VSCREENINFO || req == FBIOPUT_VSCREENINFO ||
         req == FBIOGET_FSCREENINFO || req == FBIO_FLUSH) && !ops->get_info)
        return -EOPNOTSUPP;
    if ((req == FBIOGET_FSCREENINFO || req == FBIO_MAP_FB) && !ops->get_fb)
        return -EOPNOTSUPP;
    if (req == FBIO_FLUSH && !ops->flush) return -EOPNOTSUPP;

    switch (req) {
        case FBIOGET_VSCREENINFO: {
            struct fb_var_screeninfo var;
            uint32_t w, h, bpp;
            int r = ops->get_info(dev, &w, &h, &bpp);
            printf("[FB] FBIOGET_VSCREENINFO: get_info=%d w=%u h=%u bpp=%u\n",
                   r, w, h, bpp);
            if (r < 0)
                return r;
            fb_fill_var_screeninfo(&var, w, h, bpp);
            r = fb_apply_layout(dev, ops, &var, NULL);
            if (r) return r;
            printf("[FB] FBIOGET_VSCREENINFO: xres=%u yres=%u bpp=%u\n",
                   var.xres, var.yres, var.bits_per_pixel);
            return copy_to_user(arg, &var, sizeof(var)) < 0 ? -EFAULT : 0;
        }
        case FBIOPUT_VSCREENINFO: {
            struct fb_var_screeninfo var;
            if (copy_from_user(&var, arg, sizeof(var)) < 0)
                return -EFAULT;
            uint32_t w, h, bpp;
            int r = ops->get_info(dev, &w, &h, &bpp);
            if (r < 0)
                return r;
            /*
             * Mode setting is not supported.  Accept requests that match
             * the current mode as a no-op so standard fbdev userspace
             * (weston, Xfbdev) can run unmodified.
             */
            if (var.xres != w || var.yres != h || var.bits_per_pixel != bpp)
                return -EINVAL;
            return 0;
        }
        case FBIOGET_FSCREENINFO: {
            struct fb_fix_screeninfo fix;
            uintptr_t fb_phys;
            size_t fb_size;
            int r = ops->get_fb(dev, &fb_phys, &fb_size);
            if (r < 0)
                return r;

            memset(&fix, 0, sizeof(fix));
            const char *driver_name = dev->drv && dev->drv->name ?
                                      dev->drv->name : "A20_FB";
            strncpy(fix.id, driver_name, sizeof(fix.id) - 1);
            fix.smem_start = fb_phys;
            fix.smem_len = fb_size;
            fix.type = FB_TYPE_PACKED_PIXELS;
            fix.visual = FB_VISUAL_TRUECOLOR;

            uint32_t w, h, bpp;
            r = ops->get_info(dev, &w, &h, &bpp);
            if (r < 0)
                return r;
            /* The exported framebuffer is a tightly described scanout.  GPU
             * drivers restrict smem_len to the visible pitch * height. */
            if (!h) return -EINVAL;
            fix.line_length = (uint32_t)(fb_size / h);
            struct fb_var_screeninfo var;
            fb_fill_var_screeninfo(&var, w, h, bpp);
            r = fb_apply_layout(dev, ops, &var, &fix.line_length);
            if (r) return r;
            return copy_to_user(arg, &fix, sizeof(fix)) < 0 ? -EFAULT : 0;
        }
        case FBIO_MAP_FB: {
            uintptr_t fb_phys;
            size_t fb_size;
            int r = ops->get_fb(dev, &fb_phys, &fb_size);
            if (r < 0)
                return r;
            fb_autoflush_kick();

#ifdef CONFIG_NOMMU
            /*
             * NOMMU user and kernel code share the physical address space.
             * Userspace obtains fb_phys through FBIOGET_FSCREENINFO and uses
             * it directly; there is no virtual mapping to install.
             */
            (void)arg;
            return 0;
#else
            task_t *curr = proc_current();
            if (!curr || !curr->mm) return -EFAULT;
            int64_t mapped = fb_map_physical(curr->mm, (uintptr_t)arg, fb_phys,
                                fb_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_FIXED_NOREPLACE);
            return mapped < 0 ? (int)mapped : 0;
#endif
        }
        case FBIO_FLUSH: {
            uint32_t w, h, bpp;
            int r = ops->get_info(dev, &w, &h, &bpp);
            if (r < 0)
                return r;
            return ops->flush(dev, 0, 0, w, h);
        }
        case FBIOPAN_DISPLAY: {
            /* Pan the display to a new x/y offset.  A20OS scanout is a fixed
             * full-frame buffer, so only (0,0) is accepted. */
            struct fb_var_screeninfo var;
            if (copy_from_user(&var, arg, sizeof(var)) < 0)
                return -EFAULT;
            return (var.xoffset == 0 && var.yoffset == 0) ? 0 : -EINVAL;
        }
        case FBIOBLANK: {
            /* No DPMS blanking primitive; accept 0 (unblank) and FB_BLANK_NORMAL
             * as no-ops so standard fbdev userspace can probe blanking. */
            int blank = (int)(uintptr_t)arg;
            if (blank == 0 || blank == 1)
                return 0;
            return -EINVAL;
        }
        case FBIOGETCMAP: {
            /* Truecolor framebuffer: the palette is not used.  Return the
             * identity map for compatibility with tools that read it. */
            if (!arg)
                return -EFAULT;
            struct fb_cmap cmap;
            if (copy_from_user(&cmap, arg, sizeof(cmap)) < 0)
                return -EFAULT;
            if (cmap.len == 0)
                return 0;
            if (!cmap.red || !cmap.green || !cmap.blue)
                return -EINVAL;
            size_t bytes = (size_t)cmap.len * sizeof(uint16_t);
            uint16_t scratch[256];
            for (uint32_t i = 0; i < cmap.len && i < 256; i++) {
                uint16_t v = (uint16_t)(((cmap.start + i) * 65535) / 255);
                scratch[i] = v;
            }
            if (bytes > sizeof(scratch))
                return -EINVAL;
            if (copy_to_user(cmap.red, scratch, bytes) < 0 ||
                copy_to_user(cmap.green, scratch, bytes) < 0 ||
                copy_to_user(cmap.blue, scratch, bytes) < 0)
                return -EFAULT;
            return 0;
        }
        case FBIOPUTCMAP: {
            /* Truecolor: accept and discard palette writes. */
            if (!arg)
                return -EFAULT;
            struct fb_cmap cmap;
            if (copy_from_user(&cmap, arg, sizeof(cmap)) < 0)
                return -EFAULT;
            if (cmap.start > 256 || cmap.len > 256 - cmap.start)
                return -EINVAL;
            if (cmap.len && (!cmap.red || !cmap.green || !cmap.blue))
                return -EINVAL;
            return 0;
        }
        default:
            return -EINVAL;
    }
}

static int fb_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    struct gpu_device_access access;
    int result = gpu_device_default_begin(&access);
    if (result) return result;
    result = fb_ioctl_active(&access, vf, req, arg);
    gpu_device_access_end(&access);
    return result;
}
static int fb_close(vfile_t *vf) {
    (void)vf;
    return 0;
}

vfile_ops_t g_devfs_fb_ops = {
    .read  = fb_read,
    .write = fb_write,
    .ioctl = fb_ioctl,
    .close = fb_close,
};

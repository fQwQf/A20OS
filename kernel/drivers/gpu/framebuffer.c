#include "drivers/gpu/framebuffer.h"
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "fs/devfs.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "abi/linux/errno.h"
#include "mm/vm.h"

static int fb_read(vfile_t *vf, char *buf, size_t count) {
    return -ENOSYS;
}

static int fb_write(vfile_t *vf, const char *buf, size_t count) {
    return -ENOSYS;
}

static int fb_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    struct device *dev = virtio_gpu_get_dev();
    if (!dev) return -ENODEV;
    
    gpu_dev_ops_t *ops = (gpu_dev_ops_t *)dev->drv->class_ops;
    if (!ops) return -ENODEV;
    
    switch (req) {
        case FBIOGET_VSCREENINFO: {
            struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
            uint32_t w, h, bpp;
            ops->get_info(dev, &w, &h, &bpp);
            var->xres = w;
            var->yres = h;
            var->bits_per_pixel = bpp;
            return 0;
        }
        case FBIOGET_FSCREENINFO: {
            struct fb_fix_screeninfo *fix = (struct fb_fix_screeninfo *)arg;
            uintptr_t fb_phys;
            size_t fb_size;
            ops->get_fb(dev, &fb_phys, &fb_size);
            
            memset(fix->id, 0, sizeof(fix->id));
            strncpy(fix->id, "A20_FB", sizeof(fix->id) - 1);
            fix->smem_start = fb_phys;
            fix->smem_len = fb_size;
            
            uint32_t w, h, bpp;
            ops->get_info(dev, &w, &h, &bpp);
            fix->line_length = w * (bpp / 8);
            return 0;
        }
        case FBIO_MAP_FB: {
            // A20OS specific mmap logic: Map fb physical memory to the specified va
            uintptr_t va = (uintptr_t)arg;
            uintptr_t fb_phys;
            size_t fb_size;
            ops->get_fb(dev, &fb_phys, &fb_size);
            
            task_t *curr = proc_current();
            if (!curr || !curr->mm) return -EFAULT;
            
            pte_t flags = PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D; // User R/W
            // Map the range
            pt_map_range(curr->mm->pgdir, va, fb_phys, fb_size, flags);
            return 0;
        }
        case FBIO_FLUSH: {
            uint32_t w, h, bpp;
            ops->get_info(dev, &w, &h, &bpp);
            ops->flush(dev, 0, 0, w, h);
            return 0;
        }
        default:
            return -EINVAL;
    }
}

static int fb_close(vfile_t *vf) {
    return 0;
}

vfile_ops_t g_devfs_fb_ops = {
    .read  = fb_read,
    .write = fb_write,
    .ioctl = fb_ioctl,
    .close = fb_close,
};

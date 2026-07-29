#include "drivers/gpu/framebuffer.h"
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
#include "abi/linux/errno.h"
#include "abi/linux/mman.h"
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

static void fb_autoflush_thread(void) {
    for (;;) {
        struct device *dev = gpu_device_get_default();
        if (dev && dev->drv && dev->drv->class_ops) {
            gpu_dev_ops_t *ops = (gpu_dev_ops_t *)dev->drv->class_ops;
            if (ops->get_info && ops->flush) {
                uint32_t w = 0, h = 0, bpp = 0;
                if (ops->get_info(dev, &w, &h, &bpp) == 0 && w && h)
                    (void)ops->flush(dev, 0, 0, w, h);
            }
        }
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

int64_t fbdev_linux_mmap(uint64_t addr, size_t len, int prot, int flags,
                         uint64_t off) {
    struct device *dev = gpu_device_get_default();
    if (!dev) return -ENODEV;
    gpu_dev_ops_t *ops = (gpu_dev_ops_t *)dev->drv->class_ops;
    if (!ops) return -ENODEV;

    fb_autoflush_kick();

    uintptr_t fb_phys;
    size_t fb_size;
    int r = ops->get_fb(dev, &fb_phys, &fb_size);
    if (r < 0)
        return r;

    if (off & (PAGE_SIZE - 1))
        return -EINVAL;
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
    mm_struct_t *mm = curr->mm;

    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && addr != 0 &&
        (addr & (PAGE_SIZE - 1)))
        return -EINVAL;

    /*
     * A physical VRAM BAR is MMIO, not ordinary RAM.  Omitting PTE_MAT1
     * keeps the mapping at the default (Device) attribute index on
     * AArch64; see FBIO_MAP_FB.
     */
    pte_t ptef = PTE_V | PTE_U | PTE_A | PTE_LEAF;
    if (prot & PROT_READ) ptef |= PTE_R;
    if (prot & PROT_WRITE) ptef |= PTE_W | PTE_D | PTE_R;
    if (prot & PROT_EXEC) ptef |= PTE_X;

    uint64_t lock_flags = spin_lock_irqsave(&mm->lock);

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
            if (vma->start < addr + len && vma->end > addr) {
                spin_unlock_irqrestore(&mm->lock, lock_flags);
                return -EEXIST;
            }
            if (vma->start >= addr + len)
                break;
        }
        flags |= MAP_FIXED;
    }
    if ((flags & MAP_FIXED) && addr != 0) {
        mm_munmap(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);
    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT) {
        spin_unlock_irqrestore(&mm->lock, lock_flags);
        return -ENOMEM;
    }

    size_t mapped = 0;
    for (; mapped < len; mapped += PAGE_SIZE) {
        r = pt_map(mm->pgdir, addr + mapped, fb_phys + off + mapped, ptef);
        if (r < 0)
            break;
    }
    if (r < 0) {
        while (mapped > 0) {
            mapped -= PAGE_SIZE;
            pt_unmap(mm->pgdir, addr + mapped);
        }
        spin_unlock_irqrestore(&mm->lock, lock_flags);
        arch_tlb_flush();
        return r;
    }

    vm_area_t *vma = kcalloc(1, sizeof(*vma));
    if (!vma) {
        while (mapped > 0) {
            mapped -= PAGE_SIZE;
            pt_unmap(mm->pgdir, addr + mapped);
        }
        spin_unlock_irqrestore(&mm->lock, lock_flags);
        arch_tlb_flush();
        return -ENOMEM;
    }
    vma->start = addr;
    vma->end = addr + len;
    vma->vm_flags = VM_SHARED | VM_DONTFORK | VM_PFNMAP;
    if (prot & PROT_READ) vma->vm_flags |= VM_READ;
    if (prot & PROT_WRITE) vma->vm_flags |= VM_WRITE;
    if (prot & PROT_EXEC) vma->vm_flags |= VM_EXEC;
    vma->pte_flags = ptef;
    vma->file_fd = -1;
    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    mm->rss += len / PAGE_SIZE;

    arch_tlb_flush();
    spin_unlock_irqrestore(&mm->lock, lock_flags);
    return (int64_t)addr;
#endif
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

static int fb_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    (void)vf;
    struct device *dev = gpu_device_get_default();
    if (!dev) return -ENODEV;
    
    gpu_dev_ops_t *ops = (gpu_dev_ops_t *)dev->drv->class_ops;
    if (!ops) return -ENODEV;
    
    switch (req) {
        case FBIOGET_VSCREENINFO: {
            struct fb_var_screeninfo var;
            uint32_t w, h, bpp;
            int r = ops->get_info(dev, &w, &h, &bpp);
            if (r < 0)
                return r;
            fb_fill_var_screeninfo(&var, w, h, bpp);
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
            fix.line_length = (uint32_t)(fb_size / h);
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
            uintptr_t va = (uintptr_t)arg;
            task_t *curr = proc_current();
            if (!curr || !curr->mm) return -EFAULT;
            if ((va & (PAGE_SIZE - 1)) != 0 || fb_size == 0 ||
                va >= USER_VA_LIMIT || fb_size > USER_VA_LIMIT - va)
                return -EINVAL;

            uint64_t lock_flags = spin_lock_irqsave(&curr->mm->lock);
            for (vm_area_t *vma = curr->mm->mmap; vma; vma = vma->next) {
                if (vma->start < va + fb_size && vma->end > va) {
                    spin_unlock_irqrestore(&curr->mm->lock, lock_flags);
                    return -EEXIST;
                }
                if (vma->start >= va + fb_size)
                    break;
            }
            for (size_t off = 0; off < fb_size; off += PAGE_SIZE) {
                if (pt_translate(curr->mm->pgdir, va + off) != 0) {
                    spin_unlock_irqrestore(&curr->mm->lock, lock_flags);
                    return -EEXIST;
                }
            }

            /*
             * A physical VRAM BAR is MMIO, not ordinary RAM.  In particular,
             * mapping it as cacheable Normal memory on AArch64 leaves LVGL
             * writes in the CPU cache and produces a permanently black VBox
             * screen.  The default attribute index is Device; architectures
             * without memory-type bits simply ignore the omitted MAT1 flag.
             */
            pte_t flags = PTE_V | PTE_R | PTE_W | PTE_U |
                          PTE_A | PTE_D | PTE_LEAF;
            size_t mapped = 0;
            for (; mapped < fb_size; mapped += PAGE_SIZE) {
                r = pt_map(curr->mm->pgdir, va + mapped,
                           fb_phys + mapped, flags);
                if (r < 0)
                    break;
            }
            if (r < 0) {
                while (mapped > 0) {
                    mapped -= PAGE_SIZE;
                    pt_unmap(curr->mm->pgdir, va + mapped);
                }
                spin_unlock_irqrestore(&curr->mm->lock, lock_flags);
                arch_tlb_flush();
                return r;
            }

            vm_area_t *vma = kcalloc(1, sizeof(*vma));
            if (!vma) {
                while (mapped > 0) {
                    mapped -= PAGE_SIZE;
                    pt_unmap(curr->mm->pgdir, va + mapped);
                }
                spin_unlock_irqrestore(&curr->mm->lock, lock_flags);
                arch_tlb_flush();
                return -ENOMEM;
            }
            vma->start = va;
            vma->end = va + fb_size;
            vma->vm_flags = VM_READ | VM_WRITE | VM_SHARED |
                            VM_DONTFORK | VM_PFNMAP;
            vma->pte_flags = flags;
            vma->file_fd = -1;
            mm_insert_vma(curr->mm, vma);
            curr->mm->total_vm += fb_size / PAGE_SIZE;
            curr->mm->rss += fb_size / PAGE_SIZE;

            arch_tlb_flush();
            spin_unlock_irqrestore(&curr->mm->lock, lock_flags);
            return 0;
#endif
        }
        case FBIO_FLUSH: {
            uint32_t w, h, bpp;
            int r = ops->get_info(dev, &w, &h, &bpp);
            if (r < 0)
                return r;
            return ops->flush(dev, 0, 0, w, h);
        }
        default:
            return -EINVAL;
    }
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

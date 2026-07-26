#include "drivers/gpu/framebuffer.h"
#include "drivers/gpu/gpu_core.h"
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
#include "mm/slab.h"

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
            var.xres = w;
            var.yres = h;
            var.bits_per_pixel = bpp;
            return copy_to_user(arg, &var, sizeof(var)) < 0 ? -EFAULT : 0;
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
